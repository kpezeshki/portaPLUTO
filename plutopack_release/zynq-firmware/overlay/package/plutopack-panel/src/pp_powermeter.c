/*
 * pp-powermeter — absolute RX power meter.
 *
 * The user sets an LO frequency; the device captures the ~30 MHz window around
 * it, finds the highest-power tone (excluding the DC/LO-leakage guard), auto-
 * ranges the RX gain to stay in the calibrated, non-overloaded region, and
 * displays that tone's absolute power in dBm using the RX power calibration.
 *
 * GATED: unusable unless a valid power cal (pp_cal_dir()/rxpower.cal, written by
 * pp-rxpowercal) is present AND covers the tone frequency. This is the ONLY app
 * that consumes the power calibration.
 *
 * usage: pp-powermeter [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_ui.h"
#include "pp_rxcal.h"
#include "pp_rxmeas.h"

static const long long STEP[4]={100000LL,1000000LL,10000000LL,100000000LL};
static const char     *STEPN[4]={"100k","1M","10M","100M"};

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    static pp_rxcal_t cal;
    int have = pp_rxcal_load(&cal, pp_cal_path(PP_RXCAL_NAME)) && pp_rxcal_valid(&cal);

    long long fs = have? cal.fs_hz : 30000000LL;
    long long lo = have? cal.freq_hz[cal.nfreq/2] : 1000000000LL;
    int gi = have? cal.ngain-1 : 0;                 /* start high, range down */
    int step=1;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0; int32_t last_enc=0; int enc_init=0, enc_acc=0;

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_acc += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int det=enc_acc/4; enc_acc-=det*4;
            if(pressed&PP_BTN_7) return 0;                          /* exit */
            if(pressed&PP_BTN_CENTER) step=(step+1)&3;              /* cycle tune step */
            int move = det + (!!(pressed&PP_BTN_RIGHT)) - (!!(pressed&PP_BTN_LEFT));
            if(move){ lo += (long long)move*STEP[step];
                      if(lo<70000000LL)lo=70000000LL; if(lo>6000000000LL)lo=6000000000LL; }
        }

        pp_fill(buf,0,0,PP_W,PP_H,C_BG);
        pp_ui_status_bar(buf,"POWER METER",&st,link_ok,NULL,C_WARN);
        /* max-input warning (matches the launcher); RX level meter is fed by
         * pp_rx_capture() into the status bar's RX bars each measurement. */
        pp_text(buf, PP_W-pp_tw("0 dBm IN MAX",1)-6, 23, "0 dBm IN MAX", C_BAD, 1);

        if(!have){
            pp_text(buf, 20, 70, "NO POWER CAL", C_BAD, 2);
            pp_text(buf, 20, 100, "Run RX POWER CAL first.", C_TEXT, 1);
            pp_text(buf, 20, 120, "This meter needs a valid", C_DIM, 1);
            pp_text(buf, 20, 132, "rxpower.cal on the data partition.", C_DIM, 1);
            pp_text(buf, 6, PP_H-12, "BACK: exit", C_DIM, 1);
            pp_refresh(buf); usleep(120000); continue;
        }

        /* ---- measure: auto gain-range, then find + invert the peak tone ---- */
        float pdbm=PP_RXCAL_NODATA; long long tone_hz=0; float level=0; const char *stat="...";
        int used_gi=gi, over=0, weak=0;
        for(int tries=0; tries<cal.ngain; tries++){
            int rawpk=0;
            if(pp_rx_capture(lo,fs,cal.gain_db[gi],fs,&rawpk)<0){ stat="RX ERR"; break; }
            if(pp_rx_overloaded(rawpk)){ over=1; if(gi>0){ gi--; continue; } stat="OVER"; break; }
            over=0;
            long long off; float lv;
            if(pp_rx_find_tone(fs,&off,&lv)<0){ weak=1; if(gi<cal.ngain-1){ gi++; continue; } stat="NO TONE"; break; }
            /* tone too close to the floor -> try more gain */
            if(lv - pp_rx_floor() < 8.f && gi<cal.ngain-1){ gi++; continue; }
            used_gi=gi; level=lv; tone_hz=lo+off;
            if(!pp_rxcal_covers(&cal,tone_hz)){ stat="UNCAL f"; break; }
            pdbm=pp_rxcal_invert(&cal,tone_hz,used_gi,off,lv);
            stat = (pdbm<=PP_RXCAL_NODATA+1.f) ? "RANGE" : "OK";
            break;
        }

        /* ---- big readout ---- */
        char big[24];
        if(pdbm>PP_RXCAL_NODATA+1.f){ snprintf(big,sizeof big,"%+.1f", pdbm);
            int w=pp_tw(big,4); pp_text(buf,(PP_W-w)/2-24,80,big,C_GOOD,4);
            pp_text(buf,(PP_W+w)/2-24+6,104,"dBm",C_TEXT,2);
        } else {
            const char *msg = over?"OVER": weak?"WEAK": !strcmp(stat,"UNCAL f")?"UNCAL":"---";
            int w=pp_tw(msg,4); pp_text(buf,(PP_W-w)/2,80,msg,C_BAD,4);
        }

        char l[48];
        snprintf(l,sizeof l,"LO %.3f MHz  [%s]", lo/1e6, STEPN[step]);
        pp_text(buf, 20, 150, l, C_TEXT, 1);
        if(tone_hz){ snprintf(l,sizeof l,"tone %.4f MHz  (%+.2f MHz)", tone_hz/1e6, (tone_hz-lo)/1e6);
                     pp_text(buf, 20, 165, l, C_ACCENT, 1); }
        snprintf(l,sizeof l,"gain %.0f dB   level %.1f dBFS   %s",
                 cal.gain_db[used_gi], level, stat);
        pp_text(buf, 20, 180, l, C_DIM, 1);
        snprintf(l,sizeof l,"cal: %d MHz-%d MHz  fs %.1f MHz",
                 (int)(cal.freq_hz[0]/1000000), (int)(cal.freq_hz[cal.nfreq-1]/1000000), fs/1e6);
        pp_text(buf, 20, 195, l, C_DIM, 1);

        pp_text(buf, 6, PP_H-12, "turn/L-R tune   CENTER step   BACK exit", C_DIM, 1);
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
