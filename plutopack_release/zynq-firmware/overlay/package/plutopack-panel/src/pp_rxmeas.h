/*
 * pp_rxmeas.h — shared RX measurement for the power-cal and power-meter apps.
 *
 * Configures the AD9361 via maia, captures IQ through the maia-httpd recorder
 * (the only working RX path on this bitstream — see pp_maiarx.h), and produces
 * a power-averaged spectrum plus a peak-tone finder and an overload flag.
 *
 * Several constants are HARDWARE-TUNABLE (int16 full-scale of the recorder
 * stream, overload margin, DC guard); tune them on the bench and re-cal.
 */
#ifndef PP_RXMEAS_H
#define PP_RXMEAS_H
#include "pp_maiarx.h"
#include "pp_fft.h"
#include <math.h>

#define PP_RX_NFFT       2048         /* FFT size (pow2, <= PP_FFT_MAX) */
#define PP_RX_NAVG       8            /* power-averaged FFT blocks */
#define PP_RX_ADC_FS     32767.0f     /* recorder int16 full-scale (HW-tunable) */
#define PP_RX_OVERLOAD   0.85f        /* "stop before overload" fraction of FS */
#define PP_RX_DCGUARD_HZ 300000LL     /* ignore |offset| < this (DC / LO leakage) */
#define PP_RX_TONE_BINS  2            /* integrate tone power over peak +/- this */

static int16_t pp_rx_iq[PP_RX_NFFT*PP_RX_NAVG*2];
static float   pp_rx_db[PP_RX_NFFT];
/* diagnostics from the last capture (raw ADC counts), exposed so the cal app can
 * show real levels: rawpk = peak |sample| INCLUDING DC; acpk = peak with the DC
 * offset removed (the meaningful clip metric); dc_i/dc_q = the DC means. */
static int    pp_rx_rawpk=0, pp_rx_acpk=0;
static double pp_rx_dc_i=0, pp_rx_dc_q=0;
#define PP_RX_SETTLE_BLKS 2          /* drop this many blocks after a retune */

/* Configure + capture + power-averaged spectrum. Skips the first block(s) after
 * a retune (LO/gain settling transient) and removes the DC offset, so a large DC
 * bias / self-mixing spur no longer false-triggers overload. Returns the AC peak
 * |sample| in *pk (the overload metric), or <0 on capture failure. */
static int pp_rx_capture(long long lo,long long fs,double gain,long long bw,int *pk){
    if(!mrx_config(lo,fs,gain,bw>0?bw:fs,"Manual")) return -1;
    int need=PP_RX_NFFT*PP_RX_NAVG;
    double dur=(double)need/(double)fs + 0.004;              /* samples + settle margin */
    int n=mrx_capture(pp_rx_iq,need,dur);
    if(n<PP_RX_NFFT) return -1;
    int nb=n/PP_RX_NFFT; if(nb<1) nb=1;
    int skip = nb>=4 ? PP_RX_SETTLE_BLKS : (nb>=2 ? 1 : 0);
    int s0=skip*PP_RX_NFFT, cnt=n-s0; if(cnt<1){ s0=0; cnt=n; skip=0; }
    /* DC means over the usable (post-settle) region */
    long si=0,sq=0; for(int i=s0;i<n;i++){ si+=pp_rx_iq[2*i]; sq+=pp_rx_iq[2*i+1]; }
    double di=(double)si/cnt, dq=(double)sq/cnt;
    /* raw peak (incl DC) and AC peak (DC removed) */
    int rawp=0; double acp=0;
    for(int i=s0;i<n;i++){
        int I=pp_rx_iq[2*i], Q=pp_rx_iq[2*i+1];
        int aI=I<0?-I:I, aQ=Q<0?-Q:Q; if(aI>rawp)rawp=aI; if(aQ>rawp)rawp=aQ;
        double xI=(double)I-di, xQ=(double)Q-dq, axI=xI<0?-xI:xI, axQ=xQ<0?-xQ:xQ;
        if(axI>acp)acp=axI; if(axQ>acp)acp=axQ;
    }
    pp_rx_rawpk=rawp; pp_rx_acpk=(int)acp; pp_rx_dc_i=di; pp_rx_dc_q=dq;
    if(pk)*pk=(int)acp;
    pp_rx_ol_note(1, (int)(acp*100.0/PP_RX_ADC_FS));         /* RX meter shows AC level */
    /* power-averaged spectrum over the usable blocks (settle skipped) */
    static float acc[PP_RX_NFFT]; for(int k=0;k<PP_RX_NFFT;k++) acc[k]=0.f;
    static float blk[PP_RX_NFFT]; int used=0;
    for(int b=skip;b<nb;b++){
        pp_iq_spectrum(pp_rx_iq + b*PP_RX_NFFT*2, PP_RX_NFFT, blk);
        for(int k=0;k<PP_RX_NFFT;k++) acc[k]+=powf(10.f, blk[k]*0.1f);
        used++;
    }
    if(used<1) used=1;
    for(int k=0;k<PP_RX_NFFT;k++) pp_rx_db[k]=10.f*log10f(acc[k]/used + 1e-12f);
    return 1;
}
/* overload judged on the AC peak (DC excluded) vs the tunable full-scale margin */
static int pp_rx_overloaded(int acpk){ return acpk >= (int)(PP_RX_OVERLOAD*PP_RX_ADC_FS); }

/* bin index -> LO offset (Hz); pp_rx_db is fftshifted (DC at NFFT/2). */
static long long pp_rx_bin_off(int k,long long fs){
    return (long long)(((double)(k-PP_RX_NFFT/2))*(double)fs/(double)PP_RX_NFFT);
}
/* integrated tone power (dB) over peak +/- PP_RX_TONE_BINS, reduces scalloping. */
static float pp_rx_tone_level(int kpeak){
    double s=0; int lo=kpeak-PP_RX_TONE_BINS, hi=kpeak+PP_RX_TONE_BINS;
    if(lo<0)lo=0; if(hi>PP_RX_NFFT-1)hi=PP_RX_NFFT-1;
    for(int k=lo;k<=hi;k++) s+=pow(10.0, pp_rx_db[k]*0.1);
    return (float)(10.0*log10(s+1e-12));
}
/* highest-power tone outside the DC guard: returns bin, sets *off_hz,*level_db.
 * *level_db is the integrated tone level; returns -1 if the band is empty. */
static int pp_rx_find_tone(long long fs,long long *off_hz,float *level_db){
    int best=-1; float bv=-1e30f;
    for(int k=0;k<PP_RX_NFFT;k++){
        long long off=pp_rx_bin_off(k,fs);
        if(off> -PP_RX_DCGUARD_HZ && off< PP_RX_DCGUARD_HZ) continue;
        if(pp_rx_db[k]>bv){ bv=pp_rx_db[k]; best=k; }
    }
    if(best<0) return -1;
    if(off_hz)   *off_hz=pp_rx_bin_off(best,fs);
    if(level_db) *level_db=pp_rx_tone_level(best);
    return best;
}
/* peak within +/- win of an EXPECTED offset (cal: the source freq is known, but
 * may be slightly off). returns bin, sets actual *off_hz and integrated *level. */
static int pp_rx_peak_near(long long want_off,long long fs,long long win,long long *off_hz,float *level){
    int kc=(int)llround((double)want_off/(double)fs*PP_RX_NFFT)+PP_RX_NFFT/2;
    int kw=(int)llround((double)win/(double)fs*PP_RX_NFFT); if(kw<1)kw=1;
    int lo=kc-kw, hi=kc+kw; if(lo<0)lo=0; if(hi>PP_RX_NFFT-1)hi=PP_RX_NFFT-1;
    int best=-1; float bv=-1e30f;
    for(int k=lo;k<=hi;k++) if(pp_rx_db[k]>bv){ bv=pp_rx_db[k]; best=k; }
    if(best<0) return -1;
    if(off_hz)*off_hz=pp_rx_bin_off(best,fs);
    if(level) *level=pp_rx_tone_level(best);
    return best;
}
/* noise-floor estimate (median-ish via min-average) for tone-present detection */
static float pp_rx_floor(void){
    float mn=1e30f; for(int k=0;k<PP_RX_NFFT;k++) if(pp_rx_db[k]<mn) mn=pp_rx_db[k];
    return mn;
}
#endif /* PP_RXMEAS_H */
