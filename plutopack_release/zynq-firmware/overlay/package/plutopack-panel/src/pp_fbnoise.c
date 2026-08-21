/*
 * pp-fbnoise — write random noise to /dev/fb0. Fast display sanity check
 * (proves the fbtft write path + panel are alive, independent of any renderer).
 * usage: pp-fbnoise [fbdev=/dev/fb0] [frames=5]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
int main(int argc,char**argv){
    const char*dev=argc>1?argv[1]:"/dev/fb0";
    int frames=argc>2?atoi(argv[2]):5;
    int fb=open(dev,O_RDWR); if(fb<0){perror("open fb");return 1;}
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fb,FBIOGET_VSCREENINFO,&v); ioctl(fb,FBIOGET_FSCREENINFO,&f);
    size_t n=(size_t)f.line_length*v.yres;
    uint8_t *b=malloc(n); if(!b){return 1;}
    for(int k=0;k<frames;k++){
        for(size_t i=0;i<n;i++) b[i]=(uint8_t)rand();
        pwrite(fb,b,n,0);
        printf("frame %d (%zu bytes)\n",k,n); fflush(stdout);
        sleep(1);
    }
    free(b); return 0;
}
