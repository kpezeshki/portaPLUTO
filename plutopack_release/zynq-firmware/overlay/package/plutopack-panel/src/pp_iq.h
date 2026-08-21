/*
 * pp_iq.h — raw complex-baseband I/Q streaming over libiio for the IQ
 *   record/playback app. RX capture from cf-ad9361-lpc, TX playback into the
 *   DAC DMA buffer of cf-ad9361-dds-core-lpc, and AD9361 control on ad9361-phy.
 *
 * RX is the standard libiio capture (cf-ad9361-lpc) — clean, gapless, gain-
 * controlled. (It briefly went through the maia recorder as a workaround while
 * cf-ad9361-lpc failed to probe; that is fixed via the AD9361 SKIP_TX dig-tune
 * in the device tree, so we're back on the proper streaming path.)
 * Integer Hz values use the longlong setter (kHz*1000 exceeds 2^31 above 2.147 GHz).
 *
 * A PP_SIM branch synthesizes I/Q so the app is exercisable in the web simulator.
 */
#ifndef PP_IQ_H
#define PP_IQ_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define IQ_RX_DEV  "cf-ad9361-lpc"
#define IQ_TX_DEV  "cf-ad9361-dds-core-lpc"

#ifndef PP_SIM
/* ------------------------------ real (libiio) ---------------------------- */
#include <iio.h>
static struct iio_context *iq_ctx;
static struct iio_device  *iq_phy, *iq_rxd, *iq_txd;
static struct iio_channel *iq_rxi, *iq_rxq, *iq_txi, *iq_txq;
static struct iio_buffer  *iq_rxb, *iq_txb;
static int                 iq_rxn, iq_txn;
static int                 iq_adc_fs = 2048;   /* raw-sample full scale (from IIO data format) */

static struct iio_channel* iq_phy_rx(void){ return iio_device_find_channel(iq_phy,"voltage0",false); }
static struct iio_channel* iq_phy_tx(void){ return iio_device_find_channel(iq_phy,"voltage0",true);  }

static int iq_init(int *have_tx){
    iq_ctx = iio_create_local_context(); if(!iq_ctx) return 0;
    iq_phy = iio_context_find_device(iq_ctx,"ad9361-phy");
    iq_rxd = iio_context_find_device(iq_ctx,IQ_RX_DEV);
    iq_txd = iio_context_find_device(iq_ctx,IQ_TX_DEV);
    if(!iq_phy || !iq_rxd) return 0;
    iq_rxi = iio_device_find_channel(iq_rxd,"voltage0",false);
    iq_rxq = iio_device_find_channel(iq_rxd,"voltage1",false);
    if(!iq_rxi || !iq_rxq) return 0;
    /* raw buffer holds (12-bit value)<<shift; full scale = 1<<(bits-1+shift) */
    { const struct iio_data_format *df = iio_channel_get_data_format(iq_rxi);
      int b = df ? (int)df->bits + (int)df->shift : 12; if(b<9)b=9; if(b>16)b=16;
      iq_adc_fs = 1 << (b-1); }
    /* The device tree enables NO calibration tracking, so RX runs with bad I/Q
     * imbalance (image spurs) + a large DC offset. Turn on the AD9361's built-in
     * quadrature error + DC-offset tracking (RF and baseband). */
    iio_device_attr_write(iq_phy,"in_voltage_quadrature_tracking_en","1");
    iio_device_attr_write(iq_phy,"in_voltage_rf_dc_offset_tracking_en","1");
    iio_device_attr_write(iq_phy,"in_voltage_bb_dc_offset_tracking_en","1");
    if(iq_txd){
        iq_txi = iio_device_find_channel(iq_txd,"voltage0",true);
        iq_txq = iio_device_find_channel(iq_txd,"voltage1",true);
    }
    if(have_tx) *have_tx = (iq_txd && iq_txi && iq_txq) ? 1 : 0;
    return 1;
}

/* ---- control (all values in Hz; dB for gain/atten) ---- */
static int iq_set_rxlo(long long hz){
    struct iio_channel *c=iio_device_find_channel(iq_phy,"altvoltage0",true);   /* RX_LO */
    return c && iio_channel_attr_write_longlong(c,"frequency",hz)>=0;
}
/* re-clear the TX LO powerdown (maia re-parks it while it owns the radio) */
static void iq_txlo_on(void){
    struct iio_channel *c=iio_device_find_channel(iq_phy,"altvoltage1",true);
    if(c) iio_channel_attr_write_longlong(c,"powerdown",0);
}
static int iq_set_txlo(long long hz){
    struct iio_channel *c=iio_device_find_channel(iq_phy,"altvoltage1",true);   /* TX_LO */
    if(!c) return 0;
    iio_channel_attr_write_longlong(c,"powerdown",0);                           /* ensure TX LO on */
    return iio_channel_attr_write_longlong(c,"frequency",hz)>=0;
}
static int iq_set_fs(long hz){       /* shared RX/TX sample rate */
    struct iio_channel *c=iq_phy_rx();
    return c && iio_channel_attr_write_longlong(c,"sampling_frequency",(long long)hz)>=0;
}
static int iq_set_rxbw(long hz){
    struct iio_channel *c=iq_phy_rx();
    return c && iio_channel_attr_write_longlong(c,"rf_bandwidth",(long long)hz)>=0;
}
static int iq_set_txbw(long hz){
    struct iio_channel *c=iq_phy_tx();
    return c && iio_channel_attr_write_longlong(c,"rf_bandwidth",(long long)hz)>=0;
}
static int iq_set_rxgain(double db){
    struct iio_channel *c=iq_phy_rx(); if(!c) return 0;
    iio_channel_attr_write(c,"gain_control_mode","manual");
    return iio_channel_attr_write_double(c,"hardwaregain",db)>=0;
}
static int iq_set_gainmode(const char *mode){    /* "manual"/"slow_attack"/"fast_attack" */
    struct iio_channel *c=iq_phy_rx();
    return c && iio_channel_attr_write(c,"gain_control_mode",mode)>=0;
}
static int iq_set_txatten(double db){    /* TX hardwaregain is 0..-89.75 dB (attenuation) */
    struct iio_channel *c=iq_phy_tx(); if(!c) return 0;
    if(db>0) db=-db;                      /* accept a positive "attenuation" argument */
    return iio_channel_attr_write_double(c,"hardwaregain",db)>=0;
}
/* read-back the AD9361-quantized values so the UI can reconcile (sync rule) */
static int iq_get_fs(long long *hz){
    struct iio_channel *c=iq_phy_rx(); long long v;
    if(c && iio_channel_attr_read_longlong(c,"sampling_frequency",&v)>=0){ *hz=v; return 1; } return 0;
}
static int iq_get_rxlo(long long *hz){
    struct iio_channel *c=iio_device_find_channel(iq_phy,"altvoltage0",true); long long v;
    if(c && iio_channel_attr_read_longlong(c,"frequency",&v)>=0){ *hz=v; return 1; } return 0;
}
static int iq_get_rxgain(double *db){
    struct iio_channel *c=iq_phy_rx(); double v;
    if(c && iio_channel_attr_read_double(c,"hardwaregain",&v)>=0){ *db=v; return 1; } return 0;
}

/* ---- RX streaming ---- */
static int iq_rx_open(int n){
    iio_channel_enable(iq_rxi); iio_channel_enable(iq_rxq);
    iq_rxb = iio_device_create_buffer(iq_rxd,(size_t)n,false); /* non-cyclic */
    if(!iq_rxb) return 0;
    iq_rxn = n; return 1;
}
/* fill iq[2*n] interleaved I,Q; returns #complex samples or -1 */
static int iq_rx_read(int16_t *iq){
    if(!iq_rxb) return -1;
    ssize_t r=iio_buffer_refill(iq_rxb); if(r<0) return -1;
    ptrdiff_t inc=iio_buffer_step(iq_rxb);
    char *end=iio_buffer_end(iq_rxb), *p; int n=0;
    for(p=(char*)iio_buffer_first(iq_rxb,iq_rxi); p<end && n<iq_rxn; p+=inc){
        iq[2*n]   = ((int16_t*)p)[0];   /* I (voltage0, first in the sample)  */
        iq[2*n+1] = ((int16_t*)p)[1];   /* Q (voltage1, next 16-bit lane)     */
        n++;
    }
    return n;
}
static void iq_rx_close(void){
    if(iq_rxb){ iio_buffer_destroy(iq_rxb); iq_rxb=0; }
    if(iq_rxi) iio_channel_disable(iq_rxi);
    if(iq_rxq) iio_channel_disable(iq_rxq);
}

/* ---- TX streaming (DAC DMA) ---- */
static int iq_tx_open(int n){
    if(!iq_txd || !iq_txi || !iq_txq) return 0;
    iio_channel_enable(iq_txi); iio_channel_enable(iq_txq);
    iq_txb = iio_device_create_buffer(iq_txd,(size_t)n,false); /* streaming, non-cyclic */
    if(!iq_txb) return 0;
    iq_txn = n; return 1;
}
/* push n<=iq_txn complex samples (interleaved I,Q); returns 1 on success */
static int iq_tx_write(const int16_t *iq, int n){
    if(!iq_txb) return 0;
    if(n>iq_txn) n=iq_txn;
    ptrdiff_t inc=iio_buffer_step(iq_txb);
    char *start=(char*)iio_buffer_first(iq_txb,iq_txi), *end=iio_buffer_end(iq_txb), *p;
    int k=0;
    for(p=start; p<end; p+=inc){
        if(k<n){ ((int16_t*)p)[0]=iq[2*k]; ((int16_t*)p)[1]=iq[2*k+1]; }
        else   { ((int16_t*)p)[0]=0;       ((int16_t*)p)[1]=0; }   /* pad tail with silence */
        k++;
    }
    iq_txlo_on();                    /* keep TX LO up: maia re-parks it ~1 Hz */
    return iio_buffer_push(iq_txb)>=0;
}
static void iq_tx_close(void){
    if(iq_txb){ iio_buffer_destroy(iq_txb); iq_txb=0; }
    if(iq_txi) iio_channel_disable(iq_txi);
    if(iq_txq) iio_channel_disable(iq_txq);
}

#else
/* ------------------------------ sim (synth) ------------------------------ */
#include <math.h>
static long long iq_lo=433920000LL; static long iq_fs=2083333; static double iq_g=40.0;
static int iq_adc_fs = 2048;    /* raw-sample full scale (mirrors the non-sim default) */
static int iq_init(int *have_tx){ if(have_tx)*have_tx=1; return 1; }
static void iq_txlo_on(void){}
static int iq_set_rxlo(long long hz){ iq_lo=hz; return 1; }
static int iq_set_txlo(long long hz){ (void)hz; return 1; }
static int iq_set_fs(long hz){ iq_fs=hz>0?hz:1; return 1; }
static int iq_set_rxbw(long hz){ (void)hz; return 1; }
static int iq_set_txbw(long hz){ (void)hz; return 1; }
static int iq_set_rxgain(double db){ iq_g=db; return 1; }
static int iq_set_gainmode(const char *mode){ (void)mode; return 1; }
static int iq_set_txatten(double db){ (void)db; return 1; }
static int iq_get_fs(long long *hz){ *hz=iq_fs; return 1; }
static int iq_get_rxlo(long long *hz){ *hz=iq_lo; return 1; }
static int iq_get_rxgain(double *db){ *db=iq_g; return 1; }
static int iq_sim_n;
static int iq_rx_open(int n){ iq_sim_n=n; return 1; }
static int iq_rx_read(int16_t *iq){
    static double ph[3]; static const long long SIG[3]={433920000LL,868000000LL,315000000LL};
    static const double AMP[3]={900.0,500.0,300.0};
    double g=pow(10.0,(iq_g-40.0)/20.0);
    for(int i=0;i<iq_sim_n;i++){
        double I=((rand()&0xff)/255.0-0.5)*60.0, Q=((rand()&0xff)/255.0-0.5)*60.0;
        for(int s=0;s<3;s++){
            double off=(double)(SIG[s]-iq_lo);
            if(fabs(off) < iq_fs*0.48){
                double dph=2.0*M_PI*off/(double)iq_fs; ph[s]+=dph;
                double a=AMP[s]*(0.6+0.4*sin(i*0.001+s));       /* slow OOK-ish envelope */
                I+=g*a*cos(ph[s]); Q+=g*a*sin(ph[s]);
            }
        }
        iq[2*i]=(int16_t)I; iq[2*i+1]=(int16_t)Q;
    }
    return iq_sim_n;
}
static void iq_rx_close(void){}
static int iq_tx_open(int n){ (void)n; return 1; }
static int iq_tx_write(const int16_t *iq, int n){ (void)iq; (void)n; return 1; }
static void iq_tx_close(void){}
#endif
#endif
