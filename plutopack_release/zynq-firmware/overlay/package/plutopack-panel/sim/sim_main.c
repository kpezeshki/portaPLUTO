/*
 * sim_main.c — PlutoPack front-panel simulator backend + web server.
 *
 * Runs the REAL app sources (pp_launcher.c / pp_sdr.c / pp_test.c) compiled with
 * -DPP_SIM, so what the browser shows is exactly what pp_ui.h / pp_screen.h draw
 * on the panel. The PP_SIM seam in pp_common.h routes:
 *    - framebuffer writes  -> sim_fb_write()  (an in-memory 320x240 panel)
 *    - Pico status polls    -> sim_input_get() (web-injected buttons + encoder)
 * and pp_maia.h's PP_SIM branch feeds a synthetic spectrum + radio model.
 *
 * A tiny embedded HTTP server serves the control page, the live framebuffer
 * (raw RGBA), and input events. No external dependencies.
 *
 *   usage: ppsim [port=8080]      then open http://localhost:8080
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "pp_common.h"      /* PP_W/PP_H, PP_BTN_*, pp_status_t, pp_rgb */
#pragma GCC diagnostic ignored "-Wunused-function"   /* pp_common.h statics unused here */

/* ------- app entry points (each app's main, renamed via -Dmain=..._main) ---- */
extern int pp_launcher_main(int, char**);
extern int pp_sdr_main(int, char**);
extern int pp_test_main(int, char**);
extern int pp_tonegen_main(int, char**);
extern int pp_spectrum_main(int, char**);
extern int pp_sna_main(int, char**);
extern int pp_scope_main(int, char**);
extern int pp_iqrec_main(int, char**);
extern int pp_rtl433_main(int, char**);
extern int pp_tetris_main(int, char**);
extern int pp_acks_main(int, char**);
extern int pp_adsb_main(int, char**);
extern int pp_vna_main(int, char**);
extern int pp_twotone_main(int, char**);
extern int pp_ampgain_main(int, char**);
extern int pp_atten_main(int, char**);
extern int pp_manual_main(int, char**);
extern int pp_term_main(int, char**);
extern int pp_rficstat_main(int, char**);
extern int pp_touchcal_main(int, char**);

/* ------------------------------ shared state ------------------------------- */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static uint16_t panel[PP_W*PP_H];        /* the displayed 565 framebuffer */
static unsigned frame_ver = 1;           /* bumps on every change (for /fb?v=) */
static uint8_t  btn_held = 0;            /* buttons held down (mouse/key down) */
static uint8_t  btn_pulse = 0;           /* one-shot taps, cleared once polled */
static int32_t  enc_pos = 0;             /* encoder count (4 per detent)        */
static uint8_t  touch_down = 0;          /* 1 while the display image is pressed */
static int      touch_sx = 0, touch_sy = 0;  /* last touch pixel (0..319, 0..239) */

/* called from pp_common.h (PP_SIM) */
void sim_fb_write(const uint16_t *buf, int y0, int y1){
    pthread_mutex_lock(&lk);
    memcpy(&panel[y0*PP_W], &buf[y0*PP_W], (size_t)(y1-y0)*PP_W*sizeof(uint16_t));
    frame_ver++;
    pthread_mutex_unlock(&lk);
}
void sim_input_get(uint8_t *btn, int32_t *enc){
    pthread_mutex_lock(&lk);
    *btn = btn_held | btn_pulse; *enc = enc_pos; btn_pulse = 0;   /* consume taps */
    pthread_mutex_unlock(&lk);
}
/* called from pp_common.h (PP_SIM): current touch as a raw 12-bit sample. The
 * screen pixel is inverse-mapped through the same linear scale pp_touch_map()
 * uses when no cal file is present, so a click at pixel (sx,sy) lands there. */
void sim_touch_get(uint8_t *down, int *rawx, int *rawy){
    pthread_mutex_lock(&lk);
    *down = touch_down;
    *rawx = touch_sx * 4096 / PP_W;
    *rawy = touch_sy * 4096 / PP_H;
    pthread_mutex_unlock(&lk);
}

/* called from pp_launcher.c (PP_SIM): run an app in-process, return on its BTN7 */
int sim_run_app(const char *path, const char *spidev, const char *fbdev){
    char *argv[4] = { (char*)path, (char*)spidev, (char*)fbdev, 0 };
    if(strstr(path,"pp-sdr"))      return pp_sdr_main(3, argv);
    if(strstr(path,"pp-tonegen"))  return pp_tonegen_main(3, argv);
    if(strstr(path,"pp-spectrum")) return pp_spectrum_main(3, argv);
    if(strstr(path,"pp-sna"))      return pp_sna_main(3, argv);
    if(strstr(path,"pp-scope"))    return pp_scope_main(3, argv);
    if(strstr(path,"pp-iqrec"))    return pp_iqrec_main(3, argv);
    if(strstr(path,"pp-rtl433"))   return pp_rtl433_main(3, argv);
    if(strstr(path,"pp-adsb"))     return pp_adsb_main(3, argv);
    if(strstr(path,"pp-vna"))      return pp_vna_main(3, argv);
    if(strstr(path,"pp-twotone"))  return pp_twotone_main(3, argv);
    if(strstr(path,"pp-ampgain"))  return pp_ampgain_main(3, argv);
    if(strstr(path,"pp-atten"))    return pp_atten_main(3, argv);
    if(strstr(path,"pp-manual"))   return pp_manual_main(3, argv);
    if(strstr(path,"pp-term"))     return pp_term_main(3, argv);
    if(strstr(path,"pp-rficstat")) return pp_rficstat_main(3, argv);
    if(strstr(path,"pp-touchcal")) return pp_touchcal_main(3, argv);
    if(strstr(path,"pp-tetris"))   return pp_tetris_main(3, argv);
    if(strstr(path,"pp-acks"))     return pp_acks_main(3, argv);
    if(strstr(path,"pp-test"))     return pp_test_main(3, argv);
    return 0;
}

/* ------------------------------ input routing ------------------------------ */
static uint8_t bit_of(const char *name){
    if(!strcmp(name,"UP"))     return PP_BTN_UP;
    if(!strcmp(name,"DOWN"))   return PP_BTN_DOWN;
    if(!strcmp(name,"LEFT"))   return PP_BTN_LEFT;
    if(!strcmp(name,"RIGHT"))  return PP_BTN_RIGHT;
    if(!strcmp(name,"CENTER")) return PP_BTN_CENTER;
    if(!strcmp(name,"B7"))     return PP_BTN_7;
    if(!strcmp(name,"B8"))     return PP_BTN_8;
    return 0;
}
static void input_apply(const char *query){    /* e.g. "b=UP" or "e=1" or "bd=UP" */
    char key[16]={0}, val[16]={0};
    const char *eq=strchr(query,'='); if(!eq) return;
    int kl=(int)(eq-query); if(kl>15)kl=15; memcpy(key,query,kl);
    int i=0; for(const char*p=eq+1; *p && *p!='&' && i<15; p++) val[i++]=*p;
    pthread_mutex_lock(&lk);
    if(!strcmp(key,"b"))  btn_pulse |= bit_of(val);            /* tap */
    else if(!strcmp(key,"bd")) btn_held |= bit_of(val);       /* hold down */
    else if(!strcmp(key,"bu")) btn_held &= ~bit_of(val);      /* release */
    else if(!strcmp(key,"e"))  enc_pos += 4*atoi(val);        /* one detent = 4 */
    else if(!strcmp(key,"t")){                                /* touch down/move: "sx,sy" */
        int sx=0, sy=0; char *comma=strchr(val,',');
        if(comma){ sx=atoi(val); sy=atoi(comma+1);
            if(sx<0)sx=0; if(sx>PP_W-1)sx=PP_W-1;
            if(sy<0)sy=0; if(sy>PP_H-1)sy=PP_H-1;
            touch_sx=sx; touch_sy=sy; touch_down=1; }
    }
    else if(!strcmp(key,"tu")) touch_down=0;                  /* touch release */
    pthread_mutex_unlock(&lk);
}

/* ------------------------------- web page ---------------------------------- */
static const char *PAGE =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>PlutoPack Sim</title><style>"
"*{-webkit-tap-highlight-color:transparent;box-sizing:border-box}"
"html,body{overscroll-behavior:none}"
"body{background:#0c0e14;color:#c7ccd8;font-family:system-ui,sans-serif;text-align:center;"
"margin:12px;-webkit-user-select:none;user-select:none;touch-action:manipulation}"
"canvas{image-rendering:pixelated;border:3px solid #222;background:#000;cursor:crosshair;"
"width:min(640px,96vw);height:auto;aspect-ratio:4/3}"
".pad{display:inline-grid;grid-template-columns:repeat(3,72px);gap:10px;margin:14px;vertical-align:top}"
".side{display:inline-block;margin:14px;vertical-align:top;text-align:left}"
"button{background:#1a1f2b;color:#e6e9f0;border:1px solid #33405a;border-radius:12px;"
"padding:18px;font-size:20px;cursor:pointer;min-width:64px;"
"touch-action:manipulation;-webkit-user-select:none;user-select:none}"
"button:active{background:#369eff;color:#0c0e14}.sp{visibility:hidden;pointer-events:none}"
".side button{min-width:160px;margin:5px 0;display:block}.enc button{display:inline-block;min-width:80px}"
"h2{font-weight:600;font-size:18px}.k{color:#5a6376;font-size:12px}</style></head><body>"
"<h2>PlutoPack Front-Panel Simulator</h2>"
"<canvas id=c width=320 height=240></canvas><div>"
"<div class=pad>"
"<button class=sp>.</button><button data-b=UP>&#9650;</button><button class=sp>.</button>"
"<button data-b=LEFT>&#9668;</button><button data-b=CENTER>&#9679;</button><button data-b=RIGHT>&#9658;</button>"
"<button class=sp>.</button><button data-b=DOWN>&#9660;</button><button class=sp>.</button></div>"
"<div class=side>"
"<button data-b=B7>BTN7 &nbsp;back</button>"
"<button data-b=B8>BTN8 &nbsp;settings</button>"
"<div class=enc style='margin-top:10px'>Encoder&nbsp; "
"<button data-e=-1>&#8635; &minus;</button><button data-e=1>+ &#8634;</button></div>"
"</div></div>"
"<p class=k>click / drag the screen = touch &middot; arrows = D-pad &middot; Enter = center &middot; 7/8 = buttons &middot; , / . = encoder</p>"
"<script>"
"var cv=document.getElementById('c'),cx=cv.getContext('2d'),img=cx.createImageData(320,240),ver=0;"
/* self-clocking: fetch the next frame only after the last arrives (no pileup);"
 " 204 = unchanged, so idle/menu screens cost almost nothing */
"async function loop(){var d=15;try{var r=await fetch('/fb?v='+ver);"
"var h=r.headers.get('X-Ver');if(h)ver=parseInt(h);"
"if(r.status===200){var u=new Uint8Array(await r.arrayBuffer());"
"if(u.length===320*240*2){var o=img.data;"
"for(var i=0,j=0;i<u.length;i+=2,j+=4){var p=u[i]|(u[i+1]<<8);"
"o[j]=((p>>11)&31)*527+23>>6;o[j+1]=((p>>5)&63)*259+33>>6;o[j+2]=(p&31)*527+23>>6;o[j+3]=255;}"
"cx.putImageData(img,0,0);}}else{d=60;}"
"}catch(e){d=250;}setTimeout(loop,d);}"
"loop();"
"function send(u){fetch(u).catch(function(){});}"
"function url(el){return el.dataset.b?('/in?b='+el.dataset.b):('/in?e='+el.dataset.e);}"
"function rep(el){return !!el.dataset.e||['UP','DOWN','LEFT','RIGHT'].indexOf(el.dataset.b)>=0;}"
"document.querySelectorAll('button').forEach(function(el){"
"if(el.classList.contains('sp'))return;var t=null,u=url(el);"
"function start(e){e.preventDefault();send(u);if(rep(el))t=setInterval(function(){send(u);},170);}"
"function stop(){if(t){clearInterval(t);t=null;}}"
"el.addEventListener('touchstart',start,{passive:false});"
"el.addEventListener('touchend',stop);el.addEventListener('touchcancel',stop);"
"el.addEventListener('mousedown',start);el.addEventListener('mouseup',stop);"
"el.addEventListener('mouseleave',stop);"
"el.addEventListener('click',function(e){e.preventDefault();});"
"});"
/* click / drag anywhere on the display image = a touch at that panel pixel */
"var tdn=false;"
"function tpx(e){var r=cv.getBoundingClientRect();"
"var cx=(e.touches?e.touches[0].clientX:e.clientX)-r.left;"
"var cy=(e.touches?e.touches[0].clientY:e.clientY)-r.top;"
"var sx=Math.round(cx*320/r.width),sy=Math.round(cy*240/r.height);"
"if(sx<0)sx=0;if(sx>319)sx=319;if(sy<0)sy=0;if(sy>239)sy=239;return sx+','+sy;}"
"function tstart(e){e.preventDefault();tdn=true;send('/in?t='+tpx(e));}"
"function tmove(e){if(!tdn)return;e.preventDefault();send('/in?t='+tpx(e));}"
"function tend(e){if(!tdn)return;e.preventDefault();tdn=false;send('/in?tu=1');}"
"cv.addEventListener('mousedown',tstart);"
"addEventListener('mousemove',tmove);addEventListener('mouseup',tend);"
"cv.addEventListener('touchstart',tstart,{passive:false});"
"cv.addEventListener('touchmove',tmove,{passive:false});"
"cv.addEventListener('touchend',tend);cv.addEventListener('touchcancel',tend);"
"var km={ArrowUp:'b=UP',ArrowDown:'b=DOWN',ArrowLeft:'b=LEFT',ArrowRight:'b=RIGHT',"
"Enter:'b=CENTER','7':'b=B7','8':'b=B8',',':'e=-1','.':'e=1'};"
"addEventListener('keydown',function(e){var m=km[e.key];if(m){e.preventDefault();send('/in?'+m);}});"
"</script></body></html>";

/* ------------------------------- http server ------------------------------- */
static void send_all(int c, const char *p, int n){
    while(n>0){ int w=write(c,p,n); if(w<=0) break; p+=w; n-=w; }
}
static void send_html(int c){
    char h[128]; int bl=(int)strlen(PAGE);
    int hl=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type:text/html\r\n"
                    "Content-Length:%d\r\nConnection:close\r\n\r\n",bl);
    send_all(c,h,hl); send_all(c,PAGE,bl);
}
/* send the framebuffer as raw RGB565 (little-endian), or 204 if unchanged since
 * the client's last-seen version `cv` — so static screens cost ~nothing and the
 * client only re-draws on a real change. */
static void send_fb(int c, unsigned cv){
    static __thread uint8_t px[PP_W*PP_H*2];   /* thread-local: server is multi-threaded */
    pthread_mutex_lock(&lk);
    unsigned ver=frame_ver;
    if(cv==ver){ pthread_mutex_unlock(&lk);
        char h[96]; int hl=snprintf(h,sizeof h,
            "HTTP/1.1 204 No Content\r\nX-Ver:%u\r\nContent-Length:0\r\nConnection:close\r\n\r\n",ver);
        send_all(c,h,hl); return;
    }
    for(int i=0;i<PP_W*PP_H;i++){ uint16_t p=panel[i]; px[i*2]=p&0xff; px[i*2+1]=p>>8; }
    pthread_mutex_unlock(&lk);
    char h[160]; int bl=PP_W*PP_H*2;
    int hl=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Type:application/octet-stream\r\n"
                    "X-Ver:%u\r\nContent-Length:%d\r\nCache-Control:no-store\r\nConnection:close\r\n\r\n",ver,bl);
    send_all(c,h,hl); send_all(c,(char*)px,bl);
}
static void send_ok(int c){
    const char *r="HTTP/1.1 200 OK\r\nContent-Length:2\r\nConnection:close\r\n\r\nok";
    send_all(c,r,(int)strlen(r));
}
static void handle(int c){
    char req[4096]; int n=(int)read(c,req,sizeof req-1);
    if(n<=0){ close(c); return; }
    req[n]=0;
    char method[8]={0}, url[1024]={0};
    if(sscanf(req,"%7s %1023s",method,url)!=2){ close(c); return; }
    if(!strncmp(url,"/fb",3)){
        unsigned cv=0; char *q=strchr(url,'?');                 /* /fb?v=<lastseen> */
        if(q){ char *vp=strstr(q,"v="); if(vp) cv=(unsigned)strtoul(vp+2,0,10); }
        send_fb(c,cv);
    }
    else if(!strncmp(url,"/in",3)){ char *q=strchr(url,'?'); if(q){ q++;   /* split &-tokens */
                                        char *sp=0, *tok=strtok_r(q,"&",&sp);
                                        while(tok){ input_apply(tok); tok=strtok_r(0,"&",&sp); } }
                                    send_ok(c); }
    else                            send_html(c);
    close(c);
}
static int listen_port;
static void *conn_thread(void *arg){ handle((int)(intptr_t)arg); return 0; }
static void *server_thread(void *arg){
    (void)arg;
    int s=socket(AF_INET,SOCK_STREAM,0);
    int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY); a.sin_port=htons(listen_port);
    if(bind(s,(struct sockaddr*)&a,sizeof a)<0){ perror("bind"); exit(1); }
    listen(s,64);
    for(;;){ int c=accept(s,0,0); if(c<0) continue;   /* one thread per connection */
        pthread_t t;                                  /* so a slow /fb never blocks /in */
        if(pthread_create(&t,0,conn_thread,(void*)(intptr_t)c)==0) pthread_detach(t);
        else handle(c);
    }
    return 0;
}

int main(int argc, char **argv){
    listen_port = (argc>1)?atoi(argv[1]):8080;
    for(int i=0;i<PP_W*PP_H;i++) panel[i]=pp_rgb(0x0c,0x0e,0x14);
    pthread_t th; pthread_create(&th,0,server_thread,0);
    printf("PlutoPack simulator on http://localhost:%d\n", listen_port);
    fflush(stdout);
    char *av[3] = { (char*)"ppsim", (char*)"/sim/spi", (char*)"/sim/fb" };
    return pp_launcher_main(3, av);        /* boot into the app switcher */
}
