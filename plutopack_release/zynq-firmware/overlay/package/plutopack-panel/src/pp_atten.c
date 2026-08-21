/*
 * pp-atten — attenuator / loss checker (single-frequency SNA).
 *   TX a CW tone at CENTER and continuously measure the received level. Press REF
 *   in loopback (or through your cal path) to store the reference; then install the
 *   DUT and the screen shows the live transmission delta
 *       loss = P_ref - P_meas  [dB]
 *   i.e. the DUT's insertion loss / attenuation. A ratio, so it is insensitive to
 *   the absolute TX/RX levels.
 *
 *   DATA : CENTER, REF, then live loss readout   SETTINGS: TXPWR, RXGAIN, AVG
 * usage: pp-atten [spidev] [fbdev]
 */
#include "pp_ui.h"
#include "pp_vna.h"


static long center_khz=1000000, ref_dummy=0;
static long txpwr_db=0, rxgain_db=0, avg_n=4;
static const long P7[7]={1000000,100000,10000,1000,100,10,1};
static const long P2[2]={10,1};
static const long P1[1]={1};

static double cur_db=-90, ref_db=0; static int ref_valid=0, g_run_ref=0, g_radio=1;
static long ap_center=-1, ap_tx=-999, ap_rx=-999;    /* last-applied config */

static void act_ref(void){ g_run_ref=1; }
static void fmt_mhz(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld.%03ld",*f->val/1000,*f->val%1000); }
static void fmt_int(const pp_field_t*f,char*v,int n){ snprintf(v,n,"%ld",*f->val); }
static void fmt_go(const pp_field_t*f,char*v,int n){ (void)f; snprintf(v,n,"SET"); }

static void apply_cfg(void){
    long fs=4000000, d=fs/4; long long lo=(long long)center_khz*1000 - d;
    vna_set_fs(fs); vna_set_rxbw(fs); vna_set_rxlo(lo); vna_set_txlo(lo);
    vna_set_rxgain((double)rxgain_db); vna_set_txatten((double)(-txpwr_db));
    vna_rx_open(); vna_tone(d,1);
    ap_center=center_khz; ap_tx=txpwr_db; ap_rx=rxgain_db;
}
static double measure_db(void){
    long d=4000000/4; double re,im; if(!vna_measure(d,(int)avg_n,&re,&im)){ g_radio=0; return -90; }
    return vna_mag_db(re,im);
}
static void draw(uint16_t*buf){
    char s[48];
    pp_fill(buf,0,48,PP_W,PP_H-48-14,C_BG);
    /* big loss number */
    if(ref_valid){
        double loss=ref_db-cur_db;
        snprintf(s,sizeof s,"%.2f",loss);
        int w=pp_tw(s,4); pp_text(buf,(PP_W-w)/2,74,s,loss>=0?C_ACCENT:C_WARN,4);
        pp_text(buf,(PP_W)/2+w/2+6,98,"dB",C_UNIT,2);
        pp_text(buf,(PP_W-pp_tw("INSERTION LOSS",1))/2,60,"INSERTION LOSS",C_DIM,1);
    } else {
        pp_text(buf,(PP_W-pp_tw("take a loopback REF",1))/2,90,"take a loopback REF",C_WARN,1);
    }
    /* measured / reference readout (ADC level is in the status-bar RX meter now) */
    snprintf(s,sizeof s,"meas %.1f dB", cur_db); pp_text(buf,8,150,s,C_TEXT,1);
    if(ref_valid){ snprintf(s,sizeof s,"ref %.1f dB", ref_db); pp_text(buf,PP_W-pp_tw(s,1)-8,150,s,C_GOOD,1); }
    pp_text(buf,8,178,"CENTER sets freq;  REF = store loopback",C_DIM,1);
    pp_text(buf,8,190,"then install the DUT to read loss",C_DIM,1);
    if(!g_radio) pp_text(buf,8,204,"no radio",C_BAD,1);
}
static pp_field_t F[8]; static int NF=0;
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
    addf("REF","",&ref_dummy,0,0,P1,1,0,fmt_go,act_ref);
    int ndata=NF;
    addf("TXPWR","dB",&txpwr_db,-60,0,P2,2,1,fmt_int,0);
    addf("RXGAIN","dB",&rxgain_db,0,73,P2,2,1,fmt_int,0);
    addf("AVG","",&avg_n,1,32,P2,2,1,fmt_int,0);
    pp_ui_t ui; pp_ui_init(&ui,F,NF); ui.ndata=ndata;
    static uint16_t buf[PP_W*PP_H]; pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN]; int prev_screen=-1; memset(buf,0,sizeof buf);
    if(g_radio) apply_cfg();
    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0) link_ok=1;
        if(link_ok){ if(pp_ui_input(&ui,&st)==PP_UI_EXIT) break; }
        pp_ui_sync(&ui);
        if(g_radio && (center_khz!=ap_center||txpwr_db!=ap_tx||rxgain_db!=ap_rx)) apply_cfg();
        if(g_radio) cur_db=measure_db();
        if(g_run_ref){ g_run_ref=0; ref_db=cur_db; ref_valid=1; }

        if(ui.screen!=prev_screen){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev_screen=ui.screen; }
        pp_ui_status_bar(buf,"ATTEN",&st,link_ok, g_radio?NULL:"no radio", C_BAD);
        if(ui.screen==PP_DATA){ pp_ui_draw_field_band(buf,&F[ui.focus],ui.editing,20); draw(buf); vna_draw_ports(buf,"TX1 out / RX1 in"); }
        else                    pp_ui_draw_settings(buf,&ui,20,PP_H-20-14);
        pp_ui_footer(buf,&ui);
        pp_refresh(buf);
        usleep(30000);
    }
    vna_tone(0,0); vna_rx_close();
    return 0;
}
