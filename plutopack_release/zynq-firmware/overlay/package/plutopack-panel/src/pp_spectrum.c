/*
 * pp-Spectrum — swept FFT spectrum analyzer, DIRECT libiio capture + on-CPU FFT.
 *   Engine: single-sideband offset tuning, one LO retune per output pixel. Per
 *   pixel we capture SP_N complex samples from cf-ad9361-lpc (pp_iq.h), run our
 *   own Hann-windowed FFT (pp_fft.h / pp_iq_spectrum), and read the power in a
 *   FIXED positive-IF bin window (never DC, so no LO-leakage spike). Because every
 *   pixel is measured at the SAME relative IF, the AD9361 filter response is a
 *   constant offset for all pixels -> no scalloping at any RBW. The window tiles
 *   the span (LO advances one pixel's worth of Hz per step) so every frequency is
 *   covered; peak-detect within the window so narrow signals in a pixel aren't
 *   missed. RBW = fs/SP_N, so fs = RBW*SP_N (narrower RBW -> lower fs -> smaller
 *   band per step -> more steps -> slower sweep).
 *
 *   NOT maia-sdr — reading the raw ADC means the status-bar RX overload meter
 *   works here too, and the span is no longer capped at maia's ~50 MHz.
 *
 *   DATA  : trace over a dB grid; browsable fields GAIN + MODE (gain-ctrl mode).
 *   SETTINGS: CENTER, SPAN, RBW, REF, RANGE, AVG, MAXHOLD, AUTO, RXBW.
 *
 * usage: pp-spectrum [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <math.h>
#include "pp_ui.h"
#include "pp_iq.h"                  /* direct RX (libiio, cf-ad9361-lpc) */
#include "pp_fft.h"                 /* on-CPU Hann-windowed FFT -> dB spectrum */

#define LT_BAND_Y0 20
#define PY0 56          /* plot top */
#define PY1 196         /* plot bottom */
#define SP_N 4096       /* capture / FFT size */
#define SP_IF_FRAC 0.20 /* fixed IF: positive-frequency window centre (clear of DC) */
#define SP_FLOOR (-120.0f)
#define SP_BATCH 8      /* pixels measured per UI frame (keeps buttons responsive) */

/* ---- fields ---- */
static long gain_db=40, mode=0;                          /* DATA quick fields  */
static long center_khz=2440000, span_khz=200000, rbw_hz=10000;
static long ref_db=60, range_db=80, avg_n=1, maxhold=0, autoscale=1;
static long bw_khz=40000;                                /* RX analog bandwidth */

static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P5[5]={10000,1000,100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static void fmt_mhz (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_rbw (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%02ld",*f->val/1000,(*f->val%1000)/10); }
static void fmt_mode(const pp_field_t*f,char*v,int n){ const char*m[3]={"MANUAL","SLOW","FAST"}; snprintf(v,n,"%s",m[*f->val%3]); }
static void fmt_onoff(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }

static pp_field_t F[16]; static int NF=0;
static void addf(const char*name,const char*unit,long*val,long lo,long hi,
                 const long*pl,int np,int sel,void(*fmt)(const pp_field_t*,char*,int)){
    pp_field_t *f=&F[NF++]; memset(f,0,sizeof *f);
    f->name=name; f->unit=unit; f->val=val; f->vmin=lo; f->vmax=hi;
    f->places=pl; f->nplace=np; f->sel=sel; f->fmt=fmt;
}

static pp_marker_t g_mark;          /* touch frequency marker */
/* marker DATA fields for no-touch control (mirror g_mark; two-way synced each loop) */
static long mk_on=0, mk_px=PP_W/2;
static void fmt_mk_on (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
static void fmt_mk_pos(const pp_field_t*f,char*v,int n){   /* show the frequency at pixel *val, MHz */
    long fk=(center_khz-span_khz/2)+(long)((double)*f->val*span_khz/PP_W);
    snprintf(v,n,"%ld.%03ld",fk/1000,fk%1000); }
static void mk_toggle(void){ mk_on^=1; }

/* ---- sweep state ---- */
static float TR[PP_W], MX[PP_W];    /* trace (dB) + max-hold (dB) */
static int   sw_active=0, sw_step=0, sw_avg=0; static double sw_wacc=0;
static long  sw_fs;
static int   sw_bin0=2867, sw_binw=1;                     /* fixed positive-IF bin window */
static long long sw_if, sw_start, sw_span, sw_pxhz;       /* Hz (64-bit) */
static double sw_time=0;
static int    g_radio=1;
static int16_t g_iqbuf[SP_N*2];

static void sp_gain(void){
    if(mode==0){ iq_set_gainmode("manual"); iq_set_rxgain((double)gain_db); }
    else        iq_set_gainmode(mode==1?"slow_attack":"fast_attack");
}
/* LO for pixel px: tune so pixel px's frequency lands in the fixed IF window */
static long long sw_lo_for(int px){
    long long f = sw_start + (long long)px*sw_pxhz;       /* low edge of pixel px */
    long long lo = f - sw_if;
    if(lo<70000000LL) lo=70000000LL; if(lo>6000000000LL) lo=6000000000LL;
    return lo;
}
static void sp_settle(void){ iq_rx_read(g_iqbuf); }        /* drop one block after an LO retune */

static void sweep_setup(void){
    sw_fs = rbw_hz*4096L;
    if(sw_fs<2100000L) sw_fs=2100000L; if(sw_fs>61440000L) sw_fs=61440000L;
    sw_span = (long long)span_khz*1000; if(sw_span<1) sw_span=1;
    sw_start= (long long)(center_khz - span_khz/2)*1000;
    sw_pxhz = sw_span/PP_W; if(sw_pxhz<1) sw_pxhz=1;
    int off = (int)(SP_IF_FRAC*SP_N + 0.5);                /* +0.20*fs, in bins */
    sw_bin0 = SP_N/2 + off;                                /* DC-centred spectrum -> +IF bin */
    sw_binw = (int)((double)sw_pxhz*SP_N/(double)sw_fs + 0.5); if(sw_binw<1) sw_binw=1;
    if(sw_bin0+sw_binw > SP_N-1){ sw_binw = SP_N-1-sw_bin0; if(sw_binw<1) sw_binw=1; }
    sw_if = (long long)((double)off*(double)sw_fs/SP_N);   /* IF freq of the window */
    sw_time = (double)PP_W*(avg_n+1)*(4096.0/(double)sw_fs) + PP_W*0.001;
    iq_set_fs(sw_fs); iq_set_rxbw((long)bw_khz*1000); sp_gain();
    iq_rx_close();                                          /* re-setup safe: drop any prior buffer */
    if(!iq_rx_open(SP_N)){ g_radio=0; return; }
    sw_step=0; sw_avg=0; sw_wacc=0; sw_active=1;
    iq_set_rxlo(sw_lo_for(0)); sp_settle();
}
/* measure the current pixel from avg_n captures; advance the LO. Also feeds the
 * status-bar RX meter from the raw ADC peak so you can watch headroom. */
static void sp_measure_pixel(void){
    static float db[SP_N];
    int n=iq_rx_read(g_iqbuf); if(n<=0){ g_radio=0; return; }
    int pk=0; for(int i=0;i<2*n;i++){ int v=g_iqbuf[i]<0?-g_iqbuf[i]:g_iqbuf[i]; if(v>pk)pk=v; }
    pp_rx_ol_note(1, pk*100/iq_adc_fs);
    pp_iq_spectrum(g_iqbuf, SP_N, db);
    double mx=-1e30; for(int b=sw_bin0;b<sw_bin0+sw_binw && b<SP_N;b++) if(db[b]>mx)mx=db[b];
    sw_wacc += pow(10.0, mx/10.0);                         /* accumulate linear power */
    if(++sw_avg < avg_n) return;                           /* keep averaging this pixel */
    double pw = sw_wacc/avg_n;
    if(sw_step>=0 && sw_step<PP_W) TR[sw_step] = (float)(pw>0?10.0*log10(pw):SP_FLOOR);
    sw_wacc=0; sw_avg=0;
    if(++sw_step >= PP_W){                                  /* sweep complete */
        if(maxhold) for(int x=0;x<PP_W;x++){ if(TR[x]>MX[x]) MX[x]=TR[x]; }
        sw_step=0;
    }
    iq_set_rxlo(sw_lo_for(sw_step)); sp_settle();           /* retune + settle for next pixel */
}

/* ---- display ---- */
static float g_top=90, g_bot=30;
static int mapy(float db){
    float top=g_top, bot=g_bot; if(top-bot<1) top=bot+1;
    if(db>=top) return PY0; if(db<=bot) return PY1;
    return PY1 - (int)((db-bot)*(PY1-PY0)/(top-bot));
}
static void draw_spectrum(uint16_t *buf){
    char s[48];
    pp_fill(buf,0,LT_BAND_Y0+28,PP_W,PP_H-(LT_BAND_Y0+28)-14,C_BG);
    if(autoscale){
        float mn=1e9f, mx=-1e9f;
        for(int x=0;x<PP_W;x++) if(TR[x]>SP_FLOOR){ if(TR[x]<mn)mn=TR[x]; if(TR[x]>mx)mx=TR[x]; }
        if(mx<mn){ mn=0; mx=80; }
        g_bot=mn-5; g_top=mx+5; if(g_top-g_bot<10) g_top=g_bot+10;
    } else { g_top=(float)ref_db; g_bot=(float)ref_db-(float)range_db; }
    int ndiv=(int)((g_top-g_bot)/10); if(ndiv<1)ndiv=1; if(ndiv>12)ndiv=12;
    for(int d=0;d<=ndiv;d++){ int y=PY1-d*(PY1-PY0)/ndiv; pp_fill(buf,0,y,PP_W,1,C_PANEL); }
    snprintf(s,sizeof s,"%d",(int)g_top); pp_text(buf,2,PY0,s,C_DIM,1);
    snprintf(s,sizeof s,"%d",(int)g_bot); pp_text(buf,2,PY1-8,s,C_DIM,1);
    snprintf(s,sizeof s,"RBW %ld.%02ldk", rbw_hz/1000,(rbw_hz%1000)/10);
    pp_text(buf,PP_W-pp_tw(s,1)-2,48,s,C_DIM,1);
    if(maxhold){ int pp=-1; for(int x=0;x<PP_W;x++){ if(MX[x]<=SP_FLOOR){pp=-1;continue;} int y=mapy(MX[x]);
        if(pp>=0){ int a=y<pp?y:pp,b=y<pp?pp:y; pp_fill(buf,x,a,1,b-a+1,C_DIM);} pp=y; } }
    int prev=-1;
    for(int x=0;x<PP_W;x++){ if(TR[x]<=SP_FLOOR){prev=-1;continue;} int y=mapy(TR[x]);
        if(prev>=0){ int a=y<prev?y:prev,b=y<prev?prev:y; pp_fill(buf,x,a,1,b-a+1,C_ACCENT);} else pp_fill(buf,x,y,1,1,C_ACCENT); prev=y; }
    int cx=(int)((double)sw_step*PP_W/PP_W); if(cx>=0&&cx<PP_W) pp_fill(buf,cx,PY0,1,PY1-PY0,C_PANEL2);
    long f0=center_khz-span_khz/2, fc=center_khz, f1=center_khz+span_khz/2;
    snprintf(s,sizeof s,"%ld.%03ld",f0/1000,f0%1000); pp_text(buf,0,PY1+3,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld",fc/1000,fc%1000); pp_text(buf,(PP_W-pp_tw(s,1))/2,PY1+3,s,C_ACCENT,1);
    snprintf(s,sizeof s,"%ld.%03ld",f1/1000,f1%1000); pp_text(buf,PP_W-pp_tw(s,1),PY1+3,s,C_DIM,1);
    int pk=0; for(int x=1;x<PP_W;x++) if(TR[x]>TR[pk]) pk=x;
    if(TR[pk]>SP_FLOOR){ int y=mapy(TR[pk]); pp_fill(buf,pk-2,y-2,5,1,C_GOOD); pp_fill(buf,pk,y-4,1,5,C_GOOD);
        long pf=(center_khz-span_khz/2)+(long)((double)pk*span_khz/PP_W);
        snprintf(s,sizeof s,"PK %ld.%03ld MHz  %d dB",pf/1000,pf%1000,(int)TR[pk]); pp_text(buf,2,PY1+13,s,C_GOOD,1); }
    /* touch marker: dashed line + value/frequency readout */
    if(g_mark.active){
        int px=g_mark.px; if(px<0)px=0; if(px>=PP_W)px=PP_W-1;
        long fk=(center_khz-span_khz/2)+(long)((double)px*span_khz/PP_W);
        char vs[24], fs[24];
        if(TR[px]>SP_FLOOR) snprintf(vs,sizeof vs,"%.1f dB",TR[px]); else snprintf(vs,sizeof vs,"-- dB");
        snprintf(fs,sizeof fs,"%ld.%03ld MHz",fk/1000,fk%1000);
        int ty=(TR[px]>SP_FLOOR)?mapy(TR[px]):PY1;
        pp_marker_draw(buf,&g_mark,PY0,PY1,PY0,PY1,ty,vs,fs);
    }
}

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi=pp_spi_open(spidev)>=0;
    int htx=0; g_radio=iq_init(&htx);

    NF=0;
    addf("CENTER","MHz",&center_khz,70000,6000000,P7,7,3,fmt_mhz);  /* DATA quick fields */
    addf("SPAN",  "MHz",&span_khz, 1000, 3000000,P7,7,2,fmt_mhz);
    /* marker on the DATA page: on/off toggle + a linear position control (encoder or L/R moves it) */
    addf("MARK", "",   &mk_on, 0,1,       P1,1,0,fmt_mk_on);  F[NF-1].action=mk_toggle;
    addf("M POS","MHz",&mk_px, 0,PP_W-1,  P1,1,0,fmt_mk_pos); F[NF-1].linear=1;
    int ndata=NF;
    addf("RBW",   "kHz",&rbw_hz,   500,  15000,  P5,5,1,fmt_rbw);
    addf("GAIN",  "dB", &gain_db,  0,    73,      P2,2,1,fmt_int);
    addf("MODE",  "",   &mode,     0,    2,       P1,1,0,fmt_mode);
    addf("REF",   "dB", &ref_db,  -20,  120,     P2,2,1,fmt_int);
    addf("RANGE", "dB", &range_db, 10,  120,     P2,2,1,fmt_int);
    addf("AVG",   "",   &avg_n,    1,   16,      P2,2,1,fmt_int);
    addf("MAXHOLD","",  &maxhold,  0,   1,       P1,1,0,fmt_onoff);
    addf("AUTO",   "",   &autoscale,0,  1,       P1,1,0,fmt_onoff);
    addf("RXBW",  "MHz", &bw_khz,   200,  56000,  P5,5,1,fmt_mhz);

    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;
    for(int x=0;x<PP_W;x++){ TR[x]=SP_FLOOR; MX[x]=SP_FLOOR; }

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    long swp[4]={~center_khz,0,0,0}, gnp[2]={~gain_db,0};
    long swc[4], swpv[4]={0,0,0,0}, gnc[2], gnpv[2]={0,0};
    int prev_screen=-1, resync=0;
    memset(buf,0,sizeof buf);
    if(g_radio) sweep_setup();

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT){ if(g_radio) iq_rx_close(); return 0; } }
        /* marker: apply field state -> g_mark, let a touch override, then reflect back
         * so both the touchscreen and the encoder/buttons stay in sync. */
        g_mark.active=(int)mk_on; g_mark.px=(int)mk_px;
        if(link_ok && ui.screen==PP_DATA) pp_marker_update(&g_mark,&st,0,PY0,PP_W,PY1-PY0);
        mk_on=g_mark.active; mk_px=g_mark.px;

        /* restart the sweep when CENTER/SPAN/RBW/AVG settle to a new value */
        swc[0]=center_khz; swc[1]=span_khz; swc[2]=rbw_hz; swc[3]=avg_n;
        if(!memcmp(swc,swpv,sizeof swc) && memcmp(swc,swp,sizeof swc)){ if(g_radio) sweep_setup(); memcpy(swp,swc,sizeof swc); }
        memcpy(swpv,swc,sizeof swc);
        /* re-apply gain/mode when they settle */
        gnc[0]=gain_db; gnc[1]=mode;
        if(!memcmp(gnc,gnpv,sizeof gnc) && memcmp(gnc,gnp,sizeof gnc)){ if(g_radio) sp_gain(); memcpy(gnp,gnc,sizeof gnc); }
        memcpy(gnpv,gnc,sizeof gnc);

        /* measure a batch of pixels this frame */
        if(g_radio && sw_active) for(int k=0;k<SP_BATCH && g_radio;k++) sp_measure_pixel();

        /* AGC modes: reflect the live hardware gain in the field */
        if(mode!=0 && ++resync>=33){ resync=0; double gn; if(iq_get_rxgain(&gn)) gain_db=(long)(gn+0.5); }

        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        const char *note = !g_radio ? "no radio" : NULL;
        pp_ui_status_bar(buf,"Spectrum",&st,link_ok,note,C_BAD);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,LT_BAND_Y0); draw_spectrum(buf); }
        else                    pp_ui_draw_settings(buf,&ui,LT_BAND_Y0,PP_H-LT_BAND_Y0-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(sw_active?1500:30000);
    }
    return 0;
}
