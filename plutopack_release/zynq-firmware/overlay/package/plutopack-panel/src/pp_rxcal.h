/*
 * pp_rxcal.h — RX power calibration table: format, persistence, inversion.
 *
 * The RX POWER CAL app (pp-rxpowercal) writes this table; the POWER METER app
 * (pp-powermeter) reads it. NO other app uses the power calibration.
 *
 * The table is the raw measured peak-tone level (dBFS) over four axes:
 *   [frequency][input power dBm][rx gain dB][LO offset Hz]
 * captured at a single fixed sample rate (fs_hz). Cells that were never measured
 * (tone below the noise floor, or the gain/power combination would overload the
 * ADC) hold PP_RXCAL_NODATA.
 *
 * The METER inverts it: given a measured level at a known (freq, gain, offset),
 * interpolate over freq+offset and invert the monotonic level-vs-power curve to
 * recover the absolute input power in dBm.
 */
#ifndef PP_RXCAL_H
#define PP_RXCAL_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define PP_RXCAL_NAME     "rxpower.cal"
#define PP_RXCAL_MAGIC    0x50505243u        /* 'PPRC' */
#define PP_RXCAL_VERSION  1
#define PP_RXCAL_NODATA   (-999.0f)

/* axis maxima (bound static memory; the default grid fits well inside these) */
#define PP_RXCAL_MAXF 12
#define PP_RXCAL_MAXP 10
#define PP_RXCAL_MAXG 20
#define PP_RXCAL_MAXO 40
#define PP_RXCAL_MAXCELLS (PP_RXCAL_MAXF*PP_RXCAL_MAXP*PP_RXCAL_MAXG*PP_RXCAL_MAXO)

typedef struct {
    int       nfreq, npower, ngain, noffset;
    long long fs_hz;                 /* capture sample rate the table is valid at */
    int       complete;             /* 1 = full sweep finished */
    int       temp_c10;             /* ambient temp x10 at cal time (provenance) */
    long long freq_hz  [PP_RXCAL_MAXF];
    float     power_dbm[PP_RXCAL_MAXP];
    float     gain_db  [PP_RXCAL_MAXG];
    long long offset_hz[PP_RXCAL_MAXO];
    float     meas     [PP_RXCAL_MAXCELLS];   /* [f][p][g][o] dBFS, or NODATA */
} pp_rxcal_t;

static int pp_rxcal_idx(const pp_rxcal_t *c,int f,int p,int g,int o){
    return ((f*c->npower + p)*c->ngain + g)*c->noffset + o;
}
static int pp_rxcal_cells(const pp_rxcal_t *c){
    return c->nfreq*c->npower*c->ngain*c->noffset;
}
static int pp_rxcal_dims_ok(const pp_rxcal_t *c){
    return c->nfreq>0 && c->nfreq<=PP_RXCAL_MAXF && c->npower>0 && c->npower<=PP_RXCAL_MAXP
        && c->ngain>0 && c->ngain<=PP_RXCAL_MAXG && c->noffset>0 && c->noffset<=PP_RXCAL_MAXO
        && c->fs_hz>0;
}

/* --------------------------- persistence -------------------------------- */
/* write only the used portion; atomic (temp + rename). returns 1 on success. */
static int pp_rxcal_save(const pp_rxcal_t *c, const char *path){
    if(!pp_rxcal_dims_ok(c)) return 0;
    char tmp[128]; snprintf(tmp,sizeof tmp,"%s.tmp",path);
    FILE *f=fopen(tmp,"wb"); if(!f) return 0;
    uint32_t magic=PP_RXCAL_MAGIC, ver=PP_RXCAL_VERSION;
    int ok=1;
    ok &= fwrite(&magic,4,1,f)==1;
    ok &= fwrite(&ver,4,1,f)==1;
    ok &= fwrite(&c->complete,sizeof c->complete,1,f)==1;
    ok &= fwrite(&c->fs_hz,sizeof c->fs_hz,1,f)==1;
    ok &= fwrite(&c->temp_c10,sizeof c->temp_c10,1,f)==1;
    ok &= fwrite(&c->nfreq,sizeof(int),1,f)==1;
    ok &= fwrite(&c->npower,sizeof(int),1,f)==1;
    ok &= fwrite(&c->ngain,sizeof(int),1,f)==1;
    ok &= fwrite(&c->noffset,sizeof(int),1,f)==1;
    ok &= fwrite(c->freq_hz, sizeof(long long),c->nfreq,f)==(size_t)c->nfreq;
    ok &= fwrite(c->power_dbm,sizeof(float),   c->npower,f)==(size_t)c->npower;
    ok &= fwrite(c->gain_db,  sizeof(float),   c->ngain,f)==(size_t)c->ngain;
    ok &= fwrite(c->offset_hz,sizeof(long long),c->noffset,f)==(size_t)c->noffset;
    int nc=pp_rxcal_cells(c);
    ok &= fwrite(c->meas,sizeof(float),nc,f)==(size_t)nc;
    if(fflush(f)==0) fsync(fileno(f)); else ok=0;
    fclose(f);
    if(!ok){ remove(tmp); return 0; }
    return rename(tmp,path)==0;
}
/* load + validate. returns 1 on success (c fully populated). */
static int pp_rxcal_load(pp_rxcal_t *c, const char *path){
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    uint32_t magic=0,ver=0; int ok=1;
    memset(c,0,sizeof *c);
    ok &= fread(&magic,4,1,f)==1;
    ok &= fread(&ver,4,1,f)==1;
    if(!ok || magic!=PP_RXCAL_MAGIC || ver!=PP_RXCAL_VERSION){ fclose(f); return 0; }
    ok &= fread(&c->complete,sizeof c->complete,1,f)==1;
    ok &= fread(&c->fs_hz,sizeof c->fs_hz,1,f)==1;
    ok &= fread(&c->temp_c10,sizeof c->temp_c10,1,f)==1;
    ok &= fread(&c->nfreq,sizeof(int),1,f)==1;
    ok &= fread(&c->npower,sizeof(int),1,f)==1;
    ok &= fread(&c->ngain,sizeof(int),1,f)==1;
    ok &= fread(&c->noffset,sizeof(int),1,f)==1;
    if(!ok || !pp_rxcal_dims_ok(c)){ fclose(f); return 0; }
    ok &= fread(c->freq_hz, sizeof(long long),c->nfreq,f)==(size_t)c->nfreq;
    ok &= fread(c->power_dbm,sizeof(float),   c->npower,f)==(size_t)c->npower;
    ok &= fread(c->gain_db,  sizeof(float),   c->ngain,f)==(size_t)c->ngain;
    ok &= fread(c->offset_hz,sizeof(long long),c->noffset,f)==(size_t)c->noffset;
    int nc=pp_rxcal_cells(c);
    ok &= fread(c->meas,sizeof(float),nc,f)==(size_t)nc;
    fclose(f);
    return ok;
}
static int pp_rxcal_valid(const pp_rxcal_t *c){        /* usable by the meter */
    return pp_rxcal_dims_ok(c) && c->complete;
}
static int pp_rxcal_covers(const pp_rxcal_t *c,long long f_hz){
    return f_hz>=c->freq_hz[0] && f_hz<=c->freq_hz[c->nfreq-1];
}

/* --------------------------- inversion ---------------------------------- */
/* bracket an ascending long long axis: sets i0,i1 and frac in [0,1] (clamped). */
static void pp_rxcal_bracket_ll(const long long *a,int n,long long x,int *i0,int *i1,float *fr){
    if(n<=1 || x<=a[0]){ *i0=*i1=0; *fr=0; return; }
    if(x>=a[n-1]){ *i0=*i1=n-1; *fr=0; return; }
    int i=0; while(i<n-1 && a[i+1]<=x) i++;
    *i0=i; *i1=i+1;
    long long d=a[i+1]-a[i]; *fr = d>0 ? (float)((double)(x-a[i])/(double)d) : 0.f;
}
/* invert the monotonic level-vs-power curve at fixed (fi,gi,oi): level m -> dBm.
 * returns NODATA if m is below the weakest / above the strongest measured point. */
static float pp_rxcal_invpow(const pp_rxcal_t *c,int fi,int gi,int oi,float m){
    float pv[PP_RXCAL_MAXP], lv[PP_RXCAL_MAXP]; int nv=0;
    for(int p=0;p<c->npower;p++){
        float v=c->meas[pp_rxcal_idx(c,fi,p,gi,oi)];
        if(v<=PP_RXCAL_NODATA+1.f) continue;              /* skip NODATA */
        pv[nv]=c->power_dbm[p]; lv[nv]=v; nv++;
    }
    if(nv<2) return PP_RXCAL_NODATA;
    if(m<lv[0] || m>lv[nv-1]) return PP_RXCAL_NODATA;     /* out of measured range */
    for(int k=0;k<nv-1;k++){
        if(m>=lv[k] && m<=lv[k+1]){
            float d=lv[k+1]-lv[k];
            float t = d>1e-6f ? (m-lv[k])/d : 0.f;
            return pv[k] + t*(pv[k+1]-pv[k]);
        }
    }
    return PP_RXCAL_NODATA;
}
/* full inversion: measured level m at (f_hz, gain index gi, o_hz) -> input dBm.
 * bilinear over freq+offset, inverting the power curve at each valid corner.
 * returns NODATA if no corner yields a value (out of range everywhere). */
static float pp_rxcal_invert(const pp_rxcal_t *c,long long f_hz,int gi,long long o_hz,float m){
    if(gi<0||gi>=c->ngain) return PP_RXCAL_NODATA;
    int f0,f1,o0,o1; float ff,of;
    pp_rxcal_bracket_ll(c->freq_hz,c->nfreq,f_hz,&f0,&f1,&ff);
    pp_rxcal_bracket_ll(c->offset_hz,c->noffset,o_hz,&o0,&o1,&of);
    struct{int fi,oi; float w;} cn[4]={
        {f0,o0,(1-ff)*(1-of)},{f0,o1,(1-ff)*of},{f1,o0,ff*(1-of)},{f1,o1,ff*of}};
    float acc=0,wsum=0;
    for(int i=0;i<4;i++){
        if(cn[i].w<=0) continue;
        float v=pp_rxcal_invpow(c,cn[i].fi,gi,cn[i].oi,m);
        if(v<=PP_RXCAL_NODATA+1.f) continue;
        acc+=cn[i].w*v; wsum+=cn[i].w;
    }
    return wsum>0 ? acc/wsum : PP_RXCAL_NODATA;
}
#endif /* PP_RXCAL_H */
