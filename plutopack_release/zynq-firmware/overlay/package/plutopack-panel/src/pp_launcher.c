/*
 * pp-launcher — PlutoPack front-panel app picker (the boot screen / root).
 *   UP/DOWN or ENCODER : move the selection (wrapping)
 *   CENTER             : launch the selected app
 * The launched app returns here when the user presses BTN7 at its DATA screen;
 * the picker forks/execs the app, waits, then redraws. BTN7/BTN8 are no-ops at
 * the root. Add apps to APPS[]. See the UI style spec.
 *
 * usage: pp-launcher [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <sys/wait.h>
#include <stdlib.h>
#include "pp_common.h"

enum { IC_WATERFALL, IC_TONE, IC_SPECTRUM, IC_TEST, IC_SNA, IC_SCOPE, IC_IQREC, IC_RTL433, IC_ADSB,
       IC_VNA, IC_TWOTONE, IC_AMPGAIN, IC_ATTEN, IC_MANUAL, IC_TETRIS, IC_RFIC, IC_ACKS, IC_TCAL, IC_TERM };
typedef struct { const char *label, *path; int icon, exp; } app_t;   /* exp=1 -> "EXP" badge */
static const app_t APPS[] = {
    /* stable apps first */
    { "WATERFALL", "/usr/bin/pp-sdr",      IC_WATERFALL, 0 },
    { "TONEGEN",   "/usr/bin/pp-tonegen",  IC_TONE,      0 },
    { "SPECTRUM",  "/usr/bin/pp-spectrum", IC_SPECTRUM,  0 },
    { "SNA",       "/usr/bin/pp-sna",      IC_SNA,       0 },
    { "ATTEN",     "/usr/bin/pp-atten",    IC_ATTEN,     0 },
    { "SCOPE",     "/usr/bin/pp-scope",    IC_SCOPE,     0 },
    { "RTL_433",   "/usr/bin/pp-rtl433",   IC_RTL433,    0 },
    { "ADS-B",     "/usr/bin/pp-adsb",     IC_ADSB,      0 },
    { "RFIC STAT", "/usr/bin/pp-rficstat", IC_RFIC,      0 },
    { "TERMINAL",  "/usr/bin/pp-term",     IC_TERM,      0 },
    { "FRONTEND STAT","/usr/bin/pp-test",  IC_TEST,      0 },
    { "TOUCH CAL", "/usr/bin/pp-touchcal", IC_TCAL,      0 },
    { "TETRIS",    "/usr/bin/pp-tetris",   IC_TETRIS,    0 },
    { "MANUAL",    "/usr/bin/pp-manual",   IC_MANUAL,    0 },
    { "CREDITS",   "/usr/bin/pp-acks",     IC_ACKS,      0 },
    /* experimental apps pushed to the bottom */
    { "VNA",       "/usr/bin/pp-vna",      IC_VNA,       1 },
    { "TWO-TONE",  "/usr/bin/pp-twotone",  IC_TWOTONE,   1 },
    { "AMP GAIN",  "/usr/bin/pp-ampgain",  IC_AMPGAIN,   1 },
    { "IQ REC",    "/usr/bin/pp-iqrec",    IC_IQREC,     1 },
};
#define NAPP ((int)(sizeof APPS / sizeof APPS[0]))
#define VIS  4                       /* apps visible at once; list scrolls beyond */

/* ~1.5-cycle sine + quarter-shifted cosine for the tone icon (no libm dependency) */
static const signed char SINE24[24]={0,3,5,7,8,7,5,3,0,-3,-5,-7,-8,-7,-5,-3,0,3,5,7,8,7,5,3};
static const signed char COS24[24] ={8,7,5,3,0,-3,-5,-7,-8,-7,-5,-3,0,3,5,7,8,7,5,3,0,-3,-5,-7};

/* draw a 24x24 app icon at (ix,iy) */
static void draw_icon(uint16_t *buf, int ix, int iy, int icon, int sel){
    (void)sel;
    if(icon==IC_WATERFALL){                             /* stacked heat rows */
        uint16_t pal[6]={ pp_rgb(0x30,0x40,0xff), pp_rgb(0x20,0xc0,0xff), pp_rgb(0x30,0xd0,0x60),
                          pp_rgb(0xf0,0xd0,0x30), pp_rgb(0xff,0x80,0x20), pp_rgb(0xff,0x30,0x30) };
        for(int r=0;r<6;r++) pp_fill(buf, ix, iy+r*4, 24, 3, pal[r]);
    } else if(icon==IC_TONE){                           /* I/Q tone: two colored waves */
        for(int x=0;x<24;x++){
            pp_fill(buf, ix+x, iy+11+SINE24[x], 1,2, pp_rgb(0xff,0x7a,0x2f));   /* I: orange */
            pp_fill(buf, ix+x, iy+11+COS24[x],  1,2, pp_rgb(0x20,0xc0,0xff));   /* Q: cyan   */
        }
    } else if(icon==IC_SPECTRUM){                       /* Lorentzian peak, yellow */
        uint16_t y6=pp_rgb(0xff,0xd0,0x20); int prev=-1;
        for(int x=0;x<24;x++){
            double t=(x-11.5)/3.5, L=1.0/(1.0+t*t);
            int yy=iy+22-(int)(20.0*L+0.5);
            if(prev>=0){ int lo=yy<prev?yy:prev, hi=yy<prev?prev:yy; pp_fill(buf,ix+x,lo,1,hi-lo+1,y6); }
            else pp_fill(buf,ix+x,yy,1,2,y6);
            prev=yy;
        }
    } else if(icon==IC_TERM){                           /* console: framed screen, green '>' prompt */
        uint16_t fr=C_DIM, gr=pp_rgb(0x36,0xd3,0x99);
        pp_rect(buf, ix+1, iy+3, 22, 18, fr);
        pp_rect(buf, ix+2, iy+4, 20, 16, fr);
        for(int k=0;k<4;k++){ pp_fill(buf, ix+5+k, iy+8+k,  2,2, gr);    /* '>' upper arm */
                              pp_fill(buf, ix+5+k, iy+16-k, 2,2, gr); }  /* '>' lower arm */
        pp_fill(buf, ix+12, iy+16, 7,2, gr);                             /* cursor underscore */
    } else if(icon==IC_TEST){                           /* D-pad, a color per button */
        pp_fill(buf, ix+9,  iy+2,  6,6, pp_rgb(0xff,0x5c,0x5c));   /* up    red    */
        pp_fill(buf, ix+9,  iy+16, 6,6, pp_rgb(0x36,0xd3,0x99));   /* down  green  */
        pp_fill(buf, ix+2,  iy+9,  6,6, pp_rgb(0x36,0x9e,0xff));   /* left  blue   */
        pp_fill(buf, ix+16, iy+9,  6,6, pp_rgb(0xf0,0xd0,0x30));   /* right yellow */
        pp_fill(buf, ix+9,  iy+9,  6,6, pp_rgb(0xe6,0xe9,0xf0));   /* center white */
    } else if(icon==IC_SNA){                            /* bandpass response curve */
        static const signed char curve[24]={2,2,3,5,9,14,18,20,20,20,20,20,20,20,20,18,14,9,5,3,2,2,2,2};
        uint16_t c=pp_rgb(0x36,0xd3,0x99); int prev=-1;
        for(int x=0;x<24;x++){ int yy=iy+23-curve[x];
            if(prev>=0){ int a=yy<prev?yy:prev,b=yy<prev?prev:yy; pp_fill(buf,ix+x,a,1,b-a+1,c); }
            else pp_fill(buf,ix+x,yy,1,2,c); prev=yy; }
    } else if(icon==IC_SCOPE){                          /* two stacked traces,
                                                           green pulse left (CH1) over blue pulse right (CH2) */
        static const signed char bump[9]={0,1,3,6,7,6,3,1,0};
        uint16_t g=pp_rgb(0x36,0xd3,0x99), c2=pp_rgb(0x20,0xc0,0xff);
        int pg=-1, pb=-1, baseT=iy+8, baseB=iy+20;
        for(int x=0;x<24;x++){
            int dt=x-5,  ht=(dt>=-4&&dt<=4)?bump[dt+4]:0, yt=baseT-ht;    /* top: pulse on left  */
            int dl=x-18, hb=(dl>=-4&&dl<=4)?bump[dl+4]:0, yb=baseB-hb;    /* bottom: pulse on right */
            if(pg>=0){ int a=yt<pg?yt:pg,b=yt<pg?pg:yt; pp_fill(buf,ix+x,a,1,b-a+1,g);}  else pp_fill(buf,ix+x,yt,1,1,g);
            if(pb>=0){ int a=yb<pb?yb:pb,b=yb<pb?pb:yb; pp_fill(buf,ix+x,a,1,b-a+1,c2);} else pp_fill(buf,ix+x,yb,1,1,c2);
            pg=yt; pb=yb;
        }
    } else if(icon==IC_IQREC){                          /* red REC dot over waterfall rows */
        uint16_t pal[4]={ pp_rgb(0x20,0xc0,0xff), pp_rgb(0x30,0xd0,0x60),
                          pp_rgb(0xf0,0xd0,0x30), pp_rgb(0xff,0x80,0x20) };
        for(int r=0;r<4;r++) pp_fill(buf, ix, iy+10+r*4, 24, 3, pal[r]);
        for(int yy=0;yy<10;yy++){ int w=(yy<5?yy:9-yy);                     /* filled disc */
            pp_fill(buf, ix+12-w-1, iy+yy, (w+1)*2, 1, pp_rgb(0xff,0x30,0x30)); }
    } else if(icon==IC_RTL433){                         /* antenna + radiating chevrons */
        uint16_t c=pp_rgb(0x36,0xd3,0x99);
        pp_fill(buf, ix+11, iy+9, 2, 13, c);                               /* mast */
        pp_fill(buf, ix+7,  iy+20, 10, 2, c);                              /* base */
        pp_fill(buf, ix+9,  iy+6, 6, 4, pp_rgb(0xff,0xd0,0x30));           /* feed dot */
        for(int k=1;k<=3;k++){ int d=k*4;                                  /* three V-chevrons */
            for(int a=0;a<=d;a++){ int py=iy+8-a;
                if(py>=iy){ pp_fill(buf, ix+12+d-a, py, 1,1, c);           /* right arm */
                            pp_fill(buf, ix+12-d+a, py, 1,1, c); } }        /* left  arm */
        }
    } else if(icon==IC_TETRIS){                         /* an S-tetromino in block colors */
        uint16_t cy=pp_rgb(0x22,0xd3,0xee), gr=pp_rgb(0x36,0xd3,0x99),
                 rd=pp_rgb(0xff,0x4d,0x4d), yl=pp_rgb(0xf5,0xd0,0x2a);
        /* four 10x10 cells forming an S, each with a dark grid border */
        int cs=10; struct{int x,y; uint16_t c;} q[4]={{1,1,cy},{13,1,yl},{1,13,gr},{13,13,rd}};
        for(int i=0;i<4;i++){ pp_fill(buf, ix+q[i].x, iy+q[i].y, cs, cs, q[i].c);
                              pp_rect(buf, ix+q[i].x, iy+q[i].y, cs, cs, pp_rgb(0x10,0x14,0x1c)); }
    } else if(icon==IC_ADSB){                           /* top-view airplane */
        uint16_t c=pp_rgb(0x8a,0xe0,0xff);
        pp_fill(buf, ix+11, iy+2, 2, 20, c);                               /* fuselage */
        pp_fill(buf, ix+2,  iy+10, 20, 2, c);                              /* main wings */
        pp_fill(buf, ix+7,  iy+18, 10, 2, c);                              /* tailplane */
        pp_fill(buf, ix+10, iy+1, 4, 3, c);                                /* nose */
    } else if(icon==IC_VNA){                            /* resonance notch dip + phase wiggle */
        static const signed char dip[24]={0,0,0,1,1,2,3,6,12,18,20,21,20,18,12,6,3,2,1,1,0,0,0,0};
        uint16_t c=pp_rgb(0xff,0xd0,0x30); int prev=-1;
        for(int x=0;x<24;x++){ int yy=iy+2+dip[x];         /* a downward notch */
            if(prev>=0){int a=yy<prev?yy:prev,b=yy<prev?prev:yy; pp_fill(buf,ix+x,a,1,b-a+1,c);} else pp_fill(buf,ix+x,yy,1,1,c); prev=yy; }
    } else if(icon==IC_TWOTONE){                        /* two tones: readout + pump */
        pp_fill(buf, ix+5,  iy+4, 2, 18, pp_rgb(0x22,0xd3,0xee));   /* readout (cyan) */
        pp_fill(buf, ix+16, iy+9, 2, 13, pp_rgb(0xff,0x8a,0x1f));   /* pump (orange), swept */
        pp_fill(buf, ix+2,  iy+22, 20,2, C_DIM);
    } else if(icon==IC_AMPGAIN){                        /* amplifier triangle */
        uint16_t c=pp_rgb(0x36,0xd3,0x99);
        for(int x=0;x<16;x++){ int h=x*10/15; pp_fill(buf, ix+4+x, iy+12-h, 1, 2*h+1, c); }
        pp_fill(buf, ix+2, iy+11, 4,2, c); pp_fill(buf, ix+20, iy+11, 3,2, c);
    } else if(icon==IC_ATTEN){                          /* attenuator: boxed pad */
        uint16_t c=pp_rgb(0x8a,0xe0,0xff);
        pp_rect(buf, ix+4, iy+7, 16,10, c);
        pp_fill(buf, ix+0, iy+11, 4,2, c); pp_fill(buf, ix+20, iy+11, 4,2, c);
        pp_fill(buf, ix+7, iy+10, 10,1, C_DIM); pp_fill(buf, ix+7, iy+13, 10,1, C_DIM);
    } else if(icon==IC_MANUAL){                         /* open book */
        uint16_t p=pp_rgb(0xe6,0xe9,0xf0), sp=pp_rgb(0x36,0x9e,0xff);
        pp_fill(buf, ix+2, iy+4, 9,16, p); pp_fill(buf, ix+13, iy+4, 9,16, p);
        pp_fill(buf, ix+11, iy+3, 2,18, sp);                               /* spine */
        for(int r=0;r<4;r++){ pp_fill(buf, ix+4, iy+7+r*3, 6,1, C_DIM);
                              pp_fill(buf, ix+14,iy+7+r*3, 6,1, C_DIM); }
    } else if(icon==IC_RFIC){                           /* RFIC: chip die + pins */
        uint16_t c=pp_rgb(0x8a,0xe0,0xff);
        pp_rect(buf, ix+5, iy+5, 14,14, c);
        pp_fill(buf, ix+9, iy+9, 6,6, C_ACCENT);                          /* die */
        for(int k=0;k<3;k++){ pp_fill(buf, ix+2, iy+7+k*4, 3,2, C_DIM);   /* pins L */
                              pp_fill(buf, ix+19,iy+7+k*4, 3,2, C_DIM); }  /* pins R */
    } else if(icon==IC_TCAL){                           /* touch cal: crosshair on a screen */
        uint16_t c=pp_rgb(0x8a,0xe0,0xff), a=C_ACCENT;
        pp_rect(buf, ix+2, iy+3, 20,18, c);                                /* screen bezel */
        pp_fill(buf, ix+7, iy+12, 10,1, a);                                /* crosshair H */
        pp_fill(buf, ix+11,iy+8,  1,10, a);                                /* crosshair V */
        pp_rect(buf, ix+9, iy+10, 5,5, a);                                 /* target box  */
    } else {                                            /* IC_ACKS: lines of text */
        uint16_t c=pp_rgb(0x8a,0xe0,0xff);
        pp_fill(buf, ix+2, iy+3,  20,3, C_ACCENT);
        pp_fill(buf, ix+2, iy+9,  16,2, c);
        pp_fill(buf, ix+2, iy+14, 20,2, c);
        pp_fill(buf, ix+2, iy+19, 12,2, c);
    }
}

static void draw_menu(uint16_t *buf, int sel, const pp_status_t *st, int link_ok){
    static int first=0;                                         /* scroll window top */
    if(sel<first) first=sel;
    if(sel>=first+VIS) first=sel-VIS+1;
    if(first<0) first=0;

    pp_fill(buf, 0,0, PP_W,PP_H, C_BG);
    pp_fill(buf, 0,0, PP_W,26, C_PANEL); pp_fill(buf, 0,26, PP_W,2, C_ACCENT);
    pp_logo(buf, 8,9, C_ACCENT, 1);                     /* scale 1: clears the temp readout */
    /* battery indicator — same code/place as the apps' status bar */
    { int bp = link_ok ? (st->soc>0?st->soc:(st->batt_mV?(st->batt_mV-3300)/9:0)) : 0;
      if(bp<0)bp=0; if(bp>100)bp=100;
      char pc[8]; snprintf(pc,sizeof pc,"%3d%%",bp); pp_text(buf, PP_W-78,6, pc, C_TEXT,1);
      int gx=PP_W-40; uint16_t bc=bp>=40?C_GOOD:(bp>=15?C_WARN:C_BAD);
      pp_rect(buf,gx,5,22,11,C_DIM); pp_fill(buf,gx+22,8,2,5,C_DIM);
      pp_fill(buf,gx+2,7,(22-4)*bp/100,7,bc);
      if(link_ok&&st->charging) pp_text(buf,gx+26,5,"/",C_WARN,1); }
    /* RFIC ('RF') + Zynq PS ('AP') die temps, left of the battery (also on the menu) */
    { char ts[16]; int tx=PP_W-78;
      int rf=pp_ad9361_temp_c();
      if(rf>-200){ snprintf(ts,sizeof ts,"RF%dC",rf); tx-=5+pp_tw(ts,1); pp_text(buf,tx,6,ts,C_DIM,1); }
      int ap=pp_zynq_temp_c();
      if(ap>-200){ snprintf(ts,sizeof ts,"AP%dC",ap); tx-=5+pp_tw(ts,1); pp_text(buf,tx,6,ts,C_DIM,1); } }
    pp_text(buf, 8,40, "SELECT AN APP", C_DIM, 1);
    pp_text(buf, PP_W-pp_tw("0 dBm IN MAX",1)-6, 40, "0 dBm IN MAX", C_BAD, 1);

    int top=60, bot=PP_H-24, pitch=(bot-top)/VIS, boxh=pitch-6;
    int nshow = NAPP<VIS?NAPP:VIS;
    for(int r=0;r<nshow;r++){
        int i=first+r; if(i>=NAPP) break;
        int y=top+r*pitch;
        pp_fill(buf, 12,y, PP_W-24,boxh, (i==sel)?C_PANEL:C_PANEL2);
        pp_rect(buf, 12,y, PP_W-24,boxh, (i==sel)?C_ACCENT:C_PANEL);
        if(i==sel) pp_fill(buf, 12,y, 4,boxh, C_ACCENT);         /* selection rail */
        int ix=24, iy=y+(boxh-24)/2;                             /* icon, then left-justified label */
        draw_icon(buf, ix, iy, APPS[i].icon, i==sel);
        pp_text(buf, ix+24+12, y+(boxh-16)/2, APPS[i].label, (i==sel)?C_TEXT:C_DIM, 2);
        if(APPS[i].exp) pp_text(buf, PP_W-24-pp_tw("EXP",1), y+4, "EXP", C_WARN, 1);  /* experimental */
    }
    if(first>0)          pp_text(buf, PP_W-16, top-11,        "^", C_DIM, 1);   /* more above */
    if(first+VIS<NAPP)   pp_text(buf, PP_W-16, PP_H-22-9,     "v", C_DIM, 1);   /* more below */

    pp_fill(buf, 0,PP_H-22, PP_W,22, C_PANEL2); pp_rect(buf, 0,PP_H-22, PP_W,22, C_PANEL);
    pp_text(buf, 8,PP_H-16, "UP/DN or turn to choose   CENTER open", C_DIM, 1);
}

#ifndef PP_SIM
/* maia-httpd owns the radio (it re-parks the TX LO ~1 Hz and would otherwise
 * contend for the ADC), but only WATERFALL still uses it — SNA/SPECTRUM and the
 * cQED apps now capture directly via libiio. So keep maia stopped except while
 * WATERFALL runs; the direct apps then have the radio to themselves. */
static void maia_ctl(const char *arg){   /* on-demand maia (see the fishball7020 S60 override) */
    char c[80]; snprintf(c,sizeof c,"/etc/init.d/S60maia-httpd %s >/dev/null 2>&1", arg); system(c);
}
#endif

/* Boot splash: a large centred portaPLUTO logo, shown from the moment the panel
 * launches (early in boot) until rcS drops /tmp/pp_boot_done at the end of boot. */
static void boot_splash(uint16_t *buf){
#ifndef PP_SIM
    pp_fill(buf,0,0,PP_W,PP_H,C_BG);
    int sc=3, lw=80*sc;                                  /* pp_logo width ~= 10*8*sc */
    pp_logo(buf,(PP_W-lw)/2,(PP_H-8*sc)/2,C_ACCENT,sc);
    pp_full(buf);
    for(int i=0;i<200 && access("/tmp/pp_boot_done",F_OK)!=0;i++) usleep(100000);  /* wait <=20 s */
    memset(pp_prev,0xA5,sizeof pp_prev);                 /* force a full menu repaint next */
#else
    (void)buf;
#endif
}

int main(int argc, char **argv){
    const char *spidev = argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev  = argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    int sel=0;
    int32_t last_enc=0; int enc_init=0, enc_accum=0;

    boot_splash(buf);                 /* hold the splash until boot completes */
    draw_menu(buf,sel,&st,0); pp_full(buf);
    int have_spi = pp_spi_open(spidev)>=0;
#ifndef PP_SIM
    maia_ctl("off");                  /* at the menu, WATERFALL isn't open -> maia off */
#endif

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            /* UP/DOWN or encoder detents move the selection (4 counts/detent, wrapping) */
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_accum += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_accum/4; enc_accum-=steps*4;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn = st.btn_bitmap;
            steps += !!(pressed&PP_BTN_DOWN) - !!(pressed&PP_BTN_UP);
            if(steps){ sel=((sel+steps)%NAPP+NAPP)%NAPP; }
            if(pressed&PP_BTN_CENTER){
                if(pp_spi_fd>=0){ close(pp_spi_fd); pp_spi_fd=-1; }  /* free the Pico bus for the app */
#ifdef PP_SIM
                { extern int sim_run_app(const char*,const char*,const char*);
                  sim_run_app(APPS[sel].path, spidev, fbdev); }        /* in-process in the sim */
#else
                int is_wf = strstr(APPS[sel].path,"pp-sdr")!=NULL;   /* only WATERFALL needs maia */
                maia_ctl(is_wf ? "on" : "off");
                pid_t pid=fork();
                if(pid==0){ execl(APPS[sel].path, APPS[sel].path, spidev, fbdev, (char*)0); _exit(127); }
                if(pid>0){ int ws; waitpid(pid,&ws,0); }
                if(is_wf) maia_ctl("off");                           /* back at menu: maia off again */
#endif
                have_spi = pp_spi_open(spidev)>=0;                   /* app returned */
                /* The app exits on a CENTER press (confirm-exit popup). That press
                 * is very likely still held now; last_btn=0 would read it as a NEW
                 * CENTER here and immediately relaunch the app just exited. Seed
                 * last_btn all-set so held buttons are masked until released. */
                last_btn=0xff; enc_init=0; enc_accum=0;               /* re-sync encoder baseline */
                memset(pp_prev,0xA5,sizeof pp_prev);                 /* force a full repaint */
            }
        }
        draw_menu(buf,sel,&st,link_ok);                              /* redraw each loop -> live battery */
        pp_refresh(buf);
        usleep(40000);
    }
    return 0;
}
