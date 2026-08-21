/*
 * pp-twotone — two-tone / pump sweep for qubit spectroscopy & punch-out.
 *   Topology: an EXTERNAL generator supplies the resonator readout tone; the Pluto
 *   supplies the qubit PUMP (TX) and READS the resonator output (RX). RX LO parks
 *   the (fixed) external readout tone at a convenient offset and measures its
 *   complex amplitude; TX sweeps the pump over frequency x power. The map shows the
 *   readout response CHANGE (dB) vs pump-off baseline -> qubit spectroscopy / AC
 *   Stark / punch-out. Uses the AD9361's independent RX & TX LOs (readout and pump
 *   may be GHz apart).
 *
 *   DATA : READOUT(ext gen f), PCENTER, PSPAN, RUN   SETTINGS: PFPTS, PW0, PW1, PWSTEP, RXGAIN
 * usage: pp-twotone [spidev] [fbdev]
 */
#include "pp_ui.h"
#include "pp_vna.h"
#include "pp_waterfall.h"          /* PPWF_TURBO[] */

#define MAXF 161
#define MAXP 45
#define MX0 8
#define MY0 54
#define MW  (PP_W-16)
#define MH  150                    /* map area 54..204 */

static long readout_khz=5000000, pcenter_khz=4700000, pspan_khz=200000;
static long run_dummy=0;
static long pfpts=101, pw0_db=-40, pw1_db=0, pwstep_db=5, rxgain_db=0;
static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P5[5]={10000,1000,100,10,1};
static const long P3[3]={100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static float MAP[MAXP][MAXF];      /* readout response (dB) vs pump-off */
static int   nf=101, npw=9, have_map=0, g_run=0, g_radio=1;
static float map_lo=-20, map_hi=5;

static void act_run(void){ g_run=1; }
static void fmt_mhz(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_go(const pp_field_t*f,char*v,int n){ (void)f; snprintf(v,n,"GO"); }

static uint8_t poll_pressed(pp_status_t*st){
    static uint8_t last=0xff; uint8_t rx[PP_FRAME_LEN];
    if(pp_spi_fd<0 || pp_poll(rx)!=0 || pp_decode(rx,st)<0) return 0;
    uint8_t pr=st->btn_bitmap & ~last; last=st->btn_bitmap; return pr;
}

/* measure the readout tone's magnitude (dB); pump set by caller */
static double read_mag(void){
    long fs=4000000, dr=fs/4;                          /* readout at +fs/4 offset */
    (void)fs; double re,im; if(!vna_measure(dr,4,&re,&im)){ g_radio=0; return -90; }
    return vna_mag_db(re,im);
}
static int do_sweep(uint16_t*buf, pp_status_t*st){
    long fs=4000000, dr=fs/4, dp=fs/4;
    long long rlo=(long long)readout_khz*1000 - dr;    /* park readout at +dr */
    nf=(int)pfpts; if(nf<2)nf=2; if(nf>MAXF)nf=MAXF;
    npw=(int)((pw1_db-pw0_db)/(pwstep_db>0?pwstep_db:1))+1; if(npw<1)npw=1; if(npw>MAXP)npw=MAXP;
    vna_set_fs(fs); vna_set_rxbw(fs); vna_set_rxlo(rlo); vna_set_rxgain((double)rxgain_db);
    if(!vna_rx_open()){ g_radio=0; return 0; }
    long long pstart=(long long)(pcenter_khz-pspan_khz/2)*1000;
    long long pspan=(long long)pspan_khz*1000;
    /* pump-off baseline */
    vna_tone(0,0);
#ifdef PP_SIM
    vna_sim_pump(0,-99);
#endif
    double m0=read_mag();
    int stopped=0;
    for(int p=0;p<npw && !stopped;p++){
        double pw=pw0_db + p*pwstep_db;
        vna_set_txatten((double)(-pw));               /* pump power */
        for(int i=0;i<nf;i++){
            long long pf = pstart + (nf>1? (long long)i*pspan/(nf-1):0);
            vna_set_txlo(pf-dp); vna_tone(dp,1);       /* pump emits at pf */
#ifdef PP_SIM
            vna_sim_pump((double)pf,pw);
#endif
            MAP[p][i] = (float)(read_mag()-m0);
            if(((p*nf+i)&15)==0){
                if(poll_pressed(st)&PP_BTN_7){ stopped=1; break; }
                char s[24]; snprintf(s,sizeof s,"SWEEP %d/%d",p*nf+i,npw*nf);
                pp_fill(buf,PP_W-96,22,96,10,C_BG); pp_text(buf,PP_W-94,22,s,C_ACCENT,1);
                pp_ui_status_bar(buf,"TWO-TONE",st,1,NULL,C_BAD);   /* live RX meter during sweep */
                pp_refresh(buf);
            }
        }
    }
    vna_tone(0,0); vna_set_txatten(60);
    /* autoscale color range */
    map_lo=1e9; map_hi=-1e9;
    for(int p=0;p<npw;p++) for(int i=0;i<nf;i++){ float v=MAP[p][i]; if(v<map_lo)map_lo=v; if(v>map_hi)map_hi=v; }
    if(map_hi-map_lo<3){ map_hi=map_lo+3; }
    return !stopped;
}
static void draw_map(uint16_t*buf){
    char s[56];
    pp_fill(buf,0,48,PP_W,PP_H-48-14,C_BG);
    if(!have_map){ pp_text(buf,4,MY0+30,"set READOUT to ext-gen freq, RUN",C_DIM,1); return; }
    for(int y=0;y<MH;y++){
        int p = npw>1? y*(npw-1)/(MH-1) : 0;                    /* low power at bottom */
        p = npw-1-p;
        uint16_t*line=&buf[(MY0+y)*PP_W];
        for(int x=0;x<MW;x++){ int i= nf>1? x*(nf-1)/(MW-1):0;
            float f=(MAP[p][i]-map_lo)/(map_hi-map_lo); if(f<0)f=0; if(f>1)f=1;
            line[MX0+x]=PPWF_TURBO[(int)(f*255)]; }
    }
    pp_rect(buf,MX0-1,MY0-1,MW+2,MH+2,C_PANEL);
    /* axes */
    long pf0=pcenter_khz-pspan_khz/2, pf1=pcenter_khz+pspan_khz/2;
    snprintf(s,sizeof s,"%ld.%03ld",pf0/1000,pf0%1000); pp_text(buf,MX0,MY0+MH+2,s,C_DIM,1);
    snprintf(s,sizeof s,"pump MHz");                    pp_text(buf,(PP_W-pp_tw(s,1))/2,MY0+MH+2,s,C_ACCENT,1);
    snprintf(s,sizeof s,"%ld.%03ld",pf1/1000,pf1%1000); pp_text(buf,PP_W-pp_tw(s,1),MY0+MH+2,s,C_DIM,1);
    snprintf(s,sizeof s,"%lddB",pw1_db); pp_text(buf,MX0,MY0-1,s,C_DIM,1);
    snprintf(s,sizeof s,"%lddB",pw0_db); pp_text(buf,MX0,MY0+MH-8,s,C_DIM,1);
    snprintf(s,sizeof s,"readout resp %.0f..%.0f dB",map_lo,map_hi);
    pp_text(buf,2,MY0+MH+12,s,C_GOOD,1);
}
static pp_field_t F[12]; static int NF=0;
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
    addf("READOUT","MHz",&readout_khz,70000,6000000,P7,7,3,fmt_mhz,0);
    addf("PCENTER","MHz",&pcenter_khz,70000,6000000,P7,7,3,fmt_mhz,0);
    addf("PSPAN","MHz",&pspan_khz,1000,2000000,P7,7,2,fmt_mhz,0);
    addf("RUN","",&run_dummy,0,0,P1,1,0,fmt_go,act_run);
    int ndata=NF;
    addf("PFPTS","",&pfpts,11,MAXF,P3,3,1,fmt_int,0);
    addf("PW0","dB",&pw0_db,-60,0,P2,2,1,fmt_int,0);
    addf("PW1","dB",&pw1_db,-60,0,P2,2,1,fmt_int,0);
    addf("PWSTEP","dB",&pwstep_db,1,20,P2,2,1,fmt_int,0);
    addf("RXGAIN","dB",&rxgain_db,0,73,P2,2,1,fmt_int,0);
    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;
    static uint16_t buf[PP_W*PP_H]; pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN]; int prev_screen=-1; memset(buf,0,sizeof buf);
    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) break; }
        pp_ui_sync(&ui);
        if(g_run){ g_run=0; if(g_radio){ have_map=1; do_sweep(buf,&st); prev_screen=-1; } }
        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        const char*note = !g_radio?"no radio":NULL;
        pp_ui_status_bar(buf,"TWO-TONE",&st,link_ok,note,C_BAD);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,20); draw_map(buf); vna_draw_ports(buf,"pump TX1 / read RX1"); }
        else                    pp_ui_draw_settings(buf,&ui,20,PP_H-20-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    vna_tone(0,0); vna_rx_close();
    return 0;
}
