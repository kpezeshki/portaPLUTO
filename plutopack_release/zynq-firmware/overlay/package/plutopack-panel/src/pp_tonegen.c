/*
 * pp-ToneGen — AD9361 CW tone generator over the IIO sysfs ABI (see pp_iio.h).
 *   DATA screen : per-channel FREQUENCY (RF) and ATTENUATION; a frequency ruler
 *                 shows each channel's tone. UP/DOWN/WHEEL cycle the field,
 *                 CENTER edits, L/R digit, wheel/UP-DOWN value.
 *   SETTINGS    : RF bandwidth, sample rate, master output enable, per-channel
 *                 enable. Same edit model.
 * The AD9361 has ONE shared TX LO: the app parks it 1 MHz below the lowest
 * channel and realises each channel as an independent DDS baseband offset
 * (channels must lie within ~fs/2 of the LO). Every settled change re-tunes the
 * radio and reconciles the display to the read-back (amber = not yet confirmed).
 *
 * usage: pp-tonegen [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_ui.h"
#include "pp_iio.h"
#include "pp_cap.h"                /* RX capture (libiio) to feed the status-bar RX meter */

#define LO_MIN_KHZ   70000L
#define LO_MAX_KHZ   6000000L
#define RF_MIN_KHZ   71000L        /* >= LO_MIN + 1 MHz offset */
#define RF_MAX_KHZ   6000000L
#define ATT_MAX      89L
#define BW_MIN_KHZ   200L
#define BW_MAX_KHZ   40000L
#define FS_MIN_KHZ   2083L
#define FS_MAX_KHZ   61440L
#define F_OFF_KHZ    1000L         /* park LO this far below the lowest tone */
#define RETUNE_RETRY 33            /* ~1s backoff on IIO failure */
#define LT_BAND_Y0   20            /* field band just under the status bar */
#define LT_D0        52            /* data area starts here */

/* ---- backing values (the fields point at these) ---- */
static long rf_khz[TG_MAXCH] = {2440000, 2450000};
static long att_db[TG_MAXCH] = {20, 20};
static long en[TG_MAXCH]     = {1, 0};
static long phase_deg[TG_MAXCH] = {0, 0};
static long bw_khz = 18000, fs_khz = 30720, master = 1;
static int  NTX = 1;
static long g_lo_khz = 2439000;    /* last applied LO (for the ruler) */

/* read-back verification warning (shown as a banner for ~a few seconds) */
static char g_warn[56]; static int g_warn_ttl = 0;
static void g_warn_set(const char *m){ snprintf(g_warn,sizeof g_warn,"%s",m); g_warn_ttl=150; }
static void g_warn_clear(void){ g_warn_ttl=0; }

static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P5[5]={10000,1000,100,10,1};
static const long P3[3]={100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static void fmt_mhz(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_onoff(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }

/* ---- field table (built after we know NTX) ---- */
static pp_field_t F[3 + 4*TG_MAXCH];        /* OUT,BW,RATE + per-ch FRQ,ATT,ON,PH */
static int NF=0;
static char nm_frq[TG_MAXCH][8], nm_att[TG_MAXCH][8], nm_en[TG_MAXCH][8], nm_ph[TG_MAXCH][8];
static void addf(const char*name,const char*unit,long*val,long lo,long hi,
                 const long*pl,int np,int sel,void(*fmt)(const pp_field_t*,char*,int)){
    pp_field_t *f=&F[NF++]; memset(f,0,sizeof *f);
    f->name=name; f->unit=unit; f->val=val; f->vmin=lo; f->vmax=hi;
    f->places=pl; f->nplace=np; f->sel=sel; f->fmt=fmt;
}

/* Push the whole radio state; after each write read the value back via IIO and
 * (a) reconcile the display to the applied value, (b) if the applied value is far
 * from what we asked — a rejected/ignored write — record a warning. Transport
 * failures (IIO write/read error) return 0 so the caller retries with backoff. */
#define TG_WARN(cond,msg) do{ if(!w[0] && !(cond)) snprintf(w,sizeof w,"%s",msg); }while(0)
static int do_retune(void){
    char w[56]=""; long req_bw=bw_khz, req_fs=fs_khz;
    long req_rf[TG_MAXCH], req_att[TG_MAXCH];
    for(int c=0;c<NTX;c++){ req_rf[c]=rf_khz[c]; req_att[c]=att_db[c]; }

    long lo = rf_khz[0] - F_OFF_KHZ;              /* C1 is master: LO parks just below it */
    if(lo<LO_MIN_KHZ) lo=LO_MIN_KHZ; if(lo>LO_MAX_KHZ) lo=LO_MAX_KHZ;

    long long lo_got=0; if(!tg_lo((long long)lo*1000,&lo_got)){ g_warn_set("LO: IIO write failed"); return 0; }
    g_lo_khz = (long)(lo_got/1000);
    TG_WARN(llabs(lo_got-(long long)lo*1000)<=10000, "LO did not apply");

    long bwg=0,fsg=0;
    if(!tg_bw(req_bw*1000L,&bwg)){ g_warn_set("BW: IIO write failed"); return 0; } bw_khz=bwg/1000L;
    if(!tg_fs(req_fs*1000L,&fsg)){ g_warn_set("RATE: IIO write failed"); return 0; } fs_khz=fsg/1000L;
    TG_WARN(labs(fsg-req_fs*1000L)<=10000, "RATE did not apply");   /* BW quantizes coarsely: no strict check */
    long nyq = fs_khz/2;

    for(int c=0;c<NTX;c++){
        long want_off = req_rf[c]-g_lo_khz; if(want_off<0) want_off=0; if(want_off>nyq) want_off=nyq;
        double scale = (en[c] && master) ? TG_AMP : 0.0;
        long offg=0; double attg=0;
        if(!tg_tone(c, want_off*1000L, scale, phase_deg[c], &offg)){ snprintf(w,sizeof w,"C%d tone: IIO fail",c+1); g_warn_set(w); return 0; }
        rf_khz[c] = g_lo_khz + offg/1000L;                 /* realised RF */
        if(!w[0] && labs(rf_khz[c]-(g_lo_khz+want_off))>10) snprintf(w,sizeof w,"C%d FREQ did not apply",c+1);
        if(!tg_atten(c, (double)req_att[c], &attg)){ snprintf(w,sizeof w,"C%d atten: IIO fail",c+1); g_warn_set(w); return 0; }
        att_db[c] = (long)(attg+0.5);
        if(!w[0] && fabs(attg-(double)req_att[c])>1.0) snprintf(w,sizeof w,"C%d ATTEN did not apply",c+1);
    }
    if(w[0]) g_warn_set(w); else g_warn_clear();            /* clear once everything verifies */
    return 1;
}

/* ---- data-screen visualization: ruler + tone markers + status rows ---- */
static void draw_data(uint16_t *buf){
    char s[40];
    pp_fill(buf,0,LT_D0,PP_W,PP_H-LT_D0-14,C_BG);
    snprintf(s,sizeof s,"TONE OUT   fs %ld.%ld MHz   BW %ld.%ld", fs_khz/1000,(fs_khz%1000)/100, bw_khz/1000,(bw_khz%1000)/100);
    pp_text(buf,6,LT_D0+2,s,C_DIM,1);
    int ax=10, aw=PP_W-20, ay=LT_D0+70;                    /* ruler baseline */
    long f0=g_lo_khz, f1=g_lo_khz + fs_khz/2; if(f1<=f0) f1=f0+1;
    pp_fill(buf,ax,ay,aw,1,C_DIM);
    snprintf(s,sizeof s,"%ld.%03ld",f0/1000,f0%1000); pp_text(buf,ax,ay+6,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld",f1/1000,f1%1000); pp_text(buf,ax+aw-pp_tw(s,1),ay+6,s,C_DIM,1);
    pp_fill(buf,ax,ay-2,1,5,C_DIM); pp_fill(buf,ax+aw-1,ay-2,1,5,C_DIM);
    pp_text(buf,ax,ay-14,"LO",C_ACCENT,1);
    for(int c=0;c<NTX;c++){
        long x = ax + (long)(rf_khz[c]-f0)*aw/(f1-f0); if(x<ax)x=ax; if(x>ax+aw-1)x=ax+aw-1;
        int on = en[c] && master;
        uint16_t col = on?C_GOOD:C_DIM;
        int h = on ? 44 - (int)(att_db[c]*40/ATT_MAX) : 6;  /* taller = less atten */
        if(h<4)h=4;
        pp_fill(buf,(int)x,ay-h,2,h,col);
        snprintf(s,sizeof s,"C%d",c+1); pp_text(buf,(int)x-3,ay-h-10,s,col,1);
    }
    int ry=ay+22;
    for(int c=0;c<NTX;c++){
        int on = en[c] && master;
        snprintf(s,sizeof s,"C%d %ld.%03ld MHz  -%ld dB  %s",
                 c+1, rf_khz[c]/1000, rf_khz[c]%1000, att_db[c], on?"ON":"OFF");
        pp_text(buf,8,ry+c*14,s, on?C_TEXT:C_DIM,1);
    }
}

int main(int argc, char **argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    long fs_hz=0; int iio_ok = tg_init(&NTX, &fs_hz);
    if(NTX<1) NTX=1; if(NTX>TG_MAXCH) NTX=TG_MAXCH;
    if(iio_ok && fs_hz>0) fs_khz = fs_hz/1000L;

    /* Optional RX loopback meter: sample RX so the status-bar RX1/RX2 bars show
     * the looped-back tone level if the user has TX->RX wired. RX is independent
     * of the DDS TX path (different device), so it doesn't perturb the tone. */
    int nrx=0, rx_ok=cap_init(&nrx);
    if(rx_ok){ cap_set_gain(0,20.0); if(nrx>1) cap_set_gain(1,20.0); }
    long rx_lo_khz=-1;

    /* Persistence: adopt the AD9361's CURRENT TX state on open (a tone left running
     * by a previous session is read back and shown, and NOT re-applied on open). */
    if(iio_ok){
        long long lo=0; int haslo = tg_get_lo(&lo) && lo>0; int anyon=0;
        if(haslo) g_lo_khz = (long)(lo/1000);               /* ruler reflects the live LO */
        for(int c=0;c<NTX;c++){
            long off=0, ph=0; double sc=0;
            if(tg_get_tone(c,&off,&sc,&ph)){
                if(haslo){ long r=(long)((lo+off)/1000); if(r<RF_MIN_KHZ)r=RF_MIN_KHZ; if(r>RF_MAX_KHZ)r=RF_MAX_KHZ; rf_khz[c]=r; }
                en[c] = (sc>0.01)?1:0; if(en[c]) anyon=1;
                phase_deg[c] = ((ph%360)+360)%360;
            }
            double a=0; if(tg_get_atten(c,&a)){ if(a<0)a=0; if(a>ATT_MAX)a=ATT_MAX; att_db[c]=(long)(a+0.5); }
        }
        master = anyon;                                     /* on if any channel is live */
        long bw=0,fs=0;
        if(tg_get_bw(&bw)&&bw>0){ long k=bw/1000L; if(k<BW_MIN_KHZ)k=BW_MIN_KHZ; if(k>BW_MAX_KHZ)k=BW_MAX_KHZ; bw_khz=k; }
        if(tg_get_fs(&fs)&&fs>0){ long k=fs/1000L; if(k<FS_MIN_KHZ)k=FS_MIN_KHZ; if(k>FS_MAX_KHZ)k=FS_MAX_KHZ; fs_khz=k; }
    }

    /* DATA quick fields: OUT (global on/off) + per-ch FREQ,ATT. SETTINGS lists all
     * plus BW, RATE, per-channel PHASE, and per-channel enables. */
    NF=0;                                                   /* rebuild fresh (app may be re-launched in-process) */
    addf("OUT", "",   &master,0,1,P1,1,0,fmt_onoff);        /* global on/off */
    for(int c=0;c<NTX;c++){
        snprintf(nm_frq[c],sizeof nm_frq[c],"C%d FRQ",c+1);
        snprintf(nm_att[c],sizeof nm_att[c],"C%d ATT",c+1);
        snprintf(nm_en[c], sizeof nm_en[c], "C%d ON", c+1);
        addf(nm_frq[c],"MHz",&rf_khz[c],RF_MIN_KHZ,RF_MAX_KHZ,P7,7,3,fmt_mhz);
        addf(nm_att[c],"dB", &att_db[c],0,ATT_MAX,       P2,2,1,fmt_int);
        addf(nm_en[c], "",   &en[c],    0,1,             P1,1,0,fmt_onoff);   /* per-ch enable on DATA */
    }
    int ndata=NF;                                           /* OUT + per-ch FREQ/ATT/ON */
    addf("BW",  "MHz",&bw_khz,BW_MIN_KHZ,BW_MAX_KHZ,P5,5,1,fmt_mhz);
    addf("RATE","MHz",&fs_khz,FS_MIN_KHZ,FS_MAX_KHZ,P5,5,1,fmt_mhz);
    for(int c=0;c<NTX;c++){ snprintf(nm_ph[c],sizeof nm_ph[c],"C%d PH",c+1);
        addf(nm_ph[c],"deg",&phase_deg[c],0,359,P3,3,1,fmt_int); }

    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    long snap[32], prevsnap[32], sentsnap[32]; int have_sent=1, backoff=0;
    int prev_screen=-1;

    memset(buf,0,sizeof buf);
    for(int i=0;i<NF;i++){ prevsnap[i]=*F[i].val; sentsnap[i]=*F[i].val; }  /* adopt hw state; no push until edited */

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) return 0; }

        /* C1 is master: the LO parks just below C1, so C1 tunes freely. C2 must lie
         * in the DAC band above the LO, [C1-F_OFF, C1-F_OFF+fs/2]; if moving C1
         * pushes C2 out of band, snap C2 to the nearest edge. Editing C2 inside the
         * band never touches C1 (no case moves C1 from a C2 change). */
        if(NTX>1){
            long blo = rf_khz[0]-F_OFF_KHZ, bhi = blo + fs_khz/2;
            if(blo<RF_MIN_KHZ) blo=RF_MIN_KHZ; if(bhi>RF_MAX_KHZ) bhi=RF_MAX_KHZ;
            if(rf_khz[1]<blo) rf_khz[1]=blo; else if(rf_khz[1]>bhi) rf_khz[1]=bhi;
        }

        /* enforce MIN < channels within one Nyquist span of the parked LO */
        for(int i=0;i<NF;i++) snap[i]=*F[i].val;
        int settled = (memcmp(snap,prevsnap,NF*sizeof(long))==0);
        int dirty   = (!have_sent) || memcmp(snap,sentsnap,NF*sizeof(long))!=0;
        if(settled && dirty){
            if(backoff>0){ backoff--; for(int i=0;i<NF;i++) F[i].synced=0; }
            else if(iio_ok && do_retune()){
                for(int i=0;i<NF;i++){ sentsnap[i]=*F[i].val; F[i].synced=1; }
                have_sent=1; backoff=0;
            } else { for(int i=0;i<NF;i++) F[i].synced=0; backoff=iio_ok?RETUNE_RETRY:RETUNE_RETRY; }
        }
        for(int i=0;i<NF;i++) prevsnap[i]=snap[i];

        /* keep the TX LO powered up while transmitting: maia re-asserts
         * TX_LO_powerdown at ~1 Hz, so re-clear it every loop or the tone dies
         * a second after each retune (see tg_txlo_on in pp_iio.h). */
        if(iio_ok && master){ int anyon=0; for(int c=0;c<NTX;c++) if(en[c]) anyon=1;
            if(anyon) tg_txlo_on(); }

        /* RX loopback meter: tune RX to the TX LO and sample a small block so the
         * status-bar RX1/RX2 bars reflect the looped-back tone (if TX->RX wired). */
        if(rx_ok){
            if(g_lo_khz!=rx_lo_khz){ cap_set_lo((long long)g_lo_khz*1000); rx_lo_khz=g_lo_khz; }
            static float m0[256], m1[256]; float *mag[2]={m0,m1};
            cap_frame(256, nrx>2?2:nrx, mag);
        }

        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }

        int all_sync=1; for(int i=0;i<NF;i++) if(!F[i].synced) all_sync=0;
        const char *note = !iio_ok ? "no iio" : (!all_sync ? "sync.." : NULL);
        uint16_t nc = !iio_ok ? C_BAD : C_WARN;
        pp_ui_status_bar(buf,"ToneGen",&st,link_ok,note,nc);

        int ac = ui.focus; int active_sync = (ac<NF)?F[ac].synced:1;
        if(ui.screen==PP_DATA){
            pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,LT_BAND_Y0);
            draw_data(buf);
        } else {
            pp_ui_draw_settings(buf,&ui,LT_BAND_Y0,PP_H-LT_BAND_Y0-14);
        }
        (void)active_sync;
        if(g_warn_ttl>0){                                   /* read-back verification warning */
            pp_fill(buf,0,PP_H-28,PP_W,14,C_BAD);
            pp_text(buf,4,PP_H-26,g_warn,C_BG,1);
            g_warn_ttl--;
        }
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
