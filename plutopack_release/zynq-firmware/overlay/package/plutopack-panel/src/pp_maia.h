/*
 * pp_maia.h — talk to the local maia-httpd (127.0.0.1:80, plain HTTP, no auth).
 *   - /waterfall websocket  : real power spectrum (4096 little-endian f32/frame,
 *                             bin 0 = seq tag, bins 1..4095 = scaled linear power)
 *   - GET/PATCH /api/ad9361 : read/set RX LO frequency, sample rate (span), gain
 * Header-only, no external deps. (Validated against maia-httpd 0.6.0 / maia-hdl 0.6.1.)
 */
#ifndef PP_MAIA_H
#define PP_MAIA_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/select.h>

#define MAIA_FFT 4096

static int maia_port = 80;

/* pull a numeric JSON field like "key":123.4 -> double (shared) */
static int maia_json_num(const char*s,const char*key,double*out){
    if(!s) return 0;
    char pat[48]; snprintf(pat,sizeof pat,"\"%s\":",key);
    const char*p=strstr(s,pat); if(!p) return 0;
    *out=strtod(p+strlen(pat),NULL); return 1;
}

#ifndef PP_SIM
static int maia_connect(void){
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(maia_port);
    inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
    if(connect(fd,(struct sockaddr*)&a,sizeof a)<0){ close(fd); return -1; }
    struct timeval tv={2,0};    /* never let a hung maia freeze the UI loop */
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    return fd;
}
static int maia_rdn(int fd,void*b,unsigned long n){
    unsigned long g=0; while(g<n){ long r=read(fd,(char*)b+g,n-g); if(r<=0) return -1; g+=r; } return 0;
}

/* ---- /waterfall websocket ---- */
static int maia_ws_open(void){
    int fd=maia_connect(); if(fd<0) return -1;
    const char*req="GET /waterfall HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n";
    if(write(fd,req,strlen(req))<0){ close(fd); return -1; }
    char h[2048]; int n=0;
    while(n<(int)sizeof h-1){ char c; long r=read(fd,&c,1); if(r<=0) break; h[n++]=c;
        if(n>=4&&h[n-4]=='\r'&&h[n-3]=='\n'&&h[n-2]=='\r'&&h[n-1]=='\n') break; }
    h[n]=0;
    if(!strstr(h,"101")){ close(fd); return -1; }
    struct timeval tv={1,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);  /* don't hang mid-frame */
    return fd;
}
/* read one binary frame into out[MAIA_FFT]; returns #floats or -1 */
static int maia_ws_frame(int fd, float *out){
    for(;;){
        unsigned char b[2]; if(maia_rdn(fd,b,2)) return -1;
        int op=b[0]&0xf; unsigned long len=b[1]&0x7f; int mask=b[1]&0x80;
        if(len==126){ unsigned char e[2]; if(maia_rdn(fd,e,2))return -1; len=((unsigned long)e[0]<<8)|e[1]; }
        else if(len==127){ unsigned char e[8]; if(maia_rdn(fd,e,8))return -1; len=0; for(int i=0;i<8;i++) len=(len<<8)|e[i]; }
        if(mask){ unsigned char m[4]; if(maia_rdn(fd,m,4))return -1; }
        if(op==0x8) return -1;                       /* close */
        if(op!=0x2){ char t[512]; unsigned long left=len;    /* skip ping/pong/text */
                     while(left){ unsigned long c=left>512?512:left; if(maia_rdn(fd,t,c))return -1; left-=c; } continue; }
        unsigned long nb = len; if(nb > MAIA_FFT*4UL) nb = MAIA_FFT*4UL;
        if(maia_rdn(fd,out,nb)) return -1;
        if(len>nb){ char t[512]; unsigned long left=len-nb;   /* discard any tail */
                    while(left){ unsigned long c=left>512?512:left; if(maia_rdn(fd,t,c))return -1; left-=c; } }
        return (int)(nb/4);
    }
}
/* drain all buffered frames, keep the newest in out[]; returns 1 if a frame was read */
static int maia_ws_drain(int fd, float *out){
    int got=0;
    for(;;){
        fd_set r; FD_ZERO(&r); FD_SET(fd,&r);
        struct timeval z={0,0};
        if(select(fd+1,&r,0,0,&z)<=0) break;
        if(maia_ws_frame(fd,out)<0) return got ? got : -1;
        got=1;
    }
    return got;
}

/* ---- GET/PATCH /api/ad9361 ---- */
/* One HTTP request. Returns the HTTP status code (e.g. 200), or <0 on a
 * transport failure (no connection / no reply). *out_body, if given, points
 * into resp[] at the start of the response body. */
static int maia_http_req(const char*method,const char*path,const char*body,
                         char*resp,int rn,const char**out_body){
    if(out_body) *out_body=resp;
    int fd=maia_connect(); if(fd<0) return -1;
    char req[640];
    int bl = body?(int)strlen(body):0;
    if(body) snprintf(req,sizeof req,"%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                     "Content-Length: %d\r\nConnection: close\r\n\r\n%s",method,path,bl,body);
    else     snprintf(req,sizeof req,"%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",method,path);
    if(write(fd,req,strlen(req))<0){ close(fd); return -1; }
    int n=0; while(n<rn-1){ long r=read(fd,resp+n,rn-1-n); if(r<=0) break; n+=r; }
    resp[n]=0; close(fd);
    if(n<=0) return -1;                                   /* timed out / empty */
    if(out_body){ char*b=strstr(resp,"\r\n\r\n"); *out_body=b?b+4:resp; }
    if(strncmp(resp,"HTTP/1.",7)) return -1;              /* not a valid reply */
    return (int)strtol(resp+9,NULL,10);                  /* "HTTP/1.1 <code> " */
}
/* legacy body-returning helper (callers that don't care about the status) */
static const char* maia_http(const char*method,const char*path,const char*body,char*resp,int rn){
    const char*b=NULL; maia_http_req(method,path,body,resp,rn,&b); return b;
}
/* read current radio state (Hz, Hz, dB); returns 1 only on a 2xx with a body */
static int maia_get_ad9361(long long*lo_hz,long long*span_hz,double*gain_db){
    char resp[1024]; const char*body=NULL;
    int code=maia_http_req("GET","/api/ad9361",NULL,resp,sizeof resp,&body);
    if(code<200||code>=300||!body) return 0;
    double v;
    if(maia_json_num(body,"rx_lo_frequency",&v)) *lo_hz=(long long)v;    /* Hz > 2^31: needs 64-bit */
    if(maia_json_num(body,"sampling_frequency",&v)) *span_hz=(long long)v;
    if(maia_json_num(body,"rx_gain",&v)) *gain_db=v;
    return 1;
}
/* apply a setting; returns 1 only if maia accepted it (HTTP 2xx) */
static int maia_patch(const char*json){
    char resp[512]; int code=maia_http_req("PATCH","/api/ad9361",json,resp,sizeof resp,NULL);
    return (code>=200&&code<300);
}
/* PATCH /api/spectrometer (e.g. {"number_integrations":N}). The waterfall frame
 * rate = input_sampling_frequency/(fft_size*number_integrations) = rbw_hz/N, so N
 * trades frame rate against per-frame averaging. */
static int maia_patch_spec(const char*json){
    char resp[512]; int code=maia_http_req("PATCH","/api/spectrometer",json,resp,sizeof resp,NULL);
    return (code>=200&&code<300);
}
/* apply a setting, then read the actual applied state back (authoritative).
 * Returns 1 only if both the PATCH and the read-back succeed. */
static int maia_patch_get(const char*json,long long*lo_hz,long long*span_hz,double*gain_db){
    if(!maia_patch(json)) return 0;
    return maia_get_ad9361(lo_hz,span_hz,gain_db);
}
#else  /* PP_SIM: synthetic radio model + spectrum, no sockets */
#include <math.h>
static struct { long long lo_hz, span_hz; double gain; } smodel = { 2442000000LL, 20000000LL, 40.0 };
static int maia_get_ad9361(long long*lo_hz,long long*span_hz,double*gain_db){
    *lo_hz=smodel.lo_hz; *span_hz=smodel.span_hz; *gain_db=smodel.gain; return 1;
}
static int maia_patch(const char*json){
    double v;
    if(maia_json_num(json,"rx_lo_frequency",&v))    smodel.lo_hz=(long long)v;
    if(maia_json_num(json,"sampling_frequency",&v)){ long long q=100000LL, hz=(long long)v;  /* quantize 100kHz */
        smodel.span_hz = hz>=q ? (hz/q)*q : hz; }
    if(maia_json_num(json,"rx_gain",&v))            smodel.gain=(double)((long)(v+0.5));  /* integer dB */
    return 1;
}
static int maia_patch_get(const char*json,long long*lo_hz,long long*span_hz,double*gain_db){
    if(!maia_patch(json)) return 0; return maia_get_ad9361(lo_hz,span_hz,gain_db);
}
static int maia_patch_spec(const char*json){ (void)json; return 1; }
static int maia_ws_open(void){ return 999; }         /* dummy fd */
static int maia_ws_drain(int fd, float *out){         /* one synthetic frame/call */
    (void)fd; static unsigned t; t++;
    double span=(double)smodel.span_hz; if(span<1) span=1;
    for(int i=0;i<MAIA_FFT;i++){                        /* linear-power noise floor */
        double n = 1e-6*(0.6 + 0.8*((rand()&0x3ff)/1023.0)); out[i]=(float)n; }
    for(int k=-30;k<=30;k++){ int idx=((k)%MAIA_FFT+MAIA_FFT)%MAIA_FFT;   /* LO leakage at DC */
        out[idx]+=(float)(4e-3*exp(-(double)k*k/120.0)); }
    /* fixed absolute-frequency signals -> sweeping/tuning reveals them in place */
    static const long   SIG[3] = { 2400000000L, 2437000000L, 2480000000L };
    static const double AMP[3] = { 5e-3, 9e-3, 3e-3 };
    for(int s=0;s<3;s++){ long off=SIG[s]-smodel.lo_hz;
        if(off>0 && (double)off < span*0.49){
            int i=(int)((double)off*MAIA_FFT/span);
            if(i>2 && i<2047) for(int k=-6;k<=6;k++){ int b=i+k;
                if(b>0 && b<2048) out[b]+=(float)(AMP[s]*exp(-(double)k*k/8.0)); } } }
    double g=pow(10.0,(smodel.gain-40.0)/20.0);        /* gain -> brightness */
    for(int i=0;i<MAIA_FFT;i++) out[i]*=(float)g;
    out[1]=out[2];
    return 1;
}
#endif /* PP_SIM */
#endif
