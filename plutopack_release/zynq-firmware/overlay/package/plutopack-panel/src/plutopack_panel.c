/*
 * plutopack-panel — PlutoSky front-panel daemon.
 *
 * Polls the Pico over the Zynq<->Pico SPI bus (spidev on the spi-gpio master),
 * decodes its status frame (gauge / encoder / buttons / touch — the same fields
 * the Pico demo prints over USB serial), and renders them to /dev/fb0 (the
 * ILI9341 panel, 320x240 RGB565) using the shared pp_screen renderer. At start
 * it releases the panel reset and asserts CS through the Pico.
 *
 * Frame protocol (32 bytes, SPI mode 0, must match plutopack_mcu.c):
 *   master->pico: [0]=0xA5 [1]=cmd [2]=arg0 [3]=arg1 [4]=ack_events ... [31]=crc8
 *   pico->master: [0]=0x5A [1]=seq [2]=flags [3]=btnmap [4..7]=enc(i32)
 *                 [8]=touch_dn [9..10]=x [11..12]=y [13..14]=z1
 *                 [15..16]=batt_mV [17]=soc [18..19]=cur(i16) [20]=lcdctl
 *                 [21]=evt_count ... [31]=crc8
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/spi/spidev.h>
#include "pp_screen.h"

#define FRAME_LEN 32
#define M_MAGIC 0xA5
#define S_MAGIC 0x5A
#define CMD_NOP 0x00
#define CMD_SET_DISP_CTRL 0x01   /* arg0 bit0=RST level, bit1=CS level */
#define CMD_PULSE_RESET   0x02   /* arg0 = low-pulse ms */

static uint8_t crc8(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) { c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c<<1)^0x07) : (uint8_t)(c<<1); }
    return c;
}

static int spi_fd = -1;
static uint32_t spi_speed = 1000000;

static int spi_xfer(const uint8_t *tx, uint8_t *rx) {
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof t);
    t.tx_buf = (unsigned long)tx;
    t.rx_buf = (unsigned long)rx;
    t.len = FRAME_LEN;
    t.speed_hz = spi_speed;
    t.bits_per_word = 8;
    return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 ? -1 : 0;
}

/* send one command frame; capture the returned status frame (may be stale by 1) */
static int cmd(uint8_t c, uint8_t a0, uint8_t a1, uint8_t ack, uint8_t *rx) {
    uint8_t tx[FRAME_LEN];
    memset(tx, 0, FRAME_LEN);
    tx[0] = M_MAGIC; tx[1] = c; tx[2] = a0; tx[3] = a1; tx[4] = ack;
    tx[FRAME_LEN-1] = crc8(tx, FRAME_LEN-1);
    return spi_xfer(tx, rx);
}

static int decode(const uint8_t *r, pp_status_t *s) {
    if (r[0] != S_MAGIC) return -1;
    if (crc8(r, FRAME_LEN-1) != r[FRAME_LEN-1]) return -2;
    uint8_t fl = r[2];
    s->gauge_ok = !!(fl & 0x01);
    s->touch_ok = !!(fl & 0x02);
    s->charging = !!(fl & 0x08);
    s->btn_bitmap = r[3];
    s->enc_pos = (int32_t)((uint32_t)r[4] | (r[5]<<8) | (r[6]<<16) | ((uint32_t)r[7]<<24));
    s->touch_down = r[8];
    s->touch_x = r[9]  | (r[10]<<8);
    s->touch_y = r[11] | (r[12]<<8);
    s->touch_z = r[13] | (r[14]<<8);
    s->batt_mV = r[15] | (r[16]<<8);
    s->soc = r[17];
    s->current_mA = (int16_t)(r[18] | (r[19]<<8));
    s->seq = r[1];
    return r[21]; /* queued event count */
}

int main(int argc, char **argv) {
    const char *spidev = argc > 1 ? argv[1] : "/dev/spidev1.0";
    const char *fbdev  = argc > 2 ? argv[2] : "/dev/fb0";

    /* --- framebuffer --- */
    int fb = open(fbdev, O_RDWR);
    if (fb < 0) { perror("open fb"); return 1; }
    struct fb_var_screeninfo vinfo; struct fb_fix_screeninfo finfo;
    ioctl(fb, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb, FBIOGET_FSCREENINFO, &finfo);
    if (vinfo.bits_per_pixel != 16)
        fprintf(stderr, "warning: fb is %u bpp, renderer emits RGB565\n", vinfo.bits_per_pixel);
    int stride = finfo.line_length / 2;   /* pixels per row */
    /* NOTE: fbtft flushes to the panel on the write()/fb_write path, NOT via mmap
     * deferred-I/O in this build — so we push frames with pwrite(), not memcpy. */

    /* --- spidev --- */
    spi_fd = open(spidev, O_RDWR);
    if (spi_fd < 0) { perror("open spidev"); return 1; }
    uint8_t mode = SPI_MODE_1;   /* Pico PL022 slave needs CPHA=1 (mode 1) for block transfers */ uint8_t bits = 8;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed);

    /* Do NOT reset the panel here. The Pico already holds RESET high / CS low from
     * its own boot, and fbtft ran the full ILI9341 init sequence at bind. Pulsing
     * RESET (as this used to) wipes fbtft's init — and fbtft does NOT re-init — so
     * the panel is left asleep/display-off and everything we draw shows as black. */
    uint8_t rx[FRAME_LEN];

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st, 0, sizeof st);

    for (;;) {
        int n = cmd(CMD_NOP, 0, 0, 0 /*ack*/, rx) == 0 ? decode(rx, &st) : -1;
        (void)n;
        pp_render(buf, &st);
        /* blit our 320x240 buffer into the fb honoring its stride */
        if (stride == PP_W && (int)vinfo.yres >= PP_H) {
            pwrite(fb, buf, (size_t)PP_W*PP_H*2, 0);
        } else {
            int rows = (int)vinfo.yres < PP_H ? (int)vinfo.yres : PP_H;
            int cols = (int)vinfo.xres < PP_W ? (int)vinfo.xres : PP_W;
            for (int y = 0; y < rows; y++)
                pwrite(fb, buf + (size_t)y*PP_W, (size_t)cols*2, (off_t)y*finfo.line_length);
        }
        usleep(200000); /* ~5 Hz refresh */
    }
    return 0;
}
