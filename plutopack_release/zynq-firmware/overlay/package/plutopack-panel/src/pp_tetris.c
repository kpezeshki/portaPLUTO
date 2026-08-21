/*
 * pp-tetris — a Tetris for the PlutoPack front panel (320x240).
 *   Controls: ENCODER wheel (or LEFT/RIGHT) move the piece left/right,
 *             UP or CENTER rotate CW, DOWN soft-drop, BTN8 hard-drop,
 *             BTN7 exit to the launcher. On game over, CENTER restarts.
 * Self-contained (no radio): board + 7-bag randomiser + runtime rotation +
 * line clears + gravity that speeds up per level. Built on pp_common/pp_screen;
 * the PP_SIM branch makes it playable in the web simulator.
 *
 * usage: pp-tetris [spidev=/dev/spidev2.0] [fbdev=/dev/fb0]
 */
#include <time.h>
#include "pp_ui.h"                 /* pp_ui_status_bar + palette + input decode */

/* ---- board geometry ---- */
#define COLS 10
#define ROWS 20
#define CELL 11
#define BX   8                     /* board origin x */
#define BY   20                    /* below the status bar */
#define BW   (COLS*CELL)           /* 110 */
#define BH   (ROWS*CELL)           /* 220 -> 20..240 */
#define PX0  (BX+BW+8)             /* side panel x */

/* ---- pieces: 4 canonical block offsets + box size + colour ---- */
typedef struct { signed char x, y; } pt_t;
typedef struct { pt_t c[4]; int n; } piece_def;
static const piece_def PIECES[7] = {
    {{{0,1},{1,1},{2,1},{3,1}}, 4},   /* I */
    {{{0,0},{1,0},{0,1},{1,1}}, 2},   /* O */
    {{{1,0},{0,1},{1,1},{2,1}}, 3},   /* T */
    {{{1,0},{2,0},{0,1},{1,1}}, 3},   /* S */
    {{{0,0},{1,0},{1,1},{2,1}}, 3},   /* Z */
    {{{0,0},{0,1},{1,1},{2,1}}, 3},   /* J */
    {{{2,0},{0,1},{1,1},{2,1}}, 3},   /* L */
};
static uint16_t PCOL[7];           /* filled at runtime (needs pp_rgb) */

/* resolve the 4 cell offsets of a piece at rotation `rot` (CW) */
static void cells(int type, int rot, pt_t out[4]){
    int N=PIECES[type].n;
    for(int i=0;i<4;i++){
        int x=PIECES[type].c[i].x, y=PIECES[type].c[i].y;
        for(int r=0;r<(rot&3);r++){ int nx=N-1-y, ny=x; x=nx; y=ny; }
        out[i].x=(signed char)x; out[i].y=(signed char)y;
    }
}

/* ---- game state ---- */
static uint8_t board[ROWS][COLS];  /* 0 empty, else colour index+1 */
static int cur, rot, px, py;       /* current piece */
static int bag[7], bagn=0;         /* 7-bag */
static int nextp;
static long score, lines, level;
static int over;

static int rnd_range(int n){ return (int)((unsigned)rand() % (unsigned)n); }
static int bag_next(void){
    if(bagn==0){ for(int i=0;i<7;i++) bag[i]=i;                 /* refill + shuffle */
        for(int i=6;i>0;i--){ int j=rnd_range(i+1); int t=bag[i];bag[i]=bag[j];bag[j]=t; } bagn=7; }
    return bag[--bagn];
}

static int collide(int type,int r,int ox,int oy){
    pt_t c[4]; cells(type,r,c);
    for(int i=0;i<4;i++){
        int x=ox+c[i].x, y=oy+c[i].y;
        if(x<0||x>=COLS||y>=ROWS) return 1;
        if(y>=0 && board[y][x]) return 1;
    }
    return 0;
}
static void spawn(void){
    cur=nextp; nextp=bag_next(); rot=0;
    px=(COLS/2)-2; py=0;
    if(collide(cur,rot,px,py)) over=1;   /* topped out */
}
static void reset_game(void){
    memset(board,0,sizeof board); score=0; lines=0; level=0; over=0;
    bagn=0; nextp=bag_next(); spawn();
}
static void lock_piece(void){
    pt_t c[4]; cells(cur,rot,c);
    for(int i=0;i<4;i++){ int x=px+c[i].x, y=py+c[i].y; if(y>=0&&y<ROWS&&x>=0&&x<COLS) board[y][x]=(uint8_t)(cur+1); }
    /* clear full rows */
    int cleared=0;
    for(int y=ROWS-1;y>=0;y--){
        int full=1; for(int x=0;x<COLS;x++) if(!board[y][x]){ full=0; break; }
        if(full){ cleared++;
            for(int yy=y;yy>0;yy--) memcpy(board[yy],board[yy-1],COLS);
            memset(board[0],0,COLS); y++;                       /* re-check this row */
        }
    }
    if(cleared){
        static const int LT[5]={0,40,100,300,1200};
        score += (long)LT[cleared]*(level+1);
        lines += cleared; level = lines/10;
    }
    spawn();
}
static int soft_drop(void){                                    /* returns 1 if locked */
    if(!collide(cur,rot,px,py+1)){ py++; return 0; }
    lock_piece(); return 1;
}
static void hard_drop(void){
    int d=0; while(!collide(cur,rot,px,py+1)){ py++; d++; }
    score += 2*d; lock_piece();
}
static void try_rotate(void){
    int nr=(rot+1)&3;
    static const int KICK[5]={0,-1,1,-2,2};
    for(int k=0;k<5;k++) if(!collide(cur,nr,px+KICK[k],py)){ px+=KICK[k]; rot=nr; return; }
}
static void try_move(int dx){ if(!collide(cur,rot,px+dx,py)) px+=dx; }

/* ---- rendering ---- */
static void draw_cell(uint16_t*buf,int gx,int gy,uint16_t col){
    int x=BX+gx*CELL, y=BY+gy*CELL;
    pp_fill(buf,x+1,y+1,CELL-1,CELL-1,col);
    pp_rect(buf,x,y,CELL,CELL,pp_rgb(0x10,0x14,0x1c));
}
static void draw_mini(uint16_t*buf,int type,int ox,int oy){
    pt_t c[4]; cells(type,0,c);
    for(int i=0;i<4;i++){ int x=ox+c[i].x*8, y=oy+c[i].y*8;
        pp_fill(buf,x+1,y+1,7,7,PCOL[type]); pp_rect(buf,x,y,8,8,pp_rgb(0x10,0x14,0x1c)); }
}
static void render(uint16_t*buf, const pp_status_t*st, int link_ok){
    pp_fill(buf,0,BY,PP_W,PP_H-BY,C_BG);
    /* board well */
    pp_fill(buf,BX,BY,BW,BH,pp_rgb(0x07,0x09,0x0e));
    pp_rect(buf,BX-1,BY-1,BW+2,BH+2,C_PANEL);
    for(int y=0;y<ROWS;y++) for(int x=0;x<COLS;x++)
        if(board[y][x]) draw_cell(buf,x,y,PCOL[board[y][x]-1]);
    if(!over){
        /* ghost */
        int gy=py; while(!collide(cur,rot,px,gy+1)) gy++;
        pt_t c[4]; cells(cur,rot,c);
        for(int i=0;i<4;i++){ int x=px+c[i].x, y=gy+c[i].y;
            if(y>=0){ int sx=BX+x*CELL, sy=BY+y*CELL; pp_rect(buf,sx,sy,CELL,CELL,C_DIM); } }
        for(int i=0;i<4;i++){ int x=px+c[i].x, y=py+c[i].y; if(y>=0) draw_cell(buf,x,y,PCOL[cur]); }
    }
    /* side panel */
    char s[24];
    pp_text(buf,PX0,BY+4,"SCORE",C_DIM,1);  snprintf(s,sizeof s,"%ld",score); pp_text(buf,PX0,BY+16,s,C_TEXT,2);
    pp_text(buf,PX0,BY+40,"LINES",C_DIM,1); snprintf(s,sizeof s,"%ld",lines); pp_text(buf,PX0,BY+52,s,C_TEXT,2);
    pp_text(buf,PX0,BY+76,"LEVEL",C_DIM,1); snprintf(s,sizeof s,"%ld",level); pp_text(buf,PX0,BY+88,s,C_ACCENT,2);
    pp_text(buf,PX0,BY+112,"NEXT",C_DIM,1); draw_mini(buf,nextp,PX0,BY+124);
    pp_text(buf,PX0,PP_H-40,"wheel:move",C_DIM,1);
    pp_text(buf,PX0,PP_H-30,"UP:rotate",C_DIM,1);
    pp_text(buf,PX0,PP_H-20,"DN drop  BACK exit",C_DIM,1);
    if(over){
        pp_fill(buf,BX-1,BY+BH/2-18,BW+2,40,C_PANEL);
        pp_rect(buf,BX-1,BY+BH/2-18,BW+2,40,C_BAD);
        pp_text(buf,BX+(BW-pp_tw("GAME OVER",1))/2,BY+BH/2-12,"GAME OVER",C_BAD,1);
        pp_text(buf,BX+(BW-pp_tw("CENTER: retry",1))/2,BY+BH/2+2,"CENTER: retry",C_TEXT,1);
    }
    pp_ui_status_bar(buf,"TETRIS",st,link_ok,NULL,C_WARN);
}

int main(int argc,char**argv){
    const char *spidev=argc>1?argv[1]:"/dev/spidev2.0";
    const char *fbdev =argc>2?argv[2]:"/dev/fb0";
    if(pp_fb_open(fbdev)<0){ perror("open fb"); return 1; }
    int have_spi = pp_spi_open(spidev)>=0;
    srand((unsigned)time(NULL));
    PCOL[0]=pp_rgb(0x22,0xd3,0xee); PCOL[1]=pp_rgb(0xf5,0xd0,0x2a);   /* I cyan, O yellow */
    PCOL[2]=pp_rgb(0xb4,0x5c,0xf0);                                  /* T purple */
    PCOL[3]=pp_rgb(0x36,0xd3,0x99); PCOL[4]=pp_rgb(0xff,0x4d,0x4d);   /* S green, Z red */
    PCOL[5]=pp_rgb(0x3b,0x82,0xf6); PCOL[6]=pp_rgb(0xff,0x8a,0x1f);   /* J blue, L orange */

    reset_game();

    static uint16_t buf[PP_W*PP_H];
    pp_status_t st; memset(&st,0,sizeof st);
    uint8_t rx[PP_FRAME_LEN], last_btn=0;
    int32_t last_enc=0; int enc_init=0, enc_accum=0;
    int grav=0;
    memset(buf,0,sizeof buf);

    int confirm=0;
    for(;;){
        int link_ok=0;
        if(have_spi && pp_poll(rx)==0 && pp_decode(rx,&st)>=0){
            link_ok=1;
            if(!enc_init){ last_enc=st.enc_pos; enc_init=1; }
            enc_accum += (int)(st.enc_pos-last_enc); last_enc=st.enc_pos;
            int steps=enc_accum/4; enc_accum-=steps*4;        /* 4 counts/detent */
            uint8_t pressed = st.btn_bitmap & ~last_btn; last_btn=st.btn_bitmap;

            if(confirm){
                if(pressed&PP_BTN_CENTER) break;               /* confirm -> exit to menu */
                if(pressed&PP_BTN_7) confirm=0;                /* cancel -> resume */
            }else if(pressed&PP_BTN_7){ confirm=1;             /* BACK -> confirm exit */
            }else if(over){
                if(pressed&PP_BTN_CENTER) reset_game();
            }else{
                if(steps>0) for(int i=0;i<steps;i++)  try_move(1);
                if(steps<0) for(int i=0;i<-steps;i++) try_move(-1);
                if(pressed&PP_BTN_RIGHT) try_move(1);
                if(pressed&PP_BTN_LEFT)  try_move(-1);
                if(pressed&(PP_BTN_UP|PP_BTN_CENTER)) try_rotate();
                if(pressed&PP_BTN_8) hard_drop();
                if(st.btn_bitmap&PP_BTN_DOWN){ if(soft_drop()) score+=1; grav=0; }  /* soft drop */
            }
        }
        /* gravity (frozen while the exit modal is up) */
        if(!over && !confirm){
            int period=(int)(22-level*2); if(period<3)period=3;
            if(++grav>=period){ grav=0; soft_drop(); }
        }
        render(buf,&st,link_ok);
        pp_confirm_overlay(buf,confirm);
        pp_refresh(buf);
        usleep(30000);
    }
    return 0;
}
