/*
 * pp-sdr — front-panel SDR over the REAL maia-sdr spectrum, built on pp_ui.h.
 *   DATA screen : scrolling waterfall of maia's real power spectrum, with one
 *                 adjustable field shown above it. UP/DOWN/WHEEL cycle the field
 *                 (CENTER -> SPAN -> GAIN -> MIN -> MAX); CENTER edits it.
 *   SETTINGS    : BTN8 -> list of all five settings, same edit model.
 *   BTN7        : layered back (edit -> browse -> screen -> app switcher).
 * CENTER/SPAN/GAIN are pushed to the AD9361 via PATCH /api/ad9361 and verified
 * by read-back (see pp_maia.h / pp_ui.h sync rule); MIN/MAX are the local fixed
 * colormap range for the waterfall. See the UI style spec.
 *
 * usage: pp-sdr [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <math.h>
#include "pp_ui.h"
#include "pp_waterfall.h"        /* PPWF_TURBO[] */
#include "pp_maia.h"

/* ---- layout ---- */
#define LT_BAND_Y0 20
#define LT_AXIS_Y  50
#define LT_WF_Y0   62
#define LT_WF_ROWS 162           /* 62 .. 224, above the 226px footer */

/* ---- tuner values + digit places ---- */
static long lo_khz   = 2442000; static const long PLACE_CTR[7]={1000000,100000,10000,1000,100,10,1};
static long span_khz = 20000;   static const long PLACE_SPN[5]={10000,1000,100,10,1};
static long gain_db  = 40;      static const long PLACE_GN[2] ={10,1};
static long cmin_db  = 45;      static const long PLACE_DB[2] ={10,1};
static long cmax_db  = 78;
static long bw_khz   = 18000;   /* RX analog bandwidth (settings) */
#define LO_MIN 70000L
#define LO_MAX 6000000L
#define SPAN_MIN 2000L
/* The maia spectrometer on the DDS-enabled bitstream stops streaming above ~50 MHz
 * (measured: 50 MHz OK, 56 MHz dead) — the ADC-rate FFT pipeline can't keep up at
 * the full 61.44 Msps this build. Cap the max span at the highest confirmed-
 * streaming rate so full span never freezes the waterfall. */
#define SPAN_MAX 50000L
#define GAIN_MIN 0L
#define GAIN_MAX 71L
#define DB_LIMIT 120L
#define RESYNC_LOOPS 66          /* ~2s: re-read radio to catch external changes */
#define WF_TARGET_FPS 25         /* waterfall scroll rate we aim maia to deliver */

/* maia frame rate = (fs/4096)/number_integrations. A FIXED number_integrations
 * (the old code pinned it at 1023) makes the frame rate collapse as the span
 * (=fs) narrows: at 2 MHz span that is (2e6/4096)/1023 = 0.5 fps, so the
 * waterfall looks frozen and tuning/span feel dead. Recompute NI from the
 * CURRENT fs to hold ~WF_TARGET_FPS, and re-apply whenever span changes. */
static void wf_set_ni(long long fs_hz){
    if(fs_hz<1) fs_hz=1;
    long ni = (long)((double)fs_hz/4096.0/(double)WF_TARGET_FPS + 0.5);
    if(ni<1) ni=1; if(ni>1023) ni=1023;
    char j[48]; snprintf(j,sizeof j,"{\"number_integrations\":%ld}", ni);
    maia_patch_spec(j);
}

/* ---- value formatters ---- */
static void fmt_ctr (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%04ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_span(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%02ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%02ld",*f->val); }
static const char *GMODE[3]={"MANUAL","SLOW","FAST"};
static void fmt_mode(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",GMODE[(*f->val)%3]); }

/* gain-control mode: 0 Manual, 1 Slow attack, 2 Fast attack (maia JSON strings) */
static long gmode=0;
static const long PLACE_1[1]={1};

/* ---- maia apply callbacks (64-bit Hz — kHz*1000 exceeds 2^31 above 2.147 GHz) ---- */
static int ap_lo(pp_field_t*f){
    long long lo=(long long)*f->val*1000, sp=0; double gn=0;
    char j[80]; snprintf(j,sizeof j,"{\"rx_lo_frequency\":%lld}", lo);
    if(!maia_patch_get(j,&lo,&sp,&gn)) return 0; *f->val=(long)(lo/1000); return 1;
}
static int ap_span(pp_field_t*f){
    long long lo=0, sp=(long long)*f->val*1000; double gn=0;
    char j[80]; snprintf(j,sizeof j,"{\"sampling_frequency\":%lld}", sp);
    if(!maia_patch_get(j,&lo,&sp,&gn)) return 0; *f->val=(long)(sp/1000);
    /* changing fs (a) collapses the maia frame rate unless NI is rescaled and
     * (b) leaves the AD9361 analog RF filter at its old width, so narrowing the
     * span used to change nothing visible. Rescale NI for the new fs and pull the
     * analog bandwidth in to ~0.8*fs so the filter tracks the displayed span. */
    wf_set_ni(sp);
    { long long bw=(long long)(0.8*(double)sp);
      char b[64]; snprintf(b,sizeof b,"{\"rx_rf_bandwidth\":%lld}", bw);
      maia_patch(b); bw_khz=(long)(bw/1000); }
    return 1;
}
/* push gain-control mode then gain; reconcile the RX-gain read-back.
 * maia rejects a COMBINED {mode,gain} PATCH when it follows an fs/bw change
 * (returns 422), so mode and gain must go as two separate patches. */
static int gain_push(long *reconcile){
    long long lo=0,sp=0; double gn=gain_db; char j[48];
    if(gmode==0){
        if(!maia_patch("{\"rx_gain_mode\":\"Manual\"}")) return 0;
        snprintf(j,sizeof j,"{\"rx_gain\":%ld.0}", gain_db);
        if(!maia_patch_get(j,&lo,&sp,&gn)) return 0;
    } else {
        snprintf(j,sizeof j,"{\"rx_gain_mode\":\"%s\"}", gmode==1?"SlowAttack":"FastAttack");
        if(!maia_patch_get(j,&lo,&sp,&gn)) return 0;
    }
    if(reconcile) *reconcile=(long)(gn+0.5);
    return 1;
}
static int ap_gain(pp_field_t*f){ return gain_push(f->val); }          /* GAIN edit */
static int ap_mode(pp_field_t*f){ (void)f; return gain_push(&gain_db); } /* MODE edit -> show live gain */
static int ap_bw(pp_field_t*f){                                        /* RX analog bandwidth */
    char j[80]; snprintf(j,sizeof j,"{\"rx_rf_bandwidth\":%lld}", (long long)*f->val*1000);
    return maia_patch(j);
}

/* ---- colorbar widget for MIN/MAX (reads cmin_db/cmax_db) ---- */
static void wg_colorbar(const pp_field_t*f,uint16_t*buf,int x,int y){
    (void)f; int cbw=52,cbh=9,cby=y+9; char s[16];
    for(int i=0;i<cbw;i++) pp_fill(buf,x+i,cby,1,cbh,PPWF_TURBO[i*255/(cbw-1)]);
    pp_rect(buf,x,cby,cbw,cbh,C_DIM);
    snprintf(s,sizeof s,"%ld-%ld",cmin_db,cmax_db); pp_text(buf,x+cbw+4,y+10,s,C_DIM,1);
}

/* ---- 2D waterfall marker (touch tap, or U/D/L/R in M CTRL mode) ---- */
static float dbhist[LT_WF_ROWS][PP_W];         /* dB history: row 0 oldest .. ROWS-1 newest  */
static long  mk_on=0, mk_ctrl=0;               /* on/off + 2D move-mode (DATA fields)          */
static int   mk_x=PP_W/2, mk_y=LT_WF_ROWS/2;   /* marker cell: freq pixel, waterfall row       */
static void fmt_onoff2(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%s",*f->val?"ON":"OFF"); }
static void mk_on_toggle (void){ mk_on^=1; }
static void mk_ctrl_enter(void){ mk_ctrl=1; mk_on=1; }   /* enter move mode (exit via CENTER/BACK) */

static pp_field_t fields[9] = {
    { "CENTER","MHz", &lo_khz,   LO_MIN,  LO_MAX,   PLACE_CTR,7,3, fmt_ctr,  ap_lo,   NULL,        0,0,1,0 },
    { "SPAN",  "MHz", &span_khz, SPAN_MIN,SPAN_MAX, PLACE_SPN,5,1, fmt_span, ap_span, NULL,        0,0,1,0 },
    { "GAIN",  "dB",  &gain_db,  GAIN_MIN,GAIN_MAX, PLACE_GN, 2,1, fmt_int,  ap_gain, NULL,        0,0,1,0 },
    { "GMODE", "",    &gmode,    0,       2,        PLACE_1,  1,0, fmt_mode, ap_mode, NULL,        0,0,1,0 },
    { "MIN",   "dB",  &cmin_db,  0,       DB_LIMIT, PLACE_DB, 2,1, fmt_int,  NULL,    wg_colorbar, 0,0,1,0 },
    { "MAX",   "dB",  &cmax_db,  0,       DB_LIMIT, PLACE_DB, 2,1, fmt_int,  NULL,    wg_colorbar, 0,0,1,0 },
    { "MARKER","",    &mk_on,    0,       1,        PLACE_1,  1,0, fmt_onoff2,NULL,    NULL,        0,0,1,0, mk_on_toggle,  0 },
    { "M CTRL","",    &mk_ctrl,  0,       1,        PLACE_1,  1,0, fmt_onoff2,NULL,    NULL,        0,0,1,0, mk_ctrl_enter, 0 },
    { "RXBW",  "MHz", &bw_khz,   200,     56000,    PLACE_SPN,5,1, fmt_span, ap_bw,   NULL,        0,0,1,0 },
};

/* ---- maia frame (4096 f32 linear power) -> dB[320] ---- */
static void convert_frame(const float *f, int nf, float *db){
    static float b[MAIA_FFT];
    b[0] = (f[1]>0.f)?10.f*log10f(f[1]):-120.f;
    for(int i=1;i<nf;i++) b[i] = (f[i]>0.f)?10.f*log10f(f[i]):-120.f;
    for(int x=0;x<PP_W;x++){
        int a=x*nf/PP_W, e=(x+1)*nf/PP_W; if(e<=a) e=a+1;
        float mx=-1e30f;
        for(int k=a;k<e;k++){ int idx=(k+nf/2)%nf; if(b[idx]>mx) mx=b[idx]; }
        db[x]=mx;
    }
}
static uint8_t as_idx(float d){
    float f=(d-(float)cmin_db)/((float)(cmax_db-cmin_db));
    if(f<0)f=0; if(f>1)f=1; return (uint8_t)(f*255.f+0.5f);
}
/* scroll the dB HISTORY up one row (newest at the bottom). The pixels are rebuilt
 * from this history each frame by wf_render, so the 2D marker overlay never smears. */
static void wf_push(const float *db){
    memmove(&dbhist[0][0], &dbhist[1][0], (size_t)(LT_WF_ROWS-1)*PP_W*sizeof(float));
    memcpy(&dbhist[LT_WF_ROWS-1][0], db, PP_W*sizeof(float));
}
static void wf_render(uint16_t *buf){
    for(int r=0;r<LT_WF_ROWS;r++){
        uint16_t *line=&buf[(LT_WF_Y0+r)*PP_W];
        for(int x=0;x<PP_W;x++) line[x]=PPWF_TURBO[as_idx(dbhist[r][x])];
        line[80]=C_DIM; line[240]=C_DIM; line[PP_W/2]=C_ACCENT;      /* freq grid + center */
    }
}
static void draw_marker2d(uint16_t *buf){
    if(!mk_on) return;
    int px=mk_x, ry=mk_y;
    if(px<0)px=0; if(px>=PP_W)px=PP_W-1;
    if(ry<0)ry=0; if(ry>=LT_WF_ROWS)ry=LT_WF_ROWS-1;
    int y0=LT_WF_Y0, y1=LT_WF_Y0+LT_WF_ROWS, sy=y0+ry;
    for(int y=y0;y<y1;y++) if((y&1)==0) buf[y*PP_W+px]=C_MARKER;    /* dashed vertical (freq) */
    for(int x=0;x<PP_W;x++) if((x&1)==0) buf[sy*PP_W+x]=C_MARKER;   /* dashed horizontal (row) */
    /* label: frequency at px + power (dBFS) from the history cell */
    long fl=lo_khz-span_khz/2, fk=fl+(long)((long long)px*span_khz/(PP_W>1?PP_W-1:1));
    char l1[16],l2[16];
    snprintf(l1,sizeof l1,"%ld.%03ld",fk/1000000,(fk/1000)%1000);
    snprintf(l2,sizeof l2,"%.0f dBFS",dbhist[ry][px]);
    int w1=pp_tw(l1,1),w2=pp_tw(l2,1),bw=(w1>w2?w1:w2)+6,bh=20;
    int bx=(px+6+bw<=PP_W)?px+6:px-6-bw; if(bx<0)bx=0;
    int by=(sy-6-bh>=y0)?sy-6-bh:sy+6; if(by+bh>y1)by=y1-bh; if(by<y0)by=y0;
    pp_fill(buf,bx,by,bw,bh,C_PANEL); pp_rect(buf,bx,by,bw,bh,C_MARKER);
    pp_text(buf,bx+3,by+2,l1,C_TEXT,1);
    pp_text(buf,bx+3,by+11,l2,C_MARKER,1);
}
static void draw_axis(uint16_t *buf){
    char s[16]; long fl=lo_khz-span_khz/2, fc=lo_khz, fr=lo_khz+span_khz/2;
    pp_fill(buf,0,LT_AXIS_Y-1,PP_W,11,C_BG);
    snprintf(s,sizeof s,"%ld.%03ld",fl/1000000,(fl/1000)%1000); pp_text(buf,2,LT_AXIS_Y,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld",fc/1000000,(fc/1000)%1000); pp_text(buf,(PP_W-pp_tw(s,1))/2,LT_AXIS_Y,s,C_ACCENT,1);
    snprintf(s,sizeof s,"%ld.%03ld",fr/1000000,(fr/1000)%1000); pp_text(buf,PP_W-pp_tw(s,1)-2,LT_AXIS_Y,s,C_DIM,1);
}

int main(int argc, char **argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if (pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    /* start from the radio's current state */
    long long lo_hz=0, span_hz=0; double g=0;
    if(maia_get_ad9361(&lo_hz,&span_hz,&g)){
        if(lo_hz>0)   lo_khz  = (long)(lo_hz/1000);
        if(span_hz>0) span_khz= (long)(span_hz/1000);
        if(span_khz<SPAN_MIN)span_khz=SPAN_MIN; if(span_khz>SPAN_MAX)span_khz=SPAN_MAX;
        gain_db = (long)(g+0.5); if(gain_db<GAIN_MIN)gain_db=GAIN_MIN; if(gain_db>GAIN_MAX)gain_db=GAIN_MAX;
    }
    /* Prime the maia pipeline like pp-spectrum does: assert sampling_frequency
     * (which reprograms the AD9361 and restarts the spectrometer) so /waterfall
     * streams a live spectrum immediately. Without this the waterfall stayed blank
     * until the span was changed or Spectrum had been opened/closed once — the
     * app only READ the radio state, it never asserted it. ap_span also sizes NI
     * for a constant frame rate and pulls rx_rf_bandwidth to match the span. */
    ap_span(&fields[1]);                                  /* fields[1] = SPAN */
    int wsfd = maia_ws_open();

    pp_ui_t ui; pp_ui_init(&ui, fields, 9); ui.ndata=8;   /* RXBW is settings-only; MARKER/M CTRL on DATA */
    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    float frame[MAIA_FFT], db[PP_W];
    int prev_screen=-1, resync=0;
    int prev_td=0, m_init=0, m_accum=0; int32_t m_lastenc=0; uint8_t m_lastbtn=0;   /* marker-mode input */

    memset(buf,0,sizeof buf);
    for(int r=0;r<LT_WF_ROWS;r++) for(int x=0;x<PP_W;x++) dbhist[r][x]=-120.f;   /* empty history */

    for(;;){
        int link_ok=0;
        if (have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if (link_ok){
            if(mk_ctrl){
                /* 2D marker move mode: own edge/encoder handling; CENTER or BACK exits.
                 * U/D move the waterfall row, L/R move frequency (coarse), wheel = fine. */
                if(!m_init){ m_lastenc=st.enc_pos; m_lastbtn=st.btn_bitmap; m_accum=0; m_init=1; }
                m_accum += (int)(st.enc_pos-m_lastenc); m_lastenc=st.enc_pos;
                int wheel=m_accum/4; m_accum-=wheel*4;
                uint8_t pr = st.btn_bitmap & ~m_lastbtn; m_lastbtn=st.btn_bitmap;
                mk_x += wheel;
                if(pr&PP_BTN_LEFT)  mk_x-=8;
                if(pr&PP_BTN_RIGHT) mk_x+=8;
                if(pr&PP_BTN_UP)    mk_y-=4;
                if(pr&PP_BTN_DOWN)  mk_y+=4;
                if(mk_x<0)mk_x=0; if(mk_x>=PP_W)mk_x=PP_W-1;
                if(mk_y<0)mk_y=0; if(mk_y>=LT_WF_ROWS)mk_y=LT_WF_ROWS-1;
                if(pr&(PP_BTN_CENTER|PP_BTN_7)){ mk_ctrl=0; m_init=0;   /* exit move mode */
                    ui.last_btn=st.btn_bitmap; ui.last_enc=st.enc_pos; ui.enc_accum=0; }  /* resync UI edges */
            }else{
                m_init=0;
                if (pp_ui_input(&ui,&st)==PP_UI_EXIT) return 0;   /* BTN7 on DATA -> switcher */
            }
            /* touch tap: drop the 2D marker anywhere on the waterfall (any mode) */
            if(st.touch_down && !prev_td){
                int sx,sy; pp_touch_map(st.touch_x,st.touch_y,&sx,&sy);
                if(sy>=LT_WF_Y0 && sy<LT_WF_Y0+LT_WF_ROWS && sx>=0 && sx<PP_W){
                    mk_x=sx; mk_y=sy-LT_WF_Y0; mk_on=1;
                }
            }
            prev_td=st.touch_down;
        }
        /* keep MIN < MAX for a sane colormap */
        if(cmax_db>DB_LIMIT)cmax_db=DB_LIMIT;
        if(cmin_db>=cmax_db){ if(ui.focus==5) cmin_db=cmax_db-1; else cmax_db=cmin_db+1; }  /* MAX now idx 5 */
        if(cmin_db<0)cmin_db=0; if(cmax_db>DB_LIMIT)cmax_db=DB_LIMIT;

        pp_ui_sync(&ui);                                     /* push settled changes */
        if(++resync>=RESYNC_LOOPS){                          /* periodic re-read */
            resync=0; long long lo=(long long)lo_khz*1000, sp=(long long)span_khz*1000; double gn=gain_db;
            if(maia_get_ad9361(&lo,&sp,&gn)){
                pp_ui_field_extern(&fields[0], (long)(lo/1000));
                pp_ui_field_extern(&fields[1], (long)(sp/1000));
                pp_ui_field_extern(&fields[2], (long)(gn+0.5));
            }
        }

        /* pull the newest spectrum frame regardless of screen (keep socket clear) */
        int got=0;
        if(wsfd>=0){ got=maia_ws_drain(wsfd,frame); if(got<0){ close(wsfd); wsfd=maia_ws_open(); got=0; } }
        else wsfd=maia_ws_open();

        if(ui.screen!=prev_screen){                          /* repaint dynamic area */
            pp_fill(buf,0,LT_BAND_Y0,PP_W,PP_H-LT_BAND_Y0,C_BG);
            memset(pp_prev,0xA5,sizeof pp_prev);
            prev_screen=ui.screen;
        }

        const char *note=NULL; uint16_t notecol=C_WARN;
        if(wsfd<0){ note="no maia"; notecol=C_BAD; }
        else if(!pp_ui_all_synced(&ui)){ note="sync.."; notecol=C_WARN; }
        pp_ui_status_bar(buf,NULL,&st,link_ok,note,notecol);    /* NULL -> portaPluto logo */

        if(ui.screen==PP_DATA){
            if(got>0){ convert_frame(frame,MAIA_FFT,db); wf_push(db); }
            wf_render(buf);                    /* repaint waterfall from history (marker can't smear) */
            draw_marker2d(buf);                /* 2D marker overlay */
            pp_ui_draw_field_band(buf,&fields[ui.focus],ui.editing,LT_BAND_Y0);
            draw_axis(buf);
        }else{
            pp_ui_draw_settings(buf,&ui,LT_BAND_Y0,PP_H-LT_BAND_Y0-14);
        }
        pp_ui_footer(buf,&ui);
        if(mk_ctrl){ pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);   /* marker-mode hint over the footer */
            pp_text(buf,4,PP_H-12,"MARKER: U/D/L-R move  wheel fine  CENTER/BACK exit",C_DIM,1); }
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
