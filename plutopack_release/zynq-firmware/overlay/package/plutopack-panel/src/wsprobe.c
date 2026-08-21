/* wsprobe — diagnostic: open maia's /waterfall websocket and report whether
 * frames are actually streaming (count, peak linear power, nonzero bins) over ~3s.
 * Used to tell "spectrometer not streaming" from "streaming but blank (gain)". */
#include <stdio.h>
#include <unistd.h>
#include "pp_maia.h"
int main(void){
    int fd=maia_ws_open();
    if(fd<0){ printf("ws_open FAIL\n"); return 1; }
    static float fr[MAIA_FFT];
    int frames=0, nonzero=0; float gmax=0;
    for(int i=0;i<60;i++){
        int got=maia_ws_drain(fd,fr);
        if(got>0){ frames++;
            for(int b=1;b<MAIA_FFT;b++){ if(fr[b]>gmax)gmax=fr[b]; if(fr[b]>1e-7f)nonzero++; }
        }
        usleep(50000);
    }
    printf("frames=%d peak_lin=%.3e nonzero_bins~%d\n", frames, gmax, nonzero/(frames?frames:1));
    return 0;
}
