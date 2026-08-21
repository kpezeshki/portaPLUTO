/* mrx_test — validate pp_maiarx.h on hardware: capture IQ via the maia recorder,
 * report |max| and the spectral peak (should land on an injected tone). No fb. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "pp_maiarx.h"
#include "pp_fft.h"
int main(int argc,char**argv){
    long long lo = argc>1? atoll(argv[1]) : 2400000000LL;
    long long fs = argc>2? atoll(argv[2]) : 4000000LL;
    double  gain = argc>3? atof(argv[3])  : 10.0;
    if(!mrx_config(lo,fs,gain,fs,"Manual")){ printf("config FAIL\n"); return 1; }
    long long rfs = mrx_get_fs();
    static int16_t iq[4096*2];
    int n = mrx_capture(iq,4096,0.01);
    printf("captured=%d fs=%lld\n", n, rfs);
    if(n<256){ printf("TOO FEW\n"); return 1; }
    int nfft=4096; while(nfft>n) nfft>>=1;
    int mx=0; for(int i=0;i<nfft*2;i++){ int a=iq[i]<0?-iq[i]:iq[i]; if(a>mx)mx=a; }
    static float db[4096];
    pp_iq_spectrum(iq,nfft,db);
    int pk=0; for(int i=1;i<nfft;i++) if(db[i]>db[pk]) pk=i;
    /* median as a floor estimate */
    static float srt[4096]; for(int i=0;i<nfft;i++) srt[i]=db[i];
    for(int a=0;a<nfft;a++) for(int b=a+1;b<nfft;b++) if(srt[b]<srt[a]){float t=srt[a];srt[a]=srt[b];srt[b]=t;}
    float floor=srt[nfft/2];
    double fpk = (double)(pk - nfft/2) * (double)rfs / nfft;
    printf("nfft=%d |max|=%d peak f=%+.3f MHz  %.1f dB above floor\n",
           nfft, mx, fpk/1e6, db[pk]-floor);
    return 0;
}
