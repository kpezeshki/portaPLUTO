/*
 * pp-iqrec — I/Q record & playback over raw libiio (no maia).
 *   DATA screen : lower ~2/3 is a live waterfall (own CPU FFT of the RX I/Q) so
 *                 you can narrow in on a band; above it one adjustable field.
 *                 Fields cycle CENTER -> SPAN -> GAIN -> REC -> REVIEW -> TX.
 *     CENTER/SPAN/GAIN : tune the AD9361 (applied + read-back, amber until synced)
 *     REC     : CENTER starts/stops recording raw cs16 to IQ_FILE (RAM), capped
 *     REVIEW  : replay the recorded file into the waterfall (no RF)
 *     TX      : retransmit the recorded file over the air (AD9361 TX)  *** emits RF ***
 *   SETTINGS (BTN8): RXBW, colormap MIN/MAX, record cap MAXMB, TX attenuation.
 *   BTN7 : layered back; during REC/REVIEW/TX it stops the operation.
 *
 * File: interleaved int16 I/Q (cs16) at IQ_FILE plus a JSON sidecar (LO / sample
 * rate / gain / sample count). The root FS is a RAM disk, so recordings vanish on
 * reboot and length is bounded by MAXMB. See pp_iq.h / pp_fft.h and the UI spec.
 *
 * usage: pp-iqrec [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <math.h>
#include "pp_ui.h"
#include "pp_iq.h"
#include "pp_fft.h"
#include "pp_waterfall.h"        /* PPWF_TURBO[] */

#define IQ_FILE "/tmp/pp_iq.cs16"
#define IQ_META "/tmp/pp_iq.json"

/* ---- capture sizes ---- */
#define N_FFT   4096             /* live/review transform size */
#define N_TUNE  4096             /* RX buffer while tuning */
#define N_REC   16384            /* RX buffer while recording (fewer refills) */
#define N_TX    32768            /* TX DMA buffer while playing back */

/* ---- layout ---- */
#define LT_BAND_Y0 20
#define LT_INFO_Y  49
#define LT_AXIS_Y  62
#define LT_WF_Y0   74
#define LT_WF_ROWS 150           /* 74 .. 224, above the footer */

enum { M_TUNE=0, M_REC, M_REVIEW, M_TX };
static int mode = M_TUNE;

/* ---- tuner values + digit places ---- */
static long lo_khz   = 433920;  static const long PLACE_CTR[7]={1000000,100000,10000,1000,100,10,1};
static long span_khz = 20000;   static const long PLACE_SPN[5]={10000,1000,100,10,1};
static long gain_db  = 40;      static const long PLACE_GN[2] ={10,1};
static long cmin_db  = -10;     static const long PLACE_DB[3] ={100,10,1};
static long cmax_db  = 60;
static long bw_khz   = 18000;
static long maxmb    = 64;       static const long PLACE_MB[3] ={100,10,1};
static long txatt_db = 10;       static const long PLACE_AT[2] ={10,1};
static long dummy    = 0;        static const long PLACE_1[1] ={1};

#define LO_MIN   70000L
#define LO_MAX   6000000L
#define SPAN_MIN 2084L           /* AD9361 min sampling_frequency ~2.083 MHz */
#define SPAN_MAX 61440L
#define GAIN_MIN 0L
#define GAIN_MAX 71L
#define DB_LO   (-60L)
#define DB_HI   120L

/* ---- record bookkeeping (params captured at record time, reused by TX) ---- */
static long long rec_lo=0, rec_fs=0; static long rec_gain=0;
static int   have_file=0, have_tx=0, rx_open=0;
static unsigned long long file_bytes=0;

/* ---- value formatters ---- */
static void fmt_ctr (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%04ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_span(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%02ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int (const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
/* REC field shows the est. record time at the current rate; TX/REVIEW show file size */
static void fmt_rec(const pp_field_t*f,char*v,int n){ (void)f;
    double secs=(double)maxmb*1024*1024/((double)span_khz*1000.0*4.0);
    snprintf(v,n,"cap %.1fs",secs); }
static void fmt_fileinfo(const pp_field_t*f,char*v,int n){ (void)f;
    if(have_file) snprintf(v,n,"%.1fMB",(double)file_bytes/(1024*1024));
    else          snprintf(v,n,"none"); }

/* ---- apply callbacks (libiio; read back the quantized value) ---- */
static int ap_lo(pp_field_t*f){ if(!iq_set_rxlo((long long)*f->val*1000)) return 0;
    long long r; if(iq_get_rxlo(&r)&&r>0) *f->val=(long)(r/1000); return 1; }
static int ap_span(pp_field_t*f){ if(!iq_set_fs((long)((long long)*f->val*1000))) return 0;
    long long r; if(iq_get_fs(&r)&&r>0) *f->val=(long)(r/1000); return 1; }
static int ap_gain(pp_field_t*f){ if(!iq_set_rxgain((double)*f->val)) return 0;
    double r; if(iq_get_rxgain(&r)) *f->val=(long)(r+0.5); return 1; }
static int ap_rxbw(pp_field_t*f){ return iq_set_rxbw((long)((long long)*f->val*1000)); }

/* ---- action callbacks: arm a mode; the main loop runs the operation ---- */
static void act_rec(void){ mode=M_REC; }
static void act_review(void){ if(have_file) mode=M_REVIEW; }
static void act_tx(void){ if(have_file && have_tx) mode=M_TX; }

/* ---- colorbar widget for MIN/MAX ---- */
static void wg_colorbar(const pp_field_t*f,uint16_t*buf,int x,int y){
    (void)f; int cbw=52,cbh=9,cby=y+9; char s[16];
    for(int i=0;i<cbw;i++) pp_fill(buf,x+i,cby,1,cbh,PPWF_TURBO[i*255/(cbw-1)]);
    pp_rect(buf,x,cby,cbw,cbh,C_DIM);
    snprintf(s,sizeof s,"%ld/%ld",cmin_db,cmax_db); pp_text(buf,x+cbw+4,y+10,s,C_DIM,1);
}

static pp_field_t fields[11] = {
  { "CENTER","MHz", &lo_khz,   LO_MIN,  LO_MAX,   PLACE_CTR,7,3, fmt_ctr,     ap_lo,   NULL,        0,0,1,0, NULL },
  { "SPAN",  "MHz", &span_khz, SPAN_MIN,SPAN_MAX, PLACE_SPN,5,1, fmt_span,    ap_span, NULL,        0,0,1,0, NULL },
  { "GAIN",  "dB",  &gain_db,  GAIN_MIN,GAIN_MAX, PLACE_GN, 2,1, fmt_int,     ap_gain, NULL,        0,0,1,0, NULL },
  { "REC",   "",    &dummy,    0,       0,        PLACE_1,  1,0, fmt_rec,     NULL,    NULL,        0,0,1,0, act_rec },
  { "REVIEW","",    &dummy,    0,       0,        PLACE_1,  1,0, fmt_fileinfo,NULL,    NULL,        0,0,1,0, act_review },
  { "TX PLAY","",   &dummy,    0,       0,        PLACE_1,  1,0, fmt_fileinfo,NULL,    NULL,        0,0,1,0, act_tx },
  { "RXBW",  "MHz", &bw_khz,   200,     56000,    PLACE_SPN,5,1, fmt_span,    ap_rxbw, NULL,        0,0,1,0, NULL },
  { "MIN",   "dB",  &cmin_db,  DB_LO,   DB_HI,    PLACE_DB, 3,2, fmt_int,     NULL,    wg_colorbar, 0,0,1,0, NULL },
  { "MAX",   "dB",  &cmax_db,  DB_LO,   DB_HI,    PLACE_DB, 3,2, fmt_int,     NULL,    wg_colorbar, 0,0,1,0, NULL },
  { "MAXMB", "MB",  &maxmb,    4,       256,      PLACE_MB, 3,2, fmt_int,     NULL,    NULL,        0,0,1,0, NULL },
  { "TXATTN","dB",  &txatt_db, 0,       89,       PLACE_AT, 2,1, fmt_int,     NULL,    NULL,        0,0,1,0, NULL },
};
#define NFIELDS 11
#define NDATA   6                 /* CENTER..TX on DATA; the rest are settings-only */

/* ---- DC-centred dB spectrum (N_FFT bins) -> 320 columns (max-downsample) ---- */
static void spec_to_cols(const float *db, float *col){
    for(int x=0;x<PP_W;x++){
        int a=x*N_FFT/PP_W, e=(x+1)*N_FFT/PP_W; if(e<=a)e=a+1;
        float mx=-1e30f; for(int k=a;k<e;k++) if(db[k]>mx) mx=db[k];
        col[x]=mx;
    }
}
static uint8_t as_idx(float d){
    float f=(d-(float)cmin_db)/((float)(cmax_db-cmin_db));
    if(f<0)f=0; if(f>1)f=1; return (uint8_t)(f*255.f+0.5f);
}
static void wf_push(uint16_t *buf, const float *col){
    memmove(&buf[LT_WF_Y0*PP_W], &buf[(LT_WF_Y0+1)*PP_W],
            (size_t)(LT_WF_ROWS-1)*PP_W*sizeof(uint16_t));
    uint16_t *line=&buf[(LT_WF_Y0+LT_WF_ROWS-1)*PP_W];
    for(int x=0;x<PP_W;x++) line[x]=PPWF_TURBO[as_idx(col[x])];
    line[PP_W/2]=C_ACCENT;                             /* centre (LO) marker */
}
/* one RX block (interleaved int16, N_FFT samples) -> waterfall row */
static void frame_to_wf(uint16_t *buf, const int16_t *iq){
    static float db[N_FFT], col[PP_W];
    pp_iq_spectrum(iq, N_FFT, db); spec_to_cols(db, col); wf_push(buf, col);
}

static void draw_axis(uint16_t *buf){
    char s[16]; long fl=lo_khz-span_khz/2, fc=lo_khz, fr=lo_khz+span_khz/2;
    pp_fill(buf,0,LT_AXIS_Y-1,PP_W,11,C_BG);
    snprintf(s,sizeof s,"%ld.%03ld",fl/1000,fl%1000); pp_text(buf,2,LT_AXIS_Y,s,C_DIM,1);
    snprintf(s,sizeof s,"%ld.%03ld",fc/1000,fc%1000); pp_text(buf,(PP_W-pp_tw(s,1))/2,LT_AXIS_Y,s,C_ACCENT,1);
    snprintf(s,sizeof s,"%ld.%03ld",fr/1000,fr%1000); pp_text(buf,PP_W-pp_tw(s,1)-2,LT_AXIS_Y,s,C_DIM,1);
}
/* info line under the field band: mode + file/HUD text */
static void draw_info(uint16_t *buf, const char *txt, uint16_t col){
    pp_fill(buf,0,LT_INFO_Y,PP_W,12,C_BG);
    if(txt) pp_text(buf,4,LT_INFO_Y,txt,col,1);
}

/* poll one SPI frame; return newly-pressed buttons (0 if no link). Uses a static
 * baseline so the CENTER that launched an op does not immediately stop it. */
static uint8_t poll_pressed(pp_status_t *st){
    static uint8_t last=0xff; uint8_t rx[PP_FRAME_LEN];
    if(pp_spi_fd<0 || pp_poll(rx)!=0 || pp_decode(rx,st)<0) return 0;
    uint8_t pr=st->btn_bitmap & ~last; last=st->btn_bitmap; return pr;
}
static void reset_poll_baseline(void){ pp_status_t st; poll_pressed(&st); }  /* swallow current state */

static void write_sidecar(unsigned long long nsamp){
    FILE *j=fopen(IQ_META,"w"); if(!j) return;
    fprintf(j,"{\"format\":\"cs16\",\"lo_hz\":%lld,\"sample_rate_hz\":%lld,"
              "\"gain_db\":%ld,\"n_samples\":%llu}\n",
            rec_lo, rec_fs, rec_gain, nsamp);
    fclose(j);
}

/* ============================ operations ============================ */
/* RECORD: tight capture loop (self-paced by iio_buffer_refill), tee to file. */
static void do_record(uint16_t *buf, pp_status_t *st){
    static int16_t blk[N_REC*2];
    rec_lo=(long long)lo_khz*1000; rec_fs=(long long)span_khz*1000; rec_gain=gain_db;
    FILE *f=fopen(IQ_FILE,"wb");
    if(!f){ draw_info(buf,"REC: cannot open file",C_BAD); pp_refresh(buf); mode=M_TUNE; return; }
    if(rx_open){ iq_rx_close(); rx_open=0; }
    if(!iq_rx_open(N_REC)){ fclose(f); draw_info(buf,"REC: RX busy",C_BAD); pp_refresh(buf);
                            iq_rx_open(N_TUNE); rx_open=1; mode=M_TUNE; return; }
    unsigned long long cap=(unsigned long long)maxmb*1024*1024, tot=0, nsamp=0;
    int stop=0, bc=0; reset_poll_baseline();
    while(!stop){
        int n=iq_rx_read(blk); if(n<=0) continue;
        fwrite(blk,4,(size_t)n,f); tot+=(unsigned long long)n*4; nsamp+=n;
        if(tot>=cap) stop=1;
        if(++bc>=6 || stop){ bc=0;
            uint8_t pr=poll_pressed(st); if(pr&(PP_BTN_7|PP_BTN_CENTER)) stop=1;
            frame_to_wf(buf,blk);
            char s[48]; snprintf(s,sizeof s,"REC  %.1fs  %.1fMB%s",
                (double)nsamp/((double)rec_fs>0?(double)rec_fs:1),
                (double)tot/(1024*1024), stop?"  DONE":"");
            pp_ui_status_bar(buf,NULL,st,1,"REC",C_BAD);
            draw_info(buf,s,C_BAD); pp_refresh(buf);
        }
#ifdef PP_SIM
        usleep(4000);                              /* sim RX is instant; pace it */
#endif
    }
    fclose(f); file_bytes=tot; have_file=1; write_sidecar(nsamp);
    iq_rx_close(); iq_rx_open(N_TUNE); rx_open=1;
    mode=M_TUNE;
}

/* REVIEW: replay the file into the waterfall only (no RF). */
static void do_review(uint16_t *buf, pp_status_t *st){
    static int16_t blk[N_FFT*2];
    FILE *f=fopen(IQ_FILE,"rb");
    if(!f){ draw_info(buf,"REVIEW: no file",C_BAD); pp_refresh(buf); mode=M_TUNE; return; }
    unsigned long long done=0; int stop=0; reset_poll_baseline();
    while(!stop){
        size_t n=fread(blk,4,N_FFT,f); if(n<(size_t)N_FFT){ stop=1; if(n==0) break; }
        done+=n*4;
        frame_to_wf(buf,blk);
        char s[48]; snprintf(s,sizeof s,"REVIEW  %.0f%%",
            file_bytes? 100.0*(double)done/(double)file_bytes : 0.0);
        pp_ui_status_bar(buf,NULL,st,1,"REVIEW",C_GOOD);
        draw_info(buf,s,C_GOOD); pp_refresh(buf);
        uint8_t pr=poll_pressed(st); if(pr&(PP_BTN_7|PP_BTN_CENTER)) stop=1;
        usleep(25000);                             /* watchable scroll rate */
    }
    fclose(f); mode=M_TUNE;
}

/* TX: retransmit the file over the air (self-paced by DMA back-pressure). */
static void do_tx(uint16_t *buf, pp_status_t *st){
    static int16_t blk[N_TX*2];
    FILE *f=fopen(IQ_FILE,"rb");
    if(!f){ draw_info(buf,"TX: no file",C_BAD); pp_refresh(buf); mode=M_TUNE; return; }
    if(rx_open){ iq_rx_close(); rx_open=0; }
    iq_set_txlo(rec_lo); iq_set_fs((long)rec_fs);
    iq_set_txbw((long)((long long)bw_khz*1000)); iq_set_txatten((double)txatt_db);
    if(!iq_tx_open(N_TX)){ fclose(f); draw_info(buf,"TX: DAC busy",C_BAD); pp_refresh(buf);
                           iq_rx_open(N_TUNE); rx_open=1; mode=M_TUNE; return; }
    unsigned long long done=0; int stop=0, bc=0; reset_poll_baseline();
    while(!stop){
        size_t n=fread(blk,4,N_TX,f);
        if(n==0){ stop=1; break; }                 /* single-shot playback */
        iq_tx_write(blk,(int)n); done+=n*4;
        if(++bc>=2 || n<(size_t)N_TX){ bc=0;
            uint8_t pr=poll_pressed(st); if(pr&(PP_BTN_7|PP_BTN_CENTER)) stop=1;
            char s[56]; snprintf(s,sizeof s,"TX %ld.%03ld MHz  %.0f%%",
                (long)(rec_lo/1000000),(long)((rec_lo/1000)%1000),
                file_bytes?100.0*(double)done/(double)file_bytes:0.0);
            pp_ui_status_bar(buf,NULL,st,1,"TX-RF",C_WARN);
            draw_info(buf,s,C_WARN); pp_refresh(buf);
        }
    }
    fclose(f); iq_tx_close();
    iq_set_txatten(89.0);                            /* park TX at max attenuation */
    iq_rx_open(N_TUNE); rx_open=1; mode=M_TUNE;
}

int main(int argc, char **argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    int radio = iq_init(&have_tx);
    if(radio){
        iq_set_rxlo((long long)lo_khz*1000);
        iq_set_fs((long)((long long)span_khz*1000));
        iq_set_rxbw((long)((long long)bw_khz*1000));
        iq_set_rxgain((double)gain_db);
        long long r; if(iq_get_fs(&r)&&r>0){ span_khz=(long)(r/1000);
            if(span_khz<SPAN_MIN)span_khz=SPAN_MIN; if(span_khz>SPAN_MAX)span_khz=SPAN_MAX; }
        rx_open = iq_rx_open(N_TUNE);
    }

    pp_ui_t ui; pp_ui_init(&ui, fields, NFIELDS); ui.ndata=NDATA;
    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    static int16_t tune[N_TUNE*2];
    int prev_screen=-1;

    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) break; }

        /* an action field may have armed an operation -> run it, then resume */
        if(mode!=M_TUNE){
            if     (mode==M_REC)    do_record(buf,&st);
            else if(mode==M_REVIEW) do_review(buf,&st);
            else if(mode==M_TX)     do_tx(buf,&st);
            prev_screen=-1;                        /* force full repaint on return */
            memset(pp_prev,0xA5,sizeof pp_prev);
            continue;
        }

        /* keep MIN < MAX */
        if(cmin_db>=cmax_db){ if(ui.focus==8) cmin_db=cmax_db-1; else cmax_db=cmin_db+1; }
        pp_ui_sync(&ui);

        if(ui.screen!=prev_screen){
            pp_fill(buf,0,LT_BAND_Y0,PP_W,PP_H-LT_BAND_Y0,C_BG);
            memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen;
        }

        const char *note=NULL; uint16_t notecol=C_WARN;
        if(!radio){ note="no radio"; notecol=C_BAD; }
        else if(!rx_open){ note="RX busy"; notecol=C_BAD; }
        else if(!pp_ui_all_synced(&ui)){ note="sync.."; notecol=C_WARN; }
        pp_ui_status_bar(buf,NULL,&st,link_ok,note,notecol);

        if(ui.screen==PP_DATA){
            if(rx_open){ int n=iq_rx_read(tune); if(n>0){
                int pk=0; for(int i=0;i<2*n;i++){ int v=tune[i]<0?-tune[i]:tune[i]; if(v>pk)pk=v; }
                pp_rx_ol_note(1, pk*100/iq_adc_fs);      /* ADC level -> status-bar meter */
                frame_to_wf(buf,tune); } }
            pp_ui_draw_field_band(buf,&fields[ui.focus],ui.editing,LT_BAND_Y0);
            char inf[48];
            if(have_file) snprintf(inf,sizeof inf,"file %.1fMB @ %lld.%03lld MHz",
                (double)file_bytes/(1024*1024), rec_lo/1000000,(rec_lo/1000)%1000);
            else          snprintf(inf,sizeof inf,"%s","no recording yet");
            draw_info(buf,inf, have_file?C_GOOD:C_DIM);
            draw_axis(buf);
        }else{
            pp_ui_draw_settings(buf,&ui,LT_BAND_Y0,PP_H-LT_BAND_Y0-14);
        }
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    if(rx_open) iq_rx_close();
    return 0;
}
