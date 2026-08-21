/*
 * pp-rtl433 — front-panel GUI over rtl_433 for ISM-band device decoding.
 *   Pipes  pp-iqfeed LO FS D GAIN | rtl_433 -r cs16:- -s FS/D -F json  and parses
 *   the JSON event stream into a live device table (one row per model+id,
 *   updated in place with its latest decoded values and a last-seen age).
 *   DATA screen : the device table, with one adjustable field above it.
 *     BAND  : ISM preset (433.92 / 315 / 345 / 868.3 / 915 MHz) -> retunes + restarts
 *     GAIN  : RX gain (dB) -> restarts the pipeline
 *   SETTINGS (BTN8): DECIM (feed decimation; higher = lower CPU, narrower band).
 *   BTN7 : layered back / exit (tears the pipeline down).
 *
 * usage: pp-rtl433 [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include "pp_ui.h"

#define FEED_FS   2100000            /* just above the AD9361 min (25M/12=2083333);
                                        maia bumps anything below that min to 4 MHz,
                                        which would desync rtl_433's -s rate. 2.1 MHz
                                        is accepted exactly, so fs/D stays correct. */
#define RTL_PATH  "/usr/bin/rtl_433"
#define FEED_PATH "/usr/bin/pp-iqfeed"
#define MAXDEV    48

/* ---- ISM band presets ---- */
typedef struct { const char *name; long lo_khz; } band_t;
static const band_t BANDS[] = {
    { "433.92", 433920 }, { "315.00", 315000 }, { "345.00", 345000 },
    { "868.30", 868300 }, { "915.00", 915000 },
};
#define NBAND ((long)(sizeof BANDS/sizeof BANDS[0]))

/* ---- fields ---- */
static long band_i = 0;   static const long PLACE_1[1]={1};
static long gain_db = 65; static const long PLACE_GN[2]={10,1};  /* bare antenna is weak
    (even at max gain 71, 915 noise floor is ~0.2% FS — no saturation risk). Start
    high; use the ADC meter, and lower DECIM to widen coverage across 902-928. */
static long decim  = 8;   static const long PLACE_DC[2]={10,1};
static long gain_mode = 0;  /* 0=manual (use GAIN), 1=slow AGC, 2=fast AGC */

static void fmt_band(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",BANDS[(*f->val)%NBAND].name); }
static void fmt_int (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_gmode(const pp_field_t*f,char*v,int n){ const char*m[3]={"MANUAL","SLOW AGC","FAST AGC"}; snprintf(v,n,"%s",m[(*f->val)%3]); }

static int pipeline_restart(void);   /* fwd */
static int ap_band(pp_field_t*f){ (void)f; return pipeline_restart(); }
static int ap_gain(pp_field_t*f){ (void)f; return pipeline_restart(); }
static int ap_mode(pp_field_t*f){ (void)f; return pipeline_restart(); }
static int ap_decim(pp_field_t*f){ (void)f; return pipeline_restart(); }

static pp_field_t fields[4] = {
  { "BAND", "MHz", &band_i,   0, NBAND-1, PLACE_1, 1,0, fmt_band,  ap_band,  NULL, 0,0,1,0, NULL },
  { "GAIN", "dB",  &gain_db,  0, 71,      PLACE_GN,2,1, fmt_int,   ap_gain,  NULL, 0,0,1,0, NULL },
  { "MODE", "",    &gain_mode,0, 2,       PLACE_1, 1,0, fmt_gmode, ap_mode,  NULL, 0,0,1,0, NULL },
  { "DECIM","",    &decim,    1, 64,      PLACE_DC,2,1, fmt_int,   ap_decim, NULL, 0,0,1,0, NULL },
};
#define NFIELDS 4
#define NDATA   2

/* ---- device table ---- */
typedef struct { char key[48], model[26], val[40]; long id; time_t seen; int count; } rdev_t;
static rdev_t devs[MAXDEV]; static int ndev=0;

/* ---- tiny JSON scanners (flat objects, as rtl_433 emits) ---- */
static int j_num(const char*s,const char*key,double*out){
    char pat[40]; snprintf(pat,sizeof pat,"\"%s\":",key);
    const char*p=strstr(s,pat); if(!p) return 0;
    p+=strlen(pat); while(*p==' ')p++;
    if(*p=='"')return 0;                         /* it's a string, not a number */
    *out=strtod(p,NULL); return 1;
}
static int j_str(const char*s,const char*key,char*out,int n){
    char pat[40]; snprintf(pat,sizeof pat,"\"%s\":\"",key);
    const char*p=strstr(s,pat); if(!p) return 0;
    p+=strlen(pat); int i=0;
    while(*p && *p!='"' && i<n-1){ if(*p=='\\'&&p[1]) p++; out[i++]=*p++; }
    out[i]=0; return 1;
}
/* build a compact value summary from whatever recognisable fields are present */
static void summarize(const char*line,char*out,int n){
    char b[40]; double d; out[0]=0; int len=0;
    #define APP(...) do{ char t[48]; snprintf(t,sizeof t,__VA_ARGS__); \
        int l=(int)strlen(t); if(len+l<n-1){ memcpy(out+len,t,l); len+=l; out[len]=0; } }while(0)
    if(j_num(line,"temperature_C",&d))    APP("%.1fC ",d);
    else if(j_num(line,"temperature_F",&d))APP("%.0fF ",d);
    if(j_num(line,"humidity",&d))         APP("%.0f%% ",d);
    if(j_num(line,"wind_avg_km_h",&d))    APP("%.0fkm/h ",d);
    if(j_num(line,"pressure_kPa",&d))     APP("%.0fkPa ",d);
    if(j_num(line,"moisture",&d))         APP("moist%.0f ",d);
    if(j_num(line,"depth_cm",&d))         APP("%.0fcm ",d);
    if(j_str(line,"state",b,sizeof b))    APP("%s ",b);
    if(j_num(line,"battery_ok",&d) && d==0.0) APP("LOWBAT ");
    if(!len && j_str(line,"model",b,sizeof b)) APP("(seen)");   /* nothing numeric */
    #undef APP
}
static void table_update(const char*line){
    char model[26]; if(!j_str(line,"model",model,sizeof model)) return;   /* not a device event */
    double idd=0; long id=(long)(j_num(line,"id",&idd)?idd:0);
    double chd=0; int has_ch=j_num(line,"channel",&chd);
    char key[48];
    if(has_ch) snprintf(key,sizeof key,"%s/%ld/c%d",model,id,(int)chd);
    else       snprintf(key,sizeof key,"%s/%ld",model,id);
    char val[40]; summarize(line,val,sizeof val);
    int i; for(i=0;i<ndev;i++) if(!strcmp(devs[i].key,key)) break;
    if(i==ndev){
        if(ndev<MAXDEV) ndev++;
        else{ int old=0; for(int k=1;k<ndev;k++) if(devs[k].seen<devs[old].seen) old=k; i=old; }  /* evict LRU */
        snprintf(devs[i].key,sizeof devs[i].key,"%s",key);
        snprintf(devs[i].model,sizeof devs[i].model,"%s",model);
        devs[i].id=id; devs[i].count=0;
    }
    snprintf(devs[i].val,sizeof devs[i].val,"%s",val);
    devs[i].seen=time(NULL); devs[i].count++;
}

/* ============================ pipeline ============================ */
static pid_t pid_feed=-1, pid_rtl=-1;
static int   rtl_fd=-1;
static int   feed_efd=-1;              /* pp-iqfeed stderr (LVL level lines) */
static int   lvl_rms=0, lvl_peak=0;    /* ADC level, % of full scale */

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
    if(pid_feed>0){ kill(pid_feed,SIGKILL); waitpid(pid_feed,NULL,0); pid_feed=-1; }
    if(pid_rtl >0){ kill(pid_rtl, SIGKILL); waitpid(pid_rtl, NULL,0); pid_rtl =-1; }
    if(rtl_fd>=0){ close(rtl_fd); rtl_fd=-1; }
    if(feed_efd>=0){ close(feed_efd); feed_efd=-1; }
}
static int pipeline_start(void){
    pipeline_stop();
    int p1[2], p2[2], fe[2];
    if(pipe(p1)<0) return 0;
    if(pipe(p2)<0){ close(p1[0]);close(p1[1]); return 0; }
    if(pipe(fe)<0){ close(p1[0]);close(p1[1]);close(p2[0]);close(p2[1]); return 0; }
    long fs=FEED_FS, D=decim<1?1:decim; long rate=fs/D;
    char slo[24],sfs[16],sd[16],sg[16],srate[16];
    snprintf(slo,sizeof slo,"%lld",(long long)BANDS[band_i%NBAND].lo_khz*1000);
    snprintf(sfs,sizeof sfs,"%ld",fs); snprintf(sd,sizeof sd,"%ld",D);
    snprintf(sg,sizeof sg,"%ld",gain_db); snprintf(srate,sizeof srate,"%ld",rate);

    pid_feed=fork();
    if(pid_feed==0){
        dup2(p1[1],1); dup2(fe[1],2); close(p1[0]);close(p1[1]);close(p2[0]);close(p2[1]);close(fe[0]);close(fe[1]);
        const char *gm = gain_mode==2?"fast":(gain_mode==1?"slow":"manual");
        execl(FEED_PATH,"pp-iqfeed",slo,sfs,sd,sg,gm,(char*)0); _exit(127);
    }
    pid_rtl=fork();
    if(pid_rtl==0){
        dup2(p1[0],0); dup2(p2[1],1);
        int dn=open("/dev/null",O_WRONLY); if(dn>=0){ dup2(dn,2); if(dn>2)close(dn); }
        close(p1[0]);close(p1[1]);close(p2[0]);close(p2[1]);close(fe[0]);close(fe[1]);
        execl(RTL_PATH,"rtl_433","-r","cs16:-","-s",srate,"-F","json",(char*)0); _exit(127);
    }
    close(p1[0]);close(p1[1]);close(p2[1]);close(fe[1]);
    rtl_fd=p2[0]; fcntl(rtl_fd,F_SETFL,O_NONBLOCK);
    feed_efd=fe[0]; fcntl(feed_efd,F_SETFL,O_NONBLOCK);
    return (pid_feed>0 && pid_rtl>0);
}
/* drain available JSON lines from rtl_433 into the table */
static void pipeline_poll(void){
    if(rtl_fd<0) return;
    static char lb[2048]; static int ll=0;
    char tmp[1024]; ssize_t r;
    while((r=read(rtl_fd,tmp,sizeof tmp))>0){
        for(ssize_t k=0;k<r;k++){
            char c=tmp[k];
            if(c=='\n'){ lb[ll]=0; if(lb[0]=='{') table_update(lb); ll=0; }
            else if(ll<(int)sizeof lb-1) lb[ll++]=c;
        }
    }
}
#else  /* PP_SIM: no radio/subprocess — inject canned events so the table populates */
static int pipeline_start(void){ rtl_fd=1; return 1; }
static void pipeline_stop(void){ rtl_fd=-1; }
static void pipeline_poll(void){
    static unsigned t=0; static const char *CANNED[]={
      "{\"model\":\"Nexus-TH\",\"id\":42,\"channel\":1,\"temperature_C\":21.7,\"humidity\":48,\"battery_ok\":1}",
      "{\"model\":\"Acurite-Tower\",\"id\":1234,\"channel\":2,\"temperature_C\":23.1,\"humidity\":55}",
      "{\"model\":\"LaCrosse-TX\",\"id\":7,\"temperature_C\":19.9,\"battery_ok\":0}",
      "{\"model\":\"Prologue\",\"id\":88,\"channel\":3,\"temperature_C\":25.4,\"humidity\":40}",
    };
    if((t++%20)==0) table_update(CANNED[(t/20)%4]);
    lvl_rms=25+(int)(t/8)%30; lvl_peak=lvl_rms+40;   /* animate the meter in the sim */
}
#endif
static int pipeline_restart(void){ return pipeline_start(); }

/* ============================ rendering ============================ */
#define TBL_Y0 50
static void trunc_cpy(char*d,const char*s,int max){ int i=0; for(;s[i]&&i<max;i++)d[i]=s[i]; d[i]=0; }
static void draw_table(uint16_t*buf){
    pp_fill(buf,0,TBL_Y0,PP_W,PP_H-TBL_Y0-14,C_BG);
    int hy=TBL_Y0;                    /* ADC level now lives in the status-bar RX meter */
    pp_text(buf,4,hy,"MODEL",C_DIM,1);
    pp_text(buf,150,hy,"ID",C_DIM,1);
    pp_text(buf,190,hy,"VALUE",C_DIM,1);
    pp_text(buf,PP_W-26,hy,"AGE",C_DIM,1);
    int rh=14, top=hy+13, rows=(PP_H-14-top)/rh;
    /* order newest-seen first */
    int idx[MAXDEV], m=ndev; for(int i=0;i<m;i++) idx[i]=i;
    for(int a=0;a<m;a++) for(int b=a+1;b<m;b++) if(devs[idx[b]].seen>devs[idx[a]].seen){ int t=idx[a];idx[a]=idx[b];idx[b]=t; }
    time_t now=time(NULL);
    for(int r=0; r<rows && r<m; r++){
        int i=idx[r], y=top+r*rh; char s[40];
        pp_fill(buf,4,y-1,PP_W-8,rh-1,(r&1)?C_PANEL2:C_BG);
        trunc_cpy(s,devs[i].model,18);            pp_text(buf,4,y,s,C_TEXT,1);
        snprintf(s,sizeof s,"%ld",devs[i].id);    pp_text(buf,150,y,s,C_UNIT,1);
        trunc_cpy(s,devs[i].val,20);              pp_text(buf,190,y,s,C_ACCENT,1);
        long age=(long)(now-devs[i].seen); if(age<0)age=0;
        if(age<999) snprintf(s,sizeof s,"%lds",age); else snprintf(s,sizeof s,">999");
        pp_text(buf,PP_W-pp_tw(s,1)-2,y,s, age<60?C_GOOD:C_DIM,1);
    }
    if(m==0) pp_text(buf,4,top+4,"listening...",C_DIM,1);
}

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    int running = pipeline_start();

    pp_ui_t ui; pp_ui_init(&ui, fields, NFIELDS); ui.ndata=NDATA;
    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    int prev_screen=-1;
    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) break; }

        pp_ui_sync(&ui);            /* BAND/GAIN/DECIM edits restart the pipeline via apply */
        pipeline_poll();
        level_poll();               /* update ADC saturation meter */

        if(ui.screen!=prev_screen){
            pp_fill(buf,0,20,PP_W,PP_H-20,C_BG);
            memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen;
        }

        const char *note=NULL; uint16_t notecol=C_WARN;
        if(!running){ note="no rtl_433"; notecol=C_BAD; }
        else if(!pp_ui_all_synced(&ui)){ note="tune.."; notecol=C_WARN; }
        pp_ui_status_bar(buf,NULL,&st,link_ok,note,notecol);

        if(ui.screen==PP_DATA){
            pp_ui_draw_field_band(buf,&fields[ui.focus],ui.editing,20);
            draw_table(buf);
        }else{
            pp_ui_draw_settings(buf,&ui,20,PP_H-20-14);
        }
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    pipeline_stop();
    return 0;
}
