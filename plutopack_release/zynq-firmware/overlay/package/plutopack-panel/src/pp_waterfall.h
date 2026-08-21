/*
 * pp_waterfall.h — PlutoPack SDR waterfall renderer (320x240 RGB565).
 *
 * Same host/target single-source-of-truth approach as pp_screen.h: this draws
 * the live /dev/fb0 on the PlutoSky AND the pixel-accurate mockup PNGs (see
 * waterfall_mockups/wf_mock.py, which mirrors this layout + the turbo LUT).
 *
 * DATA FORMAT (the interface to maia-sdr): each new spectrum is one row of
 * power values in dBFS. Feed it with ppwf_push(psd_dbfs, n_bins); the renderer
 * resamples to WF_BINS columns, clamps to [PPWF_DB_MIN, PPWF_DB_MAX], and maps
 * through the turbo colormap. maia's spectrometer FFT-power array is exactly
 * this (a per-bin power vector) — the port is: pull maia's spectrum (shared mem
 * / maia-httpd), convert to dBFS, ppwf_push(), ppwf_render().
 *
 * Layout:  status 0..21 | accent 22..23 | spectrum 24..73 | axis 74..85 | waterfall 86..239
 */
#ifndef PP_WATERFALL_H
#define PP_WATERFALL_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "pp_screen.h"           /* PP_W/PP_H, pp_rgb, pp_fill, pp_text, pp_tw, palette */

#define PPWF_STAT_H  22
#define PPWF_SPEC_Y0 24
#define PPWF_SPEC_Y1 74
#define PPWF_AXIS_Y0 74
#define PPWF_WF_Y0   86
#define PPWF_WF_ROWS (PP_H - PPWF_WF_Y0)   /* 154 */
#define PPWF_WF_BINS PP_W                  /* 320 */
#define PPWF_DB_MIN  (-100.0f)
#define PPWF_DB_MAX  (-30.0f)

#define C_TRACE pp_rgb(0x8a,0xe0,0xff)
#define C_GRID  pp_rgb(0x24,0x2b,0x3a)

/* turbo colormap, RGB565, 256 entries (generated to match wf_mock.py) */
static const uint16_t PPWF_TURBO[256] = {
  0x3087, 0x30a8, 0x30c9, 0x30ca, 0x30eb, 0x310b, 0x310c, 0x392d,
  0x394e, 0x396f, 0x396f, 0x3990, 0x39b1, 0x39b2, 0x39d2, 0x39f3,
  0x4214, 0x4214, 0x4235, 0x4256, 0x4256, 0x4277, 0x4297, 0x4298,
  0x42b8, 0x42d9, 0x42d9, 0x42fa, 0x431a, 0x431b, 0x433b, 0x435c,
  0x435c, 0x437c, 0x439d, 0x439d, 0x43bd, 0x43de, 0x43de, 0x43fe,
  0x43fe, 0x441f, 0x443f, 0x443f, 0x445f, 0x447f, 0x447f, 0x449f,
  0x449f, 0x44bf, 0x3cdf, 0x3cdf, 0x3cff, 0x3d1f, 0x3d1f, 0x3d3f,
  0x355f, 0x355f, 0x357e, 0x357e, 0x2d9e, 0x2dbe, 0x2dbd, 0x2ddd,
  0x2ddd, 0x25fd, 0x261c, 0x261c, 0x263c, 0x263b, 0x1e5b, 0x1e5b,
  0x1e7a, 0x1e7a, 0x1e9a, 0x1e99, 0x1eb9, 0x1eb9, 0x16d8, 0x16d8,
  0x16f8, 0x16f7, 0x1f17, 0x1f17, 0x1f17, 0x1f36, 0x1f36, 0x1f36,
  0x1f55, 0x2755, 0x2755, 0x2774, 0x2774, 0x2f74, 0x2f73, 0x2f93,
  0x3792, 0x3792, 0x3fb2, 0x3fb1, 0x3fb1, 0x47b0, 0x47b0, 0x4fd0,
  0x4fcf, 0x57cf, 0x57ce, 0x5fce, 0x5fcd, 0x67ed, 0x67ed, 0x6fec,
  0x6fec, 0x77eb, 0x77eb, 0x7feb, 0x7fea, 0x87ea, 0x87ea, 0x87e9,
  0x8fe9, 0x8fe9, 0x97e8, 0x97e8, 0x9fe8, 0x9fe8, 0x9fe7, 0xa7e7,
  0xa7e7, 0xa7c7, 0xafc7, 0xafc6, 0xafc6, 0xb7c6, 0xb7c6, 0xb7a6,
  0xbfa6, 0xbfa6, 0xbf86, 0xc786, 0xc786, 0xc766, 0xcf66, 0xcf66,
  0xcf46, 0xcf46, 0xd746, 0xd726, 0xd726, 0xdf06, 0xdf06, 0xdf06,
  0xdee6, 0xe6e6, 0xe6c6, 0xe6c7, 0xe6a7, 0xeea7, 0xee87, 0xee87,
  0xee67, 0xee67, 0xf647, 0xf647, 0xf627, 0xf627, 0xf607, 0xf607,
  0xfde7, 0xfde7, 0xfdc7, 0xfda6, 0xfda6, 0xfd86, 0xfd86, 0xfd66,
  0xfd46, 0xfd46, 0xfd26, 0xfd06, 0xfd05, 0xfce5, 0xfcc5, 0xfcc5,
  0xfca5, 0xfc85, 0xfc65, 0xfc64, 0xfc44, 0xfc24, 0xfc04, 0xfc04,
  0xfbe4, 0xfbc3, 0xfba3, 0xfba3, 0xf383, 0xf363, 0xf343, 0xf342,
  0xf322, 0xf302, 0xf302, 0xf2e2, 0xeac2, 0xeac2, 0xeaa1, 0xea81,
  0xea81, 0xea61, 0xea41, 0xe241, 0xe221, 0xe221, 0xe201, 0xe201,
  0xd9e1, 0xd9e0, 0xd9c0, 0xd9c0, 0xd1a0, 0xd1a0, 0xd180, 0xd180,
  0xd160, 0xc960, 0xc940, 0xc940, 0xc140, 0xc120, 0xc120, 0xc100,
  0xb900, 0xb8e0, 0xb8e0, 0xb0e0, 0xb0c0, 0xb0c0, 0xa8c0, 0xa8a0,
  0xa8a0, 0xa0a0, 0xa080, 0xa080, 0x9880, 0x9860, 0x9060, 0x9060,
  0x9040, 0x8840, 0x8840, 0x8040, 0x8020, 0x8020, 0x7820, 0x7820,
};

/* waterfall ring buffer: colormap indices, newest row at wf_head */
static uint8_t  ppwf_ring[PPWF_WF_ROWS][PPWF_WF_BINS];
static int      ppwf_head = -1;          /* -1 = empty */
static int      ppwf_count = 0;
static float    ppwf_last[PPWF_WF_BINS];  /* newest PSD row (for the spectrum trace) */

static inline uint8_t ppwf_db2idx(float db) {
    float f = (db - PPWF_DB_MIN) / (PPWF_DB_MAX - PPWF_DB_MIN);
    if (f < 0) f = 0; if (f > 1) f = 1;
    return (uint8_t)(f * 255.0f + 0.5f);
}

static void ppwf_reset(void) { ppwf_head = -1; ppwf_count = 0; }

/* push one spectrum (n dBFS values); resampled (nearest) to WF_BINS columns */
static void ppwf_push(const float *psd_dbfs, int n) {
    ppwf_head = (ppwf_head + 1) % PPWF_WF_ROWS;
    if (ppwf_count < PPWF_WF_ROWS) ppwf_count++;
    for (int x = 0; x < PPWF_WF_BINS; x++) {
        int src = (n == PPWF_WF_BINS) ? x : (int)((long)x * n / PPWF_WF_BINS);
        float db = psd_dbfs[src];
        ppwf_last[x] = db;
        ppwf_ring[ppwf_head][x] = ppwf_db2idx(db);
    }
}

/* render the whole screen. lo/span in kHz; batt 0..100. */
static void ppwf_render(uint16_t *fb, uint32_t lo_khz, uint32_t span_khz,
                        int batt_pct, int charging) {
    char s[40];
    pp_fill(fb, 0, 0, PP_W, PP_H, C_BG);

    /* ---- waterfall: newest row at WF_Y0, scrolling down ---- */
    for (int r = 0; r < PPWF_WF_ROWS; r++) {
        int y = PPWF_WF_Y0 + r;
        int ring = ppwf_head - r;                 /* newest first */
        if (ring < 0) ring += PPWF_WF_ROWS;
        if (r >= ppwf_count) { pp_fill(fb, 0, y, PP_W, 1, C_BG); continue; }
        uint16_t *line = &fb[y * PP_W];
        const uint8_t *idx = ppwf_ring[ring];
        for (int x = 0; x < PP_W; x++) line[x] = PPWF_TURBO[idx[x]];
    }

    /* ---- spectrum trace (newest row) + grid ---- */
    for (int gy = PPWF_SPEC_Y0; gy < PPWF_SPEC_Y1; gy += 12) pp_fill(fb, 0, gy, PP_W, 1, C_GRID);
    if (ppwf_count > 0) {
        int h = PPWF_SPEC_Y1 - PPWF_SPEC_Y0 - 1;
        for (int x = 0; x < PP_W; x++) {
            float f = (ppwf_last[x] - PPWF_DB_MIN) / (PPWF_DB_MAX - PPWF_DB_MIN);
            if (f < 0) f = 0; if (f > 1) f = 1;
            int yv = PPWF_SPEC_Y1 - 1 - (int)(f * h);
            for (int yy = yv; yy < PPWF_SPEC_Y1 - 1; yy++) fb[yy * PP_W + x] = C_GRID; /* fill */
            fb[yv * PP_W + x] = C_TRACE;                                              /* line */
        }
    }

    /* ---- freq axis ticks + LO marker (guides through spectrum+waterfall) ---- */
    uint32_t f_lo = lo_khz - span_khz / 2;
    for (int k = 1; k <= 3; k++) {                 /* 3 evenly spaced GHz ticks */
        uint32_t fk = f_lo + (uint32_t)((uint64_t)span_khz * k / 4);
        int x = (int)((int64_t)(fk - f_lo) * PP_W / span_khz);
        for (int y = PPWF_SPEC_Y0; y < PP_H; y += 2) fb[y * PP_W + x] = C_GRID;
        snprintf(s, sizeof s, "%lu.%02lu", (unsigned long)(fk / 1000000),
                 (unsigned long)((fk % 1000000) / 10000));
        int lx = x - pp_tw(s, 1) / 2; if (lx < 0) lx = 0;
        if (lx > PP_W - pp_tw(s, 1)) lx = PP_W - pp_tw(s, 1);
        pp_text(fb, lx, PPWF_AXIS_Y0 + 2, s, C_DIM, 1);
    }
    int xlo = (int)((int64_t)(lo_khz - f_lo) * PP_W / span_khz);
    for (int y = PPWF_SPEC_Y0; y < PP_H; y++) fb[y * PP_W + xlo] = C_ACCENT;

    /* ---- status bar ---- */
    pp_fill(fb, 0, 0, PP_W, PPWF_STAT_H, C_PANEL);
    pp_fill(fb, 0, PPWF_STAT_H, PP_W, 2, C_ACCENT);
    pp_logo(fb, 5, 7, C_ACCENT, 1);
    snprintf(s, sizeof s, "%lu.%03lu GHz  %lu MHz",
             (unsigned long)(lo_khz / 1000000), (unsigned long)((lo_khz % 1000000) / 1000),
             (unsigned long)(span_khz / 1000));
    pp_text(fb, 96, 7, s, C_TEXT, 1);
    int bx = PP_W - 70;
    snprintf(s, sizeof s, "%3d%%", batt_pct);
    pp_text(fb, bx, 7, s, C_TEXT, 1);
    /* battery glyph */
    int gx = bx + 30, gy = 6, gw = 22, gh = 11;
    uint16_t bc = batt_pct >= 40 ? C_GOOD : (batt_pct >= 15 ? C_WARN : C_BAD);
    pp_rect(fb, gx, gy, gw, gh, C_DIM);
    pp_fill(fb, gx + gw, gy + 3, 2, gh - 6, C_DIM);   /* nub */
    int fw = (gw - 4) * (batt_pct < 0 ? 0 : batt_pct > 100 ? 100 : batt_pct) / 100;
    pp_fill(fb, gx + 2, gy + 2, fw, gh - 4, bc);
    if (charging) pp_text(fb, gx + gw + 4, gy + 1, "/", C_WARN, 1);
}
#endif
