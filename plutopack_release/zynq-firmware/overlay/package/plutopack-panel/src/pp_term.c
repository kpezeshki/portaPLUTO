/*
 * pp-term — on-panel root command runner (no serial cable needed).
 *
 *   Type a shell command with the on-screen keyboard, ENTER runs it through
 *   /bin/sh -c with stdout+stderr streamed live into a scrollable pane. This is
 *   a COMMAND RUNNER, not a full VT100 terminal: one-shot commands work (cat,
 *   ls, echo>sysfs, dmesg, iio tools, /etc/init.d/... restart), but full-screen
 *   interactive programs (vi/top) are not emulated (their ANSI escapes are
 *   stripped). Runs as root, like every panel app.
 *
 *   Input (Pico buttons + encoder, or the touchscreen):
 *     wheel / D-pad  move the key selector (grid)
 *     CENTER         press the selected key
 *     touch          tap a key (or preset) to press it directly; tap the top/
 *                    bottom of the output pane to page up/down
 *     BTN8           cycle keyboard layer: letters -> symbols -> presets
 *     BTN7           cancel a running command (SIGINT, then SIGKILL); when idle, exit
 *   Control keys (shared bottom row of both typing layers):
 *     SPACE BKSP ENTER TAB CLR  and  PgU/PgD (scroll the output pane)
 *   Presets layer: quick canned commands + recent history; selecting one loads
 *   it onto the input line and jumps the selector to ENTER (press CENTER to run).
 *
 * usage: pp-term [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/wait.h>
#include "pp_ui.h"

/* ---- layout ---- */
#define OUT_Y0   22
#define OUT_LH   9
#define OUT_ROWS 11                 /* 22..22+11*9 = 121 */
#define INP_Y    124
#define KB_Y0    140
#define KB_COLS  10
#define KB_ROWS  4
#define KB_CW    32                 /* 10*32 = 320 */
#define KB_CH    25                 /* 140..240 */
#define NCELL    (KB_COLS*KB_ROWS)
#define DISP_COLS 39                /* chars across the output pane */

/* ---- scrollback ---- */
#define SB_LINES 400
#define SB_COLS  42
static char sb[SB_LINES][SB_COLS];
static int  sb_head=0, sb_count=1, sb_col=0, sb_scroll=0;
static int  esc_state=0;            /* ANSI escape stripper: 0 none, 1 ESC, 2 CSI */

static void sb_advance(void){
    int n=(sb_head+1)%SB_LINES; sb[n][0]=0; sb_head=n; sb_col=0;
    if(sb_count<SB_LINES) sb_count++;
    if(sb_scroll>0) sb_scroll++;     /* hold the user's scroll position while streaming */
}
static void sb_putc(char c){
    if(c=='\r') return;
    if(c=='\n'){ sb_advance(); return; }
    if(c=='\t'){ do sb_putc(' '); while(sb_col%4); return; }
    if(c<0x20 || c>0x7e) c='?';
    if(sb_col>=DISP_COLS) sb_advance();
    sb[sb_head][sb_col++]=c; sb[sb_head][sb_col]=0;
}
static void feed(char c){            /* strip CSI escape sequences, then store */
    if(esc_state==1){ esc_state=(c=='[')?2:0; return; }
    if(esc_state==2){ if(c>=0x40 && c<=0x7e) esc_state=0; return; }
    if(c==0x1b){ esc_state=1; return; }
    sb_putc(c);
}
static void feed_str(const char*s){ while(*s) feed(*s++); }

/* ---- input line + command state ---- */
#define CMDMAX 160
static char cmd[CMDMAX]; static int clen=0;
static void cmd_ins(char c){ if(clen<CMDMAX-1){ cmd[clen++]=c; cmd[clen]=0; } }
static void cmd_bksp(void){ if(clen>0) cmd[--clen]=0; }

/* ---- history ---- */
#define HN 24
static char hist[HN][CMDMAX]; static int hist_n=0;
static void hist_push(const char*c){
    if(!c[0]) return;
    if(hist_n>0 && !strcmp(hist[hist_n-1],c)) return;    /* skip immediate dup */
    if(hist_n<HN){ strncpy(hist[hist_n],c,CMDMAX-1); hist[hist_n][CMDMAX-1]=0; hist_n++; }
    else { memmove(hist[0],hist[1],(HN-1)*CMDMAX); strncpy(hist[HN-1],c,CMDMAX-1); hist[HN-1][CMDMAX-1]=0; }
}

/* ---- child process (streaming) ---- */
static pid_t child_pid=-1; static int child_fd=-1, sent_int=0;
static void run_cmd(const char*c){
    if(child_pid>0 || !c[0]) return;
    int pf[2]; if(pipe(pf)<0){ feed_str("[pipe failed]\n"); return; }
    pid_t p=fork();
    if(p<0){ close(pf[0]); close(pf[1]); feed_str("[fork failed]\n"); return; }
    if(p==0){
        close(pf[0]); dup2(pf[1],1); dup2(pf[1],2); close(pf[1]);
        setpgid(0,0);                                    /* own group -> kill whole pipeline */
        execl("/bin/sh","sh","-c",c,(char*)NULL); _exit(127);
    }
    close(pf[1]); child_fd=pf[0]; child_pid=p; sent_int=0;
    int fl=fcntl(child_fd,F_GETFL,0); fcntl(child_fd,F_SETFL,fl|O_NONBLOCK);
    sb_scroll=0;                                         /* jump to the live tail */
    feed_str("$ "); feed_str(c); feed('\n');
}
static void child_poll(void){
    if(child_fd<0) return;
    char b[4096]; int total=0;
    for(;;){
        ssize_t n=read(child_fd,b,sizeof b);
        if(n>0){ for(ssize_t i=0;i<n;i++) feed(b[i]); total+=n; if(total>=16384) break; }
        else if(n==0){                                   /* EOF: child done */
            int ws=0; waitpid(child_pid,&ws,0);
            char t[32];
            if(WIFEXITED(ws)) snprintf(t,sizeof t,"[exit %d]\n",WEXITSTATUS(ws));
            else if(WIFSIGNALED(ws)) snprintf(t,sizeof t,"[signal %d]\n",WTERMSIG(ws));
            else snprintf(t,sizeof t,"[done]\n");
            feed_str(t);
            close(child_fd); child_fd=-1; child_pid=-1; sent_int=0; break;
        } else { break; }                                /* EAGAIN: no data this frame */
    }
}
static void child_cancel(void){
    if(child_pid<=0) return;
    if(!sent_int){ kill(-child_pid,SIGINT); sent_int=1; feed_str("[^C]\n"); }
    else          { kill(-child_pid,SIGKILL); feed_str("[killed]\n"); }
}

/* ---- keyboard model ---- */
enum { A_CH=0, A_SPACE, A_BKSP, A_ENTER, A_TAB, A_CLR, A_PGUP, A_PGDN };
typedef struct { char ch; unsigned char act; const char *lab; } cell_t;
static cell_t LET[NCELL], SYM[NCELL];

static void ctrl_row(cell_t *g){     /* indices 30..39, shared by both typing layers */
    struct { int i; int act; const char*l; } c[]={
        {30,A_SPACE,"SPC"},{31,A_SPACE,"SPC"},{32,A_BKSP,"BS"},{33,A_BKSP,"BS"},
        {34,A_ENTER,"ENT"},{35,A_ENTER,"ENT"},{36,A_TAB,"TAB"},{37,A_CLR,"CLR"},
        {38,A_PGUP,"PgU"},{39,A_PGDN,"PgD"} };
    for(unsigned k=0;k<sizeof c/sizeof c[0];k++){ g[c[k].i].ch=0; g[c[k].i].act=c[k].act; g[c[k].i].lab=c[k].l; }
}
static void kb_build(void){
    const char *L="abcdefghijklmnopqrstuvwxyz";     /* 26 -> cells 0..25   */
    const char *Lt="/.-_";                          /*        cells 26..29 */
    const char *S="0123456789" "=:;,'\"|\\~`" "!?*+()<>$&";  /* 30 -> cells 0..29 */
    for(int i=0;i<NCELL;i++){ LET[i].act=A_CH; LET[i].ch=' '; LET[i].lab=0; SYM[i]=LET[i]; }
    for(int i=0;i<26;i++) LET[i].ch=L[i];
    for(int i=0;i<4;i++)  LET[26+i].ch=Lt[i];
    for(int i=0;i<30;i++) SYM[i].ch=S[i];
    ctrl_row(LET); ctrl_row(SYM);
}

/* ---- TAB path completion (last token, filesystem prefix) ---- */
static void do_complete(void){
    int ts=clen; while(ts>0 && cmd[ts-1]!=' ') ts--;          /* start of last token */
    const char *tok=cmd+ts; int tl=clen-ts;
    char dbuf[CMDMAX]; const char *dir; const char *base;
    const char *sp=NULL; for(int i=0;i<tl;i++) if(tok[i]=='/') sp=tok+i;
    if(sp){ int dl=(int)(sp-tok); if(dl==0){ dir="/"; } else { memcpy(dbuf,tok,dl); dbuf[dl]=0; dir=dbuf; } base=sp+1; }
    else { dir="."; base=tok; }
    int bl=(int)strlen(base);
    DIR *d=opendir(dir); if(!d){ return; }
    struct dirent *e; char lcp[256]; int have=0, nmatch=0; char names[512]; int nl=0;
    while((e=readdir(d))){
        if(!strncmp(e->d_name,".",1) && base[0]!='.') continue;   /* hide dotfiles unless asked */
        if(strncmp(e->d_name,base,bl)) continue;
        nmatch++;
        if(!have){ strncpy(lcp,e->d_name,sizeof lcp-1); lcp[sizeof lcp-1]=0; have=1; }
        else { int i=0; while(lcp[i] && e->d_name[i] && lcp[i]==e->d_name[i]) i++; lcp[i]=0; }
        if(nl < (int)sizeof names-64){ nl+=snprintf(names+nl,sizeof names-nl,"%s  ",e->d_name); }
    }
    closedir(d);
    if(nmatch==0) return;
    for(int i=bl; lcp[i]; i++) cmd_ins(lcp[i]);                 /* extend to common prefix */
    if(nmatch==1){                                             /* single: add '/' if it's a dir */
        char probe[CMDMAX]; snprintf(probe,sizeof probe,"%s/%s",dir,lcp);
        DIR *dd=opendir(probe); if(dd){ closedir(dd); cmd_ins('/'); }
    } else { feed_str(names); feed('\n'); }                    /* many: list them (shell-style) */
}

/* ---- rendering ---- */
static int  layer=0;        /* 0 letters, 1 symbols, 2 presets */
static int  sel=0;          /* selected cell on typing layers  */
static int  psel=0;         /* selected item on presets layer  */

/* preset quick commands */
static const struct { const char*lab, *cmd; } QUICK[]={
    {"dmesg tail",   "dmesg | tail -30"},
    {"iio devices",  "ls /sys/bus/iio/devices"},
    {"RX cal state", "for f in /sys/bus/iio/devices/iio:device0/in_voltage_*_tracking_en; do echo \"$f = $(cat $f)\"; done"},
    {"mem free",     "free"},
    {"disk df",      "df -h"},
    {"processes",    "ps"},
    {"net addrs",    "ip addr 2>/dev/null || ifconfig"},
    {"maia on",      "/etc/init.d/S60maia-httpd on"},
    {"uptime/load",  "cat /proc/uptime /proc/loadavg"},
    {"kernel ver",   "uname -a"},
};
#define NQUICK ((int)(sizeof QUICK/sizeof QUICK[0]))

static void draw_out(uint16_t *buf){
    pp_fill(buf,0,OUT_Y0-1,PP_W,INP_Y-OUT_Y0,C_BG);
    int newest=sb_count-1;
    int bot=newest-sb_scroll; if(bot<0)bot=0;
    int top=bot-(OUT_ROWS-1); if(top<0)top=0;
    for(int r=0; top+r<=bot; r++){
        int logi=top+r, back=newest-logi;
        int ring=(sb_head-back+SB_LINES*4)%SB_LINES;
        if(sb[ring][0]) pp_text(buf,2,OUT_Y0+r*OUT_LH,sb[ring],C_TEXT,1);
    }
    if(sb_scroll>0){ char s[16]; snprintf(s,sizeof s,"-%d",sb_scroll);   /* scrolled-up indicator */
        pp_text(buf,PP_W-pp_tw(s,1)-2,OUT_Y0,s,C_WARN,1); }
    pp_fill(buf,0,INP_Y-2,PP_W,1,C_PANEL);
}
static void draw_input(uint16_t *buf){
    pp_fill(buf,0,INP_Y,PP_W,11,C_PANEL2);
    pp_text(buf,2,INP_Y+1,"$",C_GOOD,1);
    int avail=(PP_W-14)/8;                                 /* chars that fit after "$ " */
    const char *show=cmd; if(clen>avail) show=cmd+(clen-avail);
    pp_text(buf,12,INP_Y+1,show,C_TEXT,1);
    int cx=12+(int)strlen(show)*8;
    pp_fill(buf,cx,INP_Y+1,6,8, child_pid>0?C_DIM:C_ACCENT); /* cursor (dim while a cmd runs) */
}
static void draw_key(uint16_t *buf,const cell_t*g,int i,int selected){
    int col=i%KB_COLS, row=i/KB_COLS;
    int x=col*KB_CW, y=KB_Y0+row*KB_CH;
    pp_fill(buf,x,y,KB_CW-1,KB_CH-1, selected?C_PANEL:C_PANEL2);
    pp_rect(buf,x,y,KB_CW-1,KB_CH-1, selected?C_ACCENT:C_PANEL);
    if(selected) pp_fill(buf,x,y,3,KB_CH-1,C_ACCENT);
    if(g[i].act==A_CH){ char s[2]={g[i].ch,0};
        pp_text(buf,x+(KB_CW-16)/2, y+(KB_CH-16)/2, s, selected?C_TEXT:C_DIM, 2); }
    else { const char*l=g[i].lab?g[i].lab:"?";
        uint16_t c = selected?C_TEXT:(g[i].act==A_ENTER?C_GOOD:C_ACCENT);
        pp_text(buf,x+(KB_CW-pp_tw(l,1))/2, y+(KB_CH-8)/2, l, c, 1); }
}
static void draw_keyboard(uint16_t *buf){
    const cell_t *g = layer==1?SYM:LET;
    for(int i=0;i<NCELL;i++) draw_key(buf,g,i, i==sel);
}
static int preset_rows(void){ return (PP_H-KB_Y0)/OUT_LH; }
static int preset_top(int rows){                            /* first visible item index */
    int total=NQUICK+hist_n, top=psel-rows/2;
    if(top>total-rows)top=total-rows; if(top<0)top=0; return top;
}
static void draw_presets(uint16_t *buf){
    pp_fill(buf,0,KB_Y0,PP_W,PP_H-KB_Y0,C_BG);
    int rows=preset_rows();                                 /* visible list rows */
    int total=NQUICK+hist_n;
    int top=preset_top(rows);
    for(int r=0; r<rows && top+r<total; r++){
        int idx=top+r, y=KB_Y0+r*OUT_LH, foc=(idx==psel);
        const char *lab, *cm; int isq=idx<NQUICK;
        if(isq){ lab=QUICK[idx].lab; cm=QUICK[idx].cmd; }
        else { int h=hist_n-1-(idx-NQUICK); lab="hist"; cm=hist[h]; }    /* newest first */
        if(foc) pp_fill(buf,0,y-1,PP_W,OUT_LH,C_PANEL);
        pp_text(buf,2,y, isq?"*":"h", isq?C_ACCENT:C_WARN, 1);
        char line[46]; snprintf(line,sizeof line,"%s", cm);
        pp_text(buf,14,y, line, foc?C_TEXT:C_DIM, 1);
        (void)lab;
    }
    pp_text(buf,2,KB_Y0-0,"",C_DIM,1);
}
static void term_draw(uint16_t *buf,const pp_status_t *st,int link_ok){
    const char *note = child_pid>0 ? "RUN" : (layer==0?"abc":layer==1?"#?!":"PRESET");
    uint16_t nc = child_pid>0 ? C_WARN : C_DIM;
    pp_ui_status_bar(buf,"Term",st,link_ok,note,nc);
    draw_out(buf);
    draw_input(buf);
    if(layer==2) draw_presets(buf); else draw_keyboard(buf);
}

/* ---- key activation ---- */
static void key_press(void){
    const cell_t *g = layer==1?SYM:LET;
    cell_t k=g[sel];
    switch(k.act){
        case A_CH:    cmd_ins(k.ch); break;
        case A_SPACE: cmd_ins(' '); break;
        case A_BKSP:  cmd_bksp(); break;
        case A_CLR:   clen=0; cmd[0]=0; break;
        case A_TAB:   do_complete(); break;
        case A_PGUP:  sb_scroll+=OUT_ROWS-1; break;
        case A_PGDN:  sb_scroll-=OUT_ROWS-1; if(sb_scroll<0)sb_scroll=0; break;
        case A_ENTER: if(clen){ hist_push(cmd); run_cmd(cmd); clen=0; cmd[0]=0; } break;
    }
    int mx=sb_count-OUT_ROWS; if(mx<0)mx=0; if(sb_scroll>mx)sb_scroll=mx;
}
static void preset_press(void){
    int total=NQUICK+hist_n; if(psel>=total) return;
    const char *cm = psel<NQUICK ? QUICK[psel].cmd : hist[hist_n-1-(psel-NQUICK)];
    strncpy(cmd,cm,CMDMAX-1); cmd[CMDMAX-1]=0; clen=(int)strlen(cmd);
    layer=0; sel=34;                                        /* land on ENTER: CENTER runs it */
}

#ifndef PP_TERM_NOMAIN
int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    signal(SIGPIPE,SIG_IGN);
    kb_build();
    feed_str("pp-term ready. type a command, ENTER to run.\n");

    int have_spi=pp_spi_open(spidev)>=0;
    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    int32_t last_enc=0; int enc_init=0, enc_accum=0, prev_touch=0;
    memset(buf,0,sizeof buf);

    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_accum += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_accum/4; enc_accum-=steps*4;
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;

            if(pressed&PP_BTN_8){ layer=(layer+1)%3; }
            if(pressed&PP_BTN_7){ if(child_pid>0) child_cancel(); else break; }  /* cancel / exit */

            if(layer==2){                                  /* presets: vertical list */
                int total=NQUICK+hist_n; if(total<1)total=1;
                psel += steps; if(pressed&PP_BTN_DOWN)psel++; if(pressed&PP_BTN_UP)psel--;
                if(psel<0)psel=0; if(psel>=total)psel=total-1;
                if(pressed&PP_BTN_CENTER) preset_press();
            } else {                                       /* typing: grid nav */
                sel = ((sel+steps)%NCELL+NCELL)%NCELL;
                int col=sel%KB_COLS, row=sel/KB_COLS;
                if(pressed&PP_BTN_LEFT)  col=(col+KB_COLS-1)%KB_COLS;
                if(pressed&PP_BTN_RIGHT) col=(col+1)%KB_COLS;
                if(pressed&PP_BTN_UP)    row=(row+KB_ROWS-1)%KB_ROWS;
                if(pressed&PP_BTN_DOWN)  row=(row+1)%KB_ROWS;
                sel=row*KB_COLS+col;
                if(pressed&PP_BTN_CENTER) key_press();
            }

            /* touch: tap a key/preset to press it directly */
            if(st.touch_down && !prev_touch){
                int sx,sy; pp_touch_map(st.touch_x,st.touch_y,&sx,&sy);
                if(sy>=KB_Y0){
                    if(layer==2){
                        int rows=preset_rows(), idx=preset_top(rows)+(sy-KB_Y0)/OUT_LH;
                        if(idx>=0 && idx<NQUICK+hist_n){ psel=idx; preset_press(); }
                    }else{
                        int col=sx/KB_CW, row=(sy-KB_Y0)/KB_CH;
                        if(col>=0&&col<KB_COLS&&row>=0&&row<KB_ROWS){ sel=row*KB_COLS+col; key_press(); }
                    }
                } else if(sy>=OUT_Y0 && sy<INP_Y-2){            /* tap upper/lower half of output = page */
                    if(sy < (OUT_Y0+INP_Y)/2) sb_scroll+=OUT_ROWS-1; else { sb_scroll-=OUT_ROWS-1; if(sb_scroll<0)sb_scroll=0; }
                    int mx=sb_count-OUT_ROWS; if(mx<0)mx=0; if(sb_scroll>mx)sb_scroll=mx;
                }
            }
            prev_touch=st.touch_down;
        }
        child_poll();
        term_draw(buf,&st,link_ok);
        pp_refresh(buf);
        usleep(child_pid>0?15000:35000);                   /* snappier while streaming */
    }
    if(child_pid>0){ kill(-child_pid,SIGKILL); waitpid(child_pid,0,0); }
    return 0;
}
#endif
