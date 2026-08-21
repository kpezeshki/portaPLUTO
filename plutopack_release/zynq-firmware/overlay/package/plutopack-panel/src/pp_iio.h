/*
 * pp_iio.h — thin AD9361 tone-generator control over the IIO sysfs ABI.
 *   Devices: "ad9361-phy" (TX LO, per-ch attenuation, RF bandwidth, sample rate)
 *            "cf-ad9361-dds-core-lpc" (the DAC DDS: per-channel F1 I/Q tone).
 *   Each setter writes the attribute then reads it back, returning the ACTUAL
 *   applied (quantized) value so the UI can stay in sync (amber-until-confirmed).
 *   A PP_SIM branch models the radio locally so pp-ToneGen is testable in the
 *   web simulator without hardware.
 *
 * DDS tone-gen (requires the DDS core enabled in the FPGA — DAC_DDS_DISABLE 0):
 * a single complex tone per channel = F1 I at phase 90 deg + F1 Q at phase 0,
 * same frequency & scale; F2 muted (scale 0). scale 0 => channel off.
 */
#ifndef PP_IIO_H
#define PP_IIO_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TG_AMP     0.25     /* DDS scale when a channel is on (backoff from full scale) */
#define TG_MAXCH   2

#ifndef PP_SIM
/* ------------------------------- real sysfs ------------------------------ */
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

static char iio_phy[128], iio_dds[128];

static int iio_read_name(const char *dir, char *out, int n){
    char p[160]; snprintf(p,sizeof p,"%s/name",dir);
    FILE *f=fopen(p,"r"); if(!f) return 0;
    int ok = fgets(out,n,f)!=NULL; fclose(f);
    if(ok){ char *nl=strchr(out,'\n'); if(nl)*nl=0; }
    return ok;
}
static int iio_find_dev(const char *want, char *outdir){
    DIR *d=opendir("/sys/bus/iio/devices"); if(!d) return 0;
    struct dirent *e; char nm[64];
    while((e=readdir(d))){
        if(strncmp(e->d_name,"iio:device",10)) continue;
        char dir[128]; snprintf(dir,sizeof dir,"/sys/bus/iio/devices/%s",e->d_name);
        if(iio_read_name(dir,nm,sizeof nm) && !strcmp(nm,want)){ strcpy(outdir,dir); closedir(d); return 1; }
    }
    closedir(d); return 0;
}
/* resolve an attribute whose name ENDS WITH `sub` (DDS altvoltage index varies).
 * Suffix (not substring) match is essential: every phy attr has a read-only
 * "<name>_available" sibling that ALSO contains the substring — a plain strstr
 * can return that instead, so writes hit a read-only attr (EACCES) and readbacks
 * parse the "[min step max]" range string as garbage. Ending-with rejects it. */
static int iio_find_attr(const char *dir, const char *sub, char *outpath){
    DIR *d=opendir(dir); if(!d) return 0;
    struct dirent *e; size_t ls=strlen(sub);
    while((e=readdir(d))){
        size_t ln=strlen(e->d_name);
        if(ln>=ls && !strcmp(e->d_name+ln-ls,sub)){ snprintf(outpath,160,"%s/%s",dir,e->d_name); closedir(d); return 1; }
    }
    closedir(d); return 0;
}
static int iio_wr(const char *path, const char *val){
    int fd=open(path,O_WRONLY); if(fd<0) return 0;
    int n=(int)strlen(val); int ok = write(fd,val,n)==n; close(fd); return ok;
}
static int iio_rd(const char *path, double *out){
    int fd=open(path,O_RDONLY); if(fd<0) return 0;
    char b[64]; int n=(int)read(fd,b,sizeof b-1); close(fd);
    if(n<=0) return 0; b[n]=0; *out=strtod(b,NULL); return 1;
}
/* write `val` to dir/<attr containing sub>, then read it back into *got.
 * Integer-valued attrs (TX_LO_frequency, rf_bandwidth, DDS frequency/phase) are
 * parsed with kstrtoull on the kernel side and REJECT a decimal point ("2.4e9"
 * -> -EINVAL -> write fails), so an integral value must be written WITHOUT a
 * fractional part. Only genuinely fractional values (e.g. DDS scale 0.25) get
 * the "%.6f" form (those attrs use a float parser). */
static int iio_set(const char *dir, const char *sub, double val, double *got){
    char path[160]; if(!iio_find_attr(dir,sub,path)) return 0;
    char s[48];
    if(val==floor(val) && fabs(val)<9e15) snprintf(s,sizeof s,"%lld",(long long)val);
    else                                   snprintf(s,sizeof s,"%.6f",val);
    if(!iio_wr(path,s)) return 0;
    if(got) return iio_rd(path,got); return 1;
}

static int tg_init(int *ntx, long *fs_hz){
    if(!iio_find_dev("ad9361-phy",iio_phy)) return 0;
    if(!iio_find_dev("cf-ad9361-dds-core-lpc",iio_dds)) return 0;
    int n=0; for(int c=0;c<TG_MAXCH;c++){ char sub[32]; snprintf(sub,sizeof sub,"out_voltage%d_hardwaregain",c);
        char p[160]; if(iio_find_attr(iio_phy,sub,p)) n++; }
    *ntx = n<1?1:n;
    double fs=0; iio_set(iio_phy,"out_voltage_sampling_frequency",0,&fs); /* 0 write rejected; just read */
    { char p[160]; if(iio_find_attr(iio_phy,"out_voltage_sampling_frequency",p)) iio_rd(p,&fs); }
    *fs_hz = (long)fs;
    return 1;
}
/* maia-httpd parks the TX LO powered-down and RE-ASSERTS powerdown=1 at ~1 Hz
 * (it owns the radio for RX and kills TX spurs). A one-shot clear dies within a
 * second, so a transmitting app must re-clear this every UI loop. Verified on
 * hardware: maia touches ONLY out_altvoltage1_TX_LO_powerdown, never the TX LO
 * frequency, so there is no frequency contention. Without this the DDS tone is
 * generated but never reaches RF — the "transmit doesn't work" bug. */
static void tg_txlo_on(void){ iio_set(iio_phy,"TX_LO_powerdown",0.0,NULL); }
static int tg_lo(long long want, long long *got){ double g=0; char sub[]="TX_LO_frequency";  /* Hz > 2^31 */
    if(!iio_set(iio_phy,sub,(double)want,&g)) return 0; tg_txlo_on(); *got=(long long)g; return 1; }
static int tg_atten(int ch, double want_db, double *got){ char sub[32]; snprintf(sub,sizeof sub,"out_voltage%d_hardwaregain",ch);
    double g=0; if(!iio_set(iio_phy,sub,-want_db,&g)) return 0; *got=-g; return 1; }
static int tg_bw(long want, long *got){ double g=0; if(!iio_set(iio_phy,"out_voltage_rf_bandwidth",(double)want,&g)) return 0; *got=(long)g; return 1; }
static int tg_fs(long want, long *got){ double g=0; if(!iio_set(iio_phy,"out_voltage_sampling_frequency",(double)want,&g)) return 0; *got=(long)g; return 1; }
/* ph_deg: channel phase (0..359). Applied to both I and Q so their 90 deg
 * quadrature is preserved; sets the tone's phase relative to the other channel. */
static int tg_tone(int ch, long off, double scale, long ph_deg, long *got){
    char sub[40]; double g=0; int ok=1;
    double q=(double)(((ph_deg%360)+360)%360)*1000.0;   /* Q phase (mdeg) */
    double i=q+90000.0; if(i>=360000.0) i-=360000.0;     /* I leads by 90 deg */
    snprintf(sub,sizeof sub,"TX%d_I_F1_phase",ch+1);     iio_set(iio_dds,sub,i,0);
    snprintf(sub,sizeof sub,"TX%d_Q_F1_phase",ch+1);     iio_set(iio_dds,sub,q,0);
    snprintf(sub,sizeof sub,"TX%d_I_F1_scale",ch+1);     ok&=iio_set(iio_dds,sub,scale,0);
    snprintf(sub,sizeof sub,"TX%d_Q_F1_scale",ch+1);     ok&=iio_set(iio_dds,sub,scale,0);
    snprintf(sub,sizeof sub,"TX%d_I_F2_scale",ch+1);     iio_set(iio_dds,sub,0.0,0);
    snprintf(sub,sizeof sub,"TX%d_Q_F2_scale",ch+1);     iio_set(iio_dds,sub,0.0,0);
    snprintf(sub,sizeof sub,"TX%d_Q_F1_frequency",ch+1); iio_set(iio_dds,sub,(double)off,0);
    snprintf(sub,sizeof sub,"TX%d_I_F1_frequency",ch+1); ok&=iio_set(iio_dds,sub,(double)off,&g);
    if(!ok) return 0; *got=(long)g; return 1;
}
/* ---- read-back getters (populate the UI from the live radio on app open) ---- */
static int iio_get(const char *dir, const char *sub, double *out){
    char p[160]; if(!iio_find_attr(dir,sub,p)) return 0; return iio_rd(p,out);
}
static int tg_get_lo(long long *hz){ double v; if(!iio_get(iio_phy,"TX_LO_frequency",&v)) return 0; *hz=(long long)v; return 1; }
static int tg_get_atten(int ch,double *db){ char s[32]; snprintf(s,sizeof s,"out_voltage%d_hardwaregain",ch);
    double v; if(!iio_get(iio_phy,s,&v)) return 0; *db=-v; return 1; }
static int tg_get_bw(long *hz){ double v; if(!iio_get(iio_phy,"out_voltage_rf_bandwidth",&v)) return 0; *hz=(long)v; return 1; }
static int tg_get_fs(long *hz){ double v; if(!iio_get(iio_phy,"out_voltage_sampling_frequency",&v)) return 0; *hz=(long)v; return 1; }
static int tg_get_tone(int ch,long *off,double *scale,long *ph_deg){
    char s[40]; double v;
    snprintf(s,sizeof s,"TX%d_I_F1_frequency",ch+1); if(!iio_get(iio_dds,s,&v)) return 0; *off=(long)v;
    snprintf(s,sizeof s,"TX%d_I_F1_scale",ch+1);     *scale = iio_get(iio_dds,s,&v)?v:0;
    snprintf(s,sizeof s,"TX%d_Q_F1_phase",ch+1);     *ph_deg= iio_get(iio_dds,s,&v)?(((long)(v/1000.0))%360):0;
    return 1;
}

#else  /* --------------------------- PP_SIM model --------------------------- */
static struct { long long lo; long off[TG_MAXCH], ph[TG_MAXCH], bw, fs; double atten[TG_MAXCH], scale[TG_MAXCH]; } M =
    { 2439000000LL, {1000000,11000000}, {0,0}, 18000000L, 30720000L, {20,20}, {0.25,0} };
static int tg_init(int *ntx, long *fs_hz){ *ntx=2; *fs_hz=M.fs; return 1; }
static void tg_txlo_on(void){}
static int tg_lo(long long want, long long *got){ M.lo=((want+1)/2)*2; *got=M.lo; return 1; }   /* ~2 Hz step */
static int tg_atten(int ch, double db, double *got){ double q=round(db*4)/4; M.atten[ch]=q; *got=q; return 1; } /* 0.25 dB */
static int tg_bw(long want, long *got){ M.bw=(want/1000)*1000; *got=M.bw; return 1; }
static int tg_fs(long want, long *got){ M.fs=want; *got=M.fs; return 1; }
static int tg_tone(int ch, long off, double scale, long ph_deg, long *got){
    M.off[ch]=(off/100)*100; M.scale[ch]=scale; M.ph[ch]=((ph_deg%360)+360)%360; *got=M.off[ch]; return 1; }
static int tg_get_lo(long long *hz){ *hz=M.lo; return 1; }
static int tg_get_atten(int ch,double *db){ *db=M.atten[ch]; return 1; }
static int tg_get_bw(long *hz){ *hz=M.bw; return 1; }
static int tg_get_fs(long *hz){ *hz=M.fs; return 1; }
static int tg_get_tone(int ch,long *off,double *scale,long *ph_deg){ *off=M.off[ch]; *scale=M.scale[ch]; *ph_deg=M.ph[ch]; return 1; }
#endif
#endif
