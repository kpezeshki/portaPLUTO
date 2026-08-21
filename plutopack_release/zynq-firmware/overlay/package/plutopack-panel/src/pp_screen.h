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
    uint16_t touch_x, touch_y, touch_z;   /* raw 12-bit (Pico already 90-rotated) */
    int      gpio_in;                     /* GPIO15 digital input level  */
    int      gpio_o0, gpio_o1;            /* GPIO13/14 output levels (echoed) */
    int      penirq;                      /* raw PENIRQ line level (1=high/idle, 0=asserted) */
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

/* italic (right-sheared) glyphs, for the "Pluto" half of the logo */
static void pp_chi(uint16_t *fb, int x, int y, char ch, uint16_t c, int s) {
    if (ch < 0x20 || ch > 0x7f) ch = '?';
    const uint8_t *g = PP_FONT8[ch - 0x20];
    for (int row = 0; row < 8; row++) { int sh = ((7-row)*s)/3;   /* top rows lean right */
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col)) pp_fill(fb, x+col*s+sh, y+row*s, s, s, c);
    }
}
static void pp_texti(uint16_t *fb, int x, int y, const char *s, uint16_t c, int sc) {
    for (; *s; s++) { pp_chi(fb, x, y, *s, c, sc); x += 8*sc; }
}
/* product logo: "porta" upright + "PLUTO" italic. returns the end x */
static int pp_logo(uint16_t *fb, int x, int y, uint16_t c, int sc) {
    pp_text (fb, x, y, "porta", c, sc); x += 5*8*sc;
    pp_texti(fb, x, y, "PLUTO", c, sc); x += 5*8*sc;
    return x;
}

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
    pp_logo(fb, 6, 4, C_ACCENT, 2);
    /* link seq counter (proves live refresh) */
    snprintf(line, sizeof line, "SEQ%03u", (unsigned)(st->seq & 0xFFF));
    pp_text(fb, PP_W - pp_tw(line,1) - 6, 8, line, C_DIM, 1);

    /* ---- battery / gauge panel ---- */
    int bx=6, by=30, bw=200, bh=92;
    pp_fill(fb, bx, by, bw, bh, C_PANEL2);
    pp_rect(fb, bx, by, bw, bh, C_PANEL);
    pp_text(fb, bx+8, by+6, "GAS GAUGE", C_DIM, 1);
    pp_text(fb, bx+8+pp_tw("GAS GAUGE",1)+8, by+6, "(center)", C_ACCENT, 1);  /* CENTER -> gauge detail */

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
    /* 7 push buttons; SW2 is the encoder (not a button) so the bits skip bit1.
     * Labels are sequential 1..7 — the old {1,3,4,5,6,7,8} confusingly dropped 2. */
    static const int btn_bit[7] = {0,2,3,4,5,6,7};
    int cellw = 42, x0 = 6;
    for (int i = 0; i < 7; i++) {
        int on = (st->btn_bitmap >> btn_bit[i]) & 1;
        int cx = x0 + i*cellw;
        pp_fill(fb, cx, yb+12, cellw-6, 30, on ? C_ON : C_OFF);
        pp_rect(fb, cx, yb+12, cellw-6, 30, on ? C_ACCENT : C_PANEL);
        snprintf(line, sizeof line, "%d", i+1);
        pp_text(fb, cx + (cellw-6 - pp_tw(line,2))/2, yb+20, line, on ? C_TEXT : C_DIM, 2);
    }

    /* ---- GPIO control hint (kept left of the GPIO panel to avoid overlap) ---- */
    pp_text(fb, 6, 178, "toggle L=14 R=13", C_DIM, 1);

    /* ---- touch panel (left; narrowed to make room for the wider GPIO panel).
     * Shows the boot-probe result (touch_ok) plus live hardware-debug: the raw
     * PENIRQ pen-detect line and the raw TSC2007 pressure z. Read them together:
     *   n/a           -> TSC2007 not found on I2C at boot (see gauge: shared bus)
     *   PIRQ stays HI  -> pen-detect line never asserts (panel/flex/PENIRQ wiring)
     *   PIRQ LO, z low -> contact sensed but no continuity (touch-glass / connector)
     *   PIRQ LO, z ok  -> touch working (coords shown) ---- */
    int tx=6, ty=188, tw=148, th=50;
    pp_fill(fb, tx, ty, tw, th, C_PANEL2);
    pp_rect(fb, tx, ty, tw, th, C_PANEL);
    pp_text(fb, tx+8, ty+4, "TOUCH", C_DIM, 1);
    pp_text(fb, tx+8+pp_tw("TOUCH ",1), ty+4,
            st->touch_ok ? (st->touch_down?"DOWN":"up") : "n/a",
            st->touch_ok ? (st->touch_down?C_GOOD:C_DIM) : C_BAD, 1);
    /* raw pen line (LO=asserted) + raw pressure, ALWAYS shown */
    pp_text(fb, tx+8, ty+18, "PIRQ", C_DIM, 1);
    pp_text(fb, tx+8+pp_tw("PIRQ ",1), ty+18, st->penirq?"HI":"LO",
            st->penirq?C_DIM:C_GOOD, 1);
    snprintf(line, sizeof line, "z:%u", st->touch_z);
    pp_text(fb, tx+8+pp_tw("PIRQ HI   ",1), ty+18, line, st->touch_z>10?C_GOOD:C_WARN, 1);
    if (st->touch_down) {
        snprintf(line, sizeof line, "%u,%u", st->touch_x, st->touch_y);
        pp_text(fb, tx+8, ty+32, line, C_TEXT, 1);
    } else {
        pp_text(fb, tx+8, ty+32, "(no contact)", C_DIM, 1);
    }

    /* ---- user-GPIO panel: state cells (15/14/13) + a 6-pin header mockup ---- */
    int gx=158, gy=176, gw=156, gh=58;
    pp_fill(fb, gx, gy, gw, gh, C_PANEL2);
    pp_rect(fb, gx, gy, gw, gh, C_PANEL);
    pp_text(fb, gx+6, gy+2, "GPIO", C_DIM, 1);
    /* status cells, ordered 15(in) / 14(out) / 13(out) left->right */
    struct { const char *lbl; int val; int is_out; } gp[3] = {
        { "15", st->gpio_in, 0 }, { "14", st->gpio_o1, 1 }, { "13", st->gpio_o0, 1 },
    };
    for (int i = 0; i < 3; i++) {
        int cx = gx + 10 + i*48, cy = gy+12, cw = 30, ch = 12;
        uint16_t c = gp[i].val ? (gp[i].is_out ? C_ON : C_GOOD) : C_OFF;
        pp_fill(fb, cx, cy, cw, ch, c);
        pp_rect(fb, cx, cy, cw, ch, gp[i].is_out ? C_ACCENT : C_PANEL);
        pp_text(fb, cx + (cw - pp_tw(gp[i].lbl,1))/2, cy+2, gp[i].lbl,
                gp[i].val ? C_TEXT : C_DIM, 1);
        pp_text(fb, cx + (cw - pp_tw(gp[i].is_out?"out":"in",1))/2, gy+25,
                gp[i].is_out ? "out" : "in", C_DIM, 1);
    }
    /* 6-pin header mockup: pins 13,14,15,3V3,G,5V left->right (physical order).
     * The three GPIO pads mirror the live state; the power pins are static. */
    struct { const char *lbl; uint16_t c; } hp[6] = {
        { "15", st->gpio_in?C_GOOD:C_OFF }, { "14", st->gpio_o1?C_ON:C_OFF },
        { "13", st->gpio_o0?C_ON:C_OFF }, { "3V3", C_BAD },
        { "G", C_DIM }, { "5V", C_BAD },
    };
    int hx=gx+6, hy=gy+38, hsp=(gw-12)/6;                 /* 6 pins across the panel */
    pp_fill(fb, hx-2, hy-2, 6*hsp+4, 10, C_OFF);          /* connector body strip */
    pp_rect(fb, hx-2, hy-2, 6*hsp+4, 10, C_PANEL);
    for (int i = 0; i < 6; i++) {
        int pcx = hx + i*hsp + hsp/2;                     /* pin centre */
        pp_fill(fb, pcx-3, hy, 6, 6, hp[i].c);            /* pin pad */
        pp_rect(fb, pcx-3, hy, 6, 6, C_PANEL);
        pp_text(fb, pcx - pp_tw(hp[i].lbl,1)/2, hy+9, hp[i].lbl, C_DIM, 1);
    }
}
#endif
