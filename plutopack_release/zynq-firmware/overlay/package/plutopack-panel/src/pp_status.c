/*
 * pp-status — poll the Pico and print decoded status as text (no display).
 * The Zynq-side counterpart to the Pico USB heartbeat: use it to verify the
 * SPI link and see live sensor values headless.
 * usage: pp-status [spidev=/dev/spidev2.0] [count=0(=forever)]
 */
#include "pp_common.h"

int main(int argc, char **argv) {
    const char *spidev = argc>1?argv[1]:"/dev/spidev2.0";
    long count = argc>2?atol(argv[2]):0;
    if (pp_spi_open(spidev)<0){ perror("open spidev"); return 1; }
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN];
    unsigned ok=0,bad=0;
    for(long i=0; count==0 || i<count; i++){
        if (pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            ok++;
            printf("[ok %u/%u] seq=%3u flags=%c%c%c batt=%dmV soc=%d%% cur=%+dmA enc=%ld btn=0x%02x touch=%s x=%u y=%u\n",
                ok,ok+bad,(unsigned)st.seq,
                st.gauge_ok?'G':'-', st.touch_ok?'T':'-', st.charging?'C':'-',
                st.batt_mV, st.soc, st.current_mA, (long)st.enc_pos, st.btn_bitmap,
                st.touch_down?"DN":"up", st.touch_x, st.touch_y);
        } else {
            bad++;
            printf("[BAD %u] magic=0x%02x crc? (link down / mode?)\n", bad, rx[0]);
        }
        fflush(stdout);
        usleep(250000);
    }
    return 0;
}
