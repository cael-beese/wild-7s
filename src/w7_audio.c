/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_audio.c - the sound engine: a stereo software synth, a small
 *  music sequencer and every sound effect in the game.
 *
 *  Included from wild7_libretro.c where the old AUDIO section was (after
 *  game_t, before update()), because update() calls it.  Everything runs
 *  on the main thread: the sfx_* calls from update(), audio_frame() from
 *  retro_run().  Nothing here touches the game RNG - the synth has its
 *  own - so sound on or off can never change an outcome.
 *
 *  THE VOICE.  One struct plays every sound: an oscillator (PolyBLEP
 *  square and saw, triangle, noise, FM bell, sine, Karplus-Strong pluck,
 *  swept band noise, a detuned-saw "brass" through a resonant low-pass),
 *  an envelope, a pitch sweep, a stereo position and a reverb send.
 *
 *  CONTROL RATE.  A frame of 735 samples is cut into 35 blocks of 21.
 *  Envelope, pitch and filter coefficients are worked out once per block
 *  and the amplitude is ramped linearly across it, so the per-sample
 *  work is only the oscillator and three multiply-adds.  Voices are
 *  rendered one at a time over the whole frame (voice-major), which
 *  keeps the type switch out of the inner loop.  Start times are
 *  quantised to a block, 0.48 ms, which nobody can hear.
 *
 *  POOLS.  64 voices for effects, 32 for music, so the band can never
 *  steal a jackpot note and a fanfare can never silence the bass.  When a
 *  pool is full the quietest sounding voice is stolen (a queued note is
 *  taken only when every voice is louder), and what it was outputting is
 *  faded out over ~3 ms by a residue on the bus, so a steal never clicks.
 *
 *  THE MIX.  Effects on bus 0; the two music players (the one playing
 *  and the one fading out) on buses 1 and 2.  Music is ducked under loud
 *  effects by a side-chain on bus 0 and by explicit music_duck() calls.
 *  Every bus feeds a stereo reverb (four damped combs and two allpasses
 *  per side, after a 12 ms pre-delay).  The master is DC-blocked, then a
 *  peak limiter at -1.9 dBFS, then a soft clip whose ceiling is -0.35
 *  dBFS, so the biggest fanfare squashes rather than cracks.
 * ===================================================================== */
#include "w7_audio.h"
#ifdef W7_AUDIO_PROF
#include <time.h>
#endif

static inline int stripAt(int r,int i);      /* with the reels, further down */

#define AUD_SUB    21                         /* control block, samples       */
#define AUD_NSUB   (SPF/AUD_SUB)              /* 35 a frame                   */
#define AUD_SR     ((float)SRATE)
#define AUD_SUBDT  ((float)AUD_SUB/(float)SRATE)
#define NVOICE     64                         /* effects                      */
#define NMVOICE    32                         /* music                        */
#define NVTOT      (NVOICE+NMVOICE)
#define KSLEN      1024                       /* pluck delay: 43 Hz floor     */
#define SINTAB     4096
#define NBUS       3

#define AUD_MASTER 0.50f       /* base game ~3 dB over the old synth        */
#define AUD_MUSGAIN 0.40f      /* music sits well under the effects        */
#define AUD_LIM    0.80f       /* limiter threshold                        */
#define AUD_KNEE   0.80f       /* soft clip starts here ...                */
#define AUD_CEIL   0.96f       /* ... and never passes this                */

#if SPF % AUD_SUB
#error "SPF must be a multiple of the audio control block"
#endif

static int opt_music = 1;

enum { ENV_LEGACY, ENV_ADSR };
enum { SW_LIN, SW_EXP, SW_LOG };
enum { FLT_LP, FLT_BP, FLT_HP };
enum { TAG_NONE=0, TAG_ANTIC=1 /* +reel */ };

typedef struct {
  uint8_t on, type, env, sweep, bus, filt, tag, kill, started;
  int   del;                    /* control blocks before it sounds        */
  uint32_t age;
  float t, dur;                 /* seconds sounded, total length          */
  float f0, f1, fc, fg, fm, glide;
  float vol, gl, gr, send;
  float att, dk, dcur, dmul, sus, rel;
  float amp, kstep;             /* envelope at the end of the last block  */
  float ph, ph2, det;
  float fmr, fmi, fmic, fmdk, fmimul;
  float fcb, fce, q;
  float s1, s2;                 /* filter state / pluck feedback          */
  float kbright, kt60, ksg;
  int   ksn, ksi;
  float lastL, lastR;           /* the last sample it put on its bus      */
} avoice_t;

static avoice_t aud_v[NVTOT];
static avoice_t aud_dummy;                    /* sink when sound is off     */
static float    aud_ks[NVTOT][KSLEN];
static float    aud_sin[SINTAB+1], aud_mtof[128];
static float    aud_bl[NBUS][SPF], aud_br[NBUS][SPF], aud_bs[NBUS][SPF];
static float    aud_resL[NBUS], aud_resR[NBUS];   /* steal residue        */
static uint32_t aud_rng = 0x2545F491u, aud_age = 0;
static int      aud_ready = 0, aud_live = 0, aud_tag = TAG_NONE;
static long     aud_fc = 0;                        /* frames rendered      */

static inline uint32_t arng(void){
  aud_rng ^= aud_rng<<13; aud_rng ^= aud_rng>>17; aud_rng ^= aud_rng<<5;
  return aud_rng;
}
static inline float arnd(void){ return (float)(arng()>>8)*(1.0f/16777216.0f); }
static inline float anoise(void){ return (float)(int32_t)arng()*(1.0f/2147483648.0f); }

/*  Sine by table, phase in cycles (anything above -8).                 */
static inline float aud_sinp(float ph){
  float x=(ph+8.0f)*(float)SINTAB;
  int i=(int)x; float fr=x-(float)i; i&=SINTAB-1;
  return aud_sin[i]+fr*(aud_sin[i+1]-aud_sin[i]);
}
/*  The PolyBLEP residual: rounds the corner of a step so it does not
 *  alias.  t is the phase, dt the phase increment.                      */
static inline float aud_blep(float t,float dt,float idt){
  if(t<dt){ t*=idt; return t+t-t*t-1.0f; }
  if(t>1.0f-dt){ t=(t-1.0f)*idt; return t*t+t+t+1.0f; }
  return 0.0f;
}
/*  Zero-delay-feedback state-variable filter coefficients (Simper).
 *  tan() by a Pade approximant, good to 0.1% up to 12 kHz.             */
static inline void aud_svf(float fc,float q,float*a1,float*a2,float*a3,float*k){
  float x=3.14159265f*fc/AUD_SR;
  x=clampf(x,0.0003f,0.85f);
  float x2=x*x, g=x*(15.0f-x2)/(15.0f-6.0f*x2);
  *k=1.0f/q; *a1=1.0f/(1.0f+g*(g+*k)); *a2=g**a1; *a3=g**a2;
}

static void mus_parse(void);
static void aud_init(void){
  if(aud_ready) return;
  aud_ready=1;
  for(int i=0;i<=SINTAB;i++) aud_sin[i]=sinf(TAU*(float)i/(float)SINTAB);
  for(int i=0;i<128;i++) aud_mtof[i]=440.0f*powf(2.0f,(float)(i-69)/12.0f);
  mus_parse();
}

/* -- voice allocation ----------------------------------------------- */
static avoice_t* aud_alloc(int lo,int hi){
  int best=lo; float bs=1e30f;
  for(int i=lo;i<hi;i++){
    avoice_t*V=&aud_v[i];
    if(!V->on){ best=i; bs=-1e30f; break; }
    /* sounding: its current level; queued: after every sounding voice,
       the latest-starting first */
    float s = V->started ? V->amp - (V->kill?1.0f:0.0f)
                         : 4.0f + V->vol - (float)V->del*1e-4f;
    if(s<bs || (s==bs && V->age<aud_v[best].age)){ bs=s; best=i; }
  }
  avoice_t*V=&aud_v[best];
  if(V->on && V->started){ aud_resL[V->bus]+=V->lastL; aud_resR[V->bus]+=V->lastR; }
  memset(V,0,sizeof *V);
  V->age=++aud_age;
  return V;
}

static void aud_pan(avoice_t*V,float p){
  p=clampf(p,-1.0f,1.0f);
  V->gl = p>0.0f ? 1.0f-p : 1.0f;
  V->gr = p<0.0f ? 1.0f+p : 1.0f;
}
/* a struck sound: fast attack, exponential decay, short tail fade */
static void aud_perc(avoice_t*V,float att,float dk){
  V->env=ENV_ADSR; V->att=att; V->dk=dk; V->sus=0.0f;
  V->rel = V->dur*0.25f<0.03f ? V->dur*0.25f : 0.03f;
}
/* a held sound: attack, decay toward sus, release over the last rel s */
static void aud_adsr(avoice_t*V,float att,float dk,float sus,float rel){
  V->env=ENV_ADSR; V->att=att; V->dk=dk; V->sus=sus;
  V->rel = rel<V->dur ? rel : V->dur;
}

/*  A voice with the defaults of its type, i.e. exactly what snd() asks
 *  for.  The caller may then adjust fields; derived values (decay
 *  multipliers, sweep factors, the pluck's string) are set up when the
 *  voice actually starts, so adjusting afterwards is safe.              */
static avoice_t* aud_new(int bus,float del,int type,float f0,float f1,float dur,float vol){
  if(!opt_sound || (bus>0 && !opt_music)){
    memset(&aud_dummy,0,sizeof aud_dummy); return &aud_dummy;
  }
  aud_init();
  avoice_t*V = bus==0 ? aud_alloc(0,NVOICE) : aud_alloc(NVOICE,NVTOT);
  if(type<0 || type>SND_BRASS) type=SND_TRI;
  f0 = f0>1.0f ? (f0<20000.0f?f0:20000.0f) : 1.0f;
  f1 = f1>1.0f ? (f1<20000.0f?f1:20000.0f) : 1.0f;
  if(!(dur>0.005f)) dur=0.005f;
  if(!(vol>0.0f)) vol=0.0f;
  V->on=1; V->type=(uint8_t)type; V->bus=(uint8_t)bus; V->tag=(uint8_t)aud_tag;
  V->del = del>0.0f ? (int)(del*(AUD_SR/(float)AUD_SUB)+0.5f) : 0;
  V->f0=f0; V->f1=f1; V->dur=dur; V->vol=vol; V->gl=V->gr=1.0f;
  V->send=0.14f; V->att=0.0015f; V->env=ENV_LEGACY; V->rel=0.02f;
  V->sweep=SW_LIN; V->q=0.7f; V->fcb=1.5f; V->fce=4.0f; V->det=0.004f;
  V->fmr=3.5f; V->fmi=0.45f; V->kbright=0.6f; V->kt60=dur;
  switch(type){
  case SND_NOISE: V->send=0.10f; break;
  case SND_BELL:  aud_perc(V,0.001f,4.5f/dur); V->rel=dur*0.15f;
                  V->fmdk=6.0f/dur; V->send=0.30f; break;
  case SND_SINE:  V->att=0.003f; break;
  case SND_PLUCK: aud_adsr(V,0.0005f,0.0f,1.0f,dur*0.1f); V->send=0.20f; break;
  case SND_WHOOSH:V->att=dur*0.35f; V->filt=FLT_BP; V->q=1.4f;
                  V->sweep=SW_LOG; V->send=0.30f; break;
  case SND_BRASS: aud_adsr(V,0.02f,2.5f,0.65f,dur*0.18f<0.03f?0.03f:dur*0.18f);
                  V->send=0.25f; break;
  }
  return V;
}
static avoice_t* sv(float del,int type,float f0,float f1,float dur,float vol){
  return aud_new(0,del,type,f0,f1,dur,vol);
}

/*  Fade every voice carrying `tag` out over 60 ms (queued ones just go). */
static void aud_release_tag(int tag){
  for(int i=0;i<NVTOT;i++){
    avoice_t*V=&aud_v[i];
    if(!V->on || V->tag!=tag || V->kill) continue;
    if(!V->started){ V->on=0; continue; }
    V->kill=1; V->kstep=V->amp*(AUD_SUBDT/0.06f)+1e-7f;
  }
}
static void aud_release_bus(int bus){
  for(int i=0;i<NVTOT;i++){
    avoice_t*V=&aud_v[i];
    if(!V->on || V->bus!=bus || V->kill) continue;
    if(!V->started){ V->on=0; continue; }
    V->kill=1; V->kstep=V->amp*(AUD_SUBDT/0.03f)+1e-7f;
  }
}

/* -- the original API ----------------------------------------------- */
AUDAPI void snd(float f0,float f1,float dur,int type,float vol){
  sv(0,type,f0,f1,dur,vol);
}
AUDAPI void snd_at(float del,float f0,float f1,float dur,int type,float vol){
  sv(del,type,f0,f1,dur,vol);
}
AUDAPI void snd_noise(float dur,float vol,float lp){ sv(0,SND_NOISE,lp,lp,dur,vol); }
AUDAPI void snd_noise_at(float del,float dur,float vol,float lp){
  sv(del,SND_NOISE,lp,lp,dur,vol);
}
/*  A triad: triangle root and third, square fifth underneath, as it
 *  always was - now spread a little across the stereo field.           */
AUDAPI void snd_chord(float t0,float a,float b,float c,float dur,float vol){
  aud_pan(sv(t0,SND_TRI,a,a,dur,vol),-0.25f);
  aud_pan(sv(t0,SND_TRI,b,b,dur,vol*0.80f),0.25f);
  sv(t0,SND_SQUARE,c,c,dur,vol*0.45f);
}

/* -- instruments ---------------------------------------------------
 *  One place that knows what a "brass" or a "vibes" note is, shared by
 *  the effects and the music so the two sound like one machine.
 * ---------------------------------------------------------------- */
static avoice_t* aud_inst(int bus,float del,int inst,float hz,float len,float vel,float pan){
  avoice_t*V;
  if(len<0.02f) len=0.02f;
  switch(inst){
  case W7I_EP:        /* FM at 1:1 with a decaying index: a tine piano */
    V=aud_new(bus,del,SND_BELL,hz,hz,len+0.35f,0.30f*vel);
    V->fmr=1.0f; V->fmi=0.22f; V->fmdk=2.5f;
    aud_adsr(V,0.003f,1.6f,0.30f,0.30f); V->send=0.22f; break;
  case W7I_UPRIGHT:
    V=aud_new(bus,del,SND_PLUCK,hz,hz,len+0.08f,0.55f*vel);
    V->kbright=0.30f; V->kt60=1.4f; V->rel=0.08f; V->send=0.08f; break;
  case W7I_VIBES:
    V=aud_new(bus,del,SND_BELL,hz,hz,len+0.9f,0.22f*vel);
    V->fmr=4.0f; V->fmi=0.16f; V->fmdk=7.0f;
    aud_adsr(V,0.002f,1.1f,0.0f,0.4f); V->send=0.35f; break;
  case W7I_PLUCK:
    V=aud_new(bus,del,SND_PLUCK,hz,hz,len+0.10f,0.40f*vel);
    V->kbright=0.75f; V->kt60=0.7f; V->rel=0.08f; V->send=0.25f; break;
  case W7I_SAWBASS:
    V=aud_new(bus,del,SND_BRASS,hz,hz,len,0.30f*vel);
    V->det=0.003f; V->fcb=1.1f; V->fce=6.0f;
    aud_adsr(V,0.003f,7.0f,0.5f,0.05f); V->send=0.04f; break;
  case W7I_PAD:
    V=aud_new(bus,del,SND_BRASS,hz,hz,len+0.5f,0.12f*vel);
    V->det=0.007f; V->fcb=1.0f; V->fce=1.0f;
    aud_adsr(V,0.35f,0.4f,0.85f,0.5f); V->send=0.45f; break;
  case W7I_BRASS:
    V=aud_new(bus,del,SND_BRASS,hz,hz,len+0.15f,0.22f*vel);
    V->det=0.005f; V->fcb=1.2f; V->fce=6.0f;
    aud_adsr(V,0.025f,2.0f,0.7f,0.15f); V->send=0.30f; break;
  case W7I_BELL:
    V=aud_new(bus,del,SND_BELL,hz,hz,len+1.2f,0.20f*vel);
    V->fmr=3.5f; V->fmi=0.42f; V->fmdk=3.0f;
    aud_perc(V,0.001f,2.2f); V->rel=0.35f; V->send=0.40f; break;
  case W7I_MARIMBA:   /* fast-dying index: a woody knock, then a pure tone */
    V=aud_new(bus,del,SND_BELL,hz,hz,0.45f,0.30f*vel);
    V->fmr=4.0f; V->fmi=0.30f; V->fmdk=22.0f;
    aud_perc(V,0.001f,6.0f); V->rel=0.08f; V->send=0.20f; break;
  case W7I_TIMP:
    V=aud_new(bus,del,SND_SINE,hz*1.3f,hz,1.3f,0.60f*vel);
    V->sweep=SW_EXP; V->glide=14.0f;
    aud_perc(V,0.002f,2.4f); V->rel=0.25f; V->send=0.35f;
    aud_pan(V,pan);
    V=aud_new(bus,del,SND_NOISE,700,300,0.10f,0.20f*vel);
    aud_perc(V,0.001f,30.0f); break;
  case W7I_STRING:
    V=aud_new(bus,del,SND_BRASS,hz,hz,len+0.12f,0.14f*vel);
    V->det=0.008f; V->fcb=1.6f; V->fce=2.0f;
    aud_adsr(V,0.06f,0.6f,0.85f,0.12f); V->send=0.35f; break;
  case W7I_SUB:
    V=aud_new(bus,del,SND_SINE,hz,hz,len+0.1f,0.50f*vel);
    aud_adsr(V,0.01f,0.0f,1.0f,0.1f); V->send=0.0f; break;
  default:
    V=aud_new(bus,del,SND_TRI,hz,hz,len,0.2f*vel); break;
  }
  aud_pan(V,pan);
  return V;
}

/*  The drum kit.  Every piece is a sine with a pitch drop, a band of
 *  noise, or both - which is all a drum machine ever was.               */
static void aud_drum(int bus,float del,char c,float vel,float pan){
  avoice_t*V;
  switch(c){
  case 'k':                                           /* kick            */
    V=aud_new(bus,del,SND_SINE,150,45,0.32f,0.85f*vel);
    V->sweep=SW_EXP; V->glide=28.0f; aud_perc(V,0.001f,9.0f); V->send=0.03f;
    V=aud_new(bus,del,SND_WHOOSH,3200,3200,0.012f,0.18f*vel);
    aud_perc(V,0.0005f,80.0f); V->q=0.8f; break;
  case 's': case 'r':                                 /* snare / roll    */
    { float g = c=='r' ? 0.45f : 1.0f, d = c=='r' ? 0.10f : 0.20f;
      V=aud_new(bus,del,SND_TRI,230,170,0.09f,0.30f*vel*g);
      aud_perc(V,0.001f,30.0f); aud_pan(V,pan);
      V=aud_new(bus,del,SND_WHOOSH,2000,2000,d,0.60f*vel*g);
      aud_perc(V,0.001f,c=='r'?26.0f:15.0f); V->q=0.7f; V->send=0.25f; }
    break;
  case 'c':                                           /* clap            */
    for(int i=0;i<3;i++){
      V=aud_new(bus,del+i*0.011f,SND_WHOOSH,1200,1200,0.02f,0.35f*vel);
      aud_perc(V,0.0005f,60.0f); V->q=1.3f; aud_pan(V,pan);
    }
    V=aud_new(bus,del+0.033f,SND_WHOOSH,1200,1100,0.16f,0.30f*vel);
    aud_perc(V,0.001f,16.0f); V->q=1.3f; V->send=0.35f; break;
  case 'h':                                           /* closed hat      */
    V=aud_new(bus,del,SND_WHOOSH,8000,8000,0.05f,0.20f*vel);
    aud_perc(V,0.0005f,55.0f); V->filt=FLT_HP; V->q=0.7f; V->send=0.10f; break;
  case 'o':                                           /* open hat        */
    V=aud_new(bus,del,SND_WHOOSH,7000,7000,0.30f,0.15f*vel);
    aud_perc(V,0.0005f,9.0f); V->filt=FLT_HP; V->q=0.7f; break;
  case 'b':                                           /* brush swish     */
    V=aud_new(bus,del,SND_WHOOSH,3500,3000,0.18f,0.12f*vel);
    aud_perc(V,0.03f,10.0f); V->q=0.6f; break;
  case 'y':                                           /* ride            */
    V=aud_new(bus,del,SND_BELL,3100,3100,0.5f,0.045f*vel);
    V->fmr=1.47f; V->fmi=0.35f; V->fmdk=4.0f; aud_perc(V,0.001f,5.0f); aud_pan(V,pan);
    V=aud_new(bus,del,SND_WHOOSH,9000,9000,0.3f,0.05f*vel);
    aud_perc(V,0.001f,9.0f); V->filt=FLT_HP; break;
  case 'a':                                           /* shaker          */
    V=aud_new(bus,del,SND_WHOOSH,6000,6000,0.07f,0.10f*vel);
    aud_perc(V,0.01f,30.0f); V->filt=FLT_HP; V->q=0.8f; break;
  case 'C':                                           /* crash           */
    V=aud_new(bus,del,SND_WHOOSH,5000,4000,1.9f,0.18f*vel);
    aud_perc(V,0.001f,2.0f); V->filt=FLT_HP; V->q=0.6f; V->send=0.45f; aud_pan(V,pan);
    V=aud_new(bus,del,SND_WHOOSH,3000,2600,1.0f,0.08f*vel);
    aud_perc(V,0.001f,3.5f); V->q=1.0f; V->send=0.45f; break;
  case 'l': case 'd':                                 /* heartbeat       */
    V=aud_new(bus,del,SND_SINE,c=='l'?72:64,c=='l'?44:42,c=='l'?0.24f:0.20f,
              (c=='l'?0.95f:0.60f)*vel);
    V->sweep=SW_EXP; V->glide=22.0f; aud_perc(V,0.002f,11.0f); V->send=0.05f; break;
  case 'T':                                           /* taiko           */
    V=aud_new(bus,del,SND_SINE,120,58,0.7f,0.95f*vel);
    V->sweep=SW_EXP; V->glide=10.0f; aud_perc(V,0.001f,4.5f); V->send=0.30f; aud_pan(V,pan);
    V=aud_new(bus,del,SND_NOISE,900,900,0.09f,0.30f*vel);
    aud_perc(V,0.001f,25.0f); break;
  case 't':                                           /* tom             */
    V=aud_new(bus,del,SND_SINE,190,120,0.35f,0.55f*vel);
    V->sweep=SW_EXP; V->glide=12.0f; aud_perc(V,0.001f,8.0f); V->send=0.2f; break;
  default: return;
  }
  aud_pan(V,pan);
}

AUDAPI void snd_inst(float del,int inst,float hz,float len,float vel,float pan){
  aud_inst(0,del,inst,hz,len,vel,pan);
}
AUDAPI void snd_drum(float del,char code,float vel,float pan){
  aud_drum(0,del,code,vel,pan);
}

/* -- rendering ------------------------------------------------------- */
static void aud_start(avoice_t*V,int vi){
  V->started=1; V->t=0; V->amp=0; V->dcur=1.0f; V->fc=V->f0; V->fg=1.0f;
  V->dmul   = V->dk>0.0f   ? expf(-V->dk*AUD_SUBDT)   : 1.0f;
  V->fmic   = V->fmi;
  V->fmimul = V->fmdk>0.0f ? expf(-V->fmdk*AUD_SUBDT) : 1.0f;
  if(V->sweep==SW_EXP)      V->fm=expf(-V->glide*AUD_SUBDT);
  else if(V->sweep==SW_LOG) V->fm=powf(V->f1/V->f0,AUD_SUBDT/V->dur);
  if(V->att<0.0001f) V->att=0.0001f;
  if(V->rel<0.001f)  V->rel=0.001f;
  if(V->type==SND_PLUCK){
    /* the string: a burst of filtered noise, zero-mean, normalised */
    int n=(int)(AUD_SR/V->f0-0.5f+0.5f);
    n=clampi(n,2,KSLEN);
    float*b=aud_ks[vi], lp=0, mean=0, mx=0, al=clampf(V->kbright,0.03f,1.0f);
    for(int i=0;i<n;i++){ lp+=al*(anoise()-lp); b[i]=lp; mean+=lp; }
    mean/=(float)n;
    for(int i=0;i<n;i++){ b[i]-=mean; if(fabsf(b[i])>mx) mx=fabsf(b[i]); }
    float sc = mx>1e-6f ? 1.0f/mx : 0.0f;
    for(int i=0;i<n;i++) b[i]*=sc;
    float t60 = V->kt60<0.05f ? 0.05f : V->kt60;
    V->ksn=n; V->ksi=0; V->s1=0;
    V->ksg=powf(0.001f,1.0f/(V->f0*t60));
  }
}

static inline float aud_env(avoice_t*V,float t1){
  if(V->kill){
    float a=V->amp-V->kstep;
    if(a<=0.0f){ a=0.0f; V->on=0; }
    return a;
  }
  float A = t1<V->att ? t1/V->att : 1.0f, D;
  if(V->env==ENV_LEGACY){
    float u=t1/V->dur; if(u>1.0f) u=1.0f;
    D=(1.0f-u)*(1.0f-u);
  } else {
    V->dcur*=V->dmul;
    D=V->sus+(1.0f-V->sus)*V->dcur;
    float r=(V->dur-t1)/V->rel;
    if(r<1.0f) D*= r>0.0f ? r : 0.0f;
  }
  return V->vol*A*D;
}
static inline float aud_freq(avoice_t*V,float t1){
  switch(V->sweep){
  case SW_EXP: V->fg*=V->fm; return V->f1+(V->f0-V->f1)*V->fg;
  case SW_LOG: V->fc*=V->fm; return V->fc;
  default: { float u=t1/V->dur; if(u>1.0f) u=1.0f; return V->f0+(V->f1-V->f0)*u; }
  }
}

static void aud_run_voice(int vi){
  avoice_t*V=&aud_v[vi];
  if(V->del>=AUD_NSUB){ V->del-=AUD_NSUB; return; }
  int sb=V->del; V->del=0;
  if(!V->started) aud_start(V,vi);
  float*L=aud_bl[V->bus], *R=aud_br[V->bus], *S=aud_bs[V->bus];
  float tmp[AUD_SUB];
  for(;sb<AUD_NSUB;sb++){
    float t1=V->t+AUD_SUBDT;
    float a0=V->amp, a1=aud_env(V,t1);
    float f=aud_freq(V,t1);
    f=clampf(f,1.0f,20000.0f);
    float inc=f*(1.0f/AUD_SR); if(inc>0.45f) inc=0.45f;
    float p=V->ph;
    switch(V->type){
    case SND_SQUARE: {
      float idt=1.0f/inc;
      for(int i=0;i<AUD_SUB;i++){
        float p2=p+0.5f; if(p2>=1.0f) p2-=1.0f;
        tmp[i]=(p<0.5f?1.0f:-1.0f)+aud_blep(p,inc,idt)-aud_blep(p2,inc,idt);
        p+=inc; if(p>=1.0f) p-=1.0f;
      } } break;
    case SND_SAW: {
      float idt=1.0f/inc;
      for(int i=0;i<AUD_SUB;i++){
        tmp[i]=2.0f*p-1.0f-aud_blep(p,inc,idt);
        p+=inc; if(p>=1.0f) p-=1.0f;
      } } break;
    case SND_NOISE: {
      /* the original one-pole, coefficient and all, so the old thuds
         keep their colour; now it can sweep */
      float al=clampf(f/(AUD_SR*0.5f),0.002f,0.95f), lp=V->s1;
      for(int i=0;i<AUD_SUB;i++){ lp+=al*(anoise()-lp); tmp[i]=lp; }
      V->s1=lp; } break;
    case SND_BELL: {
      float I=V->fmic; V->fmic*=V->fmimul;
      float dI=(V->fmic-I)*(1.0f/AUD_SUB), inc2=inc*V->fmr, q2=V->ph2;
      if(inc2>0.9f) inc2=0.9f;
      for(int i=0;i<AUD_SUB;i++){
        float m=aud_sinp(q2)*I;
        tmp[i]=aud_sinp(p+m);
        I+=dI; p+=inc; if(p>=1.0f) p-=1.0f;
        q2+=inc2; if(q2>=1.0f) q2-=1.0f;
      }
      V->ph2=q2; } break;
    case SND_SINE:
      for(int i=0;i<AUD_SUB;i++){ tmp[i]=aud_sinp(p); p+=inc; if(p>=1.0f) p-=1.0f; }
      break;
    case SND_PLUCK: {
      float*b=aud_ks[vi], g=V->ksg*0.5f, prev=V->s1;
      int n=V->ksn, j=V->ksi;
      for(int i=0;i<AUD_SUB;i++){
        float y=b[j], z=g*(y+prev);
        prev=y; b[j]=z; tmp[i]=z;
        if(++j>=n) j=0;
      }
      V->s1=prev; V->ksi=j; } break;
    case SND_WHOOSH: {
      float a1c,a2c,a3c,k;
      aud_svf(f,V->q,&a1c,&a2c,&a3c,&k);
      float wl = V->filt==FLT_LP, wb = V->filt==FLT_BP ? k : 0.0f, wh = V->filt==FLT_HP;
      float ic1=V->s1, ic2=V->s2;
      for(int i=0;i<AUD_SUB;i++){
        float v0=anoise(), v3=v0-ic2;
        float v1=a1c*ic1+a2c*v3, v2=ic2+a2c*ic1+a3c*v3;
        ic1=2.0f*v1-ic1; ic2=2.0f*v2-ic2;
        tmp[i]=wl*v2+wb*v1+wh*(v0-k*v1-v2);
      }
      V->s1=ic1; V->s2=ic2; } break;
    case SND_BRASS: {
      /* the filter opens with the envelope - the "blat" of a brass
         attack.  Two cascaded one-poles rather than a state-variable
         filter: 12 dB/oct either way, and each stage's feedback is a
         single multiply-add, so it pipelines instead of stalling.     */
      float en = V->vol>0.0f ? a1/V->vol : 0.0f;
      float w=6.2831853f*f*(V->fcb+V->fce*en)*(1.0f/AUD_SR);
      if(w>2.5f) w=2.5f;
      float c=w/(1.0f+w);
      float i1=inc*(1.0f+V->det), i2=inc*(1.0f-V->det);
      float d1=1.0f/i1, d2=1.0f/i2, q2=V->ph2, y1=V->s1, y2=V->s2;
      for(int i=0;i<AUD_SUB;i++){
        float x=(p+q2-1.0f)-0.5f*(aud_blep(p,i1,d1)+aud_blep(q2,i2,d2));
        p+=i1; if(p>=1.0f) p-=1.0f;
        q2+=i2; if(q2>=1.0f) q2-=1.0f;
        y1+=c*(x-y1); y2+=c*(y1-y2);
        tmp[i]=y2;
      }
      V->ph2=q2; V->s1=y1; V->s2=y2; } break;
    default: /* SND_TRI */
      for(int i=0;i<AUD_SUB;i++){
        tmp[i]=4.0f*fabsf(p-0.5f)-1.0f;
        p+=inc; if(p>=1.0f) p-=1.0f;
      }
      break;
    }
    V->ph=p;
    int o=sb*AUD_SUB;
    float da=(a1-a0)*(1.0f/AUD_SUB), a=a0, gl=V->gl, gr=V->gr, gs=V->send;
    float*l=L+o, *r=R+o, *s=S+o;
    for(int i=0;i<AUD_SUB;i++){
      a+=da;
      float x=tmp[i]*a;
      l[i]+=x*gl; r[i]+=x*gr; s[i]+=x*gs;
    }
    V->amp=a1; V->t=t1;
    float last=tmp[AUD_SUB-1]*a1;
    V->lastL=last*gl; V->lastR=last*gr;
    if(t1>=V->dur || !V->on){ V->on=0; break; }
  }
}

/* === MUSIC =======================================================
 *  Tunes are written as text, one lane per instrument, one token per
 *  sixteenth note:
 *
 *      .      rest              -      hold the previous note
 *      C4     a note (C4 = middle C, sharps #, flats b)
 *      A3+C4+E4   a chord       >X     accent
 *      .*7    any token repeated
 *
 *  A drum lane's tokens are kit letters (see snd_drum), and "kh" hits
 *  both.  Lanes loop independently, so a one-bar drum loop runs under
 *  an eight-bar tune.  A lane with minI > 0 only plays once the track's
 *  intensity has reached it: that is how HOLD & SPIN piles on layers as
 *  the grid fills and FREE SPINS gets bigger as the multiplier climbs.
 * ================================================================= */
enum { TR_LOUNGE, TR_FREE, TR_PICK, TR_HOLD, TR_WHEEL, NTRACK };
#define MLANES 8
typedef struct { uint8_t inst; float vol, pan, minI; uint8_t cresc; const char*pat; } mlane_t;
typedef struct { float bpm, bpm_hi, swing, level; mlane_t ln[MLANES]; } mtrack_t;

static const mtrack_t TRK[NTRACK] = {
  /* LOUNGE - the base game.  A ii-V lounge in F with a swung ride, a
     walking upright and a tine piano comping; vibes noodle over the
     top.  Quiet enough to play for hours.                              */
  { 88, 88, 0.45f, 0.45f, {
    { W7I_EP, 0.9f, 0.0f, 0, 0,
      "A3+C4+E4 -*2 .*3 A3+C4+E4 -*3 .*6 "
      "A3+C4+F4 -*2 .*3 A3+C4+F4 -*3 .*6 "
      "Bb3+D4+F4 -*2 .*3 Bb3+D4+F4 -*3 .*6 "
      "Bb3+E4+G4 -*2 .*3 Bb3+E4+G4 -*3 .*6 "
      "G3+C4+E4 -*2 .*3 G3+C4+E4 -*3 .*6 "
      "F#3+C4+E4 -*2 .*3 F#3+C4+E4 -*3 .*6 "
      "F3+Bb3+D4 -*2 .*5 E3+Bb3+D4 -*2 .*5 "
      "A3+C4+E4 -*7 .*8" },
    { W7I_UPRIGHT, 1.0f, 0.0f, 0, 0,
      "F2 -*2 . A2 -*2 . C3 -*2 . A2 -*2 . "
      "D2 -*2 . F2 -*2 . A2 -*2 . C3 -*2 . "
      "G2 -*2 . A2 -*2 . Bb2 -*2 . B2 -*2 . "
      "C3 -*2 . Bb2 -*2 . G2 -*2 . E2 -*2 . "
      "A2 -*2 . C3 -*2 . E3 -*2 . C3 -*2 . "
      "D3 -*2 . C3 -*2 . A2 -*2 . F#2 -*2 . "
      "G2 -*2 . Bb2 -*2 . C3 -*2 . E2 -*2 . "
      "F2 -*2 . C3 -*2 . F2 -*2 . E2 -*2 ." },
    { W7I_DRUM, 0.8f, 0.35f, 0, 0, "y . . . yb . y . y . . . yb . y ." },
    { W7I_VIBES, 0.8f, -0.30f, 0, 0,
      ".*8 C5 -*3 A4 -*3 "
      "D5 -*5 .*10 "
      ".*8 F5 -*3 D5 -*3 "
      "E5 -*7 .*4 G4 - A4 - "
      "C5 -*5 .*2 E5 -*3 D5 - C5 - "
      "A4 -*7 .*4 F#4 - A4 - "
      "Bb4 -*3 A4 -*3 G4 -*3 E4 -*3 "
      "F4 -*11 .*4" },
  } },
  /* FREE SPINS - driving, four on the floor, Am F C G with a pumping
     octave bass, a sixteenth-note pluck arpeggio and a bell hook in the
     second half.  Shaker at x2, brass stabs from x3.                    */
  { 124, 128, 0.0f, 0.80f, {
    { W7I_DRUM, 1.0f, 0.0f, 0, 0, "k . h . kc . h . k . h . kc . h h" },
    { W7I_DRUM, 0.8f, 0.2f, 0, 0, "C .*127" },
    { W7I_DRUM, 0.8f, -0.3f, 0.2f, 0, "a*16" },
    { W7I_SAWBASS, 1.0f, 0.0f, 0, 0,
      "A1 - A2 . A1 - A2 . A1 - A2 . A1 - A2 . "
      "F1 - F2 . F1 - F2 . F1 - F2 . F1 - F2 . "
      "C2 - C3 . C2 - C3 . C2 - C3 . C2 - C3 . "
      "G1 - G2 . G1 - G2 . G1 - G2 . G1 - G2 . "
      "A1 - A2 . A1 - A2 . A1 - A2 . A1 - A2 . "
      "F1 - F2 . F1 - F2 . F1 - F2 . F1 - F2 . "
      "C2 - C3 . C2 - C3 . C2 - C3 . C2 - C3 . "
      "E1 - E2 . E1 - E2 . E1 - E2 . E1 - E2 ." },
    { W7I_PLUCK, 0.75f, 0.30f, 0, 0,
      "A4 C5 E5 C5 A5 E5 C5 E5 A4 C5 E5 C5 A5 E5 C5 E5 "
      "F4 A4 C5 A4 F5 C5 A4 C5 F4 A4 C5 A4 F5 C5 A4 C5 "
      "C5 E5 G5 E5 C6 G5 E5 G5 C5 E5 G5 E5 C6 G5 E5 G5 "
      "G4 B4 D5 B4 G5 D5 B4 D5 G4 B4 D5 B4 G5 D5 B4 D5 "
      "A4 C5 E5 C5 A5 E5 C5 E5 A4 C5 E5 C5 A5 E5 C5 E5 "
      "F4 A4 C5 A4 F5 C5 A4 C5 F4 A4 C5 A4 F5 C5 A4 C5 "
      "C5 E5 G5 E5 C6 G5 E5 G5 C5 E5 G5 E5 C6 G5 E5 G5 "
      "E4 G#4 B4 G#4 E5 B4 G#4 B4 E4 G#4 B4 G#4 E5 B4 G#4 B4" },
    { W7I_PAD, 1.0f, 0.0f, 0, 0,
      "A3+C4+E4 -*15 A3+C4+F4 -*15 G3+C4+E4 -*15 G3+B3+D4 -*15 "
      "A3+C4+E4 -*15 A3+C4+F4 -*15 G3+C4+E4 -*15 G#3+B3+E4 -*15" },
    { W7I_BELL, 0.9f, -0.20f, 0, 0,
      ".*64 "
      "E5 . . E5 . . G5 . A5 -*3 .*4 "
      "F5 . . F5 . . E5 . C5 -*3 .*4 "
      "E5 . . E5 . . G5 . C6 -*3 B5 - A5 - "
      "G#5 -*3 .*4 B5 -*3 E5 -*3" },
    { W7I_BRASS, 0.55f, 0.0f, 0.45f, 0,
      "A3+C4+E4 - .*8 A3+C4+E4 - .*4 A3+C4+F4 - .*8 A3+C4+F4 - .*4 "
      "G3+C4+E4 - .*8 G3+C4+E4 - .*4 G3+B3+D4 - .*8 G3+B3+D4 - .*4 "
      "A3+C4+E4 - .*8 A3+C4+E4 - .*4 A3+C4+F4 - .*8 A3+C4+F4 - .*4 "
      "G3+C4+E4 - .*8 G3+C4+E4 - .*4 G#3+B3+E4 - .*8 G#3+B3+E4 - .*4" },
  } },
  /* LUCKY 7 PICK - playful oom-pa in F: pizzicato bass and chords, a
     marimba tune, a shaker.                                             */
  { 112, 112, 0.12f, 0.60f, {
    { W7I_UPRIGHT, 1.0f, 0.0f, 0, 0,
      "F2 - .*6 C2 - .*6 "
      "Bb1 - .*6 F2 - .*6 "
      "C2 - .*6 G2 - .*6 "
      "F2 - .*6 C2 - . . E2 - . ." },
    { W7I_PLUCK, 0.8f, 0.25f, 0, 0,
      ".*4 A3+C4+F4 .*7 A3+C4+F4 .*3 "
      ".*4 Bb3+D4+F4 .*7 Bb3+D4+F4 .*3 "
      ".*4 Bb3+E4+G4 .*7 Bb3+E4+G4 .*3 "
      ".*4 A3+C4+F4 .*7 A3+C4+F4 .*3" },
    { W7I_MARIMBA, 1.0f, -0.20f, 0, 0,
      "F5 . A5 . C6 . A5 . G5 .*3 F5 .*3 "
      "D5 . F5 . Bb5 . F5 . D5 .*7 "
      "C5 . E5 . G5 . Bb5 . A5 . G5 . E5 . C5 . "
      "F5 .*3 C5 .*3 F4 .*7" },
    { W7I_DRUM, 0.9f, 0.1f, 0, 0, "ka . a a a . a a ka . a a a . a a" },
    { W7I_DRUM, 0.5f, 0.0f, 0, 0, "C .*63" },
  } },
  /* HOLD & SPIN - a heartbeat over a pulsing D pedal.  The tempo climbs
     with the grid, the heart doubles, an ostinato and ticking come in,
     then strings, then taiko.                                           */
  { 92, 128, 0.0f, 0.85f, {
    { W7I_DRUM, 1.0f, 0.0f, 0, 0, "l . d . .*4 l . d . .*4" },
    { W7I_DRUM, 1.0f, 0.0f, 0.55f, 0, ".*4 l . d . .*4 l . d ." },
    { W7I_SAWBASS, 0.9f, 0.0f, 0, 0,
      "D2 . D2 . D2 . D2 . D2 . D2 . D2 . D2 . "
      "D2 . D2 . D2 . D2 . D2 . D2 . D2 . D2 . "
      "Bb1 . Bb1 . Bb1 . Bb1 . Bb1 . Bb1 . Bb1 . Bb1 . "
      "A1 . A1 . A1 . A1 . A1 . A1 . A1 . A1 ." },
    { W7I_PLUCK, 0.6f, 0.25f, 0.30f, 0,
      "D4 A3 F4 A3 D4 A3 F4 A3 D4 A3 F4 A3 D4 A3 F4 A3 "
      "D4 A3 F4 A3 D4 A3 F4 A3 D4 A3 F4 A3 D4 A3 F4 A3 "
      "D4 Bb3 F4 Bb3 D4 Bb3 F4 Bb3 D4 Bb3 F4 Bb3 D4 Bb3 F4 Bb3 "
      "C#4 A3 E4 A3 C#4 A3 E4 A3 C#4 A3 E4 A3 C#4 A3 E4 A3" },
    { W7I_DRUM, 0.45f, -0.3f, 0.30f, 0, "h*16" },
    { W7I_STRING, 1.0f, 0.0f, 0.60f, 0,
      "D3+A3+F4 -*31 D3+Bb3+F4 -*15 C#3+A3+E4 -*15" },
    { W7I_DRUM, 0.9f, 0.0f, 0.80f, 0, "T .*7 T .*2 T .*4" },
  } },
  /* WHEEL - a snare roll swelling over two bars, timpani, a brass
     suspension that never quite resolves.                               */
  { 120, 120, 0.0f, 0.80f, {
    { W7I_DRUM, 1.0f, 0.1f, 0, 1, "r*32" },
    { W7I_TIMP, 1.0f, 0.0f, 0, 0, "D2 .*15 A1 .*7 A1 .*3 D2 .*3" },
    { W7I_PAD, 1.1f, 0.0f, 0, 0, "D3+A3+D4 -*15 E3+A3+C#4 -*15" },
    { W7I_DRUM, 0.6f, 0.0f, 0, 0, "C .*31" },
  } },
};

typedef struct { uint8_t n, len, acc, tie; uint8_t v[4]; } mev_t;
#define MEVMAX 4096
static mev_t mus_ev[MEVMAX];
static int   mus_nev, mus_off[NTRACK][MLANES], mus_len[NTRACK][MLANES];

/*  "C#4" -> MIDI 61; -1 if it is not a note.                             */
static int mus_note(const char*s){
  static const int semi[7]={9,11,0,2,4,5,7};      /* A B C D E F G */
  if(*s<'A'||*s>'G') return -1;
  int n=semi[*s-'A']; s++;
  if(*s=='#'){ n++; s++; } else if(*s=='b'){ n--; s++; }
  if(*s<'0'||*s>'9') return -1;
  n += 12*((*s-'0')+1);
  return n<0?0:(n>127?127:n);
}
static void mus_parse(void){
  mus_nev=0;
  for(int t=0;t<NTRACK;t++) for(int l=0;l<MLANES;l++){
    const mlane_t*Ln=&TRK[t].ln[l];
    mus_off[t][l]=mus_nev; mus_len[t][l]=0;
    if(!Ln->pat) continue;
    const char*p=Ln->pat;
    while(*p){
      while(*p==' ') p++;
      if(!*p) break;
      char tok[48]; int n=0;
      while(*p && *p!=' ' && n<47) tok[n++]=*p++;
      tok[n]=0;
      int rep=1; char*st=strchr(tok,'*');
      if(st){ rep=atoi(st+1); *st=0; if(rep<1) rep=1; }
      mev_t e; memset(&e,0,sizeof e);
      char*q=tok;
      if(*q=='>'){ e.acc=1; q++; }
      if(!strcmp(q,"-")) e.tie=1;
      else if(strcmp(q,".")){
        if(Ln->inst==W7I_DRUM){
          for(;*q && e.n<4;q++) e.v[e.n++]=(uint8_t)*q;
        } else {
          for(char*c=q; c && *c && e.n<4; ){
            int m=mus_note(c);
            if(m>=0) e.v[e.n++]=(uint8_t)m;
            c=strchr(c,'+'); if(c) c++;
          }
        }
      }
      for(int r=0;r<rep && mus_nev<MEVMAX;r++) mus_ev[mus_nev++]=e;
    }
    int off=mus_off[t][l], len=mus_nev-off;
    mus_len[t][l]=len;
    for(int i=0;i<len;i++){                      /* ties -> note lengths */
      mev_t*e=&mus_ev[off+i];
      if(!e->n) continue;
      int k=1; while(i+k<len && mus_ev[off+i+k].tie && k<255) k++;
      e->len=(uint8_t)k;
    }
  }
}

/*  Two players: the one playing and the one fading out.                */
typedef struct { int tr; long step; float next, gain, target, rate, level; int firing; } mplayer_t;
static mplayer_t mp[2] = { {-1,0,0,0,0,0,0,0}, {-1,0,0,0,0,0,0,0} };
static int   mp_cur=0;
static float mus_I=0, mus_hint=0, mus_hint_t=0, mus_scene=1.0f;
static float mus_g0[2], mus_g1[2];               /* bus gain, frame start/end */
static float duck_g=1.0f, duck_hold=0, duck_lvl=1.0f, aud_sfxpk=0, aud_sfxenv=0;

AUDAPI void music_duck(float level,float secs){
  level=clampf(level,0.0f,1.0f);
  if(duck_hold<=0.0f || level<duck_lvl) duck_lvl=level;
  if(secs>duck_hold) duck_hold=secs;
}
AUDAPI void music_intensity(float x){ mus_hint=clampf(x,0.0f,1.0f); mus_hint_t=0.25f; }

static void mus_select(int want){
  mplayer_t*P=&mp[mp_cur];
  if(want<0){
    if(P->tr>=0 && P->firing){ P->firing=0; P->target=0; P->rate=DT/1.0f; }
    return;
  }
  if(P->tr==want && P->firing) return;
  if(P->tr>=0){ P->firing=0; P->target=0; P->rate=DT/1.2f; }   /* fade out */
  int o=mp_cur^1; mplayer_t*Q=&mp[o];
  aud_release_bus(1+o);                          /* whatever that slot held */
  Q->tr=want; Q->step=0; Q->next=0; Q->gain=0; Q->target=1.0f;
  Q->rate=DT/0.12f; Q->firing=1; Q->level=TRK[want].level;
  mp_cur=o;
}

static void mus_fire(int p,long step,float off,float sl){
  const mplayer_t*P=&mp[p];
  const mtrack_t*T=&TRK[P->tr];
  float del=off/AUD_SR, ssec=sl/AUD_SR;
  for(int l=0;l<MLANES;l++){
    const mlane_t*Ln=&T->ln[l];
    int len=mus_len[P->tr][l];
    if(!Ln->pat || !len || mus_I+1e-4f<Ln->minI) continue;
    int idx=(int)(step%len);
    const mev_t*e=&mus_ev[mus_off[P->tr][l]+idx];
    if(!e->n) continue;
    float vel=Ln->vol*(e->acc?1.3f:1.0f);
    if(Ln->cresc) vel*=0.3f+0.7f*(float)idx/(float)len;
    if(Ln->inst==W7I_DRUM){
      for(int k=0;k<e->n;k++){
        char c=(char)e->v[k];
        aud_drum(1+p,del,c,vel,Ln->pan);
        if(c=='r') aud_drum(1+p,del+ssec*0.5f,'r',vel*0.85f,Ln->pan);
      }
    } else {
      for(int k=0;k<e->n;k++){
        float pan=Ln->pan+((float)k-(float)(e->n-1)*0.5f)*0.22f;
        aud_inst(1+p,del,Ln->inst,aud_mtof[e->v[k]],(float)e->len*ssec*0.92f,vel,pan);
      }
    }
  }
}

static void mus_tick(void){
  for(int p=0;p<2;p++){
    mplayer_t*P=&mp[p];
    float g0=P->gain;
    if(P->tr>=0 && P->firing){
      const mtrack_t*T=&TRK[P->tr];
      float bpm=T->bpm+(T->bpm_hi-T->bpm)*mus_I;
      float sl=AUD_SR*60.0f/(bpm*4.0f);
      while(P->next<(float)SPF){
        float off=P->next;
        if((P->step&3)==2) off+=T->swing*sl;       /* swung off-beats */
        mus_fire(p,P->step,off,sl);
        P->step++; P->next+=sl;
      }
      P->next-=(float)SPF;
    }
    if(P->gain<P->target){ P->gain+=P->rate; if(P->gain>P->target) P->gain=P->target; }
    else if(P->gain>P->target){ P->gain-=P->rate; if(P->gain<P->target) P->gain=P->target; }
    if(!P->firing && P->gain<=0.0f) P->tr=-1;
    mus_g0[p]=g0*P->level; mus_g1[p]=P->gain*P->level;
  }
  /* ducking: the side-chain off the effects bus, and explicit holds */
  aud_sfxenv = aud_sfxpk>aud_sfxenv ? aud_sfxpk : aud_sfxenv*0.9f;
  float tgt=clampf(1.0f-1.4f*(aud_sfxenv-0.25f),0.35f,1.0f);
  if(duck_hold>0.0f){ duck_hold-=DT; if(duck_lvl<tgt) tgt=duck_lvl; }
  float dg0=duck_g;
  duck_g += (tgt-duck_g)*(tgt<duck_g?0.5f:0.06f);
  float k0=AUD_MUSGAIN*dg0, k1=AUD_MUSGAIN*duck_g;
  static float scene0=1.0f;
  for(int p=0;p<2;p++){ mus_g0[p]*=k0*scene0; mus_g1[p]*=k1*mus_scene; }
  scene0=mus_scene;
}

/*  Which tune the game state wants, every frame.                       */
static void music_auto(void){
  int want=TR_LOUNGE; float I=0, scene=1.0f;
  switch(G.state){
  case ST_JACKPOT:  want=-1; break;          /* the fanfare is the music  */
  case ST_BONUSEND: want=-1; break;          /* the stinger plays alone   */
  case ST_FSINTRO:  want=G.inFree?TR_FREE:-1; break;
  case ST_BONUS:    want=TR_PICK; break;
  case ST_HOLD: {
    /* the coins locked on the board, not the ones that started it */
    int c=G.hold.locked;
    want=TR_HOLD; I=clampf((float)(c-5)/14.0f,0.0f,1.0f); } break;
  case ST_WHEEL:    want=TR_WHEEL; I=clampf(G.t/5.0f,0.0f,1.0f); break;
  case ST_GAMBLE:   want=TR_HOLD; I=0.35f; break;
  case ST_ATTRACT:  scene=0.55f; break;
  case ST_BROKE:    scene=0.50f; break;
  default: break;
  }
  if(want==TR_LOUNGE && G.inFree){ want=TR_FREE; I=clampf((float)(G.fsMult-1)/4.0f,0.0f,1.0f); }
  if(mus_hint_t>0.0f){ mus_hint_t-=DT; if(mus_hint>I) I=mus_hint; }
  if(!opt_music) want=-1;
  mus_select(want);
  mus_I += (I-mus_I)*0.05f;
  mus_scene += (scene-mus_scene)*0.03f;
}

/* === SOUND EFFECTS =============================================== */
#define HZ_C4 261.63f
#define HZ_E4 329.63f
#define HZ_G4 392.00f
#define HZ_C5 523.25f
#define HZ_E5 659.26f
#define HZ_G5 783.99f
#define HZ_C6 1046.50f
#define HZ_E6 1318.51f
#define HZ_G6 1567.98f
#define HZ_C7 2093.00f

static void fx_crash(float del,float vel){ aud_drum(0,del,'C',vel,0.0f); }
static void fx_boom(float del,float vel){                 /* sub drop     */
  avoice_t*V=sv(del,SND_SINE,90,32,1.0f,0.55f*vel);
  V->sweep=SW_EXP; V->glide=5.0f; aud_perc(V,0.002f,3.2f); V->send=0.1f;
}
static void fx_brass_chord(float del,const float*hz,int n,float len,float vel){
  for(int i=0;i<n;i++)
    aud_inst(0,del,W7I_BRASS,hz[i],len,vel,((float)i-(float)(n-1)*0.5f)*0.3f);
}
static void fx_bells(float del,float step,const float*hz,int n,float vel){
  for(int i=0;i<n;i++)
    aud_inst(0,del+step*(float)i,W7I_BELL,hz[i],0.1f,vel,-0.5f+(float)i/(float)(n>1?n-1:1));
}

AUDAPI void sfx_whoosh(float dur,int up){
  avoice_t*V=sv(0,SND_WHOOSH,up?300.0f:3200.0f,up?3200.0f:300.0f,dur,0.14f);
  V->q=1.0f;
}
AUDAPI void sfx_riser(float dur){
  avoice_t*V=sv(0,SND_WHOOSH,250,3500,dur,0.15f);
  V->q=2.2f; aud_adsr(V,dur*0.85f,0.0f,1.0f,0.06f);
  V=sv(0,SND_BRASS,196,392,dur,0.08f);
  V->sweep=SW_LOG; aud_adsr(V,dur*0.8f,0.0f,1.0f,0.06f); V->det=0.012f; V->fce=3.0f;
  music_duck(0.5f,dur);
}
AUDAPI void sfx_impact(float power){
  float p=clampf(power,0.1f,1.0f);
  fx_boom(0,p); fx_crash(0,0.8f*p);
  aud_drum(0,0,'T',p,0.0f);
}
AUDAPI void sfx_coin_shower(float dur,int n){
  for(int i=0;i<n;i++){
    float f=2300.0f+arnd()*1900.0f;
    avoice_t*V=sv(arnd()*dur,SND_BELL,f,f,0.18f,0.04f+0.03f*arnd());
    V->fmr=1.41f+0.3f*arnd(); V->fmi=0.35f; V->fmdk=14.0f;
    aud_pan(V,(arnd()-0.5f)*1.4f); V->send=0.2f;
  }
}

/* ---- spin and reels ---- */
/*  Which trigger fanfares (1 scatter, 2 crown) this spin has already
 *  played: a fourth scatter dings higher, it does not re-trigger.      */
static int aud_trig=0;

AUDAPI void sfx_spin_start(void){
  aud_trig=0;
  avoice_t*V;
  V=sv(0,SND_WHOOSH,300,2400,0.30f,0.15f); V->q=1.1f; aud_pan(V,-0.35f);
  V=sv(0.025f,SND_WHOOSH,420,3200,0.28f,0.10f); V->q=1.1f; aud_pan(V,0.35f);
  V=sv(0,SND_SINE,130,52,0.16f,0.30f);                    /* the lever   */
  V->sweep=SW_EXP; V->glide=30.0f; aud_perc(V,0.001f,14.0f); V->send=0.05f;
  sv(0,SND_NOISE,2500,2500,0.035f,0.07f);                  /* latch click */
  if(G.inFree){                                            /* sparkle     */
    static const float up[4]={1318.5f,1661.2f,1975.5f,2637.0f};
    for(int i=0;i<4;i++){
      V=sv(0.04f*(float)i,SND_BELL,up[i],up[i],0.35f,0.05f);
      V->fmr=2.0f; V->fmi=0.25f; aud_pan(V,-0.4f+0.27f*(float)i);
    }
  }
}

static void aud_land_scatter(float del,int n,float pan){
  static const float sc[5]={659.26f,830.61f,987.77f,1318.51f,1661.22f};
  float f=sc[clampi(n-1,0,4)];
  aud_inst(0,del,W7I_BELL,f,0.1f,0.8f,pan);
  aud_inst(0,del+0.02f,W7I_BELL,f*2.0f,0.05f,0.3f,-pan);
  if(n==2) aud_inst(0,del,W7I_VIBES,f*0.5f,0.3f,0.8f,0);
  if(n>=3 && !(aud_trig&1)){                  /* that is the trigger    */
    aud_trig|=1;
    static const float ch[4]={329.63f,415.30f,493.88f,659.26f};
    fx_brass_chord(del+0.08f,ch,4,0.5f,0.75f);
    fx_crash(del+0.08f,0.6f);
    fx_boom(del+0.08f,0.6f);
  }
}
static void aud_land_crown(float del,int n,float pan){
  static const float cr[5]={523.25f,659.26f,783.99f,1046.50f,1318.51f};
  float f=cr[clampi(n-1,0,4)];
  aud_inst(0,del,W7I_MARIMBA,f,0.1f,1.0f,pan);
  aud_inst(0,del+0.05f,W7I_PLUCK,f*2.0f,0.1f,0.6f,-pan);
  if(n>=3 && !(aud_trig&2)){
    aud_trig|=2;
    static const float ch[4]={349.23f,440.0f,523.25f,698.46f};
    fx_brass_chord(del+0.08f,ch,4,0.5f,0.75f);
    fx_crash(del+0.08f,0.6f);
  }
}

AUDAPI void sfx_reel_stop(int r){
  static long rs_frame=-1; static int rs_n=0;
  if(rs_frame==aud_fc) rs_n++; else { rs_frame=aud_fc; rs_n=0; }
  r=clampi(r,0,NREEL-1);
  /* a slam stops every reel in one frame: roll them, don't stack them */
  float del=0.03f*(float)rs_n, g=1.0f/(1.0f+0.3f*(float)rs_n);
  int attract = G.state==ST_ATTRACT;
  if(attract) g*=0.35f;
  float pan=((float)r-2.0f)*0.32f, jit=0.97f+0.06f*arnd();
  float f=(125.0f+9.0f*(float)r)*jit;
  avoice_t*V;
  V=sv(del,SND_SINE,f*1.9f,f*0.42f,0.20f,0.34f*g);          /* thunk      */
  V->sweep=SW_EXP; V->glide=32.0f; aud_perc(V,0.0008f,15.0f); V->send=0.06f; aud_pan(V,pan);
  V=sv(del,SND_WHOOSH,(1500.0f+140.0f*r)*jit,(1100.0f+100.0f*r)*jit,0.035f,0.20f*g);
  V->q=1.6f; aud_perc(V,0.0004f,70.0f); aud_pan(V,pan);     /* clack      */
  V=sv(del,SND_NOISE,1800,1800,0.07f,0.09f*g); aud_pan(V,pan);
  aud_release_tag(TAG_ANTIC+r);
  if(attract) return;

  /* what landed: the ding climbs with every scatter / crown so far */
  int base=(int)floorf(G.rpos[r]+0.5f), st=0, cr=0, jp=0, ul=0, co=0, wh=0;
  for(int row=0;row<NROW;row++){
    int s=stripAt(r,base-row);
    st+=s==SY_STAR; cr+=s==SY_CROWN; jp+=s==SY_JACKPOT;
    ul+=s==SY_ULT;  co+=s==SY_COIN;  wh+=s==SY_WHEEL;
  }
  int tst=0, tcr=0;
  for(int q=0;q<NREEL;q++){
    if(G.rstate[q]!=3) continue;
    int b=(int)floorf(G.rpos[q]+0.5f);
    for(int row=0;row<NROW;row++){
      int s=stripAt(q,b-row); tst+=s==SY_STAR; tcr+=s==SY_CROWN;
    }
  }
  if(st) aud_land_scatter(del,tst,pan);
  if(cr) aud_land_crown(del,tcr,pan);
  if(ul){
    V=sv(del,SND_BELL,196,196,1.8f,0.22f);                  /* deep gong  */
    V->fmr=1.4f; V->fmi=0.6f; V->fmdk=1.5f; aud_pan(V,pan); V->send=0.5f;
  }
  if(co){
    float cf=2100.0f*jit;
    V=sv(del,SND_BELL,cf,cf,0.30f,0.12f);
    V->fmr=1.41f; V->fmi=0.45f; V->fmdk=12.0f; aud_pan(V,pan);
  }
  if(wh) for(int i=0;i<3;i++){
    V=sv(del+0.045f*(float)i,SND_WHOOSH,2800,2800,0.015f,0.10f);
    V->q=2.0f; aud_perc(V,0.0003f,90.0f); aud_pan(V,pan);
  }
  if(jp>=2){
    static const float jr[5]={130.81f,146.83f,164.81f,174.61f,196.0f};
    aud_inst(0,del,W7I_BRASS,jr[r],0.12f,0.35f,pan);
  }
}

AUDAPI void sfx_anticipation(int r){
  float len=0.95f+(opt_turbo?0.34f:0.55f)+0.05f;
  float k = r>=4 ? 1.26f : 1.0f;
  avoice_t*V;
  aud_tag=TAG_ANTIC+clampi(r,0,NREEL-1);
  V=sv(0,SND_WHOOSH,260.0f*k,3000.0f*k,len,0.15f);
  V->q=2.2f; aud_adsr(V,len*0.85f,0.0f,1.0f,0.06f);
  V=sv(0,SND_BRASS,196.0f*k,392.0f*k,len,0.09f);
  V->sweep=SW_LOG; aud_adsr(V,len*0.8f,0.0f,1.0f,0.06f);
  V->det=0.012f; V->fcb=1.4f; V->fce=3.0f; aud_pan(V,-0.3f);
  V=sv(0,SND_BRASS,294.0f*k,587.0f*k,len,0.07f);
  V->sweep=SW_LOG; aud_adsr(V,len*0.8f,0.0f,1.0f,0.06f);
  V->det=0.012f; V->fcb=1.4f; V->fce=3.0f; aud_pan(V,0.3f);
  aud_drum(0,0.00f,'l',0.7f,0); aud_drum(0,0.14f,'d',0.7f,0);
  aud_drum(0,0.62f,'l',0.8f,0); aud_drum(0,0.76f,'d',0.8f,0);
  aud_tag=TAG_NONE;
  music_duck(0.30f,len);
}

/* ---- the bet ladder: its pitch climbs a pentatonic with the bet ---- */
static float aud_bet_hz(int idx){
  static const int pent[13]={0,2,4,7,9,12,14,16,19,21,24,26,28};
  return aud_mtof[72+pent[clampi(idx,0,12)]];
}
static void aud_click(float vol){
  avoice_t*V=sv(0,SND_WHOOSH,4200,4200,0.012f,vol);
  V->q=1.0f; aud_perc(V,0.0003f,90.0f);
}
AUDAPI void sfx_bet_up(int idx){
  aud_init();
  float f=aud_bet_hz(idx);
  aud_click(0.08f);
  aud_inst(0,0,W7I_MARIMBA,f*0.75f,0.05f,0.45f,0.2f);
  aud_inst(0,0.045f,W7I_MARIMBA,f,0.05f,0.55f,0.25f);
}
AUDAPI void sfx_bet_down(int idx){
  aud_init();
  float f=aud_bet_hz(idx);
  aud_click(0.08f);
  aud_inst(0,0,W7I_MARIMBA,f*1.335f,0.05f,0.45f,-0.2f);
  aud_inst(0,0.045f,W7I_MARIMBA,f,0.05f,0.50f,-0.25f);
}
AUDAPI void sfx_bet_limit(void){
  avoice_t*V=sv(0,SND_TRI,196,175,0.10f,0.10f);
  aud_perc(V,0.001f,18.0f);
  aud_click(0.05f);
}
AUDAPI void sfx_bet_trim(void){
  aud_inst(0,0,W7I_MARIMBA,784.0f,0.05f,0.5f,0);
  aud_inst(0,0.07f,W7I_MARIMBA,587.3f,0.05f,0.5f,0);
}
AUDAPI void sfx_ui_move(int dir){
  aud_inst(0,0,W7I_MARIMBA,dir>=0?1318.5f:1174.7f,0.05f,0.35f,dir>=0?0.15f:-0.15f);
  aud_click(0.05f);
}
AUDAPI void sfx_ui_page(void){
  avoice_t*V=sv(0,SND_WHOOSH,800,3000,0.14f,0.07f); V->q=0.9f;
  aud_inst(0,0.04f,W7I_MARIMBA,1568.0f,0.05f,0.3f,0);
}
AUDAPI void sfx_ui_open(void){
  avoice_t*V=sv(0,SND_WHOOSH,500,2500,0.18f,0.07f); V->q=0.9f;
  aud_inst(0,0,W7I_MARIMBA,HZ_C6,0.05f,0.4f,-0.1f);
  aud_inst(0,0.05f,W7I_MARIMBA,HZ_G6,0.05f,0.4f,0.1f);
}

/* ---- money ---- */
AUDAPI void sfx_add_credits(int amount){
  avoice_t*V;
  V=sv(0,SND_WHOOSH,2600,2600,0.06f,0.20f); V->q=0.9f; aud_perc(V,0.0005f,40.0f); /* cha  */
  sv(0,SND_NOISE,3000,3000,0.03f,0.10f);                                          /* drawer */
  V=sv(0,SND_SINE,180,90,0.08f,0.22f); V->sweep=SW_EXP; V->glide=20; aud_perc(V,0.001f,20);
  V=aud_inst(0,0.07f,W7I_BELL,2637.0f,0.1f,0.8f,-0.2f); V->fmr=2.0f;              /* ching */
  V=aud_inst(0,0.07f,W7I_BELL,3520.0f,0.1f,0.6f,0.2f);  V->fmr=2.0f;
  int n = amount>=5000?12:amount>=2500?9:amount>=1000?7:amount>=500?5:3;
  for(int i=0;i<n;i++){                              /* coins into the tray */
    float f=2300.0f+arnd()*1900.0f;
    V=sv(0.12f+arnd()*0.55f,SND_BELL,f,f,0.18f,0.04f+0.03f*arnd());
    V->fmr=1.41f+0.3f*arnd(); V->fmi=0.35f; V->fmdk=14.0f; aud_pan(V,(arnd()-0.5f)*1.4f);
  }
}

AUDAPI void sfx_big_win_tier(int t){
  static const float cmaj[4]={HZ_C4,HZ_E4,HZ_G4,HZ_C5};
  static const float bb[3]={233.08f,293.66f,349.23f};
  static const float dmaj[4]={293.66f,369.99f,440.0f,587.33f};
  static const float casc[6]={HZ_C6,1174.66f,HZ_E6,HZ_G6,1760.0f,HZ_C7};
  if(t<=0){                                              /* BIG          */
    fx_crash(0,0.7f);
    aud_inst(0,0,W7I_TIMP,130.81f,0.2f,0.8f,0);
    aud_inst(0,0.00f,W7I_BRASS,HZ_G4,0.07f,0.8f,-0.2f);
    aud_inst(0,0.09f,W7I_BRASS,HZ_C5,0.07f,0.8f,0.0f);
    aud_inst(0,0.18f,W7I_BRASS,HZ_E5,0.07f,0.8f,0.2f);
    aud_inst(0,0.27f,W7I_BRASS,HZ_G5,0.55f,1.0f,0.0f);
    fx_brass_chord(0.27f,cmaj,3,0.6f,0.7f);
    aud_inst(0,0.27f,W7I_TIMP,65.41f,0.3f,1.0f,0);
    fx_bells(0.30f,0.05f,casc+2,4,0.5f);
    music_duck(0.25f,1.4f);
    return;
  }
  for(int i=0;i<6;i++)                                   /* timpani roll */
    aud_inst(0,0.06f*(float)i,W7I_TIMP,98.0f,0.1f,0.3f+0.1f*(float)i,0);
  fx_brass_chord(0.40f,bb,3,0.28f,0.9f);
  fx_crash(0.72f,1.0f); fx_boom(0.72f,0.9f);
  fx_brass_chord(0.72f,cmaj,4,1.1f,1.0f);
  aud_inst(0,0.72f,W7I_BRASS,130.81f,1.1f,0.9f,0);
  aud_inst(0,0.72f,W7I_TIMP,65.41f,0.3f,1.0f,0);
  fx_bells(0.80f,0.06f,casc,6,0.5f);
  if(t>=2){                                              /* EPIC: lift   */
    fx_crash(1.90f,1.0f); fx_boom(1.90f,1.0f);
    fx_brass_chord(1.90f,dmaj,4,1.3f,1.0f);
    aud_inst(0,1.90f,W7I_TIMP,73.42f,0.3f,1.0f,0);
    static const float casc2[6]={1174.66f,1318.51f,1479.98f,1760.0f,1975.53f,2349.32f};
    fx_bells(2.00f,0.06f,casc2,6,0.5f);
  }
  music_duck(0.15f,t>=2?3.6f:2.3f);
}

AUDAPI void sfx_win(int total){
  float x = TOTBET>0 ? (float)total/(float)TOTBET : 1.0f;
  if(x>=10.0f){ sfx_big_win_tier(0); return; }
  if(x<1.0f){
    aud_inst(0,0,W7I_BELL,HZ_E6,0.05f,0.40f,-0.1f);
    aud_inst(0,0.08f,W7I_BELL,HZ_G6,0.05f,0.35f,0.1f);
  } else if(x<3.0f){
    static const float a[3]={HZ_C6,HZ_E6,HZ_G6};
    fx_bells(0,0.07f,a,3,0.5f);
    for(int i=0;i<3;i++) aud_inst(0,0.07f*(float)i,W7I_PLUCK,a[i]*0.5f,0.08f,0.5f,0);
  } else {
    static const float a[5]={HZ_C5,HZ_E5,HZ_G5,HZ_C6,HZ_E6};
    static const float ch[3]={HZ_C4,HZ_E4,HZ_G4};
    for(int i=0;i<5;i++){
      aud_inst(0,0.065f*(float)i,W7I_PLUCK,a[i],0.08f,0.6f,-0.4f+0.2f*(float)i);
      aud_inst(0,0.065f*(float)i,W7I_BELL,a[i]*2.0f,0.05f,0.25f,0.4f-0.2f*(float)i);
    }
    fx_brass_chord(0.33f,ch,3,0.45f,0.55f);
    sfx_coin_shower(0.5f,6);
  }
}
AUDAPI void sfx_win_tick(float progress){
  float p=clampf(progress,0.0f,1.0f);
  float f=(1350.0f+1500.0f*p)*(0.985f+0.03f*arnd());
  avoice_t*V=sv(0,SND_BELL,f,f,0.07f,0.05f);
  V->fmr=2.0f; V->fmi=0.25f; V->fmdk=30.0f; aud_perc(V,0.0008f,40.0f);
  aud_pan(V,(arnd()-0.5f)*0.7f); V->send=0.12f;
}
AUDAPI void sfx_rollup_done(void){
  aud_inst(0,0,W7I_BELL,HZ_C7,0.05f,0.5f,-0.15f);
  aud_inst(0,0.085f,W7I_BELL,2637.0f,0.05f,0.55f,0.15f);
}
AUDAPI void sfx_jackpot_tick(float k){
  k=clampf(k,0.0f,1.0f);
  float f=(1500.0f+1300.0f*k)*(0.985f+0.03f*arnd());
  avoice_t*V=sv(0,SND_BELL,f,f,0.06f,0.045f);
  V->fmr=2.0f; V->fmi=0.25f; V->fmdk=30.0f; aud_perc(V,0.0008f,40.0f);
  aud_pan(V,(arnd()-0.5f)*0.9f);
  f=2600.0f+arnd()*1600.0f;                                   /* a coin */
  V=sv(0.02f,SND_BELL,f,f,0.15f,0.03f);
  V->fmr=1.41f; V->fmi=0.35f; V->fmdk=14.0f; aud_pan(V,(arnd()-0.5f)*1.4f);
}

/* -- the fanfares -------------------------------------------------
 *  Each prize keeps the phrase it always had - the same notes at the
 *  same moments, so they can still be told apart with your back to the
 *  machine: MINOR three notes up, MAJOR an arpeggio into a chord, MEGA
 *  a triple-tongue call, the ULTIMATE a run and two big chords - but the
 *  notes are now brass over timpani, the chords land on cymbals and a
 *  sub drop, the sparkle is real bells, and it all sits in the hall.
 * ---------------------------------------------------------------- */
AUDAPI void sfx_jackpot(int tier){
  avoice_t*V;
  switch(tier){
  case JP_MINOR: {                            /* three notes up, ~1s     */
    static const float ch[3]={HZ_C5,HZ_E5,HZ_G5};
    aud_drum(0,0,'C',0.45f,0.3f);
    aud_inst(0,0.00f,W7I_BRASS,HZ_G5,0.12f,0.9f,-0.2f);
    aud_inst(0,0.10f,W7I_BRASS,HZ_C6,0.12f,0.9f,0.0f);
    aud_inst(0,0.20f,W7I_BRASS,HZ_E6,0.36f,1.0f,0.2f);
    aud_inst(0,0.20f,W7I_BELL,HZ_E6,0.1f,0.6f,0.3f);
    fx_brass_chord(0.20f,ch,3,0.36f,0.55f);
    aud_inst(0,0.20f,W7I_TIMP,130.81f,0.2f,0.7f,0);
    V=sv(0.44f,SND_BELL,2093,2637,0.40f,0.10f); V->sweep=SW_LOG; aud_pan(V,0.3f);
    fx_bells(0.50f,0.06f,(const float[]){3136.0f,4186.0f},2,0.3f);
    music_duck(0.25f,1.2f);
    break; }
  case JP_MAJOR: {                            /* arpeggio into a chord   */
    static const float n[4]={HZ_C5,HZ_E5,HZ_G5,HZ_C6};
    static const float ch[3]={HZ_C6,HZ_E6,HZ_G6};
    static const float pad[3]={HZ_C4,HZ_E4,HZ_G4};
    fx_crash(0,0.6f);
    for(int i=0;i<4;i++){
      aud_inst(0,0.10f*(float)i,W7I_BRASS,n[i],0.14f,0.9f,-0.3f+0.2f*(float)i);
      aud_inst(0,0.10f*(float)i,W7I_PLUCK,n[i]*2.0f,0.1f,0.4f,0.3f-0.2f*(float)i);
    }
    fx_brass_chord(0.42f,ch,3,0.70f,0.75f);
    fx_brass_chord(0.42f,pad,3,0.75f,0.6f);
    aud_inst(0,0.42f,W7I_BRASS,261.63f*0.5f,0.75f,0.9f,0);
    aud_inst(0,0.42f,W7I_TIMP,65.41f,0.3f,1.0f,0);
    fx_crash(0.42f,0.8f); fx_boom(0.42f,0.8f);
    V=sv(1.05f,SND_BELL,2093,3136,0.50f,0.10f); V->sweep=SW_LOG;
    fx_bells(1.10f,0.06f,(const float[]){2637.0f,3136.0f,4186.0f},3,0.35f);
    music_duck(0.2f,1.9f);
    break; }
  case JP_MEGA: {                             /* a triple-tongue call    */
    static const float ch[3]={HZ_E6,HZ_G6,HZ_C7};
    for(int i=0;i<8;i++)                      /* timpani roll into it    */
      aud_inst(0,0.06f*(float)i,W7I_TIMP,98.0f,0.1f,0.25f+0.06f*(float)i,0);
    for(int i=0;i<3;i++){
      aud_inst(0,0.16f*(float)i,W7I_BRASS,HZ_G5,0.10f,1.0f,-0.15f+0.15f*(float)i);
      aud_drum(0,0.16f*(float)i,'s',0.6f,0.2f);
    }
    aud_inst(0,0.48f,W7I_BRASS,HZ_C6,0.55f,1.1f,0);
    aud_inst(0,0.48f,W7I_BRASS,HZ_C5,0.55f,0.9f,0);
    aud_inst(0,0.48f,W7I_TIMP,130.81f,0.3f,0.9f,0);
    fx_crash(0.48f,0.8f);
    fx_brass_chord(1.05f,ch,3,0.90f,0.8f);
    aud_inst(0,1.05f,W7I_BRASS,329.63f,0.95f,0.9f,-0.2f);
    aud_inst(0,1.05f,W7I_BRASS,174.61f,1.20f,0.9f,0.2f);
    aud_inst(0,1.05f,W7I_TIMP,87.31f,0.3f,1.0f,0);
    fx_crash(1.05f,1.0f); fx_boom(1.05f,1.0f);
    for(int i=0;i<6;i++){
      float f=2093.0f+180.0f*(float)i;
      V=sv(2.00f+0.07f*(float)i,SND_BELL,f,3000.0f,0.40f,0.08f);
      V->sweep=SW_LOG; aud_pan(V,-0.6f+0.24f*(float)i);
    }
    music_duck(0.15f,2.8f);
    break; }
  default: {                                  /* the ULTIMATE, ~5s       */
    static const float run[8]={523.25f,587.33f,659.26f,783.99f,880.0f,1046.5f,1174.66f,1318.51f};
    static const float c1[3]={HZ_C6,HZ_E6,HZ_G6};
    static const float c2[3]={1174.66f,HZ_G6,HZ_C7};
    static const float p1[3]={HZ_C4,HZ_E4,HZ_G4};
    static const float p2[3]={293.66f,HZ_G4,493.88f};
    fx_crash(0,1.0f); fx_boom(0,0.8f);
    for(int i=0;i<8;i++){
      aud_inst(0,0.075f*(float)i,W7I_BRASS,run[i],0.12f,0.9f,-0.5f+0.14f*(float)i);
      if(i&1) aud_inst(0,0.075f*(float)i,W7I_PLUCK,run[i]*2.0f,0.08f,0.4f,0.3f);
    }
    fx_brass_chord(0.62f,c1,3,1.20f,0.85f);
    fx_brass_chord(0.62f,p1,3,1.20f,0.7f);
    aud_inst(0,0.62f,W7I_BRASS,130.81f,1.3f,1.0f,0);
    aud_inst(0,0.62f,W7I_TIMP,65.41f,0.3f,1.0f,0);
    fx_crash(0.62f,1.0f); fx_boom(0.62f,1.0f);
    V=sv(1.35f,SND_WHOOSH,4000,9000,0.60f,0.10f);       /* cymbal swell  */
    V->filt=FLT_HP; aud_adsr(V,0.55f,0.0f,1.0f,0.05f);
    fx_brass_chord(1.95f,c2,3,1.40f,0.85f);
    fx_brass_chord(1.95f,p2,3,1.40f,0.7f);
    aud_inst(0,1.95f,W7I_BRASS,146.83f,1.5f,1.0f,0);
    aud_inst(0,1.95f,W7I_TIMP,73.42f,0.3f,1.0f,0);
    fx_crash(1.95f,1.0f); fx_boom(1.95f,1.0f);
    for(int i=0;i<10;i++){                               /* bells out    */
      float f=2093.0f*(1.0f+0.06f*(float)i);
      V=sv(3.30f+0.09f*(float)i,SND_BELL,f,2600.0f,0.60f,0.075f);
      V->sweep=SW_LOG; aud_pan(V,-0.7f+0.15f*(float)i);
    }
    for(int i=0;i<3;i++)                                  /* and a glow   */
      aud_inst(0,3.30f,W7I_PAD,p1[i]*2.0f,1.4f,1.2f,-0.3f+0.3f*(float)i);
    music_duck(0.0f,5.5f);
    break; }
  }
}

/*  The meter climbing.  The root rises with the level, so the ear knows
 *  how high it went without reading the panel: the same four notes as
 *  ever (half, root, fifth, octave glide) on pluck and bells, with a
 *  brass chord under them and a quick whoosh into it.                   */
AUDAPI void sfx_mult(int lvl){
  static const float root[6]={0,1046,1046,1318,1568,2093};
  float r=root[lvl<1?1:(lvl>5?5:lvl)];
  avoice_t*V=sv(0,SND_WHOOSH,600,4000,0.22f,0.07f); V->q=1.0f;
  aud_inst(0,0.00f,W7I_PLUCK,r*0.5f,0.1f,0.8f,-0.2f);
  aud_inst(0,0.06f,W7I_BELL,r,0.2f,0.9f,0.0f);
  aud_inst(0,0.14f,W7I_BELL,r*1.5f,0.2f,0.55f,0.2f);
  V=sv(0.26f,SND_BELL,r*2.0f,r*2.5f,0.40f,0.07f); V->sweep=SW_LOG; aud_pan(V,0.3f);
  const float ch[3]={r*0.25f,r*0.3125f,r*0.375f};
  fx_brass_chord(0.06f,ch,3,0.35f,0.45f);
}

/* ---- features ---- */
AUDAPI void sfx_fs_intro(void){
  static const float ch[4]={HZ_C4,HZ_E4,HZ_G4,HZ_C5};
  avoice_t*V=sv(0,SND_WHOOSH,300,3500,0.42f,0.12f); V->q=1.2f;
  aud_inst(0,0.00f,W7I_BRASS,HZ_G4,0.08f,0.8f,-0.3f);
  aud_inst(0,0.10f,W7I_BRASS,HZ_C5,0.08f,0.8f,-0.1f);
  aud_inst(0,0.20f,W7I_BRASS,HZ_E5,0.08f,0.8f,0.1f);
  aud_inst(0,0.30f,W7I_BRASS,HZ_G5,0.08f,0.8f,0.3f);
  aud_inst(0,0.42f,W7I_BRASS,HZ_C6,0.9f,1.0f,0);
  fx_brass_chord(0.42f,ch,4,0.9f,0.8f);
  aud_inst(0,0.42f,W7I_TIMP,65.41f,0.3f,1.0f,0);
  fx_crash(0.42f,0.9f); fx_boom(0.42f,0.8f);
  V=sv(0.42f,SND_BELL,HZ_C5,HZ_C6,0.9f,0.10f); V->sweep=SW_LOG;   /* the old glide */
  music_duck(0.1f,1.6f);
}
AUDAPI void sfx_fs_end(void){
  static const float g7[4]={196.0f,246.94f,293.66f,349.23f};
  static const float c[4]={HZ_C4,HZ_E4,HZ_G4,HZ_C5};
  static const float b[4]={HZ_C6,HZ_E6,HZ_G6,HZ_C7};
  fx_brass_chord(0,g7,4,0.28f,0.8f);
  fx_brass_chord(0.36f,c,4,1.0f,0.9f);
  aud_inst(0,0.36f,W7I_TIMP,65.41f,0.3f,1.0f,0);
  fx_crash(0.36f,0.8f);
  fx_bells(0.40f,0.07f,b,4,0.45f);
  if(TOTBET>0 && G.fsWon>=20*TOTBET) sfx_coin_shower(1.2f,14);
  music_duck(0.1f,1.6f);
}
AUDAPI void sfx_bonus_start(void){
  static const float ch[3]={349.23f,440.0f,HZ_C5};
  static const float run[4]={698.46f,880.0f,1046.5f,1396.91f};
  avoice_t*V;
  V=sv(0,SND_BELL,440,1320,0.5f,0.12f); V->sweep=SW_LOG; aud_pan(V,-0.2f);  /* the old */
  V=sv(0,SND_BELL,660,1980,0.6f,0.07f); V->sweep=SW_LOG; aud_pan(V,0.2f);   /* sweeps  */
  V=sv(0,SND_WHOOSH,400,3000,0.45f,0.08f); V->q=1.0f;
  fx_brass_chord(0.45f,ch,3,0.4f,0.8f);
  fx_crash(0.45f,0.5f);
  for(int i=0;i<4;i++) aud_inst(0,0.45f+0.05f*(float)i,W7I_MARIMBA,run[i],0.05f,0.7f,-0.3f+0.2f*(float)i);
}
AUDAPI void sfx_pick_reveal(int val){
  static const float a[6]={HZ_C6,HZ_E6,HZ_G6,HZ_C7,2637.0f,3136.0f};
  int n=3;
  if(TOTBET>0) n += (val>=2*TOTBET) + 2*(val>=3*TOTBET);
  fx_bells(0,0.05f,a,n,0.5f);
  static const float ch[3]={HZ_C5,HZ_E5,HZ_G5};
  fx_brass_chord(0,ch,3,0.12f,0.5f);
  sfx_coin_shower(0.4f,4);
}
AUDAPI void sfx_pick_stop(int n){
  avoice_t*V=sv(0,SND_BRASS,300,120,0.35f,0.12f);      /* the old drop */
  V->sweep=SW_EXP; V->glide=8.0f; V->fce=2.0f;
  aud_drum(0,0,'t',0.8f,0);
  if(n<3) aud_inst(0,0.12f,W7I_PLUCK,233.08f,0.1f,0.6f,0);
}
AUDAPI void sfx_pick_end(void){
  if(G.pickTotal>0){
    static const float ch[3]={HZ_C4,349.23f,440.0f};
    static const float b[3]={1396.91f,1760.0f,HZ_C7};
    fx_brass_chord(0.1f,ch,3,0.6f,0.9f);
    aud_inst(0,0.1f,W7I_TIMP,87.31f,0.3f,0.9f,0);
    fx_crash(0.1f,0.6f);
    fx_bells(0.14f,0.07f,b,3,0.45f);
  } else {                                  /* womp womp womp wommmp  */
    static const float w[4]={349.23f,329.63f,311.13f,293.66f};
    for(int i=0;i<4;i++){
      avoice_t*V=aud_inst(0,0.25f+0.28f*(float)i,W7I_BRASS,w[i],i==3?0.7f:0.2f,0.7f,0);
      V->fce=2.5f;
    }
  }
  music_duck(0.2f,1.5f);
}
AUDAPI void sfx_broke(void){
  aud_inst(0,0.00f,W7I_EP,HZ_G4,0.15f,0.6f,0);
  aud_inst(0,0.18f,W7I_EP,HZ_E4,0.15f,0.6f,0);
  aud_inst(0,0.36f,W7I_EP,HZ_C4,0.40f,0.6f,0);
}

/* ---- for the feature modules ---- */
AUDAPI void sfx_coin_land(int tier,int col){
  tier=clampi(tier,0,3);
  float pan=((float)clampi(col,0,NREEL-1)-2.0f)*0.3f;
  float f=2100.0f*(1.0f+0.12f*(float)tier)*(0.97f+0.06f*arnd());
  avoice_t*V;
  V=sv(0,SND_BELL,f,f,0.30f,0.13f);
  V->fmr=1.41f; V->fmi=0.45f; V->fmdk=12.0f; aud_pan(V,pan);
  V=sv(0.012f,SND_BELL,f*1.5f,f*1.5f,0.22f,0.06f);
  V->fmr=1.41f; V->fmi=0.35f; V->fmdk=14.0f; aud_pan(V,-pan);
  V=sv(0,SND_SINE,160,70,0.12f,0.28f);
  V->sweep=SW_EXP; V->glide=25.0f; aud_perc(V,0.001f,16.0f); aud_pan(V,pan);
  if(tier>=1) aud_inst(0,0.03f,W7I_BELL,f*0.5f,0.1f,0.5f,pan);
  if(tier>=2){
    static const float ch[3]={HZ_C5,HZ_E5,HZ_G5};
    fx_brass_chord(0.05f,ch,3,0.3f,0.6f); fx_crash(0.05f,0.4f);
  }
  if(tier>=3) sfx_impact(0.8f);
}
AUDAPI void sfx_respin_reset(void){
  static const float a[3]={HZ_G5,HZ_C6,HZ_E6};
  avoice_t*V=sv(0,SND_WHOOSH,500,3000,0.25f,0.08f); V->q=1.0f;
  fx_bells(0.05f,0.05f,a,3,0.5f);
}
AUDAPI void sfx_countdown(int left){
  if(left<=1){
    aud_drum(0,0,'t',0.8f,0);
    aud_inst(0,0,W7I_BELL,1760.0f,0.05f,0.4f,0);
  } else {
    aud_inst(0,0,W7I_MARIMBA,1568.0f,0.05f,0.4f,0);
  }
}
AUDAPI void sfx_wheel_tick(float speed){
  float s=clampf(speed,0.0f,1.0f);
  avoice_t*V=sv(0,SND_WHOOSH,2600.0f+800.0f*s,2600.0f+800.0f*s,0.015f,0.08f+0.08f*s);
  V->q=2.0f; aud_perc(V,0.0003f,90.0f); aud_pan(V,0.1f);
  V=sv(0,SND_TRI,1700.0f+400.0f*s,1500.0f,0.02f,0.03f); aud_perc(V,0.0005f,60.0f);
}
AUDAPI void sfx_wheel_stop(int tier){
  static const float b[4]={HZ_C6,HZ_E6,HZ_G6,HZ_C7};
  tier=clampi(tier,0,3);
  sfx_impact(0.5f+0.15f*(float)tier);
  fx_bells(0.02f,0.04f,b,3+(tier>=3),0.55f);
}
AUDAPI void sfx_thunder(float power){
  float p=clampf(power,0.2f,1.0f);
  avoice_t*V;
  V=sv(0,SND_WHOOSH,5000,1500,0.18f,0.35f*p);                /* crack   */
  V->filt=FLT_HP; aud_perc(V,0.0005f,18.0f);
  V=sv(0.05f,SND_NOISE,900,120,2.2f,0.40f*p);                 /* rumble  */
  aud_adsr(V,0.05f,1.5f,0.0f,0.3f); aud_pan(V,-0.3f);
  V=sv(0.20f,SND_NOISE,700,100,2.0f,0.30f*p);
  aud_adsr(V,0.08f,1.4f,0.0f,0.3f); aud_pan(V,0.3f);
  V=sv(0,SND_SINE,70,28,1.2f,0.55f*p);                        /* boom    */
  V->sweep=SW_EXP; V->glide=3.0f; aud_perc(V,0.002f,3.0f);
  music_duck(0.4f,1.5f);
}
AUDAPI void sfx_strike(int col){
  float pan=((float)clampi(col,0,NREEL-1)-2.0f)*0.3f;
  avoice_t*V=sv(0,SND_SAW,2400,180,0.22f,0.10f);
  V->sweep=SW_LOG; aud_perc(V,0.001f,10.0f); aud_pan(V,pan);
  V=sv(0,SND_WHOOSH,6000,2000,0.10f,0.20f);
  V->filt=FLT_HP; aud_perc(V,0.0005f,25.0f); aud_pan(V,pan);
  fx_boom(0,0.4f);
}
AUDAPI void sfx_card_flip(void){
  avoice_t*V=sv(0,SND_WHOOSH,1800,5200,0.07f,0.12f); V->q=0.9f;
  aud_click(0.06f);
}
AUDAPI void sfx_gamble_win(void){
  static const float a[4]={HZ_C6,HZ_E6,HZ_G6,HZ_C7};
  static const float ch[3]={HZ_C5,HZ_E5,HZ_G5};
  fx_bells(0,0.05f,a,4,0.55f);
  fx_brass_chord(0.05f,ch,3,0.25f,0.7f);
}
AUDAPI void sfx_gamble_lose(void){
  static const float w[3]={329.63f,311.13f,293.66f};
  for(int i=0;i<3;i++) aud_inst(0,0.22f*(float)i,W7I_BRASS,w[i],i==2?0.5f:0.16f,0.6f,0);
  aud_drum(0,0,'t',0.7f,0);
}
AUDAPI void sfx_transition(void){
  static const float a[3]={HZ_G6,HZ_C7,2637.0f};
  sfx_whoosh(0.5f,1);
  fx_boom(0.45f,0.7f); fx_crash(0.45f,0.5f);
  fx_bells(0.47f,0.05f,a,3,0.35f);
}
AUDAPI void sfx_feature_start(int kind){
  if(kind==0){
    static const float dm[3]={293.66f,349.23f,440.0f};
    for(int i=0;i<5;i++) aud_inst(0,0.05f*(float)i,W7I_TIMP,73.42f,0.1f,0.3f+0.1f*(float)i,0);
    fx_brass_chord(0.28f,dm,3,0.5f,0.9f); fx_crash(0.28f,0.8f); fx_boom(0.28f,0.8f);
  } else if(kind==1){
    static const float cm[3]={HZ_C4,HZ_E4,HZ_G4};
    sfx_whoosh(0.3f,1);
    fx_brass_chord(0.28f,cm,3,0.5f,0.9f); fx_crash(0.28f,0.8f);
  } else sfx_impact(0.7f);
}

/* === OBSERVING THE GAME ===========================================
 *  A few moments are worth a sound but have no line of their own in the
 *  flow code: a win being shown, the roll-up finishing, a bonus ending,
 *  the reels ticking past.  Rather than thread calls through the flow,
 *  the audio watches the state once a frame and reacts to changes.
 * ================================================================= */
static int   aw_state=-1, aw_rolled=0;
static float aw_rpos[NREEL];
static void aud_watch(void){
  int s=G.state;
  if(s!=aw_state){
    if(s==ST_SHOWWIN && G.winTotal>0 && G.winTotal<TOTBET*40) sfx_win(G.winTotal);
    if(s==ST_BONUSEND){ if(G.banner==3) sfx_fs_end(); else if(G.banner==4) sfx_pick_end(); }
    if(s==ST_BROKE && aw_state>=0 && aw_state!=ST_ADDCR) sfx_broke();
    aw_rolled=0;
  }
  if(s==ST_SHOWWIN && !aw_rolled && G.winTotal>0 && G.winShown>=G.winTotal){
    aw_rolled=1; sfx_rollup_done();
  }
  /* a soft tick as each symbol passes the window: the reels have weight */
  for(int r=0;r<NREEL;r++){
    if(G.rstate[r]==1 && s!=ST_ATTRACT && (int)floorf(aw_rpos[r])!=(int)floorf(G.rpos[r])){
      float f=(1900.0f+120.0f*(float)r)*(0.96f+0.08f*arnd());
      avoice_t*V=sv(0,SND_WHOOSH,f,f,0.012f,0.018f);
      V->q=2.0f; aud_perc(V,0.0003f,120.0f); aud_pan(V,((float)r-2.0f)*0.32f); V->send=0.03f;
    }
    aw_rpos[r]=G.rpos[r];
  }
  aw_state=s;
}

/* === THE MIX ===================================================== */
#define RV_SPREAD 23
#define RV_PRE    530                             /* 12 ms pre-delay      */
static const int rv_cl[4]={1116,1277,1422,1557};
static const int rv_al[2]={556,441};
static float rv_cb[2][4][1557+RV_SPREAD], rv_cf[2][4];
static float rv_ab[2][2][556+RV_SPREAD];
static int   rv_ci[2][4], rv_ai[2][2];
static float rv_pre[1024]; static int rv_pi;
static float m_x1[2], m_y1[2], m_lim;

static inline float aud_soft(float x){
  float a=fabsf(x);
  if(a<=AUD_KNEE) return x;
  float z=(a-AUD_KNEE)*(1.0f/(AUD_CEIL-AUD_KNEE));
  float t = z<3.0f ? z*(27.0f+z*z)/(27.0f+9.0f*z*z) : 1.0f;
  float y=AUD_KNEE+(AUD_CEIL-AUD_KNEE)*t;
  return x<0.0f?-y:y;
}

static void aud_master(void){
  /* residue of stolen voices, decaying over ~3 ms */
  for(int b=0;b<NBUS;b++){
    float rl=aud_resL[b], rr=aud_resR[b];
    if(fabsf(rl)+fabsf(rr)<1e-6f){ aud_resL[b]=aud_resR[b]=0; continue; }
    for(int i=0;i<SPF;i++){ aud_bl[b][i]+=rl; aud_br[b][i]+=rr; rl*=0.993f; rr*=0.993f; }
    aud_resL[b]=rl; aud_resR[b]=rr;
  }
  float pk=0;
  const float fb=0.80f, dmp=0.30f;
  for(int i=0;i<SPF;i++){
    float u=(float)(i+1)*(1.0f/(float)SPF);
    float g1=mus_g0[0]+(mus_g1[0]-mus_g0[0])*u;
    float g2=mus_g0[1]+(mus_g1[1]-mus_g0[1])*u;
    float sL=aud_bl[0][i], sR=aud_br[0][i];
    float a=fabsf(sL), b=fabsf(sR);
    if(a>pk) pk=a;
    if(b>pk) pk=b;
    float L=sL+g1*aud_bl[1][i]+g2*aud_bl[2][i];
    float R=sR+g1*aud_br[1][i]+g2*aud_br[2][i];
    float S=aud_bs[0][i]+g1*aud_bs[1][i]+g2*aud_bs[2][i];
    /* reverb: pre-delay, four damped combs, two allpasses, per side */
    /* the 1e-18 keeps the tails out of denormal range: RetroArch does
       not set flush-to-zero, and a denormal costs ~100x on x86 */
    float in=rv_pre[(rv_pi-RV_PRE)&1023]*0.14f+1e-18f;
    rv_pre[rv_pi]=S; rv_pi=(rv_pi+1)&1023;
    float w[2];
    for(int ch=0;ch<2;ch++){
      float out=0;
      for(int c=0;c<4;c++){
        int n=rv_cl[c]+ch*RV_SPREAD, j=rv_ci[ch][c];
        float y=rv_cb[ch][c][j];
        rv_cf[ch][c]=y*(1.0f-dmp)+rv_cf[ch][c]*dmp;
        rv_cb[ch][c][j]=in+rv_cf[ch][c]*fb;
        rv_ci[ch][c] = ++j>=n ? 0 : j;
        out+=y;
      }
      for(int c=0;c<2;c++){
        int n=rv_al[c]+ch*RV_SPREAD, j=rv_ai[ch][c];
        float bo=rv_ab[ch][c][j];
        rv_ab[ch][c][j]=out+bo*0.5f;
        out=bo-out;
        rv_ai[ch][c] = ++j>=n ? 0 : j;
      }
      w[ch]=out;
    }
    L=(L+w[0])*AUD_MASTER; R=(R+w[1])*AUD_MASTER;
    /* DC blocker, ~3 Hz */
    float yl=L-m_x1[0]+0.9995f*m_y1[0]; m_x1[0]=L; m_y1[0]=fabsf(yl)>1e-12f?yl:0.0f;
    float yr=R-m_x1[1]+0.9995f*m_y1[1]; m_x1[1]=R; m_y1[1]=fabsf(yr)>1e-12f?yr:0.0f;
    /* peak limiter: ~0.1 ms attack, ~0.3 s release */
    float p=fabsf(yl)>fabsf(yr)?fabsf(yl):fabsf(yr);
    if(p>m_lim) m_lim+=(p-m_lim)*0.35f; else if(m_lim>1e-6f) m_lim*=0.99993f;
    if(m_lim>AUD_LIM){ float g=AUD_LIM/m_lim; yl*=g; yr*=g; }
    yl=aud_soft(yl); yr=aud_soft(yr);
    abuf[i*2]  =(int16_t)(yl*32767.0f);
    abuf[i*2+1]=(int16_t)(yr*32767.0f);
  }
  aud_sfxpk=pk;
}

#ifdef W7_AUDIO_PROF
static double aud_prof_sum=0, aud_prof_max=0; static long aud_prof_n=0;
#endif

AUDAPI void audio_frame(void){
#ifdef W7_AUDIO_PROF
  struct timespec ta,tb; clock_gettime(CLOCK_MONOTONIC,&ta);
#endif
  aud_init();
  if(!opt_sound){
    memset(abuf,0,sizeof abuf);
    if(aud_live){                               /* forget everything      */
      memset(aud_v,0,sizeof aud_v);
      memset(rv_cb,0,sizeof rv_cb); memset(rv_ab,0,sizeof rv_ab);
      memset(rv_pre,0,sizeof rv_pre); memset(rv_cf,0,sizeof rv_cf);
      for(int p=0;p<2;p++){ mp[p].tr=-1; mp[p].firing=0; mp[p].gain=0; }
      aud_live=0;
    }
    return;
  }
  aud_live=1;
  aud_watch();
  music_auto();
  mus_tick();
  memset(aud_bl,0,sizeof aud_bl); memset(aud_br,0,sizeof aud_br); memset(aud_bs,0,sizeof aud_bs);
  for(int v=0;v<NVTOT;v++) if(aud_v[v].on) aud_run_voice(v);
  aud_master();
  aud_fc++;
#ifdef W7_AUDIO_PROF
  clock_gettime(CLOCK_MONOTONIC,&tb);
  double ms=(tb.tv_sec-ta.tv_sec)*1e3+(tb.tv_nsec-ta.tv_nsec)/1e6;
  aud_prof_sum+=ms; aud_prof_n++; if(ms>aud_prof_max) aud_prof_max=ms;
#endif
}
