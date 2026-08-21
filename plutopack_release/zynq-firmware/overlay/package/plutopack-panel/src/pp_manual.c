/*
 * pp-manual — on-device manual. A list of topics (device overview + one per app);
 *   CENTER opens a topic, UP/DOWN or the wheel scroll it, BTN7 goes back / exits.
 *   Concise: theory + what each field does. See app_recommendations.md for depth.
 * usage: pp-manual [spidev] [fbdev]
 */
#include "pp_ui.h"

static const char *T_OVERVIEW[]={
 "PlutoPack front panel","",
 "An AD9361 software-defined radio",
 "(70 MHz-6 GHz, 2 RX + 2 TX, ~56 MHz",
 "bandwidth) on a Zynq-7020, driving",
 "this 320x240 front panel.","",
 "CONTROLS",
 "Wheel / UP / DOWN : move between fields",
 "CENTER : edit a field, or fire an",
 "  action (RUN / CAL / REC ...)",
 "Editing: LEFT/RIGHT pick the digit,",
 "  wheel or UP/DOWN change it, CENTER ok",
 "BACK : back (edit->browse->",
 "  settings->exit to the menu)",
 "SETTINGS : open more fields","",
 "A field stays amber until the radio",
 "confirms the new value (sync).","",
 "The status bar shows die temps (AP =",
 "Zynq CPU, RF = AD9361), battery, and",
 "(in RX apps) an RX1/RX2 level meter:",
 "green to red as the ADC nears clipping.","",
 "EXP = experimental: runs in the",
 "simulator but is not yet hardware-",
 "validated - trust results with care.",
};
static const char *T_WF[]={
 "WATERFALL","",
 "Live spectrum + scrolling waterfall",
 "from the maia-sdr FFT.","",
 "CENTER : tuned frequency",
 "SPAN   : displayed bandwidth",
 "GAIN   : RX gain (dB)",
 "GMODE  : MANUAL / SLOW / FAST AGC",
 "MIN,MAX: color scale (dBFS)",
 "RXBW   : RX analog bandwidth",
};
static const char *T_TONE[]={
 "TONEGEN","",
 "CW / two-tone transmitter using the",
 "AD9361 DDS, plus an RX level meter",
 "for loopback checks.","",
 "OUT  : master on / off",
 "C1/C2 FRQ : per-channel RF frequency",
 "C1/C2 ATT : per-channel attenuation",
 "C1/C2 ON  : per-channel enable (DATA)",
 "RATE : sample rate  BW : TX bandwidth",
 "C1 is the master tone: moving it drags",
 "C2 so both stay within one Nyquist of",
 "the shared LO.",
 "Wire TX->RX to see the RX1/RX2 bars.",
};
static const char *T_SPEC[]={
 "SPECTRUM","",
 "Swept spectrum analyzer. Samples one",
 "FFT bin at a fixed IF and steps the",
 "LO, so the filter shape cancels",
 "(scallop-free) at any bandwidth.","",
 "CENTER,SPAN : sweep range",
 "RBW  : resolution bandwidth",
 "AVG  : averaging",
 "GAIN,MODE : RX gain and AGC",
 "REF,RANGE : vertical scale",
 "MAXHOLD : hold the peak trace",
 "RXBW : RX analog bandwidth","",
 "Touch the plot to drop a marker: a",
 "dashed line reads out that frequency",
 "and the trace value.",
};
static const char *T_SNA[]={
 "SNA - scalar network analyzer","",
 "|S21| transmission magnitude vs freq.",
 "TX a tone, step the LO, read power;",
 "normalize to a stored THRU (RUN REF).","",
 "SPAN,POINTS : sweep",
 "TXPWR : drive level",
 "RUN : sweep   RUN REF : store thru",
 "CENTER,IFBW,AVG : resolution",
 "REF,RANGE : scale   CONT : re-sweep",
 "RXGAIN,RXBW : receiver","",
 "Touch the plot for a marker (dashed",
 "line + freq and |S21| readout).",
};
static const char *T_VNA[]={
 "VNA - vector resonator analyzer","",
 "Complex S21: magnitude AND phase.",
 "Fixed LO with the DDS stepped",
 "digitally, so phase is coherent;",
 "CAL divides out a THRU baseline",
 "(cable delay + ripple). Fits f0 and",
 "loaded Q from the 3 dB dip.",
 "Phase is qualitative without an RX2",
 "reference (see the manual notes).","",
 "CENTER : resonator frequency",
 "SPAN : span, <=24 MHz (single LO)",
 "POINTS : sweep points",
 "CAL : store the THRU baseline",
 "RUN : sweep   CONT : repeat",
 "TXPWR,RXGAIN,AVG : levels",
 "RANGE : magnitude scale (dB)","",
 "Touch the plot for a marker (dashed",
 "line + freq and |S21| readout).",
};
static const char *T_TWO[]={
 "TWO-TONE / pump sweep","",
 "An EXTERNAL generator drives the",
 "resonator readout tone; the Pluto TX",
 "is the qubit PUMP and the Pluto RX",
 "reads the resonator. Uses the",
 "independent RX/TX LOs (readout and",
 "pump may be GHz apart). Maps the",
 "readout change vs pump freq x power",
 "(qubit spectroscopy / punch-out).","",
 "READOUT : external gen frequency",
 "PCENTER,PSPAN : pump sweep range",
 "RUN : run the map",
 "PFPTS : pump frequency points",
 "PW0,PW1,PWSTEP : pump power (dB)",
 "RXGAIN : receiver gain",
};
static const char *T_AMP[]={
 "AMP GAIN - gain & P1dB","",
 "Sweep TX drive and measure output.",
 "Gain = Pout - Pin; P1dB is where the",
 "gain has dropped 1 dB. Levels are",
 "relative (no absolute power ref).",
 "Put the amplifier in the TX->RX path.","",
 "CENTER : test frequency",
 "PW0,PW1 : drive sweep range (dB)",
 "RUN : sweep",
 "PWSTEP : step size",
 "RXGAIN,AVG : receiver",
};
static const char *T_ATT[]={
 "ATTEN - attenuator / loss checker","",
 "Single frequency. Store a loopback",
 "REFERENCE, install the DUT, and read",
 "insertion loss = Pref - Pmeas (dB).",
 "A ratio, so absolute levels don't",
 "matter.","",
 "CENTER : test frequency",
 "REF : store the loopback reference",
 "TXPWR,RXGAIN,AVG : levels",
};
static const char *T_SCOPE[]={
 "SCOPE - I/Q time-domain","",
 "Raw RX magnitude vs time from direct",
 "capture (libiio).","",
 "LO : tuned frequency",
 "SRATE : sample rate",
 "GAIN1,GAIN2 : per-channel RX gain",
 "RXBW : RX analog bandwidth",
 "TIME : window (capped to buffer)",
 "MODE : CONT (free-run) / PAUSE",
 "GO : capture one frame",
 "Touch the plot for a time cursor",
 "with CH1/CH2 readouts.",
};
static const char *T_IQ[]={
 "IQ REC - record & playback","",
 "Waterfall (own FFT) to find a band;",
 "record raw cs16 to RAM; review it; or",
 "transmit it back over the air.","",
 "CENTER,SPAN,GAIN : tune",
 "REC : start/stop record (RAM-capped)",
 "REVIEW : replay the file to waterfall",
 "TX PLAY : retransmit (emits RF!)",
 "RXBW,MIN,MAX : receiver & color",
 "MAXMB : record size cap (RAM)",
 "TXATTN : TX attenuation for playback",
};
static const char *T_RTL[]={
 "RTL_433 - ISM sensors","",
 "Decodes 433/315/345/868/915 MHz",
 "devices via rtl_433. Live table of",
 "decoded sensors (model, id, values).","",
 "BAND : ISM band preset",
 "GAIN : RX gain (manual mode)",
 "MODE : MANUAL / SLOW / FAST AGC",
 "DECIM : feed decimation (CPU vs BW)",
 "Needs a suitable antenna.",
};
static const char *T_ADSB[]={
 "ADS-B - aircraft tracker","",
 "1090 MHz Mode S via dump1090. Live",
 "table (ICAO / flight / alt / speed /",
 "age); CENTER opens a detail card.",
 "Positions need paired frames (CPR).","",
 "UP/DN : select   CENTER : detail",
 "SETTINGS : gain + AGC mode   BACK: exit",
 "Needs a 1090 MHz antenna.",
};
static const char *T_TET[]={
 "TETRIS","",
 "Wheel or LEFT/RIGHT : move",
 "UP or CENTER : rotate",
 "DOWN : soft drop",
 "SETTINGS : hard drop",
 "BACK : exit",
 "CENTER restarts after game over.",
};
static const char *T_UI[]={
 "FRONTEND STAT","",
 "Front-panel status: button / encoder /",
 "display / touch test. Each input lights",
 "up as you use it.",
 "",
 "USER GPIO panel (bottom right):",
 " 13,14 = outputs  15 = input",
 " LEFT flips 14, RIGHT flips 13;",
 " input 15 shows live.",
 " A 6-pin header mock (15 14 13",
 " 3V3 G 5V) shows the pinout.",
};
static const char *T_TCAL[]={
 "TOUCH CAL","",
 "Touchscreen calibration. Tap each",
 "crosshair in turn; an affine map is fit",
 "and saved, then used by every app.",
 "",
 "After saving, tap to test the mapping.",
 "CENTER redoes it, BTN7 exits.",
 "",
 "The cal is stored in RAM and clears on",
 "reboot - re-run after a power cycle.",
};

typedef struct { const char *title; const char *const *lines; int n; } sect_t;
#define S(t,a) { t, a, (int)(sizeof(a)/sizeof((a)[0])) }
static const sect_t SEC[]={
 S("Device overview",T_OVERVIEW),
 S("WATERFALL",T_WF), S("TONEGEN",T_TONE), S("SPECTRUM",T_SPEC), S("SNA",T_SNA),
 S("VNA",T_VNA), S("TWO-TONE",T_TWO), S("AMP GAIN",T_AMP), S("ATTEN",T_ATT),
 S("SCOPE",T_SCOPE), S("IQ REC",T_IQ), S("RTL_433",T_RTL), S("ADS-B",T_ADSB),
 S("TETRIS",T_TET), S("FRONTEND STAT",T_UI), S("TOUCH CAL",T_TCAL),
};
#define NSEC ((int)(sizeof SEC/sizeof SEC[0]))

#define LH 10
enum { V_LIST=0, V_PAGE };

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi=pp_spi_open(spidev)>=0;
    int view=V_LIST, sel=0, top=0, prev=-1;
    static uint16_t buf[PP_W*PP_H]; pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0; int32_t last_enc=0; int enc_init=0, enc_acc=0;
    memset(buf,0,sizeof buf);

    int confirm=0;
    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_acc += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_acc/4; enc_acc-=steps*4;
            uint8_t pr = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;
            int mv = steps + !!(pr&PP_BTN_DOWN) - !!(pr&PP_BTN_UP);
            if(confirm){
                if(pr&PP_BTN_CENTER) break;              /* confirm -> exit to menu */
                if(pr&PP_BTN_7) confirm=0;               /* cancel */
            }else if(view==V_LIST){
                if(pr&PP_BTN_7) confirm=1;               /* BACK on list -> confirm exit */
                else { sel+=mv; if(sel<0)sel=0; if(sel>=NSEC)sel=NSEC-1;
                       if(pr&PP_BTN_CENTER){ view=V_PAGE; top=0; prev=-1; } }
            }else{
                if(pr&PP_BTN_7){ view=V_LIST; prev=-1; }
                else{ int vis=(PP_H-16-24)/LH, mx=SEC[sel].n>vis?SEC[sel].n-vis:0;
                    top+=mv; if(top<0)top=0; if(top>mx)top=mx; }
            }
        }
        if(view!=prev){ pp_fill(buf,0,20,PP_W,PP_H-20,C_BG); memset(pp_prev,0xA5,sizeof pp_prev); prev=view; }

        if(view==V_LIST){
            pp_fill(buf,0,20,PP_W,PP_H-20,C_BG);
            int rh=13, y0=26, rows=(PP_H-14-y0)/rh, first=0;
            if(sel>=rows) first=sel-rows+1;
            for(int r=0;r<rows && first+r<NSEC;r++){ int i=first+r, y=y0+r*rh; int fo=(i==sel);
                pp_fill(buf,4,y-1,PP_W-8,rh-1, fo?C_PANEL:C_BG);
                if(fo) pp_fill(buf,4,y-1,3,rh-1,C_ACCENT);
                pp_text(buf,12,y,SEC[i].title, fo?C_TEXT:C_DIM,1); }
            pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
            pp_text(buf,4,PP_H-12,"UP/DN select   CENTER open   BACK exit",C_DIM,1);
            pp_ui_status_bar(buf,"MANUAL",&st,link_ok,NULL,C_WARN);
        }else{
            const sect_t *s=&SEC[sel];
            pp_fill(buf,0,20,PP_W,PP_H-20,C_BG);
            int vis=(PP_H-16-24)/LH;
            for(int r=0;r<vis && top+r<s->n;r++){ const char*ln=s->lines[top+r]; int y=24+r*LH;
                uint16_t col=C_TEXT; if(top+r==0) col=C_ACCENT;      /* title line */
                pp_text(buf,6,y,ln,col,1); }
            int mx=s->n>vis?s->n-vis:0;
            if(mx>0){ int trh=PP_H-16-24, kh=trh*vis/s->n; if(kh<8)kh=8; int ky=24+(trh-kh)*top/mx;
                pp_fill(buf,PP_W-4,24,2,trh,C_PANEL2); pp_fill(buf,PP_W-4,ky,2,kh,C_ACCENT); }
            pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
            pp_text(buf,4,PP_H-12,"UP/DN or wheel scroll   BACK",C_DIM,1);
            pp_ui_status_bar(buf,"MANUAL",&st,link_ok,NULL,C_WARN);
        }
        pp_confirm_overlay(buf,confirm);
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
