/*
 * pp_maiarx.h — raw RX I/Q for the front-panel apps via the maia-httpd recorder.
 *
 * WHY THIS EXISTS: the maia-sdr FPGA image on fishball7020 does NOT instantiate
 * the ADI RX capture DMA (`cf-ad9361-lpc` is absent — the IIO context has only
 * ad9361-phy, xadc and cf-ad9361-dds-core-lpc). maia keeps the ADC stream for its
 * own spectrometer/recorder, so libiio-based RX (pp_iq.h / pp_cap.h) has no device
 * and every raw-IQ app fails. The SUPPORTED way to get raw IQ is maia-httpd's
 * recorder: PATCH /api/recorder {state_change:Start,mode:IQ16bit,maximum_duration}
 * then GET /recording (note: /recording, NOT /api/recording). The body is pure
 * interleaved little-endian int16 I,Q from byte 0 (verified: no header/SigMF meta),
 * captured at the CURRENT sampling_frequency. maia clamps fs to the AD9361 minimum
 * (~2.083 Msps), so narrowband work decimates in software (mrx_decimate).
 *
 * This is capture-then-download, NOT a continuous stream: each mrx_capture() is one
 * recorder round-trip (~dur + tens of ms of HTTP). Fine for a scope, VNA-per-point
 * and best-effort decoder chunks; it is not a gapless live stream.
 *
 * Control (LO/fs/gain/bw) rides the same maia /api/ad9361 PATCH path as the
 * spectrum apps (pp_maia.h), so it stays consistent with maia's own state.
 *
 * A PP_SIM branch synthesises IQ so the apps stay runnable in the web simulator.
 */
#ifndef PP_MAIARX_H
#define PP_MAIARX_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pp_maia.h"          /* maia_connect / maia_patch / maia_http_req / maia_json_num */

#ifndef PP_SIM
#include <unistd.h>

/* current radio state as last configured (Hz / dB) — getters for the UI */
static long long mrx_lo_hz = 2400000000LL;
static long long mrx_fs_hz = 4000000LL;
static double    mrx_gain  = 30.0;

/* Configure the RX. gmode: "Manual" (uses gain_db), "SlowAttack" or "FastAttack".
 * maia 422s a combined {mode,gain} PATCH after an fs/bw change, so each attribute
 * is patched separately (same lesson as pp_sdr.c / pp_spectrum.c). */
static int mrx_config(long long lo_hz, long long fs_hz, double gain_db,
                      long long bw_hz, const char *gmode){
    char j[96];
    snprintf(j,sizeof j,"{\"sampling_frequency\":%lld}", fs_hz); maia_patch(j);
    snprintf(j,sizeof j,"{\"rx_rf_bandwidth\":%lld}", bw_hz>0?bw_hz:fs_hz); maia_patch(j);
    snprintf(j,sizeof j,"{\"rx_lo_frequency\":%lld}", lo_hz); if(!maia_patch(j)) return 0;
    if(!gmode || !strcmp(gmode,"Manual")){
        maia_patch("{\"rx_gain_mode\":\"Manual\"}");
        snprintf(j,sizeof j,"{\"rx_gain\":%.1f}", gain_db); maia_patch(j);
    } else {
        snprintf(j,sizeof j,"{\"rx_gain_mode\":\"%s\"}", gmode); maia_patch(j);
    }
    mrx_lo_hz=lo_hz; mrx_fs_hz=fs_hz; mrx_gain=gain_db;
    return 1;
}
/* read back the AD9361-quantised fs (maia clamps to hardware steps) */
static long long mrx_get_fs(void){
    long long lo=mrx_lo_hz,sp=mrx_fs_hz; double g=mrx_gain;
    if(maia_get_ad9361(&lo,&sp,&g)){ mrx_fs_hz=sp; mrx_gain=g; return sp; }
    return mrx_fs_hz;
}

/* Capture up to max_n complex samples over ~dur_s into iq[2*max_n] (I,Q int16).
 * Returns #complex samples, or -1 on failure. dur_s should cover the samples you
 * need (n/fs) plus a little; the recorder may hand back slightly more. */
static int mrx_capture(int16_t *iq, int max_n, double dur_s){
    if(max_n<1) return -1;
    if(dur_s<0.001) dur_s=0.001;
    char j[128];
    snprintf(j,sizeof j,"{\"state_change\":\"Start\",\"mode\":\"IQ16bit\",\"maximum_duration\":%.4f}", dur_s);
    char resp[512];
    if(maia_http_req("PATCH","/api/recorder",j,resp,sizeof resp,NULL) < 200) return -1;
    /* wait until the recorder reports Stopped (bounded: ~dur + 1s) */
    int waits = (int)(dur_s*1000/20) + 60;
    for(int i=0;i<waits;i++){
        usleep(20000);
        const char *b=NULL; char r[512];
        int code=maia_http_req("GET","/api/recorder",NULL,r,sizeof r,&b);
        if(code>=200 && b && strstr(b,"\"Stopped\"")) break;
    }
    /* download /recording (Connection: close -> read to EOF) straight into iq */
    int fd=maia_connect(); if(fd<0) return -1;
    const char *req="GET /recording HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if(write(fd,req,strlen(req))<0){ close(fd); return -1; }
    /* skip HTTP headers */
    char h[1024]; int hn=0, hdr_done=0;
    while(hn<(int)sizeof h-1 && !hdr_done){
        char c; long r=read(fd,&c,1); if(r<=0) break; h[hn++]=c;
        if(hn>=4 && h[hn-4]=='\r'&&h[hn-3]=='\n'&&h[hn-2]=='\r'&&h[hn-1]=='\n') hdr_done=1;
    }
    if(!hdr_done){ close(fd); return -1; }
    /* body -> iq buffer, capped at max_n*4 bytes */
    size_t want=(size_t)max_n*4, got=0;
    char *dst=(char*)iq;
    while(got<want){
        long r=read(fd,dst+got,want-got);
        if(r<=0) break;
        got+=(size_t)r;
    }
    /* drain any tail so the socket closes cleanly */
    char t[4096]; while(read(fd,t,sizeof t)>0){}
    close(fd);
    return (int)(got/4);
}

#else /* ---------------------- PP_SIM: synthetic IQ ---------------------- */
#include <math.h>
static long long mrx_lo_hz=433920000LL, mrx_fs_hz=4000000LL; static double mrx_gain=30.0;
static int mrx_config(long long lo,long long fs,double g,long long bw,const char*gm){
    (void)bw;(void)gm; mrx_lo_hz=lo; mrx_fs_hz=fs>0?fs:1; mrx_gain=g; return 1; }
static long long mrx_get_fs(void){ return mrx_fs_hz; }
static int mrx_capture(int16_t *iq,int max_n,double dur_s){
    (void)dur_s;
    static double ph[3]; static const long long SIG[3]={433920000LL,868000000LL,2402000000LL};
    static const double AMP[3]={1500.0,700.0,1000.0};
    double g=pow(10.0,(mrx_gain-30.0)/20.0);
    for(int i=0;i<max_n;i++){
        double I=((rand()&0xff)/255.0-0.5)*80.0, Q=((rand()&0xff)/255.0-0.5)*80.0;
        for(int s=0;s<3;s++){ double off=(double)(SIG[s]-mrx_lo_hz);
            if(fabs(off)<mrx_fs_hz*0.48){ ph[s]+=2.0*M_PI*off/(double)mrx_fs_hz;
                double a=AMP[s]*(0.6+0.4*sin(i*0.0007+s)); I+=g*a*cos(ph[s]); Q+=g*a*sin(ph[s]); } }
        iq[2*i]=(int16_t)I; iq[2*i+1]=(int16_t)Q;
    }
    return max_n;
}
#endif

/* ---- shared helper: integer boxcar decimation (fs -> fs/dec), in place-safe ----
 * averages `dec` complex samples per output; returns #output complex samples. */
static int mrx_decimate(const int16_t *in, int nin, int dec, int16_t *out){
    if(dec<1) dec=1;
    int no=nin/dec, k=0;
    for(int o=0;o<no;o++){
        long si=0,sq=0;
        for(int d=0;d<dec;d++){ si+=in[2*k]; sq+=in[2*k+1]; k++; }
        out[2*o]=(int16_t)(si/dec); out[2*o+1]=(int16_t)(sq/dec);
    }
    return no;
}
#endif
