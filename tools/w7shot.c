/* =====================================================================
 *  w7shot - headless host for the WILD 7's core, for development.
 *
 *  Includes the core's own source (like src/sim.c), drives the real
 *  retro_run() loop, and writes chosen frames as PNG so graphics can be
 *  looked at without RetroArch or a screen.  Also times every frame.
 *
 *    gcc -O2 -Isrc tools/w7shot.c -o w7shot -lm -lz -lpthread
 *    ./w7shot -n 600 -s 120,300,599 -o /tmp/shots [-i "40:start;110:a"]
 *
 *  -n N        frames to run
 *  -s a,b,c    frames to save (frame index, 0-based); "-s every:30" saves
 *              every 30th frame
 *  -o DIR      output directory (default .)
 *  -i SCRIPT   input script "frame:btn+btn;frame:btn" - buttons are
 *              up down left right a b x y start select l r.  Ignored when
 *              WILD7_AUTOPILOT is set (the core's own pilot wins).
 *  -p NAME     file name prefix (default f)
 *  -r SEED     rng seed
 *  -g FILE     load FILE as the content (a .w7 settings file) and print
 *              the settings it produced
 *  -w FILE     write everything handed to audio_batch_cb as a 16-bit
 *              stereo WAV, and print its peak / RMS / DC
 *  -H N        hash every Nth frame (1 = all): prints "h FRAME STATE HASH"
 *              per hashed frame and a final "digest" line.  Two runs drew
 *              identical frames iff their outputs match, which is how the
 *              band renderer is proved against the single-threaded one:
 *              W7SHOT_OPTS=wild7_threads=1 against =3.  See tools/bandcheck.sh
 *  -q          with -H, print only the digest line
 *
 *  The WILD7_AUTOPILOT / WILD7_FORCE environment hooks work as on the Pi.
 *  Prints mean / max ms per frame at the end (x86 numbers: the Pi 4 is
 *  roughly 4-6x slower per core - measure there before trusting a budget).
 * ===================================================================== */
#define W7_AUDIO_PROF                /* time audio_frame() on its own */
#include "../src/wild7_libretro.c"
#ifndef W7SHOT_NOPNG
#include <zlib.h>
#endif
#include <time.h>

static const uint32_t *last_fb;
static void vid(const void*d,unsigned w,unsigned h,size_t p){ (void)w;(void)h;(void)p; last_fb=(const uint32_t*)d; }
/*  WAV capture.  The header is written with zero sizes and patched when
 *  the run ends; level statistics are gathered on the way through.   */
static FILE *wavf; static long wav_frames;
static double wav_sq[2], wav_sum[2]; static int wav_pk[2]; static long wav_clip;
static void le32(FILE*f,uint32_t v){ unsigned char b[4]={v,v>>8,v>>16,v>>24}; fwrite(b,1,4,f); }
static void le16(FILE*f,uint16_t v){ unsigned char b[2]={v,v>>8}; fwrite(b,1,2,f); }
static void wav_head(FILE*f,uint32_t frames){
  fwrite("RIFF",1,4,f); le32(f,36+frames*4); fwrite("WAVEfmt ",1,8,f);
  le32(f,16); le16(f,1); le16(f,2); le32(f,SRATE); le32(f,SRATE*4); le16(f,4); le16(f,16);
  fwrite("data",1,4,f); le32(f,frames*4);
}
static size_t aud(const int16_t*d,size_t f){
  if(wavf){
    fwrite(d,sizeof(int16_t),f*2,wavf); wav_frames+=(long)f;
    for(size_t i=0;i<f*2;i++){ int c=i&1, v=d[i], a=v<0?-v:v;
      wav_sum[c]+=v; wav_sq[c]+=(double)v*v; if(a>wav_pk[c]) wav_pk[c]=a; if(a>=32000) wav_clip++; }
  }
  return f;
}
static void inpoll(void){}
static int cur_btn;
static int16_t inst(unsigned port,unsigned dev,unsigned idx,unsigned id){
  (void)dev;(void)idx; if(port) return 0;
  static const int map[16]={
    [RETRO_DEVICE_ID_JOYPAD_B]=B_B,[RETRO_DEVICE_ID_JOYPAD_Y]=B_Y,
    [RETRO_DEVICE_ID_JOYPAD_SELECT]=B_SELECT,[RETRO_DEVICE_ID_JOYPAD_START]=B_START,
    [RETRO_DEVICE_ID_JOYPAD_UP]=B_UP,[RETRO_DEVICE_ID_JOYPAD_DOWN]=B_DOWN,
    [RETRO_DEVICE_ID_JOYPAD_LEFT]=B_LEFT,[RETRO_DEVICE_ID_JOYPAD_RIGHT]=B_RIGHT,
    [RETRO_DEVICE_ID_JOYPAD_A]=B_A,[RETRO_DEVICE_ID_JOYPAD_X]=B_X,
    [RETRO_DEVICE_ID_JOYPAD_L]=B_L,[RETRO_DEVICE_ID_JOYPAD_R]=B_R };
  return (id<16 && (cur_btn & map[id])) ? 1 : 0;
}
static bool env(unsigned cmd,void*data){
  switch(cmd){
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable*v=(struct retro_variable*)data; v->value=NULL;
    const char*e=getenv("W7SHOT_OPTS");       /* "wild7_turbo=on,wild7_sound=off" */
    if(e && v->key){
      static char buf[64]; const char*p=strstr(e,v->key);
      if(p && p[strlen(v->key)]=='='){ p+=strlen(v->key)+1; int n=0;
        while(p[n] && p[n]!=',' && n<63){ buf[n]=p[n]; n++; } buf[n]=0; v->value=buf; return true; }
    }
    return false; }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data=false; return true;
  default: return false;
  }
}

/* ---- tiny PNG writer (zlib).  -DW7SHOT_NOPNG builds without zlib
   (e.g. on a Pi without zlib1g-dev) and writes binary PPM instead. ---- */
#ifdef W7SHOT_NOPNG
#define SHOT_EXT "ppm"
static int write_png(const char*path,const uint32_t*px,int w,int h){
  FILE*f=fopen(path,"wb"); if(!f) return -1;
  fprintf(f,"P6\n%d %d\n255\n",w,h);
  for(size_t i=0;i<(size_t)w*h;i++){ unsigned char c[3]={(unsigned char)(px[i]>>16),(unsigned char)(px[i]>>8),(unsigned char)px[i]}; fwrite(c,1,3,f); }
  fclose(f); return 0;
}
#else
#define SHOT_EXT "png"
static void be32(unsigned char*p,uint32_t v){ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void chunk(FILE*f,const char*t,const unsigned char*d,uint32_t n){
  unsigned char h[8]; be32(h,n); memcpy(h+4,t,4); fwrite(h,1,8,f);
  if(n) fwrite(d,1,n,f);
  uint32_t c=crc32(0,(const Bytef*)t,4); if(n) c=crc32(c,d,n);
  unsigned char cc[4]; be32(cc,c); fwrite(cc,1,4,f);
}
static int write_png(const char*path,const uint32_t*px,int w,int h){
  size_t raw=(size_t)h*(w*3+1);
  unsigned char*r=malloc(raw); if(!r) return -1;
  for(int y=0;y<h;y++){ unsigned char*o=r+(size_t)y*(w*3+1); *o++=0;
    for(int x=0;x<w;x++){ uint32_t c=px[(size_t)y*w+x]; *o++=(c>>16)&255; *o++=(c>>8)&255; *o++=c&255; } }
  uLongf zl=compressBound(raw); unsigned char*z=malloc(zl);
  if(!z){ free(r); return -1; }
  compress2(z,&zl,r,raw,6);
  FILE*f=fopen(path,"wb"); if(!f){ free(r); free(z); return -1; }
  static const unsigned char sig[8]={0x89,'P','N','G','\r','\n',0x1a,'\n'};
  fwrite(sig,1,8,f);
  unsigned char ih[13]; be32(ih,w); be32(ih+4,h); ih[8]=8; ih[9]=2; ih[10]=0; ih[11]=0; ih[12]=0;
  chunk(f,"IHDR",ih,13); chunk(f,"IDAT",z,(uint32_t)zl); chunk(f,"IEND",NULL,0);
  fclose(f); free(r); free(z); return 0;
}
#endif

static int parse_btn(const char*s){
  static const struct { const char*n; int b; } T[]={
    {"up",B_UP},{"down",B_DOWN},{"left",B_LEFT},{"right",B_RIGHT},{"a",B_A},{"b",B_B},
    {"x",B_X},{"y",B_Y},{"start",B_START},{"select",B_SELECT},{"l",B_L},{"r",B_R}};
  int b=0; char tmp[128]; snprintf(tmp,sizeof tmp,"%s",s);
  for(char*t=strtok(tmp,"+");t;t=strtok(NULL,"+"))
    for(size_t i=0;i<sizeof T/sizeof T[0];i++) if(!strcmp(t,T[i].n)) b|=T[i].b;
  return b;
}

/* 64-bit FNV-1a style mix over the frame, eight bytes at a step: fast
   enough to hash every frame of a long run. */
static uint64_t frame_hash(const uint32_t*px,size_t n){
  uint64_t h=1469598103934665603ULL;
  for(size_t i=0;i+1<n;i+=2){
    uint64_t w=(uint64_t)px[i] | ((uint64_t)px[i+1]<<32);
    h^=w; h*=1099511628211ULL; h^=h>>29;
  }
  return h;
}

static int cmp_d(const void*a,const void*b){ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:(x>y); }
static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1e3+ts.tv_nsec/1e6; }

int main(int argc,char**argv){
  long N=300, hashEvery=0; int quiet=0; const char*outdir="."; const char*saves="299"; const char*script=NULL; const char*pfx="f";
  const char*wavpath=NULL, *gamepath=NULL;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"-n")&&i+1<argc) N=atol(argv[++i]);
    else if(!strcmp(argv[i],"-s")&&i+1<argc) saves=argv[++i];
    else if(!strcmp(argv[i],"-o")&&i+1<argc) outdir=argv[++i];
    else if(!strcmp(argv[i],"-i")&&i+1<argc) script=argv[++i];
    else if(!strcmp(argv[i],"-p")&&i+1<argc) pfx=argv[++i];
    else if(!strcmp(argv[i],"-r")&&i+1<argc) rngs=(uint32_t)strtoul(argv[++i],NULL,0);
    else if(!strcmp(argv[i],"-w")&&i+1<argc) wavpath=argv[++i];
    else if(!strcmp(argv[i],"-H")&&i+1<argc) hashEvery=atol(argv[++i]);
    else if(!strcmp(argv[i],"-q")) quiet=1;
    else if(!strcmp(argv[i],"-g")&&i+1<argc) gamepath=argv[++i];
  }
  int every=0; if(!strncmp(saves,"every:",6)) every=atoi(saves+6);
  static long savef[256]; int ns=0;
  if(!every){ char tmp[2048]; snprintf(tmp,sizeof tmp,"%s",saves);
    for(char*t=strtok(tmp,",");t&&ns<256;t=strtok(NULL,",")) savef[ns++]=atol(t); }
  static long sf[512]; static int sb[512]; int nsc=0;
  if(script){ char tmp[4096]; snprintf(tmp,sizeof tmp,"%s",script);
    char*save=NULL;
    for(char*t=strtok_r(tmp,";",&save);t&&nsc<512;t=strtok_r(NULL,";",&save)){
      char*c=strchr(t,':'); if(!c) continue; *c=0; sf[nsc]=atol(t); sb[nsc]=parse_btn(c+1); nsc++; } }

  if(wavpath){ wavf=fopen(wavpath,"wb"); if(wavf) wav_head(wavf,0); else perror(wavpath); }
  retro_set_environment(env);
  retro_set_video_refresh(vid);
  retro_set_audio_sample_batch(aud);
  retro_set_input_poll(inpoll);
  retro_set_input_state(inst);
  double t0=now_ms();
  retro_init();
  { static char gbuf[4096]; struct retro_game_info gi; memset(&gi,0,sizeof gi);
    size_t gn=0; FILE*gf=gamepath?fopen(gamepath,"rb"):NULL;
    if(gf){ gn=fread(gbuf,1,sizeof gbuf,gf); fclose(gf); gi.path=gamepath; gi.data=gbuf; gi.size=gn; }
    else if(gamepath) perror(gamepath);
    retro_load_game(gf?&gi:NULL);
    if(gamepath) printf("content %s: sound %d music %d turbo %d limiter %d credits %lld\n",
                        gamepath,opt_sound,opt_music,opt_turbo,opt_limiter,G.credits); }
  double tinit=now_ms()-t0;

  double sum=0,mx=0; long mxf=0;
  double*ft=(double*)malloc(sizeof(double)*(size_t)(N>0?N:1));  /* for the median */
  uint64_t digest=1469598103934665603ULL; long nhash=0;
  for(long f=0;f<N;f++){
    cur_btn=0;
    for(int k=0;k<nsc;k++) if(sf[k]==f) cur_btn|=sb[k];
    double a=now_ms();
    retro_run();
    double d=now_ms()-a; sum+=d; if(ft) ft[f]=d; if(d>mx){ mx=d; mxf=f; }
    if(hashEvery>0 && (f%hashEvery)==0 && last_fb){
      uint64_t h=frame_hash(last_fb,(size_t)FBW*FBH);
      digest^=h; digest*=1099511628211ULL; nhash++;
      if(!quiet) printf("h %ld %d %016llx\n",f,G.state,(unsigned long long)h);
    }
    int save=every? (f%every==0) : 0;
    for(int k=0;k<ns && !save;k++) if(savef[k]==f) save=1;
    if(save && last_fb){
      char path[1024]; snprintf(path,sizeof path,"%s/%s%05ld." SHOT_EXT,outdir,pfx,f);
      if(write_png(path,last_fb,FBW,FBH)==0) printf("wrote %s  (state %d)\n",path,G.state);
    }
  }
  if(hashEvery>0) printf("digest %016llx over %ld frames\n",(unsigned long long)digest,nhash);
  /* the median and 95th percentile shrug off the odd preempted frame,
     which on a shared box the mean and max do not */
  double p50=0,p95=0;
  if(ft && N>0){
    qsort(ft,(size_t)N,sizeof(double),cmp_d);
    p50=ft[N/2]; p95=ft[(N*95)/100<N?(N*95)/100:N-1];
  }
  printf("init %.1f ms   frames %ld   mean %.2f ms   max %.2f ms (frame %ld)   p50 %.2f  p95 %.2f\n",
         tinit,N,sum/N,mx,mxf,p50,p95);
  free(ft);
  printf("audio_frame  mean %.3f ms   max %.3f ms\n",aud_prof_n?aud_prof_sum/aud_prof_n:0.0,aud_prof_max);
  if(wavf){
    fseek(wavf,0,SEEK_SET); wav_head(wavf,(uint32_t)wav_frames); fclose(wavf);
    for(int c=0;c<2;c++){
      double n=wav_frames>0?(double)wav_frames:1.0, rms=sqrt(wav_sq[c]/n);
      printf("wav %s  peak %5d (%6.2f dBFS)   rms %6.2f dBFS   dc %+.1f\n",c?"R":"L",wav_pk[c],
        20*log10((wav_pk[c]+1e-9)/32768.0),20*log10((rms+1e-9)/32768.0),wav_sum[c]/n);
    }
    printf("wav %s  %.1f s   samples >= 32000: %ld\n",wavpath,wav_frames/(double)SRATE,wav_clip);
  }
  retro_deinit();
  return 0;
}
