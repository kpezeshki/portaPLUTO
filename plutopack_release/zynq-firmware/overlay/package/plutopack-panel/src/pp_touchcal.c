/*
 * pp-touchcal — touchscreen calibration. Tap the crosshairs in turn; the app
 * least-squares-fits an affine raw->pixel transform and writes it to
 * /tmp/pp_touchcal, which every app then uses for touch input (pp_touch_map in
 * pp_common.h). The Pico already delivers touch coordinates rotated to the
 * display orientation, so the fit here corrects residual scale/offset/skew.
 *
 * Non-persistent by design: the cal lives in tmpfs and clears on reboot, so
 * re-run after a power cycle.
 *   collecting: tap each crosshair; BTN7 cancels back to the launcher
 *   after save: tap anywhere to test the mapping; CENTER re-runs; BTN7 exits
 * usage: pp-touchcal [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include "pp_common.h"
#include <math.h>

#define NP 5
static const int TGT[NP][2] = {
    { 40, 30 }, { 280, 30 }, { 280, 210 }, { 40, 210 }, { 160, 120 },
};

/* solve a 3x3 linear system M x = b (Gaussian elimination, partial pivot) */
static int solve3(double M[3][3], double b[3], double out[3]){
    double a[3][4];
    for(int i=0;i<3;i++){ for(int j=0;j<3;j++) a[i][j]=M[i][j]; a[i][3]=b[i]; }
    for(int c=0;c<3;c++){
        int p=c; for(int r=c+1;r<3;r++) if(fabs(a[r][c])>fabs(a[p][c])) p=r;
        if(fabs(a[p][c])<1e-9) return -1;
        if(p!=c) for(int j=0;j<4;j++){ double t=a[c][j]; a[c][j]=a[p][j]; a[p][j]=t; }
        for(int r=0;r<3;r++) if(r!=c){ double f=a[r][c]/a[c][c];
            for(int j=c;j<4;j++) a[r][j]-=f*a[c][j]; }
    }
    for(int i=0;i<3;i++) out[i]=a[i][3]/a[i][i];
    return 0;
}

static void draw_cross(uint16_t *buf, int x, int y, uint16_t c){
    pp_fill(buf, x-10, y, 21, 1, c);
    pp_fill(buf, x, y-10, 1, 21, c);
    pp_rect(buf, x-4, y-4, 9, 9, c);
}

int main(int argc, char **argv){
    const char *spidev = argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev  = argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;

    int idx=0, done=0, saved_ok=0, was_down=0;
    double rawx[NP], rawy[NP], acc_x=0, acc_y=0; long acc_n=0;

    for(;;){
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn = st.btn_bitmap;
            if(pressed & PP_BTN_7) return 0;                 /* exit / cancel */
            if(done && (pressed & PP_BTN_CENTER)){           /* recalibrate */
                idx=0; done=0; saved_ok=0; was_down=0; acc_n=0; acc_x=acc_y=0;
            }
            if(!done){
                if(st.touch_down){
                    acc_x += st.touch_x; acc_y += st.touch_y; acc_n++; was_down=1;
                } else if(was_down){                          /* release -> record */
                    was_down=0;
                    if(acc_n>=3){ rawx[idx]=acc_x/acc_n; rawy[idx]=acc_y/acc_n; idx++; }
                    acc_x=acc_y=0; acc_n=0;
                    if(idx>=NP){                              /* fit affine raw->screen */
                        double M[3][3]={{0}}, bx[3]={0}, by[3]={0}, ax[3], ay[3];
                        for(int i=0;i<NP;i++){
                            double X=rawx[i], Y=rawy[i];
                            M[0][0]+=X*X; M[0][1]+=X*Y; M[0][2]+=X;
                            M[1][1]+=Y*Y; M[1][2]+=Y;
                            bx[0]+=X*TGT[i][0]; bx[1]+=Y*TGT[i][0]; bx[2]+=TGT[i][0];
                            by[0]+=X*TGT[i][1]; by[1]+=Y*TGT[i][1]; by[2]+=TGT[i][1];
                        }
                        M[1][0]=M[0][1]; M[2][0]=M[0][2]; M[2][1]=M[1][2]; M[2][2]=NP;
                        if(solve3(M,bx,ax)==0 && solve3(M,by,ay)==0){
                            FILE *f=fopen(PP_TOUCHCAL_PATH,"w");
                            if(f){ fprintf(f,"%.8g %.8g %.8g %.8g %.8g %.8g\n",
                                          ax[0],ax[1],ax[2],ay[0],ay[1],ay[2]);
                                   fclose(f); saved_ok=1;
                                   pp_tcal_ok=0; pp_tcal_tried=0; }  /* force reload */
                        }
                        done=1;
                    }
                }
            }
        }

        /* ---- render ---- */
        pp_fill(buf,0,0,PP_W,PP_H,C_BG);
        pp_text(buf, 8, 8, "TOUCH CAL", C_ACCENT, 2);
        if(!done){
            char l[48]; snprintf(l,sizeof l,"Tap the crosshair    %d/%d", idx+1, NP);
            pp_text(buf, 8, 30, l, C_TEXT, 1);
            pp_text(buf, 8, PP_H-14, "BTN7: cancel", C_DIM, 1);
            draw_cross(buf, TGT[idx][0], TGT[idx][1], st.touch_down?C_GOOD:C_WARN);
        } else if(saved_ok){
            pp_text(buf, 8, 30, "Saved. Tap to test.", C_GOOD, 1);
            if(st.touch_down){
                int sx,sy; pp_touch_map(st.touch_x, st.touch_y, &sx, &sy);
                draw_cross(buf, sx, sy, C_ACCENT);
                char l[48]; snprintf(l,sizeof l,"x=%d  y=%d", sx, sy);
                pp_text(buf, 8, 48, l, C_TEXT, 1);
            }
            pp_text(buf, 8, PP_H-14, "CENTER: redo   BTN7: exit", C_DIM, 1);
        } else {
            pp_text(buf, 8, 30, "Calibration failed - retry.", C_BAD, 1);
            pp_text(buf, 8, PP_H-14, "CENTER: redo   BTN7: exit", C_DIM, 1);
        }
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
