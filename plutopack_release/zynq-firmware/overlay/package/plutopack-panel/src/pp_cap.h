/*
 * pp_cap.h — raw RX I/Q buffer capture for the SCOPE app.
 *   Streams N complex samples from the AD9361 RX (cf-ad9361-lpc) via libiio and
 *   returns per-channel magnitude sqrt(I^2+Q^2) (phase discarded). Control (RX LO,
 *   sample rate, per-channel manual gain) is set on ad9361-phy. A PP_SIM branch
 *   synthesizes I/Q so the SCOPE app is testable in the web simulator.
 */
#ifndef PP_CAP_H
#define PP_CAP_H
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define CAP_MAXSAMP 16384
#define CAP_MAXCH   2

#ifndef PP_SIM
/* ------------------------------ real (libiio) ---------------------------- */
#include <iio.h>
static struct iio_context *g_ctx;
static struct iio_device  *g_phy, *g_rx;
static struct iio_channel *g_iq[4];              /* I0,Q0,I1,Q1 (input) */
static int g_adc_fs = 2048;                      /* converted-sample full scale (IIO format) */

static int cap_init(int *nrx){
    g_ctx = iio_create_local_context(); if(!g_ctx) return 0;
    g_phy = iio_context_find_device(g_ctx,"ad9361-phy");
    g_rx  = iio_context_find_device(g_ctx,"cf-ad9361-lpc");
    if(!g_phy || !g_rx) return 0;
    const char *nm[4]={"voltage0","voltage1","voltage2","voltage3"};
    for(int i=0;i<4;i++) g_iq[i]=iio_device_find_channel(g_rx,nm[i],false);
    if(!g_iq[0] || !g_iq[1]) return 0;
    *nrx = (g_iq[2] && g_iq[3]) ? 2 : 1;
    /* iio_channel_read returns converted (shift-stripped) values -> FS = 1<<(bits-1) */
    { const struct iio_data_format *df = iio_channel_get_data_format(g_iq[0]);
      int b = df ? (int)df->bits : 12; if(b<9)b=9; if(b>16)b=16; g_adc_fs = 1 << (b-1); }
    /* enable AD9361 quadrature + DC-offset tracking (DT leaves them off) */
    iio_device_attr_write(g_phy,"in_voltage_quadrature_tracking_en","1");
    iio_device_attr_write(g_phy,"in_voltage_rf_dc_offset_tracking_en","1");
    iio_device_attr_write(g_phy,"in_voltage_bb_dc_offset_tracking_en","1");
    return 1;
}
static int cap_set_lo(long long hz){
    struct iio_channel *c=iio_device_find_channel(g_phy,"altvoltage0",true);   /* RX_LO (Hz > 2^31) */
    return c && iio_channel_attr_write_longlong(c,"frequency",hz)>=0;
}
static int cap_set_fs(long hz){
    struct iio_channel *c=iio_device_find_channel(g_phy,"voltage0",false);
    return c && iio_channel_attr_write_longlong(c,"sampling_frequency",(long long)hz)>=0;
}
static int cap_set_bw(long hz){    /* RX analog bandwidth (in_voltage_rf_bandwidth) */
    struct iio_channel *c=iio_device_find_channel(g_phy,"voltage0",false);
    return c && iio_channel_attr_write_longlong(c,"rf_bandwidth",(long long)hz)>=0;
}
static int cap_set_gain(int ch,double db){
    char n[12]; snprintf(n,sizeof n,"voltage%d",ch);
    struct iio_channel *c=iio_device_find_channel(g_phy,n,false); if(!c) return 0;
    iio_channel_attr_write(c,"gain_control_mode","manual");
    return iio_channel_attr_write_double(c,"hardwaregain",db)>=0;
}
/* capture nsamp samples, fill mag[ch][0..nsamp-1]; returns 1 on success.
 * The RX buffer is PERSISTENT (created once, recreated only when nsamp changes) —
 * allocating/destroying a DMA buffer every frame made the scope very laggy. */
static struct iio_buffer *g_capb; static int g_capn, g_capch;
static int cap_frame(int nsamp,int nch,float **mag){
    if(nsamp<1) nsamp=1; if(nsamp>CAP_MAXSAMP) nsamp=CAP_MAXSAMP;
    if(nch>CAP_MAXCH) nch=CAP_MAXCH;
    if(!g_capb || g_capn!=nsamp || g_capch!=nch){
        if(g_capb){ iio_buffer_destroy(g_capb); g_capb=0; }
        for(int i=0;i<nch*2 && i<4;i++) if(g_iq[i]) iio_channel_enable(g_iq[i]);
        g_capb=iio_device_create_buffer(g_rx,(size_t)nsamp,false);
        if(!g_capb) return 0;
        g_capn=nsamp; g_capch=nch;
    }
    ssize_t r=iio_buffer_refill(g_capb); if(r<0){ iio_buffer_destroy(g_capb); g_capb=0; return 0; }
    static int16_t Ib[CAP_MAXSAMP], Qb[CAP_MAXSAMP];
    int pk[2]={0,0};
    for(int c=0;c<nch;c++){
        iio_channel_read(g_iq[2*c],   g_capb, Ib, (size_t)nsamp*2);
        iio_channel_read(g_iq[2*c+1], g_capb, Qb, (size_t)nsamp*2);
        for(int n=0;n<nsamp;n++){ int a=Ib[n]<0?-Ib[n]:Ib[n],b2=Qb[n]<0?-Qb[n]:Qb[n];
            if(a>pk[c])pk[c]=a; if(b2>pk[c])pk[c]=b2; double I=Ib[n],Q=Qb[n]; mag[c][n]=(float)sqrt(I*I+Q*Q); }
    }
    for(int c=0;c<nch;c++) pp_rx_ol_note(c+1, pk[c]*100/g_adc_fs);  /* per-channel ADC level -> meter */
    return 1;
}

#else
/* ------------------------------ sim (synth) ------------------------------ */
static long  c_fs=30720000; static double c_g[2]={40,40};
static int cap_init(int *nrx){ *nrx=2; return 1; }
static int cap_set_lo(long long hz){ (void)hz; return 1; }
static int cap_set_fs(long hz){ c_fs=hz>0?hz:1; return 1; }
static int cap_set_bw(long hz){ (void)hz; return 1; }
static int cap_set_gain(int ch,double db){ if(ch>=0&&ch<2) c_g[ch]=db; return 1; }
static int cap_frame(int nsamp,int nch,float **mag){
    if(nsamp>CAP_MAXSAMP) nsamp=CAP_MAXSAMP; if(nch>CAP_MAXCH) nch=CAP_MAXCH;
    double g0=pow(10.0,(c_g[0]-40.0)/20.0), g1=pow(10.0,(c_g[1]-40.0)/20.0);
    for(int n=0;n<nsamp;n++){
        double t=(double)n/(double)c_fs, nz=((rand()&0xff)/255.0-0.5)*70.0;
        if(nch>0) mag[0][n]=(float)(g0*1300.0*(0.85+0.15*sin(2*M_PI*80000.0*t))+nz);   /* ~CW envelope */
        if(nch>1) mag[1][n]=(float)(g1*1100.0*fabs(sin(2*M_PI*12000.0*t))+nz);          /* AM/rectified  */
    }
    return 1;
}
#endif
#endif
