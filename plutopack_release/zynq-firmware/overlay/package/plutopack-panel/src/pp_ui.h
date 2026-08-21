/*
 * pp_ui.h — shared front-panel UI framework (PlutoPack UI style spec).
 *   Implements the ONE input grammar every app follows:
 *     - screens : DATA  <--BTN8/BTN7-->  SETTINGS   (BTN7 on DATA exits to switcher)
 *     - states  : BROWSE (pick a field) <--CENTER--> EDIT (change its value)
 *     - BROWSE  : UP/DOWN/WHEEL cycle the focused field
 *     - EDIT    : LEFT/RIGHT move the digit cursor; UP/DOWN/WHEEL change the value
 *     - BTN7    : layered back (EDIT->BROWSE, SETTINGS->DATA, DATA->exit)
 *   Plus the value sync rule (settle -> apply -> read-back -> retry; amber until
 *   confirmed) and the common chrome (status bar, field band, settings list,
 *   footer hints).  Apps supply their fields + an optional data-area draw.
 *
 * See claude_notes/plutopack_review/plutopack_ui_style_spec.md.
 */
#ifndef PP_UI_H
#define PP_UI_H
#include <math.h>
#include "pp_common.h"

#define C_UNIT      pp_rgb(0xff,0x7a,0x2f)   /* orange: units */
#define PP_SYNC_RETRY 33                     /* ~1s @30ms: backoff before retry */

enum { PP_DATA=0, PP_SETTINGS=1 };
enum { PP_UI_NONE=0, PP_UI_REDRAW=1, PP_UI_EXIT=2 };

typedef struct pp_field pp_field_t;
struct pp_field {
    const char *name;
    const char *unit;                 /* "MHz"/"kHz"/"dB"/"" */
    long *val, vmin, vmax;
    const long *places;               /* digit place weights, largest-first */
    int   nplace, sel;                /* cursor over places[]               */
    /* value -> display string; if NULL, prints the integer value */
    void (*fmt)(const pp_field_t*, char*, int);
    /* push a settled value to hardware; MAY reconcile *val to the applied
     * (read-back) value. return 1 on confirmed success, 0 on failure.
     * NULL => local-only field (always considered synced). */
    int  (*apply)(pp_field_t*);
    /* optional widget drawn in the field band, right of the unit */
    void (*widget)(const pp_field_t*, uint16_t*, int x, int y);
    /* sync bookkeeping (managed by pp_ui_sync) */
    long sent, prevv; int synced, backoff;
    void (*action)(void);             /* if set, CENTER fires this (in browse) instead of editing */
    int  linear;                      /* 1 = slider/marker field: in edit, encoder AND L/R both
                                       * move *val by places[0] (L/R coarse); no digit cursor    */
};

typedef struct {
    pp_field_t *f; int nf;            /* the app's fields                    */
    int ndata;                        /* first ndata fields appear on DATA;
                                       * SETTINGS lists all nf (ndata<=nf)    */
    int screen, focus, editing;
    int32_t last_enc; int enc_init, enc_accum;
    uint8_t last_btn;
    int confirm_exit;                 /* exit-to-menu confirmation modal is up */
} pp_ui_t;

static void pp_ui_init(pp_ui_t *u, pp_field_t *f, int nf){
    memset(u,0,sizeof *u);
    u->f=f; u->nf=nf; u->ndata=nf; u->screen=PP_DATA; u->focus=0; u->editing=0;
    for(int i=0;i<nf;i++){ f[i].sent=*f[i].val; f[i].prevv=*f[i].val; f[i].synced=1; f[i].backoff=0; }
}

/* ---- input: fold one decoded Pico status frame into the UI state ---- */
static int pp_ui_input(pp_ui_t *u, const pp_status_t *st){
    if(!u->enc_init){ u->last_enc=st->enc_pos; u->enc_init=1; }
    u->enc_accum += (int)(st->enc_pos - u->last_enc); u->last_enc=st->enc_pos;
    int wheel = u->enc_accum/4; u->enc_accum -= wheel*4;    /* 4 counts/detent */
    uint8_t pressed = st->btn_bitmap & ~u->last_btn; u->last_btn = st->btn_bitmap;
    int up = !!(pressed&PP_BTN_UP), down = !!(pressed&PP_BTN_DOWN);
    int redraw=0;

    if(u->confirm_exit){                             /* exit-confirmation modal */
        if(pressed&PP_BTN_CENTER){ u->confirm_exit=0; return PP_UI_EXIT; }    /* confirm -> menu */
        if(pressed&PP_BTN_7){ u->confirm_exit=0; return PP_UI_REDRAW; }       /* cancel -> app  */
        return PP_UI_NONE;                           /* swallow everything else */
    }
    if(pressed&PP_BTN_7){                            /* layered back */
        if(u->editing){ u->editing=0; return PP_UI_REDRAW; }
        if(u->screen==PP_SETTINGS){ u->screen=PP_DATA;
            if(u->focus>=u->ndata) u->focus=0;       /* keep focus on a DATA field */
            return PP_UI_REDRAW; }
        u->confirm_exit=1; return PP_UI_REDRAW;      /* DATA back -> confirm exit to menu */
    }
    if((pressed&PP_BTN_8) && u->screen==PP_DATA){ u->screen=PP_SETTINGS; u->editing=0; return PP_UI_REDRAW; }
    if(pressed&PP_BTN_CENTER){
        pp_field_t *cf = (u->nf>0)? &u->f[u->focus] : 0;
        if(cf && cf->action && !u->editing){ cf->action(); redraw=1; }   /* action field */
        else { u->editing=!u->editing; redraw=1; }
    }

    if(u->nf>0){
        pp_field_t *f=&u->f[u->focus];
        if(u->editing && f->linear){
            /* marker/slider: encoder & L/R both move the value (L/R = coarse); no digit cursor */
            int fine = up - down + wheel;
            int coarse = !!(pressed&PP_BTN_RIGHT) - !!(pressed&PP_BTN_LEFT);
            long step = f->places[0];
            long nv = *f->val + (long)fine*step + (long)coarse*step*8;
            if(nv<f->vmin)nv=f->vmin; if(nv>f->vmax)nv=f->vmax;
            if(nv!=*f->val){ *f->val=nv; redraw=1; }
        }else if(u->editing){
            int vd = up - down + wheel;              /* UP/wheel+ = increase */
            if(vd){ long nv=*f->val + (long)vd*f->places[f->sel];
                    if(nv<f->vmin)nv=f->vmin; if(nv>f->vmax)nv=f->vmax; *f->val=nv; redraw=1; }
            if(pressed&PP_BTN_LEFT ){ if(f->sel>0)          f->sel--; redraw=1; }
            if(pressed&PP_BTN_RIGHT){ if(f->sel<f->nplace-1)f->sel++; redraw=1; }
        }else{
            int n = (u->screen==PP_DATA)?u->ndata:u->nf; if(n<1) n=1;
            int d = down - up + wheel;                /* DOWN/wheel+ = next field */
            if(d){ u->focus=((u->focus+d)%n+n)%n; redraw=1; }
        }
    }
    return redraw?PP_UI_REDRAW:PP_UI_NONE;
}

/* ---- sync: push settled changes, verify, retry with backoff ---- */
static void pp_ui_sync(pp_ui_t *u){
    for(int i=0;i<u->nf;i++){
        pp_field_t *f=&u->f[i];
        if(!f->apply){ f->synced=1; continue; }      /* local-only */
        long v=*f->val;
        if(v==f->prevv){                             /* settled this loop */
            if(v!=f->sent){
                if(f->backoff>0){ f->backoff--; f->synced=0; }
                else if(f->apply(f)){                 /* apply may reconcile *val */
                    f->sent=*f->val; f->prevv=*f->val; f->synced=1; f->backoff=0;
                }else{ f->synced=0; f->backoff=PP_SYNC_RETRY; }
            }else f->synced=1;
        }
        f->prevv=v;
    }
}
/* reconcile an idle, in-sync field to an externally-read value (periodic re-read) */
static void pp_ui_field_extern(pp_field_t *f, long applied){
    if(*f->val==f->sent && f->backoff==0 && applied!=*f->val){
        *f->val=applied; f->sent=applied; f->prevv=applied;
    }
}
static int pp_ui_all_synced(const pp_ui_t *u){
    for(int i=0;i<u->nf;i++) if(!u->f[i].synced) return 0; return 1;
}

/* ---------------------------- rendering ---------------------------------- */
/* char index of the digit representing `place` (a power of 10), aligned from the
 * RIGHT so it tracks the actual place edited even when the string is shorter than
 * the places[] array (e.g. plain %ld with no leading zeros). */
static int pp_ui_digbox(const char *s, long place){
    int e=0; long p=place>0?place:1; while(p>1){ p/=10; e++; }   /* e = exponent of place */
    int cnt=0;
    for(int i=(int)strlen(s)-1;i>=0;i--)                          /* e-th digit from the right */
        if(s[i]>='0'&&s[i]<='9'){ if(cnt==e) return i; cnt++; }
    for(int i=0;s[i];i++) if(s[i]>='0'&&s[i]<='9') return i;      /* place beyond width: leftmost */
    return 0;
}
static void pp_ui_fmtval(const pp_field_t *f, char *v, int n){
    if(f->fmt) f->fmt(f,v,n); else snprintf(v,n,"%ld",*f->val);
}

/* persistent 20px status bar: brand left, note, AD9361 temp, battery right */
static void pp_ui_status_bar(uint16_t *buf, const char *brand, const pp_status_t *st,
                             int link_ok, const char *note, uint16_t notecol){
    char s[16];
    pp_fill(buf, 0,0, PP_W,20, C_PANEL);
    if(brand) pp_text(buf, 5,6, brand, C_ACCENT, 1);
    else      pp_logo(buf, 5,6, C_ACCENT, 1);          /* NULL brand -> product logo */
    /* temperatures (right to left): RFIC ('RF') rightmost, then Zynq PS ('AP'),
     * so the RX meter can sit flush to their left. Both grey (like the launcher). */
    char ts[16]; int tx=PP_W-78;                        /* % text moved left -> padding to the icon */
    int rf=pp_ad9361_temp_c();
    if(rf>-200){ snprintf(ts,sizeof ts,"RF%dC",rf); tx -= 5+pp_tw(ts,1); pp_text(buf, tx,6, ts, C_DIM,1); }
    int ap=pp_zynq_temp_c();
    if(ap>-200){ snprintf(ts,sizeof ts,"AP%dC",ap); tx -= 5+pp_tw(ts,1); pp_text(buf, tx,6, ts, C_DIM,1); }
    /* log-scale RX1/RX2 headroom meter, right-aligned just left of the temps.
     * green->amber->red; RX2 greyed unless fed. */
    int xleft=tx;                                       /* leftmost x of the right-hand block */
    if(pp_rx_lvl_ttl[0]>0 || pp_rx_lvl_ttl[1]>0){
        int bw=32, bxr=tx-5, bx=bxr-bw; double wdb=-99.0; int any=0;  /* two stacked 6px bars, 5px gap */
        for(int c=0;c<2;c++){
            int by=3+c*7, bh=6, used=pp_rx_lvl_ttl[c]>0; uint16_t col=C_DIM; int fw=0;
            if(used){
                int pct=pp_rx_lvl[c];
                double dbfs = pct>0 ? 20.0*log10((double)pct/100.0) : -99.0;   /* peak dBFS */
                double f=(dbfs+42.0)/42.0; if(f<0)f=0; if(f>1)f=1;             /* -42..0 dBFS window */
                fw=(int)(f*(bw-2)+0.5);
                col = dbfs>=-3.0?C_BAD:(dbfs>=-12.0?C_WARN:C_GOOD);
                if(dbfs>wdb)wdb=dbfs; any=1; pp_rx_lvl_ttl[c]--;
            }
            pp_rect(buf, bx,by, bw,bh, col);
            if(fw>0) pp_fill(buf, bx+1,by+1, fw, bh-2, col);
        }
        (void)wdb; (void)any;
        pp_text(buf, bx-20, 6, "RX", C_TEXT, 1);       /* label white, padded left of the bars */
        xleft = bx-20;
    }
    /* note in the left-centre area; slide it left of the right-hand block if that
     * block (temps + RX meter) has grown far enough left to collide, clamped to
     * sit just right of the brand/logo so it never runs under either. */
    if(note){ int nw=pp_tw(note,1), nx=100;
        if(nx+nw > xleft-4) nx = xleft-4-nw;
        int bmin = 5 + (brand?pp_tw(brand,1):10*8) + 6;
        if(nx < bmin) nx = bmin;
        pp_text(buf, nx,6, note, notecol, 1); }
    int bp = link_ok ? (st->soc>0?st->soc:(st->batt_mV?(st->batt_mV-3300)/9:0)) : 0;
    if(bp<0)bp=0; if(bp>100)bp=100;
    snprintf(s,sizeof s,"%3d%%",bp); pp_text(buf, PP_W-78,6, s, C_TEXT,1);
    int gx=PP_W-40; uint16_t bc=bp>=40?C_GOOD:(bp>=15?C_WARN:C_BAD);
    pp_rect(buf,gx,5,22,11,C_DIM); pp_fill(buf,gx+22,8,2,5,C_DIM);
    pp_fill(buf,gx+2,7,(22-4)*bp/100,7,bc);
    if(link_ok&&st->charging) pp_text(buf,gx+26,5,"/",C_WARN,1);
}

/* field band (DATA screen): NAME  VALUE(boxed digit while editing)  UNIT  [widget] */
static void pp_ui_draw_field_band(uint16_t *buf, const pp_field_t *f, int editing, int y0){
    pp_fill(buf, 0,y0, PP_W,28, C_PANEL2);
    pp_text(buf, 4, y0+10, f->name, f->synced?C_ACCENT:C_WARN, 1);   /* amber = unconfirmed */
    char v[24]; pp_ui_fmtval(f,v,sizeof v);
    int sc=2, cw=8*sc, x0=56, vy=y0+5, selc=(editing&&!f->linear)?pp_ui_digbox(v,f->places[f->sel]):-1;
    for(int i=0; v[i]; i++){ int cx=x0+i*cw;
        if(i==selc){ pp_fill(buf,cx-1,vy-2,cw+2,8*sc+4,C_ACCENT); pp_ch(buf,cx,vy,v[i],C_BG,sc); }
        else pp_ch(buf,cx,vy,v[i],C_TEXT,sc); }
    int ux=x0+(int)strlen(v)*cw+6;
    if(f->unit && f->unit[0]){ pp_text(buf, ux, y0+10, f->unit, C_UNIT, 1); ux += (int)strlen(f->unit)*8+6; }
    if(f->widget) f->widget(f, buf, ux, y0);
}

/* settings screen: scrollable list of every field, same edit model */
static void pp_ui_draw_settings(uint16_t *buf, const pp_ui_t *u, int y0, int h){
    pp_fill(buf, 0,y0, PP_W, h, C_BG);
    pp_text(buf, 6, y0, "SETTINGS", C_DIM, 1);
    int rh=22, top=y0+14, rows=(h-14)/rh;
    int first=0; if(u->focus>=rows) first=u->focus-rows+1;    /* keep focus visible */
    for(int r=0; r<rows && first+r<u->nf; r++){
        int i=first+r, y=top+r*rh; const pp_field_t *f=&u->f[i];
        int foc=(i==u->focus);
        pp_fill(buf, 6,y, PP_W-12,rh-2, foc?C_PANEL:C_PANEL2);
        pp_rect(buf, 6,y, PP_W-12,rh-2, foc?C_ACCENT:C_PANEL);
        if(foc) pp_fill(buf, 6,y, 4,rh-2, C_ACCENT);          /* selection rail */
        pp_text(buf, 16, y+6, f->name, f->synced?C_TEXT:C_WARN, 1);
        char v[24]; pp_ui_fmtval(f,v,sizeof v);
        int editing=foc&&u->editing, selc=(editing&&!f->linear)?pp_ui_digbox(v,f->places[f->sel]):-1;
        int ux=PP_W-24-(int)strlen(v)*8-((f->unit&&f->unit[0])?(int)strlen(f->unit)*8+4:0);
        for(int k=0; v[k]; k++){ int cx=ux+k*8;
            if(k==selc){ pp_fill(buf,cx-1,y+4,10,10,C_ACCENT); pp_ch(buf,cx,y+5,v[k],C_BG,1); }
            else pp_ch(buf,cx,y+5,v[k],C_TEXT,1); }
        if(f->unit&&f->unit[0]) pp_text(buf, ux+(int)strlen(v)*8+4, y+6, f->unit, C_UNIT,1);
    }
}

/* context-sensitive footer hint bar (also hosts the exit-confirmation overlay) */
static void pp_ui_footer(uint16_t *buf, pp_ui_t *u){
    const char *h;
    if(u->confirm_exit)           h="CENTER exit   BACK cancel";
    else if(u->editing && u->nf>0 && u->f[u->focus].linear) h="turn / L-R move   CENTER ok  BACK";
    else if(u->editing)           h="L/R digit  turn value  CENTER ok  BACK";
    else if(u->screen==PP_SETTINGS) h="UP/DN move  CENTER edit  BACK";
    else                          h="UP/DN field  CENTER edit  SETTINGS BACK";
    pp_fill(buf, 0,PP_H-14, PP_W,14, C_PANEL2);
    pp_text(buf, 4,PP_H-12, h, C_DIM, 1);
    pp_confirm_overlay(buf, u->confirm_exit);        /* shared overlay, on top of all */
}

/* ------------------------- touch marker (SA / SNA / VNA) ----------------- *
 * A touch-set frequency marker for the swept-graph apps: touch the plot to drop
 * a dashed vertical line; the app annotates it with the trace value (top row)
 * and frequency (bottom row). Drawn in a distinct colour so it stands apart
 * from the data trace and the axes/grid.
 *
 * Label placement is adaptive so it stays on-screen and off the data:
 *   - the box sits LEFT of the line (right-aligned) when the line is on the
 *     right half of the screen, else RIGHT of the line (left-aligned);
 *   - it sits at the TOP or BOTTOM edge of the label region, whichever is
 *     farther from the trace point (trace_y), to minimise covering the trace.
 * The dashed line may span a larger area than the label region (e.g. VNA draws
 * it across both the magnitude and phase plots while the label stays in mag). */
#define C_MARKER pp_rgb(0xff,0x3a,0xd6)          /* magenta: distinct from trace/axes */
typedef struct { int active, px; } pp_marker_t;

/* set/move the marker from a touch inside the plot rect (call each DATA frame) */
static void pp_marker_update(pp_marker_t *m, const pp_status_t *st,
                             int x0, int y0, int w, int h){
    if(!st->touch_down) return;                       /* touch_ok flag may be unset even when touch works */
    int sx, sy; pp_touch_map((int)st->touch_x, (int)st->touch_y, &sx, &sy);
    if(sx>=x0 && sx<x0+w && sy>=y0 && sy<y0+h){ m->active=1; m->px=sx; }
}

static void pp_marker_draw(uint16_t *buf, const pp_marker_t *m,
                           int line_y0, int line_y1, int lab_y0, int lab_y1,
                           int trace_y, const char *val, const char *freq){
    if(!m->active) return;
    int x=m->px; if(x<0)x=0; if(x>PP_W-1)x=PP_W-1;
    for(int y=line_y0; y<line_y1; y+=4) pp_fill(buf,x,y,1,2,C_MARKER);   /* dashed */
    int tw1=pp_tw(val,1), tw2=pp_tw(freq,1), tw=tw1>tw2?tw1:tw2, pad=3;
    int bw=tw+2*pad, bh=2*8+3*pad;
    int left = (x > PP_W/2);                          /* right-half line -> label left */
    int bx = left ? (x-2-bw) : (x+3);
    if(bx<0) bx=0; if(bx+bw>PP_W) bx=PP_W-bw;
    int at_top = (trace_y > (lab_y0+lab_y1)/2);       /* trace low -> label at top */
    int by = at_top ? (lab_y0+2) : (lab_y1-2-bh);
    if(by<lab_y0) by=lab_y0; if(by+bh>lab_y1) by=lab_y1-bh; if(by<0) by=0;
    pp_fill(buf,bx,by,bw,bh,C_PANEL); pp_rect(buf,bx,by,bw,bh,C_MARKER);
    int vx = left ? (bx+bw-pad-tw1) : (bx+pad);       /* right-align on the left side */
    int fx = left ? (bx+bw-pad-tw2) : (bx+pad);
    pp_text(buf, vx, by+pad,   val,  C_TEXT,   1);    /* value on top   */
    pp_text(buf, fx, by+pad+9, freq, C_MARKER, 1);    /* frequency below */
}
#endif
