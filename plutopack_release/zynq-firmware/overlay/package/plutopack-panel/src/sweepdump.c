/* sweepdump — run pp_spectrum.c's EXACT sweep DSP against the real maia websocket
 * and dump the stitched trace TR[] as numbers, so the sawtooth can be measured
 * (not eyeballed). Includes the app's .c directly (main renamed) => identical code.
 * argv: [center_khz span_khz rbw_hz avg_n gain_db mode]  */
#define main pp_spectrum_main
#include "pp_spectrum.c"
#undef main

int main(int argc,char**argv){
    setbuf(stdout,NULL); setbuf(stderr,NULL);
    if(argc>1) center_khz=atol(argv[1]);
    if(argc>2) span_khz  =atol(argv[2]);
    if(argc>3) rbw_hz    =atol(argv[3]);
    if(argc>4) avg_n     =atol(argv[4]);
    if(argc>5) gain_db   =atol(argv[5]);
    if(argc>6) mode      =atol(argv[6]);
    int want_sweeps = (argc>7)?atoi(argv[7]):1;
    for(int x=0;x<PP_W;x++){ TR[x]=SP_FLOOR; MX[x]=SP_FLOOR; }
    int wsfd=maia_ws_open();
    printf("WSFD %d\n", wsfd);
    if(wsfd<0){ printf("MAIA_WS_OPEN_FAILED\n"); return 1; }
    sweep_setup();
    printf("CFG center=%ld span=%ld rbw=%ld avg=%ld gain=%ld mode=%ld\n",
           center_khz,span_khz,rbw_hz,avg_n,gain_db,mode);
    printf("SWEEP nsteps=%d fs=%lld pxhz=%lld if=%lld bin0=%d binw=%d time=%.1fs\n",
           sw_nsteps,sw_fs,sw_pxhz,sw_if,sw_bin0,sw_binw,sw_time);
    float fr[SP_N];
    int frames=0, gotframes=0, completed=0, iters=0;
    while(completed<want_sweeps && iters<2000000){
        int got=maia_ws_drain(wsfd,fr); iters++;
        if(got>0){ int before=sw_step; sweep_frame(fr); frames++; gotframes++;
                   if(sw_step<before) completed++; }
        else usleep(2000);
        if(iters%200==0) fprintf(stderr,"HB iters=%d gotframes=%d step=%d completed=%d\n",iters,gotframes,sw_step,completed);
    }
    printf("FRAMES %d GOTFRAMES %d COMPLETED_SWEEPS %d ITERS %d\n",frames,gotframes,completed,iters);
    printf("TRACE_BEGIN\n");
    for(int x=0;x<PP_W;x++) printf("%d %.2f\n", x, TR[x]);
    printf("TRACE_END\n");
    return 0;
}
