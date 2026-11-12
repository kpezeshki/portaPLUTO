/*
 * pp-rxpowercal — RX absolute-power calibration.
 *
 * Builds the 4-axis power cal [frequency][input power][rx gain][LO offset] that
 * the POWER METER consumes. The user supplies a calibrated tone from a bench
 * source; the screen tells them which frequency + power to set. Only the
 * frequency and power are manual (one prompt each) — for each, the SDR sweeps
 * rx gain (low -> high, stopping before ADC overload) and LO offset (retuning
 * the LO around the tone so it lands across the passband) automatically, and
 * records the measured peak-tone level (dBFS) at every point.
 *
 * The sweep runs one capture per UI frame so it stays responsive and can be
 * cancelled. Progress is persisted: BACK saves a partial cal and exits, and a
 * re-launch resumes at the first unmeasured point.
 *
 * usage: pp-rxpowercal [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_ui.h"
#include "pp_rxcal.h"
#include "pp_rxmeas.h"
#include <stdlib.h>

#define CAL_FS_HZ   30000000LL     /* 30 MHz capture window (per design) */
#define TONE_SNR_DB 8.f            /* tone-present threshold above the floor */
#define SRCH_WIN_HZ 500000LL       /* search +/- this around the expected offset */

/* ---- default grid (edit here to change coverage) ---- */
static void grid_init(pp_rxcal_t *c){
    static const int   FMHZ[10]={300,500,800,1100,1400,1700,2100,2400,2700,3000};
    static const float PW[6]  ={-60,-50,-40,-30,-20,-10};
    c->nfreq=10;   for(int i=0;i<c->nfreq;i++)   c->freq_hz[i]=(long long)FMHZ[i]*1000000LL;
    c->npower=6;   for(int i=0;i<c->npower;i++)  c->power_dbm[i]=PW[i];
    c->ngain=15;   for(int i=0;i<c->ngain;i++)   c->gain_db[i]=(float)(i*5);        /* 0..70 dB */
    c->noffset=29; for(int i=0;i<c->noffset;i++) c->offset_hz[i]=(long long)(i-14)*1000000LL; /* +/-14 MHz */
    c->fs_hz=CAL_FS_HZ; c->complete=0; c->temp_c10=0;
    for(int k=0;k<pp_rxcal_cells(c);k++) c->meas[k]=PP_RXCAL_NODATA;
}
static int point_measured(const pp_rxcal_t *c,int fi,int pi){
    for(int g=0;g<c->ngain;g++) for(int o=0;o<c->noffset;o++)
        if(c->meas[pp_rxcal_idx(c,fi,pi,g,o)]>PP_RXCAL_NODATA+1.f) return 1;
    return 0;
}

enum { ST_PROMPT, ST_SWEEP, ST_DONE };

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    static pp_rxcal_t cal;
    grid_init(&cal);
    long long fs=cal.fs_hz;
    int NP = cal.nfreq*cal.npower;

    /* resume from an existing partial cal with the same grid */
    static pp_rxcal_t old;
    if(pp_rxcal_load(&old, pp_cal_path(PP_RXCAL_NAME)) && !old.complete
       && old.nfreq==cal.nfreq && old.npower==cal.npower
       && old.ngain==cal.ngain && old.noffset==cal.noffset && old.fs_hz==cal.fs_hz){
        memcpy(cal.meas, old.meas, sizeof(float)*pp_rxcal_cells(&cal));
    }
    int mp=0; while(mp<NP && point_measured(&cal, mp/cal.npower, mp%cal.npower)) mp++;

    int state = (mp>=NP)? ST_DONE : ST_PROMPT;
    int sw_oi=0, sw_gi=0;
    long long live_off=0; float live_lv=0; const char *live="";
    int warn=0;                 /* frames to show a validation warning */

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0; int32_t last_enc=0; int enc_init=0;

    for(;;){
        int link_ok=0, pressed=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; } last_enc=st.enc_pos;
        }
        int fi=mp/cal.npower, pi=mp%cal.npower;

        /* -------------------- input / state transitions -------------------- */
        if(pressed&PP_BTN_7){
            if(state==ST_SWEEP){ state=ST_PROMPT; pp_sim_set_source(0,0,0); }   /* cancel point */
            else { pp_rxcal_save(&cal, pp_cal_path(PP_RXCAL_NAME));            /* save & exit */
                   return 0; }
        }
        if(state==ST_PROMPT && (pressed&PP_BTN_8) && mp<NP){                    /* skip point */
            mp++; while(mp<NP && point_measured(&cal,mp/cal.npower,mp%cal.npower)) mp++;
            if(mp>=NP){ cal.complete=0; state=ST_DONE; }
        }
        if(state==ST_PROMPT && (pressed&PP_BTN_CENTER) && mp<NP){
            /* validate a tone is actually present before committing to a sweep */
            pp_sim_set_source(cal.freq_hz[fi], cal.power_dbm[pi], 1);
            int rawpk=0; long long go; float lv=-999;
            if(pp_rx_capture(cal.freq_hz[fi]-2000000LL, fs, cal.gain_db[cal.ngain/2], fs, &rawpk)>=0)
                pp_rx_peak_near(2000000LL, fs, SRCH_WIN_HZ, &go, &lv);
            if(lv - pp_rx_floor() < TONE_SNR_DB){ warn=40; pp_sim_set_source(0,0,0); }
            else {                                                    /* start the sweep */
                for(int g=0;g<cal.ngain;g++) for(int o=0;o<cal.noffset;o++)
                    cal.meas[pp_rxcal_idx(&cal,fi,pi,g,o)]=PP_RXCAL_NODATA;
                sw_oi=0; sw_gi=0; state=ST_SWEEP;
            }
        }

        /* ------------------ one sweep step per frame ----------------------- */
        if(state==ST_SWEEP){
            long long foff=cal.offset_hz[sw_oi];
            if(llabs(foff) < PP_RX_DCGUARD_HZ){                       /* DC/LO guard: skip */
                sw_oi++; sw_gi=0; live="(DC guard)";
            } else {
                int rawpk=0; long long LO=cal.freq_hz[fi]-foff;
                int ok = pp_rx_capture(LO, fs, cal.gain_db[sw_gi], fs, &rawpk)>=0;
                if(ok && pp_rx_overloaded(rawpk)){                    /* higher gains also overload */
                    for(int g=sw_gi; g<cal.ngain; g++)
                        cal.meas[pp_rxcal_idx(&cal,fi,pi,g,sw_oi)]=PP_RXCAL_NODATA;
                    live="OVERLOAD -> next offset"; live_off=foff;
                    sw_oi++; sw_gi=0;
                } else {
                    float lv=-999, val=PP_RXCAL_NODATA; long long go=foff;
                    if(ok){ pp_rx_peak_near(foff, fs, SRCH_WIN_HZ, &go, &lv);
                            if(lv - pp_rx_floor() >= TONE_SNR_DB) val=lv; }
                    cal.meas[pp_rxcal_idx(&cal,fi,pi,sw_gi,sw_oi)]=val;
                    live_off=go; live_lv=lv; live=(val>PP_RXCAL_NODATA+1.f)?"":"(weak)";
                    sw_gi++;
                    if(sw_gi>=cal.ngain){ sw_gi=0; sw_oi++; }
                }
            }
            if(sw_oi>=cal.noffset){                                   /* point complete */
                pp_sim_set_source(0,0,0);
                pp_rxcal_save(&cal, pp_cal_path(PP_RXCAL_NAME));      /* checkpoint */
                mp++; while(mp<NP && point_measured(&cal,mp/cal.npower,mp%cal.npower)) mp++;
                if(mp>=NP){ cal.complete=1; pp_rxcal_save(&cal, pp_cal_path(PP_RXCAL_NAME)); state=ST_DONE; }
                else state=ST_PROMPT;
            }
        }

        /* no-input diagnostic probe (throttled): capture at 0 dB gain so the raw/
         * AC/DC ADC counts are visible on the prompt with nothing connected. */
        if(state==ST_PROMPT && mp<NP){
            static int probe=0;
            if(++probe>=4){ probe=0; int pk=0;
                pp_rx_capture(cal.freq_hz[fi]-2000000LL, fs, 0.0, fs, &pk); }
        }

        /* ---------------------------- draw --------------------------------- */
        pp_fill(buf,0,0,PP_W,PP_H,C_BG);
        pp_ui_status_bar(buf,"RX POWER CAL",&st,link_ok,NULL,C_WARN);
        /* max-input warning (matches the launcher); RX level meter is fed by
         * pp_rx_capture() into the status bar's RX bars during the sweep. */
        pp_text(buf, PP_W-pp_tw("0 dBm IN MAX",1)-6, 23, "0 dBm IN MAX", C_BAD, 1);
        char l[64];
        if(state==ST_DONE){
            pp_text(buf, 20, 80, "CALIBRATION COMPLETE", C_GOOD, 2);
            snprintf(l,sizeof l,"%d freq x %d power x %d gain x %d offset",
                     cal.nfreq,cal.npower,cal.ngain,cal.noffset);
            pp_text(buf, 20, 108, l, C_TEXT, 1);
            snprintf(l,sizeof l,"saved to %s/%s", pp_cal_dir(), PP_RXCAL_NAME);
            pp_text(buf, 20, 124, l, C_DIM, 1);
            pp_text(buf, 6, PP_H-12, "BACK: exit", C_DIM, 1);
        } else {
            snprintf(l,sizeof l,"Point %d / %d", mp+1, NP); pp_text(buf,20,30,l,C_ACCENT,1);
            snprintf(l,sizeof l,"f = %.3f MHz", cal.freq_hz[fi]/1e6); pp_text(buf,20,50,l,C_TEXT,2);
            snprintf(l,sizeof l,"P = %+.1f dBm", cal.power_dbm[pi]);  pp_text(buf,20,74,l,C_TEXT,2);

            int othr=(int)(PP_RX_OVERLOAD*PP_RX_ADC_FS);
            if(state==ST_PROMPT){
                pp_text(buf,20,100,"Set the calibrated source to the",C_DIM,1);
                pp_text(buf,20,112,"above, connect to RX, then CENTER.",C_DIM,1);
                if(warn>0){ pp_text(buf,20,126,"! No tone detected - check source/cable",C_BAD,1); warn--; }
                /* live no-input ADC readout (raw incl DC / AC excl DC / DC means) */
                snprintf(l,sizeof l,"chk @0dB: raw %d  ac %d  dc %.0f/%.0f",
                         pp_rx_rawpk, pp_rx_acpk, pp_rx_dc_i, pp_rx_dc_q);
                pp_text(buf,20,146,l,C_DIM,1);
                snprintf(l,sizeof l,"ovld thr(ac) %d   floor %.0f dB", othr, pp_rx_floor());
                pp_text(buf,20,158,l, pp_rx_acpk>=othr?C_BAD:C_GOOD,1);
                pp_text(buf,6,PP_H-12,"CENTER sweep  B8 skip  BACK save+exit",C_DIM,1);
            } else { /* ST_SWEEP */
                int prog = (sw_oi*cal.ngain + sw_gi)*100 / (cal.noffset*cal.ngain);
                pp_rect(buf,20,104,280,12,C_DIM); pp_fill(buf,21,105,278*prog/100,10,C_ACCENT);
                snprintf(l,sizeof l,"sweeping... %d%%", prog); pp_text(buf,20,120,l,C_TEXT,1);
                snprintf(l,sizeof l,"gain %.0f dB  offset %+.2f MHz",
                         cal.gain_db[sw_gi<cal.ngain?sw_gi:cal.ngain-1], live_off/1e6);
                pp_text(buf,20,136,l,C_DIM,1);
                snprintf(l,sizeof l,"level %.1f dBFS  %s", live_lv, live);
                pp_text(buf,20,150,l,C_DIM,1);
                snprintf(l,sizeof l,"raw %d  ac %d  dc %.0f/%.0f  thr %d",
                         pp_rx_rawpk, pp_rx_acpk, pp_rx_dc_i, pp_rx_dc_q, othr);
                pp_text(buf,20,164,l, pp_rx_acpk>=othr?C_BAD:C_DIM,1);
                pp_text(buf,6,PP_H-12,"BACK: cancel this point",C_DIM,1);
            }
        }
        pp_refresh(buf);
        usleep(state==ST_SWEEP?1000:30000);
    }
    return 0;
}
