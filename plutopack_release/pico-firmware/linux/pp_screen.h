/*
 * pp_screen.h — PlutoPack 320x240 status-screen renderer (portable, RGB565).
 *
 * The SAME code renders the live /dev/fb0 on the PlutoSky (target build) and
 * the pixel-accurate mockup PNGs on a host, so what you preview is exactly what
 * the panel shows. No dependencies beyond a framebuffer pointer.
 *
 * Data comes from the Pico over the Zynq<->Pico SPI link (the status frame in
 * plutopack_mcu.c) — i.e. the same gauge / encoder / button / touch fields the
 * demo firmware prints over USB serial.
 */
#ifndef PP_SCREEN_H
#define PP_SCREEN_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define PP_W 320
#define PP_H 240

/* status snapshot (decoded from the Pico SPI status frame) */
typedef struct {
    int      gauge_ok, touch_ok, charging;
    uint16_t batt_mV;
    int      soc;            /* %  */
    int      current_mA;     /* signed */
    int32_t  enc_pos;
    uint8_t  btn_bitmap;     /* bit(id-1): SW1,SW3,SW4,SW5,SW6,SW7,SW8 -> ids 1,3,4,5,6,7,8 */
    int      touch_down;
    uint16_t touch_x, touch_y, touch_z;   /* raw 12-bit */
    uint32_t seq;
} pp_status_t;

/* RGB888 -> RGB565 */
static inline uint16_t pp_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* palette */
#define C_BG      pp_rgb(0x0c,0x0e,0x14)
#define C_PANEL   pp_rgb(0x1a,0x1f,0x2b)
#define C_PANEL2  pp_rgb(0x12,0x16,0x20)
#define C_ACCENT  pp_rgb(0x36,0x9e,0xff)
#define C_TEXT    pp_rgb(0xe6,0xe9,0xf0)
#define C_DIM     pp_rgb(0x8a,0x93,0xa6)
#define C_GOOD    pp_rgb(0x36,0xd3,0x99)
#define C_WARN    pp_rgb(0xff,0xd1,0x3b)
#define C_BAD     pp_rgb(0xff,0x5c,0x5c)
#define C_ON      pp_rgb(0x36,0x9e,0xff)
#define C_OFF     pp_rgb(0x2c,0x33,0x42)

/* --- 8x8 font (ASCII 0x20..0x7f), classic 8x8 bitmap ---------------------- */
extern const uint8_t PP_FONT8[96][8];

static void pp_px(uint16_t *fb, int x, int y, uint16_t c) {
    if ((unsigned)x < PP_W && (unsigned)y < PP_H) fb[y*PP_W + x] = c;
}
static void pp_fill(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) pp_px(fb, x+i, y+j, c);
}
static void pp_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int i = 0; i < w; i++){ pp_px(fb,x+i,y,c); pp_px(fb,x+i,y+h-1,c);}
    for (int j = 0; j < h; j++){ pp_px(fb,x,y+j,c); pp_px(fb,x+w-1,y+j,c);}
}
/* draw char scaled by s */
static void pp_ch(uint16_t *fb, int x, int y, char ch, uint16_t c, int s) {
    if (ch < 0x20 || ch > 0x7f) ch = '?';
    const uint8_t *g = PP_FONT8[ch - 0x20];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col))
                pp_fill(fb, x+col*s, y+row*s, s, s, c);
}
static void pp_text(uint16_t *fb, int x, int y, const char *s, uint16_t c, int sc) {
    for (; *s; s++) { pp_ch(fb, x, y, *s, c, sc); x += 8*sc; }
}
static int pp_tw(const char *s, int sc){ return (int)strlen(s)*8*sc; }

/* horizontal battery bar with fill fraction 0..1 */
static void pp_bar(uint16_t *fb, int x, int y, int w, int h, float frac, uint16_t c) {
    pp_rect(fb, x, y, w, h, C_DIM);
    pp_fill(fb, x+w, y+h/2 - 3, 3, 6, C_DIM);           /* battery nub */
    int iw = (int)((w-4) * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
    pp_fill(fb, x+2, y+2, iw, h-4, c);
}

/* ============================ the screen ================================= */
static void pp_render(uint16_t *fb, const pp_status_t *st) {
    char line[48];
    pp_fill(fb, 0, 0, PP_W, PP_H, C_BG);

    /* title bar */
    pp_fill(fb, 0, 0, PP_W, 22, C_PANEL);
    pp_fill(fb, 0, 22, PP_W, 2, C_ACCENT);
    pp_text(fb, 6, 4, "PlutoPack", C_ACCENT, 2);
    /* link seq counter (proves live refresh) */
    snprintf(line, sizeof line, "SEQ%03u", (unsigned)(st->seq & 0xFFF));
    pp_text(fb, PP_W - pp_tw(line,1) - 6, 8, line, C_DIM, 1);

    /* ---- battery / gauge panel ---- */
    int bx=6, by=30, bw=200, bh=92;
    pp_fill(fb, bx, by, bw, bh, C_PANEL2);
    pp_rect(fb, bx, by, bw, bh, C_PANEL);
    pp_text(fb, bx+8, by+6, "GAS GAUGE", C_DIM, 1);

    uint16_t socc = st->soc < 15 ? C_BAD : st->soc < 40 ? C_WARN : C_GOOD;
    if (st->gauge_ok) {
        snprintf(line, sizeof line, "%d%%", st->soc);      /* big SoC, top-left */
        pp_text(fb, bx+8, by+18, line, socc, 3);
        if (st->charging) {                                 /* CHG badge */
            pp_fill(fb, bx+96, by+22, 34, 16, C_GOOD);
            pp_text(fb, bx+100, by+24, "CHG", C_BG, 1);
        }
        pp_bar(fb, bx+8, by+46, 184, 16, st->soc/100.0f, socc);
        snprintf(line, sizeof line, "%u mV", st->batt_mV);
        pp_text(fb, bx+8, by+70, line, C_TEXT, 1);
        snprintf(line, sizeof line, "%+d mA", st->current_mA);
        pp_text(fb, bx+96, by+70, line, st->current_mA < 0 ? C_WARN : C_GOOD, 1);
    } else {
        pp_text(fb, bx+8, by+30, "no I2C", C_BAD, 2);
        pp_text(fb, bx+8, by+56, "BQ27441 not responding", C_DIM, 1);
    }

    /* ---- encoder panel ---- */
    int ex=212, ey=30, ew=102, eh=92;
    pp_fill(fb, ex, ey, ew, eh, C_PANEL2);
    pp_rect(fb, ex, ey, ew, eh, C_PANEL);
    pp_text(fb, ex+8, ey+6, "ENCODER", C_DIM, 1);
    snprintf(line, sizeof line, "%ld", (long)st->enc_pos);
    pp_text(fb, ex + (ew - pp_tw(line,3))/2, ey+34, line, C_ACCENT, 3);
    pp_text(fb, ex+8, ey+eh-14, "position", C_DIM, 1);

    /* ---- buttons row ---- */
    int yb=132;
    pp_text(fb, 6, yb, "BUTTONS", C_DIM, 1);
    static const int ids[7] = {1,3,4,5,6,7,8};
    int cellw = 42, x0 = 6;
    for (int i = 0; i < 7; i++) {
        int on = (st->btn_bitmap >> (ids[i]-1)) & 1;
        int cx = x0 + i*cellw;
        pp_fill(fb, cx, yb+12, cellw-6, 30, on ? C_ON : C_OFF);
        pp_rect(fb, cx, yb+12, cellw-6, 30, on ? C_ACCENT : C_PANEL);
        snprintf(line, sizeof line, "%d", ids[i]);
        pp_text(fb, cx + (cellw-6 - pp_tw(line,2))/2, yb+20, line, on ? C_TEXT : C_DIM, 2);
    }

    /* ---- touch panel ---- */
    int tx=6, ty=188, tw=308, th=46;
    pp_fill(fb, tx, ty, tw, th, C_PANEL2);
    pp_rect(fb, tx, ty, tw, th, C_PANEL);
    pp_text(fb, tx+8, ty+6, "TOUCH", C_DIM, 1);
    pp_text(fb, tx+8, ty+22, st->touch_ok ? (st->touch_down?"DOWN":"up  ") : "n/a",
            st->touch_down ? C_GOOD : C_DIM, 2);
    if (st->touch_down) {
        snprintf(line, sizeof line, "X:%4u Y:%4u Z:%4u", st->touch_x, st->touch_y, st->touch_z);
        pp_text(fb, tx+92, ty+24, line, C_TEXT, 1);
        /* live crosshair mapped from 12-bit raw into a mini map on the right */
        int mx = tx + tw - 60, my = ty + 6, mw = 52, mh = th - 12;
        pp_rect(fb, mx, my, mw, mh, C_DIM);
        int px = mx + 2 + (st->touch_x * (mw-4))/4095;
        int py = my + 2 + (st->touch_y * (mh-4))/4095;
        pp_fill(fb, px-2, py, 5, 1, C_ACCENT);
        pp_fill(fb, px, py-2, 1, 5, C_ACCENT);
    } else {
        pp_text(fb, tx+70, ty+24, "(no contact)", C_DIM, 1);
    }
}
#endif
