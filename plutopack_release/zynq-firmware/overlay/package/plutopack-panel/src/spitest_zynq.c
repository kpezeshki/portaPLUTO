/*
 * spitest_zynq — Zynq-side half of the PlutoPack SPI link test.
 * Master (spidev over the spi-gpio bus) that drives the call/response frames the
 * Pico plutopack_spitest firmware expects, and validates the Pico's replies.
 *
 *   Usage: spitest_zynq [/dev/spidevX.Y] [n_frames] [speed_hz]
 *   Default: /dev/spidev1.0  200 frames  100000 Hz
 *
 * Call  (master->pico): [0xC0, seq, seq^0xFF, 0xC3, 0,0,0, crc8]
 * Resp  (pico->master): [0x5A, last_seq, valid_lo, valid_hi, xact_lo, xact_hi, 0, crc8]
 *   (the Pico preloads its TX, so a reply reflects the PREVIOUS call — 1-frame lag)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define FRAME 8
static uint8_t crc8(const uint8_t *p, int n){uint8_t c=0;for(int i=0;i<n;i++){c^=p[i];for(int b=0;b<8;b++)c=(c&0x80)?(uint8_t)((c<<1)^7):(uint8_t)(c<<1);}return c;}

int main(int argc, char **argv){
    const char *dev = argc>1?argv[1]:"/dev/spidev1.0";
    int n = argc>2?atoi(argv[2]):200;
    uint32_t hz = argc>3?(uint32_t)atoi(argv[3]):100000;

    int fd = open(dev, O_RDWR);
    if(fd<0){ perror("open spidev"); return 1; }
    uint8_t mode=SPI_MODE_0, bits=8;
    ioctl(fd,SPI_IOC_WR_MODE,&mode); ioctl(fd,SPI_IOC_WR_BITS_PER_WORD,&bits);
    ioctl(fd,SPI_IOC_WR_MAX_SPEED_HZ,&hz);
    printf("spitest_zynq: %s  %d frames @ %u Hz\n", dev, n, hz);

    int resp_seen=0, resp_ok=0;
    uint32_t last_valid=0, last_xact=0;
    for(int i=0;i<n;i++){
        uint8_t tx[FRAME]={0}, rx[FRAME]={0};
        uint8_t seq=(uint8_t)i;
        tx[0]=0xC0; tx[1]=seq; tx[2]=seq^0xFF; tx[3]=0xC3; tx[7]=crc8(tx,FRAME-1);
        struct spi_ioc_transfer t; memset(&t,0,sizeof t);
        t.tx_buf=(unsigned long)tx; t.rx_buf=(unsigned long)rx; t.len=FRAME;
        t.speed_hz=hz; t.bits_per_word=8;
        if(ioctl(fd,SPI_IOC_MESSAGE(1),&t)<0){ perror("SPI_IOC_MESSAGE"); return 1; }
        if(rx[0]==0x5A && crc8(rx,FRAME-1)==rx[FRAME-1]){
            resp_seen++; resp_ok++;
            last_valid = rx[2]|(rx[3]<<8);
            last_xact  = rx[4]|(rx[5]<<8);
        } else if(rx[0]==0x5A){ resp_seen++; }
        usleep(2000);
    }
    printf("--- result ---\n");
    printf("frames sent          : %d\n", n);
    printf("0x5A replies seen    : %d\n", resp_seen);
    printf("valid (magic+crc) rep: %d\n", resp_ok);
    printf("pico last valid count: %u\n", last_valid);
    printf("pico last xact count : %u\n", last_xact);
    if(resp_ok>n-3) printf("VERDICT: SPI LINK CLEAN (bidirectional OK)\n");
    else if(resp_seen>0) printf("VERDICT: FLAKY (some replies corrupt) -> signaling marginal\n");
    else printf("VERDICT: NO VALID REPLY -> MISO dead, wrong dev, or Pico not running\n");
    close(fd);
    return 0;
}
