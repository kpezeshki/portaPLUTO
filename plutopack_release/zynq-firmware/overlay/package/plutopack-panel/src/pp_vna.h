/*
 * pp_vna.h — shared vector-measurement engine for the microwave-characterization
 *   apps (VNA / two-tone / amp-gain / attenuator). It combines:
 *     - TX stimulus  : the FPGA DDS CW tone via pp_iio.h (tg_*), on ad9361-phy +
 *                      the DDS core (sysfs) — clean CW at TX_LO + offset, no
 *                      streaming.
 *     - RX capture   : raw complex I/Q via pp_iq.h (libiio, cf-ad9361-lpc).
 *     - measurement  : a single-bin complex DFT (Goertzel-style phasor recurrence)
 *                      at a chosen baseband offset -> complex amplitude+phase.
 *
 * The vector trick without an external reference: keep the RX LO FIXED across a
 * sweep and step the stimulus DIGITALLY (DDS offset) within the baseband. Because
 * every point shares one LO, the TX/RX LO phase offset is a per-sweep CONSTANT, so
 * relative phase across the sweep is coherent and a THRU/baseline normalization
 * removes cable delay + ripple. (RX2 reference-channel mode is a future addition.)
 *
 * PP_SIM models a notch resonator (+ optional pump influence) so the apps run in
 * the web simulator.
 */
#ifndef PP_VNA_H
#define PP_VNA_H
#include <math.h>
#include <stdint.h>
#include "pp_iio.h"                /* TX: tg_init/tg_lo/tg_tone/tg_atten/tg_fs/tg_bw */
#include "pp_iq.h"                 /* RX: iq_init/iq_set_*/iq_rx_open/iq_rx_read (libiio) */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define VNA_N 4096                 /* samples per measurement block */

static long      g_vna_fs = 4000000;
static long long g_vna_rxlo = 5000000000LL;
static double    g_vna_txatt = 20.0;
static int       g_vna_ntx = 1, g_vna_rx2 = 0, g_vna_rxopen = 0;

static int vna_init(int *ntx, int *have_rx2){
    int htx=0, ok_rx = iq_init(&htx);
    long fs=0; int ok_tx = tg_init(&g_vna_ntx, &fs);
    if(fs>0) g_vna_fs=fs;
    g_vna_rx2 = 0;                  /* RX2 reference mode: reserved for a future rev */
    if(ntx) *ntx=g_vna_ntx; if(have_rx2) *have_rx2=g_vna_rx2;
    return ok_rx && ok_tx;
}
static void vna_set_fs(long hz){ long g; g_vna_fs=hz; iq_set_fs(hz); tg_fs(hz,&g); }
static void vna_set_rxlo(long long hz){ g_vna_rxlo=hz; iq_set_rxlo(hz); }
static void vna_set_txlo(long long hz){ long long g; tg_lo(hz,&g); }
static void vna_set_rxbw(long hz){ iq_set_rxbw(hz); }
static void vna_set_rxgain(double db){ iq_set_rxgain(db); }
static void vna_set_txatten(double db){ double g; g_vna_txatt=db; tg_atten(0,db,&g); }
static void vna_tone(long off_hz, int on){ long g; if(on) tg_txlo_on(); tg_tone(0, off_hz, on?TG_AMP:0.0, 0, &g); }

static int  vna_rx_open(void){ if(g_vna_rxopen) return 1; g_vna_rxopen=iq_rx_open(VNA_N); return g_vna_rxopen; }
static void vna_rx_close(void){ if(g_vna_rxopen){ iq_rx_close(); g_vna_rxopen=0; } }

/* port label — these apps always use TX1 (out) and RX1 (in) of the AD9361 */
static void vna_draw_ports(uint16_t *buf, const char *txt){
    pp_text(buf, PP_W-pp_tw(txt,1)-2, 50, txt, C_DIM, 1);
}
static double vna_mag_db(double re,double im){ double p=re*re+im*im; return p>1e-9?10.0*log10(p):-90.0; }
static double vna_phase_deg(double re,double im){ return atan2(im,re)*180.0/M_PI; }

#ifndef PP_SIM
/* single-bin complex DFT of interleaved int16 I/Q at baseband offset off_hz */
static void vna_dft(const int16_t *iq, int n, double off_hz, double *re, double *im){
    double w = 2.0*M_PI*off_hz/(double)g_vna_fs;      /* rad/sample */
    double cr=cos(w), ci=-sin(w);                     /* phasor step e^{-jw} */
    double pr=1.0, pi=0.0, ar=0.0, ai=0.0;
    for(int k=0;k<n;k++){
        double I=iq[2*k], Q=iq[2*k+1];
        ar += I*pr - Q*pi; ai += I*pi + Q*pr;
        double npr=pr*cr - pi*ci, npi=pr*ci + pi*cr; pr=npr; pi=npi;
    }
    *re=ar/n; *im=ai/n;
}
/* measure complex response at baseband offset off_hz, averaging navg blocks.
 *
 * IMPORTANT: we do NOT coherently sum the per-block DFT phasors. Each block is a
 * SEPARATE libiio refill and consecutive non-cyclic refills are not guaranteed to
 * be phase-contiguous (there can be an arbitrary sample gap between them), so the
 * tone's phase at each block start is effectively random. Summing those phasors is
 * a random walk -> the averaged magnitude bounces every frame (the "extremely
 * noisy" level). Instead we average POWER (|r|^2+|i|^2) across blocks, which is
 * phase-insensitive, so a stable CW tone yields a stable reading; the reported
 * phase is taken from the strongest single block (good enough for the level apps;
 * true vector phase needs one contiguous capture, a future rev).
 *
 * maia also re-parks the TX LO powerdown ~1 Hz; a parked block reads ~0 power, so
 * we re-clear powerdown before every block and drop blocks far below the strongest
 * before averaging. Returns 1 on success. */
static int vna_measure(long off_hz, int navg, double *re, double *im){
    static int16_t buf[VNA_N*2];
    if(!vna_rx_open()) return 0;
    if(navg<1) navg=1; if(navg>64) navg=64;
    tg_txlo_on();
    iq_rx_read(buf);                                  /* discard one (stale DMA) */
    static double p[64], pr[64], pi[64];
    int pk=0; double mx=0;
    for(int a=0;a<navg;a++){
        tg_txlo_on();                                 /* keep the stimulus LO up mid-capture */
        if(iq_rx_read(buf)<=0) return 0;
        for(int k=0;k<VNA_N;k++){ int i0=buf[2*k]<0?-buf[2*k]:buf[2*k], q0=buf[2*k+1]<0?-buf[2*k+1]:buf[2*k+1];
            if(i0>pk)pk=i0; if(q0>pk)pk=q0; }
        double r,i; vna_dft(buf,VNA_N,(double)off_hz,&r,&i);
        pr[a]=r; pi[a]=i; p[a]=r*r+i*i; if(p[a]>mx)mx=p[a];
    }
    pp_rx_ol_note(1, pk*100/iq_adc_fs);               /* ADC level -> status-bar meter */
    /* incoherent (power) average over non-dropout blocks; phase from the loudest */
    double psum=0; int nn=0, best=0;
    for(int a=0;a<navg;a++){
        if(navg<2 || p[a] >= 0.4*mx){ psum+=p[a]; nn++; }   /* skip TX-park dropouts */
        if(p[a] > p[best]) best=a;
    }
    if(nn<1){ psum=p[best]; nn=1; }
    double mag=sqrt(psum/nn), bmag=sqrt(pr[best]*pr[best]+pi[best]*pi[best]);
    if(bmag>1e-9){ *re=mag*pr[best]/bmag; *im=mag*pi[best]/bmag; }   /* mag from power avg, phase from loudest */
    else         { *re=mag; *im=0; }
    return 1;
}
#else  /* ------------------------- PP_SIM model ------------------------- */
static double vsim_pumpf=0, vsim_pumpp=-99;           /* two-tone app informs the model */
static void vna_sim_pump(double f_hz,double p_db){ vsim_pumpf=f_hz; vsim_pumpp=p_db; }
static int vna_measure(long off_hz, int navg, double *re, double *im){
    (void)navg;
    double rf = (double)g_vna_rxlo + (double)off_hz;  /* RF being probed */
    /* notch resonator at f0_sim; pump near a fake qubit freq shifts/loads it */
    double f0 = 5.0e9, Q = 8000.0, kappa = f0/Q;
    double shift = 0.0, extra_loss = 0.0;
    if(vsim_pumpp > -40){
        double qf = 4.7e9, det=(vsim_pumpf-qf)/2.0e6;
        double drive = pow(10.0,(vsim_pumpp+30)/20.0);
        shift = 1.5e5*drive/(1.0+det*det);            /* AC-Stark-ish pull */
        extra_loss = 0.4*drive/(1.0+det*det);
    }
    double x = 2.0*Q*(rf-(f0+shift))/f0;
    double denom = 1.0 + x*x;
    double s_re = 1.0 - (1.0+extra_loss)/denom;        /* notch dip */
    double s_im = -(1.0+extra_loss)*x/denom;
    double delay = 2.0*M_PI*rf*2.0e-9;                 /* 2 ns cable delay */
    double cr=cos(delay), ci=-sin(delay);
    double gr=s_re*cr - s_im*ci, gi=s_re*ci + s_im*cr;
    double amp = 1200.0*pow(10.0,(-g_vna_txatt)/20.0); /* TX level */
    double nz=((rand()&0xff)/255.0-0.5);
    *re = amp*gr + 8*nz; *im = amp*gi + 8*nz; (void)kappa; return 1;
}
#endif
#endif
