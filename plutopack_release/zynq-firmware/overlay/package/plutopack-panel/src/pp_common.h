/*
 * pp_common.h — shared helpers for the PlutoPack demo/debug suite.
 *   - Pico SPI link (mode 1 / CPHA=1, the PL022-slave requirement) + status decode
 *   - framebuffer partial-refresh (diff row-bands; pace from PP_SPI_HZ env)
 * Kept header-only so each demo main stays small.
 */
#ifndef PP_COMMON_H
#define PP_COMMON_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/spi/spidev.h>
#include "pp_screen.h"

#define PP_FRAME_LEN 32
#define PP_M_MAGIC 0xA5
#define PP_S_MAGIC 0x5A
#define PP_CMD_NOP 0x00
#define PP_CMD_SET_GPIO 0x04   /* arg0 bit0=GPIO13, bit1=GPIO14 (see plutopack_mcu.c) */
#define PP_CMD_GET_GAUGE 0x05  /* request a gauge-detail frame as the next response */
#define PP_G_MAGIC 0x5B        /* gauge-detail frame magic */

/* live BQ27441 gauge registers, fetched from the Pico via PP_CMD_GET_GAUGE */
typedef struct {
    int      mv, ma;          /* Voltage (mV), AverageCurrent (mA, signed)      */
    int      temp_c10;        /* temperature in 0.1 C                            */
    int      soc;             /* StateOfCharge (%)                               */
    int      rm, fcc;         /* Remaining / FullCharge capacity (mAh)           */
    int      nac, fac;        /* Nominal / FullAvailable capacity (mAh)          */
    int      dcap, qmax;      /* DesignCapacity / learned Qmax (mAh)             */
    int      termv;           /* Terminate Voltage (mV)                          */
    uint16_t flags, ctrl, chem;/* Flags, CONTROL_STATUS, ChemID (raw)           */
} pp_gauge_t;

/* AD9361 die temperature in whole deg C for the header (shown next to battery on
 * every screen), or the last good value / -999 if unavailable. Cached; re-read
 * ~once/second by callers running at ~30 fps. ad9361-phy's IIO index (and its
 * in_temp0_input, in milli-deg C) is resolved once. Returns -999 in the web sim
 * (no /sys). Shared by the launcher header and pp_ui.h's app status bar. */
static int pp_ad9361_temp_c(void){
    static int cached=-999, tick=0, resolved=0; static char path[80]={0};
    if(tick-->0) return cached;
    tick=30;
    if(!resolved){
        resolved=1;
        for(int i=0;i<8;i++){
            char nm[80]; snprintf(nm,sizeof nm,"/sys/bus/iio/devices/iio:device%d/name",i);
            FILE *f=fopen(nm,"r"); if(!f) continue;
            char b[32]={0}; char *r=fgets(b,sizeof b,f); fclose(f); if(!r) continue;
            if(!strncmp(b,"ad9361-phy",10)){ snprintf(path,sizeof path,"/sys/bus/iio/devices/iio:device%d/in_temp0_input",i); break; }
        }
    }
    if(!path[0]) return cached;
    FILE *f=fopen(path,"r"); if(!f) return cached;
    int md; int ok=(fscanf(f,"%d",&md)==1); fclose(f);
    if(ok) cached = md/1000;
    return cached;
}

/* Zynq PS on-die temperature (application processor), whole deg C, for the header
 * next to the RFIC temp. The Zynq XADC IIO device ("xadc") exposes raw/offset/scale
 * (no in_temp0_input), so T[C] = (raw + offset) * scale / 1000. Cached, re-read
 * ~1/second like pp_ad9361_temp_c. Returns -999 if unavailable (web sim / XADC off). */
static int pp_zynq_temp_c(void){
    static int cached=-999, tick=0, resolved=0; static char base[80]={0};
    if(tick-->0) return cached;
    tick=30;
    if(!resolved){
        resolved=1;
        for(int i=0;i<12;i++){
            char nm[100]; snprintf(nm,sizeof nm,"/sys/bus/iio/devices/iio:device%d/name",i);
            FILE *f=fopen(nm,"r"); if(!f) continue;
            char b[32]={0}; char *r=fgets(b,sizeof b,f); fclose(f); if(!r) continue;
            if(!strncmp(b,"xadc",4) || strstr(b,"ams")){
                snprintf(base,sizeof base,"/sys/bus/iio/devices/iio:device%d",i); break; }
        }
    }
    if(!base[0]) return cached;
    char p[128]; double raw=0, off=0, scale=0; int hr=0, hs=0;
    snprintf(p,sizeof p,"%s/in_temp0_raw",base);    FILE*f=fopen(p,"r"); if(f){ hr=(fscanf(f,"%lf",&raw)==1); fclose(f); }
    snprintf(p,sizeof p,"%s/in_temp0_offset",base); f=fopen(p,"r"); if(f){ if(fscanf(f,"%lf",&off)!=1) off=0; fclose(f); }
    snprintf(p,sizeof p,"%s/in_temp0_scale",base);  f=fopen(p,"r"); if(f){ hs=(fscanf(f,"%lf",&scale)==1); fclose(f); }
    if(hr && hs) cached = (int)((raw+off)*scale/1000.0 + 0.5);
    return cached;
}

/* --------------------- shared exit-to-menu confirmation ------------------- *
 * A true overlay usable by any app (framework or not): draw it every frame with
 * `active` = your confirm flag; it stashes the pixels it covers the frame it
 * first appears and restores them the frame it is dismissed, so no app draw-code
 * cooperation is needed. The app owns the flag: raise it instead of exiting on
 * BACK, then CENTER = confirm (exit), BACK = cancel (clear the flag).            */
#define PPC_W 210
#define PPC_H 74
static void pp_confirm_overlay(uint16_t *buf, int active){
    static uint16_t save[PPC_W*PPC_H]; static int shown=0;
    int bx=(PP_W-PPC_W)/2, by=(PP_H-PPC_H)/2;
    if(active){
        if(!shown){
            for(int r=0;r<PPC_H;r++) memcpy(&save[r*PPC_W], &buf[(by+r)*PP_W+bx], PPC_W*sizeof(uint16_t));
            shown=1;
        }
        pp_fill(buf,bx,by,PPC_W,PPC_H,C_PANEL);
        pp_rect(buf,bx,by,PPC_W,PPC_H,C_ACCENT); pp_rect(buf,bx+1,by+1,PPC_W-2,PPC_H-2,C_ACCENT);
        const char *t="Exit to menu?";
        const char *a="CENTER  =  exit";
        const char *b="BACK  =  cancel";
        pp_text(buf, bx+(PPC_W-pp_tw(t,1))/2, by+14, t, C_TEXT, 1);
        pp_text(buf, bx+(PPC_W-pp_tw(a,1))/2, by+40, a, C_GOOD, 1);
        pp_text(buf, bx+(PPC_W-pp_tw(b,1))/2, by+56, b, C_WARN, 1);
    }else if(shown){
        for(int r=0;r<PPC_H;r++) memcpy(&buf[(by+r)*PP_W+bx], &save[r*PPC_W], PPC_W*sizeof(uint16_t));
        shown=0;
    }
}

/* Horizontal ADC-level bar: filled to rms%, a bright marker at peak%, and a red
 * tick at the ~90% clip-warning line. Colour goes green->amber->red as peak rises
 * so you can set RX gain without saturating the ADC. Values are % of full scale. */
static void pp_levelbar(uint16_t *buf, int x, int y, int w, int h, int rms_pct, int peak_pct){
    if(rms_pct<0)rms_pct=0; if(rms_pct>100)rms_pct=100;
    if(peak_pct<0)peak_pct=0; if(peak_pct>100)peak_pct=100;
    pp_fill(buf,x,y,w,h,C_PANEL2); pp_rect(buf,x,y,w,h,C_DIM);
    uint16_t c = peak_pct>=98?C_BAD:(peak_pct>=85?C_WARN:C_GOOD);
    pp_fill(buf,x+1,y+1,(w-2)*rms_pct/100,h-2,c);                 /* rms fill */
    int rz=x+1+(w-2)*90/100; pp_fill(buf,rz,y+1,1,h-2,C_BAD);     /* 90% clip line */
    int px=x+1+(w-2)*peak_pct/100; if(px>x+w-2)px=x+w-2;
    pp_fill(buf,px,y,1,h,C_TEXT);                                 /* peak marker */
}

/* RX ADC level for the status-bar headroom meter. Apps doing RX capture call
 * pp_rx_ol_note(ch, peak%FS) per block (ch=1 -> RX1, ch=2 -> RX2); the status bar
 * draws a small log-scale (dBFS) bar per channel, green->amber->red as the ADC
 * nears clipping, with RX2 greyed unless it's actually fed (only SCOPE, in 2R2T).
 * pp_rx_lvl_ttl[] keeps each bar visible between updates and fades it out when you
 * leave an RX app. peak%FS MUST be normalized to the TRUE converter full scale:
 * the cf-ad9361 libiio samples are 12-bit (~2048 raw, NOT 32768), so the capture
 * code derives full scale from the IIO data format (iq_adc_fs / g_adc_fs). */
static int pp_rx_lvl[2]={0,0}, pp_rx_lvl_ttl[2]={0,0};
static void pp_rx_ol_note(int ch, int peak_pct){
    int i=(ch==2)?1:0;
    if(peak_pct<0)peak_pct=0; if(peak_pct>100)peak_pct=100;
    pp_rx_lvl[i]=peak_pct; pp_rx_lvl_ttl[i]=30;            /* ~0.9 s hold @30 ms loop */
}

/* --------------------------- button bitmap map --------------------------- *
 * THE single place the physical->bit mapping lives. Confirmed at the board via
 * UITEST (physical button -> UITEST label -> status-frame bit; encoder is SW2 =
 * bit 1, delivered as enc_pos). There is no physical button 8.
 *   1 left / 2 center / 3 down / 4 up / 5 right / 6 back / 7 settings          */
#define PP_BTN_LEFT   0x01   /* bit0: UITEST 1 = left     */
#define PP_BTN_CENTER 0x04   /* bit2: UITEST 2 = center   */
#define PP_BTN_DOWN   0x08   /* bit3: UITEST 3 = down     */
#define PP_BTN_UP     0x10   /* bit4: UITEST 4 = up       */
#define PP_BTN_RIGHT  0x20   /* bit5: UITEST 5 = right    */
#define PP_BTN_7      0x40   /* bit6: UITEST 6 = back (layered-back button)     */
#define PP_BTN_8      0x80   /* bit7: UITEST 7 = settings                       */

/* ------------------------------- SPI link -------------------------------- */
static int      pp_spi_fd = -1;
static uint32_t pp_spi_hz = 1000000;

static uint8_t pp_crc8(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

#ifndef PP_SIM
/* open spidev in mode 1 (CPHA=1) — required by the RP2040 PL022 slave */
static int pp_spi_open(const char *dev) {
    pp_spi_fd = open(dev, O_RDWR);
    if (pp_spi_fd < 0) return -1;
    uint8_t mode = SPI_MODE_1, bits = 8;
    ioctl(pp_spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(pp_spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(pp_spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &pp_spi_hz);
    return pp_spi_fd;
}

/* exchange one 32-byte frame (master sends A5 NOP unless tx given) */
static int pp_xfer(const uint8_t *tx, uint8_t *rx) {
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof t);
    t.tx_buf = (unsigned long)tx; t.rx_buf = (unsigned long)rx;
    t.len = PP_FRAME_LEN; t.speed_hz = pp_spi_hz; t.bits_per_word = 8;
    return ioctl(pp_spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 ? -1 : 0;
}
static int pp_poll(uint8_t *rx) {
    uint8_t tx[PP_FRAME_LEN];
    memset(tx, 0, PP_FRAME_LEN);
    tx[0] = PP_M_MAGIC; tx[1] = PP_CMD_NOP;
    tx[PP_FRAME_LEN-1] = pp_crc8(tx, PP_FRAME_LEN-1);
    return pp_xfer(tx, rx);
}
/* send a command frame (e.g. PP_CMD_SET_GPIO) and read back the status frame.
 * IMPORTANT: after a command the Pico re-arms its SPI-slave DMA in the CS-rise
 * ISR; if the very next transaction starts before that completes, the frame
 * boundary slips by a couple of bytes and the link stays desynced (rx reads all
 * zeros) until the Pico is power-cycled. A poll immediately after a command
 * reliably triggers this. Space the next transaction out (the launcher's steady
 * ~30 ms NOP cadence never hits it); 30 ms was verified clean on hardware while
 * 0 ms desynced every time. */
static int pp_cmd(uint8_t cmd, uint8_t arg0, uint8_t *rx) {
    uint8_t tx[PP_FRAME_LEN];
    memset(tx, 0, PP_FRAME_LEN);
    tx[0] = PP_M_MAGIC; tx[1] = cmd; tx[2] = arg0;
    tx[PP_FRAME_LEN-1] = pp_crc8(tx, PP_FRAME_LEN-1);
    usleep(30000);                 /* space from the PRECEDING transaction (e.g. the poll */
                                   /* that read the button) so it isn't back-to-back      */
    int r = pp_xfer(tx, rx);
    usleep(30000);                 /* and let the Pico re-arm before the NEXT frame        */
    return r;
}
#else  /* PP_SIM: synthesize a Pico status frame from web-injected input */
extern void sim_input_get(uint8_t *btn, int32_t *enc);   /* provided by sim_main.c */
extern void sim_touch_get(uint8_t *down, int *rawx, int *rawy);  /* web-injected touch */
static uint8_t pp_sim_gpio=0;                            /* bit0=GPIO13, bit1=GPIO14 */
static int pp_spi_open(const char *dev){ (void)dev; pp_spi_fd=0; return 0; }
static int pp_poll(uint8_t *rx){
    static uint8_t seq;
    uint8_t btn=0; int32_t enc=0; sim_input_get(&btn,&enc);
    uint8_t td=0; int trx=0, try_=0; sim_touch_get(&td,&trx,&try_);
    memset(rx,0,PP_FRAME_LEN);
    rx[0]=PP_S_MAGIC; rx[1]=seq++;
    rx[2]=0x01|0x02                        /* gauge_ok | touch_ok */
         |(td?0:0x80)                       /* penirq: 1/hi=idle, cleared while touched */
         |((pp_sim_gpio&1)?0x20:0)|((pp_sim_gpio&2)?0x40:0);  /* echo GPIO13/14 */
    rx[3]=btn;
    rx[4]=enc&0xff; rx[5]=(enc>>8)&0xff; rx[6]=(enc>>16)&0xff; rx[7]=(enc>>24)&0xff;
    if(td){ rx[8]=1;                       /* touch_down + raw 12-bit coords + pressure z */
        rx[9]=trx&0xff; rx[10]=(trx>>8)&0xff; rx[11]=try_&0xff; rx[12]=(try_>>8)&0xff;
        rx[13]=400&0xff; rx[14]=(400>>8)&0xff; }
    rx[15]=(3900)&0xff; rx[16]=(3900)>>8;  /* batt_mV */
    rx[17]=76;                             /* soc %  */
    rx[18]=(120)&0xff; rx[19]=((120)>>8)&0xff;  /* current_mA */
    rx[21]=0;                              /* event count (>=0 => link_ok) */
    rx[PP_FRAME_LEN-1]=pp_crc8(rx,PP_FRAME_LEN-1);
    return 0;
}
static int pp_cmd(uint8_t cmd, uint8_t arg0, uint8_t *rx){
    if(cmd==PP_CMD_SET_GPIO) pp_sim_gpio=arg0&0x03;
    return pp_poll(rx);
}
#endif
/* decode a status frame into pp_status_t; returns queued event count or <0 */
static int pp_decode(const uint8_t *r, pp_status_t *s) {
    if (r[0] != PP_S_MAGIC) return -1;
    if (pp_crc8(r, PP_FRAME_LEN-1) != r[PP_FRAME_LEN-1]) return -2;
    uint8_t fl = r[2];
    s->gauge_ok = !!(fl & 0x01); s->touch_ok = !!(fl & 0x02); s->charging = !!(fl & 0x08);
    s->gpio_in = !!(fl & 0x10); s->gpio_o0 = !!(fl & 0x20); s->gpio_o1 = !!(fl & 0x40);
    s->penirq  = !!(fl & 0x80);
    s->btn_bitmap = r[3];
    s->enc_pos = (int32_t)((uint32_t)r[4] | (r[5]<<8) | (r[6]<<16) | ((uint32_t)r[7]<<24));
    s->touch_down = r[8];
    s->touch_x = r[9] | (r[10]<<8); s->touch_y = r[11] | (r[12]<<8); s->touch_z = r[13] | (r[14]<<8);
    s->batt_mV = r[15] | (r[16]<<8); s->soc = r[17];
    s->current_mA = (int16_t)(r[18] | (r[19]<<8));
    s->seq = r[1];
    return r[21];
}

/* decode a gauge-detail frame (0x5B) into pp_gauge_t; returns 1 on success */
static int pp_gauge_decode(const uint8_t *r, pp_gauge_t *g){
    if(r[0]!=PP_G_MAGIC) return 0;
    if(pp_crc8(r,PP_FRAME_LEN-1)!=r[PP_FRAME_LEN-1]) return 0;
    #define PPG16(o) ((uint16_t)((uint16_t)r[o] | ((uint16_t)r[(o)+1]<<8)))
    g->mv=PPG16(2); g->ma=(int16_t)PPG16(4); g->temp_c10=(int)PPG16(6)-2731; g->soc=PPG16(8);
    g->rm=PPG16(10); g->fcc=PPG16(12); g->nac=PPG16(14); g->fac=PPG16(16);
    g->dcap=PPG16(18); g->qmax=PPG16(20); g->termv=PPG16(22);
    g->flags=PPG16(24); g->ctrl=PPG16(26); g->chem=PPG16(28);
    #undef PPG16
    return 1;
}
/* fetch live gauge registers from the Pico: request a gauge frame, then read it back */
static int pp_get_gauge(pp_gauge_t *g){
#ifdef PP_SIM
    g->mv=3900; g->ma=-850; g->temp_c10=312; g->soc=76; g->rm=1980; g->fcc=2600;
    g->nac=1980; g->fac=2600; g->dcap=3000; g->qmax=2560; g->termv=3200;
    g->flags=0x018f; g->ctrl=0x228a; g->chem=0x0128; return 1;
#else
    uint8_t rx[PP_FRAME_LEN];
    pp_cmd(PP_CMD_GET_GAUGE, 0, rx);         /* arms the gauge frame for the next poll */
    if(pp_poll(rx)!=0) return 0;
    return pp_gauge_decode(rx, g);
#endif
}

/* ------------------------- touch calibration ----------------------------- *
 * The Pico delivers touch coordinates already rotated to the display
 * orientation; the TOUCH CAL app (pp-touchcal) fits an affine raw->pixel
 * transform from crosshair taps and writes the 6 coefficients to the file
 * below. Any app that maps a touch to a screen position calls pp_touch_map();
 * with no cal file it falls back to a plain 12-bit->screen linear scale so
 * touch is still usable. The file lives in tmpfs so it clears on reboot
 * (re-run TOUCH CAL after a power cycle). */
#define PP_TOUCHCAL_PATH "/tmp/pp_touchcal"
static float pp_tcal[6];
static int   pp_tcal_ok = 0, pp_tcal_tried = 0;
static void pp_touch_load(void){
    pp_tcal_tried = 1; pp_tcal_ok = 0;
    FILE *f = fopen(PP_TOUCHCAL_PATH, "r"); if(!f) return;
    float a[6]; int n = 0;
    for(; n < 6; n++) if(fscanf(f, "%f", &a[n]) != 1) break;
    fclose(f);
    if(n == 6){ for(int i=0;i<6;i++) pp_tcal[i]=a[i]; pp_tcal_ok = 1; }
}
/* map a raw touch sample to a screen pixel (clamped to the panel) */
static void pp_touch_map(int rawx, int rawy, int *sx, int *sy){
    if(!pp_tcal_tried) pp_touch_load();
    float fx, fy;
    if(pp_tcal_ok){
        fx = pp_tcal[0]*rawx + pp_tcal[1]*rawy + pp_tcal[2];
        fy = pp_tcal[3]*rawx + pp_tcal[4]*rawy + pp_tcal[5];
    } else {
        fx = rawx * (float)PP_W / 4096.0f;
        fy = rawy * (float)PP_H / 4096.0f;
    }
    int ix = (int)(fx + 0.5f), iy = (int)(fy + 0.5f);
    if(ix < 0) ix = 0; if(ix > PP_W-1) ix = PP_W-1;
    if(iy < 0) iy = 0; if(iy > PP_H-1) iy = PP_H-1;
    *sx = ix; *sy = iy;
}

/* --------------------------- framebuffer I/O ----------------------------- */
static int      pp_fb = -1, pp_stride = PP_W, pp_ll = PP_W*2;
static uint16_t pp_prev[PP_W*PP_H];
static unsigned long pp_disp_hz = 250000;    /* for refresh pacing */

#ifdef PP_SIM
extern void sim_fb_write(const uint16_t *buf, int y0, int y1);   /* provided by sim_main.c */
static int pp_fb_open(const char *dev){ (void)dev; pp_disp_hz=100000000UL;   /* no pacing in sim */
    memset(pp_prev,0xA5,sizeof pp_prev); return (pp_fb=0); }
static void pp_blit(const uint16_t *buf, int y0, int y1){
    if(y0<0)y0=0; if(y1>PP_H)y1=PP_H; if(y1<=y0)return; sim_fb_write(buf,y0,y1);
}
#else
static int pp_fb_open(const char *dev) {
    pp_fb = open(dev, O_RDWR);
    if (pp_fb < 0) return -1;
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(pp_fb, FBIOGET_VSCREENINFO, &v);
    ioctl(pp_fb, FBIOGET_FSCREENINFO, &f);
    pp_ll = f.line_length; pp_stride = pp_ll / 2;
    /* pace refresh to the ACTUAL display SPI clock. Priority: PP_SPI_HZ env,
     * then the device tree's ili9341 spi-max-frequency (authoritative — matches
     * what fbtft clocks, needs no marker file, so a fresh flash paces right),
     * then the legacy /boot/pp_spi_hz marker, else the 250 kHz default. */
    unsigned long h = 0;
    const char *e = getenv("PP_SPI_HZ");
    if (e) h = strtoul(e, 0, 0);
    if (h < 50000) {                 /* device tree: big-endian u32 */
        FILE *fp = fopen("/proc/device-tree/amba/spi@e0007000/display@0/spi-max-frequency", "rb");
        if (fp) { unsigned char b[4];
                  if (fread(b,1,4,fp)==4) h = ((unsigned long)b[0]<<24)|((unsigned long)b[1]<<16)|((unsigned long)b[2]<<8)|b[3];
                  fclose(fp); }
    }
    if (h < 50000) {                 /* legacy marker */
        FILE *fp = fopen("/boot/pp_spi_hz", "r");
        if (fp) { if (fscanf(fp, "%lu", &h) != 1) h = 0; fclose(fp); }
    }
    if (h >= 50000) pp_disp_hz = h;
    memset(pp_prev, 0xA5, sizeof pp_prev);   /* force first frame to fully paint */
    return pp_fb;
}
/* This ILI9341 panel is wired BGR: the fbtft MADCTL has R/B swapped vs the
 * RGB565 memory format. Compensate on the framebuffer write (keep pp_rgb as true
 * RGB565 so the simulator stays correct). Swaps the 5-bit R and B, keeps G. */
static uint16_t pp_swapbuf[PP_W*PP_H];
static void pp_blit(const uint16_t *buf, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > PP_H) y1 = PP_H;
    if (y1 <= y0) return;
    for (size_t i = (size_t)y0*PP_W; i < (size_t)y1*PP_W; i++) {
        uint16_t p = buf[i];
        pp_swapbuf[i] = (uint16_t)(((p & 0x001f) << 11) | (p & 0x07e0) | ((p >> 11) & 0x001f));
    }
    if (pp_stride == PP_W)
        pwrite(pp_fb, pp_swapbuf + (size_t)y0*PP_W, (size_t)(y1-y0)*PP_W*2, (off_t)y0*pp_ll);
    else
        for (int y = y0; y < y1; y++)
            pwrite(pp_fb, pp_swapbuf + (size_t)y*PP_W, (size_t)PP_W*2, (off_t)y*pp_ll);
}
#endif  /* PP_SIM */
/* wait for deferred-IO to flush a band of `rows` at the current display clock */
static void pp_wait(int rows) {
#ifdef PP_SIM
    (void)rows; return;                     /* no display bus to wait on in sim */
#else
    double us = (double)rows * PP_W * 2 * 8 / pp_disp_hz * 1e6 + 40000;
    if (us > 6e6) us = 6e6;
    usleep((useconds_t)us);
#endif
}
#define PP_GAP 4
static int pp_refresh(const uint16_t *buf) {   /* push only changed row-bands */
    int y = 0, flushed = 0;
    while (y < PP_H) {
        if (!memcmp(buf + (size_t)y*PP_W, pp_prev + (size_t)y*PP_W, PP_W*2)) { y++; continue; }
        int y0 = y, run_end = y + 1, look = y + 1;
        while (look < PP_H) {
            if (memcmp(buf + (size_t)look*PP_W, pp_prev + (size_t)look*PP_W, PP_W*2))
                run_end = look + 1;
            else if (look - run_end >= PP_GAP) break;
            look++;
        }
        int y1 = run_end;
        pp_blit(buf, y0, y1);
        memcpy(pp_prev + (size_t)y0*PP_W, buf + (size_t)y0*PP_W, (size_t)(y1-y0)*PP_W*2);
        pp_wait(y1 - y0);
        flushed += (y1 - y0);
        y = y1;
    }
    return flushed;
}
static void pp_full(const uint16_t *buf) {     /* full frame */
    pp_blit(buf, 0, PP_H);
    memcpy(pp_prev, buf, sizeof pp_prev);
    pp_wait(PP_H);
}
#endif
