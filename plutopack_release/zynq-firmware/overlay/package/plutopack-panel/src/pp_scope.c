/*
 * pp-Scope — free-running oscilloscope for RX ch1/ch2 (magnitude envelope).
 *   Streams a frame of N = time_us * sample_rate samples via libiio (pp_cap.h),
 *   computes per-channel magnitude sqrt(I^2+Q^2) (phase discarded), and plots
 *   each channel in its own vertical band across the full width.
 *   DATA fields: SRATE, LO, GAIN1[, GAIN2], TIME(us, capped to the sample buffer),
 *   MODE (CONT free-run / PAUSE frozen), GO (single capture). Touch the plot for a
 *   time-cursor with per-channel CH1/CH2 readouts (same marker style as SNA/SPECTRUM).
 *
 * usage: pp-scope [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <math.h>
#include "pp_ui.h"
#include "pp_cap.h"

#define LT_BAND_Y0 20
#define D0 48                    /* data area top (under field band) */
#define DBOT (PP_H-14)           /* data area bottom (above footer)  */
#define MAXMAG 2048.0f           /* full-scale magnitude for the vertical axis */

/* ---- fields ---- */
static long srate_khz=30720, lo_khz=2440000, gain1_db=40, gain2_db=40;
static long time_us=200, cont=1, go_dummy=0;
static long bw_khz=18000;                       /* RX analog bandwidth (settings) */
static int  NRX=1;

static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P5[5]={10000,1000,100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static void fmt_mhz(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_onoff(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"CONT":"PAUSE"); }
static void fmt_go(const pp_field_t*f,char*v,int n){ (void)f; snprintf(v,n,"GO"); }
static pp_marker_t g_mark[CAP_MAXCH];   /* independent touch cursor per channel band */
/* marker DATA fields for no-touch control (mirror g_mark; two-way synced each loop) */
static long mk_on[CAP_MAXCH]={0,0}, mk_px[CAP_MAXCH]={PP_W/2,PP_W/2};
static void fmt_mk_on (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
static void fmt_mk_pos(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",(long)*f->val*time_us/(PP_W>1?PP_W-1:1)); }
static void mk0_on(void){ mk_on[0]^=1; }
static void mk1_on(void){ mk_on[1]^=1; }

/* ---- capture state ---- */
static float MAG[CAP_MAXCH][CAP_MAXSAMP];
static float *magp[CAP_MAXCH];
static long  capN=0; static int have_frame=0, g_go=0, g_cap_ok=1;
static void act_go(void){ g_go=1; }

static long calc_n(void){ long n=(long)time_us*srate_khz/1000L;   /* us * (ksamp/s)/1000 */
    if(n<2)n=2; if(n>CAP_MAXSAMP)n=CAP_MAXSAMP; return n; }
static void do_capture(void){
    long n=calc_n();
    if(cap_frame((int)n,NRX,magp)){ capN=n; have_frame=1; g_cap_ok=1; }
    else g_cap_ok=0;
}

/* ---- display: NRX stacked bands, magnitude envelope per band ---- */
static void draw_scope(uint16_t *buf){
    char s[40];
    pp_fill(buf,0,D0,PP_W,DBOT-D0,C_BG);
    uint16_t chc[2]={ pp_rgb(0x36,0xd3,0x99), pp_rgb(0x20,0xc0,0xff) };  /* green, cyan */
    int bandh=(DBOT-D0)/NRX;
    for(int c=0;c<NRX;c++){
        int y0=D0+c*bandh, base=y0+bandh-9, ph=bandh-13; if(ph<8)ph=8;
        pp_fill(buf,0,base,PP_W,1,C_PANEL);                          /* baseline */
        pp_fill(buf,0,y0+2,PP_W,1,C_PANEL2);                         /* top rule */
        if(have_frame && capN>1){
            for(int x=0;x<PP_W;x++){
                long a=(long)x*capN/PP_W, b=(long)(x+1)*capN/PP_W; if(b<=a)b=a+1;
                float mx=0; for(long ssi=a;ssi<b;ssi++){ float m=MAG[c][ssi]; if(m>mx)mx=m; }
                int h=(int)(mx/MAXMAG*ph); if(h>ph)h=ph; if(h<0)h=0;
                if(h>0) pp_fill(buf,x,base-h,1,h,chc[c]);
            }
        }
        snprintf(s,sizeof s,"CH%d",c+1); pp_text(buf,2,y0+2,s,chc[c],1);
        long g = c==0?gain1_db:gain2_db;
        snprintf(s,sizeof s,"%ld dB",g); pp_text(buf,PP_W-pp_tw(s,1)-2,y0+2,s,C_DIM,1);
    }
    /* touch cursors: one per channel band, independently positioned; each shows the
     * dBFS magnitude and the time at its own x. */
    if(have_frame && capN>1) for(int c=0;c<NRX;c++){
        if(!g_mark[c].active) continue;
        int y0=D0+c*bandh, base=y0+bandh-9, ph=bandh-13; if(ph<8)ph=8;
        int px=g_mark[c].px; if(px<0)px=0; if(px>=PP_W)px=PP_W-1;
        long si=(long)px*capN/PP_W; if(si<0)si=0; if(si>=capN)si=capN-1;
        long tus=(long)px*time_us/(PP_W>1?PP_W-1:1);
        float mag=MAG[c][si]; double dbfs = mag>0 ? 20.0*log10((double)mag/MAXMAG) : -99.0;
        if(dbfs<-99.0)dbfs=-99.0;
        int h=(int)(mag/MAXMAG*ph); if(h>ph)h=ph; if(h<0)h=0;
        char vs[16], ts[16];
        snprintf(vs,sizeof vs,"CH%d %.0fdB",c+1,dbfs);
        snprintf(ts,sizeof ts,"%ld us",tus);
        pp_marker_draw(buf,&g_mark[c], y0, y0+bandh, y0, y0+bandh, base-h, vs, ts);
    }
    /* time axis (whole capture) */
    snprintf(s,sizeof s,"0"); pp_text(buf,0,DBOT-8,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld us",time_us); pp_text(buf,PP_W-pp_tw(s,1),DBOT-8,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld MHz  %ld sa", lo_khz/1000,lo_khz%1000, capN); pp_text(buf,(PP_W-pp_tw(s,1))/2,DBOT-8,s,C_DIM,1);
}

/* ---- field table ---- */
static pp_field_t F[16]; static int NF=0;
static char nm_g1[8]="GAIN1", nm_g2[8]="GAIN2";
static void addf(const char*name,const char*unit,long*val,long lo,long hi,
                 const long*pl,int np,int sel,void(*fmt)(const pp_field_t*,char*,int),void(*action)(void)){
    pp_field_t *f=&F[NF++]; memset(f,0,sizeof *f);
    f->name=name; f->unit=unit; f->val=val; f->vmin=lo; f->vmax=hi;
    f->places=pl; f->nplace=np; f->sel=sel; f->fmt=fmt; f->action=action;
}

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi=pp_spi_open(spidev)>=0;
    magp[0]=MAG[0]; magp[1]=MAG[1];
    int cap_ok=cap_init(&NRX); if(NRX<1)NRX=1; if(NRX>CAP_MAXCH)NRX=CAP_MAXCH;

    NF=0;
    addf("SRATE","MHz",&srate_khz,2083,61440,P5,5,1,fmt_mhz,0);
    addf("LO",   "MHz",&lo_khz,   70000,6000000,P7,7,3,fmt_mhz,0);
    addf(nm_g1,  "dB", &gain1_db, 0,73, P2,2,1,fmt_int,0);
    if(NRX>=2) addf(nm_g2,"dB",&gain2_db,0,73,P2,2,1,fmt_int,0);
    addf("TIME", "us", &time_us,  1,5000, P5,5,2,fmt_int,0);
    pp_field_t *ftime=&F[NF-1];   /* TIME field: its vmax is clamped live to the sample limit */
    addf("MODE", "",   &cont,     0,1,  P1,1,0,fmt_onoff,0);
    addf("GO",   "",   &go_dummy, 0,0,  P1,1,0,fmt_go,act_go);
    /* per-channel markers on the DATA page: on/off toggle + a linear position control
     * (encoder or L/R moves it). Marker 2 only when a second RX band exists. */
    addf("MARK1","",   &mk_on[0], 0,1, P1,1,0,fmt_mk_on,mk0_on);
    addf("M1 POS","us",&mk_px[0], 0,PP_W-1, P1,1,0,fmt_mk_pos,0); F[NF-1].linear=1;
    if(NRX>=2){
        addf("MARK2","",   &mk_on[1], 0,1, P1,1,0,fmt_mk_on,mk1_on);
        addf("M2 POS","us",&mk_px[1], 0,PP_W-1, P1,1,0,fmt_mk_pos,0); F[NF-1].linear=1;
    }
    int ndata=NF;
    addf("RXBW", "MHz",&bw_khz,   200,56000, P5,5,2,fmt_mhz,0);   /* settings-only */

    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN]; int prev_screen=-1;
    long cur[5], prevc[5], sentc[5]={~srate_khz,0,0,0,0};    /* srate,lo,gain1,gain2,bw */
    for(int i=0;i<5;i++) prevc[i]=~0L;

    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        /* clamp TIME to what the sample buffer can actually hold at the current rate:
         * n = time_us*srate_khz/1000 <= CAP_MAXSAMP  ->  time_us <= CAP_MAXSAMP*1000/srate_khz */
        { long tmax=(long)CAP_MAXSAMP*1000L/(srate_khz>0?srate_khz:1);
          if(tmax<ftime->vmin) tmax=ftime->vmin;
          ftime->vmax=tmax; if(time_us>tmax) time_us=tmax; }
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) return 0; }
        /* markers: apply field state -> g_mark, let a touch override, then reflect back
         * to the fields so both the touchscreen and the encoder/buttons stay in sync. */
        for(int c=0;c<NRX;c++){ g_mark[c].active=(int)mk_on[c]; g_mark[c].px=(int)mk_px[c]; }
        if(link_ok && ui.screen==PP_DATA){ int bh=(DBOT-D0)/NRX;   /* touch each band moves its own cursor */
            for(int c=0;c<NRX;c++) pp_marker_update(&g_mark[c],&st,0,D0+c*bh,PP_W,bh); }
        for(int c=0;c<NRX;c++){ mk_on[c]=g_mark[c].active; mk_px[c]=g_mark[c].px; }

        /* apply RX control when SRATE/LO/GAIN settle to a new value */
        cur[0]=srate_khz; cur[1]=lo_khz; cur[2]=gain1_db; cur[3]=gain2_db; cur[4]=bw_khz;
        if(cap_ok && !memcmp(cur,prevc,sizeof cur) && memcmp(cur,sentc,sizeof cur)){
            cap_set_fs(srate_khz*1000L); cap_set_lo((long long)lo_khz*1000);   /* LO Hz: 64-bit */
            cap_set_gain(0,(double)gain1_db); if(NRX>=2) cap_set_gain(1,(double)gain2_db);
            cap_set_bw(bw_khz*1000L);
            memcpy(sentc,cur,sizeof cur);
        }
        memcpy(prevc,cur,sizeof cur);

        /* capture: free-run in CONT, else once per GO */
        if(cap_ok){
            if(cont) do_capture();
            else if(g_go){ g_go=0; do_capture(); }
        }
        if(g_go) g_go=0;

        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        const char *note = !cap_ok?"no iio":(!g_cap_ok?"capture err":(cont?"RUN":"PAUSE"));
        uint16_t nc = (!cap_ok||!g_cap_ok)?C_BAD:C_DIM;
        pp_ui_status_bar(buf,"Scope",&st,link_ok,note,nc);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,LT_BAND_Y0); draw_scope(buf); }
        else                    pp_ui_draw_settings(buf,&ui,LT_BAND_Y0,PP_H-LT_BAND_Y0-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(cont?10000:30000);
    }
    return 0;
}
