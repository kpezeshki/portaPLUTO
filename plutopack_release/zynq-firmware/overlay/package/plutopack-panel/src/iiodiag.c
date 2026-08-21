/* iiodiag — exercise the EXACT pp_iio.h code path on the board.
 * Not PP_SIM: uses the real sysfs backend. Prints which attr each sub resolves
 * to and the return/readback of every setter tonegen's do_retune() calls. */
#include <stdio.h>
#include "pp_iio.h"

static void show(const char*dir,const char*sub){
    char p[160];
    if(iio_find_attr(dir,sub,p)) printf("  %-34s -> %s\n",sub,p);
    else                          printf("  %-34s -> (NOT FOUND)\n",sub);
}
int main(void){
    int ntx=0; long fs=0;
    int ok=tg_init(&ntx,&fs);
    printf("tg_init=%d ntx=%d fs=%ld\n",ok,ntx,fs);
    if(!ok){ printf("tg_init FAILED (phy or dds device not found)\n"); return 1; }
    printf("phy=%s\ndds=%s\n",iio_phy,iio_dds);
    printf("== phy attr resolution ==\n");
    show(iio_phy,"TX_LO_frequency");
    show(iio_phy,"out_voltage_rf_bandwidth");
    show(iio_phy,"out_voltage_sampling_frequency");
    show(iio_phy,"out_voltage0_hardwaregain");
    printf("== dds attr resolution ==\n");
    show(iio_dds,"TX1_I_F1_frequency");
    show(iio_dds,"TX1_I_F1_scale");
    show(iio_dds,"TX1_I_F1_phase");
    printf("== do_retune() exact sequence ==\n");
    long long lo_got=0; int r;
    r=tg_lo((long long)2439000*1000,&lo_got); printf("tg_lo    ret=%d got=%lld\n",r,lo_got);
    long bwg=0,fsg=0;
    r=tg_bw(18000*1000L,&bwg); printf("tg_bw    ret=%d got=%ld\n",r,bwg);
    r=tg_fs(30720*1000L,&fsg); printf("tg_fs    ret=%d got=%ld\n",r,fsg);
    long offg=0; double attg=0;
    r=tg_tone(0, 1000*1000L, TG_AMP, 0, &offg); printf("tg_tone  ret=%d got=%ld\n",r,offg);
    r=tg_atten(0,20.0,&attg);                   printf("tg_atten ret=%d got=%f\n",r,attg);
    return 0;
}
