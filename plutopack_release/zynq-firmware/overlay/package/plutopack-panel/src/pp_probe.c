/*
 * pp-probe — raw spidev frame dump. Sends the A5 status-request and prints the
 * 32 raw MISO bytes, in whichever SPI mode you pass. Handy for low-level link
 * bring-up (e.g. proving the mode-1 / CPHA=1 requirement).
 * usage: pp-probe [spidev=/dev/spidev2.0] [mode=1] [hz=1000000] [n=4]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#define N 32
static uint8_t crc8(const uint8_t*p,int n){uint8_t c=0;for(int i=0;i<n;i++){c^=p[i];for(int b=0;b<8;b++)c=(c&0x80)?(uint8_t)((c<<1)^0x07):(uint8_t)(c<<1);}return c;}
int main(int argc,char**argv){
    const char*dev=argc>1?argv[1]:"/dev/spidev2.0";
    uint8_t mode=argc>2?(uint8_t)atoi(argv[2]):1;
    uint32_t hz=argc>3?(uint32_t)atoi(argv[3]):1000000;
    int reps=argc>4?atoi(argv[4]):4;
    int fd=open(dev,O_RDWR); if(fd<0){perror("open");return 1;}
    uint8_t bits=8;
    ioctl(fd,SPI_IOC_WR_MODE,&mode); ioctl(fd,SPI_IOC_WR_BITS_PER_WORD,&bits); ioctl(fd,SPI_IOC_WR_MAX_SPEED_HZ,&hz);
    printf("dev=%s mode=%u hz=%u\n",dev,mode,hz);
    for(int k=0;k<reps;k++){
        uint8_t tx[N],rx[N]; memset(tx,0,N); tx[0]=0xA5; tx[N-1]=crc8(tx,N-1); memset(rx,0xEE,N);
        struct spi_ioc_transfer t; memset(&t,0,sizeof t);
        t.tx_buf=(unsigned long)tx; t.rx_buf=(unsigned long)rx; t.len=N; t.speed_hz=hz; t.bits_per_word=8;
        if(ioctl(fd,SPI_IOC_MESSAGE(1),&t)<0){perror("xfer");return 1;}
        printf("rx%d:",k); for(int i=0;i<N;i++) printf(" %02x",rx[i]); printf("\n");
        usleep(80000);
    }
    return 0;
}
