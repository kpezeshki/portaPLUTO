/*
 * pp-SNA — scalar network analyzer (|S21| transmission) on the AD9361.
 *   Per point f_i: TX_LO = RX_LO = f_i - D, transmit a DDS tone at baseband +D
 *   (emits at f_i), and measure RX power in a single DFT bin at +D (D = fs/4, off
 *   DC to dodge LO leakage). One LO step per point. A "reference" (thru) sweep is
 *   stored and later sweeps are normalized: response = P_meas - P_ref [dB].
 *
 *   This is the SAME direct path as the ATTEN/VNA apps (pp_vna.h: libiio RX from
 *   cf-ad9361-lpc + DDS TX), just swept across the LO — NOT maia-sdr. That drops
 *   the maia dependency (and its ~50 MHz span cap / frame-rate quirks) and, since
 *   we read the raw ADC, the RX-vs-FS overload bar in the status bar works here.
 *
 *   DATA : normalized trace; fields CENTER, SPAN, POINTS, TXPWR, RUN, RUN REF.
 *   SETTINGS: IFBW, AVG, REF, RANGE, CONT, RXGAIN, RXBW.
 *
 * usage: pp-sna [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <math.h>
#include "pp_ui.h"
#include "pp_vna.h"                 /* direct RX (libiio) + DDS TX + single-bin DFT */

#define LT_BAND_Y0 20
#define PY0 56
#define PY1 196
#define SNA_MAXPTS 401
#define SNA_FLOOR (-90.0f)

/* ---- fields ---- */
static long span_khz=100000, points=101, txpwr_db=0;     /* DATA: span, #pts, TX level */
static long run_dummy=0, ref_dummy=0;                    /* action-field placeholders  */
static long center_khz=2440000, ifbw_hz=5000, avg_n=4;   /* SETTINGS                   */
static long ref_db=5, range_db=60, cont=0, rxgain_db=0, bw_khz=40000;   /* +RX bandwidth */

static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P5[5]={10000,1000,100,10,1};
static const long P3[3]={100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static int  NTX=1, g_radio=1;
static void fmt_mhz (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_ifbw(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%02ld",*f->val/1000,(*f->val%1000)/10); }
static void fmt_onoff(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
static void fmt_go  (const pp_field_t*f,char*v,int n){ (void)f; snprintf(v,n,"GO"); }

static pp_marker_t g_mark;          /* touch frequency marker */
/* marker DATA fields for no-touch control (mirror g_mark; two-way synced each loop) */
static long mk_on=0, mk_px=PP_W/2;
static void fmt_mk_on (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
/* show the sweep frequency at pixel *val, same f_lo..f_hi span mapping as the plot marker */
static void fmt_mk_pos(const pp_field_t*f,char*v,int n){
    long fk=(center_khz-span_khz/2)+(long)((long long)*f->val*span_khz/(PP_W>1?PP_W-1:1));
    snprintf(v,n,"%ld.%03ld",fk/1000,fk%1000); }
static void mk_toggle(void){ mk_on^=1; }

/* ---- sweep + reference state ---- */
static float REF[SNA_MAXPTS], MEAS[SNA_MAXPTS];
static int   ref_valid=0, have_meas=0, disp_pts=101;
static int   sweeping=0, sweep_is_ref=0, sp_i=0, sw_settle=1;
static long  sw_fs, sw_delta; static long long sw_span, sw_start; static int sw_pts=101;
static double sw_time=0;
static int   g_run=0, g_ref=0;                           /* set by the RUN / RUN REF actions */
static void act_run(void){ g_run=1; }
static void act_ref(void){ g_ref=1; }

static void tx_off(void){ long g; for(int c=0;c<NTX;c++) tg_tone(c,0,0.0,0,&g); }

/* park RX LO (+ matching TX LO) on point i; the tone offset (+D) is fixed */
static void set_point(int i){
    long long lo = sw_start + (sw_pts>1? (long long)i*sw_span/(sw_pts-1) : 0) - sw_delta;
    vna_set_rxlo(lo); vna_set_txlo(lo);                  /* TX LO tracks RX LO */
}
/* flush the RX pipeline so a fresh LO has settled before we integrate */
static void sna_flush(int nb){ static int16_t tmp[VNA_N*2]; for(int k=0;k<nb;k++) iq_rx_read(tmp); }

static void sweep_start(int is_ref){
    sw_fs = ifbw_hz*4096L;                               /* RBW ~= fs/4096 = IFBW */
    if(sw_fs<2100000L)sw_fs=2100000L; if(sw_fs>61440000L)sw_fs=61440000L;
    sw_delta = sw_fs/4;
    sw_span = (long long)span_khz*1000; sw_start=(long long)(center_khz-span_khz/2)*1000;
    sw_pts = (int)points; if(sw_pts<2)sw_pts=2; if(sw_pts>SNA_MAXPTS)sw_pts=SNA_MAXPTS;
    disp_pts = sw_pts;
    sw_settle = 1 + (int)((double)sw_fs*0.001/VNA_N); if(sw_settle>16)sw_settle=16;  /* ~1 ms LO settle */
    sw_time = sw_pts*(double)(avg_n+sw_settle+1)*(4096.0/(double)sw_fs) + sw_pts*0.003;
    vna_set_fs(sw_fs);
    vna_set_rxbw((long)bw_khz*1000);
    vna_set_rxgain((double)rxgain_db);
    vna_set_txatten((double)(-txpwr_db));                /* TX level -> attenuation */
    if(!vna_rx_open()){ g_radio=0; return; }
    vna_tone(sw_delta, 1);                               /* tone on at +D (ch0) */
    { long g; for(int c=1;c<NTX;c++) tg_tone(c,0,0.0,0,&g); }  /* other TX channels off */
    sp_i=0; sweeping=1; sweep_is_ref=is_ref;
    if(!is_ref) have_meas=1;
    set_point(0);
}
/* measure the current point, store, advance to the next (or finish the sweep) */
static void sweep_step(void){
    sna_flush(sw_settle);                                /* let the just-set LO settle */
    double re,im;
    if(vna_measure(sw_delta, (int)avg_n, &re, &im)){
        float db=(float)vna_mag_db(re,im);
        if(sweep_is_ref) REF[sp_i]=db; else MEAS[sp_i]=db;
    } else { g_radio=0; sweeping=0; tx_off(); return; }
    if(++sp_i >= sw_pts){                                 /* sweep complete */
        sweeping=0; tx_off();
        if(sweep_is_ref) ref_valid=1;
        else if(cont && ref_valid) sweep_start(0);        /* continuous re-sweep */
    } else set_point(sp_i);
}

/* ---- display ---- */
static int mapy(float db){ float top=(float)ref_db, bot=top-(float)range_db;
    if(db>=top) return PY0; if(db<=bot) return PY1;
    return PY1 - (int)((db-bot)*(PY1-PY0)/(float)range_db); }
static void draw_sna(uint16_t *buf){
    char s[56];
    pp_fill(buf,0,LT_BAND_Y0+28,PP_W,PP_H-(LT_BAND_Y0+28)-14,C_BG);
    int ndiv=range_db/10; if(ndiv<1)ndiv=1; if(ndiv>12)ndiv=12;
    for(int d=0;d<=ndiv;d++){ int y=PY1-d*(PY1-PY0)/ndiv; pp_fill(buf,0,y,PP_W,1,C_PANEL); }
    int y0=mapy(0.0f); pp_fill(buf,0,y0,PP_W,1,C_DIM);     /* 0 dB thru line */
    snprintf(s,sizeof s,"%ld",ref_db);          pp_text(buf,2,PY0,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld",ref_db-range_db); pp_text(buf,2,PY1-8,s,C_DIM,1);
    snprintf(s,sizeof s,"IFBW %ld.%02ldk  %dpt",ifbw_hz/1000,(ifbw_hz%1000)/10,(int)points);
    pp_text(buf,PP_W-pp_tw(s,1)-2,48,s,C_DIM,1);

    if(ref_valid){                                          /* draw normalized trace */
        int prev=-1, pkx=0; float pk=-1e9f;
        for(int i=0;i<disp_pts;i++){
            float v = have_meas ? (MEAS[i]-REF[i]) : 0.0f;
            int x = (disp_pts>1)? i*(PP_W-1)/(disp_pts-1) : 0, y=mapy(v);
            if(prev>=0){ int a=y<prev?y:prev,b=y<prev?prev:y; pp_fill(buf,x,a,1,b-a+1,C_ACCENT);} else pp_fill(buf,x,y,1,1,C_ACCENT);
            prev=y; if(v>pk){pk=v;pkx=x;}
        }
        if(sweeping){ int cx=(disp_pts>1)? sp_i*(PP_W-1)/(disp_pts-1):0; pp_fill(buf,cx,PY0,1,PY1-PY0,C_PANEL2); }
        if(have_meas){ long pf=(center_khz-span_khz/2)+(long)((double)pkx*span_khz/(PP_W-1));
            snprintf(s,sizeof s,"MK %ld.%03ld MHz  %+d dB",pf/1000,pf%1000,(int)pk); pp_text(buf,2,PY1+13,s,C_GOOD,1); }
    }
    /* x axis */
    long f0=center_khz-span_khz/2, fc=center_khz, f1=center_khz+span_khz/2;
    snprintf(s,sizeof s,"%ld.%03ld",f0/1000,f0%1000); pp_text(buf,0,PY1+3,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld",fc/1000,fc%1000); pp_text(buf,(PP_W-pp_tw(s,1))/2,PY1+3,s,C_ACCENT,1);
    snprintf(s,sizeof s,"%ld.%03ld",f1/1000,f1%1000); pp_text(buf,PP_W-pp_tw(s,1),PY1+3,s,C_DIM,1);
    /* state banner (top of plot) */
    const char *b=0; uint16_t bc=C_WARN;
    if(!g_radio){ b="no radio"; bc=C_BAD; }
    else if(sweeping){ static char t[24]; snprintf(t,sizeof t,"%s %d/%d",sweep_is_ref?"REF":"SWEEP",sp_i,sw_pts); b=t; bc=C_ACCENT; }
    else if(!ref_valid){ b="RUN REFERENCE"; bc=C_BAD; }
    else if(cont){ b="CONT"; bc=C_GOOD; }
    if(b) pp_text(buf,2,48,b,bc,1);
    /* touch marker: snap the dashed line to the nearest sweep point */
    if(g_mark.active){
        int px=g_mark.px; if(px<0)px=0; if(px>=PP_W)px=PP_W-1;
        int dp=disp_pts>1?disp_pts:2;
        int i=(int)((double)px*(dp-1)/(PP_W-1)+0.5); if(i<0)i=0; if(i>=dp)i=dp-1;
        int mx=i*(PP_W-1)/(dp-1);
        long fk=(center_khz-span_khz/2)+(long)((double)i*span_khz/(dp-1));
        char vs[24], fs[24];
        float v=(ref_valid&&have_meas)?(MEAS[i]-REF[i]):0.0f;
        if(ref_valid&&have_meas) snprintf(vs,sizeof vs,"%+.1f dB",v); else snprintf(vs,sizeof vs,"-- dB");
        snprintf(fs,sizeof fs,"%ld.%03ld MHz",fk/1000,fk%1000);
        pp_marker_t mm=g_mark; mm.px=mx;
        pp_marker_draw(buf,&mm,PY0,PY1,PY0,PY1,mapy(v),vs,fs);
    }
}

/* ---- field table ---- */
static pp_field_t F[16]; static int NF=0;
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
    int rx2=0; g_radio=vna_init(&NTX,&rx2); if(NTX<1)NTX=1; if(NTX>TG_MAXCH)NTX=TG_MAXCH;
    if(g_radio) tx_off();                                  /* TX off on launch */

    NF=0;
    addf("CENTER","MHz",&center_khz,70000,6000000,P7,7,3,fmt_mhz,0); /* DATA quick fields */
    addf("SPAN", "MHz",&span_khz, 1000,3000000,P7,7,2,fmt_mhz, 0);
    addf("POINTS","",  &points,   11,  SNA_MAXPTS,P3,3,1,fmt_int,0);
    addf("TXPWR","dB", &txpwr_db,-60, 0,        P2,2,1,fmt_int, 0);
    addf("RUN",  "",   &run_dummy,0,   0,       P1,1,0,fmt_go,  act_run);
    addf("RUNREF","",  &ref_dummy,0,   0,       P1,1,0,fmt_go,  act_ref);
    /* touch marker on the DATA page: on/off toggle + a linear position control
     * (encoder or L/R moves it; L/R is 8x coarse). */
    addf("MARK", "",   &mk_on, 0,1,             P1,1,0,fmt_mk_on, mk_toggle);
    addf("M POS","MHz",&mk_px, 0,PP_W-1,        P1,1,0,fmt_mk_pos,0); F[NF-1].linear=1;
    int ndata=NF;
    addf("IFBW", "kHz",&ifbw_hz, 500, 15000,    P5,5,1,fmt_ifbw,0);
    addf("AVG",  "",   &avg_n,   1,   16,       P2,2,1,fmt_int, 0);
    addf("REF",  "dB", &ref_db, -60,  60,       P2,2,1,fmt_int, 0);
    addf("RANGE","dB", &range_db,10,  120,      P2,2,1,fmt_int, 0);
    addf("CONT", "",   &cont,    0,   1,        P1,1,0,fmt_onoff,0);
    addf("RXGAIN","dB",&rxgain_db,0,  73,       P2,2,1,fmt_int, 0);
    addf("RXBW", "MHz",&bw_khz,  200, 56000,    P5,5,1,fmt_mhz, 0);   /* RX analog bandwidth */

    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    /* watch the sweep-defining params; changing any invalidates the reference */
    long inv[5]={~span_khz,0,0,0,0}, invp[5]={0,0,0,0,0};
    int prev_screen=-1;
    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT){
            if(sweeping||cont){ sweeping=0; cont=0; if(g_radio) tx_off(); }  /* BTN7 stops a sweep */
            else { if(g_radio){ tx_off(); vna_rx_close(); } return 0; } } }  /* ...else exit (TX off) */
        /* marker: apply field state -> g_mark, let a touch override, then reflect back
         * to the fields so both the touchscreen and the encoder/buttons stay in sync. */
        g_mark.active=(int)mk_on; g_mark.px=(int)mk_px;
        if(link_ok && ui.screen==PP_DATA) pp_marker_update(&g_mark,&st,0,PY0,PP_W,PY1-PY0);
        mk_on=g_mark.active; mk_px=g_mark.px;

        /* reference invalidation on span/points/txpwr/center/ifbw change */
        long cur[5]={span_khz,points,txpwr_db,center_khz,ifbw_hz};
        if(!memcmp(cur,invp,sizeof cur) && memcmp(cur,inv,sizeof cur)){
            ref_valid=0; have_meas=0; if(sweeping){ sweeping=0; if(g_radio) tx_off(); }
            memcpy(inv,cur,sizeof cur);
        }
        memcpy(invp,cur,sizeof cur);

        /* actions */
        if(g_ref){ g_ref=0; if(g_radio) sweep_start(1); }
        if(g_run){ g_run=0;
            if(sweeping){ sweeping=0; cont=0; if(g_radio) tx_off(); }        /* RUN again = stop */
            else if(ref_valid && g_radio) sweep_start(0); }

        /* one sweep point per iteration keeps the UI (and BTN7 stop) responsive */
        if(sweeping) sweep_step();

        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        const char *note = !g_radio?"no radio":NULL;
        pp_ui_status_bar(buf,"SNA",&st,link_ok,note,C_BAD);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,LT_BAND_Y0); draw_sna(buf); }
        else                    pp_ui_draw_settings(buf,&ui,LT_BAND_Y0,PP_H-LT_BAND_Y0-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(sweeping?1500:30000);                        /* run fast while sweeping */
    }
    return 0;
}
