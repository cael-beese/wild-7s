/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7audio - bench and level checks for the WILD 7's sound engine.
 *
 *  Includes the core like w7shot does, so it measures the code that
 *  ships.  Built with the core's own optimisation flags (make audio).
 *
 *    ./w7audio            bench: worst-case audio_frame() cost, voice
 *                         start/stop click check, voice-steal check,
 *                         music lane lengths
 *    ./w7audio stat a.wav [b.wav ...]
 *                         peak, RMS, loudest 400 ms, DC, clipped samples
 *                         and the largest sample-to-sample jump
 * ===================================================================== */
#define W7_AUDIO_PROF
#include "../src/wild7_libretro.c"
#include <time.h>

static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1e3+ts.tv_nsec/1e6; }

/* ---- WAV statistics ---- */
static int stat_wav(const char*path){
  FILE*f=fopen(path,"rb"); if(!f){ perror(path); return 1; }
  unsigned char h[44]; if(fread(h,1,44,f)!=44){ fclose(f); return 1; }
  fseek(f,0,SEEK_END); long n=(ftell(f)-44)/4; fseek(f,44,SEEK_SET);
  int16_t*d=malloc((size_t)n*4); if(!d){ fclose(f); return 1; }
  if(fread(d,4,(size_t)n,f)!=(size_t)n){ fclose(f); free(d); return 1; }
  fclose(f);
  double sq=0, sum[2]={0,0}, best=0; int pk=0, jump=0; long clip=0, jat=0;
  long W=SRATE*2/5; double wsq=0;
  for(long i=0;i<n;i++){
    for(int c=0;c<2;c++){
      int v=d[i*2+c], a=v<0?-v:v;
      sq+=(double)v*v; sum[c]+=v; if(a>pk) pk=a; if(a>=32000) clip++;
      if(i>0){ int j=abs(v-d[(i-1)*2+c]); if(j>jump){ jump=j; jat=i; } }
      wsq+=(double)v*v;
    }
    if(i>=W){ for(int c=0;c<2;c++){ double v=d[(i-W)*2+c]; wsq-=v*v; } }
    if(i>=W-1 && wsq>best) best=wsq;
  }
  double rms=sqrt(sq/(2.0*(n?n:1))), wr=sqrt(best/(2.0*W));
  printf("%s  %.1f s\n  peak %d (%.2f dBFS)  rms %.2f dBFS  loudest 400ms %.2f dBFS\n"
         "  dc L %+.1f R %+.1f  samples>=32000 %ld  largest jump %d at %.3f s\n",
         path,n/(double)SRATE,pk,20*log10((pk+1e-9)/32768.0),20*log10((rms+1e-9)/32768.0),
         20*log10((wr+1e-9)/32768.0),sum[0]/(n?n:1),sum[1]/(n?n:1),clip,jump,jat/(double)SRATE);
  free(d);
  return 0;
}

static void reset_audio(void){
  memset(aud_v,0,sizeof aud_v);
  memset(rv_cb,0,sizeof rv_cb); memset(rv_ab,0,sizeof rv_ab); memset(rv_pre,0,sizeof rv_pre);
  memset(rv_cf,0,sizeof rv_cf); memset(m_x1,0,sizeof m_x1); memset(m_y1,0,sizeof m_y1); m_lim=0;
  for(int p=0;p<2;p++){ mp[p].tr=-1; mp[p].firing=0; mp[p].gain=0; }
  aud_prof_sum=aud_prof_max=0; aud_prof_n=0;
}

/*  Keep every effects voice busy with a spread of the expensive types. */
static void fill_sfx(int nv){
  static const int ty[9]={SND_BRASS,SND_BELL,SND_PLUCK,SND_WHOOSH,SND_SQUARE,SND_SAW,SND_NOISE,SND_SINE,SND_BRASS};
  static int k=0;
  for(int i=0;i<nv;i++){
    if(aud_v[i].on) continue;
    int t=ty[k++%9];
    float f=110.0f*(1+(k%24));
    avoice_t*V=sv(0,t,f,f*1.5f,2.0f,0.01f);
    if(t==SND_BRASS) aud_adsr(V,0.02f,0.5f,0.8f,0.2f);
  }
}

static int cmpd(const void*a,const void*b){ double x=*(const double*)a,y=*(const double*)b; return x<y?-1:x>y; }
static void bench(const char*name,int state,int infree,int nv){
  reset_audio();
  G.state=state; G.inFree=infree; G.fsMult=5; G.t=10;
  for(int i=0;i<NCELL;i++) G.hold.val[i]=100;
  mus_I=1.0f; mus_scene=1.0f;
  long busy=0; static double t[1200];
  for(int f=0;f<1200;f++){
    fill_sfx(nv);
    mus_I=1.0f;
    double a=now_ms(); audio_frame(); t[f]=now_ms()-a;
    for(int v=0;v<NVTOT;v++) busy+=aud_v[v].on;
  }
  qsort(t,1200,sizeof(double),cmpd);
  printf("  %-18s %2d fx: mean %.3f  median %.3f  p99 %.3f  max %.3f ms  (%.1f voices sounding)\n",
         name,nv,aud_prof_sum/aud_prof_n,t[600],t[1188],t[1199],busy/1200.0);
}

/*  One voice alone, dry.  A click is a start or an end that jumps: the
 *  first and last 0.1 ms should be near zero against the peak, and the
 *  first 1 ms shows how steep the attack ramp is.                       */
static void click_test(int type){
  reset_audio(); opt_music=0; G.state=ST_IDLE; G.inFree=0;
  static float mono[SPF*120];
  avoice_t*V=sv(0,type,440,440,0.25f,0.3f); V->send=0;
  int n=0;
  for(int f=0;f<120;f++){
    audio_frame();
    for(int i=0;i<SPF;i++) mono[n++]=abuf[i*2]/32768.0f;
  }
  float pk=0; int first=-1, last=-1;
  for(int i=0;i<n;i++){ float a=fabsf(mono[i]); if(a>pk) pk=a; }
  for(int i=0;i<n;i++) if(fabsf(mono[i])>pk*1e-3f){ if(first<0) first=i; last=i; }
  float s0=0, s1=0, sa=0; int ms=SRATE/1000, us=SRATE/10000;
  for(int i=first;i<first+us && i<n;i++) if(fabsf(mono[i])>s0) s0=fabsf(mono[i]);
  for(int i=first;i<first+ms && i<n;i++) if(fabsf(mono[i])>sa) sa=fabsf(mono[i]);
  for(int i=last-us;i<=last && i>=0;i++) if(fabsf(mono[i])>s1) s1=fabsf(mono[i]);
  printf("  type %d  peak %.3f  first 0.1 ms %5.1f%%  first 1 ms %5.1f%%  last 0.1 ms %5.1f%%  length %.3f s\n",
         type,pk,100*s0/(pk+1e-9f),100*sa/(pk+1e-9f),100*s1/(pk+1e-9f),(last-first)/(float)SRATE);
  opt_music=1;
}

/*  Fill the pool with loud held notes, then ask for more: every new note
 *  steals.  The largest jump in the output should not grow.            */
static void steal_test(void){
  reset_audio(); opt_music=0;
  for(int i=0;i<NVOICE;i++){
    avoice_t*V=sv(0,SND_SINE,200.0f+7.0f*i,200.0f+7.0f*i,5.0f,0.02f);
    aud_adsr(V,0.01f,0,1,0.1f); V->send=0;
  }
  int jb=0, ja=0, prev=0;
  for(int f=0;f<20;f++){
    if(f==10) for(int k=0;k<16;k++){ avoice_t*V=sv(0,SND_SINE,90,90,0.5f,0.001f); V->send=0; }
    audio_frame();
    for(int i=0;i<SPF;i++){
      int v=abuf[i*2], j=abs(v-prev); prev=v;
      if(f>=5 && f<10 && j>jb) jb=j;
      if(f>=10 && j>ja) ja=j;
    }
  }
  printf("  steal: largest jump before %d, while stealing 16 voices %d\n",jb,ja);
  opt_music=1;
}

/*  A burst, then a minute of silence with the music off: the reverb,
 *  DC blocker and limiter tails decay towards zero.  If any of them
 *  reached denormal range the late frames would cost far more than the
 *  early ones (build without -ffast-math to see it: that is how
 *  RetroArch runs the core, with no flush-to-zero).                    */
static void silence_test(void){
  reset_audio(); opt_music=0; G.state=ST_IDLE; G.inFree=0;
  sfx_jackpot(JP_MAJOR);
  double early=0, late=0;
  for(int f=0;f<3600;f++){
    double a=now_ms(); audio_frame(); double d=now_ms()-a;
    if(f>=300 && f<900) early+=d;
    if(f>=3000) late+=d;
  }
  printf("  silence: frames 300-900 mean %.4f ms, frames 3000-3600 mean %.4f ms\n",early/600,late/600);
  opt_music=1;
}

int main(int argc,char**argv){
  if(argc>2 && !strcmp(argv[1],"stat")){
    int e=0; for(int i=2;i<argc;i++) e|=stat_wav(argv[i]); return e;
  }
  retro_init();
  aud_init();
  printf("music lanes (steps; a multiple of 16 keeps them in bar):\n");
  for(int t=0;t<NTRACK;t++){
    printf("  track %d:",t);
    for(int l=0;l<MLANES;l++) if(TRK[t].ln[l].pat){
      printf(" %d%s",mus_len[t][l],mus_len[t][l]%16?"(!)":"");
    }
    printf("\n");
  }
  printf("audio_frame, effect voices held busy + music + reverb (1200 frames):\n");
  bench("free spins, full",ST_IDLE,1,48);
  bench("hold & spin, full",ST_HOLD,0,48);
  bench("wheel",ST_WHEEL,0,48);
  bench("lounge",ST_IDLE,0,48);
  bench("free spins, full",ST_IDLE,1,64);
  printf("click check, one 0.25 s voice at 440 Hz, dry:\n");
  for(int t=0;t<=SND_BRASS;t++) click_test(t);
  steal_test();
  silence_test();
  retro_deinit();
  return 0;
}
