/*
 * pp-adsb — 1090 MHz ADS-B / Mode S aircraft viewer, GUI over dump1090-fa.
 *   dump1090 can't talk to a Pluto, so (like pp-rtl433) we bridge with pp-iqfeed
 *   and let dump1090 do the hard DSP (preamble, PPM, CRC-24, CPR positions):
 *     pp-iqfeed 1090000000 2400000 1 GAIN |
 *       dump1090 --ifile - --iformat SC16Q11 --net --net-sbs-port 30003 --quiet
 *   The AD9361's signed-12-bit-in-int16 samples map straight onto dump1090's
 *   SC16Q11 format, and 2.4 Msps matches dump1090's fixed rate, so our cs16 feed
 *   works unmodified. We connect to the SBS-1 (BaseStation) TCP output on 30003
 *   and parse its CSV into a live aircraft table.
 *
 *   DATA : selectable aircraft table (ICAO / FLIGHT / ALT / SPD / AGE).
 *          UP/DOWN or wheel move the selection; CENTER opens a detail card with
 *          every field; BTN8 opens gain; BTN7 exits.
 *   Needs a 1090 MHz antenna to hear traffic.
 *
 * usage: pp-adsb [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include "pp_ui.h"

#define FEED_PATH "/usr/bin/pp-iqfeed"
#define D1090_PATH "/usr/bin/dump1090"
#define ADSB_LO   1090000000LL
#define ADSB_FS   2400000
#define SBS_PORT  30003
#define MAXAC     64

/* ---- aircraft table ---- */
typedef struct {
    char icao[8], flight[10], squawk[8];
    long alt, gs, trk, vrate;
    double lat, lon;
    int have_flight, have_alt, have_gs, have_trk, have_pos, have_vrate, have_squawk;
    time_t seen; int msgs;
} ac_t;
static ac_t ac[MAXAC]; static int nac;

static long gain_db = 60;           /* bare 1090 antenna is weak; libiio RX doesn't
                                       saturate here (peak ~6% FS at max gain), so start
                                       high and trim with the ADC meter if it clips */
static long pending_gain = 60;      /* gain being edited in settings (shown live) */
static long gain_mode = 0, pending_mode = 0;   /* 0=manual (use GAIN), 1=slow AGC, 2=fast AGC */

/* ---- SBS CSV parsing ---- */
static int splitcsv(char *s, char *f[], int max){
    int n=0; f[n++]=s;
    for(; *s && n<max; s++) if(*s==','){ *s=0; f[n++]=s+1; }
    return n;
}
static void trim_trailing(char *s){ int n=(int)strlen(s); while(n>0 && (s[n-1]==' '||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0; }

static int ac_find_or_add(const char *icao){
    for(int i=0;i<nac;i++) if(!strcmp(ac[i].icao,icao)) return i;
    int i;
    if(nac<MAXAC) i=nac++;
    else{ i=0; for(int k=1;k<nac;k++) if(ac[k].seen<ac[i].seen) i=k; }   /* evict LRU */
    memset(&ac[i],0,sizeof ac[i]);
    snprintf(ac[i].icao,sizeof ac[i].icao,"%s",icao);
    return i;
}
static void sbs_line(char *line){
    if(strncmp(line,"MSG,",4)) return;
    char *f[24]; int n=splitcsv(line,f,24);
    if(n<5 || !f[4][0]) return;
    ac_t *a=&ac[ac_find_or_add(f[4])];
    if(n>10 && f[10][0]){ snprintf(a->flight,sizeof a->flight,"%s",f[10]); trim_trailing(a->flight); a->have_flight=1; }
    if(n>11 && f[11][0]){ a->alt=strtol(f[11],0,10);  a->have_alt=1; }     /* may carry a trailing 'H' */
    if(n>12 && f[12][0]){ a->gs =strtol(f[12],0,10);  a->have_gs=1; }
    if(n>13 && f[13][0]){ a->trk=strtol(f[13],0,10);  a->have_trk=1; }
    if(n>15 && f[14][0] && f[15][0]){ a->lat=strtod(f[14],0); a->lon=strtod(f[15],0); a->have_pos=1; }
    if(n>16 && f[16][0]){ a->vrate=strtol(f[16],0,10); a->have_vrate=1; }
    if(n>17 && f[17][0]){ snprintf(a->squawk,sizeof a->squawk,"%s",f[17]); a->have_squawk=1; }
    a->seen=time(NULL); a->msgs++;
}

/* ---- pipeline (pp-iqfeed | dump1090) + SBS TCP client ---- */
static pid_t pid_feed=-1, pid_d1090=-1;
static int   sbs_fd=-1;
static int   feed_efd=-1;              /* pp-iqfeed stderr (LVL level lines) */
static int   lvl_rms=0, lvl_peak=0;    /* ADC level, % of full scale */

/* drain pp-iqfeed's stderr; parse the newest "LVL <rms> <peak>" for the meter */
static void level_poll(void){
    if(feed_efd<0) return;
    static char lb[256]; static int ll=0; char t[512]; int r;
    while((r=read(feed_efd,t,sizeof t))>0){
        for(int k=0;k<r;k++){ char c=t[k];
            if(c=='\n'){ lb[ll]=0; int a,b; if(sscanf(lb,"LVL %d %d",&a,&b)==2){ lvl_rms=a; lvl_peak=b; pp_rx_ol_note(1,b); } ll=0; }
            else if(ll<(int)sizeof lb-1) lb[ll++]=c; }
    }
}

#ifndef PP_SIM
static void pipeline_stop(void){
    if(pid_feed >0){ kill(pid_feed, SIGKILL); waitpid(pid_feed, NULL,0); pid_feed=-1; }
    if(pid_d1090>0){ kill(pid_d1090,SIGKILL); waitpid(pid_d1090,NULL,0); pid_d1090=-1; }
    if(sbs_fd>=0){ close(sbs_fd); sbs_fd=-1; }
    if(feed_efd>=0){ close(feed_efd); feed_efd=-1; }
}
static int pipeline_start(void){
    pipeline_stop();
    int p1[2]; if(pipe(p1)<0) return 0;
    int fe[2]; if(pipe(fe)<0){ close(p1[0]); close(p1[1]); return 0; }   /* feed stderr -> meter */
    char slo[24],sfs[16],sg[12];
    snprintf(slo,sizeof slo,"%lld",ADSB_LO);
    snprintf(sfs,sizeof sfs,"%d",ADSB_FS);
    snprintf(sg,sizeof sg,"%ld",gain_db);
    const char *gm = gain_mode==2?"fast":(gain_mode==1?"slow":"manual");
    pid_feed=fork();
    if(pid_feed==0){
        dup2(p1[1],1); dup2(fe[1],2); close(p1[0]); close(p1[1]); close(fe[0]); close(fe[1]);
        execl(FEED_PATH,"pp-iqfeed",slo,sfs,"1",sg,gm,(char*)0); _exit(127);
    }
    close(fe[1]); fcntl(fe[0],F_SETFL,O_NONBLOCK); feed_efd=fe[0];
    pid_d1090=fork();
    if(pid_d1090==0){
        dup2(p1[0],0);
        int dn=open("/dev/null",O_WRONLY); if(dn>=0){ dup2(dn,1); dup2(dn,2); if(dn>2) close(dn); }
        close(p1[0]); close(p1[1]);
        /* SC16 (full-scale int16), NOT SC16Q11: the maia recorder hands back
         * left-justified full-16-bit samples (|max|~32k), not the AD9361's native
         * 12-bit Q11 (|max|~2048). Telling dump1090 SC16Q11 made every sample read
         * ~16x too hot -> preambles found but 0 valid CRC. (The old libiio RX path
         * WAS Q11; the maia-recorder rewrite changed the scale.) */
        execl(D1090_PATH,"dump1090","--ifile","-","--iformat","SC16",
              "--net","--net-bind-address","127.0.0.1","--net-sbs-port","30003","--quiet",(char*)0);
        _exit(127);
    }
    close(p1[0]); close(p1[1]);
    return (pid_feed>0 && pid_d1090>0);
}
static void sbs_try_connect(void){
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return;
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(SBS_PORT);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if(connect(fd,(struct sockaddr*)&a,sizeof a)<0){ close(fd); return; }
    fcntl(fd,F_SETFL,O_NONBLOCK); sbs_fd=fd;
}
static void pipeline_poll(void){
    if(sbs_fd<0) return;
    static char lb[600]; static int ll=0;
    char tmp[1024]; ssize_t r;
    while((r=read(sbs_fd,tmp,sizeof tmp))>0){
        for(ssize_t k=0;k<r;k++){ char c=tmp[k];
            if(c=='\n'){ lb[ll]=0; sbs_line(lb); ll=0; }
            else if(ll<(int)sizeof lb-1) lb[ll++]=c; }
    }
    if(r==0){ close(sbs_fd); sbs_fd=-1; }        /* dump1090 dropped us */
}
#else  /* PP_SIM: no radio/subprocess — inject canned SBS lines */
static int pipeline_start(void){ sbs_fd=1; return 1; }
static void pipeline_stop(void){ sbs_fd=-1; }
static void sbs_try_connect(void){ sbs_fd=1; }
static void pipeline_poll(void){
    static unsigned t=0; static char b[128];
    static const char *C[]={
      "MSG,1,1,1,4CA7B5,1,,,,,SWR123  ,,,,,,,,,",
      "MSG,3,1,1,4CA7B5,1,,,,,,37000,,,47.12345,8.54321,,,,,",
      "MSG,4,1,1,4CA7B5,1,,,,,,,461,268,,,-64,,,,",
      "MSG,3,1,1,A1B2C3,1,,,,,,12025,,,47.30000,8.10000,,,,,",
      "MSG,1,1,1,A1B2C3,1,,,,,DAL45   ,,,,,,,,,",
      "MSG,3,1,1,3C6DD2,1,,,,,,4500,,,46.99000,7.44000,,,,,",
    };
    if((t++%12)==0){ snprintf(b,sizeof b,"%s",C[(t/12)%6]); sbs_line(b); }
    lvl_rms=30+(int)(t/6)%25; lvl_peak=lvl_rms+35;   /* animate the meter in the sim */
}
#endif
static int pipeline_restart(void){ return pipeline_start(); }

/* ---- views ---- */
enum { S_TABLE=0, S_DETAIL, S_SETTINGS };
static int view=S_TABLE, sel=0;
static char detail_icao[8];

static int sort_idx(int idx[]){                    /* newest-seen first; returns count */
    int m=nac; for(int i=0;i<m;i++) idx[i]=i;
    for(int a=0;a<m;a++) for(int b=a+1;b<m;b++)
        if(ac[idx[b]].seen>ac[idx[a]].seen){ int t=idx[a];idx[a]=idx[b];idx[b]=t; }
    return m;
}

#define TBL_Y0 40
static void draw_table(uint16_t*buf){
    pp_fill(buf,0,20,PP_W,PP_H-20,C_BG);
    pp_text(buf,4,24,"ICAO",C_DIM,1); pp_text(buf,70,24,"FLIGHT",C_DIM,1);
    pp_text(buf,150,24,"ALT",C_DIM,1); pp_text(buf,210,24,"SPD",C_DIM,1);
    pp_text(buf,PP_W-30,24,"AGE",C_DIM,1);
    int idx[MAXAC], m=sort_idx(idx);
    int rh=15, top=TBL_Y0, rows=(PP_H-14-top)/rh;
    if(sel>=m) sel=m>0?m-1:0; if(sel<0) sel=0;
    int first=0; if(sel>=rows) first=sel-rows+1;
    time_t now=time(NULL);
    for(int r=0; r<rows && first+r<m; r++){
        int i=idx[first+r], y=top+r*rh; ac_t*a=&ac[i]; char s[24];
        int foc=(first+r==sel);
        pp_fill(buf,2,y-1,PP_W-4,rh-1, foc?C_PANEL:((r&1)?C_PANEL2:C_BG));
        if(foc) pp_fill(buf,2,y-1,3,rh-1,C_ACCENT);
        pp_text(buf,8,y,a->icao,C_TEXT,1);
        pp_text(buf,70,y, a->have_flight?a->flight:"-", C_ACCENT,1);
        if(a->have_alt){ snprintf(s,sizeof s,"%ld",a->alt); pp_text(buf,150,y,s,C_TEXT,1); } else pp_text(buf,150,y,"-",C_DIM,1);
        if(a->have_gs){  snprintf(s,sizeof s,"%ld",a->gs);  pp_text(buf,210,y,s,C_TEXT,1); } else pp_text(buf,210,y,"-",C_DIM,1);
        long age=(long)(now-a->seen); if(age<0)age=0;
        snprintf(s,sizeof s,"%lds",age); pp_text(buf,PP_W-pp_tw(s,1)-2,y,s, age<15?C_GOOD:C_DIM,1);
    }
    if(m==0) pp_text(buf,8,top+4,"listening for aircraft...",C_DIM,1);
    pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
    pp_text(buf,4,PP_H-12,"CENTER=detail SETTINGS=gain BACK=exit",C_DIM,1);
}
static void kv(uint16_t*buf,int y,const char*k,const char*v){
    pp_text(buf,10,y,k,C_DIM,1); pp_text(buf,120,y,v,C_TEXT,1);
}
static void draw_detail(uint16_t*buf){
    pp_fill(buf,0,20,PP_W,PP_H-20,C_BG);
    int i=-1; for(int k=0;k<nac;k++) if(!strcmp(ac[k].icao,detail_icao)){ i=k; break; }
    if(i<0){ pp_text(buf,10,40,"(aircraft lost)",C_WARN,1);
             pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2); pp_text(buf,4,PP_H-12,"BACK",C_DIM,1); return; }
    ac_t*a=&ac[i]; char s[32]; int y=40;
    pp_text(buf,10,26,a->icao,C_ACCENT,2);
    if(a->have_flight) pp_text(buf,120,30,a->flight,C_TEXT,2);
    y=56;
    snprintf(s,sizeof s,"%s",a->have_flight?a->flight:"--");        kv(buf,y,"Flight",s); y+=16;
    if(a->have_alt){ snprintf(s,sizeof s,"%ld ft",a->alt); } else snprintf(s,sizeof s,"--"); kv(buf,y,"Altitude",s); y+=16;
    if(a->have_gs){  snprintf(s,sizeof s,"%ld kt",a->gs); } else snprintf(s,sizeof s,"--");  kv(buf,y,"Ground spd",s); y+=16;
    if(a->have_trk){ snprintf(s,sizeof s,"%ld deg",a->trk); } else snprintf(s,sizeof s,"--"); kv(buf,y,"Track",s); y+=16;
    if(a->have_vrate){ snprintf(s,sizeof s,"%ld fpm",a->vrate); } else snprintf(s,sizeof s,"--"); kv(buf,y,"Vert rate",s); y+=16;
    if(a->have_pos){ snprintf(s,sizeof s,"%.4f",a->lat); kv(buf,y,"Latitude",s); y+=16;
                     snprintf(s,sizeof s,"%.4f",a->lon); kv(buf,y,"Longitude",s); y+=16; }
    else { kv(buf,y,"Position","-- (need even+odd)"); y+=16; }
    if(a->have_squawk){ snprintf(s,sizeof s,"%s",a->squawk); } else snprintf(s,sizeof s,"--"); kv(buf,y,"Squawk",s); y+=16;
    snprintf(s,sizeof s,"%d",a->msgs); kv(buf,y,"Messages",s); y+=16;
    snprintf(s,sizeof s,"%lds ago",(long)(time(NULL)-a->seen)); kv(buf,y,"Last seen",s); y+=16;
    pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
    pp_text(buf,4,PP_H-12,"7 or CENTER: back",C_DIM,1);
}
static void draw_settings(uint16_t*buf){
    pp_fill(buf,0,20,PP_W,PP_H-20,C_BG);
    int agc = pending_mode!=0;
    pp_text(buf,10,40,"GAIN",C_DIM,1);
    char s[24]; if(agc) snprintf(s,sizeof s,"AGC"); else snprintf(s,sizeof s,"%ld",pending_gain);
    pp_text(buf,10,54,s,agc?C_DIM:C_ACCENT,3); if(!agc) pp_text(buf,90,64,"dB",C_UNIT,1);
    const char *mm[3]={"MANUAL","SLOW AGC","FAST AGC"};
    pp_text(buf,10,100,"MODE",C_DIM,1); pp_text(buf,64,100,mm[pending_mode%3],C_ACCENT,1);
    /* ADC level lives in the status-bar RX meter (header) */
    pp_text(buf,10,118,"1090 MHz  2.4 Msps  SC16",C_DIM,1);
    pp_text(buf,10,152,"lower gain / use AGC if peak near 100%",C_DIM,1);
    pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
    pp_text(buf,4,PP_H-12,"wheel/UP-DN gain  L/R mode  CENTER apply",C_DIM,1);
}

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    int running = pipeline_start();
    pending_gain=gain_db; int gain_dirty=0, connect_throttle=0, confirm=0;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    int32_t last_enc=0; int enc_init=0, enc_accum=0;
    int prev_view=-1;
    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_accum += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_accum/4; enc_accum-=steps*4;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;

            if(confirm){                                 /* exit-confirmation modal */
                if(pressed&PP_BTN_CENTER) break;         /* confirm -> exit to menu */
                if(pressed&PP_BTN_7) confirm=0;          /* cancel -> back to table */
            } else if(view==S_TABLE){
                if(pressed&PP_BTN_7) confirm=1;          /* BACK on table -> confirm exit */
                sel += !!(pressed&PP_BTN_DOWN) - !!(pressed&PP_BTN_UP) + steps;
                if(sel<0)sel=0;
                if(pressed&PP_BTN_CENTER){ int idx[MAXAC],m=sort_idx(idx);
                    if(m>0){ if(sel>=m)sel=m-1; snprintf(detail_icao,sizeof detail_icao,"%s",ac[idx[sel]].icao); view=S_DETAIL; } }
                if(pressed&PP_BTN_8){ view=S_SETTINGS; pending_gain=gain_db; pending_mode=gain_mode; gain_dirty=0; }
            } else if(view==S_DETAIL){
                if(pressed&(PP_BTN_7|PP_BTN_CENTER)) view=S_TABLE;
            } else { /* S_SETTINGS: wheel/UP-DOWN -> gain, LEFT/RIGHT -> gain mode */
                int gd=steps + !!(pressed&PP_BTN_UP) - !!(pressed&PP_BTN_DOWN);
                if(gd){ pending_gain+=gd; if(pending_gain<0)pending_gain=0; if(pending_gain>71)pending_gain=71; gain_dirty=1; }
                int mdc = !!(pressed&PP_BTN_RIGHT) - !!(pressed&PP_BTN_LEFT);
                if(mdc){ pending_mode=((pending_mode+mdc)%3+3)%3; gain_dirty=1; }
                if(pressed&(PP_BTN_7|PP_BTN_CENTER)){
                    if(gain_dirty && (pending_gain!=gain_db || pending_mode!=gain_mode)){
                        gain_db=pending_gain; gain_mode=pending_mode; running=pipeline_restart(); connect_throttle=0; }
                    view=S_TABLE;
                }
            }
        }

        if(sbs_fd<0 && running){ if(++connect_throttle>=8){ connect_throttle=0; sbs_try_connect(); } }
        pipeline_poll();
        level_poll();                                /* update ADC saturation meter */

        if(view!=prev_view){ memset(pp_prev,0xA5,sizeof pp_prev); prev_view=view; }

        const char *note=NULL; uint16_t nc=C_WARN;
        if(!running){ note="no dump1090"; nc=C_BAD; }
        else if(sbs_fd<0){ note="connecting.."; nc=C_WARN; }
        else if(view==S_SETTINGS && gain_dirty){ note="gain*"; nc=C_WARN; }
        /* aircraft count is shown in the table, not the status bar */

        if(view==S_TABLE)         draw_table(buf);
        else if(view==S_DETAIL)   draw_detail(buf);
        else                      draw_settings(buf);
        pp_ui_status_bar(buf, view==S_SETTINGS?"ADS-B GAIN":"ADS-B", &st, link_ok, note, nc);
        pp_confirm_overlay(buf, confirm);            /* exit-to-menu modal on top */
        pp_refresh(buf);
        usleep(30000);
    }
    pipeline_stop();
    return 0;
}
