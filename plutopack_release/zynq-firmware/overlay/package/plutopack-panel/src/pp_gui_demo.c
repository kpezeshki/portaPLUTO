/*
 * pp-gui — animated status-GUI demo with synthesized sensor data (no Pico
 * needed). Exercises the pp_render() status screen + partial refresh: SoC bar,
 * encoder value, walking button, touch crosshair all animate; only changed
 * row-bands are pushed to the panel. BTN8 returns to the app picker.
 * usage: pp-gui [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_common.h"
#define SW_BACK 0x40   /* BTN8 -> return to the picker */

static const int SINE[32]={0,195,383,556,707,831,924,981,1000,981,924,831,707,556,383,195,
  0,-195,-383,-556,-707,-831,-924,-981,-1000,-981,-924,-831,-707,-556,-383,-195};
static int isin(int i){ return SINE[i&31]; }

int main(int argc, char **argv) {
    const char *spidev = argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev  = argc>2?argv[2]:"/dev/fb0";
    if (pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;    /* only for the BTN8 back button */
    static uint16_t buf[PP_W*PP_H];
    pp_status_t st, real; memset(&st,0,sizeof st); memset(&real,0,sizeof real);
    st.gauge_ok=1; st.touch_ok=1;
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    unsigned t=0;
    for(;;){
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&real)>=0){
            uint8_t pressed=real.btn_bitmap & ~last_btn; last_btn=real.btn_bitmap;
            if(pressed & SW_BACK) return 0;   /* BTN8 -> back to picker */
        }
        t++;
        st.seq=t;
        st.enc_pos=(int32_t)t*3;
        st.soc=55+(isin(t)*40)/1000;
        st.batt_mV=(uint16_t)(3300+st.soc*9);
        st.current_mA=(isin(t+8)*300)/1000;
        st.charging=st.current_mA>0;
        st.btn_bitmap=(uint8_t)(1u<<(t%7));
        st.touch_down=(t%5)!=0;
        st.touch_x=(uint16_t)(2047+isin(t)*2);
        st.touch_y=(uint16_t)(2047+isin(t*2+4)*2);
        st.touch_z=(uint16_t)(300+isin(t)/8);
        pp_render(buf,&st);
        pp_refresh(buf);
        usleep(120000);
    }
    return 0;
}
