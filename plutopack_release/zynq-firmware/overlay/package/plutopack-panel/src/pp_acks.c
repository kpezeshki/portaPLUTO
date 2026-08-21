/*
 * pp-acks — scrollable acknowledgements / open-source credits.
 *   UP/DOWN or the ENCODER wheel scroll; BTN7 exits. A plain, large block of
 *   text — no radio, no settings. Playable in the web simulator too.
 *
 * usage: pp-acks [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_ui.h"

/* Lines are ~<=38 chars to fit 320px at the 8px font (with a scrollbar margin). */
static const char *TEXT[] = {
 "PlutoPack Front Panel",
 "Acknowledgements",
 "",
 "This firmware is built on the work of",
 "many open-source projects. Thank you to",
 "their authors and contributors.",
 "",
 "--- Maia SDR ---",
 "Real-time spectrometer, FFT and the",
 "/waterfall stream behind the WATERFALL",
 "app.",
 "Daniel Estevez (EA4GPZ) et al.",
 "github.com/maia-sdr/maia-sdr",
 "License: GPL-3.0",
 "",
 "--- tezuka firmware ---",
 "The PlutoSDR/clone firmware builder this",
 "image is based on: board support, wide",
 "tuning range, and Maia integration.",
 "F5OEO (Evariste Courjaud) et al.",
 "github.com/F5OEO/tezuka_fw",
 "",
 "--- Analog Devices PlutoSDR ---",
 "libiio and libad9361 (raw I/Q capture",
 "and playback across the SDR apps), the",
 "AD9361 driver, and the plutosdr-fw base.",
 "github.com/analogdevicesinc",
 "libiio License: LGPL-2.1",
 "",
 "--- rtl_433 ---",
 "ISM-band device decoding behind the",
 "RTL_433 app.",
 "Benjamin Larsson (merbanan) et al.",
 "github.com/merbanan/rtl_433",
 "License: GPL-2.0+",
 "",
 "--- dump1090 (FlightAware) ---",
 "1090 MHz ADS-B / Mode S decoding behind",
 "the ADS-B app.",
 "github.com/flightaware/dump1090",
 "License: GPL-2.0+",
 "",
 "--- Buildroot ---",
 "The embedded-Linux build system that",
 "assembles this image (2026.02).",
 "buildroot.org   License: GPL-2.0+",
 "",
 "--- Linux kernel & Das U-Boot ---",
 "Kernel and bootloader for the Zynq-7020.",
 "License: GPL-2.0",
 "",
 "--- SoapySDR / soapyplutosdr ---",
 "SDR abstraction layer bundled in the",
 "image.  github.com/pothosware/SoapySDR",
 "",
 "--- Turbo colormap ---",
 "The waterfall / spectrum color palette.",
 "Anton Mikhailov (Google)",
 "License: Apache-2.0",
 "",
 "--- IBM VGA 8x8 font ---",
 "The panel bitmap font (Lat15-VGA8).",
 "",
 "----------------------------------------",
 "PlutoPack panel apps, UI framework and",
 "this build integration by Kaveh,",
 "with Claude (Anthropic).",
 "",
 "73!",
 "",
};
#define NLINES ((int)(sizeof TEXT/sizeof TEXT[0]))

#define TXT_Y0 24
#define LH     10                                  /* line height */

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    int vis=(PP_H-16-TXT_Y0)/LH;                    /* visible lines */
    int maxtop = NLINES>vis ? NLINES-vis : 0;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    int32_t last_enc=0; int enc_init=0, enc_accum=0;
    int top=0;
    memset(buf,0,sizeof buf);

    int confirm=0;
    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_accum += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_accum/4; enc_accum-=steps*4;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;
            if(confirm){
                if(pressed&PP_BTN_CENTER) break;                /* confirm -> exit to menu */
                if(pressed&PP_BTN_7) confirm=0;                 /* cancel */
            }else if(pressed&PP_BTN_7){ confirm=1;              /* BACK -> confirm exit */
            }else{
                top += steps;                                   /* wheel */
                if(pressed&PP_BTN_DOWN) top++;
                if(pressed&PP_BTN_UP)   top--;
                if(top<0) top=0; if(top>maxtop) top=maxtop;
            }
        }

        pp_fill(buf,0,TXT_Y0-4,PP_W,PP_H-(TXT_Y0-4),C_BG);
        for(int r=0;r<vis && top+r<NLINES;r++){
            const char *ln=TEXT[top+r]; int y=TXT_Y0+r*LH;
            uint16_t col=C_TEXT;
            if(ln[0]=='-'&&ln[1]=='-') col=C_ACCENT;            /* section headers */
            else if(top+r<2) col=C_ACCENT;                      /* title */
            pp_text(buf,6,y,ln,col,1);
        }
        /* scrollbar */
        if(maxtop>0){
            int trh=PP_H-16-TXT_Y0, kh=trh*vis/NLINES; if(kh<8)kh=8;
            int ky=TXT_Y0+(trh-kh)*top/maxtop;
            pp_fill(buf,PP_W-4,TXT_Y0,2,trh,C_PANEL2);
            pp_fill(buf,PP_W-4,ky,2,kh,C_ACCENT);
        }
        pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
        pp_text(buf,4,PP_H-12,"UP/DN or wheel: scroll   BACK",C_DIM,1);
        pp_ui_status_bar(buf,"CREDITS",&st,link_ok,NULL,C_WARN);
        pp_confirm_overlay(buf,confirm);
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
