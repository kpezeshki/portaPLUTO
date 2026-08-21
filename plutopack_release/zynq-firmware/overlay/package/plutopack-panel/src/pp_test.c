/*
 * pp-test — live front-panel status app (FRONTEND STAT). Polls the Pico and renders
 * REAL battery / encoder / buttons / touch / GPIO via pp_render().
 *   - SEQ counter (top-right) increments with each Pico frame => proves liveness
 *   - button cells light per the real btn_bitmap; encoder value tracks the knob
 *   - user GPIO: input 15 shown live; LEFT/RIGHT flip outputs 14/13 (CMD_SET_GPIO)
 *   - CENTER opens a live GAS GAUGE detail screen (BQ27441 registers pulled from
 *     the Pico over SPI); BACK/CENTER returns. BTN7 on the status screen exits.
 * usage: pp-test [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_common.h"

/* GAS GAUGE detail screen: name/value list of live BQ27441 registers. */
static void draw_gauge(uint16_t *buf, const pp_gauge_t *g, int valid, int link_ok){
    pp_fill(buf,0,0,PP_W,PP_H,C_BG);
    pp_fill(buf,0,0,PP_W,22,C_PANEL); pp_fill(buf,0,22,PP_W,2,C_ACCENT);
    pp_text(buf,6,7,"GAS GAUGE",C_ACCENT,1);
    pp_text(buf,PP_W-pp_tw("BQ27441",1)-6,7,"BQ27441",C_DIM,1);
    pp_fill(buf,0,PP_H-14,PP_W,14,C_PANEL2);
    pp_text(buf,4,PP_H-12,"BACK / CENTER: status",C_DIM,1);
    if(!valid){ pp_text(buf,8,44, link_ok?"reading gauge...":"no Pico link", C_WARN,1); return; }
    char s[24]; int y=30; const int LH=14;
    #define ROW(nm, ...) do { \
        if((((y-30)/LH)&1)==0) pp_fill(buf,0,y-2,PP_W,LH,C_PANEL2);  /* zebra */ \
        pp_text(buf,8,y,nm,C_DIM,1); \
        snprintf(s,sizeof s,__VA_ARGS__); pp_text(buf,124,y,s,C_ACCENT,1); \
        y+=LH; } while(0)
    ROW("Voltage",   "%d mV",  g->mv);
    ROW("Current",   "%+d mA",  g->ma);
    ROW("Temp",      "%d.%d C", g->temp_c10/10, (g->temp_c10<0?-g->temp_c10:g->temp_c10)%10);
    ROW("SoC(gauge)","%d %%",  g->soc);
    ROW("Remaining", "%d mAh", g->rm);
    ROW("FullCharge","%d mAh", g->fcc);
    ROW("NomAvail",  "%d mAh", g->nac);
    ROW("FullAvail", "%d mAh", g->fac);
    ROW("DesignCap", "%d mAh", g->dcap);
    ROW("TermVolt",  "%d mV",  g->termv);
    ROW("Flags",     "0x%04x", g->flags);
    ROW("Control",   "0x%04x", g->ctrl);
    ROW("ChemID",    "0x%04x", g->chem);
    #undef ROW
}

int main(int argc, char **argv){
    const char *spidev = argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev  = argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0, gpio_out=0;
    int screen=0;                       /* 0 = status, 1 = gauge detail */
    pp_gauge_t g; int g_valid=0;

    if(have_spi) pp_cmd(PP_CMD_SET_GPIO, gpio_out, rx);   /* known state: both low */

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn = st.btn_bitmap;
            if(screen==0){
                if(pressed & PP_BTN_7) return 0;                  /* BTN7 -> app switcher */
                if(pressed & PP_BTN_CENTER){                      /* CENTER -> gauge screen */
                    screen=1; g_valid=0; memset(pp_prev,0xA5,sizeof pp_prev); }
                if(pressed & PP_BTN_LEFT)  gpio_out ^= 0x02;      /* LEFT  -> flip GPIO14 */
                if(pressed & PP_BTN_RIGHT) gpio_out ^= 0x01;      /* RIGHT -> flip GPIO13 */
                if(pressed & (PP_BTN_LEFT|PP_BTN_RIGHT))
                    pp_cmd(PP_CMD_SET_GPIO, gpio_out, rx);
            } else {
                if(pressed & (PP_BTN_7|PP_BTN_CENTER)){           /* back to status */
                    screen=0; memset(pp_prev,0xA5,sizeof pp_prev); }
            }
        }

        if(screen==0){
            pp_render(buf, &st);
        } else {
            if(have_spi && link_ok) g_valid = pp_get_gauge(&g);   /* live gauge fetch */
            draw_gauge(buf, &g, g_valid, link_ok);
        }
        pp_refresh(buf);
        usleep(80000);
    }
    return 0;
}
