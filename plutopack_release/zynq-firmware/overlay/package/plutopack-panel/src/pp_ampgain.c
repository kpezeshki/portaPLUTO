/*
 * pp-ampgain — amplifier gain & 1 dB compression (P1dB).
 *   Fixed frequency; sweep the TX drive power and measure the received level.
 *   Relative gain = Pout_meas - Pin; small-signal gain G0 = gain at the lowest
 *   drive; P1dB = the input power where gain has fallen 1 dB below G0. Levels are
 *   uncalibrated (no absolute power ref), but compression is a SHAPE, so P1dB and
 *   the gain roll-off are meaningful. Put the amplifier DUT in the TX->RX path.
 *
 *   DATA : CENTER, PW0, PW1, RUN     SETTINGS: PWSTEP, RXGAIN, AVG
 * usage: pp-ampgain [spidev] [fbdev]
 */
#include "pp_ui.h"
#include "pp_vna.h"

#define MAXPW 81
#define GY0 56
#define GY1 200

static long center_khz=1000000, pw0_db=-50, pw1_db=0;
static long run_dummy=0, pwstep_db=1, rxgain_db=0, avg_n=4;
static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static double PIN[MAXPW], GN[MAXPW];    /* input power (dB), relative gain (dB) */
static int npw=0, have=0, g_run=0, g_radio=1;
static double G0=0, P1dB=0; int p1_valid=0;

static void act_run(void){ g_run=1; }
static void fmt_mhz(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_go(const pp_field_t*f,char*v,int n){ (void)f; snprintf(v,n,"GO"); }
static uint8_t poll_pressed(pp_status_t*st){
    static uint8_t last=0xff; uint8_t rx[PP_FRAME_LEN];
    if(pp_spi_fd<0 || pp_poll(rx)!=0 || pp_decode(rx,st)<0) return 0;
    uint8_t pr=st->btn_bitmap & ~last; last=st->btn_bitmap; return pr;
}
static int do_sweep(uint16_t*buf, pp_status_t*st){
    long fs=4000000, d=fs/4; long long lo=(long long)center_khz*1000 - d;
    vna_set_fs(fs); vna_set_rxbw(fs); vna_set_rxlo(lo); vna_set_txlo(lo); vna_set_rxgain((double)rxgain_db);
    if(!vna_rx_open()){ g_radio=0; return 0; }
    npw=(int)((pw1_db-pw0_db)/(pwstep_db>0?pwstep_db:1))+1; if(npw<2)npw=2; if(npw>MAXPW)npw=MAXPW;
    int stopped=0;
    for(int i=0;i<npw;i++){
        double pin=pw0_db + i*pwstep_db;
        vna_set_txatten(-pin); vna_tone(d,1);
        double re,im; if(!vna_measure(d,(int)avg_n,&re,&im)) g_radio=0;
        PIN[i]=pin; GN[i]=vna_mag_db(re,im)-pin;      /* relative gain */
        if((i&7)==0 || i==npw-1){
            if(poll_pressed(st)&PP_BTN_7){ stopped=1; }
            char s[24]; snprintf(s,sizeof s,"SWEEP %d/%d",i+1,npw);
            pp_fill(buf,PP_W-90,22,90,10,C_BG); pp_text(buf,PP_W-88,22,s,C_ACCENT,1);
            pp_ui_status_bar(buf,"AMP GAIN",st,1,NULL,C_BAD);   /* live RX meter during sweep */
            pp_refresh(buf);
            if(stopped) break;
        }
    }
    vna_tone(0,0); vna_set_txatten(60);
    /* G0 = mean of the lowest ~20% of drive; P1dB where gain = G0-1 */
    int nlow=npw/5; if(nlow<1)nlow=1; double acc=0; for(int i=0;i<nlow;i++) acc+=GN[i]; G0=acc/nlow;
    p1_valid=0;
    for(int i=1;i<npw;i++){ if(GN[i]<=G0-1.0){
        double f=(G0-1.0-GN[i-1])/((GN[i]-GN[i-1])!=0?(GN[i]-GN[i-1]):1e-9);
        P1dB=PIN[i-1]+f*(PIN[i]-PIN[i-1]); p1_valid=1; break; } }
    return !stopped;
}
static void draw(uint16_t*buf){
    char s[56];
    pp_fill(buf,0,48,PP_W,PP_H-48-14,C_BG);
    if(!have){ pp_text(buf,4,GY0+30,"put amp DUT in TX->RX path, RUN",C_DIM,1); return; }
    double gmin=1e9,gmax=-1e9; for(int i=0;i<npw;i++){ if(GN[i]<gmin)gmin=GN[i]; if(GN[i]>gmax)gmax=GN[i]; }
    if(gmax-gmin<3){ gmax=gmin+3; } double top=gmax+1, span=(top-(gmin-1));
    for(int d=0;d<=4;d++){ int y=GY0+d*(GY1-GY0)/4; pp_fill(buf,0,y,PP_W,1,C_PANEL); }
    int prev=-1;
    for(int i=0;i<npw;i++){ int x=(npw>1)? i*(PP_W-1)/(npw-1):0;
        double f=(top-GN[i])/span; if(f<0)f=0; if(f>1)f=1; int y=GY0+(int)(f*(GY1-GY0));
        if(prev>=0){int a=y<prev?y:prev,b=y<prev?prev:y; pp_fill(buf,x,a,1,b-a+1,C_ACCENT);} else pp_fill(buf,x,y,1,1,C_ACCENT); prev=y; }
    /* G0-1 reference line + P1dB marker */
    double f=(top-(G0-1.0))/span; if(f>=0&&f<=1){ int y=GY0+(int)(f*(GY1-GY0)); pp_fill(buf,0,y,PP_W,1,C_DIM); }
    if(p1_valid){ int x=(npw>1)?(int)((P1dB-pw0_db)/(pw1_db-pw0_db)*(PP_W-1)):0; if(x>=0&&x<PP_W) pp_fill(buf,x,GY0,1,GY1-GY0,C_WARN); }
    snprintf(s,sizeof s,"%.0f",top);            pp_text(buf,2,GY0,s,C_DIM,1);
    snprintf(s,sizeof s,"%.0f dB(rel)",top-span);pp_text(buf,2,GY1-8,s,C_DIM,1);
    snprintf(s,sizeof s,"G0 %.1f dB(rel)",G0);  pp_text(buf,2,GY1+2,s,C_GOOD,1);
    if(p1_valid) snprintf(s,sizeof s,"P1dB in %.1f dB",P1dB); else snprintf(s,sizeof s,"no compression seen");
    pp_text(buf,PP_W-pp_tw(s,1)-2,GY1+2,s,p1_valid?C_WARN:C_DIM,1);
    long l=pw0_db,r=pw1_db;
    snprintf(s,sizeof s,"%ld",l); pp_text(buf,0,GY1+12,s,C_DIM,1);
    pp_text(buf,(PP_W-pp_tw("Pin dB(rel)",1))/2,GY1+12,"Pin dB(rel)",C_ACCENT,1);
    snprintf(s,sizeof s,"%ld",r); pp_text(buf,PP_W-pp_tw(s,1),GY1+12,s,C_DIM,1);
}
static pp_field_t F[10]; static int NF=0;
static void addf(const char*nm,const char*u,long*v,long lo,long hi,const long*pl,int np,int sel,
                 void(*fmt)(const pp_field_t*,char*,int),void(*act)(void)){
    pp_field_t*f=&F[NF++]; memset(f,0,sizeof*f);
    f->name=nm; f->unit=u; f->val=v; f->vmin=lo; f->vmax=hi; f->places=pl; f->nplace=np; f->sel=sel; f->fmt=fmt; f->action=act;
}
int main(int argc,char**argv){
    const char*spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char*fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi=pp_spi_open(spidev)>=0;
    int nt,r2; g_radio=vna_init(&nt,&r2);
    NF=0;
    addf("CENTER","MHz",&center_khz,70000,6000000,P7,7,3,fmt_mhz,0);
    addf("PW0","dB",&pw0_db,-60,0,P2,2,1,fmt_int,0);
    addf("PW1","dB",&pw1_db,-60,0,P2,2,1,fmt_int,0);
    addf("RUN","",&run_dummy,0,0,P1,1,0,fmt_go,act_run);
    int ndata=NF;
    addf("PWSTEP","dB",&pwstep_db,1,10,P2,2,1,fmt_int,0);
    addf("RXGAIN","dB",&rxgain_db,0,73,P2,2,1,fmt_int,0);
    addf("AVG","",&avg_n,1,32,P2,2,1,fmt_int,0);
    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;
    static uint16_t buf[PP_W*PP_H]; pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN]; int prev_screen=-1; memset(buf,0,sizeof buf);
    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) break; }
        pp_ui_sync(&ui);
        if(g_run){ g_run=0; if(g_radio){ have=1; do_sweep(buf,&st); prev_screen=-1; } }
        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        const char*note = !g_radio?"no radio":NULL;
        pp_ui_status_bar(buf,"AMP GAIN",&st,link_ok,note,C_BAD);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,20); draw(buf); vna_draw_ports(buf,"TX1 out / RX1 in"); }
        else                    pp_ui_draw_settings(buf,&ui,20,PP_H-20-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    vna_tone(0,0); vna_rx_close();
    return 0;
}
