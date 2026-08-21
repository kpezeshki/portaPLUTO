/*
 * pp-rficstat — read-only dump of the AD9361 (ad9361-phy) internal state.
 *   Scans every IIO sysfs attribute of ad9361-phy (device-level + per-channel:
 *   RX/TX LO, gains, sample rate, RF bandwidths, RSSI, temperature, calibration
 *   flags, ENSM, FIR config, ...) and lists them "name : value", refreshed ~1/s.
 *   Scroll with UP/DOWN/wheel; BACK asks to exit (shared confirm popup). No libiio
 *   dependency — plain sysfs reads.
 *
 *   BTN8 flips to a CAL screen — the app's ONLY writes — that toggles the three RX
 *   tracking calibrations (quadrature / RF DC / BB DC, the "balance IQ" set enabled
 *   at boot) and fires one-shot TX-quad / RX-recalibrate actions. Other apps still
 *   re-assert the RX tracks when they open, so a toggle here is a live setting.
 *
 * Layout: name (grey) then value (cyan). If the pair is too wide for the screen
 * the value wraps to its own indented line so nothing clips off the edge and a
 * value is never split across lines. Values longer than one line are marked '~'.
 *
 * usage: pp-rficstat [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>                /* open/write/close for the CAL writes */
#include "pp_ui.h"                 /* status bar + pp_confirm_overlay (via pp_common) */

#define MAXR   700                 /* display rows (an attr is 1 or 2 rows) */
#define LH     11
#define TXT_Y0 24
#define X0     6                   /* text left edge */
#define WCH    37                  /* chars that fit x=6..~308 (scrollbar past that) */
#define NAMSZ  48

static char rname[MAXR][NAMSZ];    /* left column (empty on a value-continuation row) */
static char rval [MAXR][40];       /* right column / value (empty on a name-only row) */
static int  nrows, nattr;
static char phydir[128];

static int find_phy(void){
    DIR *d=opendir("/sys/bus/iio/devices"); if(!d) return 0;
    struct dirent *e; int found=0;
    while((e=readdir(d))){
        if(strncmp(e->d_name,"iio:device",10)) continue;
        char nm[176]; snprintf(nm,sizeof nm,"/sys/bus/iio/devices/%s/name",e->d_name);
        FILE *f=fopen(nm,"r"); if(!f) continue;
        char b[32]={0}; char *r=fgets(b,sizeof b,f); fclose(f); if(!r) continue;
        if(!strncmp(b,"ad9361-phy",10)){ snprintf(phydir,sizeof phydir,"/sys/bus/iio/devices/%s",e->d_name); found=1; break; }
    }
    closedir(d); return found;
}
static int cmp48(const void *a,const void *b){ return strcmp((const char*)a,(const char*)b); }

/* push one attr into the row list, wrapping the value onto its own indented line
 * when name+value would overflow. Never splits the value itself. */
static void add_attr(const char *name,const char *val){
    if(nrows>=MAXR) return;
    int n=(int)strlen(name), v=(int)strlen(val);
    if(n+1+v <= WCH){                                    /* fits on one line */
        snprintf(rname[nrows],NAMSZ,"%s",name);
        snprintf(rval [nrows],sizeof rval[0],"%s",val);
        nrows++;
    } else {                                             /* name line, then value line */
        snprintf(rname[nrows],NAMSZ,"%.*s",WCH,name);    /* names ~<=32, rarely truncated */
        rval[nrows][0]=0; nrows++;
        if(nrows>=MAXR) return;
        rname[nrows][0]=0;
        if(v <= WCH-2) snprintf(rval[nrows],sizeof rval[0],"%s",val);
        else { snprintf(rval[nrows],sizeof rval[0],"%.*s~",WCH-3,val); }  /* mark truncation */
        nrows++;
    }
}

/* rebuild the row list from sysfs (sorted, values current) */
static void refresh(void){
    nrows=0; nattr=0; if(!phydir[0]) return;
    static char names[MAXR][NAMSZ]; int nn=0;
    DIR *d=opendir(phydir); if(!d) return;
    struct dirent *e;
    while((e=readdir(d)) && nn<MAXR){
        if(e->d_name[0]=='.') continue;
        if(!strcmp(e->d_name,"uevent")||!strcmp(e->d_name,"waveform")) continue;
        snprintf(names[nn],NAMSZ,"%s",e->d_name); nn++;
    }
    closedir(d);
    qsort(names,nn,NAMSZ,cmp48);
    for(int i=0;i<nn;i++){
        char p[200]; snprintf(p,sizeof p,"%s/%s",phydir,names[i]);
        int fd=open(p,O_RDONLY); if(fd<0) continue;              /* write-only attr */
        char v[96]; int n=(int)read(fd,v,sizeof v-1); close(fd);
        if(n<=0) continue;                                       /* dir/EISDIR/empty */
        v[n]=0; char *nl=strchr(v,'\n'); if(nl)*nl=0;            /* first line only */
        add_attr(names[i], v); nattr++;
    }
}

/* ---- CAL screen: AD9361 RX/TX calibration controls (the ONLY writes this app
 * makes). The three RX tracking cals are the "balance IQ" set the panel enables at
 * boot; TX quad + Recalibrate are one-shot actions. Other apps still re-assert the
 * RX tracks on open, so a toggle here is a live setting, not a permanent one. */
static const char *CAL_ATTR[3]={
    "in_voltage_quadrature_tracking_en",     /* RX I/Q image rejection */
    "in_voltage_rf_dc_offset_tracking_en",   /* RX RF DC-offset        */
    "in_voltage_bb_dc_offset_tracking_en",   /* RX BB DC-offset        */
};
static const char *CAL_NAME[5]={
    "RX IQ (quadrature)","RX RF DC offset","RX BB DC offset",
    "TX quadrature cal","Recalibrate RX now",
};
#define NCAL 5
static int cal_rd(int i){                 /* read a tracking_en (-1 if absent) */
    if(!phydir[0]) return -1;
    char p[220]; snprintf(p,sizeof p,"%s/%s",phydir,CAL_ATTR[i]);
    FILE *f=fopen(p,"r"); if(!f) return -1;
    int v=-1; if(fscanf(f,"%d",&v)!=1) v=-1; fclose(f); return v;
}
static int cal_wr(const char *attr,const char *val){     /* 0 = accepted, -1 = missing/rejected */
    if(!phydir[0]) return -1;
    char p[220]; snprintf(p,sizeof p,"%s/%s",phydir,attr);
    int fd=open(p,O_WRONLY); if(fd<0) return -1;          /* attr absent */
    size_t len=strlen(val); ssize_t n=write(fd,val,len); close(fd);
    return n==(ssize_t)len ? 0 : -1;                      /* kernel EINVAL (bad token) -> write<0 */
}
static void cal_avail(char *out,int n){                  /* calib_mode_available (space-sep tokens) */
    out[0]=0; if(!phydir[0]) return;
    char p[220]; snprintf(p,sizeof p,"%s/calib_mode_available",phydir);
    FILE *f=fopen(p,"r"); if(!f) return;
    if(fgets(out,n,f)){ size_t L=strlen(out); if(L && out[L-1]=='\n') out[L-1]=0; } else out[0]=0;
    fclose(f);
}
/* Each action writes, then PROVES the result from the chip so it can be validated
 * on-panel: toggles read back the flag; TX-quad reports which token the driver
 * accepted (auto-falls back to manual_tx_quad); recalibrate reports write status. */
static void cal_action(int i,char *msg,int msz){
    if(i<3){
        int want = cal_rd(i)>0 ? 0 : 1;
        int w = cal_wr(CAL_ATTR[i], want?"1":"0");
        int rb = cal_rd(i);                               /* read back = ground truth */
        int ok = (w==0) && (rb==want);
        snprintf(msg,msz,"%s %s  [%s]", CAL_NAME[i],
                 rb<0?"n/a":(rb?"ON":"OFF"), ok?"ok":"FAIL");
    } else if(i==3){
        const char *tok="tx_quad"; int w=cal_wr("calib_mode","tx_quad");
        if(w<0){ tok="manual_tx_quad"; w=cal_wr("calib_mode",tok); }
        snprintf(msg,msz, w==0 ? "TX quad OK (%s)" : "TX quad REJECTED", tok);
    } else {
        int ok=1; for(int j=0;j<3;j++){ if(cal_wr(CAL_ATTR[j],"0")<0)ok=0; if(cal_wr(CAL_ATTR[j],"1")<0)ok=0; }
        snprintf(msg,msz, ok?"RX cals re-run  [ok]":"RX re-run  [FAIL]");
    }
}
/* space-break word wrap for the calib_mode options readout */
static void cal_wrap(uint16_t *buf,int x,int y,const char *s,int maxc,int lh,int maxln,uint16_t col){
    char line[64]; int li=0, ln=0; const char *p=s;
    while(*p && ln<maxln){
        while(*p==' ') p++;
        const char *w=p; while(*p && *p!=' ') p++; int wl=(int)(p-w);
        if(wl>maxc-1) wl=maxc-1;
        if(li && li+1+wl>maxc){ line[li]=0; pp_text(buf,x,y+ln*lh,line,col,1); ln++; li=0; if(ln>=maxln) break; }
        if(li) line[li++]=' ';
        memcpy(line+li,w,wl); li+=wl;
    }
    if(li && ln<maxln){ line[li]=0; pp_text(buf,x,y+ln*lh,line,col,1); }
}
static void draw_cal(uint16_t *buf,int sel,const char *msg){
    pp_fill(buf,0,TXT_Y0-4,PP_W,PP_H-(TXT_Y0-4),C_BG);
    pp_text(buf,X0,TXT_Y0,"AD9361 CALIBRATION",C_DIM,1);
    int y0=TXT_Y0+16, rh=22;
    for(int i=0;i<NCAL;i++){
        int y=y0+i*rh, foc=(i==sel);
        pp_fill(buf,6,y,PP_W-12,rh-3,foc?C_PANEL:C_PANEL2);
        pp_rect(buf,6,y,PP_W-12,rh-3,foc?C_ACCENT:C_PANEL);
        if(foc) pp_fill(buf,6,y,4,rh-3,C_ACCENT);
        pp_text(buf,16,y+5,CAL_NAME[i],foc?C_TEXT:C_DIM,1);
        char v[12]; uint16_t vc;
        if(i<3){ int e=cal_rd(i);
            if(e<0){ snprintf(v,sizeof v,"n/a"); vc=C_BAD; }
            else if(e){ snprintf(v,sizeof v,"ON"); vc=C_GOOD; }
            else { snprintf(v,sizeof v,"OFF"); vc=C_DIM; }
        } else { snprintf(v,sizeof v,"RUN"); vc=C_ACCENT; }
        pp_text(buf,PP_W-24-pp_tw(v,1),y+5,v,vc,1);
    }
    if(msg && msg[0]) pp_text(buf,X0,y0+NCAL*rh+4,msg,C_WARN,1);
    /* the chip's own token list: lets you confirm 'tx_quad' is valid without serial */
    char av[96]; cal_avail(av,sizeof av);
    int ay=y0+NCAL*rh+18;
    if(av[0]){ pp_text(buf,X0,ay,"calib_mode:",C_DIM,1);
               cal_wrap(buf,X0,ay+11,av,38,11,3,C_TEXT); }
    else       pp_text(buf,X0,ay,"calib_mode: (not present)",C_BAD,1);
}

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi=pp_spi_open(spidev)>=0;

    int ok=find_phy(); refresh();
    int vis=(PP_H-16-TXT_Y0)/LH;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    int32_t last_enc=0; int enc_init=0, enc_accum=0, top=0, confirm=0, tick=0;
    int screen=0, calsel=0, calttl=0; char calmsg[40]={0};   /* screen 0=dump, 1=CAL */
    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_accum += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_accum/4; enc_accum-=steps*4;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;
            if(confirm){
                if(pressed&PP_BTN_CENTER) break;                 /* confirm -> exit */
                if(pressed&PP_BTN_7) confirm=0;                  /* cancel */
            }else if(pressed&PP_BTN_7){ confirm=1;               /* BACK -> confirm exit */
            }else{
                if(pressed&PP_BTN_8){ screen^=1; memset(pp_prev,0xA5,sizeof pp_prev); }  /* dump <-> CAL */
                if(screen==0){                                   /* attribute dump: scroll */
                    top += steps;
                    if(pressed&PP_BTN_DOWN) top++;
                    if(pressed&PP_BTN_UP)   top--;
                }else{                                           /* CAL: select + apply */
                    calsel += steps + !!(pressed&PP_BTN_DOWN) - !!(pressed&PP_BTN_UP);
                    if(calsel<0)calsel=0; if(calsel>=NCAL)calsel=NCAL-1;
                    if(pressed&PP_BTN_CENTER){ cal_action(calsel,calmsg,sizeof calmsg); calttl=100; }
                }
            }
        }
        int maxtop = nrows>vis ? nrows-vis : 0;
        if(top<0) top=0; if(top>maxtop) top=maxtop;
        if(screen==0 && ++tick>=33){ tick=0; refresh(); if(top>maxtop) top=maxtop; }   /* live re-read ~1/s */
        if(calttl>0) calttl--;

        if(screen==1){
            draw_cal(buf,calsel, calttl>0?calmsg:"");
            pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
            pp_text(buf,4,PP_H-12,"UP/DN select  CENTER apply  BTN8 dump  BACK",C_DIM,1);
            pp_ui_status_bar(buf,"RFIC STAT",&st,link_ok,"CAL",C_ACCENT);
        }else{
            pp_fill(buf,0,TXT_Y0-4,PP_W,PP_H-(TXT_Y0-4),C_BG);
            if(!ok) pp_text(buf,X0,TXT_Y0,"ad9361-phy not found",C_BAD,1);
            for(int r=0;r<vis && top+r<nrows;r++){
                int i=top+r, y=TXT_Y0+r*LH;
                if((i&1)==0) pp_fill(buf,0,y-1,PP_W-6,LH,C_PANEL2);          /* zebra rows */
                const char *nm=rname[i], *vl=rval[i];
                if(nm[0] && vl[0]){                                          /* name + value */
                    pp_text(buf,X0,y,nm,C_TEXT,1);
                    pp_text(buf,X0+((int)strlen(nm)+1)*8,y,vl,C_ACCENT,1);
                }else if(nm[0]){                                            /* name only (value wraps) */
                    pp_text(buf,X0,y,nm,C_TEXT,1);
                }else{                                                      /* value continuation, indented */
                    pp_text(buf,X0+8,y,vl,C_ACCENT,1);
                }
            }
            /* scrollbar */
            if(maxtop>0){ int trh=PP_H-16-TXT_Y0, kh=trh*vis/nrows; if(kh<8)kh=8;
                int ky=TXT_Y0+(trh-kh)*top/maxtop;
                pp_fill(buf,PP_W-4,TXT_Y0,2,trh,C_PANEL2); pp_fill(buf,PP_W-4,ky,2,kh,C_ACCENT); }
            pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
            pp_text(buf,4,PP_H-12,"UP/DN or wheel scroll  BTN8 CAL  BACK exit",C_DIM,1);
            char hdr[24]; snprintf(hdr,sizeof hdr,"%d attr",nattr);
            pp_ui_status_bar(buf,"RFIC STAT",&st,link_ok,hdr,C_DIM);   /* short brand: fits beside AP/RF temps */
        }
        pp_confirm_overlay(buf,confirm);
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
