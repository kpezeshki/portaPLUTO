/*
 * pp-vna — Vector Resonator Analyzer (complex S21 magnitude + phase).
 *   Fixed RX/TX LO; the DDS stimulus is stepped digitally across the span so every
 *   point shares one LO and phase is coherent across the sweep. Per point: set the
 *   tone, measure the complex baseband response (pp_vna.h single-bin DFT). A THRU
 *   CAL stores a baseline that later sweeps are divided by (removes cable delay +
 *   ripple). Reports f0, loaded Q (3 dB), and dip depth. Bench/quick-look only —
 *   see app_recommendations.md for the honest limits.
 *
 *   DATA : CENTER, SPAN, POINTS, RUN, CAL      SETTINGS: TXPWR, RXGAIN, AVG, RANGE, CONT
 *   BTN7 : stop a sweep, else exit.
 * usage: pp-vna [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_ui.h"
#include "pp_vna.h"

#define C_TRACE pp_rgb(0x8a,0xe0,0xff)   /* phase trace (cyan) */
#define VNA_MAXPTS 401
#define MY0 54
#define MY1 134                 /* magnitude plot */
#define PHY0 148
#define PHY1 212                /* phase plot */

static long center_khz=5000000, span_khz=10000, points=201;
static long run_dummy=0, cal_dummy=0;
static long txpwr_db=0, rxgain_db=0, avg_n=4, range_db=40, cont=0;
static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P5[5]={10000,1000,100,10,1};
static const long P3[3]={100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static pp_marker_t g_mark;                       /* touch frequency marker */
static double SR[VNA_MAXPTS],SI[VNA_MAXPTS];     /* raw complex response */
static double BR[VNA_MAXPTS],BI[VNA_MAXPTS];     /* cal baseline (through) */
static int    npts=201, have_meas=0, cal_valid=0, g_run=0, g_cal=0, g_radio=1;
static long long sw_lo; static long sw_fs, sw_off0, sw_step;   /* sweep plan */

static void act_run(void){ g_run=1; }
static void act_cal(void){ g_cal=1; }
static void fmt_mhz(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_onoff(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
static void fmt_go(const pp_field_t*f,char*v,int n){ (void)f; snprintf(v,n,"GO"); }
/* marker DATA fields for no-touch control (mirror g_mark; two-way synced each loop) */
static long mk_on=0, mk_px=PP_W/2;
static void fmt_mk_on (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
/* show the RF frequency under px *f->val across the swept span [center-span/2 .. center+span/2] */
static void fmt_mk_pos(const pp_field_t*f,char*v,int n){
    long fk=center_khz - span_khz/2 + (long)((long long)*f->val*span_khz/(PP_W>1?PP_W-1:1));
    snprintf(v,n,"%ld.%03ld",fk/1000,fk%1000); }
static void mk_toggle(void){ mk_on^=1; }

/* map baseband point index -> RF khz (for labels) */
static long pt_khz(int i){ return center_khz - span_khz/2 + (npts>1? (long)((long long)i*span_khz/(npts-1)) : 0); }

static uint8_t poll_pressed(pp_status_t*st){
    static uint8_t last=0xff; uint8_t rx[PP_FRAME_LEN];
    if(pp_spi_fd<0 || pp_poll(rx)!=0 || pp_decode(rx,st)<0) return 0;
    uint8_t pr=st->btn_bitmap & ~last; last=st->btn_bitmap; return pr;
}

static void plan_sweep(void){
    long long span_hz=(long long)span_khz*1000;
    sw_fs = (long)(span_hz*25/10);                 /* fs >= 2.5*span keeps span<0.4fs */
    if(sw_fs<2100000L) sw_fs=2100000L; if(sw_fs>61440000L) sw_fs=61440000L;
    long guard = sw_fs/20;                          /* 0.05*fs offset off DC/LO-leak */
    sw_off0 = guard;
    npts=(int)points; if(npts<2)npts=2; if(npts>VNA_MAXPTS)npts=VNA_MAXPTS;
    sw_step = (long)(span_hz/(npts-1));
    sw_lo = (long long)(center_khz-span_khz/2)*1000 - guard;
}
/* blocking sweep with periodic stop-poll + progress; fills SR/SI (or BR/BI if cal) */
static int do_sweep(uint16_t*buf, pp_status_t*st, int is_cal){
    plan_sweep();
    vna_set_fs(sw_fs); vna_set_rxbw(sw_fs); vna_set_rxlo(sw_lo); vna_set_txlo(sw_lo);
    vna_set_rxgain((double)rxgain_db); vna_set_txatten((double)(-txpwr_db));
    if(!vna_rx_open()){ g_radio=0; return 0; }
    int stopped=0;
    for(int i=0;i<npts;i++){
        long off = sw_off0 + (long)i*sw_step;
        vna_tone(off,1);
        double re,im; if(!vna_measure(off,(int)avg_n,&re,&im)){ g_radio=0; }
        if(is_cal){ BR[i]=re; BI[i]=im; } else { SR[i]=re; SI[i]=im; }
        if((i&7)==0 || i==npts-1){
            if(poll_pressed(st)&PP_BTN_7){ stopped=1; }
            int bx=(npts>1)? i*(PP_W-1)/(npts-1):0;              /* progress marker */
            pp_fill(buf,0,MY0-2,PP_W,1,C_PANEL); pp_fill(buf,bx,MY0-2,2,2,C_ACCENT);
            char s[24]; snprintf(s,sizeof s,"%s %d/%d",is_cal?"CAL":"SWEEP",i+1,npts);
            pp_fill(buf,PP_W-90,22,90,10,C_BG); pp_text(buf,PP_W-88,22,s,C_ACCENT,1);
            pp_ui_status_bar(buf,"VNA",st,1,NULL,C_BAD);     /* keep the RX meter live during the sweep */
            pp_refresh(buf);
            if(stopped) break;
        }
    }
    vna_tone(0,0);                                  /* TX off */
    return !stopped;
}

/* ---- normalized response at point i -> (mag_db, phase_deg) ---- */
static void resp(int i,double*mdb,double*pdeg){
    double re=SR[i], im=SI[i];
    if(cal_valid){ double br=BR[i],bi=BI[i], d=br*br+bi*bi; if(d<1e-12)d=1e-12;
        double nr=(re*br+im*bi)/d, ni=(im*br-re*bi)/d; re=nr; im=ni; }  /* S/B (complex divide) */
    *mdb=vna_mag_db(re,im); *pdeg=vna_phase_deg(re,im);
}
static void draw_plot(uint16_t*buf){
    char s[56];
    pp_fill(buf,0,48,PP_W,PP_H-48-14,C_BG);
    if(!have_meas){ pp_text(buf,4,MY0+30, cal_valid?"RUN a sweep":"CAL (through), then RUN",C_DIM,1); return; }
    /* autoscale magnitude */
    double mmin=1e9,mmax=-1e9, ph[VNA_MAXPTS], mg[VNA_MAXPTS];
    for(int i=0;i<npts;i++){ resp(i,&mg[i],&ph[i]); if(mg[i]<mmin)mmin=mg[i]; if(mg[i]>mmax)mmax=mg[i]; }
    double top=mmax+2, span=(double)range_db; if(top-span>mmin) span=top-mmin+2; if(span<3)span=3;
    /* grid + mag trace */
    for(int d=0;d<=4;d++){ int y=MY0+d*(MY1-MY0)/4; pp_fill(buf,0,y,PP_W,1,C_PANEL); }
    int prev=-1, mi=0; double mv=1e9;
    for(int i=0;i<npts;i++){
        int x=(npts>1)? i*(PP_W-1)/(npts-1):0;
        double f=(top-mg[i])/span; if(f<0)f=0; if(f>1)f=1; int y=MY0+(int)(f*(MY1-MY0));
        if(prev>=0){int a=y<prev?y:prev,b=y<prev?prev:y; pp_fill(buf,x,a,1,b-a+1,C_ACCENT);} else pp_fill(buf,x,y,1,1,C_ACCENT);
        prev=y; if(mg[i]<mv){mv=mg[i];mi=i;}
    }
    snprintf(s,sizeof s,"%.0f",top);      pp_text(buf,2,MY0,s,C_DIM,1);
    snprintf(s,sizeof s,"%.0f dB",top-span); pp_text(buf,2,MY1-8,s,C_DIM,1);
    /* phase trace (-180..180) */
    for(int d=0;d<=2;d++){ int y=PHY0+d*(PHY1-PHY0)/2; pp_fill(buf,0,y,PP_W,1,C_PANEL); }
    prev=-1;
    for(int i=0;i<npts;i++){ int x=(npts>1)? i*(PP_W-1)/(npts-1):0;
        double f=(180.0-ph[i])/360.0; if(f<0)f=0; if(f>1)f=1; int y=PHY0+(int)(f*(PHY1-PHY0));
        if(prev>=0){int a=y<prev?y:prev,b=y<prev?prev:y; pp_fill(buf,x,a,1,b-a+1,C_TRACE);} else pp_fill(buf,x,y,1,1,C_TRACE);
        prev=y; }
    pp_text(buf,2,PHY0,"+180",C_DIM,1); pp_text(buf,2,PHY1-8,"-180",C_DIM,1);
    pp_text(buf,PP_W-40,PHY0,"phase",C_DIM,1);
    /* Q fit from the dip: 3 dB above the minimum */
    double thr=mv+3.0; int lo=mi,hi=mi;
    while(lo>0 && mg[lo]<thr) lo--; while(hi<npts-1 && mg[hi]<thr) hi++;
    long f0=pt_khz(mi), flo=pt_khz(lo), fhi=pt_khz(hi);
    double Ql = (fhi>flo)? (double)f0/(double)(fhi-flo) : 0;
    snprintf(s,sizeof s,"f0 %ld.%03ld MHz  QL %.0f  dip %.1f dB",f0/1000,f0%1000,Ql, top-mv);
    pp_text(buf,2,PHY1+2,s,C_GOOD,1);
    /* x axis */
    long f0k=center_khz-span_khz/2, fck=center_khz, f1k=center_khz+span_khz/2;
    snprintf(s,sizeof s,"%ld.%03ld",f0k/1000,f0k%1000); pp_text(buf,0,MY1+2,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld",fck/1000,fck%1000); pp_text(buf,(PP_W-pp_tw(s,1))/2,MY1+2,s,C_ACCENT,1);
    snprintf(s,sizeof s,"%ld.%03ld",f1k/1000,f1k%1000); pp_text(buf,PP_W-pp_tw(s,1),MY1+2,s,C_DIM,1);
    /* touch marker: dashed line spans both plots; label (|S21| + freq) in the mag plot */
    if(g_mark.active){
        int px=g_mark.px; if(px<0)px=0; if(px>=PP_W)px=PP_W-1;
        int np=npts>1?npts:2;
        int i=(int)((double)px*(np-1)/(PP_W-1)+0.5); if(i<0)i=0; if(i>=np)i=np-1;
        int mx=i*(PP_W-1)/(np-1);
        long fk=pt_khz(i);
        char vs[24], fk_s[24];
        snprintf(vs,sizeof vs,"%.1f dB",mg[i]);
        snprintf(fk_s,sizeof fk_s,"%ld.%03ld MHz",fk/1000,fk%1000);
        double ff=(top-mg[i])/span; if(ff<0)ff=0; if(ff>1)ff=1; int ty=MY0+(int)(ff*(MY1-MY0));
        pp_marker_t mm=g_mark; mm.px=mx;
        pp_marker_draw(buf,&mm,MY0,PHY1,MY0,MY1,ty,vs,fk_s);
    }
}

static pp_field_t F[14]; static int NF=0;
static void addf(const char*nm,const char*u,long*v,long lo,long hi,const long*pl,int np,int sel,
                 void(*fmt)(const pp_field_t*,char*,int),void(*act)(void)){
    pp_field_t*f=&F[NF++]; memset(f,0,sizeof*f);
    f->name=nm; f->unit=u; f->val=v; f->vmin=lo; f->vmax=hi; f->places=pl; f->nplace=np; f->sel=sel; f->fmt=fmt; f->action=act;
}
int main(int argc,char**argv){
    const char*spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char*fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi=pp_spi_open(spidev)>=0;
    int nt,r2; g_radio=vna_init(&nt,&r2);

    NF=0;
    addf("CENTER","MHz",&center_khz,70000,6000000,P7,7,3,fmt_mhz,0);
    addf("SPAN","MHz",&span_khz,100,24000,P5,5,1,fmt_mhz,0);       /* <=24 MHz single-LO */
    addf("POINTS","",&points,21,VNA_MAXPTS,P3,3,1,fmt_int,0);
    addf("RUN","",&run_dummy,0,0,P1,1,0,fmt_go,act_run);
    addf("CAL","",&cal_dummy,0,0,P1,1,0,fmt_go,act_cal);
    /* touch marker control on the DATA page: on/off toggle + a linear position control
     * (encoder or L/R moves it; L/R is 8x coarse) that mirrors the touch frequency marker. */
    addf("MARK","",&mk_on,0,1,P1,1,0,fmt_mk_on,mk_toggle);
    addf("M POS","MHz",&mk_px,0,PP_W-1,P1,1,0,fmt_mk_pos,0); F[NF-1].linear=1;
    int ndata=NF;
    addf("TXPWR","dB",&txpwr_db,-60,0,P2,2,1,fmt_int,0);
    addf("RXGAIN","dB",&rxgain_db,0,73,P2,2,1,fmt_int,0);
    addf("AVG","",&avg_n,1,32,P2,2,1,fmt_int,0);
    addf("RANGE","dB",&range_db,6,80,P2,2,1,fmt_int,0);
    addf("CONT","",&cont,0,1,P1,1,0,fmt_onoff,0);

    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;
    static uint16_t buf[PP_W*PP_H]; pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN]; int prev_screen=-1; memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) break; }
        /* marker: apply field state -> g_mark, let a touch override, then reflect back
         * so both the touchscreen and the encoder/buttons stay in sync. */
        g_mark.active=(int)mk_on; g_mark.px=(int)mk_px;
        if(link_ok && ui.screen==PP_DATA) pp_marker_update(&g_mark,&st,0,MY0,PP_W,PHY1-MY0);
        mk_on=g_mark.active; mk_px=g_mark.px;
        pp_ui_sync(&ui);

        if(g_cal){ g_cal=0; if(g_radio){ if(do_sweep(buf,&st,1)) cal_valid=1; prev_screen=-1; } }
        if(g_run){ g_run=0; if(g_radio){
            do{ have_meas=1; if(!do_sweep(buf,&st,0)) cont=0; }while(cont && g_radio);
            prev_screen=-1; } }

        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        const char*note = !g_radio?"no radio":(cal_valid?NULL:"uncal");
        pp_ui_status_bar(buf,"VNA",&st,link_ok,note, g_radio?C_WARN:C_BAD);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,20); draw_plot(buf); vna_draw_ports(buf,"TX1 out / RX1 in"); }
        else                    pp_ui_draw_settings(buf,&ui,20,PP_H-20-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    vna_tone(0,0); vna_rx_close();
    return 0;
}
