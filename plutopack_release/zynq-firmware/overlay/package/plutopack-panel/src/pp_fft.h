/*
 * pp_fft.h — tiny in-place radix-2 FFT + IQ->dB spectrum, header-only.
 *   The panel normally lets maia-sdr do the FFT on the FPGA, but the IQ
 *   record/playback app owns its RX buffer (libiio) so it can also write raw
 *   samples to disk. It therefore computes its OWN spectrum on the CPU (both for
 *   the live "narrow-in" waterfall and for reviewing a captured file), so the
 *   live view and the review view run the IDENTICAL DSP path.
 *
 * On a Zynq-7020 (dual A9) a windowed 1024-pt FFT is ~microseconds; at ~30 fps
 * it is negligible. Absolute dB reference is arbitrary (the waterfall MIN/MAX
 * colormap range is user-adjustable), so this reports relative dB.
 */
#ifndef PP_FFT_H
#define PP_FFT_H
#include <math.h>
#include <stdint.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PP_FFT_MAX 4096          /* largest transform this header supports */

/* In-place radix-2 decimation-in-time FFT. n MUST be a power of two, n<=PP_FFT_MAX.
 * re[]/im[] hold the complex input and are overwritten with the transform. */
static void pp_fft(float *re, float *im, int n){
    /* bit-reversal permutation */
    for(int i=1,j=0;i<n;i++){
        int bit=n>>1;
        for(; j&bit; bit>>=1) j^=bit;
        j^=bit;
        if(i<j){ float t=re[i];re[i]=re[j];re[j]=t; t=im[i];im[i]=im[j];im[j]=t; }
    }
    for(int len=2; len<=n; len<<=1){
        double ang=-2.0*M_PI/(double)len;
        float wr=(float)cos(ang), wi=(float)sin(ang);
        for(int i=0;i<n;i+=len){
            float cr=1.f, ci=0.f;                     /* running twiddle */
            for(int k=0;k<len/2;k++){
                float ur=re[i+k], ui=im[i+k];
                float ar=re[i+k+len/2], ai=im[i+k+len/2];
                float vr=ar*cr - ai*ci, vi=ar*ci + ai*cr;
                re[i+k]=ur+vr; im[i+k]=ui+vi;
                re[i+k+len/2]=ur-vr; im[i+k+len/2]=ui-vi;
                float ncr=cr*wr - ci*wi; ci=cr*wi + ci*wr; cr=ncr;
            }
        }
    }
}

/* Interleaved int16 IQ (I,Q,I,Q...) of n complex samples (n a power of two) ->
 * DC-centred (fftshifted) relative-dB spectrum into db[0..n-1]. Applies a Hann
 * window to suppress spectral leakage so narrow signals are easy to localise. */
static void pp_iq_spectrum(const int16_t *iq, int n, float *db){
    static float re[PP_FFT_MAX], im[PP_FFT_MAX];
    if(n>PP_FFT_MAX) n=PP_FFT_MAX;
    for(int i=0;i<n;i++){
        float w=0.5f-0.5f*cosf(2.f*(float)M_PI*(float)i/(float)(n-1));   /* Hann */
        re[i]=(float)iq[2*i]   * w;
        im[i]=(float)iq[2*i+1] * w;
    }
    pp_fft(re,im,n);
    float inv=1.f/(float)n;
    for(int i=0;i<n;i++){
        int k=(i+n/2)%n;                                  /* fftshift: DC -> centre */
        float p=(re[k]*re[k]+im[k]*im[k])*inv*inv;
        db[i] = p>1e-9f ? 10.f*log10f(p) : -90.f;
    }
}
#endif
