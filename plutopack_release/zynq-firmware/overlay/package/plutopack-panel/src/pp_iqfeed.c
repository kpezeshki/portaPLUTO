/*
 * pp-iqfeed — stream AD9361 baseband I/Q as cs16 on stdout, for piping into
 *   rtl_433 / dump1090 (which cannot talk to a Pluto directly).
 *
 * RX is the standard libiio capture (cf-ad9361-lpc) — a clean, gapless, gain-
 * controlled stream (this is what GNU Radio uses). We DC-block each block (the
 * AD9361 carries a static I/Q offset that buries weak AM/pulse signals) and
 * boxcar-decimate by D, emitting cs16 at fs/D. Verified end-to-end: dump1090
 * decodes ADS-B aircraft from this feed.
 *
 * OUTPUT SCALING: full-scale int16 -> downstream must use SC16 / cs16 (NOT Q11).
 *
 * Each block also prints a level line to stderr for the GUI saturation meter:
 *     LVL <rms%FS> <peak%FS>   (raw, pre-DC-removal; peak = ADC saturation)
 *
 * usage: pp-iqfeed [lo_hz=433920000] [fs_hz=2083333] [decim=8] [gain_db=40]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <iio.h>

#define IQ_RX_DEV "cf-ad9361-lpc"
#define FEED_N 65536          /* complex samples per capture block */

int main(int argc, char **argv){
    long long lo = argc>1 ? atoll(argv[1]) : 433920000LL;
    long      fs = argc>2 ? atol(argv[2])  : 2083333;
    int       D  = argc>3 ? atoi(argv[3])  : 8;   if(D<1) D=1;
    double    g  = argc>4 ? atof(argv[4])  : 40.0;
    const char *gmode = argc>5 ? argv[5] : "manual";   /* "manual" | "slow" | "fast" (AGC) */

    struct iio_context *ctx = iio_create_local_context();
    if(!ctx){ fprintf(stderr,"iqfeed: no iio context\n"); return 1; }
    struct iio_device *phy = iio_context_find_device(ctx,"ad9361-phy");
    struct iio_device *rxd = iio_context_find_device(ctx,IQ_RX_DEV);
    if(!phy || !rxd){ fprintf(stderr,"iqfeed: %s missing (RX capture not up)\n", rxd?"ad9361-phy":IQ_RX_DEV); return 2; }

    /* AD9361 control on ad9361-phy */
    struct iio_channel *rxlo = iio_device_find_channel(phy,"altvoltage0",true);   /* RX_LO */
    struct iio_channel *prx  = iio_device_find_channel(phy,"voltage0",false);     /* RX ctrl */
    if(rxlo) iio_channel_attr_write_longlong(rxlo,"frequency",lo);
    if(prx){
        iio_channel_attr_write_longlong(prx,"sampling_frequency",(long long)fs);
        iio_channel_attr_write_longlong(prx,"rf_bandwidth",(long long)fs);
        if(gmode[0]=='s' || gmode[0]=='f'){            /* AGC: let the AD9361 track gain */
            iio_channel_attr_write(prx,"gain_control_mode", gmode[0]=='f'?"fast_attack":"slow_attack");
        } else {
            iio_channel_attr_write(prx,"gain_control_mode","manual");
            iio_channel_attr_write_double(prx,"hardwaregain",g);
        }
    }
    /* enable AD9361 quadrature + DC-offset tracking (DT leaves them off) —
     * removes I/Q image spurs and the DC spike that hurt OOK/pulse decoding */
    iio_device_attr_write(phy,"in_voltage_quadrature_tracking_en","1");
    iio_device_attr_write(phy,"in_voltage_rf_dc_offset_tracking_en","1");
    iio_device_attr_write(phy,"in_voltage_bb_dc_offset_tracking_en","1");
    /* read back the actual (quantised) sample rate for the emit-rate report */
    long long rfs=fs; if(prx) iio_channel_attr_read_longlong(prx,"sampling_frequency",&rfs);
    fprintf(stderr,"iqfeed: fs=%lld decim=%d emit=%lld sps gain=%.0f\n", rfs, D, rfs/D, g);

    struct iio_channel *ri = iio_device_find_channel(rxd,"voltage0",false);
    struct iio_channel *rq = iio_device_find_channel(rxd,"voltage1",false);
    if(!ri || !rq){ fprintf(stderr,"iqfeed: RX channels missing\n"); return 3; }
    iio_channel_enable(ri); iio_channel_enable(rq);
    /* true full scale for the level report: raw buffer holds (bits value)<<shift */
    int adc_fs = 2048;
    { const struct iio_data_format *df = iio_channel_get_data_format(ri);
      int b = df ? (int)df->bits + (int)df->shift : 12; if(b<9)b=9; if(b>16)b=16; adc_fs = 1<<(b-1); }
    struct iio_buffer *buf = iio_device_create_buffer(rxd,(size_t)FEED_N,false);
    if(!buf){ fprintf(stderr,"iqfeed: create_buffer failed\n"); return 4; }

    static int16_t out[FEED_N*2];
    long accI=0, accQ=0; int cnt=0;
    for(;;){
        ssize_t r=iio_buffer_refill(buf); if(r<0){ fprintf(stderr,"iqfeed: refill %zd\n",r); break; }
        ptrdiff_t inc=iio_buffer_step(buf);
        char *start=(char*)iio_buffer_first(buf,ri), *end=iio_buffer_end(buf), *p;
        /* raw level (pre-DC) + DC mean over the block */
        int pk=0; double acc=0; long long sI=0,sQ=0; int n=0;
        for(p=start; p<end; p+=inc){ int I=((int16_t*)p)[0], Q=((int16_t*)p)[1];
            int a=I<0?-I:I,b=Q<0?-Q:Q; if(a>pk)pk=a; if(b>pk)pk=b;
            acc+=(double)I*I+(double)Q*Q; sI+=I; sQ+=Q; n++; }
        if(n<1) continue;
        fprintf(stderr,"LVL %d %d\n",(int)(sqrt(acc/n)/adc_fs*100.0+0.5),(int)((double)pk/adc_fs*100.0+0.5)); fflush(stderr);
        int mI=(int)(sI/n), mQ=(int)(sQ/n), no=0;
        for(p=start; p<end; p+=inc){
            accI += ((int16_t*)p)[0]-mI; accQ += ((int16_t*)p)[1]-mQ;
            if(++cnt>=D){ out[2*no]=(int16_t)(accI/D); out[2*no+1]=(int16_t)(accQ/D); no++; accI=accQ=0; cnt=0; }
        }
        if(no){ if(fwrite(out,4,(size_t)no,stdout)!=(size_t)no) break; fflush(stdout); }
    }
    iio_buffer_destroy(buf); iio_channel_disable(ri); iio_channel_disable(rq);
    return 0;
}
