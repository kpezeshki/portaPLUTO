/* pp_mock.c — render sample PlutoPack screens to PPM using the shared renderer.
 * Build: cc pp_mock.c pp_font.c -o pp_mock ; ./pp_mock outdir */
#include "pp_screen.h"
#include <stdlib.h>

static void write_ppm(const char *path, const uint16_t *fb) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", PP_W, PP_H);
    for (int i = 0; i < PP_W*PP_H; i++) {
        uint16_t p = fb[i];
        uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((p >> 5)  & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
        fputc(r,f); fputc(g,f); fputc(b,f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static uint16_t fb[PP_W*PP_H];
    char path[256];

    pp_status_t s;

    /* scene 1: nominal idle, on battery */
    memset(&s,0,sizeof s);
    s.gauge_ok=1; s.touch_ok=1; s.charging=0;
    s.batt_mV=3712; s.soc=74; s.current_mA=-142; s.enc_pos=12;
    s.btn_bitmap=0; s.touch_down=0; s.seq=418;
    pp_render(fb,&s); snprintf(path,sizeof path,"%s/scene1_idle.ppm",dir); write_ppm(path,fb);

    /* scene 2: charging, two buttons held, encoder spun */
    memset(&s,0,sizeof s);
    s.gauge_ok=1; s.touch_ok=1; s.charging=1;
    s.batt_mV=4051; s.soc=88; s.current_mA=+305; s.enc_pos=-37;
    s.btn_bitmap=(1<<(4-1))|(1<<(7-1)); s.touch_down=0; s.seq=982;
    pp_render(fb,&s); snprintf(path,sizeof path,"%s/scene2_active.ppm",dir); write_ppm(path,fb);

    /* scene 3: touch active with coordinates */
    memset(&s,0,sizeof s);
    s.gauge_ok=1; s.touch_ok=1; s.charging=0;
    s.batt_mV=3690; s.soc=71; s.current_mA=-168; s.enc_pos=12;
    s.btn_bitmap=(1<<(1-1)); s.touch_down=1; s.touch_x=1180; s.touch_y=2600; s.touch_z=430; s.seq=1203;
    pp_render(fb,&s); snprintf(path,sizeof path,"%s/scene3_touch.ppm",dir); write_ppm(path,fb);

    /* scene 4: low battery warning */
    memset(&s,0,sizeof s);
    s.gauge_ok=1; s.touch_ok=1; s.charging=0;
    s.batt_mV=3340; s.soc=12; s.current_mA=-540; s.enc_pos=5;
    s.btn_bitmap=0; s.touch_down=0; s.seq=77;
    pp_render(fb,&s); snprintf(path,sizeof path,"%s/scene4_lowbatt.ppm",dir); write_ppm(path,fb);

    /* scene 5: gauge not responding (sensor fault) */
    memset(&s,0,sizeof s);
    s.gauge_ok=0; s.touch_ok=1; s.charging=0;
    s.batt_mV=0; s.soc=0; s.current_mA=0; s.enc_pos=0;
    s.btn_bitmap=0; s.touch_down=0; s.seq=5;
    pp_render(fb,&s); snprintf(path,sizeof path,"%s/scene5_fault.ppm",dir); write_ppm(path,fb);

    return 0;
}
