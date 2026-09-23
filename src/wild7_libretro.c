/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  WILD 7's  —  video slots
 *  libretro core.  Software renderer, no dependencies beyond libm.
 *
 *  5 reels x 5 rows, ADJACENT WAYS: a win reads left to right from reel
 *  1, one symbol per reel, each on the next reel in the same or a
 *  neighbouring row.  Three, four or five reels pay, and every distinct
 *  path is a way that pays again.  Two bonus
 *  rounds: FREE SPINS (scatter) and LUCKY 7 PICK (crown).  Four
 *  progressives won from the reels: JACKPOT symbols in a cluster of
 *  5 / 6 / 7+ take MINOR / MAJOR / MEGA, and five ULTIMATE symbols
 *  touching take the ULTIMATE, 100,000 times the bet.
 *
 *  Renders 1280x720 (16:9), 60fps, 44100Hz stereo.
 *
 *  Performance note: every symbol is rasterised ONCE at init into an
 *  RGBA sprite, supersampled 4x and box-filtered down, so runtime cost
 *  is alpha-blended blits and a background memcpy.  Nothing expensive
 *  happens per frame.
 * ===================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "libretro.h"

#define FBW 1280
#define FBH 720
#define TAU 6.28318530718f
#define DT  (1.0f/60.0f)
#define SRATE 44100
#define SPF   735

static uint32_t fb[FBW*FBH];       /* what we hand to the frontend      */
static uint32_t bg[FBW*FBH];       /* static cabinet art, memcpy'd in   */
static int16_t  abuf[SPF*2];

/*  Thread-local storage for the band renderer (src/w7_thread.c).  Old
 *  Android API levels only have emulated TLS, a function call on every
 *  access, and the clip is read per pixel: there the core is built
 *  single-threaded instead.  -DW7_NOTHREADS forces that anywhere.     */
#if defined(__ANDROID__) && defined(__ANDROID_API__) && !defined(W7_NOTHREADS)
#  if __ANDROID_API__ < 29
#    define W7_NOTHREADS 1
#  endif
#endif
#ifdef W7_NOTHREADS
#  define W7_TLS
#else
#  define W7_TLS __thread
#endif

/*  The rows the current render thread may write, [clip_y0, clip_y1).
 *  Single-threaded this is the whole frame; the band renderer runs the
 *  whole draw list once per band with a narrower clip on each thread.
 *  Every primitive enforces it.  Draw code that loops over rows itself
 *  should narrow the loop with clip_rows() (so the work divides between
 *  the threads) and anything that writes fb[] directly MUST.          */
static W7_TLS int clip_y0 = 0, clip_y1 = FBH;

/*  [y0,y1) narrowed to this band's rows; 0 when nothing is left.       */
static inline int clip_rows(int*y0,int*y1){
  if(*y0<clip_y0) *y0=clip_y0;
  if(*y1>clip_y1) *y1=clip_y1;
  return *y0<*y1;
}
/*  Does [y0,y1) touch this band at all?  For skipping a whole object. */
static inline int rows_visible(int y0,int y1){ return y0<clip_y1 && y1>clip_y0; }
/*  Is row y this band's?  (clip_y0 >= 0 and clip_y1 <= FBH always.)   */
static inline int in_band(int y){ return (unsigned)(y-clip_y0) < (unsigned)(clip_y1-clip_y0); }

#include "w7_thread.c"               /* the band renderer's worker pool */

static retro_video_refresh_t      video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;
static retro_environment_t        environ_cb;
static retro_log_printf_t         log_cb;

/* ── options ────────────────────────────────────────────────────── */
static int opt_sound    = 1;
static int opt_limiter  = 1;   /* damp flashing for photosensitivity   */
static int opt_turbo    = 0;   /* faster reel spins                    */
static int opt_threads  = 0;   /* band renderer threads, 0 = auto       */

/* ── test hooks ─────────────────────────────────
 *  Off unless the matching environment variable is set, so they can
 *  never touch normal play.  They drive the real input/update/render
 *  path rather than short-circuiting it — WILD7_FORCE picks genuine
 *  reel stops that happen to show the trigger symbol, it does not
 *  fake the grid afterwards.
 *    WILD7_AUTOPILOT=1  spin loop   =2 pay table   =5 features page
 *                    =3  one spin   =4 add credits =6 climb the bet
 *                    =3  a single spin, then no further input
 *    WILD7_FORCE=free|pick|hold        land a bonus trigger
 *    WILD7_FORCE=mega|minor           force the jackpot roll to hit
 *    WILD7_FORCE=wheel                WHEEL on reels 2, 3 and 4
 *    WILD7_FORCE=storm                arm a 7 STRIKE on every base spin
 *    WILD7_THREADS=1..4   override the wild7_threads option
 *    WILD7_BANDS=N        cut the frame into N bands (tuning)
 *    WILD7_PROFILE=1      every 300 frames, print mean / max update and
 *                         render ms to stderr
 * ──────────────────────────────────────────── */
static int dbg_pilot = 0, dbg_force = 0;
static int dbg_threads = 0, dbg_bands = 0, dbg_profile = 0;
static long dbg_frame = 0;

/* ── rng ────────────────────────────────────────────────────────── */
static uint32_t rngs = 0x7777u;
static inline float frnd(void){
  rngs ^= rngs<<13; rngs ^= rngs>>17; rngs ^= rngs<<5;
  return (float)(rngs & 0xFFFFFF) / 16777216.0f;
}
static inline int   irnd(int n){ return n>0 ? (int)(frnd()*n) % n : 0; }
static inline float clampf(float v,float a,float b){ return v<a?a:(v>b?b:v); }
static inline int   clampi(int v,int a,int b){ return v<a?a:(v>b?b:v); }
static inline float lerpf(float a,float b,float t){ return a+(b-a)*t; }

/* ── colour helpers ─────────────────────────────────────────────── */
#define RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
static inline uint32_t mixc(uint32_t a,uint32_t b,float t){
  int ar=(a>>16)&255, ag=(a>>8)&255, ab=a&255;
  int br=(b>>16)&255, bg2=(b>>8)&255, bb=b&255;
  return RGB((int)(ar+(br-ar)*t), (int)(ag+(bg2-ag)*t), (int)(ab+(bb-ab)*t));
}
static inline uint32_t scalec(uint32_t c,float s){
  int r=(int)(((c>>16)&255)*s), g=(int)(((c>>8)&255)*s), b=(int)((c&255)*s);
  return RGB(clampi(r,0,255),clampi(g,0,255),clampi(b,0,255));
}
/* ── 5x7 vector-ish font ────────────────────────────────────────── */
static const uint8_t FONT[96][7] = {
/*32 sp*/{0,0,0,0,0,0,0},
/*33 ! */{0x04,0x04,0x04,0x04,0x04,0x00,0x04},
/*34 " */{0x0A,0x0A,0,0,0,0,0},
/*35 # */{0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00},
/*36 $ */{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
/*37 % */{0x11,0x02,0x02,0x04,0x08,0x08,0x11},
/*38 & */{0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
/*39 ' */{0x04,0x04,0,0,0,0,0},
/*40 ( */{0x02,0x04,0x08,0x08,0x08,0x04,0x02},
/*41 ) */{0x08,0x04,0x02,0x02,0x02,0x04,0x08},
/*42 * */{0x00,0x15,0x0E,0x1F,0x0E,0x15,0x00},
/*43 + */{0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
/*44 , */{0,0,0,0,0,0x04,0x08},
/*45 - */{0,0,0,0x1F,0,0,0},
/*46 . */{0,0,0,0,0,0x0C,0x0C},
/*47 / */{0x01,0x01,0x02,0x04,0x08,0x10,0x10},
/*48 0 */{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
/*49 1 */{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
/*50 2 */{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
/*51 3 */{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
/*52 4 */{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
/*53 5 */{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
/*54 6 */{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
/*55 7 */{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
/*56 8 */{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
/*57 9 */{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
/*58 : */{0,0x0C,0x0C,0,0x0C,0x0C,0},
/*59 ; */{0,0x0C,0x0C,0,0x0C,0x04,0x08},
/*60 < */{0x02,0x04,0x08,0x10,0x08,0x04,0x02},
/*61 = */{0,0,0x1F,0,0x1F,0,0},
/*62 > */{0x08,0x04,0x02,0x01,0x02,0x04,0x08},
/*63 ? */{0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
/*64 @ */{0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
/*65 A */{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
/*66 B */{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
/*67 C */{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
/*68 D */{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
/*69 E */{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
/*70 F */{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
/*71 G */{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
/*72 H */{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
/*73 I */{0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
/*74 J */{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
/*75 K */{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
/*76 L */{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
/*77 M */{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
/*78 N */{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
/*79 O */{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
/*80 P */{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
/*81 Q */{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
/*82 R */{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
/*83 S */{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
/*84 T */{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
/*85 U */{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
/*86 V */{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
/*87 W */{0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
/*88 X */{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
/*89 Y */{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
/*90 Z */{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
/*91 [ */{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
/*92 \ */{0x10,0x10,0x08,0x04,0x02,0x01,0x01},
/*93 ] */{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
/*94 ^ */{0x04,0x0E,0x15,0x04,0x04,0x04,0x04},   /* up arrow   */
/*95 _ */{0,0,0,0,0,0,0x1F},
/*96 ` */{0x08,0x04,0,0,0,0,0},
/*97 a */{0x04,0x04,0x04,0x04,0x15,0x0E,0x04},   /* down arrow */
/*98 b */{0x04,0x02,0x1F,0x02,0x04,0x00,0x00},   /* right arrow*/
/*99 c */{0x04,0x08,0x1F,0x08,0x04,0x00,0x00},   /* left arrow */
/*100 d*/{0x00,0x11,0x0A,0x04,0x0A,0x11,0x00},   /* multiply   */
/*101 e*/{0x00,0x00,0x0C,0x0C,0x00,0x00,0x00},   /* mid dot    */
/*102*/{0,0,0,0,0,0,0},/*103*/{0,0,0,0,0,0,0},
/*104*/{0,0,0,0,0,0,0},/*105*/{0,0,0,0,0,0,0},
/*106*/{0,0,0,0,0,0,0},/*107*/{0,0,0,0,0,0,0},
/*108*/{0,0,0,0,0,0,0},/*109*/{0,0,0,0,0,0,0},
/*110*/{0,0,0,0,0,0,0},/*111*/{0,0,0,0,0,0,0},
/*112*/{0,0,0,0,0,0,0},/*113*/{0,0,0,0,0,0,0},
/*114*/{0,0,0,0,0,0,0},/*115*/{0,0,0,0,0,0,0},
/*116*/{0,0,0,0,0,0,0},/*117*/{0,0,0,0,0,0,0},
/*118*/{0,0,0,0,0,0,0},/*119*/{0,0,0,0,0,0,0},
/*120*/{0,0,0,0,0,0,0},/*121*/{0,0,0,0,0,0,0},
/*122*/{0,0,0,0,0,0,0},/*123*/{0,0,0,0,0,0,0},
/*124*/{0,0,0,0,0,0,0},/*125*/{0,0,0,0,0,0,0},
/*126*/{0,0,0,0,0,0,0},/*127*/{0,0,0,0,0,0,0}
};

/* ═══ SPRITE SYSTEM ════════════════════════════════════════════════
 *  Art is authored on a 4x supersampled scratch canvas and box-filtered
 *  down into an RGBA sprite.  That buys antialiased edges for free and
 *  costs nothing at runtime because it all happens once, at init.
 * ================================================================= */
#define SS 4                       /* supersample factor              */
#define SYMW 106                   /* symbol sprite size              */
#define SYMH 106
#define SYMU 92.0f                 /* art is authored in 0..92 units  */
#define CANW (SYMW*SS)
#define CANH (SYMH*SS)

/*  Per-row spans.  A symbol occupies little more than half of its square
 *  sprite, so walking the transparent margin is pure waste.  rx0/rx1 give
 *  the first and last pixel with any alpha on each row; the blits iterate
 *  that instead of 0..w.                                                */
/* ═══ LAYOUT ══════════════════════════════════════════════════════
 *  1280x720.  A 5x5 reel window in the middle; the progressive ladder,
 *  feature meters and a how-to-win crib in the left rail; credits, bet,
 *  win and the controls crib in the right rail; the button deck along
 *  the bottom.
 *
 *    0                                                          1280
 *    +--------------------- MARQUEE ---------------------------+  58
 *    | jackpots |          R E E L S  5 x 5          | meters  |
 *    | features |                                    | controls|
 *    | how to   |                                    | last win| 606
 *    +----- PAYS  BET-  BET+  MAX  ADD CREDITS          (SPIN) -+ 720
 * ================================================================= */
#define NREEL 5
#define NROW  5
#define NCELL (NREEL*NROW)
#define CW  128                       /* cell                          */
#define CH  108
#define GX  320                       /* grid origin                   */
#define GY  66
#define GW  (NREEL*CW)                /* 640                           */
#define GH  (NROW*CH)                 /* 540                           */
#define SOX ((CW-SYMW)/2)             /* symbol offset inside a cell    */
#define SOY ((CH-SYMH)/2)

#define MQH   58                      /* marquee height                */
#define RAILW 280
#define LRX   12                      /* left rail                     */
#define RRX   988                     /* right rail                    */
#define DECKY 624                     /* button deck                   */
#define BTNY  638
#define BTNH  56
#define SPINX 1192                    /* big green dome                */
#define SPINY 670
#define SPINR 36

/* left rail geometry */
#define JPY   (GY-14)                 /* progressive ladder panel      */
#define JPH   254
#define FEATY (JPY+JPH+10)            /* free spins / multiplier       */
#define FEATH 78
#define HOWY  (FEATY+FEATH+10)        /* how-to-win crib               */
/* right rail geometry */
#define METY  (GY-14)                 /* credits / bet / win           */
#define METH  74
#define CTLY  (METY+3*METH+12)        /* controls crib                 */
#define CTLH  170
#define LWY   (CTLY+CTLH+10)          /* last win panel                */

typedef struct {
  int w,h;
  uint8_t *px;
  int16_t *rx0,*rx1;
} spr_t;                                          /* RGBA, straight        */

static void spr_bounds(spr_t*s){
  if(!s->px||!s->h) return;
  free(s->rx0); free(s->rx1);
  s->rx0=(int16_t*)malloc(s->h*sizeof(int16_t));
  s->rx1=(int16_t*)malloc(s->h*sizeof(int16_t));
  if(!s->rx0||!s->rx1){ free(s->rx0); free(s->rx1); s->rx0=s->rx1=NULL; return; }
  for(int y=0;y<s->h;y++){
    int a=-1,b=-1;
    const uint8_t*row=s->px+(size_t)y*s->w*4;
    for(int x=0;x<s->w;x++) if(row[x*4+3]){ if(a<0) a=x; b=x; }
    s->rx0[y]=(int16_t)(a<0?0:a);
    s->rx1[y]=(int16_t)(b<0?-1:b);
  }
}

static uint8_t canvas[CANW*CANH*4];

static void cv_clear(void){ memset(canvas,0,sizeof canvas); }

static inline void cv_px(int x,int y,uint32_t col,int a){
  if(x<0||y<0||x>=CANW||y>=CANH||a<=0) return;
  uint8_t*p = canvas + (y*CANW+x)*4;
  int sa = a>255?255:a;
  int r=(col>>16)&255, g=(col>>8)&255, b=col&255;
  int da = p[3];
  /* the common cases, opaque paint or an empty canvas, need no divides */
  if(sa>=255 || da==0){ p[0]=(uint8_t)r; p[1]=(uint8_t)g; p[2]=(uint8_t)b; p[3]=(uint8_t)sa; return; }
  /* straight-alpha over */
  int oa = sa + da*(255-sa)/255;
  if(oa<=0){ p[0]=p[1]=p[2]=p[3]=0; return; }
  p[0] = (uint8_t)((r*sa + p[0]*da*(255-sa)/255)/oa);
  p[1] = (uint8_t)((g*sa + p[1]*da*(255-sa)/255)/oa);
  p[2] = (uint8_t)((b*sa + p[2]*da*(255-sa)/255)/oa);
  p[3] = (uint8_t)oa;
}

/* vertical-gradient scanline polygon fill (even-odd) */
typedef struct { float x,y; } pt_t;

static void cv_poly(const pt_t*p,int n,uint32_t ctop,uint32_t cbot,int alpha){
  if(n<3) return;
  float miny=p[0].y, maxy=p[0].y;
  for(int i=1;i<n;i++){ if(p[i].y<miny)miny=p[i].y; if(p[i].y>maxy)maxy=p[i].y; }
  int y0=(int)floorf(miny), y1=(int)ceilf(maxy);
  if(y0<0) y0=0;
  if(y1>CANH) y1=CANH;
  float span = (maxy-miny)>0.001f ? (maxy-miny) : 1.0f;
  float xs[64];
  for(int y=y0;y<y1;y++){
    float fy = y+0.5f;
    int cnt=0;
    for(int i=0,j=n-1;i<n;j=i++){
      float y1p=p[i].y, y2p=p[j].y;
      if((y1p<=fy && y2p>fy) || (y2p<=fy && y1p>fy)){
        float t=(fy-y1p)/(y2p-y1p);
        if(cnt<64) xs[cnt++] = p[i].x + t*(p[j].x-p[i].x);
      }
    }
    for(int a=0;a<cnt-1;a++) for(int b=a+1;b<cnt;b++)
      if(xs[b]<xs[a]){ float t=xs[a]; xs[a]=xs[b]; xs[b]=t; }
    uint32_t col = mixc(ctop,cbot,clampf((fy-miny)/span,0,1));
    for(int a=0;a+1<cnt;a+=2){
      int xa=(int)ceilf(xs[a]-0.5f), xb=(int)ceilf(xs[a+1]-0.5f);
      if(xa<0) xa=0;
      if(xb>CANW) xb=CANW;
      for(int x=xa;x<xb;x++) cv_px(x,y,col,alpha);
    }
  }
}

static void cv_circle(float cx,float cy,float r,uint32_t ctop,uint32_t cbot,int alpha){
  pt_t p[48];
  for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=cx+cosf(a)*r; p[i].y=cy+sinf(a)*r; }
  cv_poly(p,48,ctop,cbot,alpha);
}
static void cv_ellipse(float cx,float cy,float rx,float ry,uint32_t ctop,uint32_t cbot,int alpha){
  pt_t p[48];
  for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=cx+cosf(a)*rx; p[i].y=cy+sinf(a)*ry; }
  cv_poly(p,48,ctop,cbot,alpha);
}
static void cv_rrect(float x,float y,float w,float h,float rad,uint32_t ctop,uint32_t cbot,int alpha){
  pt_t p[52]; int n=0;
  const int Q=12;
  for(int i=0;i<=Q;i++){ float a=-TAU/4+TAU/4*i/Q; p[n].x=x+w-rad+cosf(a)*rad; p[n].y=y+rad+sinf(a)*rad; n++; }
  for(int i=0;i<=Q;i++){ float a=0+TAU/4*i/Q;      p[n].x=x+w-rad+cosf(a)*rad; p[n].y=y+h-rad+sinf(a)*rad; n++; }
  for(int i=0;i<=Q;i++){ float a=TAU/4+TAU/4*i/Q;  p[n].x=x+rad+cosf(a)*rad;   p[n].y=y+h-rad+sinf(a)*rad; n++; }
  for(int i=0;i<=Q;i++){ float a=TAU/2+TAU/4*i/Q;  p[n].x=x+rad+cosf(a)*rad;   p[n].y=y+rad+sinf(a)*rad; n++; }
  cv_poly(p,n,ctop,cbot,alpha);
}
/* stroke = fill a scaled-up copy underneath, then the real shape on top */
static __attribute__((unused)) void cv_poly_outline(const pt_t*p,int n,float grow,uint32_t col,int alpha){
  pt_t q[64]; if(n>64) return;
  float cx=0,cy=0;
  for(int i=0;i<n;i++){ cx+=p[i].x; cy+=p[i].y; }
  cx/=n; cy/=n;
  for(int i=0;i<n;i++){
    float dx=p[i].x-cx, dy=p[i].y-cy;
    float d=sqrtf(dx*dx+dy*dy); if(d<0.001f) d=1;
    q[i].x = p[i].x + dx/d*grow;
    q[i].y = p[i].y + dy/d*grow;
  }
  cv_poly(q,n,col,col,alpha);
}

/* box-filter the supersampled canvas down into a sprite */
static void cv_resolve(spr_t*s){
  s->w=SYMW; s->h=SYMH;
  s->px = (uint8_t*)calloc(SYMW*SYMH,4);
  if(!s->px){ s->w=s->h=0; return; }
  for(int y=0;y<SYMH;y++) for(int x=0;x<SYMW;x++){
    int r=0,g=0,b=0,a=0;
    for(int j=0;j<SS;j++) for(int i=0;i<SS;i++){
      uint8_t*p = canvas + (((y*SS+j)*CANW)+(x*SS+i))*4;
      int pa=p[3];
      r += p[0]*pa; g += p[1]*pa; b += p[2]*pa; a += pa;
    }
    uint8_t*o = s->px + (y*SYMW+x)*4;
    if(a>0){ o[0]=(uint8_t)(r/a); o[1]=(uint8_t)(g/a); o[2]=(uint8_t)(b/a); }
    o[3] = (uint8_t)(a/(SS*SS));
  }
}

/* ═══ FRAMEBUFFER OPS ══════════════════════════════════════════════
 *  Every primitive clips to the band's rows up front, so a band pays
 *  only for its own rows, and the row loops are plain spans the
 *  compiler can vectorise.  The blend is the same integer formula
 *  everywhere, which is what keeps every band bit-identical to the
 *  single-threaded frame.
 * ================================================================= */

/*  One pixel of the fb_blend rule: a<=0 leaves it, a>=255 stores c,
 *  otherwise each channel moves (c-d)*a/256 of the way, floored.      */
/*
 *  The blend is written  d + ((s-d)*a >> 8)  per channel throughout the
 *  renderer.  Since d*256 + (s-d)*a == d*(256-a) + s*a and that is never
 *  negative, it is exactly  (d*(256-a) + s*a) >> 8  - and in that form
 *  red and blue ride together in one 32-bit multiply (each product fits
 *  in its own 16 bits), so a pixel costs two multiplies instead of
 *  three, with no unpacking.  Checked exhaustively for every d, s and
 *  a in 0..256; the frames are bit-identical (tools/bandcheck.sh).    */
static inline uint32_t blend_rbg(uint32_t d,uint32_t srb,uint32_t sg,uint32_t ia){
  uint32_t rb=((d&0xFF00FFu)*ia + srb)>>8;
  uint32_t g =((d&0x00FF00u)*ia + sg )>>8;
  return (rb&0xFF00FFu)|(g&0x00FF00u);
}
static inline uint32_t blend_px(uint32_t d,uint32_t c,int a){
  return blend_rbg(d,(c&0xFF00FFu)*(uint32_t)a,(c&0x00FF00u)*(uint32_t)a,256u-(uint32_t)a);
}
static inline void px_put(uint32_t*p,uint32_t c,int a){
  if(a<=0) return;
  *p = a>=255 ? c : blend_px(*p,c,a);
}
/*  The blend formula over a run, for any a in 1..256 (screen_tint uses
 *  the formula even at 255; fb_blend stores instead - see span_put).  */
static void span_blend(uint32_t*d,int n,uint32_t c,int a){
  const uint32_t srb=(c&0xFF00FFu)*(uint32_t)a, sg=(c&0x00FF00u)*(uint32_t)a;
  const uint32_t ia=256u-(uint32_t)a;
  for(int i=0;i<n;i++) d[i]=blend_rbg(d[i],srb,sg,ia);
}
static void span_fill(uint32_t*d,int n,uint32_t c){ for(int i=0;i<n;i++) d[i]=c; }
/*  fb_blend over a run.                                               */
static inline void span_put(uint32_t*d,int n,uint32_t c,int a){
  if(a<=0||n<=0) return;
  if(a>=255) span_fill(d,n,c); else span_blend(d,n,c,a);
}
/*  fb_blend of one colour through an 8-bit coverage mask.  a==0 leaves
 *  the pixel as it was either way, so there is no branch to skip it. */
static void span_mask(uint32_t*d,const uint8_t*m,int n,uint32_t c){
  const uint32_t crb=c&0xFF00FFu, cg=c&0x00FF00u;
  for(int i=0;i<n;i++){
    uint32_t a=m[i];
    uint32_t o=blend_rbg(d[i],crb*a,cg*a,256u-a);
    d[i] = a>=255 ? c : o;
  }
}

static inline void fb_px(int x,int y,uint32_t c){
  if((unsigned)x<FBW && in_band(y)) fb[y*FBW+x]=c;
}
static inline void fb_blend(int x,int y,uint32_t c,int a){
  if((unsigned)x>=FBW || !in_band(y) || a<=0) return;
  if(a>=255){ fb[y*FBW+x]=c; return; }
  fb[y*FBW+x] = blend_px(fb[y*FBW+x],c,a);
}
static inline void fb_add(int x,int y,int r,int g,int b){
  if((unsigned)x>=FBW || !in_band(y)) return;
  uint32_t d=fb[y*FBW+x];
  int dr=((d>>16)&255)+r, dg=((d>>8)&255)+g, db=(d&255)+b;
  fb[y*FBW+x] = RGB(dr>255?255:dr, dg>255?255:dg, db>255?255:db);
}
static void fb_rect(int x,int y,int w,int h,uint32_t c,int a){
  int x0=x<0?0:x, x1=x+w>FBW?FBW:x+w, y0=y, y1=y+h;
  if(a<=0 || x0>=x1 || !clip_rows(&y0,&y1)) return;
  for(int j=y0;j<y1;j++) span_put(fb+(size_t)j*FBW+x0,x1-x0,c,a);
}
static void fb_frame(int x,int y,int w,int h,int t,uint32_t c,int a){
  if(!rows_visible(y,y+h)) return;
  /* every blend here is the same colour and alpha, so the order the
     strips go down in cannot change a pixel; only how often it is hit */
  int j0=0, j1=h;
  if(y+j0<clip_y0) j0=clip_y0-y;
  if(y+j1>clip_y1) j1=clip_y1-y;
  for(int k=0;k<t;k++){
    if(in_band(y+k))     for(int i=0;i<w;i++) fb_blend(x+i,y+k,c,a);
    if(in_band(y+h-1-k)) for(int i=0;i<w;i++) fb_blend(x+i,y+h-1-k,c,a);
    for(int j=j0;j<j1;j++){ fb_blend(x+k,y+j,c,a); fb_blend(x+w-1-k,y+j,c,a); }
  }
}
/* ── rounded rectangles ────────────────────────────────────────────
 *  Signed-distance rounded rects, so every panel, cell and chip in the
 *  cabinet gets a smooth radius instead of a hard 90 degree corner.
 * ---------------------------------------------------------------- */
static inline float rr_sdf(float px,float py,float cx,float cy,
                           float hw,float hh,float r){
  float qx=fabsf(px-cx)-(hw-r), qy=fabsf(py-cy)-(hh-r);
  float ax=qx>0?qx:0, ay=qy>0?qy:0;
  float m=(qx>qy?qx:qy);
  if(m>0) m=0;
  return sqrtf(ax*ax+ay*ay)+m-r;
}
/*  Both of these used to evaluate the distance field over every pixel of
 *  the bounding box.  For a filled rect the straight middle band is
 *  simply solid, and for a frame it is two thin strips, so only the
 *  rounded ends need the field at all.  These are the most-called
 *  primitives in the renderer, so the saving shows up everywhere.
 *  Rows outside the band and columns off screen are cut before the
 *  loops; the field is only ever evaluated where it can land.         */
static void fb_rrectg(int x,int y,int w,int h,float r,
                      uint32_t top,uint32_t bot,int alpha){
  if(w<=0||h<=0) return;
  int j0=0, j1=h;
  if(y+j0<clip_y0) j0=clip_y0-y;
  if(y+j1>clip_y1) j1=clip_y1-y;
  int i0=x<0?-x:0, i1=x+w>FBW?FBW-x:w;
  if(j0>=j1 || i0>=i1) return;
  float cx=x+w*0.5f, cy=y+h*0.5f, hw=w*0.5f, hh=h*0.5f;
  int endband=(int)(r+2.0f);
  if(endband*2 > h) endband = h/2;
  /* the solid run of a straight-sided row is [2,w-2); the AA columns
     are the rest: [0,2) and [max(2,w-2),w) */
  int s0=i0>2?i0:2, s1=i1<w-2?i1:w-2;
  int e0=i1<2?i1:2, e1=(w-2>2?w-2:2); if(e1<i0) e1=i0;
  for(int j=j0;j<j1;j++){
    uint32_t col=mixc(top,bot,(float)j/(h>1?h-1:1));
    uint32_t*row=fb+(size_t)(y+j)*FBW+x;
    if(j>=endband && j<h-endband){
      /* straight-sided row: solid, with the two edge pixels antialiased */
      if(s0<s1) span_put(row+s0,s1-s0,col,alpha);
      for(int i=i0;i<e0;i++){
        float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
        float c=clampf(0.5f-d,0,1);
        if(c>0.002f) px_put(row+i,col,(int)(c*alpha));
      }
      for(int i=e1;i<i1;i++){
        float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
        float c=clampf(0.5f-d,0,1);
        if(c>0.002f) px_put(row+i,col,(int)(c*alpha));
      }
    } else {
      for(int i=i0;i<i1;i++){
        float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
        float c=clampf(0.5f-d,0,1);
        if(c>0.002f) px_put(row+i,col,(int)(c*alpha));
      }
    }
  }
}
static void fb_rrect(int x,int y,int w,int h,float r,uint32_t c,int a){
  fb_rrectg(x,y,w,h,r,c,c,a);
}
static void fb_rframe(int x,int y,int w,int h,float r,float t,uint32_t col,int alpha){
  if(w<=0||h<=0) return;
  int j0=0, j1=h;
  if(y+j0<clip_y0) j0=clip_y0-y;
  if(y+j1>clip_y1) j1=clip_y1-y;
  int i0=x<0?-x:0, i1=x+w>FBW?FBW-x:w;
  if(j0>=j1 || i0>=i1) return;
  float cx=x+w*0.5f, cy=y+h*0.5f, hw=w*0.5f, hh=h*0.5f;
  int endband=(int)(r+t+2.0f);
  int side   =(int)(t+2.5f);
  if(endband*2 > h) endband = h/2;
  if(side*2 > w)    side = w/2;
  /* a straight-sided row only has the two side strips [0,side) and
     [w-side,w); the rows at the rounded ends are evaluated in full */
  int a1=i1<side?i1:side, b0=(w-side>i0?w-side:i0);
  if(b0<a1) b0=a1;
  for(int j=j0;j<j1;j++){
    uint32_t*row=fb+(size_t)(y+j)*FBW+x;
    int mid = (j>=endband && j<h-endband);
    int n0=i0, n1=mid?a1:i1;
    for(int pass=0;pass<2;pass++){
      for(int i=n0;i<n1;i++){
        float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
        float c=clampf(0.5f-fabsf(d+t*0.5f)+t*0.5f,0,1);
        if(c>0.002f) px_put(row+i,col,(int)(c*alpha));
      }
      if(!mid) break;
      n0=b0; n1=i1;
    }
  }
}

/* thick line, used for payline paths */
static void fb_line(float x0,float y0,float x1,float y1,int t,uint32_t c,int a){
  int h2=t/2;
  float lo=y0<y1?y0:y1, hi=y0>y1?y0:y1;
  if(!rows_visible((int)floorf(lo)-h2-2,(int)ceilf(hi)+h2+2)) return;
  float dx=x1-x0, dy=y1-y0;
  int n=(int)(sqrtf(dx*dx+dy*dy))+1;
  for(int i=0;i<=n;i++){
    float u=(float)i/n, px=x0+dx*u, py=y0+dy*u;
    int iy=(int)py, ja=-h2, jb=h2;
    if(iy+ja<clip_y0) ja=clip_y0-iy;
    if(iy+jb>clip_y1-1) jb=clip_y1-1-iy;
    for(int j=ja;j<=jb;j++) for(int k=-h2;k<=h2;k++)
      if(j*j+k*k <= h2*h2+1) fb_blend((int)px+k,iy+j,c,a);
  }
}

/* alpha blit with vertical clipping (reel window) and optional tint/fade.
   Walks only each row's occupied span, and takes a straight-store path for
   fully opaque pixels, which is most of a symbol's interior.            */
static void blit(const spr_t*s,int dx,int dy,int cy0,int cy1,int alpha,uint32_t tint,float tintAmt){
  if(!s->px) return;
  if(cy0<clip_y0) cy0=clip_y0;
  if(cy1>clip_y1) cy1=clip_y1;
  int y0=0, y1=s->h;
  if(dy+y0 < cy0) y0 = cy0-dy;
  if(dy+y1 > cy1) y1 = cy1-dy;
  if(y0<0) y0=0;
  if(y1>s->h) y1=s->h;
  int plain = (alpha==255 && tintAmt<=0.0f);
  for(int y=y0;y<y1;y++){
    int fy=dy+y;
    int xa = s->rx0? s->rx0[y] : 0;
    int xb = s->rx1? s->rx1[y] : s->w-1;
    if(xb<xa) continue;
    if(dx+xa < 0)      xa = -dx;
    if(dx+xb >= FBW)   xb = FBW-1-dx;
    if(xb<xa) continue;
    const uint8_t*row = s->px + (size_t)y*s->w*4;
    uint32_t*dst = fb + (size_t)fy*FBW + dx;
    if(plain){
      /* the common case, branch-free so it vectorises: alpha 0 blends
         to the pixel unchanged, alpha 255 stores the sprite colour */
      for(int x=xa;x<=xb;x++){
        const uint8_t*p=row+x*4;
        uint32_t a=p[3], srb=((uint32_t)p[0]<<16)|p[2], sg=(uint32_t)p[1]<<8;
        uint32_t o=blend_rbg(dst[x],srb*a,sg*a,256u-a);
        dst[x] = a==255 ? (srb|sg) : o;
      }
      continue;
    }
    for(int x=xa;x<=xb;x++){
      int a=row[x*4+3];
      if(!a) continue;
      uint32_t c = RGB(row[x*4],row[x*4+1],row[x*4+2]);
      if(tintAmt>0.0f) c = mixc(c,tint,tintAmt);
      int ea = a*alpha/255;
      if(ea>=255){ dst[x]=c; continue; }
      dst[x] = blend_px(dst[x],c,ea);
    }
  }
}

/*  Blend a single colour through a sprite's alpha.  Used for the cell
 *  washes, which used to evaluate a square root per pixel per symbol per
 *  frame — with expanding wilds on screen that was twelve of them.     */
static void blit_wash(const spr_t*s,int dx,int dy,int cy0,int cy1,
                      uint32_t col,float amt){
  if(!s->px||amt<=0.0f) return;
  if(cy0<clip_y0) cy0=clip_y0;
  if(cy1>clip_y1) cy1=clip_y1;
  int y0=0,y1=s->h;
  if(dy+y0<cy0) y0=cy0-dy;
  if(dy+y1>cy1) y1=cy1-dy;
  if(y0<0) y0=0;
  if(y1>s->h) y1=s->h;
  const int sr=(col>>16)&255, sg=(col>>8)&255, sb=col&255;
  int k=(int)(amt*256.0f);
  for(int y=y0;y<y1;y++){
    int fy=dy+y;
    int xa = s->rx0? s->rx0[y] : 0;
    int xb = s->rx1? s->rx1[y] : s->w-1;
    if(xb<xa) continue;
    if(dx+xa<0)    xa=-dx;
    if(dx+xb>=FBW) xb=FBW-1-dx;
    if(xb<xa) continue;
    const uint8_t*row=s->px+(size_t)y*s->w*4;
    uint32_t*dst=fb+(size_t)fy*FBW+dx;
    /* a==0 blends to the pixel unchanged, so no branch is needed */
    if(k<=256){
      const uint32_t crb=col&0xFF00FFu, cg=col&0x00FF00u;
      for(int x=xa;x<=xb;x++){
        uint32_t a=(uint32_t)(row[x*4+3]*k)>>8;           /* 0..255 */
        dst[x]=blend_rbg(dst[x],crb*a,cg*a,256u-a);
      }
    } else for(int x=xa;x<=xb;x++){       /* amt > 1 overdrives: the plain form */
      int a=(row[x*4+3]*k)>>8;
      uint32_t d=dst[x];
      int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
      dst[x]=RGB(dr+((sr-dr)*a>>8), dg+((sg-dg)*a>>8), db+((sb-db)*a>>8));
    }
  }
}


/* ── text (5x7 cell font, integer scale, with drop shadow) ──────── */
/*  One glyph, each set cell a px-by-px block, clipped to the band and
 *  the screen.  Shared by text() and the marquee's text_run().        */
static void glyph_blocks(const uint8_t*gl,int gx,int gy,int px,uint32_t c){
  for(int r=0;r<7;r++){
    uint8_t bits=gl[r];
    if(!bits) continue;
    int ya=gy+r*px, yb=ya+px;
    if(!clip_rows(&ya,&yb)) continue;
    for(int cbit=0;cbit<5;cbit++){
      if(!(bits & (0x10>>cbit))) continue;
      int xa=gx+cbit*px, xb=xa+px;
      if(xa<0) xa=0;
      if(xb>FBW) xb=FBW;
      if(xa>=xb) continue;
      for(int yy=ya;yy<yb;yy++) span_fill(fb+(size_t)yy*FBW+xa,xb-xa,c);
    }
  }
}
static inline const uint8_t* glyph_of(int ch){
  if(ch>='a'&&ch<='z') ch-=32;      /* font is upper case only */
  if(ch<32||ch>127) ch='?';
  return FONT[ch-32];
}
static void text(const char*s,int x,int y,int px,uint32_t col,int align,int shadow){
  int n=(int)strlen(s);
  while(px>1 && n*px*6-px > FBW-10) px--;      /* auto-fit */
  if(!rows_visible(y, y+px*7+(shadow?px:0))) return;
  int adv=px*6, wtot=n*adv-px;
  int ox = align==1 ? x-wtot/2 : (align==2 ? x-wtot : x);
  for(int pass=(shadow?0:1); pass<2; pass++){
    uint32_t c = pass==0 ? 0x000000 : col;
    int off = pass==0 ? px : 0;
    for(int i=0;i<n;i++)
      glyph_blocks(glyph_of((unsigned char)s[i]),ox+i*adv+off,y+off,px,c);
  }
}

/* ═══ SHADING PRIMITIVES ═══════════════════════════════════════════
 *  Two-stop gradients and a flat rim read as clip art.  These give the
 *  symbols an actual light direction: multi-stop ramps, spherical
 *  shading with a real lambert term, and specular hot spots.
 * ================================================================= */
static uint32_t ramp(const uint32_t*st,int n,float t){
  if(t<=0) return st[0];
  if(t>=1) return st[n-1];
  float f=t*(n-1); int i=(int)f;
  if(i>n-2) i=n-2;
  return mixc(st[i],st[i+1],f-i);
}

/* multi-stop vertical gradient polygon fill */
static __attribute__((unused)) void cv_polyN(const pt_t*p,int n,const uint32_t*st,int ns,int alpha){
  if(n<3) return;
  float miny=p[0].y, maxy=p[0].y;
  for(int i=1;i<n;i++){ if(p[i].y<miny)miny=p[i].y; if(p[i].y>maxy)maxy=p[i].y; }
  int y0=(int)floorf(miny), y1=(int)ceilf(maxy);
  if(y0<0) y0=0;
  if(y1>CANH) y1=CANH;
  float span=(maxy-miny)>0.001f?(maxy-miny):1.0f;
  float xs[64];
  for(int y=y0;y<y1;y++){
    float fy=y+0.5f; int cnt=0;
    for(int i=0,j=n-1;i<n;j=i++){
      float ya=p[i].y, yb=p[j].y;
      if((ya<=fy&&yb>fy)||(yb<=fy&&ya>fy)){
        float t=(fy-ya)/(yb-ya);
        if(cnt<64) xs[cnt++]=p[i].x+t*(p[j].x-p[i].x);
      }
    }
    for(int a=0;a<cnt-1;a++) for(int b=a+1;b<cnt;b++)
      if(xs[b]<xs[a]){ float t=xs[a]; xs[a]=xs[b]; xs[b]=t; }
    uint32_t col=ramp(st,ns,clampf((fy-miny)/span,0,1));
    for(int a=0;a+1<cnt;a+=2){
      int xa=(int)ceilf(xs[a]-0.5f), xb=(int)ceilf(xs[a+1]-0.5f);
      if(xa<0) xa=0;
      if(xb>CANW) xb=CANW;
      for(int x=xa;x<xb;x++) cv_px(x,y,col,alpha);
    }
  }
}

/* a lit sphere: lambert from the upper left, plus a specular hot spot */
static __attribute__((unused)) void cv_sphere(float cx,float cy,float r,uint32_t lo,uint32_t mid,uint32_t hi){
  int x0=(int)(cx-r-1), x1=(int)(cx+r+1);
  int y0=(int)(cy-r-1), y1=(int)(cy+r+1);
  x0=x0<0?0:x0; y0=y0<0?0:y0;
  x1=x1>CANW?CANW:x1; y1=y1>CANH?CANH:y1;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=(x+0.5f-cx)/r, dy=(y+0.5f-cy)/r;
    float d2=dx*dx+dy*dy;
    if(d2>1.0f) continue;
    float nz=sqrtf(1.0f-d2);
    /* light from up-left-front */
    float l = (-dx*0.50f - dy*0.62f + nz*0.60f);
    l = clampf(l,0.0f,1.0f);
    uint32_t st[3]={lo,mid,hi};
    uint32_t c = ramp(st,3,l);
    /* specular */
    float sx=dx+0.42f, sy=dy+0.46f;
    float sp=1.0f-clampf(sqrtf(sx*sx+sy*sy)*2.6f,0,1);
    if(sp>0) c = mixc(c,0xFFFFFF,sp*sp*0.85f);
    /* soften the silhouette so the downsample keeps a clean edge */
    int a = d2>0.90f ? (int)(255*(1.0f-(d2-0.90f)/0.10f)) : 255;
    cv_px(x,y,c,a);
  }
}

/* shaded ellipse, same lighting model */
static __attribute__((unused)) void cv_ellipse_lit(float cx,float cy,float rx,float ry,
                           uint32_t lo,uint32_t mid,uint32_t hi){
  int x0=(int)(cx-rx-1), x1=(int)(cx+rx+1);
  int y0=(int)(cy-ry-1), y1=(int)(cy+ry+1);
  x0=x0<0?0:x0; y0=y0<0?0:y0;
  x1=x1>CANW?CANW:x1; y1=y1>CANH?CANH:y1;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=(x+0.5f-cx)/rx, dy=(y+0.5f-cy)/ry;
    float d2=dx*dx+dy*dy;
    if(d2>1.0f) continue;
    float nz=sqrtf(1.0f-d2);
    float l=clampf(-dx*0.48f - dy*0.60f + nz*0.62f,0,1);
    uint32_t st[3]={lo,mid,hi};
    uint32_t c=ramp(st,3,l);
    float sx=dx+0.40f, sy=dy+0.48f;
    float sp=1.0f-clampf(sqrtf(sx*sx+sy*sy)*2.4f,0,1);
    if(sp>0) c=mixc(c,0xFFFFFF,sp*sp*0.75f);
    int a = d2>0.90f ? (int)(255*(1.0f-(d2-0.90f)/0.10f)) : 255;
    cv_px(x,y,c,a);
  }
}


/* ── bubble display type ──────────────────────────────────────────
 *  Square blocks read as programmer art.  Every set pixel of a glyph is
 *  stamped as an antialiased disc into a coverage mask; overlapping
 *  discs merge into one fat rounded stroke, which is what makes a
 *  bubble letter a bubble letter.  The mask is then rendered three
 *  times - shadow, dark outline, gradient body - so the letters read as
 *  moulded plastic rather than a bitmap.
 * ---------------------------------------------------------------- */
#define TBW 1300
#define TBH 260

/* antialiased capsule: the union of discs swept along a segment, which
   is what turns a row of stamped dots into one smooth rounded stroke */
static void tb_cap(uint8_t*m,int mw,int mh,
                   float ax,float ay,float bx2,float by2,float r){
  float lo_x=(ax<bx2?ax:bx2)-r-1, hi_x=(ax>bx2?ax:bx2)+r+2;
  float lo_y=(ay<by2?ay:by2)-r-1, hi_y=(ay>by2?ay:by2)+r+2;
  int x0=(int)lo_x, x1=(int)hi_x, y0=(int)lo_y, y1=(int)hi_y;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>mw) x1=mw;
  if(y1>mh) y1=mh;
  float dx=bx2-ax, dy=by2-ay;
  float len2=dx*dx+dy*dy;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float px2=x+0.5f-ax, py2=y+0.5f-ay;
    float t = len2>0.0001f ? clampf((px2*dx+py2*dy)/len2,0,1) : 0.0f;
    float qx=px2-dx*t, qy=py2-dy*t;
    float c=clampf(r-sqrtf(qx*qx+qy*qy)+0.5f,0,1);
    int v=(int)(c*255);
    if(v>m[y*mw+x]) m[y*mw+x]=(uint8_t)v;
  }
}

static void tb_stamp(uint8_t*m,int mw,int mh,float cx,float cy,float r){
  int x0=(int)(cx-r-1), x1=(int)(cx+r+2);
  int y0=(int)(cy-r-1), y1=(int)(cy+r+2);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>mw) x1=mw;
  if(y1>mh) y1=mh;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=x+0.5f-cx, dy=y+0.5f-cy;
    float c=clampf(r-sqrtf(dx*dx+dy*dy)+0.5f,0,1);
    int v=(int)(c*255);
    if(v>m[y*mw+x]) m[y*mw+x]=(uint8_t)v;
  }
}

/*  Rasterising the bubble type is the expensive half of textb: every set
 *  pixel of every glyph stamps a disc and welds capsules to its
 *  neighbours, which for a big caption is hundreds of thousands of
 *  writes.  The result depends only on the string and the size, and the
 *  same few captions are drawn frame after frame, so the masks are
 *  cached and only the three composite passes run per frame.
 *
 *  The band renderer calls textb() from every thread at once, so the
 *  cache is shared under bp_lock: a lookup (or, on a miss, the
 *  rasterise) happens under the lock, the entry is pinned, and the
 *  composite runs unlocked.  Eviction never takes a pinned entry, and
 *  each thread pins at most one, so TBC must stay above the thread
 *  count - it is sized for a frame's worth of captions besides.       */
#define TBC 16
typedef struct {
  char     str[40];
  int      px, bw, bh, pad, capH, used, pins;
  uint8_t *fill, *out;
} tbcache_t;
static tbcache_t tbc[TBC];
static int tbc_clock;

/*  Mask geometry for a caption; textb() needs it before the cache.    */
static void tb_geom(int px,int wtot,int capH,int*pad,int*bw,int*bh){
  float Rin  = px*0.60f;
  float Rout = Rin + (px*0.30f > 1.5f ? px*0.30f : 1.5f);
  *pad=(int)(Rout+3.0f);
  *bw=wtot+*pad*2; *bh=capH+*pad*2;
  if(*bw>TBW) *bw=TBW;
  if(*bh>TBH) *bh=TBH;
}

/*  Caller holds bp_lock (when bands run in parallel).                 */
static tbcache_t* tb_render(const char*str,int px,int n,int adv,int wtot,int capH){
  for(int i=0;i<TBC;i++)
    if(tbc[i].used && tbc[i].px==px && !strcmp(tbc[i].str,str)){
      tbc[i].used=++tbc_clock;
      return &tbc[i];
    }
  /* evict the least recently used slot that no thread is drawing from */
  int slot=-1;
  for(int i=0;i<TBC;i++)
    if(!tbc[i].pins && (slot<0 || tbc[i].used < tbc[slot].used)) slot=i;
  if(slot<0) return NULL;
  tbcache_t*c=&tbc[slot];

  float Rin  = px*0.60f;
  float Rout = Rin + (px*0.30f > 1.5f ? px*0.30f : 1.5f);
  int pad,bw,bh;
  tb_geom(px,wtot,capH,&pad,&bw,&bh);

  uint8_t*f=(uint8_t*)realloc(c->fill,(size_t)bw*bh);
  if(f) c->fill=f;
  uint8_t*o=(uint8_t*)realloc(c->out ,(size_t)bw*bh);
  if(o) c->out=o;
  if(!f||!o){ free(c->fill); free(c->out); c->fill=c->out=NULL; c->used=0; return NULL; }
  memset(c->fill,0,(size_t)bw*bh);
  memset(c->out ,0,(size_t)bw*bh);

  for(int i=0;i<n;i++){
    const uint8_t*gl=glyph_of((unsigned char)str[i]);
    #define SET(rr,cc) ((rr)>=0&&(rr)<7&&(cc)>=0&&(cc)<5 && (gl[rr]&(0x10>>(cc))))
    for(int r=0;r<7;r++) for(int cc2=0;cc2<5;cc2++){
      if(!SET(r,cc2)) continue;
      float cx = pad + i*adv + cc2*px + px*0.5f;
      float cy = pad + r*px + px*0.5f;
      tb_stamp(c->out ,bw,bh,cx,cy,Rout);
      tb_stamp(c->fill,bw,bh,cx,cy,Rin);
      static const int NB[4][2] = {{0,1},{1,0},{1,1},{1,-1}};
      for(int k=0;k<4;k++){
        int nr=r+NB[k][0], nc=cc2+NB[k][1];
        if(!SET(nr,nc)) continue;
        float nx = pad + i*adv + nc*px + px*0.5f;
        float ny = pad + nr*px + px*0.5f;
        tb_cap(c->out ,bw,bh,cx,cy,nx,ny,Rout);
        tb_cap(c->fill,bw,bh,cx,cy,nx,ny,Rin);
      }
    }
    #undef SET
  }
  snprintf(c->str,sizeof c->str,"%s",str);
  c->px=px; c->bw=bw; c->bh=bh; c->pad=pad; c->capH=capH;
  c->used=++tbc_clock;
  return c;
}

/*  Rows [j0,j1) of a mask whose row 0 lands on screen row by, clipped
 *  to the band; columns clipped to the screen.  Returns 0 if empty.   */
static int tb_clip(int bx,int by,int bw,int bh,int*j0,int*j1,int*i0,int*i1){
  *j0=0; *j1=bh;
  if(by+*j0<clip_y0) *j0=clip_y0-by;
  if(by+*j1>clip_y1) *j1=clip_y1-by;
  *i0=bx<0?-bx:0; *i1=bx+bw>FBW?FBW-bx:bw;
  return *j0<*j1 && *i0<*i1;
}

static void textb(const char*str,int x,int y,int px,
                  const uint32_t*st,int ns,int align){
  int n=(int)strlen(str);
  if(n<1 || n>=(int)sizeof(tbc[0].str)) return;
  /* Auto-fit against BOTH the screen and the scratch buffer.  Fitting
     only the screen used to let a long caption overrun the buffer, which
     silently clipped its last letters rather than shrinking the type. */
  for(;;){
    int wt=n*px*6-px, pad2=2*(int)(px*0.90f+4.0f);
    if(px<=2) break;
    if(wt<=FBW-40 && wt+pad2<=TBW && px*7+pad2<=TBH) break;
    px--;
  }
  int adv=px*6, wtot=n*adv-px, capH=px*7;
  int ox = align==1 ? x-wtot/2 : (align==2 ? x-wtot : x);
  int pad,bw,bh;
  tb_geom(px,wtot,capH,&pad,&bw,&bh);
  int bx=ox-pad, by=y-pad;
  int so=(int)(px*0.34f); if(so<2) so=2;
  if(!rows_visible(by,by+bh+so)) return;       /* not this band's rows */

  if(bp_active) bp_lock();
  tbcache_t*c=tb_render(str,px,n,adv,wtot,capH);
  if(c) c->pins++;
  if(bp_active) bp_unlock();
  if(!c) return;

  int j0,j1,i0,i1;
  if(tb_clip(bx+so,by+so,bw,bh,&j0,&j1,&i0,&i1)){           /* shadow */
    for(int j=j0;j<j1;j++){
      const uint8_t*m=c->out+(size_t)j*bw;
      uint32_t*d=fb+(size_t)(by+so+j)*FBW+bx+so;
      for(int i=i0;i<i1;i++){
        uint32_t a=(uint32_t)m[i]*160u/255u;             /* black, 0..160 */
        d[i]=blend_rbg(d[i],0,0,256u-a);
      }
    }
  }
  if(tb_clip(bx,by,bw,bh,&j0,&j1,&i0,&i1)){
    for(int j=j0;j<j1;j++)                                     /* outline */
      span_mask(fb+(size_t)(by+j)*FBW+bx+i0,c->out+(size_t)j*bw+i0,i1-i0,0x180C02);
    for(int j=j0;j<j1;j++){                                    /* body    */
      float t=clampf((float)(j-pad)/(float)(capH>1?capH-1:1),0,1);
      uint32_t col=ramp(st,ns,t);
      if(t<0.34f) col=mixc(col,0xFFFFFF,(0.34f-t)/0.34f*0.55f);
      span_mask(fb+(size_t)(by+j)*FBW+bx+i0,c->fill+(size_t)j*bw+i0,i1-i0,col);
    }
  }

  if(bp_active) bp_lock();
  c->pins--;
  if(bp_active) bp_unlock();
}

/* ═══ SYMBOL ART ═══════════════════════════════════════════════════
 *  All coordinates are in 0..92 design units; U() lifts them onto the
 *  supersampled canvas.  Every symbol is built the same way:
 *      medallion  ->  art with a real surface  ->  glass over the top
 *  and the whole thing is rasterised once, at init.
 * ================================================================= */
#define U(v) ((float)(v)*(float)SS*((float)SYMW/SYMU))

/* canvas-space text, for the BAR faces */
static void cv_text(const char*s,float cx,float cy,float px,uint32_t col,int alpha){
  int n=(int)strlen(s);
  float adv=px*6.0f, wtot=n*adv-px;
  float ox=cx-wtot*0.5f, oy=cy-px*3.5f;
  for(int i=0;i<n;i++){
    int ch=(unsigned char)s[i];
    if(ch>='a'&&ch<='z') ch-=32;
    if(ch<32||ch>127) ch='?';
    const uint8_t*gl=FONT[ch-32];
    for(int r=0;r<7;r++){
      uint8_t bits=gl[r];
      for(int c=0;c<5;c++){
        if(!(bits&(0x10>>c))) continue;
        float gx=ox+i*adv+c*px, gy=oy+r*px;
        for(int j=0;j<(int)px;j++) for(int k=0;k<(int)px;k++)
          cv_px((int)gx+k,(int)gy+j,col,alpha);
      }
    }
  }
}


enum { SY_SEVEN=0, SY_DIAMOND, SY_BELL, SY_BAR, SY_GRAPES, SY_ORANGE, SY_PLUM,
       SY_CHERRY, SY_LEMON, SY_STAR, SY_CROWN, SY_JACKPOT, SY_ULT,
       SY_COIN, SY_WHEEL, NSYM };
#define NPAYSYM 9                   /* SY_SEVEN .. SY_LEMON pay as clusters */

static spr_t sym[NSYM];        /* crisp                                 */
static spr_t symb[NSYM];       /* vertically smeared, used while spinning*/
static spr_t glowspr;          /* soft radial, additive win glow        */
static spr_t washspr;          /* radial alpha mask, cell tinting       */
static spr_t domespr[2];       /* the SPIN dome, idle and pressed       */

static const char*SYMNAME[NSYM] = {
  "WILD 7","DIAMOND","BELL","BAR","GRAPES","ORANGE","PLUM","CHERRY","LEMON",
  "SCATTER","CROWN","JACKPOT","ULTIMATE","LUCKY COIN","WHEEL"
};

/* ═══ MATERIALS ════════════════════════════════════════════════════
 *  A flat polygon with a gradient reads as clip art however good the
 *  colours are.  These give any shape a surface instead: the shape is
 *  rasterised into a coverage mask, the mask is blurred into a height
 *  field, and the slope of the height field is a surface normal.  Light
 *  that and the shape gets a rounded, bevelled edge; reflect a studio
 *  environment in it and the bevel reads as polished gold or chrome.
 *  Everything here runs once, at init, on the supersampled canvas.
 * ================================================================= */
#define CVN (CANW*CANH)
static uint8_t cvm[CVN], cvm2[CVN], cvm3[CVN];   /* coverage masks     */
static float   cvf[CVN], cvg[CVN], cvt[CVN];     /* fields and scratch */

static void m_clear(uint8_t*m){ memset(m,0,CVN); }

/* even-odd scanline fill into a mask */
static void m_poly(uint8_t*m,const pt_t*p,int n){
  if(n<3) return;
  float miny=p[0].y, maxy=p[0].y;
  for(int i=1;i<n;i++){ if(p[i].y<miny) miny=p[i].y; if(p[i].y>maxy) maxy=p[i].y; }
  int y0=(int)floorf(miny), y1=(int)ceilf(maxy);
  if(y0<0) y0=0;
  if(y1>CANH) y1=CANH;
  float xs[96];
  for(int y=y0;y<y1;y++){
    float fy=y+0.5f; int cnt=0;
    for(int i=0,j=n-1;i<n;j=i++){
      float ya=p[i].y, yb=p[j].y;
      if((ya<=fy&&yb>fy)||(yb<=fy&&ya>fy)){
        float t=(fy-ya)/(yb-ya);
        if(cnt<96) xs[cnt++]=p[i].x+t*(p[j].x-p[i].x);
      }
    }
    for(int a=0;a<cnt-1;a++) for(int b=a+1;b<cnt;b++)
      if(xs[b]<xs[a]){ float t=xs[a]; xs[a]=xs[b]; xs[b]=t; }
    for(int a=0;a+1<cnt;a+=2){
      int xa=(int)ceilf(xs[a]-0.5f), xb=(int)ceilf(xs[a+1]-0.5f);
      if(xa<0) xa=0;
      if(xb>CANW) xb=CANW;
      if(xb>xa) memset(m+y*CANW+xa,255,(size_t)(xb-xa));
    }
  }
}
static void m_ellipse(uint8_t*m,float cx,float cy,float rx,float ry){
  int x0=(int)(cx-rx-1), x1=(int)(cx+rx+2), y0=(int)(cy-ry-1), y1=(int)(cy+ry+2);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=(x+0.5f-cx)/rx, dy=(y+0.5f-cy)/ry;
    if(dx*dx+dy*dy<=1.0f) m[y*CANW+x]=255;
  }
}
static __attribute__((unused)) void m_circle(uint8_t*m,float cx,float cy,float r){ m_ellipse(m,cx,cy,r,r); }
static void m_rrect(uint8_t*m,float x,float y,float w,float h,float r){
  int x0=(int)x, x1=(int)(x+w+1), y0=(int)y, y1=(int)(y+h+1);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  for(int yy=y0;yy<y1;yy++) for(int xx=x0;xx<x1;xx++)
    if(rr_sdf(xx+0.5f,yy+0.5f,x+w*0.5f,y+h*0.5f,w*0.5f,h*0.5f,r)<=0.0f) m[yy*CANW+xx]=255;
}
/* a capsule, for stems and strokes */
static __attribute__((unused)) void m_cap(uint8_t*m,float ax,float ay,float bx,float by,float r){
  tb_cap(m,CANW,CANH,ax,ay,bx,by,r);
}
/*  Bubble lettering into a mask: the same stamped-disc strokes as the
 *  display type, so the words on the symbols match the words on the
 *  cabinet.  wt is the stroke radius as a fraction of the font pixel. */
static void m_text(uint8_t*m,const char*s,float cx,float cy,float px,float wt){
  int n=(int)strlen(s);
  float adv=px*6.0f, wtot=n*adv-px;
  float ox=cx-wtot*0.5f, oy=cy-px*3.5f, R=px*wt;
  for(int i=0;i<n;i++){
    int ch=(unsigned char)s[i];
    if(ch>='a'&&ch<='z') ch-=32;
    if(ch<32||ch>127) ch='?';
    const uint8_t*gl=FONT[ch-32];
    #define SETB(rr,cc) ((rr)>=0&&(rr)<7&&(cc)>=0&&(cc)<5 && (gl[rr]&(0x10>>(cc))))
    for(int r=0;r<7;r++) for(int c=0;c<5;c++){
      if(!SETB(r,c)) continue;
      float x=ox+i*adv+c*px+px*0.5f, y=oy+r*px+px*0.5f;
      tb_stamp(m,CANW,CANH,x,y,R);
      static const int NB[4][2]={{0,1},{1,0},{1,1},{1,-1}};
      for(int k=0;k<4;k++){
        int nr=r+NB[k][0], nc=c+NB[k][1];
        if(!SETB(nr,nc)) continue;
        /* a diagonal weld only where the two cells are not already
           joined orthogonally, or the letters clot at every corner */
        if(NB[k][0]&&NB[k][1] && (SETB(r,nc)||SETB(nr,c))) continue;
        tb_cap(m,CANW,CANH,x,y,ox+i*adv+nc*px+px*0.5f,oy+nr*px+px*0.5f,R);
      }
    }
    #undef SETB
  }
}
static void m_sub(uint8_t*m,const uint8_t*cut){
  for(int i=0;i<CVN;i++) if(cut[i]) m[i]=(uint8_t)(m[i]*(255-cut[i])/255);
}
static void m_copy(uint8_t*d,const uint8_t*s){ memcpy(d,s,CVN); }

static int m_bbox(const uint8_t*m,int*x0,int*y0,int*x1,int*y1){
  int a=CANW,b=CANH,c=-1,d=-1;
  for(int y=0;y<CANH;y++){
    const uint8_t*row=m+y*CANW;
    for(int w8=0;w8<CANW;w8+=8){
      uint64_t v; memcpy(&v,row+w8,8);
      if(!v) continue;
      for(int x=w8;x<w8+8;x++) if(row[x]){
        if(x<a) a=x;
        if(x>c) c=x;
        if(y<b) b=y;
        d=y;
      }
    }
  }
  if(c<0) return 0;
  *x0=a; *y0=b; *x1=c+1; *y1=d+1;
  return 1;
}

/*  Box blur of a mask into a 0..1 field, `passes` times (two passes is
 *  a tent, three is near enough a gaussian).  Only the shape's bounding
 *  box, grown by the blur, is touched.  Returns 0 for an empty mask.  */
static int f_blur(const uint8_t*m,float*out,int r,int passes,
                  int*ox0,int*oy0,int*ox1,int*oy1){
  int x0,y0,x1,y1;
  if(!m_bbox(m,&x0,&y0,&x1,&y1)) return 0;
  int g=r*passes+2;
  x0-=g; y0-=g; x1+=g; y1+=g;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) out[y*CANW+x]=m[y*CANW+x]*(1.0f/255.0f);
  float inv=1.0f/(2*r+1);
  for(int p=0;p<passes && r>0;p++){
    for(int y=y0;y<y1;y++){
      const float*row=out+y*CANW; float*dst=cvt+y*CANW;
      float acc=0;
      for(int x=x0;x<=x0+r && x<x1;x++) acc+=row[x];
      for(int x=x0;x<x1;x++){
        dst[x]=acc*inv;
        if(x+r+1<x1) acc+=row[x+r+1];
        if(x-r>=x0)  acc-=row[x-r];
      }
    }
    for(int x=x0;x<x1;x++){
      float acc=0;
      for(int y=y0;y<=y0+r && y<y1;y++) acc+=cvt[y*CANW+x];
      for(int y=y0;y<y1;y++){
        out[y*CANW+x]=acc*inv;
        if(y+r+1<y1) acc+=cvt[(y+r+1)*CANW+x];
        if(y-r>=y0)  acc-=cvt[(y-r)*CANW+x];
      }
    }
  }
  *ox0=x0; *oy0=y0; *ox1=x1; *oy1=y1;
  return 1;
}

/*  Grow a shape by d canvas pixels: blur it and move the threshold out.
 *  Corners come out rounded, which is what a stroked outline wants.   */
static void m_dilate(const uint8_t*src,uint8_t*dst,float d){
  int r=(int)d+2, x0,y0,x1,y1;
  if(dst!=src) m_clear(dst);
  if(!f_blur(src,cvg,r,1,&x0,&y0,&x1,&y1)) return;
  float k=(float)(2*r+1);
  float th=0.5f-d/k, w=0.6f/k;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float v=(cvg[y*CANW+x]-(th-w))/(2.0f*w);
    int a=(int)(clampf(v,0,1)*255.0f);
    if(a>dst[y*CANW+x]) dst[y*CANW+x]=(uint8_t)a;
  }
}

/* paint a mask in one colour, or a vertical gradient over its extent */
static void cv_fill_mask(const uint8_t*m,uint32_t top,uint32_t bot,int alpha){
  int x0,y0,x1,y1;
  if(!m_bbox(m,&x0,&y0,&x1,&y1)) return;
  for(int y=y0;y<y1;y++){
    uint32_t c=mixc(top,bot,(float)(y-y0)/(float)(y1-y0>1?y1-y0-1:1));
    for(int x=x0;x<x1;x++){ int a=m[y*CANW+x]; if(a) cv_px(x,y,c,a*alpha/255); }
  }
}
static void cv_fill_mask_at(const uint8_t*m,int ox,int oy,uint32_t col,int alpha){
  for(int y=0;y<CANH;y++){
    int ty=y+oy; if(ty<0||ty>=CANH) continue;
    for(int x=0;x<CANW;x++){
      int a=m[y*CANW+x]; if(!a) continue;
      cv_px(x+ox,ty,col,a*alpha/255);
    }
  }
}

/*  atan2 to within a few thousandths of a radian, several times faster
 *  than libm's: the fans, rays and foils call it for every pixel.     */
static inline float fast_atan2(float y,float x){
  float ax=fabsf(x), ay=fabsf(y);
  float mx=ax>ay?ax:ay, mn=ax>ay?ay:ax;
  if(mx<1e-12f) return 0.0f;
  float a=mn/mx, q=a*a;
  float r=((-0.0464964749f*q+0.15931422f)*q-0.327622764f)*q*a+a;
  if(ay>ax) r=1.57079637f-r;
  if(x<0) r=3.14159274f-r;
  return y<0?-r:r;
}

/* ── value noise, for fire, peel and grain ───────────────────────── */
static float hash2(int x,int y,uint32_t s){
  uint32_t h=(uint32_t)x*374761393u + (uint32_t)y*668265263u + s*2246822519u;
  h=(h^(h>>13))*1274126177u; h^=h>>16;
  return (float)(h&0xFFFFFF)/16777216.0f;
}
static float vnoise(float x,float y,uint32_t s){
  int xi=(int)floorf(x), yi=(int)floorf(y);
  float fx=x-xi, fy=y-yi;
  fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy);
  float a=hash2(xi,yi,s), b=hash2(xi+1,yi,s), c=hash2(xi,yi+1,s), d=hash2(xi+1,yi+1,s);
  return lerpf(lerpf(a,b,fx),lerpf(c,d,fx),fy);
}
static float fbm(float x,float y,uint32_t s,int oct){
  float v=0, a=0.5f, tot=0;
  for(int i=0;i<oct;i++){ v+=vnoise(x,y,s+i*101u)*a; tot+=a; x*=2.03f; y*=2.03f; a*=0.5f; }
  return v/tot;
}
static float smooth01(float e0,float e1,float x){
  float t=clampf((x-e0)/(e1-e0),0,1); return t*t*(3-2*t);
}

/*  A studio environment for metal to reflect: bright softbox sky, a
 *  hard dark horizon, warm floor.  Indexed by how far the reflected ray
 *  points up.  The hard horizon is what makes chrome read as chrome.  */
static uint32_t env_map(float up,int kind){
  static const float    E[8] ={-1.0f,-0.50f,-0.14f,-0.02f,0.03f,0.30f,0.70f,1.0f};
  static const uint32_t CHR[8]={0x8E8274,0x4E4640,0x24262E,0x101216,0xDCE6F8,0x9EB0CE,0xE4ECFA,0xFFFFFF};
  static const uint32_t GLD[8]={0xB47C1C,0x7A4C0A,0x3E2204,0x1E0E02,0xFFF2BC,0xD89E28,0xFFDC80,0xFFFAE6};
  static const uint32_t PLT[8]={0x7A8AA8,0x3A4460,0x1C2236,0x0C0E18,0xE8F4FF,0xA8C4F0,0xF0F6FF,0xFFFFFF};
  const uint32_t*t = kind==1?GLD:(kind==2?PLT:CHR);
  if(up<=E[0]) return t[0];
  for(int i=0;i<7;i++) if(up<=E[i+1]) return mixc(t[i],t[i+1],(up-E[i])/(E[i+1]-E[i]));
  return t[7];
}

/*  How far a reflected ray points up, for a surface normal n seen head
 *  on.  A little of the sideways component is folded in, so a face
 *  turned toward the key light (up and left) sees the bright sky and
 *  vertical edges do not all land on the dark horizon.               */
/* x to a whole power by squaring: the specular terms, a lot cheaper
   than powf at a few million pixels per symbol sheet */
static inline float ipow(float x,int n){
  float r=1.0f;
  while(n){ if(n&1) r*=x; x*=x; n>>=1; }
  return r;
}
static inline float env_up(float nx,float ny,float nz){
  return -2.0f*nz*(ny*0.85f+nx*0.40f);
}

typedef struct {
  const uint32_t*ramp; int nr;  /* face colour, top to bottom (or across)  */
  float bevel;                  /* width of the rounded edge, design units */
  float metal;                  /* 0 lacquer .. 1 mirror                    */
  int   env;                    /* 0 chrome, 1 gold, 2 platinum             */
  float spec, shin;             /* specular strength and tightness          */
  uint32_t glow; float glowAmt, glowR;  /* hot core, design units           */
  int   horiz;                  /* ramp runs across (a turned surface)      */
  float relief;                 /* 1 raised, negative engraved              */
} mat_t;

/* the key light: up and to the left, the way every symbol is lit */
#define LX (-0.50f)
#define LY (-0.68f)
#define LZ ( 0.54f)

static void cv_shade(const uint8_t*m,const mat_t*mt,int alpha){
  int r=(int)(U(mt->bevel)*0.30f); if(r<1) r=1;
  int x0,y0,x1,y1;
  if(!f_blur(m,cvf,r,2,&x0,&y0,&x1,&y1)) return;
  if(mt->glowAmt>0.0f){
    int gx0,gy0,gx1,gy1, gr=(int)(U(mt->glowR)*0.5f); if(gr<1) gr=1;
    f_blur(m,cvg,gr,2,&gx0,&gy0,&gx1,&gy1);
  }
  int bx0=0,by0=0,bx1=0,by1=0;
  m_bbox(m,&bx0,&by0,&bx1,&by1);
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ), lx=LX/ll, ly=LY/ll, lz=LZ/ll;
  float hx=lx, hy=ly, hz=lz+1.0f, hl=sqrtf(hx*hx+hy*hy+hz*hz);
  hx/=hl; hy/=hl; hz/=hl;
  float S=(mt->relief!=0.0f?mt->relief:1.0f)*1.7f*(2*r+1);
  float spec0=ipow(hz,(int)mt->shin);
  for(int y=by0;y<by1;y++) for(int x=bx0;x<bx1;x++){
    int a=m[y*CANW+x]; if(!a) continue;
    int i=y*CANW+x;
    float gx=(x+1<x1?cvf[i+1]:cvf[i])-(x-1>=x0?cvf[i-1]:cvf[i]);
    float gy=(y+1<y1?cvf[i+CANW]:cvf[i])-(y-1>=y0?cvf[i-CANW]:cvf[i]);
    float nx=-gx*0.5f*S, ny=-gy*0.5f*S, nz=1.0f;
    float nl=sqrtf(nx*nx+ny*ny+1.0f); nx/=nl; ny/=nl; nz/=nl;
    float t = mt->horiz ? (float)(x-bx0)/(float)(bx1-bx0) : (float)(y-by0)/(float)(by1-by0);
    uint32_t c=ramp(mt->ramp,mt->nr,t);
    float k=(nx*lx+ny*ly+nz*lz)-lz;
    c = k>0 ? mixc(c,0xFFFFFF,clampf(k*0.85f,0,0.8f)) : scalec(c,clampf(1.0f+k*1.25f,0.15f,1));
    if(mt->metal>0.0f){
      float up=env_up(nx,ny,nz);          /* reflected ray, how far it points up */
      float wm=mt->metal*clampf((1.0f-nz)*3.2f,0,1);
      if(wm>0.0f) c=mixc(c,env_map(up,mt->env),wm);
    }
    if(mt->spec>0.0f){
      float sd=nx*hx+ny*hy+nz*hz;
      float sp=(sd>0?ipow(sd,(int)mt->shin):0)-spec0;
      if(sp>0) c=mixc(c,0xFFFFFF,clampf(sp*mt->spec*3.0f,0,1));
    }
    if(mt->glowAmt>0.0f){
      float g=smooth01(0.45f,1.0f,cvg[i]);
      if(g>0) c=mixc(c,mt->glow,g*mt->glowAmt);
    }
    cv_px(x,y,c,a*alpha/255);
  }
}

/* stock finishes */
static const uint32_t GOLDFACE[6]={0xFFF6D0,0xFFE27A,0xEAAA22,0xB87412,0xF6C84A,0x8A5608};
static const uint32_t GOLDTURN[7]={0x6A4206,0xE8B03A,0xFFF4C8,0xFFD560,0xC88C1C,0x8A5A0A,0x4A2E04};
static const uint32_t CHROMEFACE[5]={0xFFFFFF,0xDDE4F2,0x8A96B0,0xE6ECF8,0x6A7490};

static mat_t mat_gold(float bevel){
  mat_t m={GOLDFACE,6,bevel,0.9f,1,0.9f,40.0f,0,0,0,0,1.0f}; return m;
}
static mat_t mat_candy(const uint32_t*ramp,int n,float bevel){
  mat_t m={ramp,n,bevel,0.28f,0,1.0f,50.0f,0,0,0,0,1.0f}; return m;
}

/*  A four-point glint with a soft halo: the thing that makes a gem or a
 *  polished edge look like it just caught the light.                  */
static void cv_sparkle(float cx,float cy,float r,int alpha){
  int x0=(int)(cx-r), x1=(int)(cx+r+1), y0=(int)(cy-r), y1=(int)(cy+r+1);
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=(x+0.5f-cx)/r, dy=(y+0.5f-cy)/r, d=sqrtf(dx*dx+dy*dy);
    if(d>=0.55f) continue;
    float v=1.0f-d/0.55f; v=v*v*v;
    cv_px(x,y,0xFFFFFF,(int)(v*alpha*0.8f));
  }
  pt_t h[4]={{cx-r,cy},{cx,cy-r*0.075f},{cx+r,cy},{cx,cy+r*0.075f}};
  pt_t v[4]={{cx,cy-r},{cx+r*0.075f,cy},{cx,cy+r},{cx-r*0.075f,cy}};
  cv_poly(h,4,0xFFFFFF,0xFFFFFF,alpha);
  cv_poly(v,4,0xFFFFFF,0xFFFFFF,alpha);
  float q=r*0.42f;
  pt_t d1[4]={{cx-q,cy-q},{cx+q*0.06f,cy-q*0.06f},{cx+q,cy+q},{cx-q*0.06f,cy+q*0.06f}};
  pt_t d2[4]={{cx+q,cy-q},{cx+q*0.06f,cy+q*0.06f},{cx-q,cy+q},{cx-q*0.06f,cy-q*0.06f}};
  cv_poly(d1,4,0xFFFFFF,0xFFFFFF,alpha*2/3);
  cv_poly(d2,4,0xFFFFFF,0xFFFFFF,alpha*2/3);
}

/*  Glossy fruit.  A lambert sphere reads as a ball; fruit needs the
 *  light to wrap past the terminator and pick up a saturated, warmer
 *  colour there (the flesh under the skin), a cool rim bounced up from
 *  the medallion, and a hard-edged window reflection rather than a
 *  soft blob.  `tex` roughens the peel with noise in the normal.      */
static void cv_fruit(float cx,float cy,float rx,float ry,
                     uint32_t lo,uint32_t mid,uint32_t hi,uint32_t sss,
                     float tex,float texf,uint32_t seed){
  int x0=(int)(cx-rx-1), x1=(int)(cx+rx+2), y0=(int)(cy-ry-1), y1=(int)(cy+ry+2);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ), lx=LX/ll, ly=LY/ll, lz=LZ/ll;
  float hx=lx, hy=ly, hz=lz+1.0f, hl=sqrtf(hx*hx+hy*hy+hz*hz);
  hx/=hl; hy/=hl; hz/=hl;
  uint32_t st[3]={lo,mid,hi};
  uint32_t rim=mixc(hi,0x9AC4FF,0.55f);
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=(x+0.5f-cx)/rx, dy=(y+0.5f-cy)/ry, d2=dx*dx+dy*dy;
    if(d2>1.0f) continue;
    float nz=sqrtf(1.0f-d2), nx=dx, ny=dy;
    if(tex>0.0f){
      float f=texf/(float)SS;
      float e=0.8f;
      float n0=fbm(x*f,y*f,seed,3), n1=fbm((x+e)*f,y*f,seed,3), n2=fbm(x*f,(y+e)*f,seed,3);
      nx+=(n1-n0)*tex*60.0f; ny+=(n2-n0)*tex*60.0f;
      float nl=sqrtf(nx*nx+ny*ny+nz*nz); nx/=nl; ny/=nl; nz/=nl;
    }
    float ndl=nx*lx+ny*ly+nz*lz;
    float w=clampf((ndl+0.42f)/1.42f,0,1);
    uint32_t c=ramp(st,3,w);
    float term=1.0f-fabsf(ndl-0.08f)/0.32f;
    if(term>0) c=mixc(c,sss,term*0.42f);
    float fr=powf(1.0f-nz,2.2f)*clampf(0.25f+dx*0.55f+dy*0.75f,0,1);
    c=mixc(c,rim,clampf(fr*0.85f,0,0.7f));
    float ao=ipow(1.0f-nz,3)*clampf(0.3f+dy*0.7f,0,1);
    c=scalec(c,1.0f-ao*0.35f);
    float sd=nx*hx+ny*hy+nz*hz;
    float sp=ipow(clampf(sd,0,1),90);
    float win=smooth01(0.9935f,0.9965f,sd);               /* hard window  */
    c=mixc(c,0xFFFFFF,clampf(sp*0.55f+win*0.85f,0,1));
    float s2=ipow(clampf(nx*0.35f+ny*0.55f+nz*0.76f,0,1),30); /* bounce spec */
    c=mixc(c,0xFFFFFF,s2*0.18f);
    int a = d2>0.92f ? (int)(255*(1.0f-(d2-0.92f)/0.08f)) : 255;
    cv_px(x,y,c,a);
  }
}

/*  Fire: noise turbulence under a tongue-shaped envelope, run through a
 *  black-body ramp.  `phase` scrolls the turbulence upward, so a few
 *  phases baked side by side flicker when they are cycled.            */
static void cv_fire(float ux0,float uy0,float ux1,float uy1,float phase,uint32_t seed,int alpha){
  static const uint32_t FIRE[6]={0x5A0400,0xC01A04,0xFF5A0C,0xFFA41C,0xFFE070,0xFFFFE0};
  int x0=(int)U(ux0), x1=(int)U(ux1), y0=(int)U(uy0), y1=(int)U(uy1);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  float W=(float)(x1-x0), H=(float)(y1-y0);
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float u=(x-x0)/W, v=(y1-y)/H;                         /* v: 0 base, 1 top */
    float env=sinf(u*3.14159f); env=sqrtf(env);
    float tongues=0.55f+0.50f*fbm(u*4.2f+seed*0.37f, phase*0.8f, seed, 2);
    float turb=fbm(u*6.0f, v*3.2f-phase*2.6f, seed+7u, 4);
    float hgt=tongues*env;
    float I=(hgt-v)/(hgt+0.001f)*1.35f+(turb-0.5f)*1.05f;
    I=clampf(I,0,1);
    if(I<0.06f) continue;
    uint32_t c=ramp(FIRE,6,clampf(I*1.08f,0,1));
    int a=(int)(smooth01(0.06f,0.42f,I)*alpha);
    cv_px(x,y,c,a);
  }
}

/*  Bubble label: dark keyline, then a lacquered body with a real bevel.
 *  The same bubble strokes as the cabinet type, at canvas resolution. */
static void cv_label(const char*s,float cx,float cy,float px,
                     const uint32_t*rampc,int nr,uint32_t line,float lineW,float bevel){
  m_clear(cvm); m_text(cvm,s,cx,cy,px,0.62f);
  m_dilate(cvm,cvm2,lineW);
  cv_fill_mask(cvm2,line,line,255);
  mat_t m=mat_candy(rampc,nr,bevel);
  m.spec=0.7f;
  cv_shade(cvm,&m,255);
}

/* the chrome studio ring's cross-section: a torus, lit and reflecting */
static uint32_t ring_px(float dx,float dy,float r,float rc,float hw,int kind){
  float t=clampf((r-rc)/hw,-1,1);
  float nr=t*0.93f, nz=sqrtf(1.0f-nr*nr);
  float nx=dx/r*nr, ny=dy/r*nr;
  uint32_t c=env_map(env_up(nx,ny,nz),kind);
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ);
  float hx=LX/ll, hy=LY/ll, hz=LZ/ll+1.0f, hl=sqrtf(hx*hx+hy*hy+hz*hz);
  float sd=(nx*hx+ny*hy+nz*hz)/hl;
  float sp=ipow(clampf(sd,0,1),60);
  /* an azimuthal ripple, as a real softbox is not one even panel */
  float ca=dx/r, sa=dy/r;
  float s3=3.0f*sa-4.0f*sa*sa*sa, c3=4.0f*ca*ca*ca-3.0f*ca;      /* sin 3a, cos 3a */
  c=scalec(c,0.92f+0.10f*(s3*0.825f+c3*0.565f));
  return mixc(c,0xFFFFFF,sp*0.9f);
}

/* ── the medallion ─────────────────────────────────────────────────
 *  Every symbol sits on a chrome-ringed, domed disc, the way the premium
 *  symbols on a modern cabinet do.  The ring is a lit torus reflecting a
 *  studio, so it carries the hard horizon line that makes chrome read
 *  as chrome; a dark groove steps down to the disc; the disc is a
 *  shallow dome with wrap lighting, a shadow under the lip where the
 *  ring blocks the key light, and a cool rim light opposite.
 * ---------------------------------------------------------------- */
static uint8_t *medalRing=NULL;   /* ring, groove and lip: the same on every medal */
static void cv_medal(uint32_t lo,uint32_t mid,uint32_t hi){
  const float cx=U(46), cy=U(46);
  if(!medalRing){
    medalRing=(uint8_t*)calloc(CVN,4);
    if(medalRing){
      const float R0=U(45.4f), R1=U(39.4f), RG=U(38.5f), RD=U(37.8f);
      for(int y=0;y<CANH;y++) for(int x=0;x<CANW;x++){
        float dx=x+0.5f-cx, dy=y+0.5f-cy, r=sqrtf(dx*dx+dy*dy);
        if(r>R0||r<RD) continue;
        uint32_t c;
        if(r>=R1) c=ring_px(dx,dy,r,(R0+R1)*0.5f,(R0-R1)*0.5f,0);
        else if(r>=RG){ float t=(r-RG)/(R1-RG); c=mixc(0x05060A,0x2A2E3A,t*t); }
        else {
          float t=(r-RD)/(RG-RD);
          float lit=clampf(0.5f+(dx*0.55f+dy*0.8f)/r*0.5f,0,1);
          c=mixc(0x2A2E3A,mixc(0x8A94AA,0xF0F4FF,lit),1.0f-t);
        }
        uint8_t*o=medalRing+(y*CANW+x)*4;
        o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
        o[3]=(uint8_t)(r>R0-1.5f ? (int)(255*clampf((R0-r)/1.5f,0,1)) : 255);
      }
    }
  }
  const float R0=U(45.4f), R1=U(39.4f), RG=U(38.5f), RD=U(37.8f);
  int x0=(int)(cx-R0-2), x1=(int)(cx+R0+3), y0=(int)(cy-R0-2), y1=(int)(cy+R0+3);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  uint32_t st[4]={scalec(lo,0.7f),lo,mid,hi};
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ), lx=LX/ll, ly=LY/ll, lz=LZ/ll;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=x+0.5f-cx, dy=y+0.5f-cy, r=sqrtf(dx*dx+dy*dy);
    if(r>R0) continue;
    uint32_t c;
    if(r>=RD && medalRing){
      const uint8_t*m=medalRing+(y*CANW+x)*4;
      cv_px(x,y,RGB(m[0],m[1],m[2]),m[3]);
      continue;
    }
    if(r>=R1){                                          /* chrome torus   */
      c=ring_px(dx,dy,r,(R0+R1)*0.5f,(R0-R1)*0.5f,0);
    } else if(r>=RG){                                   /* the groove     */
      float t=(r-RG)/(R1-RG);
      c=mixc(0x05060A,0x2A2E3A,t*t);
    } else if(r>=RD){                                   /* inner lip      */
      float t=(r-RD)/(RG-RD);
      float lit=clampf(0.5f+(dx*0.55f+dy*0.8f)/r*0.5f,0,1);   /* catches light low-right */
      c=mixc(0x2A2E3A,mixc(0x8A94AA,0xF0F4FF,lit),1.0f-t);
    } else {                                            /* the dome       */
      float u=r/RD;
      float nx=dx/RD*0.62f, ny=dy/RD*0.62f, nz=sqrtf(1.0f-(nx*nx+ny*ny));
      float ndl=nx*lx+ny*ly+nz*lz;
      float w=clampf((ndl+0.30f)/1.30f,0,1);
      c=ramp(st,4,w*0.92f+0.08f*(1.0f-u));
      /* shadow under the lip, upper left, where the ring hides the key */
      float ao=smooth01(0.72f,1.0f,u)*clampf(-(dx*0.55f+dy*0.83f)/r,0,1);
      c=scalec(c,1.0f-ao*0.55f);
      /* cool rim light opposite */
      float rl=smooth01(0.80f,1.0f,u)*clampf((dx*0.5f+dy*0.86f)/r,0,1);
      c=mixc(c,mixc(hi,0xFFFFFF,0.35f),rl*0.55f);
      /* a soft, wide specular bloom on the dome */
      float sx=dx/RD+0.30f, sy=dy/RD+0.42f;
      float sp=clampf(1.0f-sqrtf(sx*sx+sy*sy)*1.6f,0,1);
      c=mixc(c,mixc(hi,0xFFFFFF,0.5f),sp*sp*0.30f);
    }
    int a = r>R0-1.5f ? (int)(255*clampf((R0-r)/1.5f,0,1)) : 255;
    cv_px(x,y,c,a);
  }
}

/*  Glass over the finished symbol: a crisp window reflection across the
 *  top of the dome and a glint on the ring.  Very light, but it is what
 *  puts the art UNDER something rather than printed on the surface.  */
static void cv_glass(void){
  const float cx=U(46), cy=U(46), RD=U(37.8f);
  int x0=(int)(cx-RD), x1=(int)(cx+RD+1), y0=(int)(cy-RD), y1=(int)(cy+1);
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=x+0.5f-cx, dy=y+0.5f-cy;
    if(dx*dx+dy*dy>RD*RD*0.93f) continue;
    /* crescent: inside the dome, above a flatter ellipse */
    float ex=dx/(RD*1.02f), ey=(dy+RD*0.30f)/(RD*0.62f);
    if(ex*ex+ey*ey<1.0f) continue;
    float v=clampf(-dy/RD,0,1);
    int a=(int)(smooth01(0.05f,0.75f,v)*46.0f);
    cv_px(x,y,0xFFFFFF,a);
  }
  cv_sparkle(U(17.5f),U(21.0f),U(7.5f),200);
}

/* ---- pieces shared by several symbols ---- */

/*  The 7.  Authored on 8..84 x 8..89, placed scaled about (ox,oy).  A
 *  gold rim with a mirror bevel round a lacquered body that glows from
 *  the core outward, like light through red glass.                    */
static void seven_shape(float ox,float oy,float s,const uint32_t*body,int nb,int rim){
  pt_t p[7]={{8,8},{84,8},{84,26},{59,89},{29,89},{61,27},{8,27}};
  for(int i=0;i<7;i++){ p[i].x=U(ox+(p[i].x-46)*s); p[i].y=U(oy+(p[i].y-48)*s); }
  m_clear(cvm3); m_poly(cvm3,p,7);                        /* the body   */
  if(rim){
    m_dilate(cvm3,cvm2,U(3.4f*s));                         /* gold rim   */
    m_dilate(cvm2,cvm,U(1.9f*s));                          /* keyline    */
    cv_fill_mask_at(cvm,(int)U(0.9f),(int)U(1.6f),0x000000,120);
    cv_fill_mask(cvm,0x1A0604,0x1A0604,255);
    mat_t g=mat_gold(2.6f*s);
    cv_shade(cvm2,&g,255);
  }
  mat_t b=mat_candy(body,nb,4.0f*s);
  b.glow=mixc(body[0],0xFFD060,0.55f); b.glowAmt=0.30f; b.glowR=8.0f*s;
  b.spec=0.9f; b.shin=36.0f;
  cv_shade(cvm3,&b,255);
  /* a lacquer highlight along the top bar */
  pt_t hl[4]={{13,11.5f},{79,11.5f},{78,15.5f},{14,15.5f}};
  for(int i=0;i<4;i++){ hl[i].x=U(ox+(hl[i].x-46)*s); hl[i].y=U(oy+(hl[i].y-48)*s); }
  cv_poly(hl,4,0xFFFFFF,0xFFFFFF,110);
  pt_t hl2[4]={{63,29},{66,29},{47,74},{45,74}};
  for(int i=0;i<4;i++){ hl2[i].x=U(ox+(hl2[i].x-46)*s); hl2[i].y=U(oy+(hl2[i].y-48)*s); }
  cv_poly(hl2,4,0xFFFFFF,0xFFFFFF,60);
}

/* flames licking up behind the 7: phase 0 is the resting frame */
static float flamePhase = 0.0f;
static void flames(float cx,float cy,float s){
  cv_fire(cx-60*s,cy-56*s,cx+60*s,cy+8*s,flamePhase,77u,255);
}

/*  A blaze round the wild 7: the 7's own silhouette, blurred and lifted
 *  a few units, becomes the fuel; turbulence scrolling up through it
 *  gives the tongues.  Painted before the 7, so only the aura shows.  */
static void seven_aura(float ox,float oy,float s){
  static const uint32_t FIRE[6]={0x3A0200,0xB01404,0xFF4A08,0xFF9A18,0xFFE070,0xFFFFE0};
  pt_t p[7]={{8,8},{84,8},{84,26},{59,89},{29,89},{61,27},{8,27}};
  for(int i=0;i<7;i++){ p[i].x=U(ox+(p[i].x-46)*s); p[i].y=U(oy+(p[i].y-48)*s); }
  m_clear(cvm); m_poly(cvm,p,7);
  int x0,y0,x1,y1;
  if(!f_blur(cvm,cvg,(int)U(5.5f),2,&x0,&y0,&x1,&y1)) return;
  int lift=(int)U(5.0f);
  const float R0=U(37.8f), cx=U(46), cy=U(46);
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    int ys=y+lift; if(ys>=CANH) continue;
    float a=cvg[ys*CANW+x];
    if(a<0.02f) continue;
    float dx=x+0.5f-cx, dy=y+0.5f-cy;
    if(dx*dx+dy*dy>R0*R0) continue;                 /* inside the disc only */
    float u=(float)x/CANW, v=(float)y/CANH;
    float turb=fbm(u*10.0f,v*7.0f+flamePhase*3.0f,91u,4);
    float I=clampf(a*2.8f+(turb-0.5f)*1.5f-0.12f,0,1)*clampf((1.0f-a)*4.0f,0,1);
    if(I<0.05f) continue;
    cv_px(x,y,ramp(FIRE,6,clampf(I*1.15f,0,1)),(int)(smooth01(0.05f,0.45f,I)*240));
  }
}

/*  A banner across the lower medal: folded tails, a gold-rimmed band
 *  and the word in bubble type, sized to fit.                         */
static void ribbon(float y,float w,const char*label,uint32_t top,uint32_t bot,uint32_t ink){
  float x=46-w*0.5f, h=12.5f;
  /* the folded tails, tucked behind */
  for(int sd=0;sd<2;sd++){
    float ex = sd? x+w-2.0f : x+2.0f, dir = sd? 1.0f : -1.0f;
    pt_t t[5]={{ex,y+2.5f},{ex+dir*7.5f,y+3.5f},{ex+dir*5.0f,y+h*0.5f+2.5f},
               {ex+dir*7.5f,y+h+1.5f},{ex,y+h+0.5f}};
    for(int i=0;i<5;i++){ t[i].x=U(t[i].x); t[i].y=U(t[i].y); }
    m_clear(cvm); m_poly(cvm,t,5);
    m_dilate(cvm,cvm2,U(0.9f));
    cv_fill_mask(cvm2,0x120804,0x120804,255);
    uint32_t tr[3]={scalec(bot,0.75f),scalec(bot,0.55f),scalec(bot,0.35f)};
    mat_t tm=mat_candy(tr,3,1.6f);
    cv_shade(cvm,&tm,255);
  }
  m_clear(cvm3); m_rrect(cvm3,U(x),U(y),U(w),U(h),U(3.2f));
  m_dilate(cvm3,cvm2,U(1.5f));
  m_dilate(cvm2,cvm,U(0.9f));
  cv_fill_mask_at(cvm,0,(int)U(1.2f),0x000000,130);
  cv_fill_mask(cvm,0x140A02,0x140A02,255);
  mat_t g=mat_gold(1.4f);
  cv_shade(cvm2,&g,255);
  uint32_t br[4]={mixc(top,0xFFFFFF,0.35f),top,bot,scalec(bot,0.7f)};
  mat_t bm=mat_candy(br,4,2.2f);
  cv_shade(cvm3,&bm,255);
  cv_rrect(U(x+1.8f),U(y+1.0f),U(w-3.6f),U(3.6f),U(1.8f),0xFFFFFF,0xFFFFFF,70);
  int n=(int)strlen(label);
  float px=(w-7.0f)/(float)(n*6-1);
  if(px>2.0f) px=2.0f;
  m_clear(cvm); m_text(cvm,label,U(46),U(y+h*0.5f+0.3f),U(px),0.60f);
  cv_fill_mask_at(cvm,0,(int)U(0.5f),0xFFFFFF,90);          /* engraved lip */
  cv_fill_mask(cvm,mixc(ink,0x000000,0.1f),ink,255);
}

/*  A leaf: pointed, tilted, lit, with a midrib and veins.             */
static void leaf(float cx,float cy,float rx,float ry){
  static const uint32_t LG[4]={0xC6F5A0,0x5CC04A,0x21862A,0x0C4A14};
  const float ang=-0.38f, ca=cosf(ang), sa=sinf(ang);
  pt_t p[24];
  for(int i=0;i<12;i++){
    float t=i/11.0f, x=-rx+2*rx*t, w=ry*sinf(t*3.14159f)*(1.0f-0.25f*t);
    p[i].x=cx+x*ca-(-w)*sa; p[i].y=cy+x*sa+(-w)*ca;
  }
  for(int i=0;i<12;i++){
    float t=1.0f-i/11.0f, x=-rx+2*rx*t, w=ry*sinf(t*3.14159f)*(1.0f-0.25f*t)*0.85f;
    p[12+i].x=cx+x*ca-w*sa; p[12+i].y=cy+x*sa+w*ca;
  }
  m_clear(cvm3); m_poly(cvm3,p,24);
  m_dilate(cvm3,cvm2,U(0.9f));
  cv_fill_mask(cvm2,0x082A0C,0x082A0C,255);
  mat_t m=mat_candy(LG,4,2.6f);
  m.spec=0.8f; m.shin=30.0f;
  cv_shade(cvm3,&m,255);
  /* midrib and a few veins */
  for(int i=0;i<=24;i++){
    float t=i/24.0f, x=-rx*0.92f+1.84f*rx*t, bow=-ry*0.10f*sinf(t*3.14159f);
    float px=cx+x*ca-bow*sa, py=cy+x*sa+bow*ca;
    cv_circle(px,py,U(0.45f),0xDFFFC0,0xDFFFC0,150);
  }
  for(int k=1;k<=3;k++){
    float t=k/4.0f, x=-rx+2*rx*t;
    for(int sd=-1;sd<=1;sd+=2) for(int i=0;i<=8;i++){
      float q=i/8.0f, vx=x+q*rx*0.28f, vw=sd*q*ry*0.62f;
      cv_circle(cx+vx*ca-vw*sa,cy+vx*sa+vw*ca,U(0.28f),0xBFF0A0,0xBFF0A0,90);
    }
  }
}

/* a stem: a lit capsule chain along a quadratic curve */
static void stem(float ax,float ay,float bx,float by,float bend,float r){
  for(int i=0;i<=30;i++){
    float t=i/30.0f;
    float mx=(ax+bx)*0.5f+bend, my=(ay+by)*0.5f;
    float x=(1-t)*(1-t)*ax+2*(1-t)*t*mx+t*t*bx, y=(1-t)*(1-t)*ay+2*(1-t)*t*my+t*t*by;
    cv_circle(x,y,r*1.25f,0x1A1004,0x1A1004,255);
  }
  for(int i=0;i<=30;i++){
    float t=i/30.0f;
    float mx=(ax+bx)*0.5f+bend, my=(ay+by)*0.5f;
    float x=(1-t)*(1-t)*ax+2*(1-t)*t*mx+t*t*bx, y=(1-t)*(1-t)*ay+2*(1-t)*t*my+t*t*by;
    cv_circle(x,y,r,0x6A8A1A,0x2E4A08,255);
    cv_circle(x-r*0.35f,y-r*0.35f,r*0.38f,0xD8F090,0xD8F090,170);
  }
}

/*  Radiating light behind a feature symbol: soft rays over the disc. */
static void cv_rays(float cx,float cy,float r0,float r1,int n,float rot,uint32_t col,int alpha){
  int x0=(int)(cx-r1), x1=(int)(cx+r1+1), y0=(int)(cy-r1), y1=(int)(cy+r1+1);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>CANW) x1=CANW;
  if(y1>CANH) y1=CANH;
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=x+0.5f-cx, dy=y+0.5f-cy, r=sqrtf(dx*dx+dy*dy);
    if(r>r1||r<1.0f) continue;
    float a=fast_atan2(dy,dx)*n/TAU+rot;
    float f=a-floorf(a);
    float ray=smooth01(0.30f,0.5f,f)*(1.0f-smooth01(0.5f,0.70f,f));
    float fall=clampf((r-r0)/(r1-r0),0,1);
    float core=1.0f-clampf(r/r1,0,1);
    int al=(int)(alpha*(ray*(1.0f-fall)*0.9f+core*core*0.35f));
    cv_px(x,y,col,al);
  }
}

/* a faceted gem: dark crown, bright table, sparkle */
static void cv_gem(float cx,float cy,float r,uint32_t lo,uint32_t mid,uint32_t hi){
  cv_circle(cx,cy+r*0.12f,r*1.22f,0x140800,0x140800,255);
  cv_fruit(cx,cy,r,r,lo,mid,hi,mid,0,0,0);
  pt_t t[6];
  for(int i=0;i<6;i++){ float a=TAU*i/6.0f+0.26f; t[i].x=cx+cosf(a)*r*0.52f; t[i].y=cy+sinf(a)*r*0.52f; }
  cv_poly(t,6,mixc(hi,0xFFFFFF,0.4f),mid,150);
  cv_sparkle(cx-r*0.35f,cy-r*0.4f,r*1.1f,230);
}

/* ---- individual pieces ---- */

/*  The wild is rendered five times (at rest and four flame phases), and
 *  only the fire differs, so the medal and the 7 over it are kept as
 *  two layers and the fire is painted between them each time.         */
static uint8_t *s7bg=NULL, *s7fg=NULL;
static int s7cached=0;
static void cv_over_layer(const uint8_t*l){
  for(int i=0;i<CVN;i++){ const uint8_t*p=l+i*4; if(p[3]) cv_px(i%CANW,i/CANW,RGB(p[0],p[1],p[2]),p[3]); }
}
static void art_seven(void){
  static const uint32_t body[5]={0xFF8A70,0xFF3A28,0xE0101C,0x980614,0x4A020A};
  if(!s7bg){ s7bg=(uint8_t*)malloc(sizeof canvas); s7fg=(uint8_t*)malloc(sizeof canvas); }
  if(s7bg && s7fg && s7cached){
    memcpy(canvas,s7bg,sizeof canvas);
    seven_aura(46,49,0.64f);
    flames(47,30,0.52f);
    cv_over_layer(s7fg);
    return;
  }
  cv_medal(0x062A0A,0x1F8A30,0x9AF080);
  cv_rays(U(46),U(40),U(10),U(38),12,0.1f,0xE0FFB0,70);
  if(s7bg && s7fg){
    memcpy(s7bg,canvas,sizeof canvas);
    cv_clear();
    seven_shape(46,49,0.64f,body,5,1);
    ribbon(76.5f,40,"WILD",0xFFE9A8,0xC08A10,0x3A1400);
    memcpy(s7fg,canvas,sizeof canvas);
    s7cached=1;
    memcpy(canvas,s7bg,sizeof canvas);
    seven_aura(46,49,0.64f);
    flames(47,30,0.52f);
    cv_over_layer(s7fg);
    return;
  }
  seven_aura(46,49,0.64f);
  flames(47,30,0.52f);                       /* tips lick up behind the top bar */
  seven_shape(46,49,0.64f,body,5,1);
  ribbon(76.5f,40,"WILD",0xFFE9A8,0xC08A10,0x3A1400);
}

static void art_diamond(void){
  cv_medal(0x04123A,0x123E92,0x74B0FF);
  const float s=0.80f, ox=46, oy=47;
  #define DP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-50)*s)}
  pt_t all[5]={DP(28,18),DP(64,18),DP(84,38),DP(46,86),DP(8,38)};
  m_clear(cvm3); m_poly(cvm3,all,5);
  m_dilate(cvm3,cvm2,U(2.2f*s));
  m_dilate(cvm2,cvm,U(1.6f*s));
  cv_fill_mask_at(cvm,(int)U(0.8f),(int)U(1.6f),0x000000,120);
  cv_fill_mask(cvm,0x000814,0x000814,255);
  mat_t ch={CHROMEFACE,5,1.8f*s,0.95f,2,1.0f,40.0f,0,0,0,0,1.0f};
  cv_shade(cvm2,&ch,255);
  /* crown: table edge T0..T3, girdle G0..G4 */
  pt_t T[4]={DP(28,18),DP(40,18),DP(52,18),DP(64,18)};
  pt_t Gd[5]={DP(8,38),DP(27,38),DP(46,38),DP(65,38),DP(84,38)};
  pt_t C=DP(46,86);
  static const uint32_t crown[7]={0xE6FAFF,0x7CD2F4,0xFFFFFF,0x4AB0E0,0xF4FDFF,0x9ADFF8,0x2E8CC4};
  pt_t tri[7][3]={{T[0],Gd[0],Gd[1]},{T[0],Gd[1],T[1]},{T[1],Gd[1],Gd[2]},{T[1],Gd[2],T[2]},
                  {T[2],Gd[2],Gd[3]},{T[2],Gd[3],T[3]},{T[3],Gd[3],Gd[4]}};
  for(int i=0;i<7;i++) cv_poly(tri[i],3,mixc(crown[i],0xFFFFFF,0.25f),crown[i],255);
  /* pavilion: eight facets meeting at the culet, alternating light */
  static const uint32_t pav[8]={0x0C4E86,0x5EC4F0,0x0A3A6A,0xB8EEFF,0x1C6CA8,0x7AD6F8,0x0A3460,0x3A9AD4};
  for(int i=0;i<8;i++){
    float xa=8+i*9.5f, xb=8+(i+1)*9.5f;
    pt_t q[3]={DP(xa,38),DP(xb,38),C};
    cv_poly(q,3,pav[i],scalec(pav[i],0.55f),255);
  }
  /* fire: a few facets throw colour */
  { pt_t q[3]={DP(36,38),DP(46,38),DP(44,62)}; cv_poly(q,3,0xFFB0E0,0xFFB0E0,70); }
  { pt_t q[3]={DP(56,38),DP(65,38),DP(52,58)}; cv_poly(q,3,0xFFF0A0,0xFFF0A0,80); }
  { pt_t q[3]={DP(18,38),DP(27,38),DP(30,50)}; cv_poly(q,3,0xA0FFD0,0xA0FFD0,70); }
  /* table: a bright band across the top */
  pt_t tb[4]={DP(29,18),DP(63,18),DP(61,21),DP(31,21)};
  cv_poly(tb,4,0xFFFFFF,0xE0F8FF,230);
  /* facet edges catch the light */
  for(int i=0;i<5;i++){
    pt_t a=Gd[i];
    pt_t e[4]={{a.x-U(0.25f),a.y},{a.x+U(0.25f),a.y},{C.x+U(0.15f),C.y},{C.x-U(0.15f),C.y}};
    cv_poly(e,4,0xFFFFFF,0xB0E8FF,120);
  }
  pt_t gl[4]={{Gd[0].x,Gd[0].y-U(0.3f)},{Gd[4].x,Gd[4].y-U(0.3f)},{Gd[4].x,Gd[4].y+U(0.35f)},{Gd[0].x,Gd[0].y+U(0.35f)}};
  cv_poly(gl,4,0xFFFFFF,0xFFFFFF,200);
  #undef DP
  cv_sparkle(U(33),U(24),U(11),255);
  cv_sparkle(U(62),U(56),U(6),200);
  cv_sparkle(U(22),U(40),U(4.5f),180);
}

static void art_bell(void){
  cv_medal(0x3A0408,0xA81222,0xFF8A78);
  cv_rays(U(46),U(44),U(12),U(38),10,0.2f,0xFFD0A0,40);
  const float s=0.72f, ox=46, oy=46;
  #define BP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-48)*s)}
  pt_t p[12]={BP(46,9),BP(60,15),BP(69,30),BP(72,52),BP(78,64),BP(84,72),
              BP(8,72),BP(14,64),BP(20,52),BP(23,30),BP(32,15),BP(46,9)};
  /* clapper first, it hangs below the lip */
  cv_circle(U(ox),U(oy+37*s),U(10.5f*s),0x140800,0x140800,255);
  cv_fruit(U(ox),U(oy+36*s),U(9*s),U(9*s),0x4A2A00,0xD8A020,0xFFF4C0,0xFFB020,0,0,0);
  m_clear(cvm3); m_poly(cvm3,p,12);
  m_rrect(cvm3,U(ox-40*s),U(oy+20*s),U(80*s),U(10*s),U(5*s));
  cv_circle(U(ox),U(oy-40*s),U(5*s),0x140800,0x140800,255);          /* crown loop */
  m_dilate(cvm3,cvm,U(2.0f));
  cv_fill_mask_at(cvm,(int)U(0.8f),(int)U(1.6f),0x000000,110);
  cv_fill_mask(cvm,0x140800,0x140800,255);
  mat_t g={GOLDTURN,7,6.5f,0.75f,1,1.0f,34.0f,0xFFF4C0,0.0f,0,1,1.0f};
  cv_shade(cvm3,&g,255);
  cv_fruit(U(ox),U(oy-40*s),U(3.6f*s),U(3.6f*s),0x6A4206,0xE8B03A,0xFFF4C8,0xFFD560,0,0,0);
  /* the lip is a separate band: re-shade it so it reads as a rolled rim */
  m_clear(cvm3); m_rrect(cvm3,U(ox-40*s),U(oy+20*s),U(80*s),U(10*s),U(5*s));
  mat_t lip={GOLDFACE,6,3.0f,0.8f,1,1.0f,34.0f,0,0,0,0,1.0f};
  cv_shade(cvm3,&lip,255);
  #undef BP
  cv_sparkle(U(34),U(26),U(8),220);
}

static void art_bar(void){
  cv_medal(0x0C0E16,0x2A2E3E,0x7A8098);
  /* the plaque: dark keyline, mirror-bevelled gold face */
  m_clear(cvm3); m_rrect(cvm3,U(8.5f),U(29),U(75),U(34),U(6));
  m_dilate(cvm3,cvm,U(1.6f));
  cv_fill_mask_at(cvm,(int)U(1.0f),(int)U(2.0f),0x000000,150);
  cv_fill_mask(cvm,0x2A1602,0x2A1602,255);
  mat_t g=mat_gold(4.0f);
  cv_shade(cvm3,&g,255);
  /* engraved border line inside the plaque */
  m_clear(cvm2); m_rrect(cvm2,U(12.5f),U(33),U(67),U(26),U(3.5f));
  m_clear(cvm3); m_rrect(cvm3,U(13.4f),U(33.9f),U(65.2f),U(24.2f),U(3.0f));
  m_sub(cvm2,cvm3);
  cv_fill_mask(cvm2,0x6A4204,0x8A5A0A,200);
  /* the word: raised black lacquer with a light keyline */
  static const uint32_t ink[4]={0x4A4A58,0x16161E,0x050508,0x000000};
  m_clear(cvm); m_text(cvm,"BAR",U(46),U(46.5f),U(3.05f),0.62f);
  m_dilate(cvm,cvm2,U(1.1f));
  cv_fill_mask_at(cvm2,(int)U(0.3f),(int)U(0.6f),0x3A2000,200);
  cv_fill_mask(cvm2,0xFFF8DC,0xE8C060,255);
  mat_t k={ink,4,1.8f,0.55f,0,1.0f,40.0f,0,0,0,0,1.0f};
  m_copy(cvm3,cvm);
  cv_shade(cvm3,&k,255);
  cv_sparkle(U(15),U(33),U(7),230);
}

static void art_grapes(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  stem(U(46),U(24),U(52),U(11),U(2),U(1.7f));
  leaf(U(61),U(17),U(13),U(6.5f));
  static const float gp[11][2]={{31,32},{46,29},{61,32},{38,43},{54,43},{30,54},{46,55},{62,54},
                                {38,66},{54,66},{46,77}};
  for(int i=0;i<11;i++){
    float x=U(gp[i][0]), y=U(gp[i][1]), r=U(i==10?8.2f:8.9f);
    cv_circle(x+U(0.6f),y+U(1.2f),r+U(0.6f),0x0A0214,0x0A0214,200);
    cv_fruit(x,y,r,r,0x1A0430,0x7426BC,0xE2B0FF,0xB040FF,0,0,0);
    /* the bloom: a dusty haze on the lit side */
    cv_circle(x-r*0.25f,y-r*0.2f,r*0.62f,0xE8D8FF,0xE8D8FF,26);
  }
}

static void art_orange(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  cv_circle(U(47),U(52),U(31.5f),0x140600,0x140600,255);
  cv_fruit(U(46),U(50),U(30),U(29),0x6A2000,0xFF7C12,0xFFE2A8,0xFF4A00,0.030f,1.4f,11u);
  cv_ellipse(U(46),U(22.5f),U(5),U(2.6f),0x3A2004,0x6A3A08,255);               /* nub  */
  cv_circle(U(46),U(21.8f),U(2.2f),0x8AA030,0x4A6010,255);
  leaf(U(59),U(19),U(12),U(5.5f));
}

static void art_plum(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  cv_ellipse(U(47),U(54),U(28.5f),U(30),0x0A0214,0x0A0214,255);
  cv_fruit(U(46),U(52),U(27),U(29),0x14032A,0x6A1C9A,0xE8B0FF,0xC030C0,0,0,0);
  for(int i=0;i<=40;i++){                                                 /* seam */
    float t=i/40.0f, y=U(25+t*52), x=U(46)-sinf(t*3.14159f)*U(9);
    cv_circle(x,y,U(1.2f),0x24053A,0x24053A,110);
    cv_circle(x-U(1.1f),y,U(0.6f),0xE0B8FF,0xE0B8FF,60);
  }
  cv_ellipse(U(34),U(40),U(9),U(13),0xF0E0FF,0xF0E0FF,40);              /* bloom */
  stem(U(46),U(26),U(49),U(14),U(1),U(1.6f));
  leaf(U(58),U(17),U(11),U(5));
}

static void art_cherry(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  stem(U(33),U(46),U(51),U(15),U(-5),U(1.8f));
  stem(U(60),U(52),U(52),U(15),U(5),U(1.7f));
  leaf(U(64),U(18),U(13),U(6));
  cv_circle(U(34),U(58),U(19.5f),0x100004,0x100004,255);
  cv_fruit(U(33),U(56),U(18.5f),U(18),0x3A0008,0xD81030,0xFFA0A8,0xFF2040,0,0,0);
  cv_ellipse(U(33),U(40.5f),U(3.5f),U(1.6f),0x3A0008,0x3A0008,120);      /* dimple */
  cv_circle(U(61),U(64),U(18.5f),0x100004,0x100004,255);
  cv_fruit(U(60),U(62),U(17.5f),U(17),0x3A0008,0xD81030,0xFFA0A8,0xFF2040,0,0,0);
  cv_ellipse(U(60),U(47),U(3.2f),U(1.5f),0x3A0008,0x3A0008,120);
}

static void art_lemon(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  cv_ellipse(U(47),U(53),U(35),U(25),0x140E00,0x140E00,255);
  cv_fruit(U(14),U(51),U(6),U(4.5f),0x5A4000,0xE8B808,0xFFF6B0,0xFFD000,0,0,0);  /* nubs */
  cv_fruit(U(78),U(51),U(6),U(4.5f),0x5A4000,0xE8B808,0xFFF6B0,0xFFD000,0,0,0);
  cv_fruit(U(46),U(51),U(32),U(23),0x4A3000,0xE0AC00,0xFFF4A8,0xFF9800,0.016f,1.9f,5u);
  leaf(U(62),U(29),U(11),U(5));
}

/*  A 3-D star: every arm is a ridge, so each arm is two flat facets,
 *  one toward the light and one away.  Lit per facet from its real
 *  plane, and reflecting the gold studio.                             */
static void star3d(float cx,float cy,float ro,float ri,float hgt){
  pt_t P[10];
  for(int i=0;i<10;i++){
    float a=-TAU/4+TAU*i/10.0f, r=(i&1)?ri:ro;
    P[i].x=cx+cosf(a)*r; P[i].y=cy+sinf(a)*r;
  }
  m_clear(cvm3); m_poly(cvm3,P,10);
  m_dilate(cvm3,cvm2,U(2.0f));
  m_dilate(cvm2,cvm,U(1.4f));
  cv_fill_mask_at(cvm,(int)U(0.8f),(int)U(1.8f),0x000000,130);
  cv_fill_mask(cvm,0x1A0C00,0x1A0C00,255);
  mat_t g=mat_gold(1.6f);
  cv_shade(cvm2,&g,255);
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ);
  for(int i=0;i<10;i++){
    pt_t a=P[i], b=P[(i+1)%10];
    /* facet (centre, a, b), centre raised by hgt */
    float ux=a.x-cx, uy=a.y-cy, uz=-hgt, vx=b.x-cx, vy=b.y-cy, vz=-hgt;
    float nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
    if(nz<0){ nx=-nx; ny=-ny; nz=-nz; }
    float nl=sqrtf(nx*nx+ny*ny+nz*nz); nx/=nl; ny/=nl; nz/=nl;
    float ndl=(nx*LX+ny*LY+nz*LZ)/ll;
    uint32_t c=env_map(env_up(nx,ny,nz),1);
    c=mixc(c,ramp(GOLDFACE,6,0.4f),0.35f);
    c = ndl>0.55f ? mixc(c,0xFFFFFF,clampf((ndl-0.55f)*1.2f,0,0.6f)) : scalec(c,clampf(0.55f+ndl*0.8f,0.35f,1));
    pt_t t[3]={{cx,cy},a,b};
    cv_poly(t,3,c,scalec(c,0.92f),255);
  }
  /* the ridges catch a line of light */
  for(int i=0;i<10;i+=2){
    pt_t a=P[i];
    float dx=a.x-cx, dy=a.y-cy, l=sqrtf(dx*dx+dy*dy), px2=-dy/l*U(0.35f), py2=dx/l*U(0.35f);
    pt_t e[4]={{cx+px2,cy+py2},{a.x,a.y},{cx-px2,cy-py2},{cx,cy}};
    cv_poly(e,4,0xFFFFFF,0xFFF0C0,i==8||i==0?150:70);
  }
}

static void art_star(void){
  cv_medal(0x03301A,0x0E8A48,0x80FFB8);
  cv_rays(U(46),U(42),U(12),U(38),16,0.19f,0xF4FFB0,110);
  star3d(U(46),U(40),U(31),U(13),U(22));
  cv_sparkle(U(46),U(10.5f),U(9),255);
  cv_sparkle(U(73),U(31),U(5),200);
  ribbon(71.5f,70,"SCATTER",0xE8FFE8,0x2AAA5A,0x02240E);
}

static void art_crown(void){
  cv_medal(0x24044A,0x6A18B0,0xD898FF);
  cv_rays(U(46),U(42),U(12),U(38),14,0.0f,0xF4D0FF,90);
  const float s=0.72f, ox=46, oy=43;
  #define CP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-48)*s)}
  /* velvet cap behind the points */
  { pt_t v[8]={CP(16,54),CP(20,34),CP(32,26),CP(46,22),CP(60,26),CP(72,34),CP(76,54),CP(46,58)};
    m_clear(cvm3); m_poly(cvm3,v,8);
    static const uint32_t vel[3]={0xE0304A,0x9A0A22,0x4A0010};
    mat_t vm=mat_candy(vel,3,5.0f); vm.spec=0.3f; vm.metal=0;
    cv_shade(cvm3,&vm,255); }
  pt_t p[7]={CP(12,76),CP(80,76),CP(82,24),CP(63,46),CP(46,14),CP(29,46),CP(10,24)};
  m_clear(cvm3); m_poly(cvm3,p,7);
  m_rrect(cvm3,U(ox-36*s),U(oy+(62-48)*s),U(72*s),U(16*s),U(3*s));
  m_dilate(cvm3,cvm2,U(1.6f));
  cv_fill_mask_at(cvm2,(int)U(0.8f),(int)U(1.6f),0x000000,120);
  cv_fill_mask(cvm2,0x140C00,0x140C00,255);
  mat_t g=mat_gold(3.4f);
  cv_shade(cvm3,&g,255);
  /* the band re-shaded as a rolled rim, then the jewels */
  m_clear(cvm3); m_rrect(cvm3,U(ox-36*s),U(oy+(62-48)*s),U(72*s),U(16*s),U(3*s));
  mat_t band={GOLDTURN,7,3.0f,0.7f,1,1.0f,30.0f,0,0,0,0,1.0f};
  band.horiz=1;
  cv_shade(cvm3,&band,255);
  cv_gem(U(ox-19*s),U(oy+22*s),U(5.4f*s),0x4A0010,0xE02040,0xFFB0B8);
  cv_gem(U(ox),      U(oy+22*s),U(6.0f*s),0x003A14,0x18B048,0xB0FFC8);
  cv_gem(U(ox+19*s),U(oy+22*s),U(5.4f*s),0x001A4A,0x2060D8,0xB0D0FF);
  cv_fruit(U(ox-36*s),U(oy-25*s),U(5*s),U(5*s),0x8E6208,0xF0D060,0xFFFCE8,0xFFD060,0,0,0);
  cv_fruit(U(ox),      U(oy-35*s),U(5.4f*s),U(5.4f*s),0x8E6208,0xF0D060,0xFFFCE8,0xFFD060,0,0,0);
  cv_fruit(U(ox+36*s),U(oy-25*s),U(5*s),U(5*s),0x8E6208,0xF0D060,0xFFFCE8,0xFFD060,0,0,0);
  #undef CP
  cv_sparkle(U(46),U(15),U(8),240);
  ribbon(71.5f,52,"BONUS",0xF8E0FF,0x9A40E0,0x24043E);
}

static void art_jackpot(void){
  cv_medal(0x3A0000,0xB01414,0xFF7A60);
  cv_rays(U(46),U(46),U(10),U(38),12,0.26f,0xFFF0A0,120);
  static const uint32_t gl[5]={0xFFFCE8,0xFFE070,0xF0A818,0xB86A08,0x7A4204};
  cv_label("JACK",U(46),U(34),U(3.15f),gl,5,0x2A0800,U(1.5f),2.2f);
  cv_label("POT", U(46),U(59),U(3.15f),gl,5,0x2A0800,U(1.5f),2.2f);
  cv_sparkle(U(22),U(24),U(8),240);
  cv_sparkle(U(70),U(66),U(6),200);
}

/*  A holographic foil: the hue turns with the angle round the disc and
 *  shifts with the radius, the way a rainbow foil does under a light. */
static void cv_holo(float cx,float cy,float r,int alpha){
  int x0=(int)(cx-r), x1=(int)(cx+r+1), y0=(int)(cy-r), y1=(int)(cy+r+1);
  static const uint32_t hue[7]={0xFF4040,0xFFB020,0xF0FF40,0x40FF90,0x40D0FF,0x7050FF,0xFF40D0};
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float dx=x+0.5f-cx, dy=y+0.5f-cy, d=sqrtf(dx*dx+dy*dy);
    if(d>r) continue;
    float h=fast_atan2(dy,dx)/TAU+0.5f+d/r*0.35f;
    h-=floorf(h);
    float f=h*7.0f; int i=(int)f; if(i>6) i=6;
    uint32_t c=mixc(hue[i],hue[(i+1)%7],f-i);
    float band=0.55f+0.45f*sinf(d/r*14.0f+fast_atan2(dy,dx)*2.0f);
    cv_px(x,y,c,(int)(alpha*band));
  }
}

static void art_ult(void){
  cv_medal(0x10101E,0x505470,0xE8ECFF);
  cv_holo(U(46),U(46),U(37.5f),96);
  cv_rays(U(46),U(46),U(10),U(38),16,0.0f,0xFFFFFF,70);
  /* one platinum seven, so it cannot be mistaken for the red wild */
  static const uint32_t plat[5]={0xFFFFFF,0xE4F2FF,0x9CC8F0,0x4A80B8,0x1A3A60};
  seven_shape(46,51,0.60f,plat,5,1);
  /* a small gold crown perched on top */
  { const float s=0.30f, ox=46, oy=18.5f;
    #define UP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-48)*s)}
    pt_t p[7]={UP(13,76),UP(79,76),UP(79,26),UP(63,47),UP(46,19),UP(29,47),UP(13,26)};
    m_clear(cvm3); m_poly(cvm3,p,7);
    m_dilate(cvm3,cvm2,U(1.2f));
    cv_fill_mask(cvm2,0x140C00,0x140C00,255);
    mat_t g=mat_gold(1.6f);
    cv_shade(cvm3,&g,255);
    cv_fruit(U(ox-33*s),U(oy-24*s),U(5*s),U(5*s),0x8E6208,0xF0D060,0xFFFCE8,0xFFD060,0,0,0);
    cv_fruit(U(ox),      U(oy-31*s),U(5*s),U(5*s),0x8E6208,0xF0D060,0xFFFCE8,0xFFD060,0,0,0);
    cv_fruit(U(ox+33*s),U(oy-24*s),U(5*s),U(5*s),0x8E6208,0xF0D060,0xFFFCE8,0xFFD060,0,0,0);
    #undef UP
  }
  cv_sparkle(U(62),U(34),U(9),255);
  ribbon(74.5f,78,"ULTIMATE",0xFFFFFF,0x9AB0F0,0x0A1030);
}

/* ── uniform outer contour ─────────────────────────────────────────
 *  On a bright reel band a symbol needs a hard dark edge or it dissolves
 *  into the white.  Rather than hand-drawing one per symbol, the alpha
 *  channel is box-blurred and thresholded: that is a cheap signed-distance
 *  field, so the contour sits at a constant distance from the silhouette
 *  whatever its shape, with a narrow ramp across the threshold for
 *  antialiasing.  The art is then composited back over it.
 * ---------------------------------------------------------------- */
static void add_contour(spr_t*s,int rad,uint32_t col,int alpha){
  if(!s->px||!s->w||!s->h) return;
  int w=s->w,h=s->h,n=w*h;
  uint16_t *a0=(uint16_t*)malloc(n*2), *a1=(uint16_t*)malloc(n*2);
  if(!a0||!a1){ free(a0); free(a1); return; }
  for(int i=0;i<n;i++) a0[i]=s->px[i*4+3];

  /* separable box blur — the distance field */
  int k=rad*2+1;
  for(int y=0;y<h;y++){
    int acc=0;
    for(int x=-rad;x<=rad;x++) acc += a0[y*w+clampi(x,0,w-1)];
    for(int x=0;x<w;x++){
      a1[y*w+x]=(uint16_t)(acc/k);
      acc -= a0[y*w+clampi(x-rad,0,w-1)];
      acc += a0[y*w+clampi(x+rad+1,0,w-1)];
    }
  }
  for(int x=0;x<w;x++){
    int acc=0;
    for(int y=-rad;y<=rad;y++) acc += a1[clampi(y,0,h-1)*w+x];
    for(int y=0;y<h;y++){
      a0[y*w+x]=(uint16_t)(acc/k);
      acc -= a1[clampi(y-rad,0,h-1)*w+x];
      acc += a1[clampi(y+rad+1,0,h-1)*w+x];
    }
  }

  uint8_t*out=(uint8_t*)calloc(n,4);
  if(!out){ free(a0); free(a1); return; }
  const float lo=14.0f, hi=42.0f;              /* threshold ramp */
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  for(int i=0;i<n;i++){
    float t=(a0[i]-lo)/(hi-lo);
    int oa=(int)(clampf(t,0,1)*alpha);
    if(oa<=0) continue;
    out[i*4]=(uint8_t)cr; out[i*4+1]=(uint8_t)cg; out[i*4+2]=(uint8_t)cb;
    out[i*4+3]=(uint8_t)oa;
  }
  /* art over contour, straight alpha */
  for(int i=0;i<n;i++){
    int sa=s->px[i*4+3];
    if(!sa) continue;
    if(sa>=255){ memcpy(out+i*4,s->px+i*4,4); continue; }
    int da=out[i*4+3];
    int oa=sa + da*(255-sa)/255;
    if(oa<=0){ out[i*4+3]=0; continue; }
    for(int c=0;c<3;c++)
      out[i*4+c]=(uint8_t)((s->px[i*4+c]*sa + out[i*4+c]*da*(255-sa)/255)/oa);
    out[i*4+3]=(uint8_t)oa;
  }
  free(s->px); s->px=out;
  free(a0); free(a1);
}

/*  Composite a blurred dark silhouette behind the art at a fixed offset.
 *  The shadow used to be a separate blit per symbol per frame — the same
 *  pixel count as the symbol — for a result that never varies.        */
static void bake_shadow(spr_t*s,int ox,int oy,int rad,int alpha){
  if(!s->px||!s->w||!s->h) return;
  int w=s->w+ox, h=s->h+oy, n=w*h;
  uint16_t *a0=(uint16_t*)calloc(n,2), *a1=(uint16_t*)calloc(n,2);
  uint8_t  *out=(uint8_t*)calloc(n,4);
  if(!a0||!a1||!out){ free(a0); free(a1); free(out); return; }
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++)
    a0[(y+oy)*w+(x+ox)] = s->px[((size_t)y*s->w+x)*4+3];

  int k=rad*2+1;
  for(int y=0;y<h;y++){
    int acc=0;
    for(int x=-rad;x<=rad;x++) acc+=a0[y*w+clampi(x,0,w-1)];
    for(int x=0;x<w;x++){
      a1[y*w+x]=(uint16_t)(acc/k);
      acc-=a0[y*w+clampi(x-rad,0,w-1)];
      acc+=a0[y*w+clampi(x+rad+1,0,w-1)];
    }
  }
  for(int x=0;x<w;x++){
    int acc=0;
    for(int y=-rad;y<=rad;y++) acc+=a1[clampi(y,0,h-1)*w+x];
    for(int y=0;y<h;y++){
      a0[y*w+x]=(uint16_t)(acc/k);
      acc-=a1[clampi(y-rad,0,h-1)*w+x];
      acc+=a1[clampi(y+rad+1,0,h-1)*w+x];
    }
  }
  for(int i=0;i<n;i++){
    int sa=a0[i]*alpha/255;
    if(sa>255) sa=255;
    out[i*4]=out[i*4+1]=0; out[i*4+2]=8; out[i*4+3]=(uint8_t)sa;
  }
  /* art over shadow, straight alpha */
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++){
    const uint8_t*sp=s->px+((size_t)y*s->w+x)*4;
    int sa=sp[3];
    if(!sa) continue;
    uint8_t*o=out+((size_t)y*w+x)*4;
    if(sa>=255){ memcpy(o,sp,4); continue; }
    int da=o[3];
    int oa=sa+da*(255-sa)/255;
    if(oa<=0){ o[3]=0; continue; }
    for(int c=0;c<3;c++) o[c]=(uint8_t)((sp[c]*sa + o[c]*da*(255-sa)/255)/oa);
    o[3]=(uint8_t)oa;
  }
  free(s->px); s->px=out; s->w=w; s->h=h;
  free(a0); free(a1);
  spr_bounds(s);
}


/*  A light unsharp mask over the solid interior of a sprite.  The 4x
 *  box filter that makes the edges clean also softens every detail
 *  inside them; this puts the snap back without touching the silhouette. */
static void spr_sharpen(spr_t*s,float k){
  if(!s->px) return;
  int w=s->w, h=s->h;
  uint8_t*o=(uint8_t*)malloc((size_t)w*h*4);
  if(!o) return;
  memcpy(o,s->px,(size_t)w*h*4);
  for(int y=1;y<h-1;y++) for(int x=1;x<w-1;x++){
    const uint8_t*p=s->px+((size_t)y*w+x)*4;
    const uint8_t*l=p-4,*r=p+4,*u=p-w*4,*d=p+w*4;
    if(p[3]<255||l[3]<255||r[3]<255||u[3]<255||d[3]<255) continue;
    for(int c=0;c<3;c++){
      float v=p[c]+k*(4.0f*p[c]-l[c]-r[c]-u[c]-d[c])*0.25f;
      o[((size_t)y*w+x)*4+c]=(uint8_t)clampi((int)(v+0.5f),0,255);
    }
  }
  free(s->px); s->px=o;
}

/*  What a reel at speed shows.  A plain vertical average turns a symbol
 *  to mush; film smears the highlights into streaks instead, so each
 *  column carries some of the brightest thing that passed through it.
 *  The sprite grows by the blur length top and bottom so the smear runs
 *  on into the neighbours, as it does on a real drum.                  */
#define BLURPAD 14
static void make_streak(const spr_t*src,spr_t*dst,int K,float keep,int fade){
  int w=src->w, h=src->h+BLURPAD*2;
  dst->w=w; dst->h=h;
  dst->px=(uint8_t*)calloc((size_t)w*h,4);
  if(!dst->px) return;
  for(int x=0;x<w;x++) for(int y=0;y<h;y++){
    float r=0,g=0,b=0,a=0,wsum=0; int best=-1, bl=-1;
    for(int k=-K;k<=K;k++){
      int yy=y-BLURPAD+k;
      float wt=1.0f-fabsf((float)k)/(K+1);
      wsum+=wt;
      if(yy<0||yy>=src->h) continue;
      const uint8_t*p=src->px+((size_t)yy*w+x)*4;
      if(!p[3]) continue;
      r+=p[0]*p[3]*wt; g+=p[1]*p[3]*wt; b+=p[2]*p[3]*wt; a+=p[3]*wt;
      int lum=p[0]*3+p[1]*6+p[2];
      if(p[3]>200 && lum>bl){ bl=lum; best=yy; }
    }
    if(a<=0) continue;
    uint8_t*o=dst->px+((size_t)y*w+x)*4;
    uint32_t c=RGB((int)(r/a),(int)(g/a),(int)(b/a));
    if(best>=0){
      const uint8_t*p=src->px+((size_t)best*w+x)*4;
      c=mixc(c,RGB(p[0],p[1],p[2]),keep);
    }
    o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
    o[3]=(uint8_t)clampi((int)(a/wsum*fade/255.0f),0,255);
  }
  spr_bounds(dst);
}

/* build the crisp sprites, their motion-blurred twins, and the glow */
static void art_coin(void);     /* w7_hold.c  */
static void art_wheel(void);    /* w7_wheel.c */
#define NFLAME 4
static spr_t symfl[NFLAME];    /* the wild with its flames at four phases */
static spr_t symb2[NSYM];      /* the long streak, reel at full speed    */
static spr_t pickEmblem;       /* a red 7 on a gold medal: the pick panels */

/*  The same canvas resolved at twice the size, for the attract loop's
 *  close-ups: sharper than magnifying the 106px sprite, and a plain blit
 *  at run time rather than a scaled one.                              */
static void cv_resolve2(spr_t*s){
  const int K=SS/2, W=CANW/K, H=CANH/K;
  s->w=W; s->h=H;
  s->px=(uint8_t*)calloc((size_t)W*H,4);
  if(!s->px){ s->w=s->h=0; return; }
  for(int y=0;y<H;y++) for(int x=0;x<W;x++){
    int r=0,g=0,b=0,a=0;
    for(int j=0;j<K;j++) for(int i=0;i<K;i++){
      uint8_t*p=canvas+(((y*K+j)*CANW)+(x*K+i))*4;
      int pa=p[3];
      r+=p[0]*pa; g+=p[1]*pa; b+=p[2]*pa; a+=pa;
    }
    uint8_t*o=s->px+((size_t)y*W+x)*4;
    if(a>0){ o[0]=(uint8_t)(r/a); o[1]=(uint8_t)(g/a); o[2]=(uint8_t)(b/a); }
    o[3]=(uint8_t)(a/(K*K));
  }
}
static spr_t symBig[NSYM];     /* double-size close-ups, feature symbols only */

static void render_symbol(spr_t*s,void(*art)(void),int glass,int idx){
  cv_clear(); art();
  if(glass) cv_glass();
  if(idx>=0 && idx<NSYM && (idx==SY_SEVEN||idx>=SY_STAR)){
    cv_resolve2(&symBig[idx]);
    spr_sharpen(&symBig[idx],0.4f);
    add_contour(&symBig[idx], SYMW/15, 0x0A0510, 255);
    spr_bounds(&symBig[idx]);
  }
  cv_resolve(s);
  spr_sharpen(s,0.55f);
  add_contour(s, SYMW/30, 0x0A0510, 255);   /* hard dark edge */
  spr_bounds(s);
}

static void build_sprites(void){
  void(*art[NSYM])(void) = { art_seven,art_diamond,art_bell,art_bar,art_grapes,
                             art_orange,art_plum,art_cherry,art_lemon,
                             art_star,art_crown,art_jackpot,art_ult,
                             art_coin,art_wheel };
  /* the coin and wheel belong to their modules, which light their own */
  for(int i=0;i<NSYM;i++) render_symbol(&sym[i],art[i],i!=SY_COIN && i!=SY_WHEEL,i);
  for(int k=0;k<NFLAME;k++){
    flamePhase=0.19f+k*0.23f;
    render_symbol(&symfl[k],art_seven,1,-1);
  }
  flamePhase=0.0f;
  { static const uint32_t red[5]={0xFF8A70,0xFF3A28,0xE0101C,0x980614,0x4A020A};
    cv_clear(); cv_medal(0x6A4206,0xE8B03A,0xFFF4C8);
    cv_rays(U(46),U(46),U(10),U(38),12,0.1f,0xFFFFFF,60);
    seven_shape(46,48,0.68f,red,5,1);
    cv_glass(); cv_resolve(&pickEmblem); spr_sharpen(&pickEmblem,0.55f);
    add_contour(&pickEmblem,SYMW/30,0x0A0510,255); spr_bounds(&pickEmblem);
    bake_shadow(&pickEmblem,SYMW/25,SYMH/20,4,200); }
  /* the smears are built from the un-shadowed art, before the shadow is
     baked in: a moving symbol does not cast a crisp shadow */
  for(int i=0;i<NSYM;i++){
    make_streak(&sym[i],&symb[i], 7,0.22f,225);
    make_streak(&sym[i],&symb2[i],13,0.34f,222);
  }
  /* cast shadows are composited straight into the symbol sprites: same
     look, half the per-frame blits */
  for(int i=0;i<NSYM;i++) bake_shadow(&sym[i], SYMW/25, SYMH/20, 4, 200);
  for(int k=0;k<NFLAME;k++) bake_shadow(&symfl[k], SYMW/25, SYMH/20, 4, 200);

  /* radial alpha mask for the cell washes */
  #define WASHR ((int)(CW*0.58f))
  washspr.w=WASHR*2; washspr.h=WASHR*2;
  washspr.px=(uint8_t*)calloc((size_t)washspr.w*washspr.h,4);
  if(washspr.px){
    for(int y=0;y<washspr.h;y++) for(int x=0;x<washspr.w;x++){
      float dx=(x-WASHR+0.5f)/WASHR, dy=(y-WASHR+0.5f)/WASHR;
      float d=sqrtf(dx*dx+dy*dy);
      float v=clampf(1.0f-d,0,1); v=v*v;
      uint8_t*o=washspr.px+((size_t)y*washspr.w+x)*4;
      o[0]=o[1]=o[2]=255; o[3]=(uint8_t)(v*255);
    }
    spr_bounds(&washspr);
  }

  /* soft radial glow */
  glowspr.w=SYMW; glowspr.h=SYMH;
  glowspr.px=(uint8_t*)calloc(SYMW*SYMH,4);
  if(glowspr.px) for(int y=0;y<SYMH;y++) for(int x=0;x<SYMW;x++){
    float dx=(x-SYMW*0.5f)/(SYMW*0.5f), dy=(y-SYMH*0.5f)/(SYMH*0.5f);
    float d=sqrtf(dx*dx+dy*dy);
    float v=clampf(1.0f-d,0,1); v=v*v;
    uint8_t*o=glowspr.px+(y*SYMW+x)*4;
    o[0]=255; o[1]=225; o[2]=140; o[3]=(uint8_t)(v*255);
  }
  spr_bounds(&glowspr);
}

/* ═══ ADJACENT WAYS ═══════════════════════════════════════════════
 *  A win starts on reel 1 and steps reel by reel to the right, each
 *  symbol in the same row as the last or one row up or down.  It never
 *  steps within a reel and never back to the left.  Three, four or five
 *  reels pay, and where a reel offers two connecting symbols each path is
 *  its own way, so the pay multiplies by the number of paths.
 *  Values are per 10 credits of total bet, so a win is PAY * ways * bet / 10
 *  (ways weighted by the wilds).  Every bet on the ladder is a multiple of
 *  10, so that is always exact.
 *  Tuned against the Monte-Carlo in src/sim.c, which runs this very
 *  evaluator over millions of spins.
 * ================================================================= */
#define MINCHAIN 3
static const char*BANDNAME[3]={"3 REELS","4 REELS","5 REELS"};

static const int PAY[NSYM][6] = {
/*                 0  1  2    3    4    5   reels */
/* SEVEN   */    { 0, 0, 0,   4,  15, 148 },
/* DIAMOND */    { 0, 0, 0,   3,   9,  42 },
/* BELL    */    { 0, 0, 0,   2,   6,  25 },
/* BAR     */    { 0, 0, 0,   2,   5,  17 },
/* GRAPES  */    { 0, 0, 0,   1,   3,  11 },
/* ORANGE  */    { 0, 0, 0,   1,   3,   9 },
/* PLUM    */    { 0, 0, 0,   1,   2,   5 },
/* CHERRY  */    { 0, 0, 0,   1,   2,   4 },
/* LEMON   */    { 0, 0, 0,   1,   2,   3 },
/* STAR    */    { 0, 0, 0,   0,   0,   0 },  /* scatter, paid apart */
/* CROWN   */    { 0, 0, 0,   0,   0,   0 },  /* bonus trigger       */
/* JACKPOT */    { 0, 0, 0,   0,   0,   0 },  /* progressive trigger */
/* ULT     */    { 0, 0, 0,   0,   0,   0 },  /* progressive trigger */
/* COIN    */    { 0, 0, 0,   0,   0,   0 },  /* hold & spin trigger */
/* WHEEL   */    { 0, 0, 0,   0,   0,   0 },  /* wheel bonus trigger */
};

static const int SCATPAY[6] = {0,0,0,2,10,50};   /* x total bet        */

#define STRIPLEN 96
static uint8_t strip[NREEL][STRIPLEN];

/* ── reel strips ───────────────────────────────────────────────────
 *  Symbols are laid down in STACKS, not as loose singles.  On a cluster
 *  grid the stacks are what make clusters possible at all: a run of
 *  three cherries on one reel beside a run of two on the next is a
 *  five-cluster, and the pay table is tuned around how often that
 *  happens.  No two runs of the same symbol may touch, so a stack is
 *  never taller than the table says.
 *
 *  JACKPOT lives on reels 2, 3 and 4 only.  Confining it to a band of
 *  fifteen cells is what makes a seven-cluster so much rarer than a
 *  five-cluster: there is simply less room.  Reel 3 carries one stack of
 *  three, the reels either side a stack of two apiece, so the three
 *  tiers are reachable and the test hooks can land them deliberately.
 *  ULTIMATE is a single symbol on every reel, so five touching means all
 *  five reels showing theirs in a chain — which is what a 1,000,000
 *  seed has to cost.
 * ---------------------------------------------------------------- */
/* Editable rather than const so the simulator can try alternatives in
   place; the shipped values are the ones written here.                */
static uint8_t CNT[3][NSYM] = {
/*  COIN (hold & spin) lands on every reel as singles; WHEEL lands on
 *  reels 2, 3 and 4 only, one showing per reel at most.  COIN is 8 / 7 / 6 (outer / 2,4 / middle): six coins on
 *  25 cells need a reel showing two, and singles only share a window
 *  where the shuffle happens to put two close, so the trigger rate is a
 *  property of the LAYOUT as much as the count - 1 in ~246 spins with
 *  the shipped strips (HOLD & SPIN), 1 in ~387 for the WHEEL.  Any change to this table reshuffles them: re-run
 *  w7sim and check the hold & spin line.                              */
/*        7   D   B  BAR  GR  OR  PL  CH  LE  ST  CR  JP  UL  CO  WH  */
  {       2,  8,  9, 10, 11, 11, 12, 10,  8,  2,  3,  0,  2,  8,  0 },  /* reels 1,5 */
  {       2,  8,  9, 10, 11, 11, 11, 10,  9,  1,  0,  3,  1,  7,  3 },  /* reels 2,4 */
  {       2,  8,  9, 10, 10, 11, 11, 10,  8,  2,  3,  3,  1,  6,  2 },  /* reel 3    */
};
static uint8_t STK[3][NSYM] = {
  {       1,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1 },
  {       1,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  2,  1,  1,  1 },
  {       1,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  3,  1,  1,  1 },
};

static void build_strips(void){
  uint32_t rs = 0x5EED77u;                 /* strips must be stable    */
  #define RND(n) (rs^=rs<<13, rs^=rs>>17, rs^=rs<<5, (int)(rs%(uint32_t)(n)))

  for(int r=0;r<NREEL;r++){
    int tbl = (r==2)?2:((r==1||r==3)?1:0);
    const uint8_t*cnt=CNT[tbl], *stk=STK[tbl];
    uint8_t rsym[STRIPLEN], rlen[STRIPLEN];
    int nr=0;
    for(int k=0;k<NSYM;k++){
      int left=cnt[k], st=stk[k];
      while(left>0){
        int L = left>=st ? st : left;
        rsym[nr]=(uint8_t)k; rlen[nr]=(uint8_t)L; nr++;
        left-=L;
      }
    }
    for(int i=nr-1;i>0;i--){             /* shuffle the runs           */
      int j=RND(i+1);
      uint8_t a1=rsym[i]; rsym[i]=rsym[j]; rsym[j]=a1;
      uint8_t b1=rlen[i]; rlen[i]=rlen[j]; rlen[j]=b1;
    }
    /* Spread the single-cell specials evenly round the strip.  Left to the
       shuffle, two crowns could land within a window of each other, and
       one reel showing two of them made the pick bonus land twice as
       often as the pay table implied.  Even spacing means a window shows
       at most one, so the trigger odds are a property of the count.    */
    {
      static const uint8_t SPC[4]={SY_STAR,SY_CROWN,SY_ULT,SY_WHEEL};
      uint8_t placed[STRIPLEN]; memset(placed,0,sizeof placed);
      #define ISSPC(v) ((v)==SY_STAR||(v)==SY_CROWN||(v)==SY_ULT||(v)==SY_WHEEL)
      for(int q=0;q<4;q++){
        int sy=SPC[q], n=0;
        for(int i=0;i<nr;i++) if(rsym[i]==sy) n++;
        for(int k=0;k<n;k++){
          int want=(int)(((long)k*nr)/n + q*5)%nr;
          for(int t=0;t<nr && (placed[want] || (ISSPC(rsym[want]) && rsym[want]!=sy));t++)
            want=(want+1)%nr;                        /* nudge off a taken slot */
          if(rsym[want]!=sy){
            int from=-1;
            for(int i=0;i<nr;i++) if(rsym[i]==sy && !placed[i]){ from=i; break; }
            if(from<0) break;
            uint8_t a1=rsym[from]; rsym[from]=rsym[want]; rsym[want]=a1;
            uint8_t b1=rlen[from]; rlen[from]=rlen[want]; rlen[want]=b1;
          }
          placed[want]=1;
        }
      }
      #undef ISSPC
    }
    /* never let two runs of the same symbol touch, or they merge into a
       taller stack than the table asked for (and skew the odds) */
    for(int pass=0;pass<8;pass++)
      for(int i=0;i<nr;i++){
        int j=(i+1)%nr;
        if(rsym[i]!=rsym[j]) continue;
        for(int k=2;k<nr;k++){
          int m=(i+k)%nr;
          if(rsym[m]!=rsym[i] && rsym[m]!=rsym[(i+2)%nr] &&
             rsym[(m+1)%nr]!=rsym[j] && rsym[(m+nr-1)%nr]!=rsym[j]){
            uint8_t a1=rsym[j]; rsym[j]=rsym[m]; rsym[m]=a1;
            uint8_t b1=rlen[j]; rlen[j]=rlen[m]; rlen[m]=b1;
            break;
          }
        }
      }
    int w=0;
    for(int i=0;i<nr && w<STRIPLEN;i++)
      for(int c=0;c<rlen[i] && w<STRIPLEN;c++) strip[r][w++]=rsym[i];
    while(w<STRIPLEN) strip[r][w++]=SY_LEMON;
  }
  #undef RND
}

/* ═══ GAME STATE ══════════════════════════════════════════════════
 *  One flat struct, no pointers, so save states just memcpy.
 * ================================================================= */
enum { ST_ATTRACT, ST_IDLE, ST_SPIN, ST_EVAL, ST_SHOWWIN,
       ST_FSINTRO, ST_BONUS, ST_BONUSEND, ST_PAYTABLE, ST_BROKE,
       ST_JACKPOT, ST_ADDCR,
       ST_HOLD,        /* hold & spin bonus        - w7_hold.c  */
       ST_WHEEL,       /* wheel bonus              - w7_wheel.c */
       ST_GAMBLE,      /* double-or-nothing        - w7_extra.c */
       ST_STORM,       /* 7 STRIKE wild storm plays out - w7_extra.c */
       ST_NSTATES };

#define MAXWINS 12
#define NPICK 9
#define NJP 4

/*  Features a spin has triggered but that have not run yet.  They run
 *  one after another once the spin's own wins have been shown and paid:
 *  see next_feature().  Order: HOLD, WHEEL, PICK, FREE SPINS.         */
#define PEND_HOLD  1
#define PEND_WHEEL 2
#define PEND_PICK  4
#define PEND_FS    8

/*  Feature modules (unity build).  Each header holds that module's
 *  saved state struct and its function prototypes; the matching .c is
 *  included further down, just before render().                      */
#include "w7_fx.h"
#include "w7_hold.h"
#include "w7_wheel.h"
#include "w7_extra.h"

typedef struct {
  int   state;
  float t;                    /* seconds in state                    */
  float idle;                 /* seconds since last input            */

  long long credits;          /* 64-bit: a jackpot at a big bet is big */
  int   betIdx;
  int   grid[NREEL][NROW];

  float rpos[NREEL];          /* strip position, continuous          */
  int   rstate[NREEL];        /* 0 idle 1 spin 2 settle 3 stopped    */
  float rt0[NREEL], rt1[NREEL], ru[NREEL], rdelay[NREEL];
  float spinT;

  int   winTotal, winShown;
  int   winSym[MAXWINS], winCnt[MAXWINS], winAmt[MAXWINS], winWays[MAXWINS];
  int   winWt[MAXWINS];      /* ways after the wild multipliers      */
  uint32_t winMask[MAXWINS]; /* bit r*NROW+row for each cell in the cluster */
  int   nWin, showIdx;
  float showT;

  int   scatCount, bonusCount;
  int   freeSpins, fsMult, fsWon, inFree;
  int   expand[NREEL];      /* reels gone fully wild in free spins */
  int   pickMult;           /* multiplier banked in the pick round */
  float multUp;             /* the meter just climbed: pop it      */
  int   multFrom;           /* what it climbed from                */

  int   pickVal[NPICK], pickKind[NPICK], pickDone[NPICK], pickCur, pickTotal, pickStops;
  float pickT;

  long long jpAcc[NJP];       /* fed in since the tier last hit, milli */
  int   jpWon;                /* tier just won, -1 for none            */
  long long jpAmt;
  uint32_t jpMask;            /* the cells that won it                 */
  float jpT;

  int   addIdx, addFrom;      /* the ADD CREDITS chooser               */
  int   ptPage;               /* 0 pay table, 1 features, 2 more       */

  int   lastWin, banner;
  float bannerT;
  float flash;                /* 0..1 screen flash, limiter-capped   */
  float reelBlur[NREEL];

  int   prevBtn, btn;
  uint32_t seed;

  int   pend;                 /* PEND_* features still to run          */
  int   coinCount, wheelCount;/* trigger counts from evaluate()        */
  hold_state_t  hold;         /* each module's saved state             */
  wheel_state_t wheel;
  extra_state_t extra;
} game_t;

static game_t G;

/* ── coin burst ────────────────────────────────────────────────────
 *  The particles live in w7_fx.c: a pool of spinning coins, sparks,
 *  stars and confetti, cosmetic and outside the save state.  These
 *  three keep their old names so existing callers still work.
 * ---------------------------------------------------------------- */
static void spawn_burst(float x,float y,int n,uint32_t col){ fx_burst_col(x,y,n,col,0); }
static void update_parts(void){}       /* fx_update() moves the particles */
static void draw_parts(void){}         /* fx_draw() draws them            */

/* ═══ THE BET LADDER ════════════════════════════════════
 *  A 1-2-5 ladder from 10 upward with no top rung of its own: BET MORE
 *  keeps climbing as long as the arithmetic stays inside a credit meter.
 *  Every rung is a multiple of 10, so pays - which are quoted per 10
 *  credits - divide exactly at all of them.
 * ================================================================= */
/*  The ceiling is arithmetic, not a rule of the game: at 100,000 a spin
 *  the top jackpot is ten billion and every figure still fits its meter
 *  exactly, so the return is identical at every rung below it.        */
#define BETMAXIDX 12                    /* 100,000 a spin */
static int bet_at(int i){
  static const int m[3]={1,2,5};
  if(i<0) i=0;
  if(i>BETMAXIDX) i=BETMAXIDX;
  long long v=m[i%3];
  for(int e=0;e<=i/3;e++) v*=10;
  return (int)v;
}
#define TOTBET  (bet_at(G.betIdx))
#define NADDS 6
static const int ADDS[NADDS] = {100,250,500,1000,2500,5000};

/*  The feature's three dials.  Variables rather than constants so the
 *  simulator can sweep them without a rebuild; the values here are the
 *  ones that ship.                                                    */
static int FS_AWARD   = 6;     /* free spins for 3+ scatters      */
static int FS_RETRIG  = 3;     /* more for a retrigger            */
static int FS_MAXMULT = 5;     /* the free-spins meter tops out   */
#define WILD_MULT 2            /* every wild in a path doubles it */

/* ═══ PROGRESSIVE JACKPOTS ══════════════════════════════
 *  Four pots, all won from the reels rather than by a hidden roll, so
 *  the player can see what they are chasing:
 *
 *      JACKPOT symbols in a cluster of  5  ->  MINOR
 *                                       6  ->  MAJOR
 *                                       7+ ->  MEGA
 *      ULTIMATE symbols, five touching     ->  ULTIMATE
 *
 *  Each pot is a multiple of the bet you are playing PLUS everything fed
 *  in since that tier last paid.  The bet multiple is the fix for the
 *  thing that made the old build absurd: the pots were seeded at a flat
 *  50 / 500 / 5,000, so a MINOR won on a 10,000 bet paid fifty credits.
 *  Now raising the bet raises all four meters on the spot, and a tier is
 *  always worth the same multiple of what was staked.
 *
 *  It also makes the return honest.  A symbol trigger lands at the same
 *  rate whatever the bet, so a prize proportional to the bet contributes
 *  the same percentage at every rung: the machine now returns the same
 *  figure whether it is played at 10 or at 10,000.
 *
 *  A tenth of every bet feeds the pots, split across the four tiers.
 * ================================================================= */
enum { JP_ULT=0, JP_MEGA, JP_MAJOR, JP_MINOR };
static const int   JP_MULT[NJP] = { 100000, 200, 40, 5 };   /* x total bet   */
static const float JP_RATE[NJP] = { 0.030f, 0.025f, 0.020f, 0.025f }; /* 10% */
static int         JP_NEED[NJP] = { 5, 7, 6, 5 };           /* cluster size  */
static const char* JP_NAME[NJP] = { "ULTIMATE", "MEGA", "MAJOR", "MINOR" };
#define JP_CLAMP 999999999999LL    /* twelve figures, what a meter holds */

/*  What a tier is worth right now: its bet multiple plus the pot.      */
static long long jp_value(int i){
  long long v = (long long)JP_MULT[i]*TOTBET + G.jpAcc[i]/1000;
  return v>JP_CLAMP ? JP_CLAMP : v;
}

static void jp_seed(void){
  /* a machine already in play: each pot carries a random part of what it
     would normally have collected by now */
  for(int i=0;i<NJP;i++)
    G.jpAcc[i] = (long long)(frnd()*0.8f*(float)JP_MULT[i]*(float)TOTBET*1000.0f);
  G.jpWon=-1; G.jpAmt=0; G.jpMask=0;
}

/* grow the pots by this spin's stake */
static void jp_contribute(int bet){
  for(int i=0;i<NJP;i++)
    G.jpAcc[i] += (long long)((double)bet*JP_RATE[i]*1000.0);
}

/* ═══ AUDIO ═══════════════════════════════════════════════════════
 *  The synth, the music and every sound effect live in w7_audio.c
 *  (public calls listed in w7_audio.h).  Included here rather than with
 *  the other modules because update() below calls it.                 */
#include "w7_audio.c"

/* ═══ REEL / EVALUATION ═══════════════════════════════════════════ */
static inline int stripAt(int r,int i){
  int n=i%STRIPLEN; if(n<0) n+=STRIPLEN;
  return strip[r][n];
}
static void snapshot_grid(void){
  for(int r=0;r<NREEL;r++){
    int base=(int)floorf(G.rpos[r]+0.5f);
    for(int row=0;row<NROW;row++) G.grid[r][row]=stripAt(r,base-row);
    G.expand[r]=0;
  }
  /* 7 STRIKE: when a spin was armed at start_spin, the storm drops its
     wilds here, so the simulator sees exactly what the player sees.   */
  extra_on_snapshot();
  /* Expanding wilds: during free spins a single wild takes its whole
     reel, and every reel that expands notches the multiplier meter up
     one.  The meter never falls back during the feature, so a run of
     wilds early is worth chasing for the rest of the spins.  This lives
     here so that the RTP simulator (which includes this file) exercises
     the same code. */
  if(G.inFree){
    int gained=0;
    for(int r=1;r<=3;r++){            /* middle three reels only */
      int has=0;
      for(int row=0;row<NROW;row++) if(G.grid[r][row]==SY_SEVEN) has=1;
      if(has){
        G.expand[r]=1;
        for(int row=0;row<NROW;row++) G.grid[r][row]=SY_SEVEN;
        gained++;
      }
    }
    if(gained && G.fsMult<FS_MAXMULT){
      G.multFrom=G.fsMult;
      G.fsMult += gained;
      if(G.fsMult>FS_MAXMULT) G.fsMult=FS_MAXMULT;
      G.multUp=1.9f;
    }
  }
  /* every LUCKY COIN on the grid gets its value as it lands */
  hold_on_snapshot();
}
static inline int is_wild(int s){ return s==SY_SEVEN; }
static inline int payable(int s){ return s<NPAYSYM; }

/*  Flood fill over the cells set in `ok`, eight-connected, from `start`.
 *  Returns the size and hands back the cells reached.  25 cells fit in
 *  one word, so a cluster is a bitmask and set algebra does the rest.  */
static int flood(uint32_t ok,int start,uint32_t*out){
  uint32_t seen=1u<<start; int stack[NCELL], sp=0, n=0;
  stack[sp++]=start;
  while(sp){
    int c=stack[--sp]; n++;
    int r=c/NROW, row=c%NROW;
    for(int dr=-1;dr<=1;dr++) for(int dw=-1;dw<=1;dw++){
      if(!dr&&!dw) continue;
      int nr=r+dr, nw=row+dw;
      if(nr<0||nr>=NREEL||nw<0||nw>=NROW) continue;
      int d=nr*NROW+nw;
      if(((ok>>d)&1u) && !((seen>>d)&1u)){ seen|=1u<<d; stack[sp++]=d; }
    }
  }
  *out=seen; return n;
}

static void add_win(int s,int len,int ways,long long wt,uint32_t m){
  long long a=(long long)PAY[s][len]*wt*TOTBET/10;
  if(a<=0) return;
  if(a>2000000000LL) a=2000000000LL;          /* one win fits an int */
  int amt=(int)a;
  if(G.nWin<MAXWINS){
    G.winSym[G.nWin]=s; G.winCnt[G.nWin]=len; G.winWays[G.nWin]=ways;
    G.winWt[G.nWin]=(int)wt; G.winAmt[G.nWin]=amt; G.winMask[G.nWin]=m;
    G.nWin++;
  }
  G.winTotal+=amt;
}

/*  Count the paths for one symbol.  ways[r][row] is the number of chains
 *  from reel 1 that end on that cell: a cell that matches inherits the
 *  paths of the three cells beside it on the reel to the left.  The
 *  longest reach pays, times the paths that get there; the cells on any
 *  such path are handed back for the lights.
 *
 *  A second, weighted count rides along in the same pass.  Cells set in
 *  `boost` — the wilds — multiply every path that runs through them by
 *  WILD_MULT, so a win with two wilds in it pays four times over.  Doing
 *  it here rather than afterwards is what makes it exact: paths through
 *  a wild are boosted and paths avoiding it are not, even when both feed
 *  the same cell.  It is the weighted figure that pays.                */
static int score_ways(uint32_t match,uint32_t boost,int*olen,long long*owt,uint32_t*omask){
  int ways[NREEL][NROW]; long long wt[NREEL][NROW];
  int reach=-1;
  for(int r=0;r<NREEL;r++){
    int any=0;
    for(int row=0;row<NROW;row++){
      ways[r][row]=0; wt[r][row]=0;
      if(!((match>>(r*NROW+row))&1u)) continue;
      long long mul = ((boost>>(r*NROW+row))&1u) ? WILD_MULT : 1;
      if(r==0){ ways[r][row]=1; wt[r][row]=mul; }
      else for(int d=-1;d<=1;d++){
        int q=row+d; if(q<0||q>=NROW) continue;
        ways[r][row]+=ways[r-1][q];
        wt[r][row]  +=wt[r-1][q]*mul;
      }
      if(ways[r][row]) any=1;
    }
    if(!any) break;
    reach=r;
  }
  int len=reach+1;
  if(len<MINCHAIN){ *olen=len; *owt=0; *omask=0; return 0; }
  int total=0; uint32_t m=0; long long tw=0;
  for(int row=0;row<NROW;row++) if(ways[reach][row]){
    total+=ways[reach][row]; tw+=wt[reach][row]; m|=1u<<(reach*NROW+row);
  }
  *owt=tw;
  /* walk back: a cell is on a winning path if it matches and feeds a
     marked cell on the reel to its right */
  for(int r=reach-1;r>=0;r--)
    for(int row=0;row<NROW;row++){
      if(!ways[r][row]) continue;
      for(int d=-1;d<=1;d++){
        int q=row+d; if(q<0||q>=NROW) continue;
        if((m>>((r+1)*NROW+q))&1u){ m|=1u<<(r*NROW+row); break; }
      }
    }
  *olen=len; *omask=m;
  return total;
}

/*  Score the grid.  Each paying symbol is scored with the wilds standing
 *  in; a pure run of wilds that shares no cell with another win pays as
 *  sevens.  Progressives are decided here too: the largest JACKPOT
 *  cluster picks the tier, five ULTIMATEs touching take the top pot.   */
static void evaluate(void){
  G.nWin=0; G.winTotal=0;
  uint32_t symm[NSYM]; memset(symm,0,sizeof symm);
  for(int r=0;r<NREEL;r++) for(int row=0;row<NROW;row++)
    symm[G.grid[r][row]] |= 1u<<(r*NROW+row);
  uint32_t wildm=symm[SY_SEVEN], won=0, done, m;
  int len, ways; long long wt;

  for(int s=1;s<NPAYSYM;s++){
    ways=score_ways(symm[s]|wildm,wildm,&len,&wt,&m);
    if(ways>0 && (m & symm[s])){ add_win(s,len,ways,wt,m); won|=m; }
  }
  /* the wild's own chain: the sevens ARE the symbol, so they do not also
     multiply themselves */
  ways=score_ways(wildm,0,&len,&wt,&m);
  if(ways>0 && !(m&won)) add_win(SY_SEVEN,len,ways,wt,m);

  G.scatCount=0; G.bonusCount=0; G.coinCount=0; G.wheelCount=0;
  for(int c=0;c<NCELL;c++){
    if((symm[SY_STAR]>>c)&1u)  G.scatCount++;
    if((symm[SY_CROWN]>>c)&1u) G.bonusCount++;
    if((symm[SY_COIN]>>c)&1u)  G.coinCount++;
    if((symm[SY_WHEEL]>>c)&1u) G.wheelCount++;
  }
  if(G.scatCount>=3) G.winTotal += SCATPAY[G.scatCount>5?5:G.scatCount]*TOTBET;
  if(G.inFree && G.fsMult>1){             /* in 64 bits: x5 on a top-bet win can pass 2^31 */
    long long t=(long long)G.winTotal*G.fsMult;
    G.winTotal=(int)(t>2000000000LL?2000000000LL:t);
  }

  /* ---- progressives ---- */
  G.jpWon=-1; G.jpMask=0;
  int best=0, tier=-1; uint32_t bm=0;
  (void)done;
  done=0;
  for(int c=0;c<NCELL;c++){
    if(!((symm[SY_JACKPOT]>>c)&1u) || ((done>>c)&1u)) continue;
    int n=flood(symm[SY_JACKPOT],c,&m); done|=m;
    if(n>best){ best=n; bm=m; }
  }
  if(best>=JP_NEED[JP_MEGA])       tier=JP_MEGA;
  else if(best>=JP_NEED[JP_MAJOR]) tier=JP_MAJOR;
  else if(best>=JP_NEED[JP_MINOR]) tier=JP_MINOR;
  done=0;
  for(int c=0;c<NCELL;c++){
    if(!((symm[SY_ULT]>>c)&1u) || ((done>>c)&1u)) continue;
    int n=flood(symm[SY_ULT],c,&m); done|=m;
    if(n>=JP_NEED[JP_ULT]){ tier=JP_ULT; bm=m; break; }
  }
  if(tier>=0){
    G.jpWon=tier; G.jpAmt=jp_value(tier); G.jpAcc[tier]=0; G.jpMask=bm;
  }
}

static void start_spin(void){
  if(!G.inFree){
    if(G.credits < TOTBET) return;
    G.credits -= TOTBET;
    jp_contribute(TOTBET);
  }
  extra_on_spin_start();          /* may arm a 7 STRIKE for this spin */
  G.state=ST_SPIN; G.t=0; G.spinT=0;
  G.winTotal=0; G.nWin=0; G.winShown=0; G.lastWin=0; G.banner=0;
  /*  The reels run at half the old speed and stop one at a time with a
   *  long, uneven gap between them - half a second to two seconds - so
   *  every spin has its own rhythm and the last reel is worth waiting
   *  for.  Turbo halves the gaps rather than removing them.  A player in
   *  a hurry can still slam the lot down with START, A or B.          */
  float gap0 = opt_turbo?0.25f:0.50f, gapr = opt_turbo?0.75f:1.50f;
  float d    = opt_turbo?0.40f:0.80f;
  for(int r=0;r<NREEL;r++){
    G.rstate[r]=1;
    G.rdelay[r]=d;
    d += gap0 + frnd()*gapr;
    G.reelBlur[r]=0;
    G.rt1[r]=0;          /* rt1 doubles as "held for anticipation" and the stop target:
                            left at the last target, the hold only ever fired on the
                            first spin after the attract loop */
  }
  sfx_spin_start();
}

/* Panel kinds: 0 credits, 1 multiplier, 2 stop.
   Nine panels: five credit values, one x2, three stops. The round runs
   until the third stop, so a player usually gets four or five picks
   rather than being knocked out on the first one.
   Shared with the RTP simulator so the two cannot drift apart.       */
#define PICK_CREDIT 0
#define PICK_MULT   1
#define PICK_STOP   2

static void bonus_fill_panels(int*kind,int*val){
  static const int pool[5] = { 7, 10, 17, 25, 34 };     /* x bet / 10 */
  int k[NPICK], v[NPICK];
  for(int i=0;i<5;i++){ k[i]=PICK_CREDIT; v[i]=pool[i]*TOTBET/10; }
  k[5]=PICK_MULT;  v[5]=2;
  k[6]=PICK_STOP;  v[6]=0;
  k[7]=PICK_STOP;  v[7]=0;
  k[8]=PICK_STOP;  v[8]=0;
  for(int i=NPICK-1;i>0;i--){            /* shuffle the board */
    int j=irnd(i+1);
    int a=k[i]; k[i]=k[j]; k[j]=a;
    int b=v[i]; v[i]=v[j]; v[j]=b;
  }
  for(int i=0;i<NPICK;i++){ kind[i]=k[i]; val[i]=v[i]; }
}

static void bonus_invalidate(void);
static void begin_bonus(void){
  bonus_invalidate();
  G.state=ST_BONUS; G.t=0; G.pickCur=0; G.pickTotal=0; G.pickStops=0;
  G.pickMult=1;
  bonus_fill_panels(G.pickKind,G.pickVal);
  for(int i=0;i<NPICK;i++) G.pickDone[i]=0;
  sfx_bonus_start();
}

/* ═══ INPUT ═══════════════════════════════════════════════════════ */
enum { B_UP=1,B_DOWN=2,B_LEFT=4,B_RIGHT=8,B_A=16,B_B=32,B_X=64,B_Y=128,
       B_START=256,B_SELECT=512,B_L=1024,B_R=2048 };
static void poll_input(void){
  input_poll_cb();
  int b=0;
  #define RB(id,bit) if(input_state_cb(0,RETRO_DEVICE_JOYPAD,0,id)) b|=bit
  RB(RETRO_DEVICE_ID_JOYPAD_UP,B_UP);      RB(RETRO_DEVICE_ID_JOYPAD_DOWN,B_DOWN);
  RB(RETRO_DEVICE_ID_JOYPAD_LEFT,B_LEFT);  RB(RETRO_DEVICE_ID_JOYPAD_RIGHT,B_RIGHT);
  RB(RETRO_DEVICE_ID_JOYPAD_A,B_A);        RB(RETRO_DEVICE_ID_JOYPAD_B,B_B);
  RB(RETRO_DEVICE_ID_JOYPAD_X,B_X);        RB(RETRO_DEVICE_ID_JOYPAD_Y,B_Y);
  RB(RETRO_DEVICE_ID_JOYPAD_START,B_START);RB(RETRO_DEVICE_ID_JOYPAD_SELECT,B_SELECT);
  RB(RETRO_DEVICE_ID_JOYPAD_L,B_L);        RB(RETRO_DEVICE_ID_JOYPAD_R,B_R);
  #undef RB
  if(dbg_pilot){
    dbg_frame++;
    b=0;
    if(dbg_pilot==2){
      if(dbg_frame==40) b|=B_START;
      if(dbg_frame==80) b|=B_SELECT;
    } else if(dbg_pilot==3){
      /* the first press only leaves the attract loop */
      if(dbg_frame==40||dbg_frame==110) b|=B_START;
    } else if(dbg_pilot==6){
      /* leave the attract loop, then climb the bet ladder */
      if(dbg_frame==40) b|=B_START;
      else if(dbg_frame>60 && dbg_frame<210 && (dbg_frame%12)==0) b|=B_RIGHT;
    } else if(dbg_pilot==5){
      if(dbg_frame==40) b|=B_START;
      if(dbg_frame==80||dbg_frame==160) b|=B_SELECT;
    } else if(dbg_pilot==4){
      if(dbg_frame==40) b|=B_START;
      if(dbg_frame==80) b|=B_Y;
      if(dbg_frame==120||dbg_frame==140) b|=B_RIGHT;
    } else {
      if(dbg_frame==40) b|=B_START;
      else if(dbg_frame>60 && (dbg_frame%70)==0)  b|=B_START;
      else if(dbg_frame>60 && (dbg_frame%70)==35) b|=B_A;
    }
  }
  G.prevBtn=G.btn; G.btn=b;
}
static inline int hit(int m){ return (G.btn&m) && !(G.prevBtn&m); }
static inline int anyhit(void){ return G.btn && !G.prevBtn; }

static float easeOutBack(float x){
  const float c1=1.70158f, c3=c1+1.0f;
  float p=x-1.0f;
  return 1.0f + c3*p*p*p + c1*p*p;
}

static void award(long long amt){
  long long c=(long long)G.credits+amt;
  if(c>JP_CLAMP) c=JP_CLAMP;                 /* what the meter can hold */
  G.credits=c;                               /* was (int)c: a bank past 2^31 went negative */
  G.lastWin=(int)(amt>2000000000LL?2000000000LL:amt);
  if(G.inFree){
    long long f=(long long)G.fsWon+amt;
    G.fsWon=(int)(f>2000000000LL?2000000000LL:f);
  }
}

/* how many scatters, or crowns, are already showing on the reels that
   have stopped — the anticipation hold keys off the larger count */
static int partial_special(void){
  int st=0, cr=0, wh=0;
  for(int r=0;r<NREEL;r++){
    if(G.rstate[r]!=3) continue;
    int base=(int)floorf(G.rpos[r]+0.5f);
    for(int row=0;row<NROW;row++){
      int s=stripAt(r,base-row);
      if(s==SY_STAR) st++;
      if(s==SY_CROWN) cr++;
      if(s==SY_WHEEL) wh++;        /* two wheels down: the third reel is held too */
    }
  }
  int m=st>cr?st:cr;
  return wh>m?wh:m;
}

/* ── the feature queue ─────────────────────────────────────────────
 *  A spin's own wins are shown and paid FIRST, then every feature it
 *  triggered runs in turn.  The old flow jumped straight into the pick
 *  round or the free-spins intro and never paid the triggering spin's
 *  line wins or its scatter pay, although the simulator counted them.
 * ---------------------------------------------------------------- */
static int pend_from_grid(void){
  int p=0;
  if(hold_triggered())  p|=PEND_HOLD;
  if(wheel_triggered()) p|=PEND_WHEEL;
  if(G.bonusCount>=3)   p|=PEND_PICK;
  if(G.scatCount>=3)    p|=PEND_FS;
  return p;
}
static void after_result(void);
static void next_feature(void){
  if(G.pend&PEND_HOLD) { G.pend&=~PEND_HOLD;  hold_begin();  return; }
  if(G.pend&PEND_WHEEL){ G.pend&=~PEND_WHEEL; wheel_begin(); return; }
  if(G.pend&PEND_PICK) { G.pend&=~PEND_PICK;  begin_bonus(); return; }
  if(G.pend&PEND_FS)   { G.pend&=~PEND_FS;
    G.state=ST_FSINTRO; G.t=0; sfx_fs_intro(); return; }
  after_result();
}
/*  A module calls this when its feature is over and its prize has been
 *  paid with award(); the queue carries on from there.                */
static void feature_done(void){ next_feature(); }

static void end_free_spins(void){
  G.inFree=0; G.freeSpins=0;
  G.banner=3; G.bannerT=0;
  G.state=ST_BONUSEND; G.t=0;
}

static void after_result(void){
  /* called once a spin's payout has been handed over */
  if(G.inFree){
    G.freeSpins--;
    if(G.freeSpins>0){ G.state=ST_IDLE; G.t=-0.55f; }
    else end_free_spins();
  } else {
    G.state = (G.credits < bet_at(0)) ? ST_BROKE : ST_IDLE;
    G.t=0;
  }
}

/* the highest rung of the ladder the bank can cover, or -1 */
static int max_affordable(void){
  int k=-1;
  for(int i=0;i<=BETMAXIDX;i++) if(G.credits>=bet_at(i)) k=i;
  return k;
}

static void open_addcr(int from){
  G.addFrom=from; G.addIdx=2;              /* 500, the old default */
  G.state=ST_ADDCR; G.t=0;
  sfx_ui_open();
}

/*  Test hooks: which rows of which reel must show which symbol.  Each
 *  is a genuine reel stop that happens to satisfy the demand, found by
 *  walking the strip, so the whole evaluation path runs for real.
 *  Returns the number of rows demanded (0 = leave this reel alone).   */
static int force_demand(int r,int*symo,int*rows){
  switch(dbg_force){
  case 1:  if(r<3){ *symo=SY_STAR; rows[0]=2; return 1; } return 0;          /* free  */
  case 2:  if(r==0||r==2||r==4){ *symo=SY_CROWN; rows[0]=2; return 1; } return 0;  /* pick */
  case 3:  if(r<3){ *symo=SY_CHERRY; rows[0]=2; return 1; } return 0;            /* win: a 3-reel cherry line */
  case 4:  /* mega: reels 2,3,4 stacks aligned = 2+3+2 */
    if(r==1||r==3){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; return 2; }
    if(r==2){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; rows[2]=3; return 3; }
    return 0;
  case 5:  /* minor: reels 2 and 3 = 2+3 */
    if(r==1){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; return 2; }
    if(r==2){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; rows[2]=3; return 3; }
    return 0;
  case 6:  *symo=SY_ULT; rows[0]=2; return 1;                                  /* ult   */
  case 7:  *symo=SY_COIN; return hold_force_rows(r,rows);                      /* hold  */
  case 8:  if(r>=1&&r<=3){ *symo=SY_WHEEL; rows[0]=2; return 1; } return 0;     /* wheel */
  case 10:  /* fsmult: scatters into free spins, then a wild on reel 3 in
              every free spin, so the multiplier climbs and pops */
    if(!G.inFree){ if(r<3){ *symo=SY_STAR; rows[0]=2; return 1; } return 0; }
    if(r==2){ *symo=SY_SEVEN; rows[0]=2; return 1; }
    return 0;
  }
  return 0;
}

/*  WILD7_FORCE=big|super|megawin|epic: a big win for the presentation
 *  work.  A per-reel demand cannot ask for "a win worth 50x", so when the
 *  first reel settles this searches whole sets of five genuine strip
 *  stops, scoring each with the real evaluate(), for one inside the
 *  asked-for band (no jackpot or bonus trigger), and the reels then stop
 *  on it.  The game state is restored afterwards: only the stops leak. */
static int fwStop[NREEL];
static void force_win_search(void){
  static const int need[4]={10,25,50,100};
  static uint32_t rs=0x2545F491u;
  static game_t save;
  int k=dbg_force-20;
  if(k<0||k>3) return;
  long long bet=TOTBET, lo=(long long)need[k]*bet;
  long long hi=k<3?(long long)need[k+1]*bet:(long long)4e18;
  save=G;
  long long best=-1;
  for(int tr=0;tr<800000;tr++){
    int st[NREEL];
    for(int r=0;r<NREEL;r++){ rs^=rs<<13; rs^=rs>>17; rs^=rs<<5; st[r]=(int)(rs%STRIPLEN); }
    for(int r=0;r<NREEL;r++) for(int row=0;row<NROW;row++) G.grid[r][row]=stripAt(r,st[r]-row);
    evaluate();
    if(G.jpWon>=0||G.scatCount>=3||G.bonusCount>=3) continue;
    long long w=G.winTotal;
    if(w>best && w<hi){ best=w; memcpy(fwStop,st,sizeof fwStop); }
    if(w>=lo && w<hi) break;
  }
  G=save;
}

/* ═══ UPDATE ══════════════════════════════════════════════════════ */
static inline int cellcx(int r);
static inline int cellcy(int row);
static void flash_btn(int i);
static void art_update(void);
static const uint32_t WINCOL[8];         /* defined with the renderer */

static void update(void){
  G.t += DT;
  if(G.btn) G.idle=0; else G.idle += DT;
  if(G.flash>0) G.flash -= DT*3.0f;
  if(G.flash<0) G.flash=0;
  if(G.bannerT>0) G.bannerT -= DT;
  if(G.multUp>0) G.multUp -= DT;
  update_parts();
  fx_update();
  art_update();                 /* cosmetic clocks and caches, w/ the art */

  /* ---- reels ---- */
  float spinv = opt_turbo?20.0f:13.0f;      /* half the old speed */
  float settle= opt_turbo?0.34f:0.55f;
  int allstop=1;
  for(int r=0;r<NREEL;r++){
    if(G.rstate[r]==1){
      G.rpos[r] += spinv*DT;
      G.reelBlur[r] = 1.0f;
      G.rdelay[r] -= DT;
      if(G.rdelay[r]<=0){
        /* anticipation: hold the last reels when a bonus is live */
        if(r>=3 && G.rt1[r]==0.0f && partial_special()>=2){
          G.rt1[r]=1.0f; G.rdelay[r]=0.95f;
          sfx_anticipation(r);
        } else {
          G.rstate[r]=2; G.ru[r]=0;
          G.rt0[r]=G.rpos[r];
          float tgt = floorf(G.rpos[r]) + 4.0f + (float)irnd(STRIPLEN);
          if(dbg_force>=20){
            if(r==0) force_win_search();   /* the first reel to settle */
            int lo=(int)floorf(G.rpos[r])+4;
            tgt=(float)(lo+((fwStop[r]-lo)%STRIPLEN+STRIPLEN)%STRIPLEN);
          } else if(dbg_force){
            int want, rows[5];
            int nd=force_demand(r,&want,rows);
            int lo=(int)floorf(G.rpos[r])+4;
            for(int k=0;k<STRIPLEN && nd;k++){
              int b2=lo+k, okk=1;
              for(int q=0;q<nd;q++) if(stripAt(r,b2-rows[q])!=want) okk=0;
              if(okk){ tgt=(float)b2; break; }
            }
          }
          G.rt1[r]=tgt;
        }
      }
      allstop=0;
    } else if(G.rstate[r]==2){
      G.ru[r] += DT/settle;
      float u=clampf(G.ru[r],0,1);
      G.rpos[r] = G.rt0[r] + (G.rt1[r]-G.rt0[r])*easeOutBack(u);
      G.reelBlur[r] = 1.0f-u;
      if(G.ru[r]>=1.0f){
        G.rstate[r]=3; G.rpos[r]=G.rt1[r]; G.reelBlur[r]=0;
        sfx_reel_stop(r);
      }
      allstop=0;
    }
  }

  switch(G.state){
  case ST_ATTRACT:
    if(anyhit()){ G.state=ST_IDLE; G.t=0; G.idle=0; break; }
    if(G.t>3.0f){                       /* keep the reels turning       */
      for(int r=0;r<NREEL;r++){ G.rstate[r]=1; G.rdelay[r]=0.4f+r*0.25f; G.rt1[r]=0; }
      G.t=0;
    }
    break;

  case ST_IDLE:
    if(G.inFree){
      if(G.t>0.35f) start_spin();
      break;
    }
    if(G.idle>25.0f){ G.state=ST_ATTRACT; G.t=3.0f; break; }
    if(hit(B_RIGHT)||hit(B_UP)){
      if(G.betIdx<BETMAXIDX){ G.betIdx++; sfx_bet_up(G.betIdx); } else sfx_bet_limit();
      flash_btn(2);
    }
    if(hit(B_LEFT)||hit(B_DOWN)){
      if(G.betIdx>0){ G.betIdx--; sfx_bet_down(G.betIdx); } else sfx_bet_limit();
      flash_btn(1);
    }
    if(hit(B_SELECT)){ flash_btn(0); G.state=ST_PAYTABLE; G.ptPage=0; G.t=0; break; }
    if(hit(B_Y))     { flash_btn(4); open_addcr(ST_IDLE); break; }
    if(hit(B_X)){
      flash_btn(3);
      int k=max_affordable();
      if(k>=0){ G.betIdx=k; start_spin(); }
      break;
    }
    if(hit(B_START)||hit(B_A)){
      if(G.credits>=TOTBET) start_spin();
      else {
        int k=max_affordable();
        if(k>=0){ G.betIdx=k; sfx_bet_trim(); flash_btn(1); }   /* bet trimmed to the bank */
        else { G.state=ST_BROKE; G.t=0; }
      }
    }
    break;

  case ST_SPIN:
    /* let the player slam the reels down */
    if((hit(B_START)||hit(B_A)||hit(B_B)) && G.t>0.3f){
      for(int r=0;r<NREEL;r++) if(G.rstate[r]==1){ G.rdelay[r]=0; G.rt1[r]=1.0f; }
    }
    if(allstop){
      snapshot_grid(); evaluate();
      if(G.multUp>1.8f){                       /* the meter just notched up */
        fx_multup_begin();                     /* flash, shake, sparks (w7_fx.c) */
        sfx_mult(G.fsMult);
      }
      G.pend=0;
      if(extra_storm_pending()) extra_storm_begin();   /* -> ST_STORM -> ST_EVAL */
      else { G.state=ST_EVAL; G.t=0; }
    }
    break;

  case ST_STORM:
    extra_storm_update();
    break;

  case ST_EVAL:
    /* a multiplier pop gets the screen to itself: its sparks reach the
       meter before the spin's wins come up */
    if(G.t>0.20f && !(G.inFree && G.multUp>0.55f)){
      if(G.jpWon>=0){
        G.state=ST_JACKPOT; G.t=0; G.jpT=0;
        sfx_jackpot(G.jpWon);
        fx_jackpot_begin(G.jpWon);         /* flash, shake, the first burst */
        break;
      }
      G.pend |= pend_from_grid();          /* idempotent: EVAL can re-enter */
      if(G.winTotal>0){
        G.state=ST_SHOWWIN; G.t=0; G.showIdx=0; G.showT=0; G.winShown=0;
        /*  G.banner is the big-win tier on show (1 BIG .. 4 EPIC, 0 for
         *  a small win) and G.bannerT the time left on its slam.  BIG
         *  lands on the first frame; the count climbs to the rest.     */
        G.banner=0; G.bannerT=0;
        for(int i=0;i<G.nWin;i++) fx_win_burst(G.winMask[i],WINCOL[i&7],1);
        if(fx_bw_tier(G.winTotal,TOTBET)>=1){ G.banner=1; G.bannerT=1.0f; fx_bigwin_slam(1);
          if(G.winTotal>=TOTBET*40) sfx_big_win_tier(0);   /* audio plays smaller wins itself */ }
        else G.flash = opt_limiter?0.15f:0.25f;
      }
      else next_feature();
    }
    break;

  case ST_SHOWWIN: {
    /*  The count is a function of G.t (fx_bw_shown): a quick ease-out
     *  for a small win, and for a big one a roll that stalls just short
     *  of each tier line before the title slams up a tier.  The first
     *  press jumps to the whole amount, the second collects.          */
    long long bet=TOTBET;
    int   T   =fx_bw_tier(G.winTotal,bet);
    float cend=fx_bw_count_end(G.winTotal,bet);
    if(G.winShown < G.winTotal){
      long long v=fx_bw_shown(G.winTotal,bet,G.t);
      G.winShown=(int)(v>G.winTotal?G.winTotal:v);
      if(((int)(G.t*60))%3==0) sfx_win_tick((float)G.winShown/(float)G.winTotal);
    }
    if(T>=1){
      int cur=fx_bw_tier(G.winShown,bet);
      if(cur<1) cur=1;
      if(cur>G.banner){ G.banner=cur; G.bannerT=1.0f; fx_bigwin_slam(cur);
        if(cur>=2) sfx_big_win_tier(cur>=4?2:1); }
      fx_bigwin_tick(G.banner,G.winShown>=G.winTotal);
    }
    G.showT += DT;
    if(G.nWin>0 && G.showT>(T>=1?1.3f:0.85f)){
      G.showT=0; G.showIdx=(G.showIdx+1)%G.nWin;
      fx_win_burst(G.winMask[G.showIdx],WINCOL[G.showIdx&7],0);
    }
    /* X on a shown win: gamble it instead of collecting (w7_extra.c) */
    if(G.winShown>=G.winTotal && G.t>0.5f && hit(B_X) && gamble_allowed()){
      fx_stop(); G.banner=0;
      gamble_begin();
      break;
    }
    if(G.winShown < G.winTotal){
      if(anyhit() && G.t>0.2f){ G.t=cend; G.winShown=G.winTotal; }
    } else {
      float hold = T>=1 ? 3.0f : (G.inFree?1.6f:5.4f);
      if((anyhit() && G.t>cend+0.25f) || G.t>cend+hold){
        fx_stop(); G.banner=0;
        award(G.winTotal);
        next_feature();
      }
    }
    break; }

  case ST_HOLD:   hold_update();   break;
  case ST_WHEEL:  wheel_update();  break;
  case ST_GAMBLE: gamble_update(); break;

  case ST_JACKPOT:
    G.jpT += DT;
    { float run = fx_jackpot_run(G.jpWon);
      fx_jackpot_tick(G.jpWon,G.t);          /* coin rain, fountains, fireworks */
      if(((int)(G.t*60))%5==0 && G.t<run){
        /* the coin ticks climb while the amount rolls up */
        float k=clampf(G.t/run,0,1);
        sfx_jackpot_tick(k);
      } }
    if(G.t>fx_jackpot_run(G.jpWon)+fx_jackpot_hold(G.jpWon) || (G.t>1.4f && anyhit())){
      fx_stop();
      award(G.jpAmt);
      G.jpWon=-1; G.jpAmt=0; G.jpMask=0;
      G.state=ST_EVAL; G.t=0.21f;         /* fall back into the normal flow */
    }
    break;

  case ST_FSINTRO:
    if(G.t>2.6f || (G.t>0.6f && anyhit())){
      if(G.inFree){
        /* a retrigger: the spin that did it still counts as one of the
           free spins, which the old flow forgot (and the sim did not) */
        G.freeSpins += FS_RETRIG;
        after_result();
      } else {
        G.freeSpins += FS_AWARD;
        G.inFree=1; G.fsMult=1; G.fsWon=0;
        G.state=ST_IDLE; G.t=0;
      }
    }
    break;

  case ST_BONUS: {
    if(hit(B_LEFT)||hit(B_L))  { G.pickCur=(G.pickCur+NPICK-1)%NPICK; sfx_ui_move(-1); }
    if(hit(B_RIGHT)||hit(B_R)) { G.pickCur=(G.pickCur+1)%NPICK;       sfx_ui_move(+1); }
    if(hit(B_UP))              { G.pickCur=(G.pickCur+NPICK-3)%NPICK; sfx_ui_move(-1); }
    if(hit(B_DOWN))            { G.pickCur=(G.pickCur+3)%NPICK;       sfx_ui_move(+1); }
    if(hit(B_A)||hit(B_START)||hit(B_B)){
      int i=G.pickCur;
      if(!G.pickDone[i]){
        G.pickDone[i]=1; G.pickT=0;
        if(G.pickKind[i]==PICK_STOP){
          G.pickStops++;
          sfx_pick_stop(G.pickStops);
          if(G.pickStops>=3){
            G.state=ST_BONUSEND; G.t=0; G.banner=4; G.bannerT=3.0f;
          }
        } else if(G.pickKind[i]==PICK_MULT){
          G.pickMult *= G.pickVal[i];
          sfx_mult(G.pickMult);
          G.flash = opt_limiter?0.3f:0.6f;
        } else {
          G.pickTotal += G.pickVal[i];
          sfx_pick_reveal(G.pickVal[i]);
          G.flash = opt_limiter?0.25f:0.5f;
        }
      }
    }
    break; }

  case ST_BONUSEND:
    if(G.t>2.4f || (G.t>0.7f && anyhit())){
      if(G.pickTotal>0){ award(G.pickTotal*(G.pickMult>0?G.pickMult:1)); G.pickTotal=0; }
      if(G.banner==3){ G.banner=0; G.state=(G.credits<bet_at(0))?ST_BROKE:ST_IDLE; G.t=0; }
      else { G.banner=0; feature_done(); }
    }
    break;

  case ST_PAYTABLE:
    if(G.t>0.3f){
      if(hit(B_SELECT)){ G.ptPage=(G.ptPage+1)%3; G.t=0; sfx_ui_page(); }
      else if(anyhit()){ G.state=ST_IDLE; G.t=0; }
    }
    break;

  case ST_BROKE:
    if(hit(B_START)||hit(B_A)||hit(B_Y)){ flash_btn(4); open_addcr(ST_BROKE); }
    break;

  case ST_ADDCR:
    if(hit(B_RIGHT)||hit(B_UP))  { G.addIdx=(G.addIdx+1)%NADDS;       sfx_ui_move(+1); }
    if(hit(B_LEFT)||hit(B_DOWN)) { G.addIdx=(G.addIdx+NADDS-1)%NADDS; sfx_ui_move(-1); }
    if(hit(B_A)||hit(B_START)){
      G.credits += ADDS[G.addIdx];
      flash_btn(4);
      sfx_add_credits(ADDS[G.addIdx]);
      spawn_burst(RRX+RAILW/2, GY+30, 14, 0x8AF0FF);
      G.state=ST_IDLE; G.t=0;
    } else if(hit(B_B)||hit(B_SELECT)||hit(B_Y)){
      G.state = (G.addFrom==ST_BROKE && G.credits<bet_at(0)) ? ST_BROKE : ST_IDLE;
      G.t=0;
    }
    break;
  }
  (void)allstop;
}

/* ═══ CABINET CHROME ═══════════════════════════════════════════════
 *  Seven-segment readouts, moulded keycaps and the SPIN dome.  These
 *  are what make a slot look like a machine rather than a web page,
 *  and they are all cheap: filled spans, no sprites.
 * ================================================================= */

/* segment bits: a b c d e f g  =  bit 0..6 */
static const uint8_t SEG7[10] = {
  0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F
};

/*  One chamfered bar.  The ends taper at 45 degrees the way a real LED
 *  segment does, and the whole glyph is sheared for the classic slant. */
static void seg_bar(float x,float y,float len,float th,int horiz,
                    float slant,float ybase,uint32_t c,int a){
  if(a<=0) return;
  int iy=(int)y, j0=0, j1=horiz?(int)th:(int)len;     /* rows iy+j */
  if(iy+j0<clip_y0) j0=clip_y0-iy;
  if(iy+j1>clip_y1) j1=clip_y1-iy;
  if(horiz){
    for(int j=j0;j<j1;j++){
      float dy = j-(th-1)*0.5f;
      float inset = fabsf(dy);
      float x0=x+inset, x1=x+len-inset;
      float sh = (ybase-(y+j))*slant;
      int xa=(int)x0+(int)sh, xb=(int)x1+(int)sh;      /* one run per row */
      if(xa<0) xa=0;
      if(xb>FBW) xb=FBW;
      if(xa<xb) span_put(fb+(size_t)(iy+j)*FBW+xa,xb-xa,c,a);
    }
  } else {
    for(int j=j0;j<j1;j++){
      float dx0=0,dx1=th;
      float d=fminf((float)j,len-1-j);
      if(d<th*0.5f){ dx0=th*0.5f-d; dx1=th-dx0; }
      float sh=(ybase-(y+j))*slant;
      for(int i=(int)dx0;i<(int)dx1;i++) fb_blend((int)(x+i+sh),(int)y+j,c,a);
    }
  }
}

/*  digit: -1 blank, 10 = the '8' ghost used behind an unlit display  */
static void seg_digit(int d,float x,float y,float w,float h,float th,
                      uint32_t on,int aon){
  uint8_t m = (d>=0&&d<=9)? SEG7[d] : (d==10?0x7F:0x00);
  if(!m || !rows_visible((int)y-1,(int)(y+h)+3)) return;    /* not this band */
  const float sl=0.10f, yb=y+h;
  float hl=w-th, vl=h*0.5f;
  if(m&0x01) seg_bar(x+th*0.5f, y,            hl,th,1,sl,yb,on,aon);        /* a */
  if(m&0x40) seg_bar(x+th*0.5f, y+h*0.5f-th*0.5f,hl,th,1,sl,yb,on,aon);     /* g */
  if(m&0x08) seg_bar(x+th*0.5f, y+h-th,       hl,th,1,sl,yb,on,aon);        /* d */
  if(m&0x20) seg_bar(x,         y+th*0.5f,    vl,th,0,sl,yb,on,aon);        /* f */
  if(m&0x02) seg_bar(x+w-th,    y+th*0.5f,    vl,th,0,sl,yb,on,aon);        /* b */
  if(m&0x10) seg_bar(x,         y+h*0.5f,     vl,th,0,sl,yb,on,aon);        /* e */
  if(m&0x04) seg_bar(x+w-th,    y+h*0.5f,     vl,th,0,sl,yb,on,aon);        /* c */
}

/*  Width of a fixed-width readout, so callers can centre one. */
static int seg_width(int ndig,float w,float h,int commas){
  float adv=w+h*0.14f, cadv=commas? w*0.42f:0.0f;
  return (int)(ndig*adv + ((ndig-1)/3)*cadv);
}

/*  Right-aligned number with ghost segments behind it, which is what
 *  gives a real cabinet display its "8,888,888" shadow.  Positions are
 *  laid out right to left so the group separators always land between
 *  the right digit groups whatever the value.
 *  Returns the total width drawn, so callers can place a label.        */
static int seg_num(long long v,int rx,int y,int ndig,float w,float h,
                   uint32_t on,uint32_t ghost,int commas){
  float th = h*0.16f; if(th<3) th=3;
  float adv = w + h*0.14f;                 /* digit pitch               */
  float cadv= commas? w*0.42f : 0.0f;      /* extra pitch for a comma   */
  char buf[24]; int n=0;
  long long q = v<0?0:v;
  do { buf[n++] = (char)('0'+(q%10)); q/=10; } while(q && n<20);

  float x = (float)rx;
  for(int i=0;i<ndig;i++){
    if(commas && i && (i%3)==0){
      x -= cadv;
      int lit = (i<n);
      fb_rrect((int)(x+cadv*0.18f),(int)(y+h-th*0.9f),
               (int)(th*0.95f),(int)(th*1.7f),(int)(th*0.45f),
               lit?on:ghost, lit?235:30);
    }
    x -= adv;
    seg_digit(10,x,(float)y,w,h,th,ghost,30);            /* ghost 8 */
    if(i<n)            seg_digit(buf[i]-'0',x,(float)y,w,h,th,on,255);
    else if(i==0&&n==0)seg_digit(0,        x,(float)y,w,h,th,on,255);
  }
  return (int)((float)rx-x);
}

/* ═══ CABINET MATERIALS (init time, straight into the framebuffer) ═══
 *  The same studio the symbols reflect, applied to the cabinet: metal
 *  mouldings with a half-round profile, tinted glass over the backdrop,
 *  and lit plastic.  All of this is painted once into the background;
 *  nothing here runs per frame.
 * ================================================================= */

/*  A metal moulding round a rounded rectangle, t pixels wide.  Across
 *  its width the profile is a half-round, so the normal sweeps from the
 *  inner edge to the outer one and the moulding picks up the studio's
 *  hard horizon line the way a chrome trim strip does.               */
static void fb_moulding(int x,int y,int w,int h,float r,float t,int kind,int alpha){
  float cx=x+w*0.5f, cy=y+h*0.5f, hw=w*0.5f, hh=h*0.5f;
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ);
  float hx=LX/ll, hy=LY/ll, hz=LZ/ll+1.0f, hl=sqrtf(hx*hx+hy*hy+hz*hz);
  hx/=hl; hy/=hl; hz/=hl;
  int band=(int)(t+r+2.0f);
  for(int j=0;j<h;j++){
    int py=y+j;
    if(py<0||py>=FBH) continue;
    int mid=(j>band && j<h-band);
    for(int i=0;i<w;i++){
      if(mid && i>(int)t+2 && i<w-(int)t-3){ i=w-(int)t-3; continue; }
      float px=x+i+0.5f, pyf=py+0.5f;
      float d=rr_sdf(px,pyf,cx,cy,hw,hh,r);
      if(d>0.6f || d<-t-0.6f) continue;
      float cov=clampf(0.5f-d,0,1)*clampf(d+t+0.5f,0,1);
      float u=clampf((d+t*0.5f)/(t*0.5f),-1,1);
      float gx=rr_sdf(px+0.5f,pyf,cx,cy,hw,hh,r)-rr_sdf(px-0.5f,pyf,cx,cy,hw,hh,r);
      float gy=rr_sdf(px,pyf+0.5f,cx,cy,hw,hh,r)-rr_sdf(px,pyf-0.5f,cx,cy,hw,hh,r);
      float gl=sqrtf(gx*gx+gy*gy);
      if(gl<1e-4f){ gx=0; gy=-1; gl=1; }
      gx/=gl; gy/=gl;
      float nr=u*0.93f, nz=sqrtf(1.0f-nr*nr), nx=gx*nr, ny=gy*nr;
      uint32_t c=env_map(env_up(nx,ny,nz),kind);
      float sd=nx*hx+ny*hy+nz*hz;
      float sp=ipow(clampf(sd,0,1),50);
      c=mixc(c,0xFFFFFF,sp*0.8f);
      fb_blend(x+i,py,c,(int)(cov*alpha));
    }
  }
}

/* a soft shadow under a panel: stacked rounded rects, init only */
static void fb_softshadow(int x,int y,int w,int h,float r,int spread,int alpha){
  for(int k=spread;k>=1;k--)
    fb_rrect(x-k,y-k+spread/2,w+2*k,h+2*k,r+k,0x000000,alpha/spread+2);
}

/*  Tinted glass: it darkens and colours what is behind it rather than
 *  hiding it, catches a soft reflection across its top, and has a thin
 *  bright edge where the light enters the glass.                     */
static void fb_glass(int x,int y,int w,int h,float r,uint32_t top,uint32_t bot,int alpha){
  fb_rrectg(x,y,w,h,r,top,bot,alpha);
  float cx=x+w*0.5f, cy=y+h*0.5f;
  int rh=(int)(h*0.42f); if(rh>60) rh=60;
  for(int j=0;j<rh;j++){
    float f=1.0f-(float)j/rh; f=f*f;
    for(int i=0;i<w;i++){
      float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,w*0.5f,h*0.5f,r);
      if(d>-1.0f) continue;
      fb_blend(x+i,y+j,0xFFFFFF,(int)(f*22.0f));
    }
  }
  /* a diagonal sheen, the kind a light bar leaves on a glass front */
  for(int j=0;j<h;j++){
    int xs=x+(int)(w*0.62f)-j/2;
    for(int k=0;k<28;k++){
      int xx=xs+k;
      if(xx<x+4||xx>=x+w-4) continue;
      float d=rr_sdf(xx+0.5f,y+j+0.5f,cx,cy,w*0.5f,h*0.5f,r);
      if(d>-2.0f) continue;
      fb_blend(xx,y+j,0xFFFFFF,(int)(7.0f*sinf(k/28.0f*3.14159f)));
    }
  }
  fb_rframe(x+1,y+1,w-2,h-2,r-1,1.0f,0xFFFFFF,34);
}

/*  A black inset window for a readout to sit in: smoked glass sunk
 *  into the panel, darker at the top where the lip shades it, with a
 *  thin bright bevel on the lower edge where the light catches.      */
static void led_window(int x,int y,int w,int h){
  fb_rrect(x,y,w,h,7,0x000000,255);
  fb_rrectg(x+2,y+2,w-4,h-4,6,0x05060A,0x0C0E16,255);
  for(int j=0;j<6;j++){                                   /* inner shade */
    int a=(6-j)*24;
    for(int i=3;i<w-3;i++) fb_blend(x+i,y+2+j,0x000000,a);
  }
  for(int j=3;j<h-3;j+=3)                                 /* faint raster */
    for(int i=3;i<w-3;i++) fb_blend(x+i,y+j,0x000000,50);
  fb_rframe(x,y,w,h,7,1.5f,0x3A3E4C,255);
  for(int i=8;i<w-8;i++) fb_blend(x+i,y+h-2,0x9AA2B8,90);   /* lower lip catches light */
  for(int i=10;i<w*0.55f;i++) fb_blend(x+i,y+4,0xFFFFFF,14);  /* glass glint */
}

/*  Moulded keycap: light top face, shaded sides, engraved label.  It is
 *  drawn live by the ADD CREDITS chooser, so it stays on the cheap
 *  primitives; the deck buttons are baked richer below.             */
static void keycap(int x,int y,int w,int h,const char*l1,const char*l2,
                   int pressed,uint32_t face){
  int dy = pressed?3:0;
  fb_rrect(x+2,y+6,w,h,10,0x000000,190);                  /* cast shadow */
  if(!pressed)                                             /* body side  */
    fb_rrectg(x,y+dy+4,w,h,10,scalec(face,0.42f),scalec(face,0.22f),255);
  fb_rrectg(x,y+dy,w,h-(pressed?0:4),10,
            mixc(face,0xFFFFFF,0.40f),scalec(face,0.70f),255);
  for(int j=0;j<h/3;j++){                                  /* top gloss  */
    int a=(h/3-j)*4;
    for(int i=6;i<w-6;i++) fb_blend(x+i,y+dy+2+j,0xFFFFFF,a);
  }
  fb_rframe(x,y+dy,w,h-(pressed?0:4),10,1.5f,scalec(face,0.30f),220);
  fb_rframe(x+2,y+dy+2,w-4,h-(pressed?4:8),8,1.0f,0xFFFFFF,70);
  int ty = y+dy+(l2? h/2-16 : h/2-11);
  text(l1,x+w/2,ty,2,0x14141C,1,0);
  if(l2) text(l2,x+w/2,ty+18,2,0x14141C,1,0);
}

/* ── additive sprites: glows, halos, bulbs ─────────────────────────
 *  A glow sprite holds its light premultiplied in RGB.  blit_add adds it
 *  scaled by k/256, clipped to the band being drawn.                  */
static void blit_add(const spr_t*s,int dx,int dy,int k){
  if(!s->px||k<=0) return;
  int y0=0, y1=s->h;
  if(dy+y0<clip_y0) y0=clip_y0-dy;
  if(dy+y1>clip_y1) y1=clip_y1-dy;
  for(int y=y0;y<y1;y++){
    int fy=dy+y;
    if((unsigned)fy>=FBH) continue;
    int xa=s->rx0?s->rx0[y]:0, xb=s->rx1?s->rx1[y]:s->w-1;
    if(dx+xa<0) xa=-dx;
    if(dx+xb>=FBW) xb=FBW-1-dx;
    const uint8_t*row=s->px+(size_t)y*s->w*4;
    uint32_t*dst=fb+(size_t)fy*FBW+dx;
    for(int x=xa;x<=xb;x++){
      const uint8_t*p=row+x*4;
      if(!p[3]) continue;
      uint32_t d=dst[x];
      int r=((d>>16)&255)+((p[0]*k)>>8), g=((d>>8)&255)+((p[1]*k)>>8), b=(d&255)+((p[2]*k)>>8);
      dst[x]=RGB(r>255?255:r,g>255?255:g,b>255?255:b);
    }
  }
}
/* a round glow of radius r, colour col, falling off as (1-d)^p */
static void make_glow(spr_t*s,int r,uint32_t col,float p,float core){
  s->w=s->h=r*2+1;
  s->px=(uint8_t*)calloc((size_t)s->w*s->h,4);
  if(!s->px) return;
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++){
    float dx=(x-r)/(float)r, dy=(y-r)/(float)r, d=sqrtf(dx*dx+dy*dy);
    if(d>=1.0f) continue;
    float v=powf(1.0f-d,p)+core*powf(clampf(1.0f-d*3.0f,0,1),2.0f);
    v=clampf(v,0,1);
    uint8_t*o=s->px+((size_t)y*s->w+x)*4;
    o[0]=(uint8_t)(cr*v); o[1]=(uint8_t)(cg*v); o[2]=(uint8_t)(cb*v);
    o[3]=(uint8_t)(v>0.004f?255:0);
  }
  spr_bounds(s);
}
/* copy a rectangle of the framebuffer out as an opaque sprite */
static void grab_sprite(spr_t*s,int x,int y,int w,int h){
  s->w=w; s->h=h;
  s->px=(uint8_t*)malloc((size_t)w*h*4);
  if(!s->px) return;
  for(int j=0;j<h;j++) for(int i=0;i<w;i++){
    uint32_t c=((unsigned)(x+i)<FBW&&(unsigned)(y+j)<FBH)?fb[(y+j)*FBW+x+i]:0;
    uint8_t*o=s->px+((size_t)j*w+i)*4;
    o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255); o[3]=255;
  }
  spr_bounds(s);
}

static spr_t domehalo;         /* the SPIN dome's idle glow            */
static spr_t bulbspr[2];       /* marquee bulbs: warm, and red         */
static spr_t sparkspr;         /* a small hot glint                    */
static spr_t btnspr[5][2];     /* deck buttons: at rest, and lit       */

/*  The big domed SPIN button: translucent green plastic lit from
 *  inside, a hard window reflection, a thick chrome bezel.  Two states,
 *  both baked; the glow round it is a separate additive sprite.      */
static void build_dome(int which){
  int r=SPINR, w=r*2+16, h=r*2+16, cx=w/2, cy=h/2;
  spr_t*sp=&domespr[which];
  sp->w=w; sp->h=h;
  sp->px=(uint8_t*)calloc((size_t)w*h,4);
  if(!sp->px) return;
  int pressed=which;
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ), lx=LX/ll, ly=LY/ll, lz=LZ/ll;
  const int S2=3;                                   /* 3x3 supersample */
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    int rs=0,gs=0,bs=0,as=0;
    for(int sy=0;sy<S2;sy++) for(int sx=0;sx<S2;sx++){
      float fx=x+(sx+0.5f)/S2-cx, fy=y+(sy+0.5f)/S2-cy, d=sqrtf(fx*fx+fy*fy);
      uint32_t c; int a=255;
      if(d<=r-1.0f){
        float nx=fx/r, ny=fy/r, nz=sqrtf(fmaxf(0.0f,1.0f-nx*nx-ny*ny));
        float lam=clampf(nx*lx+ny*ly+nz*lz,0,1);
        uint32_t base=pressed?0x10C838:0x14A62E;
        /* lit from inside: brightest in the middle, deep at the rim */
        float core=1.0f-d/r;
        c=mixc(scalec(base,0.32f),mixc(base,0xB8FF90,0.45f),clampf(core*1.1f,0,1));
        c=mixc(c,mixc(base,0xFFFFFF,0.2f),lam*0.35f);
        /* hard window reflection across the top */
        float wx=nx/0.72f, wy=(ny+0.42f)/0.34f;
        if(wx*wx+wy*wy<1.0f && ny<-0.12f) c=mixc(c,0xFFFFFF,0.55f*(1.0f-(ny+0.95f)*0.9f));
        float spec=powf(clampf(nx*-0.42f+ny*-0.52f+nz*0.74f,0,1),40.0f);
        c=mixc(c,0xFFFFFF,spec*0.9f);
        /* rim light low right */
        float rl=smooth01(0.7f,1.0f,d/r)*clampf((nx*0.5f+ny*0.86f),0,1);
        c=mixc(c,0xC8FFB0,rl*0.5f);
      } else if(d<=r+6.0f){
        float t=clampf((d-(r+2.5f))/3.5f,-1,1);
        float nr=t*0.93f, nz=sqrtf(1.0f-nr*nr);
        float nx=fx/d*nr, ny=fy/d*nr;
        c=env_map(env_up(nx,ny,nz),0);
        if(d>r+5.0f) a=(int)(255*clampf(r+6.0f-d,0,1));
      } else { a=0; c=0; }
      rs+=((c>>16)&255)*a; gs+=((c>>8)&255)*a; bs+=(c&255)*a; as+=a;
    }
    if(!as) continue;
    uint8_t*o=sp->px+((size_t)y*w+x)*4;
    o[0]=(uint8_t)(rs/as); o[1]=(uint8_t)(gs/as); o[2]=(uint8_t)(bs/as); o[3]=(uint8_t)(as/(S2*S2));
  }
  spr_bounds(sp);
  if(!which) make_glow(&domehalo,SPINR+30,0x60FF70,2.2f,0.0f);
}

static void spin_dome(int cx,int cy,int r,int pressed,float pulse){
  const spr_t*sp=&domespr[pressed?1:0];
  if(!sp->px) return;
  if(pulse>0.0f) blit_add(&domehalo,cx-domehalo.w/2,cy-domehalo.h/2,(int)(60+170*pulse));
  blit(sp,cx-sp->w/2,cy-sp->h/2,0,FBH,255,0,0.0f);
  if(pulse>0.0f){
    int rr=r+9;
    for(int a=0;a<360;a++){
      float th2=a*(TAU/360.0f);
      fb_blend(cx+(int)(cosf(th2)*rr),cy+(int)(sinf(th2)*rr),
               0xDFFFC0,(int)(170*pulse));
    }
  }
  text("SPIN",cx,cy-12,3,0x05240A,1,0);
  text("SPIN",cx,cy-13,3,0xFFFFFF,1,0);
  text("PRESS",cx,cy+12,1,0xDDFFC8,1,1);
}

/* ═══ TITLES ═══════════════════════════════════════════════════════
 *  The words that sell the machine - the logo, FREE SPINS, the bonus
 *  banners - are baked once into sprites with the full treatment: a
 *  coloured neon glow, a cast shadow, a dark keyline and a bevelled,
 *  mirror-finished face.  At run time they are only blitted (or scaled,
 *  for a zoom), which costs a fraction of drawing the bubble type live.
 * ================================================================= */
typedef struct {
  const uint32_t*ramp; int nr;   /* face colour, top to bottom of the caps */
  uint32_t line;                 /* keyline                                */
  uint32_t glow; float glowAmt;  /* neon halo                              */
  int glowR;                     /* halo radius, screen pixels             */
  int env;                       /* what the bevel reflects: 0 chrome 1 gold */
  float metal;
} tstyle_t;

static void box_blur_f(float*f,float*tmp,int W,int H,int r,int passes){
  float inv=1.0f/(2*r+1);
  for(int p=0;p<passes;p++){
    for(int y=0;y<H;y++){
      float*row=f+(size_t)y*W, *dst=tmp+(size_t)y*W, acc=0;
      for(int x=0;x<=r && x<W;x++) acc+=row[x];
      for(int x=0;x<W;x++){
        dst[x]=acc*inv;
        if(x+r+1<W) acc+=row[x+r+1];
        if(x-r>=0)  acc-=row[x-r];
      }
    }
    for(int x=0;x<W;x++){
      float acc=0;
      for(int y=0;y<=r && y<H;y++) acc+=tmp[(size_t)y*W+x];
      for(int y=0;y<H;y++){
        f[(size_t)y*W+x]=acc*inv;
        if(y+r+1<H) acc+=tmp[(size_t)(y+r+1)*W+x];
        if(y-r>=0)  acc-=tmp[(size_t)(y-r)*W+x];
      }
    }
  }
}

static void bake_title(spr_t*o,const char*s,int px,const tstyle_t*ts){
  const int K=px>=8?1:2;              /* supersample the small ones only */
  int n=(int)strlen(s);
  float P=(float)(px*K);
  int pad=ts->glowR*K*2+(int)(P*1.3f)+6;
  int W=(int)((n*6-1)*P)+pad*2, H=(int)(7*P)+pad*2;
  size_t N=(size_t)W*H;
  uint8_t*fill=(uint8_t*)calloc(N,1), *outl=(uint8_t*)calloc(N,1);
  float*hf=(float*)malloc(N*4), *tmp=(float*)malloc(N*4), *gl=(float*)malloc(N*4), *acc=(float*)calloc(N*4,4);
  memset(o,0,sizeof *o);
  if(!fill||!outl||!hf||!tmp||!gl||!acc){ free(fill); free(outl); free(hf); free(tmp); free(gl); free(acc); return; }
  float Rin=P*0.62f, Rout=Rin+P*0.30f+K*1.2f;
  for(int i=0;i<n;i++){
    int ch=(unsigned char)s[i];
    if(ch>='a'&&ch<='z') ch-=32;
    if(ch<32||ch>127) ch='?';
    const uint8_t*g=FONT[ch-32];
    #define SETT(rr,cc) ((rr)>=0&&(rr)<7&&(cc)>=0&&(cc)<5 && (g[rr]&(0x10>>(cc))))
    for(int r=0;r<7;r++) for(int c=0;c<5;c++){
      if(!SETT(r,c)) continue;
      float x=pad+i*6*P+c*P+P*0.5f, y=pad+r*P+P*0.5f;
      tb_stamp(fill,W,H,x,y,Rin); tb_stamp(outl,W,H,x,y,Rout);
      static const int NB[4][2]={{0,1},{1,0},{1,1},{1,-1}};
      for(int k=0;k<4;k++){
        int nr=r+NB[k][0], nc=c+NB[k][1];
        if(!SETT(nr,nc)) continue;
        if(NB[k][0]&&NB[k][1] && (SETT(r,nc)||SETT(nr,c))) continue;
        float x2=pad+i*6*P+nc*P+P*0.5f, y2=pad+nr*P+P*0.5f;
        tb_cap(fill,W,H,x,y,x2,y2,Rin); tb_cap(outl,W,H,x,y,x2,y2,Rout);
      }
    }
    #undef SETT
  }
  /* height field for the bevel, and the halo */
  int rb=(int)(P*0.18f); if(rb<1) rb=1;
  for(size_t i=0;i<N;i++){ hf[i]=fill[i]/255.0f; gl[i]=outl[i]/255.0f; }
  box_blur_f(hf,tmp,W,H,rb,2);
  if(ts->glowR>0) box_blur_f(gl,tmp,W,H,ts->glowR*K/2+1,3);
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ), lx=LX/ll, ly=LY/ll, lz=LZ/ll;
  float hx=lx, hy=ly, hz=lz+1.0f, hl=sqrtf(hx*hx+hy*hy+hz*hz);
  hx/=hl; hy/=hl; hz/=hl;
  float S=1.7f*(2*rb+1), spec0=ipow(hz,40);
  int so=(int)(P*0.30f)+K;
  #define OVER(i_,c_,a_) do{ float a__=(a_); if(a__>0){ float*q=acc+(i_)*4; \
      q[0]=((c_)>>16&255)*a__+q[0]*(1-a__); q[1]=((c_)>>8&255)*a__+q[1]*(1-a__); \
      q[2]=((c_)&255)*a__+q[2]*(1-a__); q[3]=a__+q[3]*(1-a__); } }while(0)
  for(int y=0;y<H;y++) for(int x=0;x<W;x++){
    size_t i=(size_t)y*W+x;
    if(ts->glowR>0){ float g=clampf(gl[i]*1.5f,0,1)*ts->glowAmt; OVER(i,ts->glow,g); }
    if(x>=so && y>=so){ float a=outl[(size_t)(y-so)*W+(x-so)]/255.0f*0.65f; OVER(i,0x000000,a); }
    if(outl[i]) OVER(i,ts->line,outl[i]/255.0f);
    if(fill[i]){
      float gx=(x+1<W?hf[i+1]:hf[i])-(x>0?hf[i-1]:hf[i]);
      float gy=(y+1<H?hf[i+W]:hf[i])-(y>0?hf[i-W]:hf[i]);
      float nx=-gx*0.5f*S, ny=-gy*0.5f*S, nz=1.0f, nl=sqrtf(nx*nx+ny*ny+1.0f);
      nx/=nl; ny/=nl; nz/=nl;
      float t=clampf((y-pad)/(7*P),0,1);
      uint32_t c=ramp(ts->ramp,ts->nr,t);
      float k=(nx*lx+ny*ly+nz*lz)-lz;
      c = k>0 ? mixc(c,0xFFFFFF,clampf(k*0.8f,0,0.75f)) : scalec(c,clampf(1.0f+k*1.2f,0.2f,1));
      float wm=ts->metal*clampf((1.0f-nz)*3.2f,0,1);
      if(wm>0) c=mixc(c,env_map(env_up(nx,ny,nz),ts->env),wm);
      float sd=nx*hx+ny*hy+nz*hz, sp=ipow(clampf(sd,0,1),40)-spec0;
      if(sp>0) c=mixc(c,0xFFFFFF,clampf(sp*3.0f,0,1));
      OVER(i,c,fill[i]/255.0f);
    }
  }
  #undef OVER
  /* resolve KxK down to the sprite, straight alpha */
  o->w=W/K; o->h=H/K;
  o->px=(uint8_t*)calloc((size_t)o->w*o->h,4);
  if(o->px) for(int y=0;y<o->h;y++) for(int x=0;x<o->w;x++){
    float r=0,g=0,b=0,a=0;
    for(int j=0;j<K;j++) for(int i=0;i<K;i++){
      const float*q=acc+((size_t)(y*K+j)*W+(x*K+i))*4;
      r+=q[0]; g+=q[1]; b+=q[2]; a+=q[3];
    }
    uint8_t*d=o->px+((size_t)y*o->w+x)*4;
    if(a>0.001f){ d[0]=(uint8_t)clampi((int)(r/a),0,255); d[1]=(uint8_t)clampi((int)(g/a),0,255); d[2]=(uint8_t)clampi((int)(b/a),0,255); }
    d[3]=(uint8_t)clampi((int)(a/(K*K)*255.0f+0.5f),0,255);
  }
  spr_bounds(o);
  free(fill); free(outl); free(hf); free(tmp); free(gl); free(acc);
}

/*  Bilinear, scaled, centred blit for zooms.  Straight-alpha sprite,
 *  filtered premultiplied so the edges do not fringe.  Honours the band
 *  clip; cost is the destination area.                               */
static void blit_scaled(const spr_t*s,int cx,int cy,float sc,int alpha){
  if(!s->px||sc<=0.01f||alpha<=0) return;
  /* a big sprite in fast motion: point sampling, four times cheaper,
     and nobody can see the difference while it is still moving */
  if(s->w*sc*s->h*sc > 90000.0f){
    int dw=(int)(s->w*sc), dh=(int)(s->h*sc), x0=cx-dw/2, y0=cy-dh/2;
    int ya=y0<clip_y0?clip_y0:y0, yb=y0+dh>clip_y1?clip_y1:y0+dh;
    int xa=x0<0?0:x0, xb=x0+dw>FBW?FBW:x0+dw;
    int inv=(int)(65536.0f/sc);
    for(int y=ya;y<yb;y++){
      int iy=((y-y0)*inv)>>16; if(iy>=s->h) continue;
      const uint8_t*row=s->px+(size_t)iy*s->w*4;
      uint32_t*d=fb+(size_t)y*FBW;
      for(int x=xa;x<xb;x++){
        int ix=((x-x0)*inv)>>16; if(ix>=s->w) continue;
        const uint8_t*p=row+ix*4;
        int ea=p[3]*alpha>>8;
        if(ea<=2) continue;
        uint32_t dd=d[x];
        int dr=(dd>>16)&255, dg=(dd>>8)&255, db=dd&255;
        d[x]=RGB(dr+((p[0]-dr)*ea>>8), dg+((p[1]-dg)*ea>>8), db+((p[2]-db)*ea>>8));
      }
    }
    return;
  }
  int dw=(int)(s->w*sc), dh=(int)(s->h*sc);
  int x0=cx-dw/2, y0=cy-dh/2;
  int ya=y0<clip_y0?clip_y0:y0, yb=y0+dh>clip_y1?clip_y1:y0+dh;
  int xa=x0<0?0:x0, xb=x0+dw>FBW?FBW:x0+dw;
  int inv=(int)(65536.0f/sc);
  for(int y=ya;y<yb;y++){
    int sy=(y-y0)*inv+ (inv>>1) - 32768;
    int iy=sy>>16, fy=(sy>>8)&255;
    if(iy<-1||iy>=s->h) continue;
    for(int x=xa;x<xb;x++){
      int sx=(x-x0)*inv+(inv>>1)-32768;
      int ix=sx>>16, fx=(sx>>8)&255;
      int r=0,g=0,b=0,a=0;
      for(int j=0;j<2;j++){
        int yy=iy+j; if(yy<0||yy>=s->h) continue;
        int wy=j?fy:256-fy;
        for(int i=0;i<2;i++){
          int xx=ix+i; if(xx<0||xx>=s->w) continue;
          const uint8_t*p=s->px+((size_t)yy*s->w+xx)*4;
          if(!p[3]) continue;
          int w=(wy*(i?fx:256-fx))>>8;
          int pa=p[3]*w;
          r+=p[0]*pa; g+=p[1]*pa; b+=p[2]*pa; a+=pa;
        }
      }
      if(a<=0) continue;
      int ea=((a>>8)*alpha)>>8;
      if(ea<=0) continue;
      uint32_t c=RGB(r/a,g/a,b/a);
      uint32_t*d=fb+(size_t)y*FBW+x;
      if(ea>=255){ *d=c; continue; }
      uint32_t dd=*d;
      int dr=(dd>>16)&255, dg=(dd>>8)&255, db=dd&255;
      *d=RGB(dr+((((c>>16)&255)-dr)*ea>>8), dg+((((c>>8)&255)-dg)*ea>>8), db+(((c&255)-db)*ea>>8));
    }
  }
}

/*  A light sweep through a sprite's solid pixels: a diagonal band that
 *  adds white where the sprite is opaque.  Per row it touches only the
 *  band, so it costs a few thousand pixels however big the sprite.   */
static void shine_sprite(const spr_t*s,int dx,int dy,int cy0,int cy1,float pos,int bw,int k,int amin){
  if(!s->px||k<=0) return;
  int y0=0,y1=s->h;
  if(dy+y0<cy0) y0=cy0-dy;
  if(dy+y1>cy1) y1=cy1-dy;
  if(dy+y0<clip_y0) y0=clip_y0-dy;
  if(dy+y1>clip_y1) y1=clip_y1-dy;
  for(int y=y0;y<y1;y++){
    int fy=dy+y;
    if((unsigned)fy>=FBH) continue;
    float c=pos-(float)y*0.55f;
    int xa=(int)(c-bw), xb=(int)(c+bw);
    int ra=s->rx0?s->rx0[y]:0, rb=s->rx1?s->rx1[y]:s->w-1;
    if(xa<ra) xa=ra;
    if(xb>rb) xb=rb;
    if(dx+xa<0) xa=-dx;
    if(dx+xb>=FBW) xb=FBW-1-dx;
    const uint8_t*row=s->px+(size_t)y*s->w*4;
    uint32_t*dst=fb+(size_t)fy*FBW+dx;
    for(int x=xa;x<=xb;x++){
      int a=row[x*4+3];
      if(a<amin) continue;
      float u=1.0f-fabsf(x-c)/bw;
      int v=(int)(u*u*k)*a>>8;
      uint32_t d=dst[x];
      int r=((d>>16)&255)+v, g=((d>>8)&255)+v, b=(d&255)+v;
      dst[x]=RGB(r>255?255:r,g>255?255:g,b>255?255:b);
    }
  }
}

/* the baked titles */
enum { TT_LOGO, TT_LOGOBIG, TT_FREESPINS, TT_FSDONE, TT_BONUSDONE, TT_PICK,
       TT_BROKE, TT_PRESS, TT_TOTALWIN, NTT };
static spr_t title[NTT];
static spr_t bigdig[10];            /* big gold digits for the count-ups */
static spr_t digX;                  /* the "X" that goes with them       */
static spr_t bigPlus;               /* and the "+" of a retrigger        */
#define NCAP 9
static spr_t capSpr[NCAP];          /* the attract loop's captions       */
static const char*CAPS[NCAP]={"WILD 7","SCATTER","LUCKY 7 PICK","HOLD & SPIN","WHEEL OF 7'S",
                              "JACKPOT","ULTIMATE","4 PROGRESSIVE JACKPOTS","FEATURES"};

static const uint32_t TGOLD[6]  ={0xFFFEF0,0xFFF0B0,0xFFD24A,0xE8A018,0xFFD870,0xA86A08};
static const uint32_t TGREEN[5] ={0xF4FFE8,0xB8FF8A,0x3CD23C,0x16861E,0x7CE860};
static const uint32_t TRED[5]   ={0xFFF0E8,0xFF9A7A,0xF02A2A,0x9A0A14,0xFF6A4A};
static const uint32_t TSILVER[5]={0xFFFFFF,0xEEF4FF,0xB0BCD8,0x6A7898,0xDCE4F8};

static void build_titles(void){
  tstyle_t logo ={TGOLD,6,0x1A0600,0xFF3010,0.95f,7,1,0.85f};
  tstyle_t logoB={TGOLD,6,0x1A0600,0xFF2A10,0.90f,18,1,0.85f};
  tstyle_t fs   ={TGREEN,5,0x021A06,0x40FF60,0.85f,16,1,0.55f};
  tstyle_t gold ={TGOLD,6,0x1A0600,0xFFB020,0.80f,14,1,0.80f};
  tstyle_t pick ={TGOLD,6,0x1A0600,0xC040FF,0.85f,10,1,0.80f};
  tstyle_t red  ={TRED,5,0x1A0204,0xFF2020,0.85f,14,1,0.50f};
  tstyle_t silv ={TSILVER,5,0x0A0C18,0x60B0FF,0.80f,10,0,0.70f};
  bake_title(&title[TT_LOGO],     "WILD 7's",5,&logo);
  bake_title(&title[TT_LOGOBIG],  "WILD 7's",15,&logoB);
  bake_title(&title[TT_FREESPINS],"FREE SPINS",12,&fs);
  bake_title(&title[TT_FSDONE],   "FREE SPINS COMPLETE",6,&fs);
  bake_title(&title[TT_BONUSDONE],"BONUS COMPLETE",8,&gold);
  bake_title(&title[TT_PICK],     "LUCKY 7 PICK",6,&pick);
  bake_title(&title[TT_BROKE],    "OUT OF CREDITS",8,&red);
  bake_title(&title[TT_PRESS],    "PRESS START",6,&silv);
  bake_title(&title[TT_TOTALWIN], "TOTAL WIN",5,&gold);
  for(int d=0;d<10;d++){ char b[2]={(char)('0'+d),0}; bake_title(&bigdig[d],b,16,&gold); }
  bake_title(&digX,"X",11,&gold);
  bake_title(&bigPlus,"+",11,&gold);
  { tstyle_t cap={TGOLD,6,0x1A0600,0xFF9020,0.70f,5,1,0.80f};
    for(int i=0;i<NCAP;i++) bake_title(&capSpr[i],CAPS[i],5,&cap); }
}
static void spr_free_t(spr_t*s){ free(s->px); free(s->rx0); free(s->rx1); memset(s,0,sizeof *s); }
static void free_titles(void){
  spr_t*all[NTT+11];
  int n=0;
  for(int i=0;i<NTT;i++) all[n++]=&title[i];
  for(int i=0;i<10;i++) all[n++]=&bigdig[i];
  all[n++]=&digX;
  spr_free_t(&bigPlus);
  for(int i=0;i<NCAP;i++) spr_free_t(&capSpr[i]);
  for(int i=0;i<n;i++){ free(all[i]->px); free(all[i]->rx0); free(all[i]->rx1); memset(all[i],0,sizeof(spr_t)); }
}

/* ═══ BACKGROUND (built once, then memcpy'd every frame) ═══════════ */
static void vgrad(int x,int y,int w,int h,uint32_t top,uint32_t bot){
  int i0=x<0?0:x, i1=x+w>FBW?FBW:x+w, j0=0, j1=h;
  if(y+j0<clip_y0) j0=clip_y0-y;
  if(y+j1>clip_y1) j1=clip_y1-y;
  if(i0>=i1) return;
  for(int j=j0;j<j1;j++){
    uint32_t c=mixc(top,bot,(float)j/(h>1?h-1:1));
    span_fill(fb+(size_t)(y+j)*FBW+i0,i1-i0,c);
  }
}

static const uint32_t SILVERG[4]= {0xFFFFFF,0xE0E8F8,0x9AA6C0,0x525C78};
static const uint32_t GOLDG[5] = {0xFFFDF0,0xFFEBA8,0xF0B420,0xA8760C,0x5E4206};
static const uint32_t REDG[4]   = {0xFFE4D8,0xFF7A5A,0xC81828,0x6A0A14};
static const uint32_t GREENG[4] = {0xEEFFE6,0x9CF57A,0x2E9A2E,0x115011};
static const uint32_t ICEG[4]   = {0xFFFFFF,0xCFEEFF,0x4FA8D8,0x14506E};
static const uint32_t RAINBOWG[6]={0xFFFFFF,0xFFE0A0,0xFF8AC8,0xA0C0FF,0x80FFD0,0x2A5A80};

/*  The cabinet has two looks: the base game, and the free-spins night
 *  set, which gets its own backdrop and gilded drums.  Both are baked
 *  at init and art_update() swaps them.                               */
static int bgTheme = 0;

/*  Reel drum shading.  A physical reel is a cylinder seen edge on, so
 *  it is brightest across the middle and falls into shadow at the top
 *  and bottom of the window.  Cream rather than white: the reference
 *  cabinet's drums are warm, and the chrome medallions read better on
 *  a warm ground than on a clinical one.  In free spins the drums take
 *  on gold.                                                           */
static uint32_t drum_shade(int y){
  float t=(float)(y-GY)/(float)(GH-1);
  float d=fabsf(t-0.5f)*2.0f;                    /* 0 centre, 1 at a lip */
  float roll=clampf((d-0.74f)/0.26f,0,1);
  uint32_t c = bgTheme ? mixc(0xFFF2CC,0xE8C480,clampf(d*0.75f,0,1))
                       : mixc(0xFFFAEE,0xEEE0C2,clampf(d*0.6f,0,1));
  c=mixc(c,bgTheme?0x5A3E14:0x5E5644,roll*roll*0.80f);
  return c;
}

/*  Rail panel: drawn live when a panel lights up, so it is cheap: a
 *  translucent gradient over the baked glass, a rim and a header.    */
static void rail_panel(int x,int y,int w,int h,uint32_t hi,uint32_t lo,uint32_t rim,
                       const char*label,uint32_t lc){
  fb_rrectg(x+3,y+3,w-6,h-6,10,hi,lo,215);
  fb_rframe(x,y,w,h,12,2.0f,rim,255);
  fb_rframe(x+3,y+3,w-6,h-6,10,1.0f,mixc(rim,0xFFFFFF,0.5f),90);
  if(label) text(label,x+w/2,y+8,1,lc,1,1);
}

/* the cabinet version: smoked glass in a moulding, with a lit header */
static void glass_rail(int x,int y,int w,int h,uint32_t top,uint32_t bot,int kind,
                       const char*label,uint32_t lc){
  fb_softshadow(x,y,w,h,12,8,120);
  fb_glass(x+2,y+2,w-4,h-4,11,top,bot,200);
  fb_moulding(x-1,y-1,w+2,h+2,13,4.0f,kind,255);
  if(label){
    int lw=(int)strlen(label)*6+14;
    fb_rrectg(x+w/2-lw/2,y+3,lw,13,6,0x000000,0x000000,120);
    text(label,x+w/2,y+6,1,lc,1,1);
  }
}

/* button deck geometry, shared by the static art and the live redraw */
#define NBTN 5
static const int BTNX[NBTN] = {  24, 236, 448, 660, 872 };
static const int BTNW[NBTN] = { 196, 196, 196, 196, 196 };
static const char*BTNL1[NBTN]= {"PAYS","BET","BET","BET","ADD"};
static const char*BTNL2[NBTN]= {NULL, "LESS","MORE","MAX","CREDITS"};
static const uint32_t BTNC[NBTN]={0x2A78F0,0xF0A020,0xF0A020,0xE8283A,0x22B84A};

/*  An illuminated arcade button: coloured translucent plastic with a
 *  lamp behind it, a hard reflection across the top and a chrome
 *  bezel.  `lit` is the pressed flash, brighter and seated deeper.  */
static void paint_button(int x,int y,int w,int h,const char*l1,const char*l2,uint32_t col,int lit){
  float r=14.0f, cx=x+w*0.5f, cy=y+h*0.5f;
  fb_rrect(x-2,y+2,w+4,h+6,r+2,0x000000,170);
  int dy=lit?2:0;
  float ll=sqrtf(LX*LX+LY*LY+LZ*LZ), lx=LX/ll, ly=LY/ll, lz=LZ/ll;
  float hw=w*0.5f-4, hh=h*0.5f-4, rr=r-3;
  for(int j=0;j<h;j++) for(int i=0;i<w;i++){
    float px=x+i+0.5f, py=y+j+0.5f;
    float d=rr_sdf(px,py,cx,cy+dy,hw,hh,rr);
    if(d>0.5f) continue;
    float cov=clampf(0.5f-d,0,1);
    /* a pillow: flat on top, rolling off over the last 9 pixels */
    float e=clampf(-d/9.0f,0,1), tilt=(1.0f-e)*(1.0f-e);
    float gx=rr_sdf(px+0.5f,py,cx,cy+dy,hw,hh,rr)-rr_sdf(px-0.5f,py,cx,cy+dy,hw,hh,rr);
    float gy=rr_sdf(px,py+0.5f,cx,cy+dy,hw,hh,rr)-rr_sdf(px,py-0.5f,cx,cy+dy,hw,hh,rr);
    float gl=sqrtf(gx*gx+gy*gy); if(gl<1e-4f){ gx=0; gy=0; gl=1; }
    float nx=gx/gl*tilt*0.95f, ny=gy/gl*tilt*0.95f, nz=sqrtf(fmaxf(0.05f,1.0f-nx*nx-ny*ny));
    /* the lamp inside: a bright core under translucent plastic */
    float ex=(px-cx)/(w*0.5f), ey=(py-cy-dy)/(h*0.5f);
    float core=clampf(1.0f-sqrtf(ex*ex*0.45f+ey*ey*1.1f),0,1);
    uint32_t dark=scalec(col,lit?0.50f:0.30f), bright=mixc(col,0xFFFFFF,lit?0.60f:0.32f);
    uint32_t c=mixc(dark,bright,clampf(core*1.3f,0,1));
    float k=(nx*lx+ny*ly+nz*lz)-lz;
    c = k>0 ? mixc(c,0xFFFFFF,clampf(k*0.9f,0,0.6f)) : scalec(c,clampf(1.0f+k*1.4f,0.3f,1));
    /* the roll-off reflects the room, like any glossy plastic */
    float up=env_up(nx,ny,nz);
    if(tilt>0.05f && up>0.03f) c=mixc(c,0xFFFFFF,0.35f*tilt);
    /* a hard window reflection over the top third */
    if(ey<-0.30f && e>0.5f){
      float f=clampf((-0.30f-ey)/0.55f,0,1);
      c=mixc(c,0xFFFFFF,0.08f+f*0.30f);
    }
    fb_blend(x+i,y+j,c,(int)(cov*255));
  }
  fb_moulding(x,y,w,h,r,5.0f,0,255);
  int ty=y+dy+(l2? h/2-15 : h/2-8);
  static const uint32_t W[3]={0xFFFFFF,0xF4F4F4,0xD0D4E0};
  textb(l1,x+w/2,ty,2,W,3,1);
  if(l2) textb(l2,x+w/2,ty+17,2,W,3,1);
}

/* ── the backdrop ─────────────────────────────────────────────────
 *  A lit casino stage: a deep gradient, stage beams falling from above
 *  the top box, out-of-focus lights (bokeh) hanging in the dark, and a
 *  faint art-deco fan behind the reels.  The rails are smoked glass, so
 *  the stage glows through them.                                     */
static void paint_backdrop(int theme){
  uint32_t top = theme?0x061430:0x1C0C3A, mid=theme?0x040A1E:0x0E0724, bot=theme?0x02040C:0x040210;
  for(int y=0;y<FBH;y++){
    float t=(float)y/(FBH-1);
    uint32_t c = t<0.5f ? mixc(top,mid,t*2.0f) : mixc(mid,bot,(t-0.5f)*2.0f);
    for(int x=0;x<FBW;x++) fb[y*FBW+x]=c;
  }
  /* art-deco fan: rays from below the reel window */
  { float ox=FBW*0.5f, oy=FBH+120.0f;
    for(int y=0;y<FBH;y++) for(int x=0;x<FBW;x++){
      float a=fast_atan2(y-oy,x-ox)*24.0f/TAU;
      float f=a-floorf(a);
      float v=(f<0.5f?1.0f:0.0f)*0.035f*(1.0f-(float)y/FBH*0.3f);
      if(v>0) fb_add(x,y,(int)(v*(theme?120:170)),(int)(v*(theme?140:90)),(int)(v*(theme?90:200)));
    } }
  /* stage beams from above: soft cones with a hot core */
  { static const float BX[6]={-120,240,520,760,1040,1400};
    static const float BA[6]={0.30f,0.12f,-0.06f,0.06f,-0.12f,-0.30f};
    static const uint32_t BC0[6]={0xFF40C0,0x40C8FF,0xFFD060,0xFFD060,0x40C8FF,0xFF40C0};
    static const uint32_t BC1[6]={0x40A0FF,0xFFD060,0x60FFD0,0x60FFD0,0xFFD060,0x40A0FF};
    for(int b=0;b<6;b++){
      uint32_t bc=theme?BC1[b]:BC0[b];
      int cr=(bc>>16)&255, cg=(bc>>8)&255, cb=bc&255;
      for(int y=0;y<FBH;y++){
        float cxb=BX[b]+BA[b]*y*1.4f, wdt=24.0f+y*0.20f;
        float fall=1.0f-(float)y/FBH*0.75f;
        int xa=(int)(cxb-wdt*1.6f), xb=(int)(cxb+wdt*1.6f);
        for(int x=xa;x<=xb;x++){
          if(x<0||x>=FBW) continue;
          float u=fabsf(x-cxb)/wdt;
          float v=expf(-u*u*1.6f)*0.12f*fall;
          fb_add(x,y,(int)(cr*v),(int)(cg*v),(int)(cb*v));
        }
      }
    } }
  /* bokeh: soft discs with a slightly brighter rim */
  for(int i=0;i<90;i++){
    float bx=hash2(i,1,theme*7u+3u)*FBW, by=hash2(i,2,theme*7u+3u)*FBH*0.95f;
    float br=5.0f+hash2(i,3,theme*7u+3u)*hash2(i,4,theme*7u+3u)*26.0f;
    static const uint32_t K0[5]={0xFFC060,0xFF60B0,0x60C0FF,0xFFFFFF,0xC080FF};
    static const uint32_t K1[5]={0xFFD870,0xFFF0B0,0x80D8FF,0xFFFFFF,0x70FFD8};
    uint32_t bc=(theme?K1:K0)[i%5];
    float inten=0.10f+hash2(i,5,theme*7u+3u)*0.22f;
    int cr=(bc>>16)&255, cg=(bc>>8)&255, cb=bc&255;
    for(int y=(int)(by-br-1);y<=(int)(by+br+1);y++) for(int x=(int)(bx-br-1);x<=(int)(bx+br+1);x++){
      if(x<0||y<0||x>=FBW||y>=FBH) continue;
      float d=sqrtf((x-bx)*(x-bx)+(y-by)*(y-by))/br;
      if(d>1.0f) continue;
      float v=(0.75f+0.25f*smooth01(0.7f,0.95f,d))*clampf((1.0f-d)*6.0f,0,1)*inten;
      fb_add(x,y,(int)(cr*v),(int)(cg*v),(int)(cb*v));
    }
  }
  /* free spins: a night sky of pin-point stars */
  if(theme) for(int i=0;i<260;i++){
    int x=(int)(hash2(i,9,1u)*FBW), y=(int)(hash2(i,10,1u)*FBH);
    int v=(int)(60+hash2(i,11,1u)*190);
    fb_add(x,y,v,v,v*9/10);
    if(v>200){ fb_add(x+1,y,v/3,v/3,v/3); fb_add(x-1,y,v/3,v/3,v/3); fb_add(x,y+1,v/3,v/3,v/3); fb_add(x,y-1,v/3,v/3,v/3); }
  }
  /* vignette */
  for(int y=0;y<FBH;y++) for(int x=0;x<FBW;x++){
    float vx=(x-FBW*0.5f)/(FBW*0.5f), vy=(y-FBH*0.45f)/(FBH*0.55f);
    fb_blend(x,y,0x000000,(int)(clampf((vx*vx+vy*vy)*0.45f-0.10f,0,0.7f)*255));
  }
}

/*  The marquee box's static half: a chrome-framed lightbox with a
 *  sunburst behind the sign and bulb sockets round the edge.  The sign,
 *  ticker, searchlights and lit bulbs are live: see draw_marquee().  */
static void paint_marquee_box(int theme){
  fb_softshadow(4,2,FBW-8,MQH-4,15,6,140);
  fb_rrectg(8,5,FBW-16,MQH-10,12,theme?0x0A2A5A:0x7A0A1E,theme?0x020816:0x1E0208,255);
  { float ox=FBW*0.5f, oy=MQH*0.5f;
    for(int y=6;y<MQH-6;y++) for(int x=10;x<FBW-10;x++){
      float a=fast_atan2((y-oy)*6.0f,x-ox)*32.0f/TAU, f=a-floorf(a);
      float dist=fabsf(x-ox)/(FBW*0.5f);
      float v=(f<0.5f?0.10f:0.0f)*(1.0f-dist)+0.20f*expf(-dist*dist*9.0f);
      if(v>0) fb_add(x,y,(int)(v*(theme?90:255)),(int)(v*(theme?150:90)),(int)(v*(theme?255:60)));
    } }
  for(int j=0;j<18;j++)
    for(int i=10;i<FBW-10;i++) fb_blend(i,7+j,0xFFFFFF,(18-j)*2);
  fb_moulding(3,1,FBW-6,MQH-2,16,5.0f,1,255);
  fb_rframe(10,7,FBW-20,MQH-14,10,1.0f,0x000000,160);
  /* bulb sockets */
  int n=(FBW-40)/26;
  for(int i=0;i<n;i++){
    int x=22+i*26;
    for(int sd=0;sd<2;sd++){
      int y = sd? MQH-9 : 9;
      for(int j=-4;j<=4;j++) for(int k=-4;k<=4;k++){
        int d2=j*j+k*k;
        if(d2<=16) fb_blend(x+k,y+j,d2>=10?0xA8A8B8:0x1A1210,230);
      }
    }
  }
}

static void paint_reel_window(void){
  /* a heavy frame: shadow, chrome, a gold inner bezel, a dark lip */
  fb_softshadow(GX-22,GY-22,GW+44,GH+44,26,10,200);
  fb_rrect(GX-22,GY-22,GW+44,GH+44,26,0x05060A,255);
  fb_moulding(GX-22,GY-22,GW+44,GH+44,26,9.0f,0,255);
  fb_moulding(GX-12,GY-12,GW+24,GH+24,17,7.0f,1,255);
  fb_rframe(GX-5,GY-5,GW+10,GH+10,11,3.0f,0x0A0602,255);
  /* corner studs: little gold domes where the mouldings meet */
  static const int SX[4]={GX-17,GX+GW+17,GX-17,GX+GW+17}, SY[4]={GY-17,GY-17,GY+GH+17,GY+GH+17};
  for(int k=0;k<4;k++)
    for(int j=-6;j<=6;j++) for(int i=-6;i<=6;i++){
      float d=sqrtf((float)(i*i+j*j))/6.0f;
      if(d>1.0f) continue;
      float nz=sqrtf(1.0f-d*d*0.9f), nx=i/6.0f*0.95f, ny=j/6.0f*0.95f;
      uint32_t c=env_map(env_up(nx,ny,nz),1);
      fb_blend(SX[k]+i,SY[k]+j,c,(int)(255*clampf((1.0f-d)*6.0f,0,1)));
    }
  /* the drums themselves - bright, which is the whole point */
  for(int y=GY;y<GY+GH;y++){
    uint32_t c=drum_shade(y);
    for(int r=0;r<NREEL;r++){
      int x0=GX+r*CW+2, x1=GX+(r+1)*CW-2;
      for(int x=x0;x<x1;x++){
        float e=fabsf((x-(x0+x1)*0.5f)/((x1-x0)*0.5f));
        uint32_t cc = e>0.90f ? mixc(c,bgTheme?0x8A6A30:0x9A9484,(e-0.90f)/0.10f*0.55f) : c;
        fb_px(x,y,cc);
      }
    }
  }
  /* cell frames: a thin gold rule between every cell */
  for(int r=0;r<NREEL;r++) for(int row=0;row<NROW;row++)
    fb_rframe(GX+r*CW+4,GY+row*CH+3,CW-8,CH-6,10,1.2f,bgTheme?0xC89A40:0xC9B47A,140);
  /* drum gaps: deep shadow so five separate reels read as five reels */
  for(int r=0;r<=NREEL;r++){
    int gx=GX+r*CW;
    for(int y=GY;y<GY+GH;y++)
      for(int k=-3;k<=2;k++){
        int a = (k==-3||k==2)?70:(k==-2||k==1)?160:235;
        fb_blend(gx+k,y,0x0A0C16,a);
      }
  }
}

/* the four tiers of the progressive sign, in their own colours */
static const uint32_t TIERC[NJP]={0xE8F0FF,0xC050FF,0xFF3040,0x30E8B0};

static void neon_frame(int x,int y,int w,int h,float r,uint32_t col,int rainbow){
  static const uint32_t HUE[6]={0xFF4060,0xFFB030,0xE0FF40,0x40FFA0,0x40B0FF,0xB050FF};
  for(int k=6;k>=1;k--){
    int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
    float v=0.10f*(7-k)/6.0f;
    for(int j=-k;j<h+k;j++) for(int i=-k;i<w+k;i++){
      if(j>=0&&j<h&&i>=0&&i<w && i>3&&i<w-4&&j>3&&j<h-4) continue;
      float d=rr_sdf(x+i+0.5f,y+j+0.5f,x+w*0.5f,y+h*0.5f,w*0.5f,h*0.5f,r);
      if(fabsf(d)>k) continue;
      if(rainbow){
        float t=(float)(i+j*2)/(w+h*2)*6.0f; t-=floorf(t/6.0f)*6.0f;
        int q=(int)t; uint32_t hc=mixc(HUE[q%6],HUE[(q+1)%6],t-q);
        cr=(hc>>16)&255; cg=(hc>>8)&255; cb=hc&255;
      }
      fb_add(x+i,y+j,(int)(cr*v),(int)(cg*v),(int)(cb*v));
    }
  }
  fb_rframe(x,y,w,h,r,2.0f,rainbow?0xFFFFFF:mixc(col,0xFFFFFF,0.5f),255);
}

static void paint_rails(void){
  /* ── left rail: the progressive sign ───────────────────────────── */
  glass_rail(LRX,JPY,RAILW,JPH,0x2A1850,0x080418,1,"PROGRESSIVE JACKPOTS",0xFFE9A8);
  static const char*JN[NJP]={"ULTIMATE","MEGA","MAJOR","MINOR"};
  static const char*JH[NJP]={"5 ULTIMATES - 100,000X","7+ JACKPOTS - 200X",
                             "6 JACKPOTS - 40X","5 JACKPOTS - 5X"};
  for(int i=0;i<NJP;i++){
    int y = i==0 ? JPY+24 : JPY+88+(i-1)*54;
    int h = i==0 ? 46 : 34;
    led_window(LRX+8,y,RAILW-16,h);
    neon_frame(LRX+8,y,RAILW-16,h,7,TIERC[i],i==0);
    int ly = y+h+3;
    text(JN[i],LRX+12,ly,2,mixc(TIERC[i],0xFFFFFF,0.25f),0,1);
    text(JH[i],LRX+RAILW-12,ly+4,1,0xA8B0D0,2,1);
  }
  /* bulb sockets down the ladder's flanks */
  for(int i=0;i<9;i++){
    int y=JPY+14+i*28;
    for(int sd=0;sd<2;sd++){
      int x= sd? LRX+RAILW-5 : LRX+5;
      for(int j=-3;j<=3;j++) for(int k=-3;k<=3;k++)
        if(j*j+k*k<=9) fb_blend(x+k,y+j,j*j+k*k>=6?0x9A9AAA:0x3A2A08,230);
    }
  }
  /* feature meters, at rest; the lit versions are drawn over these */
  glass_rail(LRX,FEATY,134,FEATH,0x16203A,0x060A16,0,"FREE SPINS",0x9AA4C8);
  glass_rail(LRX+RAILW-134,FEATY,134,FEATH,0x16203A,0x060A16,0,"MULTIPLIER",0x9AA4C8);

  /* how to win */
  { int h=GY+GH-HOWY;
    glass_rail(LRX,HOWY,RAILW,h,0x16203A,0x060A16,0,"HOW TO WIN",0xFFD98A);
    /* one line per way to win: the ways rule, the wild's own multiplier,
       and the four symbol-started features (7 STRIKE and the GAMBLE
       explain themselves when they happen) */
    static const char*HW[6]={"WAYS PAY LEFT TO RIGHT","WILD 7 = X2 PER WILD","3 STARS = FREE SPINS",
                             "3 CROWNS = PICK BONUS","6 COINS = HOLD & SPIN","3 WHEELS = THE WHEEL"};
    static const uint32_t HC[6]={0xFFFFFF,0xFFC24A,0x9CF57A,0xE0A0FF,0xFFD86A,0xE070FF};
    for(int i=0;i<6;i++) text(HW[i],LRX+RAILW/2,HOWY+30+i*26,2,HC[i],1,1); }

  /* ── right rail: meters ────────────────────────────────────────── */
  static const char*MN[3]={"CREDITS","BET","WIN"};
  static const uint32_t MT[3]={0x0E2A3A,0x3A0A10,0x3A1A08};
  for(int i=0;i<3;i++){
    int y=METY+i*METH;
    glass_rail(RRX,y,RAILW,METH-4,MT[i],0x04060C,1,MN[i],0xFFD98A);
    led_window(RRX+8,y+20,RAILW-16,44);
  }
  /* controls crib, wholly static */
  { glass_rail(RRX,CTLY,RAILW,CTLH,0x16203A,0x060A16,0,"CONTROLS",0xFFD98A);
    static const char*CK[6]={"START / A","L / R","X","Y","SELECT","B"};
    static const char*CV[6]={"SPIN","BET","MAX / GAMBLE","ADD CREDITS","PAYS","SLAM"};
    for(int i=0;i<6;i++){
      int y=CTLY+30+i*23;
      text(CK[i],RRX+12,y,2,0x9FE8FF,0,1);
      text(CV[i],RRX+RAILW-12,y,2,0xE8ECF8,2,1);
    } }
  /* last win panel, filled in live */
  glass_rail(RRX,LWY,RAILW,GY+GH-LWY,0x16203A,0x060A16,0,"LAST WIN",0x9AA4C8);
}

static void paint_deck(void){
  fb_softshadow(4,DECKY,FBW-8,FBH-DECKY-4,15,6,160);
  fb_glass(6,DECKY+2,FBW-12,FBH-DECKY-8,13,0x1E2448,0x04060E,235);
  fb_moulding(4,DECKY,FBW-8,FBH-DECKY-4,15,4.0f,1,255);
  /* red / white / blue rake along the deck lip, as on the reference cabinet */
  for(int j=0;j<6;j++){
    uint32_t c = (j<2)?0xC81828:((j<4)?0xE8ECF8:0x1A3A9A);
    for(int i=16;i<FBW-16;i++) fb_blend(i,DECKY+6+j,c,150);
  }
  /* a lit recess behind the SPIN dome */
  for(int j=-44;j<=44;j++) for(int i=-44;i<=44;i++){
    float d=sqrtf((float)(i*i+j*j))/44.0f;
    if(d>1.0f) continue;
    fb_blend(SPINX+i,SPINY+j,0x000000,(int)(200*clampf((1.0f-d)*4.0f,0,1)));
  }
  fb_moulding(SPINX-45,SPINY-45,90,90,45,4.0f,1,255);
}

static void build_bg_theme(int theme){
  bgTheme=theme;
  paint_backdrop(theme);
  paint_marquee_box(theme);
  paint_reel_window();
  paint_rails();
  paint_deck();
  /* the deck buttons, at rest; the lit variant is captured as a sprite */
  for(int i=0;i<NBTN;i++){
    if(theme==0){
      uint32_t save[260*80];
      int bx=BTNX[i]-4, by=BTNY-4, bw=BTNW[i]+8, bh=BTNH+12;
      for(int j=0;j<bh;j++) for(int k=0;k<bw;k++) save[j*bw+k]=fb[(by+j)*FBW+bx+k];
      paint_button(BTNX[i],BTNY,BTNW[i],BTNH,BTNL1[i],BTNL2[i],BTNC[i],1);
      grab_sprite(&btnspr[i][1],bx,by,bw,bh);
      for(int j=0;j<bh;j++) for(int k=0;k<bw;k++) fb[(by+j)*FBW+bx+k]=save[j*bw+k];
    }
    paint_button(BTNX[i],BTNY,BTNW[i],BTNH,BTNL1[i],BTNL2[i],BTNC[i],0);
  }
  bgTheme=0;
}

static uint32_t *bgBase=NULL, *bgFree=NULL;
static void build_marquee(void);
static void build_fire(void);
static void build_rays(void);
static void build_pick_assets(void);
static void build_bg(void){
  build_titles();
  build_fire();
  build_rays();
  make_glow(&bulbspr[0],9,0xFFD890,1.8f,0.6f);
  make_glow(&bulbspr[1],9,0xFF5030,1.8f,0.6f);
  make_glow(&sparkspr,7,0xFFFFFF,2.5f,0.8f);
  if(!bgFree) bgFree=(uint32_t*)malloc(sizeof bg);
  if(!bgBase) bgBase=(uint32_t*)malloc(sizeof bg);
  build_bg_theme(1);
  if(bgFree) memcpy(bgFree,fb,sizeof bg);
  build_bg_theme(0);
  if(bgBase) memcpy(bgBase,fb,sizeof bg);
  memcpy(bg,fb,sizeof bg);
  build_marquee();
  build_pick_assets();
}

/* half-size blit, 2x2 box filtered — used by the paytable */
static void blit_half(const spr_t*s,int dx,int dy){
  if(!s->px) return;
  /* source rows y, y+1 land on screen row dy+y/2: keep this band's */
  int y0=0, y1=s->h-1;
  if(y0<2*(clip_y0-dy)) y0=2*(clip_y0-dy);
  if(y1>2*(clip_y1-dy)) y1=2*(clip_y1-dy);
  for(int y=y0;y<y1;y+=2) for(int x=0;x+1<s->w;x+=2){
    int r=0,g=0,b=0,a=0;
    for(int j=0;j<2;j++) for(int i=0;i<2;i++){
      const uint8_t*p=s->px+((y+j)*s->w+(x+i))*4;
      r+=p[0]*p[3]; g+=p[1]*p[3]; b+=p[2]*p[3]; a+=p[3];
    }
    if(a<=0) continue;
    fb_blend(dx+x/2,dy+y/2,RGB(r/a,g/a,b/a),a/4);
  }
}

/* ═══ RENDER ══════════════════════════════════════════════════════ */
/* WILD, SCATTER, CROWN, JACKPOT and ULTIMATE are the symbols that
   change what happens next, so they get an accent colour, a breathing
   aura and a lit cell frame.  The colour also cues the feature: gold
   wild, green free spins, violet bonus, red jackpot, ice ultimate.  */
static inline int is_special(int sy){ return sy==SY_SEVEN || sy>=SY_STAR; }
static uint32_t special_col(int sy){
  switch(sy){
  case SY_SEVEN:   return 0xFFC040;
  case SY_STAR:    return 0x7CFF6A;
  case SY_CROWN:   return 0xC060FF;
  case SY_JACKPOT: return 0xFF6A30;
  case SY_COIN:    return 0xFFD24A;    /* gold: hold & spin */
  case SY_WHEEL:   return 0xA86CFF;    /* violet: wheel of 7s */
  default:         return 0x9AF0FF;
  }
}
static const uint32_t WINCOL[8] = {
  0xFFE04A,0x4FE8FF,0xFF5AA8,0x7CFF6A,0xFF8A2A,0xB07CFF,0x59D9FF,0x8AFFC0
};

static inline int cellcx(int r){ return GX + r*CW + CW/2; }
static inline int cellcy(int row){ return GY + row*CH + CH/2; }

/*  A radial wash of the accent colour, blended rather than added.  The
 *  drums are cream, so an additive glow would simply disappear; tinting
 *  the paper is what actually reads.                                  */
static void cell_wash(int cx,int cy,uint32_t col,float amt){
  blit_wash(&washspr,cx-washspr.w/2,cy-washspr.h/2,GY,GY+GH,col,amt);
}

/* ── reel effects ──────────────────────────────────────────────────
 *  A tileable strip of fire, baked once: the anticipation reel burns
 *  up from the bottom, an expanded wild reel is a column of flame.   */
#define FIREW (CW-4)
#define FIREH 192
#define NFF 6
#define FFH 230
static spr_t fireStrip, fireFade[NFF];
static float artT;              /* the art clock: seconds, never reset */
static int   flipIdx = -1;      /* the pick panel that is turning over */
static void build_fire(void){
  static const uint32_t FIRE[6]={0x3A0200,0xB01404,0xFF4A08,0xFF9A18,0xFFE070,0xFFFFE0};
  spr_t*s=&fireStrip;
  s->w=FIREW; s->h=FIREH;
  s->px=(uint8_t*)calloc((size_t)FIREW*FIREH,4);
  if(!s->px) return;
  for(int y=0;y<FIREH;y++) for(int x=0;x<FIREW;x++){
    /* tileable in y: blend two noise samples a period apart */
    float u=(float)x/FIREW, v=(float)y/FIREH;
    float n0=fbm(u*5.0f,v*6.0f,31u,4), n1=fbm(u*5.0f,v*6.0f+6.0f,31u,4);
    float n=lerpf(n0,n1,v);
    float edge=fabsf(u-0.5f)*2.0f;                 /* hotter at the sides */
    float I=clampf((n-0.34f)*2.4f,0,1)*(0.45f+0.55f*edge*edge);
    if(I<0.04f) continue;
    uint32_t c=ramp(FIRE,6,I);
    uint8_t*o=s->px+((size_t)y*FIREW+x)*4;
    o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
    o[3]=(uint8_t)(smooth01(0.04f,0.6f,I)*235);
  }
  spr_bounds(s);
  /* flames from a reel's foot: tongues under an envelope that fades out
     upward, six phases of the turbulence to cycle through */
  for(int k=0;k<NFF;k++){
    spr_t*f=&fireFade[k];
    f->w=FIREW; f->h=FFH;
    f->px=(uint8_t*)calloc((size_t)FIREW*FFH,4);
    if(!f->px) continue;
    float ph=k*0.21f;
    for(int y=0;y<FFH;y++) for(int x=0;x<FIREW;x++){
      float u=(float)x/FIREW, v=(float)(FFH-y)/FFH;
      float tongues=0.30f+0.70f*fbm(u*5.0f+3.1f,ph*0.9f,41u,2);
      float turb=fbm(u*7.0f,v*3.6f-ph*2.8f,43u,4);
      float I=clampf((tongues-v)/(tongues+0.001f)*1.3f+(turb-0.5f)*1.1f,0,1);
      if(I<0.05f) continue;
      uint32_t c=ramp(FIRE,6,I);
      uint8_t*o=f->px+((size_t)y*FIREW+x)*4;
      o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
      o[3]=(uint8_t)(smooth01(0.05f,0.5f,I)*245);
    }
    spr_bounds(f);
  }
}

/* a stable pseudo-random value for the draw code: no RNG, no state */
static inline float dhash(int a,int b){ return hash2(a,b,0x5EEDu); }

/* a flat blend over a rectangle, straight row loops, clipped to the band */
static void fb_shade_rect(int x,int y,int w,int h,uint32_t col,int a){
  if(a<=0) return;
  if(a>255) a=255;
  int x0=x<0?0:x, x1=x+w>FBW?FBW:x+w, y0=y<clip_y0?clip_y0:y, y1=y+h>clip_y1?clip_y1:y+h;
  int sr=(col>>16)&255, sg=(col>>8)&255, sb=col&255;
  for(int j=y0;j<y1;j++){
    uint32_t*q=fb+(size_t)j*FBW;
    for(int i=x0;i<x1;i++){
      uint32_t d=q[i];
      int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
      q[i]=RGB(dr+((sr-dr)*a>>8), dg+((sg-dg)*a>>8), db+((sb-db)*a>>8));
    }
  }
}

/* additive rectangle outline, for glows that must not dull the drums */
static void add_frame(int x,int y,int w,int h,uint32_t col,int k){
  int cr=((col>>16)&255)*k>>8, cg=((col>>8)&255)*k>>8, cb=(col&255)*k>>8;
  if(!(cr|cg|cb)) return;
  for(int i=0;i<w;i++){ fb_add(x+i,y,cr,cg,cb); fb_add(x+i,y+h-1,cr,cg,cb); }
  for(int j=1;j<h-1;j++){ fb_add(x,y+j,cr,cg,cb); fb_add(x+w-1,y+j,cr,cg,cb); }
}

/* fire rising through a column of the window, scrolled by the art clock */
static void fire_column(int r,int ytop,int alpha,float speed){
  int x=GX+r*CW+2, y1=GY+GH;
  int base=y1-((int)(artT*speed)%FIREH);
  for(int y=base; y+FIREH>ytop; y-=FIREH)
    blit(&fireStrip,x,y,ytop,y1,alpha,0,0.0f);
}
/* flames licking up from the bottom of a reel, cycling baked frames */
static void fire_base(int r,int alpha){
  const spr_t*f=&fireFade[((int)(artT*14.0f))%NFF];
  blit(f,GX+r*CW+2,GY+GH-f->h,GY,GY+GH,alpha,0,0.0f);
}

/*  Electric arcs down both sides of a reel: a jagged polyline re-rolled
 *  twenty times a second from a hash of the frame, never from the RNG. */
static void arc_edge(int x,int seed,uint32_t col,int a){
  int fr=(int)(artT*20.0f);
  float px=(float)x, py=(float)GY;
  for(int s=1;s<=20;s++){
    float ny=GY+GH*s/20.0f;
    float nx=x+(dhash(seed*97+s,fr)-0.5f)*14.0f;
    if(s==20) nx=(float)x;
    fb_line(px,py,nx,ny,5,col,a/3);
    fb_line(px,py,nx,ny,3,col,a*2/3);
    fb_line(px,py,nx,ny,1,0xFFFFFF,a);
    px=nx; py=ny;
  }
}

/*  The anticipation reel: when two scatters or crowns are already
 *  showing and this reel could make it three, it is lit like a fuse and
 *  the rest of the window drops into shadow so it has the stage.
 *  Draw-only - derived from the same partial_special() the hold uses. */
static void antic_under(int r,float heat){
  int x=GX+r*CW+2, w=CW-4;
  fb_shade_rect(x,GY,w,GH,0x3A0600,(int)(120*heat));
}
static void antic_over(int r,float heat,int lead){
  int x=GX+r*CW, w=CW;
  float pl=0.55f+0.45f*sinf(artT*16.0f);
  if(opt_limiter) pl=0.7f+pl*0.3f;
  if(lead){
    /* everything else steps back into the dark */
    for(int q=0;q<NREEL;q++){
      if(q==r) continue;
      fb_shade_rect(GX+q*CW,GY,CW,GH,0x06040C,110);
    }
  }
  /* heat over the reel: hottest at its edges and its foot */
  for(int y=GY;y<GY+GH;y++){
    if(y<clip_y0||y>=clip_y1) continue;
    float b=(float)(y-GY)/GH; b=b*b*b;
    for(int i=2;i<w-2;i++){
      float e=fabsf((i-w*0.5f)/(w*0.5f)); e=e*e*e*e;
      float I=heat*pl*(0.10f+0.9f*e+0.55f*b)*120.0f;
      fb_add(x+i,y,(int)I,(int)(I*0.42f),(int)(I*0.08f));
    }
  }
  fire_base(r,(int)(235*heat));
  /* a hot glow spilling out over the bezel and the darkened reels */
  for(int k=1;k<=16;k++){
    int kk=(int)((17-k)*(17-k)*1.1f*heat*pl);
    add_frame(x-k+2,GY-k,w+2*k-4,GH+2*k,0xFF8A2C,kk);
  }
  fb_rframe(x+1,GY+1,w-2,GH-2,8,6.0f,mixc(0xFF7A10,0xFFE878,pl),(int)(255*heat));
  fb_rframe(x+3,GY+3,w-6,GH-6,6,2.0f,0xFFFFFF,(int)(235*heat*pl));
  /* electric arcs down both edges */
  arc_edge(x+4,r*2,0x70D0FF,(int)(255*heat));
  arc_edge(x+w-5,r*2+1,0x70D0FF,(int)(255*heat));
  /* sparks running round the frame */
  float per=2.0f*(w+GH);
  for(int i=0;i<12;i++){
    float d=fmodf(artT*560.0f+i*per/12.0f,per);
    int sx,sy;
    if(d<w){ sx=x+(int)d; sy=GY; }
    else if(d<w+GH){ sx=x+w; sy=GY+(int)(d-w); }
    else if(d<2*w+GH){ sx=x+w-(int)(d-w-GH); sy=GY+GH; }
    else { sx=x; sy=GY+GH-(int)(d-2*w-GH); }
    blit_add(&sparkspr,sx-7,sy-7,(int)(255*heat));
  }
}

/* the free-spins expanded wild: a reel turned into a column of fire */
static void expand_under(int r){
  int x=GX+r*CW+2, w=CW-4;
  float pl=0.55f+0.45f*sinf(artT*7.0f);
  if(opt_limiter) pl=0.65f+pl*0.35f;
  for(int y=GY;y<GY+GH;y++){
    if(y<clip_y0||y>=clip_y1) continue;
    float ey=(float)(y-GY)/GH;
    uint32_t c=mixc(0xFFD040,0xC01808,ey);
    int a=(int)(150+60*pl);
    for(int i=0;i<w;i++) fb_blend(x+i,y,c,a);
  }
  fire_column(r,GY,200,110.0f);
}
static void expand_over(int r){
  int x=GX+r*CW, w=CW;
  float pl=0.55f+0.45f*sinf(artT*7.0f);
  if(opt_limiter) pl=0.65f+pl*0.35f;
  for(int k=1;k<=10;k++) add_frame(x-k,GY-k,w+2*k,GH+2*k,0xFFC040,(int)((11-k)*16*pl));
  fb_rframe(x+1,GY+1,w-2,GH-2,8,4.0f,mixc(0xFFC83A,0xFFFFFF,pl*0.5f),255);
  fb_frame(x+6,GY+6,w-12,GH-12,1,0xFFF0C0,(int)(120*pl));
  for(int i=0;i<8;i++){                       /* embers rising */
    float t=fmodf(artT*0.6f+dhash(r,i),1.0f);
    int ex=x+8+(int)(dhash(r*7,i)*(w-16)+sinf(artT*3.0f+i)*6.0f), ey=GY+GH-(int)(t*GH);
    blit_add(&sparkspr,ex-7,ey-7,(int)(230*(1.0f-t)));
  }
}

static void draw_reels(void){
  /* which reel, if any, is the anticipation reel: the next one still
     turning once two scatters or crowns are showing */
  int ps = (G.state==ST_SPIN) ? partial_special() : 0;
  int anticR=-1;
  if(ps>=2) for(int r=3;r<NREEL;r++) if(G.rstate[r]==1){ anticR=r; break; }

  for(int r=0;r<NREEL;r++){
    if(r==anticR) antic_under(r,1.0f);
    if(G.expand[r]) expand_under(r);

    int base=(int)floorf(G.rpos[r]);
    float frac=G.rpos[r]-base;
    int blurred = G.reelBlur[r] > 0.35f;
    for(int j=0;j<NROW+1;j++){
      int idx = stripAt(r, base - j + 1);
      int sy  = GY + (j-1)*CH + (int)(frac*CH) + SOY;
      int sx  = GX + r*CW + SOX;
      if(G.expand[r] && !blurred) idx = SY_SEVEN;
      if(blurred){
        blit(G.rstate[r]==1?&symb2[idx]:&symb[idx], sx,sy-BLURPAD, GY,GY+GH, 255,0,0.0f);
        continue;
      }
      int cell = r*7 + ((base-j+1)%STRIPLEN+STRIPLEN)%STRIPLEN;   /* stable per strip slot */
      if(is_special(idx) && !G.expand[r]){
        uint32_t ac=special_col(idx);
        float pl=0.60f+0.40f*sinf(artT*4.4f + r*0.7f + j*0.5f);
        if(opt_limiter) pl=0.70f+pl*0.30f;
        int cyy=sy-SOY;
        /* tinted cell backing + lit frame, both clipped to the window */
        if(cyy>=GY-CH && cyy<=GY+GH){
          cell_wash(sx+SYMW/2, sy+SYMH/2, ac, 0.34f*pl);
          if(cyy>=GY-2 && cyy+CH<=GY+GH+2){
            fb_rframe(GX+r*CW+4,cyy+3,CW-8,CH-6,10,2.5f,ac,(int)(225*pl));
            fb_rframe(GX+r*CW+6,cyy+5,CW-12,CH-10,8,1.0f,0xFFFFFF,(int)(120*pl));
          }
        }
      }
      const spr_t*sp=&sym[idx];
      if(idx==SY_SEVEN) sp=&symfl[((int)(artT*9.0f)+cell*3)&(NFLAME-1)];
      blit(sp, sx,sy, GY,GY+GH, 255,0,0.0f);
      if(is_special(idx)){
        /* a shine sweeps each special every few seconds, cell by cell */
        float ph=fmodf(artT+dhash(cell,1)*3.1f,3.1f);
        if(ph<0.75f){
          float pos=-30.0f+ph/0.75f*(SYMW+90.0f);
          shine_sprite(sp,sx,sy,GY,GY+GH,pos,11,opt_limiter?120:170,200);
        }
        /* and a glint turns slowly round the medal */
        float ang=artT*1.6f + r + j;
        float rad=(float)SYMW*0.44f;
        int gx=sx+SYMW/2+(int)(cosf(ang)*rad), gy=sy+SYMH/2+(int)(sinf(ang)*rad);
        if(gy>GY+6 && gy<GY+GH-6) blit_add(&sparkspr,gx-7,gy-7,opt_limiter?150:230);
      }
    }
  }
  for(int r=0;r<NREEL;r++){
    if(G.expand[r]) expand_over(r);
    if(r==anticR) antic_over(r,1.0f,1);
  }

  /* glass: shade at the lips, a curved-glass sheen, a diagonal glint */
  for(int j=0;j<12;j++){
    int a=(12-j)*9;
    for(int i=0;i<GW;i++){
      fb_blend(GX+i,GY+j,0x000814,a);
      fb_blend(GX+i,GY+GH-1-j,0x000814,a);
    }
  }
  for(int j=0;j<26;j++){
    int a=(int)(16.0f*sinf((j+1)/27.0f*3.14159f));
    for(int i=0;i<GW;i++) fb_blend(GX+i,GY+14+j,0xFFFFFF,a);
  }
  for(int y=0;y<GH;y++){
    int xs = GX + (int)(y*0.95f) - 40;
    for(int k=0;k<64;k++){
      int x=xs+k;
      if(x<GX||x>=GX+GW) continue;
      int a = 15 - abs(k-32)/3;
      if(a>0) fb_blend(x,GY+y,0xFFFFFF,a);
    }
  }
}

/*  Light up one win: tint and frame every cell on a winning path, then
 *  thread a line from each lit cell to the lit cells it feeds on the reel
 *  to its right — never within a reel, never leftwards — so the eye reads
 *  the way the win was made.                                          */
static void __attribute__((unused)) light_cluster(uint32_t m,uint32_t c,float pulse,int heavy){
  /* w7_fx.c: lit cells, path lines with running light, the symbol pop
     and the X2 badges; the pop is timed from when the win came up */
  fx_light_cluster(m,c,pulse,heavy,G.state==ST_SHOWWIN?G.showT:G.t);
}

static void draw_wins(void){ fx_wins_draw(); }   /* w7_fx.c */

/* the rails lower half: feature meters and the last-win panel */
/*  Only the values move.  The panels themselves are painted into the
 *  background once — redrawing rounded-rect gradients every frame
 *  measured at a quarter of the whole budget for art that never
 *  changed.                                                            */
static void draw_features(void){
  char b[48];
  int on = G.inFree || G.freeSpins>0;
  if(on) rail_panel(LRX,FEATY,134,FEATH,0x0D3A18,0x041206,0x7CFF6A,"FREE SPINS",0xBFFFB0);
  snprintf(b,sizeof b,"%d",G.freeSpins);
  textb(b,LRX+67,FEATY+26,4,GREENG,4,1);

  /*  The meter is the loudest thing in the rail once it is live: the
   *  panel takes the colour of the tier, its rim breathes, and the
   *  number pops up a size and drops back each time it climbs.
   *
   *  It only ever means something in two places - the free spins and
   *  the pick round - so it shows which one is live, and outside them
   *  it sits at X1 saying where it comes alive.  (It used to go on
   *  showing a pick round's X2 all through the base game afterwards.)
   *  The free-spins meter stays up through the FREE SPINS COMPLETE
   *  screen, saying it resets, so the drop back to X1 is seen.        */
  int fsLive = G.inFree || (G.state==ST_BONUSEND && G.banner==3) ||
               G.state==ST_FSINTRO;
  int pkLive = G.state==ST_BONUS || (G.state==ST_BONUSEND && G.banner==4);
  int m = fsLive ? G.fsMult : (pkLive ? G.pickMult : 1);
  if(G.state==ST_FSINTRO && !G.inFree) m=1;   /* a fresh feature starts at X1 */
  if(m<1) m=1;
  if(m>1){
    static const uint32_t MHI[4]={0x143A6A,0x0D3A18,0x4A3608,0x5A1030};
    static const uint32_t MLO[4]={0x040A1E,0x041206,0x1A1202,0x1A0410};
    static const uint32_t MRM[4]={0x59D9FF,0x7CFF6A,0xFFC24A,0xFF5AA8};
    int t = m>=5?3:(m>=4?2:(m>=3?1:0));
    float pl=0.5f+0.5f*sinf(G.t*7.0f);
    if(opt_limiter) pl=0.45f+pl*0.55f;
    rail_panel(LRX+RAILW-134,FEATY,134,FEATH,MHI[t],MLO[t],
               mixc(MRM[t],0xFFFFFF,pl*0.65f),"MULTIPLIER",0xFFFFFF);
    for(int k=0;k<3;k++)                      /* a lit ring while it is up */
      fb_rframe(LRX+RAILW-134-k,FEATY-k,134+k*2,FEATH+k*2,12+k,1.0f,
                MRM[t],(int)(140*pl)-k*30);
  }
  snprintf(b,sizeof b,"X%d",m);
  float pop = G.multUp>0 ? G.multUp*0.55f : 0.0f;
  int px = 4 + (int)(pop*3.0f);
  const uint32_t *mg = m>=4?GOLDG:(m>1?ICEG:SILVERG);
  int mcx = LRX+RAILW-67;
  textb(b,mcx,FEATY+19-(px-4)*4,px,mg,m>=4?5:4,1);

  /*  The ladder: five lamps, X1 to X5, lit up to where the free-spins
   *  meter stands, the top one breathing.  Up is a lamp coming on, the
   *  reset at the end is the row going dark - both visible at a glance. */
  { int lit = fsLive ? m : 0;
    float br=0.55f+0.45f*sinf(G.t*6.0f);
    if(opt_limiter) br=0.75f+br*0.25f;
    for(int k=0;k<FS_MAXMULT && k<5;k++){
      int lx=mcx-40+k*20, ly=FEATY+56;
      uint32_t c = k<lit ? (k==lit-1 ? mixc(0xFFB020,0xFFFFFF,br*0.5f) : 0xFFC24A)
                         : 0x2A2410;
      for(int j=-5;j<=5;j++) for(int i=-5;i<=5;i++){
        int d=i*i+j*j;
        if(d>25) continue;
        fb_blend(lx+i,ly+j, d>16 ? 0x000000 : c, d>16 ? 200 : 255);
      }
      if(k<lit) fb_blend(lx-1,ly-2,0xFFFFFF,200);          /* a glint on the lit ones */
    }
    const char*cap;
    uint32_t cc=0xC8D2F0;
    if(fsLive && G.state==ST_BONUSEND){ cap=(((int)(G.t*3.0f))&1)?"RESETS TO X1":""; cc=0xFF9A9A; }
    else if(fsLive && m>=FS_MAXMULT)  { cap="MAXIMUM!";        cc=0xFFE9A8; }
    else if(fsLive)                   { cap="WILD REEL = +1";  cc=0xBFFFB0; }
    else if(pkLive)                   { cap="PICK BONUS";      cc=0xE0B0FF; }
    else                                cap="IN FREE SPINS";
    text(cap,mcx,FEATY+65,1,cc,1,1);
  }

  /* last win: the featured cluster while a win shows, else the last total.
     It spells the sum out: ways, the wild boost, the free-spins meter,
     then what it paid - so every multiplier in it is on the screen.   */
  int show = (G.state==ST_SHOWWIN && G.nWin>0);
  if(show){
    int w=G.showIdx;
    uint32_t c=WINCOL[w&7];
    rail_panel(RRX,LWY,RAILW,GY+GH-LWY,0x1B2038,0x070A16,c,"LAST WIN",0xFFFFFF);
    blit_half(&sym[G.winSym[w]],RRX+14,LWY+30);
    text(SYMNAME[G.winSym[w]],RRX+76,LWY+30,2,0xFFFFFF,0,1);
    snprintf(b,sizeof b,"%d REELS  %d WAY%s",G.winCnt[w],G.winWays[w],G.winWays[w]==1?"":"S");
    text(b,RRX+76,LWY+50,2,c,0,1);
    int ly=LWY+70;
    float pl=0.6f+0.4f*sinf(G.t*8.0f);
    if(G.winWt[w]>G.winWays[w]){
      /* every wild doubles the paths through it; over several ways the
         boost is the average, so say so when it is not a whole number */
      int q=G.winWt[w]/G.winWays[w], r=G.winWt[w]%G.winWays[w];
      if(r==0) snprintf(b,sizeof b,"WILDS  X%d",q);
      else     snprintf(b,sizeof b,"WILDS  X%d.%d AVG",q,(r*10)/G.winWays[w]);
      text(b,RRX+76,ly,2,mixc(0xFFC24A,0xFFFFFF,pl*0.6f),0,1);
      ly+=20;
    }
    long long fm = (G.inFree && G.fsMult>1) ? G.fsMult : 1;
    if(fm>1){
      snprintf(b,sizeof b,"FREE SPINS  X%lld",fm);
      text(b,RRX+76,ly,2,mixc(0x7CFF6A,0xFFFFFF,pl*0.5f),0,1);
      ly+=20;
    }
    snprintf(b,sizeof b,"PAYS %lld",(long long)G.winAmt[w]*fm);
    text(b,RRX+76,ly,2,0xFFE9A8,0,1);
    if(G.nWin>1){
      snprintf(b,sizeof b,"%d OF %d",w+1,G.nWin);
      text(b,RRX+RAILW-12,LWY+8,1,0xC8D2F0,2,1);
    }
  } else if(G.lastWin>0){
    snprintf(b,sizeof b,"%d",G.lastWin);
    textb(b,RRX+RAILW/2,LWY+40,4,GOLDG,5,1);
  }
}

/* keycap press feedback — cosmetic, so deliberately not in game_t */
static float btnFlash[NBTN];
static void flash_btn(int i){ if(i>=0&&i<NBTN) btnFlash[i]=0.16f; }

static void draw_meters(void){
  /* ── progressive ladder ─────────────────────────────────────────── */
  for(int i=0;i<NJP;i++){
    int y = i==0 ? JPY+24 : JPY+88+(i-1)*54;
    int h = i==0 ? 46 : 34;
    int hot = (G.state==ST_JACKPOT && G.jpWon==i);
    float pl = hot ? 0.5f+0.5f*sinf(G.t*12.0f) : 0.0f;
    /* every tier reads in its own colour, like the sign's neon; the
       ULTIMATE shimmers platinum with a slow rainbow passing through */
    static const uint32_t TON[NJP]={0xFFF4D0,0xE488FF,0xFF6A4A,0x5AF4C0};
    static const uint32_t TGH[NJP]={0x3A3420,0x3A1848,0x4A1008,0x0C3A2A};
    uint32_t base = TON[i];
    if(i==0){
      static const uint32_t HU[6]={0xFF8AA0,0xFFD080,0xF0FF90,0x90FFC8,0x90D0FF,0xD0A0FF};
      float h6=fmodf(artT*0.35f,6.0f); int hi=(int)h6;
      uint32_t hc=mixc(HU[hi%6],HU[(hi+1)%6],h6-hi);
      float sh=0.5f+0.5f*sinf(artT*1.3f);
      base=mixc(0xFFF6DC,hc,0.25f+0.25f*sh);
      /* a light running round the tier's neon */
      float d=fmodf(artT*160.0f,(float)(2*(RAILW-16+h)));
      int rx=LRX+8, ry=y, rw=RAILW-16, px,py;
      if(d<rw){ px=rx+(int)d; py=ry; }
      else if(d<rw+h){ px=rx+rw; py=ry+(int)(d-rw); }
      else if(d<2*rw+h){ px=rx+rw-(int)(d-rw-h); py=ry+h; }
      else { px=rx; py=ry+h-(int)(d-2*rw-h); }
      blit_add(&sparkspr,px-7,py-7,opt_limiter?140:230);
    }
    uint32_t on = hot ? mixc(base,0xFFFFFF,pl) : base;
    long long v = hot?G.jpAmt:jp_value(i);
    if(i==0) seg_num(v, LRX+RAILW-13, y+8, 12, 10, 26, on, TGH[i], 1);
    else     seg_num(v, LRX+RAILW-13, y+6, 10,  8, 20, on, TGH[i], 1);
    if(hot) fb_rframe(LRX+6,y-2,RAILW-12,h+4,8,2.0f,0xFFFFFF,(int)(255*pl));
  }
  /* the ladder's bulbs chase, and run hot while a jackpot is live */
  for(int i=0;i<9;i++){
    float sp2 = (G.state==ST_JACKPOT)?9.0f:3.0f;
    float v=0.30f+0.70f*(0.5f+0.5f*sinf(G.t*sp2 - i*0.5f));
    if(opt_limiter) v=0.45f+v*0.45f;
    int y=JPY+14+i*28;
    for(int sd=0;sd<2;sd++){
      int x= sd? LRX+RAILW-5 : LRX+5;
      blit_add(&bulbspr[0],x-9,y-9,(int)(v*255));
    }
  }

  /* ── right rail meters ──────────────────────────────────────────── */
  int shown = (G.state==ST_SHOWWIN)?G.winShown:G.lastWin;
  uint32_t wc = shown>0 ? 0xFF3A2A : 0x6A1A12;
  if(shown>0 && G.state==ST_SHOWWIN){
    float pl=0.7f+0.3f*sinf(G.t*10.0f);
    wc=mixc(0xFF3A2A,0xFFE0A0,pl*0.5f);
  }
  seg_num(G.credits, RRX+RAILW-13, METY+27,       12, 11, 28, 0x8AF0FF, 0x123C48, 1);
  seg_num(TOTBET,    RRX+RAILW-13, METY+METH+27,   7, 17, 30, 0xFF3A2A, 0x5A1008, 1);
  seg_num(shown,     RRX+RAILW-13, METY+2*METH+27,11, 12, 28, wc,       0x5A1008, 1);

  /* ── deck ───────────────────────────────────────────────────────── */
  for(int i=0;i<NBTN;i++){               /* decays in render(), not here */
    if(btnFlash[i]<=0) continue;
    blit(&btnspr[i][1],BTNX[i]-4,BTNY-4,0,FBH,255,0,0.0f);
  }
  float sp = (G.state==ST_IDLE||G.state==ST_ATTRACT)
             ? 0.5f+0.5f*sinf(G.t*3.2f) : 0.0f;
  spin_dome(SPINX,SPINY,SPINR,G.state==ST_SPIN,sp);
}

/*  Full-screen tint (this band's rows of it).  fb_rect would run the
 *  general per-pixel blend with its bounds checks over 900k pixels; this
 *  is the same result as one straight run the compiler vectorises, and
 *  it is on screen whenever an overlay is up.                         */
static void screen_tint(uint32_t c,int a){
  if(a<=0) return;
  if(a>255) a=255;
  span_blend(fb+(size_t)clip_y0*FBW,(clip_y1-clip_y0)*FBW,c,a);
}
static void dim(int a){ screen_tint(0x000000,a); }

/*  Cached full frames.  Built on first use so a player who never opens
 *  the pay table never pays for it; each costs 3.6 MB, so keep them few.
 *
 *  frame_cache() draws `paint` - a function that paints the WHOLE frame
 *  - and keeps the result under `key`; while the key holds it copies the
 *  rows back instead.  It is band-aware: on a miss each band paints and
 *  stores only its own rows, so the cache fills in parallel, and it only
 *  becomes valid in render_commit(), once every band has run.  Nothing
 *  is ever painted outside the band, so no thread waits on another.  */
typedef struct {
  uint32_t *px;
  uint32_t  key, pendKey;
  int       valid, pend, nomem;
} fcache_t;
#define NFCPEND 16
static fcache_t *fc_pend[NFCPEND];
static int       fc_npend;

static void frame_cache(fcache_t*c,uint32_t key,void(*paint)(void)){
  size_t o=(size_t)clip_y0*FBW, n=(size_t)(clip_y1-clip_y0)*FBW;
  if(c->valid && c->key==key && c->px){ memcpy(fb+o,c->px+o,n*4); return; }
  paint();
  if(bp_active) bp_lock();
  if(!c->px && !c->nomem){
    c->px=(uint32_t*)malloc((size_t)FBW*FBH*4);
    if(!c->px) c->nomem=1;                 /* never retried mid-frame */
  }
  if(c->px && !c->pend && fc_npend<NFCPEND){
    c->pend=1; c->pendKey=key; fc_pend[fc_npend++]=c;
  }
  int keep = c->px && c->pend && c->pendKey==key;
  if(bp_active) bp_unlock();
  if(keep) memcpy(c->px+o,fb+o,n*4);
}
/*  Serial, after every band: the caches the bands filled are complete. */
static void render_commit(void){
  for(int i=0;i<fc_npend;i++){
    fcache_t*c=fc_pend[i];
    c->valid=1; c->key=c->pendKey; c->pend=0;
  }
  fc_npend=0;
}
static void fcache_free(fcache_t*c){ free(c->px); memset(c,0,sizeof *c); }

static fcache_t ptbg, bnbg, ptimg[3], bnimg;
static void bonus_invalidate(void){ bnimg.valid=0; }

static void cache_backdrop(fcache_t*slot, void(*paint)(void)){
  frame_cache(slot,0,paint);
}

/* pay table rows: 15 symbols in two columns of eight */
#define PTX0 40
#define PTX1 660
#define PTY0 78
#define PTRH 64

static void paint_paytable_bg(void){
  vgrad(0,0,FBW,FBH,0x12173A,0x03040C);
  fb_rframe(14,10,FBW-28,FBH-20,14,3.0f,0xE8B93C,255);
  for(int i=0;i<NSYM;i++){
    int col=i/8, row=i%8;
    int x=(col?PTX1:PTX0), y=PTY0+row*PTRH;
    fb_rrect(x-8,y-4,588,PTRH-6,10,0x1E2450,150);
  }
}

static void paint_paytable(void){
  cache_backdrop(&ptbg,paint_paytable_bg);
  textb("PAY TABLE",FBW/2,14,5,GOLDG,5,1);
  char b[96];
  /* the band header over the first column, the only one with pays */
  { int x=PTX0+72;
    for(int k=0;k<3;k++) text(BANDNAME[k],x+120+k*130,PTY0-14,1,0xFFC24A,1,1);
    text("X BET, PER WAY",x,PTY0-14,1,0xFFC24A,0,1); }
  for(int i=0;i<NSYM;i++){
    int col=i/8, row=i%8;
    int x=(col?PTX1:PTX0), y=PTY0+row*PTRH;
    blit_half(&sym[i],x+2,y+1);
    text(SYMNAME[i],x+72,y+3,2,0xFFFFFF,0,1);
    if(i<NPAYSYM){
      /* pays as multiples of the total bet, one decimal where it needs it */
      for(int k=0;k<3;k++){
        float mult=PAY[i][k+3]/10.0f;
        if(mult>=10.0f) snprintf(b,sizeof b,"%.0f",mult);
        else            snprintf(b,sizeof b,"%.1f",mult);
        text(b,x+192+k*130,y+22,2,0xFFE9A8,1,1);
      }
      if(i==SY_SEVEN) text("STANDS IN FOR ANY SYMBOL - AND DOUBLES EVERY WIN IT JOINS",x+72,y+42,1,0xFFC24A,0,1);
      else            text("X TOTAL BET, TIMES THE NUMBER OF WAYS",x+72,y+42,1,0x8A93B8,0,1);
      continue;
    }
    const char*l1="", *l2="";
    switch(i){
    case SY_STAR:
      snprintf(b,sizeof b,"3 / 4 / 5 ANYWHERE PAY %d / %d / %d X BET",SCATPAY[3],SCATPAY[4],SCATPAY[5]);
      l1=b; l2="3 OR MORE = FREE SPINS";                                   break;
    case SY_CROWN:   l1="3 CROWNS = LUCKY 7 PICK BONUS";       l2="LAND ON REELS 1, 3 AND 5";          break;
    case SY_JACKPOT: l1="5 MINOR 5X   6 MAJOR 40X   7+ MEGA 200X"; l2="LANDS ON REELS 2, 3 AND 4"; break;
    case SY_ULT:     l1="5 TOUCHING = ULTIMATE, 100,000 X BET"; l2="ONE ON EACH REEL, IN A CHAIN";     break;
    case SY_COIN:    l1="6 OR MORE ANYWHERE = HOLD & SPIN";    l2="EACH SHOWS CREDITS, OR MINOR / MAJOR"; break;
    case SY_WHEEL:   l1="ONE ON REELS 2, 3 AND 4 = THE WHEEL"; l2="WHEEL OF 7'S - UP TO THE MEGA JACKPOT"; break;
    }
    text(l1,x+72,y+22,2,0xFFC24A,0,1);
    text(l2,x+72,y+40,2,0xC8D8FF,0,1);
  }
  text("A WIN READS LEFT TO RIGHT FROM REEL 1: ONE SYMBOL PER REEL, SAME ROW OR ONE UP OR DOWN",
       FBW/2,602,2,0xFFFFFF,1,1);
  text("A TENTH OF EVERY BET FEEDS THE JACKPOTS, AND ALL FOUR ARE A MULTIPLE OF THE BET YOU PLAY",
       FBW/2,624,2,0xFFC24A,1,1);
  text("EVERY PATH IS A WAY AND PAYS AGAIN, AND EVERY WILD IN IT DOUBLES THE WIN.   BET 10 TO 100,000 A SPIN.",FBW/2,646,2,0xAFAFC8,1,1);
}

/*  A features page: four bands, each an icon, a title and up to four
 *  lines - how the features are won AND how they are played, so nobody
 *  has to learn the pick round or the gamble by losing it.            */
struct ftband { const char*title; uint32_t col; int sy1, sy2; const char*ln[4]; };
static void paint_ftpage(const char*head,const struct ftband*B){
  vgrad(0,0,FBW,FBH,0x12173A,0x03040C);
  fb_rframe(14,10,FBW-28,FBH-20,14,3.0f,0xE8B93C,255);
  textb(head,FBW/2,14,5,GOLDG,5,1);
  for(int i=0;i<4;i++){
    int y=70+i*152, h=142;
    fb_rrect(30,y,FBW-60,h,14,0x1E2450,150);
    fb_rframe(30,y,FBW-60,h,14,2.0f,B[i].col,200);
    if(B[i].sy1>=0) blit_half(&sym[B[i].sy1],46,y+14);
    if(B[i].sy2>=0) blit_half(&sym[B[i].sy2],46,y+76);
    text(B[i].title,120,y+12,3,B[i].col,0,1);
    for(int k=0;k<4 && B[i].ln[k];k++) text(B[i].ln[k],120,y+44+k*24,2,k==0?0xFFFFFF:0xC8D8FF,0,1);
  }
}

static void paint_features(void){
  char fs1[96];
  snprintf(fs1,sizeof fs1,"3 OR MORE SCATTERS AWARD %d FREE SPINS - 3 MORE DURING THE FEATURE ADD %d",
           FS_AWARD,FS_RETRIG);
  const struct ftband B[4] = {
    { "WILD 7 - THE MULTIPLIER SYMBOL", 0xFFC24A, SY_SEVEN, -1, {
      "WILD 7 STANDS IN FOR EVERY PAYING SYMBOL, AND DOUBLES EVERY WIN IT IS PART OF",
      "TWO WILDS IN ONE WIN PAY FOUR TIMES, THREE PAY EIGHT TIMES, AND SO ON",
      "THE WILDS IN A WIN LIGHT UP WEARING THEIR X2 SO YOU CAN SEE WHERE IT CAME FROM",
      NULL } },
    { "FREE SPINS", 0x7CFF6A, SY_STAR, -1, {
      fs1,
      "EXPANDING WILDS: A WILD 7 LANDING ON REEL 2, 3 OR 4 GROWS TO FILL ITS WHOLE REEL",
      "AND NOTCHES THE MULTIPLIER UP ONE - IT NEVER FALLS BACK, AND CLIMBS TO X5",
      "IT TIMES THE WHOLE SPIN, ON TOP OF THE X2 EVERY WILD ALREADY PAYS. SCATTERS PAY TOO" } },
    { "LUCKY 7 PICK", 0xC060FF, SY_CROWN, -1, {
      "3 CROWNS (THEY LAND ON REELS 1, 3 AND 5) OPEN A BOARD OF NINE HIDDEN PANELS",
      "D-PAD MOVES THE CURSOR, A TURNS A PANEL.  PANELS HIDE CREDITS, A X2 MULTIPLIER, OR A STOP",
      "THE ROUND ENDS ON THE THIRD STOP, SO YOU USUALLY GET FOUR OR FIVE PICKS",
      "THE MULTIPLIER APPLIES TO EVERYTHING YOU COLLECTED" } },
    { "PROGRESSIVE JACKPOTS", 0xFFB020, SY_JACKPOT, SY_ULT, {
      "JACKPOT LANDS ON REELS 2, 3 AND 4.  5 TOUCHING = MINOR, 6 = MAJOR, 7 OR MORE = MEGA",
      "ULTIMATE LANDS ONE PER REEL.  ALL FIVE TOUCHING = THE ULTIMATE, 100,000 TIMES YOUR BET",
      "EVERY POT IS A MULTIPLE OF YOUR BET - 5X, 40X, 200X, 100,000X - PLUS EVERYTHING FED IN",
      "HOLD & SPIN AND THE WHEEL CAN WIN THEM TOO.  A TENTH OF EVERY BET FEEDS THEM" } },
  };
  paint_ftpage("FEATURES",B);
}

static void paint_features2(void){
  static const struct ftband B[4] = {
    { "HOLD & SPIN", 0xFFD24A, SY_COIN, -1, {
      "6 OR MORE LUCKY COINS ANYWHERE START IT.  THE COINS LOCK AND EVERY OTHER CELL RESPINS",
      "3 RESPINS - EVERY NEW COIN LOCKS IN AND RESETS THEM TO 3.  IT ENDS WHEN THEY RUN OUT",
      "EVERY COIN PAYS ITS VALUE.  A MINOR OR MAJOR COIN PAYS THAT JACKPOT",
      "FILL ALL 25 CELLS FOR THE GRAND: THE MEGA JACKPOT ON TOP OF EVERY COIN" } },
    { "WHEEL OF 7'S", 0xE070FF, SY_WHEEL, -1, {
      "A WHEEL ON EACH OF REELS 2, 3 AND 4 BRINGS OUT THE WHEEL.  PRESS A TO SPIN IT",
      "24 WEDGES: 5X TO 250X YOUR BET, THE MINOR AND MAJOR JACKPOTS - AND SUPER",
      "SUPER UPGRADES TO THE SUPER WHEEL: 25X TO 500X, THE MAJOR, AND THE MEGA JACKPOT",
      NULL } },
    { "7 STRIKE", 0x9AD8FF, SY_SEVEN, -1, {
      "AT RANDOM, A STORM GATHERS OVER THE REELS WHILE THEY SPIN",
      "WHEN THEY STOP, 3 TO 8 LIGHTNING BOLTS STRIKE, AND EVERY CELL THEY HIT TURNS WILD",
      "EVERY STRUCK WILD DOUBLES THE WINS THROUGH IT, LIKE ANY OTHER WILD 7",
      NULL } },
    { "GAMBLE", 0xFF6A6A, -1, -1, {
      "AFTER A WIN, PRESS X TO GAMBLE IT.  LEFT = RED, RIGHT = BLACK: A RIGHT CALL DOUBLES IT",
      "OR UP / DOWN TO CHOOSE A SUIT, AND X TO PLAY IT FOR FOUR TIMES.  A COLLECTS",
      "UP TO 5 ROUNDS, ON WINS UP TO 50X YOUR BET.  THE CARDS ARE EXACTLY FAIR",
      NULL } },
  };
  paint_ftpage("MORE FEATURES",B);
}

/*  All three pages are static between player actions, so each is
 *  painted once into a cached frame and copied back after that.       */
#define NPTPAGE 3
static void draw_paytable(void){
  static void (*const paint[NPTPAGE])(void) = { paint_paytable, paint_features, paint_features2 };
  int pg = G.ptPage%NPTPAGE;
  if(pg<0) pg=0;
  cache_backdrop(&ptimg[pg], paint[pg]);
  if(((int)(G.t*2.0f))&1){
    char b[96];
    snprintf(b,sizeof b,"PAGE %d OF %d    SELECT = NEXT PAGE    ANY OTHER BUTTON = BACK TO THE GAME",
             pg+1,NPTPAGE);
    text(b,FBW/2,684,2,0xFFFFFF,1,1);
  }
}

/* the jackpot celebration */
static void draw_jackpot(void){ fx_jackpot_draw(); }   /* w7_fx.c */

/* the ADD CREDITS chooser, over the reel window */
static void draw_addcr(void){
  dim(150);
  int pw=GW+16, ph=250, px=GX-8, py=GY+GH/2-ph/2;
  fb_rrect(px-10,py-10,pw+20,ph+20,26,0x000000,120);
  fb_rrectg(px,py,pw,ph,18,0x2B3358,0x080B18,245);
  fb_rframe(px,py,pw,ph,18,3.0f,0xF0C24A,255);
  fb_rframe(px+6,py+6,pw-12,ph-12,14,1.0f,0x8A6A10,220);
  textb("ADD CREDITS",FBW/2,py+14,5,GOLDG,5,1);
  text("HOW MANY?",FBW/2,py+68,2,0xC8D2F0,1,1);
  const int tw=92, gap=10;
  int x0=px+(pw-(NADDS*tw+(NADDS-1)*gap))/2, ty=py+96;
  char b[16];
  for(int i=0;i<NADDS;i++){
    int x=x0+i*(tw+gap), sel=(i==G.addIdx);
    float pl=sel?0.5f+0.5f*sinf(G.t*8.0f):0.0f;
    keycap(x,ty,tw,60,"",NULL,sel,sel?0xFFE9A8:0xB9BECC);
    if(sel) fb_rframe(x-4,ty-4,tw+8,68,13,2.5f,mixc(0xFFD24A,0xFFFFFF,pl),255);
    snprintf(b,sizeof b,"%d",ADDS[i]);
    text(b,x+tw/2,ty+(sel?3:0)+20,sel?3:2,sel?0x14141C:0x2A2A38,1,0);
  }
  text("LEFT / RIGHT TO CHOOSE     A TO ADD     B TO CANCEL",FBW/2,py+188,2,0xFFFFFF,1,1);
  snprintf(b,sizeof b,"BANK  %lld",G.credits);
  text(b,FBW/2,py+214,2,0x8AF0FF,1,1);
}

/* ═══ SHOWTIME: the big moments ════════════════════════════════════
 *  Free spins awarded, a feature over, the attract loop.  These are the
 *  screens that sell the machine from across the room, so they get the
 *  Hollywood treatment - turning sunbursts, titles that zoom in and
 *  settle, counters that roll - and all of it is baked sprites, lookup
 *  tables and a hash of the clock: no per-pixel maths, no state.
 * ================================================================= */
static void commas(char*out,size_t n,long long v);
/* the sign in the middle of the top box: see build_marquee() */
#define MQPW 330
#define MQPX (FBW/2-MQPW/2)
#define MQPY 3
#define MQPH (MQH-6)

/*  A rotating sunburst.  The angle and falloff of every point of a
 *  half-resolution field are tabulated once, so a frame is a table
 *  lookup and an add per 2x2 block, and only inside the burst.       */
#define RAYW 640
#define RAYH 360
static uint8_t *rayAng=NULL, *rayFall=NULL;
static uint8_t rayProf[256];
static void build_rays(void){
  rayAng=(uint8_t*)malloc(RAYW*RAYH); rayFall=(uint8_t*)malloc(RAYW*RAYH);
  if(!rayAng||!rayFall){ free(rayAng); free(rayFall); rayAng=rayFall=NULL; return; }
  for(int y=0;y<RAYH;y++) for(int x=0;x<RAYW;x++){
    float dx=(x-RAYW*0.5f+0.5f)*2.0f, dy=(y-RAYH*0.5f+0.5f)*2.0f;
    float a=fast_atan2(dy,dx)/TAU+0.5f;
    float d=sqrtf((dx/640.0f)*(dx/640.0f)+(dy/400.0f)*(dy/400.0f));
    float f=clampf(1.0f-d,0,1);
    f=f*smooth01(0.0f,0.12f,d);
    rayAng[y*RAYW+x]=(uint8_t)((int)(a*256.0f)&255);
    rayFall[y*RAYW+x]=(uint8_t)(f*255.0f);
  }
  for(int i=0;i<256;i++){ float t=i/256.0f; rayProf[i]=(uint8_t)(255.0f*smooth01(0.18f,0.34f,t)*(1.0f-smooth01(0.66f,0.82f,t))); }
}
/*  Two counter-rotating bursts in one pass (the second may be off,
 *  k2=0): one read-modify-write per pixel however many layers.      */
static void draw_sunburst2(int cx,int cy,float rot,int nrays,uint32_t col,int k,
                           float rot2,int nrays2,uint32_t col2,int k2){
  if(!rayAng||(k<=0&&k2<=0)) return;
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  int dr=(col2>>16)&255, dg=(col2>>8)&255, db=col2&255;
  int r256=(int)(rot*256.0f), q256=(int)(rot2*256.0f);
  for(int yy=0;yy<RAYH;yy++){
    int sy=cy+(yy-RAYH/2)*2;
    if(sy+1<clip_y0||sy>=clip_y1||sy<0||sy+1>=FBH) continue;
    const uint8_t*fa=rayFall+yy*RAYW, *aa=rayAng+yy*RAYW;
    uint32_t*r0=fb+(size_t)sy*FBW, *r1=r0+FBW;
    int do0=(sy>=clip_y0), do1=(sy+1<clip_y1);
    int xa=0, xb=RAYW;
    if(cx-RAYW<0) xa=(RAYW-cx)/2+1;
    if(cx+RAYW>FBW-2) xb=RAYW-((cx+RAYW)-(FBW-2))/2-1;
    for(int xx=xa;xx<xb;xx++){
      int f=fa[xx]; if(!f) continue;
      int v1=(rayProf[(aa[xx]*nrays+r256)&255]*f>>8)*k>>8;
      int v2=k2>0?(rayProf[(aa[xx]*nrays2+q256)&255]*f>>8)*k2>>8:0;
      if(v1+v2<=2) continue;
      int ar=(cr*v1+dr*v2)>>8, ag=(cg*v1+dg*v2)>>8, ab=(cb*v1+db*v2)>>8;
      int sx=cx+(xx-RAYW/2)*2;
      for(int q=0;q<2;q++){
        if(q==0 && !do0) continue;
        if(q==1 && !do1) continue;
        uint32_t*d=(q?r1:r0)+sx;
        for(int e=0;e<2;e++){
          uint32_t c=d[e];
          int r=((c>>16)&255)+ar, g=((c>>8)&255)+ag, bb=(c&255)+ab;
          d[e]=RGB(r>255?255:r,g>255?255:g,bb>255?255:bb);
        }
      }
    }
  }
}
static __attribute__((unused)) void draw_sunburst(int cx,int cy,float rot,int nrays,uint32_t col,int k){
  draw_sunburst2(cx,cy,rot,nrays,col,k,0,1,0,0);
}

/*  The banner screens dim the game and throw a sunburst over it.  Done
 *  as two passes that is two read-modify-writes of most of a megapixel;
 *  fused, it is one.  deep=0 keeps 3/8 of the scene, deep=1 keeps 9/32.
 *  Everything below y0 (the marquee is spared) is touched.            */
static void dim_burst(int y0,int deep,int cx,int cy,
                      float rot,int nrays,uint32_t col,int k,
                      float rot2,int nrays2,uint32_t col2,int k2){
  /*  After the dim no channel is above 94, so the light added can be
   *  capped at 161 and summed as one packed word, no per-channel clamp.
   *  The light for every (falloff, angle) pair is tabulated per frame:
   *  32 x 256 words, then each pixel is a shift, two loads and an add. */
  uint32_t T[32][256];            /* per call, so bands never share it */
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  int dr=(col2>>16)&255, dg=(col2>>8)&255, db=col2&255;
  int r256=(int)(rot*256.0f), q256=(int)(rot2*256.0f);
  int lit=(rayAng && (k>0||k2>0));
  if(lit) for(int a=0;a<256;a++){
    int p1=rayProf[(a*nrays+r256)&255]*k>>8, p2=k2>0?rayProf[(a*nrays2+q256)&255]*k2>>8:0;
    for(int f=0;f<32;f++){
      int v1=p1*f/31, v2=p2*f/31;
      int ar=(cr*v1+dr*v2)>>8, ag=(cg*v1+dg*v2)>>8, ab=(cb*v1+db*v2)>>8;
      if(ar>161) ar=161;
      if(ag>161) ag=161;
      if(ab>161) ab=161;
      T[f][a]=RGB(ar,ag,ab);
    }
  }
  const uint32_t m2=deep?0x070707:0x1F1F1F; const int s2=deep?5:3;
  int ya=y0>clip_y0?y0:clip_y0;
  for(int y=ya;y<clip_y1;y++){
    uint32_t*q=fb+(size_t)y*FBW;
    int yy=((y-cy+RAYH*2)>>1)-RAYH/2;
    if(!lit||yy<0||yy>=RAYH){
      for(int i=0;i<FBW;i++){ uint32_t c=q[i]; q[i]=((c>>2)&0x3F3F3F)+((c>>s2)&m2); }
      continue;
    }
    const uint8_t*fa=rayFall+yy*RAYW, *aa=rayAng+yy*RAYW;
    int xo=((0-cx+RAYW*2)>>1)-RAYW/2;
    for(int x=0;x<FBW;x+=2){
      int xx=xo+(x>>1);
      uint32_t add = (xx>=0&&xx<RAYW) ? T[fa[xx]>>3][aa[xx]] : 0;
      uint32_t c0=q[x], c1=q[x+1];
      q[x]  =((c0>>2)&0x3F3F3F)+((c0>>s2)&m2)+add;
      q[x+1]=((c1>>2)&0x3F3F3F)+((c1>>s2)&m2)+add;
    }
  }
}

/*  A neon tube round a panel, cheap enough to run live: a stepped
 *  additive halo on the perimeter only, and the tube itself.         */
static void neon_live(int x,int y,int w,int h,float r,uint32_t col,int k){
  for(int i=1;i<=8;i++) add_frame(x-i,y-i,w+2*i,h+2*i,col,(9-i)*(9-i)*k>>8);
  fb_rframe(x,y,w,h,r,2.0f,mixc(col,0xFFFFFF,0.45f),255);
}
static uint32_t hue_at(float t){
  static const uint32_t H[6]={0xFF4060,0xFFB030,0xE0FF40,0x40FFA0,0x40B0FF,0xB050FF};
  t-=floorf(t); t*=6.0f; int i=(int)t;
  return mixc(H[i%6],H[(i+1)%6],t-i);
}

/*  A dim that spares the marquee, which keeps selling while a banner is
 *  up.  The common levels are shifts and masks rather than a blend: this
 *  runs over most of a megapixel, every frame a banner is showing.    */
static void dim_below(int y0,int a){
  if(y0<clip_y0) y0=clip_y0;
  int y1=clip_y1;
  if(a>=150){                      /* keep 3/8 */
    for(int y=y0;y<y1;y++){ uint32_t*q=fb+(size_t)y*FBW;
      for(int i=0;i<FBW;i++){ uint32_t c=q[i]; q[i]=((c>>2)&0x3F3F3F)+((c>>3)&0x1F1F1F); } }
    if(a>=195) for(int y=y0;y<y1;y++){ uint32_t*q=fb+(size_t)y*FBW;   /* then 3/4 of that */
      for(int i=0;i<FBW;i++){ uint32_t c=q[i]; q[i]=((c>>1)&0x7F7F7F)+((c>>2)&0x3F3F3F); } }
  } else fb_shade_rect(0,y0,FBW,FBH-y0,0x000000,a);
}

static float ease_back(float x){ x=clampf(x,0,1); const float c1=1.70158f,c3=c1+1.0f; float p=x-1.0f; return 1.0f+c3*p*p*p+c1*p*p; }

/* a big title that zooms in, overshoots and settles, then shines */
static void title_zoom(int id,int cx,int cy,float t,float speed){
  const spr_t*s=&title[id];
  float z=ease_back(t*speed);
  if(t*speed<1.0f) blit_scaled(s,cx,cy,z,(int)(255*clampf(t*speed*3.0f,0,1)));
  else {
    blit(s,cx-s->w/2,cy-s->h/2,0,FBH,255,0,0.0f);
    float ph=fmodf(t*1.1f,2.2f);
    if(ph<1.0f) shine_sprite(s,cx-s->w/2,cy-s->h/2,0,FBH,-60.0f+ph*(s->w+160.0f),26,170,210);
  }
}

/* big gold digits, centred, for the counters */
static void big_number(long long v,int cx,int cy,float sc){
  char b[24]; snprintf(b,sizeof b,"%lld",v);
  int n=(int)strlen(b), adv=(int)(bigdig[0].w*0.62f*sc), w=adv*(n-1);
  for(int i=0;i<n;i++){
    const spr_t*s=&bigdig[b[i]-'0'];
    int x=cx-w/2+i*adv;
    if(sc>0.995f && sc<1.005f) blit(s,x-s->w/2,cy-s->h/2,0,FBH,255,0,0.0f);
    else blit_scaled(s,x,cy,sc,255);
  }
}

/* a lit plate for a line of text on a banner */
static void banner_plate(int cx,int cy,int w,int h,uint32_t rim){
  /* a modest radius keeps the rounded-rect primitives on their fast path */
  fb_rrectg(cx-w/2,cy-h/2,w,h,10,0x140818,0x04020A,225);
  fb_rframe(cx-w/2,cy-h/2,w,h,10,2.0f,rim,255);
  fb_rframe(cx-w/2+3,cy-h/2+3,w-6,h-6,7,1.0f,mixc(rim,0xFFFFFF,0.5f),110);
}

/* glints drifting down through a banner: a hash of their index and the
   clock places them, so this is pure drawing */
static void glint_rain(int n,float t,int k){
  for(int i=0;i<n;i++){
    float x=dhash(i,11)*FBW, sp=90.0f+dhash(i,12)*160.0f;
    float y=fmodf(dhash(i,13)*FBH+t*sp,(float)(FBH-MQH))+MQH;
    float tw=0.5f+0.5f*sinf(t*6.0f+i);
    blit_add(&sparkspr,(int)x-7,(int)y-7,(int)(k*tw));
  }
}

/* ── free spins: the status bar takes over the top box ───────────── */
static void draw_fsbar(void){
  char b[64];
  float pl=0.5f+0.5f*sinf(artT*4.0f);
  for(int sd=0;sd<2;sd++){
    int x = sd ? MQPX+MQPW+4 : 10, w = MQPX-14, y=6, h=MQH-12;
    fb_rrectg(x,y,w,h,12,0x06301A,0x010A04,255);
    fb_rframe(x,y,w,h,12,2.0f,mixc(0x5AE070,0xE8FFD8,pl*0.5f),255);
    fb_rframe(x+3,y+3,w-6,h-6,9,1.0f,0xFFD24A,150);
  }
  static const uint32_t G2[4]={0xF4FFE8,0xB8FF8A,0x3CD23C,0x16861E};
  text("FREE SPINS",40,23,2,0xB8FFB0,0,1);
  snprintf(b,sizeof b,"%d",G.freeSpins);
  textb(b,260,15,4,G2,4,1);
  text(G.freeSpins==1?"LAST ONE":"LEFT",300,23,2,0xE8FFE0,0,1);
  int m=G.fsMult<1?1:G.fsMult;
  snprintf(b,sizeof b,"X%d",m);
  text("MULTIPLIER",MQPX+MQPW+34,23,2,0xFFE9A8,0,1);
  textb(b,MQPX+MQPW+194,15,4,m>1?GOLDG:SILVERG,m>1?5:4,1);
  if(m>=FS_MAXMULT) text("MAX",MQPX+MQPW+232,23,2,0xFFE9A8,0,1);
  char w[24]; commas(w,sizeof w,G.fsWon);
  snprintf(b,sizeof b,"WON %s",w);
  text(b,FBW-36,23,2,0xFFFFFF,2,1);
}

/* ── FREE SPINS awarded ─────────────────────────────────────────── */
static void draw_fsintro(void){
  float t=G.t;
  if(t<0.2f) dim_below(MQH,(int)(200*t*5.0f));
  else dim_burst(MQH,1,FBW/2,330,artT*0.08f,18,0x40FF60,opt_limiter?120:200,
                 -artT*0.05f,9,0xFFD040,opt_limiter?70:120);
  title_zoom(TT_FREESPINS,FBW/2,210,t,2.2f);
  /* the count rolls like a reel, then lands */
  int award = G.inFree ? FS_RETRIG : FS_AWARD;
  if(t>0.35f){
    float u=t-0.35f;
    int n = u<0.4f ? (int)(dhash((int)(u*24.0f),7)*9.99f) : award;
    float pop = u<0.4f ? 1.0f : 1.0f+0.35f*clampf(1.0f-(u-0.4f)*4.0f,0,1);
    if(G.inFree) blit_scaled(&bigPlus,FBW/2-70,392,0.8f*pop,255);
    big_number(n,FBW/2+(G.inFree?30:0),392,pop);
  }
  if(t>1.2f){
    int a=(int)(255*clampf((t-1.2f)*3.0f,0,1));
    banner_plate(FBW/2,512,900,40,0x5AE070);
    char msg[96];
    if(G.inFree) snprintf(msg,sizeof msg,"%d MORE FREE SPINS  -  THE MULTIPLIER STAYS AT X%d AND KEEPS CLIMBING",
                          FS_RETRIG,G.fsMult<1?1:G.fsMult);
    else snprintf(msg,sizeof msg,"MULTIPLIER STARTS AT X1  -  EVERY WILD REEL ADDS +1, UP TO X%d",FS_MAXMULT);
    text(msg,FBW/2,505,2,mixc(0x000000,0xE8FFE0,a/255.0f),1,1);
    char b[48]; snprintf(b,sizeof b,"%d SCATTERS",G.scatCount);
    text(b,FBW/2,556,3,0xFFE9A8,1,1);
  }
  glint_rain(24,t,200);
}

/* ── a feature over: free spins (banner 3) or the pick (banner 4) ── */
static void draw_bonusend(void){
  float t=G.t;
  int fs=(G.banner==3);
  if(t<0.2f) dim_below(MQH,(int)(205*t*5.0f));
  else dim_burst(MQH,1,FBW/2,340,artT*0.07f,16,fs?0x60FF80:0xFFB020,opt_limiter?110:190,
                 -artT*0.05f,8,0xFFE070,opt_limiter?60:110);
  title_zoom(fs?TT_FSDONE:TT_BONUSDONE,FBW/2,190,t,2.4f);
  long long total = fs ? (long long)G.fsWon
                       : (long long)G.pickTotal*(G.pickMult>0?G.pickMult:1);
  if(t>0.3f){
    const spr_t*tw=&title[TT_TOTALWIN];
    blit(tw,FBW/2-tw->w/2,300-tw->h/2,0,FBH,255,0,0.0f);
    float u=clampf((t-0.3f)/1.1f,0,1);
    long long shown=(long long)(total*(1.0f-(1.0f-u)*(1.0f-u)));
    int w=seg_width(10,24,64,1);
    fb_rrect(FBW/2-w/2-22,346,w+44,88,14,0x000000,230);
    fb_rframe(FBW/2-w/2-22,346,w+44,88,14,2.0f,0xFFD24A,255);
    neon_live(FBW/2-w/2-22,346,w+44,88,14,0xFFB020,200);
    float pl=0.5f+0.5f*sinf(artT*8.0f);
    seg_num(shown,FBW/2+w/2,358,10,24,64,
            u>=1.0f?mixc(0xFFB020,0xFFFFFF,pl*0.6f):0xFFB020,0x3A2804,1);
  }
  if(!fs && t>0.9f){
    char a[24],b[80];
    commas(a,sizeof a,G.pickTotal);
    snprintf(b,sizeof b,"COLLECTED %s   X%d MULTIPLIER",a,G.pickMult>0?G.pickMult:1);
    banner_plate(FBW/2,480,700,40,0xC060FF);
    text(b,FBW/2,473,2,0xF4E0FF,1,1);
  }
  /* the free-spins meter goes back to X1 here: say where it got to */
  if(fs && t>0.9f){
    char b[80];
    int m=G.fsMult<1?1:G.fsMult;
    if(m>1) snprintf(b,sizeof b,"MULTIPLIER REACHED X%d  -  BACK TO X1 FOR THE BASE GAME",m);
    else    snprintf(b,sizeof b,"THE MULTIPLIER STAYED AT X1 THIS TIME");
    banner_plate(FBW/2,480,760,40,0x5AE070);
    text(b,FBW/2,473,2,0xE8FFE0,1,1);
  }
  glint_rain(30,t,220);
}

static void draw_broke(void){
  float pl=0.5f+0.5f*sinf(artT*3.0f);
  dim_burst(MQH,1,FBW/2,300,artT*0.04f,12,0xFF3020,(int)(60+60*pl),0,1,0,0);
  const spr_t*s=&title[TT_BROKE];
  blit(s,FBW/2-s->w/2,280-s->h/2,0,FBH,255,0,0.0f);
  if(((int)(artT*2.0f))&1){
    banner_plate(FBW/2,400,620,46,0xFF5A5A);
    text("PRESS START TO ADD CREDITS",FBW/2,391,3,0xFFFFFF,1,1);
  }
}

/* ── ATTRACT: a cinematic loop while nobody is playing ───────────── */
typedef struct { int sy; const char*name; const char*l1; const char*l2; uint32_t col; } show_t;
static const show_t SHOW[7]={
  {SY_SEVEN,  "WILD 7",       "STANDS IN FOR EVERY PAYING SYMBOL",    "AND EVERY WILD IN A WIN DOUBLES IT - X2, X4, X8", 0xFFC24A},
  {SY_STAR,   "SCATTER",      "3 OR MORE ANYWHERE - FREE SPINS",      "EXPANDING WILDS, MULTIPLIER UP TO X5",            0x7CFF6A},
  {SY_CROWN,  "LUCKY 7 PICK", "3 CROWNS ON REELS 1, 3 AND 5",         "PICK PANELS FOR CREDITS AND A X2",                0xC060FF},
  {SY_COIN,   "HOLD & SPIN",  "6 OR MORE LUCKY COINS",                "LOCK THEM IN AND RESPIN FOR MORE",                0xFFD24A},
  {SY_WHEEL,  "WHEEL OF 7'S", "3 WHEELS ON REELS 2, 3 AND 4",         "SPIN THE WHEEL FOR CREDITS AND JACKPOTS",         0xFF5AA8},
  {SY_JACKPOT,"JACKPOT",      "5, 6 OR 7+ TOUCHING",                  "MINOR, MAJOR OR MEGA - PAID FROM THE REELS",      0xFF6A30},
  {SY_ULT,    "ULTIMATE",     "5 ULTIMATES TOUCHING",                 "THE ULTIMATE JACKPOT - 100,000 X YOUR BET",       0x9AF0FF},
};
static const char*FEATLINES[9]={
  "ADJACENT WAYS - WINS READ LEFT TO RIGHT",
  "EVERY WILD 7 DOUBLES THE WIN",
  "FREE SPINS - MULTIPLIER CLIMBS TO X5",
  "LUCKY 7 PICK BONUS",
  "HOLD & SPIN - 6 OR MORE LUCKY COINS",
  "WHEEL OF 7'S - 3 WHEELS ON REELS 2, 3 AND 4",
  "7 STRIKE - RANDOM WILDS DROP IN",
  "GAMBLE ANY WIN - DOUBLE OR NOTHING",
  "4 PROGRESSIVE JACKPOTS",
};
#define ATT_LOOP 21.0f

static void draw_attract(void){
  float T=fmodf(artT,ATT_LOOP);
  char b[64];
  if(T<6.0f){
    /* the logo arrives, and the top prize under it */
    dim_burst(MQH,0,FBW/2,250,artT*0.06f,20,0xFFB030,opt_limiter?110:180,
              -artT*0.04f,10,0xFF4060,opt_limiter?60:100);
    title_zoom(TT_LOGOBIG,FBW/2,215,T,1.6f);
    if(T>1.0f){
      int a=(int)(255*clampf((T-1.0f)*3.0f,0,1));
      text("THE ULTIMATE JACKPOT",FBW/2,356,3,mixc(0,0xFFE9A8,a/255.0f),1,1);
      int w=seg_width(12,18,48,1);
      fb_rrect(FBW/2-w/2-18,390,w+36,70,12,0x000000,(int)(a*0.9f));
      neon_live(FBW/2-w/2-18,390,w+36,70,12,hue_at(artT*0.3f),a);
      float pl=0.5f+0.5f*sinf(artT*2.0f);
      seg_num(jp_value(JP_ULT),FBW/2+w/2,401,12,18,48,mixc(0xFFE080,0xFFFFFF,pl*0.5f),0x3A2804,1);
    }
  } else if(T<14.0f){
    /* the feature symbols, one at a time, each with what it does */
    float u=T-6.0f;
    int i=(int)(u/(8.0f/7.0f)); if(i>6) i=6;
    float lt=u-i*(8.0f/7.0f);
    const show_t*sh=&SHOW[i];
    dim_burst(MQH,0,FBW/2,250,artT*0.10f,16,sh->col,opt_limiter?120:200,0,1,0,0);
    /* the close-up pops in and out; while it holds, it is a plain blit */
    float z=ease_back(lt*3.2f);
    if(lt>1.0f) z*=1.0f-(lt-1.0f)*4.0f;
    const spr_t*bs=&symBig[sh->sy];
    if(bs->px){
      if(z>0.985f && z<1.015f) blit(bs,FBW/2-bs->w/2,250-bs->h/2,0,FBH,255,0,0.0f);
      else if(z>0.05f) blit_scaled(bs,FBW/2,250,z*1.08f,255);
    } else if(z>0.05f) blit_scaled(&sym[sh->sy],FBW/2,250,z*2.3f,255);
    if(lt>0.25f && lt<1.08f){
      { const spr_t*c=&capSpr[i]; blit(c,FBW/2-c->w/2,410-c->h/2,0,FBH,255,0,0.0f); }
      banner_plate(FBW/2,476,820,70,sh->col);
      text(sh->l1,FBW/2,456,3,0xFFFFFF,1,1);
      text(sh->l2,FBW/2,488,2,mixc(sh->col,0xFFFFFF,0.35f),1,1);
    }
  } else if(T<17.5f){
    /* four progressives, lit like the sign on the rail */
    float u=T-14.0f;
    dim_burst(MQH,0,FBW/2,300,artT*0.05f,24,0xC060FF,opt_limiter?90:150,0,1,0,0);
    { const spr_t*c=&capSpr[7]; blit(c,FBW/2-c->w/2,114-c->h/2,0,FBH,255,0,0.0f); }
    static const char*JN[NJP]={"ULTIMATE","MEGA","MAJOR","MINOR"};
    for(int k=0;k<NJP;k++){
      float d=clampf((u-k*0.18f)*3.0f,0,1);
      if(d<=0) continue;
      int y=178+k*96, w=seg_width(12,16,42,1)+220, x=FBW/2-w/2+(int)((1.0f-ease_back(d))*400.0f);
      fb_rrect(x,y,w,72,12,0x000000,230);
      neon_live(x,y,w,72,12,k==0?hue_at(artT*0.3f):TIERC[k],220);
      text(JN[k],x+20,y+26,3,mixc(TIERC[k],0xFFFFFF,0.2f),0,1);
      seg_num(jp_value(k),x+w-18,y+15,12,16,42,k==0?0xFFF0C0:mixc(TIERC[k],0xFFFFFF,0.35f),0x2A2230,1);
    }
  } else {
    /* the whole game in nine lines */
    float u=T-17.5f;
    dim_burst(MQH,0,FBW/2,330,artT*0.06f,18,0xFFB030,opt_limiter?80:130,0,1,0,0);
    fb_shade_rect(GX-8,150,GW+16,356,0x05030C,150);
    fb_rframe(GX-8,150,GW+16,356,14,2.0f,0xFFD24A,200);
    { const spr_t*c=&capSpr[8]; blit(c,FBW/2-c->w/2,104-c->h/2,0,FBH,255,0,0.0f); }
    for(int k=0;k<9;k++){
      float d=clampf((u-k*0.12f)*4.0f,0,1);
      if(d<=0) continue;
      int y=164+k*38, x=FBW/2+(int)((1.0f-d)*(k&1?700:-700));
      text(FEATLINES[k],x,y,3,k&1?0x9FE8FF:0xFFFFFF,1,1);
    }
  }
  /* always: PRESS START, pulsing */
  { const spr_t*p=&title[TT_PRESS];
    float pl=0.5f+0.5f*sinf(artT*4.0f);
    blit(p,FBW/2-p->w/2,566-p->h/2,0,FBH,(int)(140+115*pl),0,0.0f);
    float ph=fmodf(artT*0.9f,2.0f);
    if(ph<1.0f) shine_sprite(p,FBW/2-p->w/2,566-p->h/2,0,FBH,-40.0f+ph*(p->w+100.0f),20,150,210); }
  snprintf(b,sizeof b,"SELECT = PAY TABLE     X = MAX BET     Y = ADD CREDITS");
  text(b,FBW/2,604,2,0xAFAFC8,1,1);
}

/* ═══ LUCKY 7 PICK ═════════════════════════════════════════════════
 *  A velvet stage, nine gold-framed panels each carrying the 7 emblem,
 *  and lit readouts for what has been collected.  The stage and the
 *  panel faces are baked at init; the board is painted into a cache
 *  whenever a pick changes it, and only the cursor, the glints and the
 *  panel turning over are drawn live.
 * ================================================================= */
#define PK_PW 260
#define PK_PH 148
#define PK_GX 26
#define PK_GY 18
#define PK_BX ((FBW-(3*PK_PW+2*PK_GX))/2)
#define PK_BY 166
enum { TILE_CLOSED, TILE_CREDIT, TILE_MULT, TILE_STOP, NTILE };
static spr_t tileSpr[NTILE];
static spr_t pkGlow;          /* the cursor's neon, baked: gold halo and a white tube */

static void pk_tile_xy(int i,int*x,int*y){
  *x=PK_BX+(i%3)*(PK_PW+PK_GX); *y=PK_BY+(i/3)*(PK_PH+PK_GY);
}

/* one panel face, painted into the framebuffer at (x,y) */
static void paint_tile(int x,int y,int kind){
  static const uint32_t TOP[NTILE]={0x6A2090,0x1E5CB8,0x18A848,0xC0142A};
  static const uint32_t BOT[NTILE]={0x1A0632,0x06142E,0x04280E,0x2A0206};
  const int w=PK_PW, h=PK_PH;
  float r=18.0f;
  for(int j=0;j<h;j++) for(int i=0;i<w;i++){
    float d=rr_sdf(i+0.5f,j+0.5f,w*0.5f,h*0.5f,w*0.5f,h*0.5f,r);
    if(d>0.5f) continue;
    float t=(float)j/h;
    uint32_t c=mixc(TOP[kind],BOT[kind],t);
    float dx=(i-w*0.5f)/(w*0.5f), dy=(j-h*0.42f)/(h*0.6f);
    float lamp=clampf(1.0f-sqrtf(dx*dx*0.7f+dy*dy),0,1);
    c=mixc(c,mixc(TOP[kind],0xFFFFFF,0.35f),lamp*lamp*0.6f);
    if(kind==TILE_CLOSED){                          /* quilted velvet */
      float q=fabsf(fmodf((i+j)*0.5f,16.0f)-8.0f), q2=fabsf(fmodf((i-j+400)*0.5f,16.0f)-8.0f);
      if(q<0.9f||q2<0.9f) c=scalec(c,0.72f);
      else if(q<1.8f||q2<1.8f) c=mixc(c,0xFFFFFF,0.06f);
    } else {                                        /* a sunburst behind the prize */
      float a=fast_atan2(j-h*0.5f,i-w*0.5f)*14.0f/TAU; a-=floorf(a);
      if(a<0.5f) c=mixc(c,0xFFFFFF,0.07f*lamp);
    }
    fb_blend(x+i,y+j,c,(int)(clampf(0.5f-d,0,1)*255));
  }
  for(int j=0;j<h*0.40f;j++){                      /* glass reflection */
    float f=1.0f-j/(h*0.40f);
    for(int i=10;i<w-10;i++) fb_blend(x+i,y+4+j,0xFFFFFF,(int)(f*f*34));
  }
  fb_moulding(x,y,w,h,r,7.0f,1,255);
  fb_rframe(x+8,y+8,w-16,h-16,11,1.0f,0x000000,120);
  /* gold rivets in the corners */
  static const int RX[4]={16,PK_PW-17,16,PK_PW-17}, RY[4]={16,16,PK_PH-17,PK_PH-17};
  for(int k=0;k<4;k++) for(int j=-4;j<=4;j++) for(int i=-4;i<=4;i++){
    float dd=sqrtf((float)(i*i+j*j))/4.0f; if(dd>1.0f) continue;
    float nz=sqrtf(1.0f-dd*dd*0.9f);
    fb_blend(x+RX[k]+i,y+RY[k]+j,env_map(env_up(i/4.0f*0.9f,j/4.0f*0.9f,nz),1),(int)(255*clampf((1.0f-dd)*5.0f,0,1)));
  }
  if(kind==TILE_CLOSED){
    blit(&pickEmblem,x+w/2-pickEmblem.w/2+3,y+h/2-pickEmblem.h/2+4,0,FBH,255,0,0.0f);
  } else if(kind==TILE_STOP){
    for(int k=-3;k<=3;k++){                          /* a dark cross behind the word */
      fb_line(x+60+k,y+30,x+w-60+k,y+h-30,3,0x4A0008,160);
      fb_line(x+w-60+k,y+30,x+60+k,y+h-30,3,0x4A0008,160);
    }
  }
}

/* copy a panel out of the framebuffer with its rounded outline as alpha */
static void grab_rr(spr_t*s,int x,int y,int w,int h,float r){
  s->w=w; s->h=h;
  s->px=(uint8_t*)malloc((size_t)w*h*4);
  if(!s->px) return;
  for(int j=0;j<h;j++) for(int i=0;i<w;i++){
    uint32_t c=fb[(y+j)*FBW+x+i];
    float d=rr_sdf(i+0.5f,j+0.5f,w*0.5f,h*0.5f,w*0.5f,h*0.5f,r);
    uint8_t*o=s->px+((size_t)j*w+i)*4;
    o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
    o[3]=(uint8_t)(clampf(0.5f-d,0,1)*255);
  }
  spr_bounds(s);
}

static void paint_bonus_bg(void){
  /* a velvet stage: deep gradient, a turning fan of light behind the
     board, spots from above, bokeh, and the gold frame of the screen */
  for(int y=0;y<FBH;y++){
    uint32_t c=mixc(0x2A0A40,0x060210,(float)y/FBH);
    for(int x=0;x<FBW;x++) fb[y*FBW+x]=c;
  }
  for(int y=0;y<FBH;y++) for(int x=0;x<FBW;x++){
    float dx=x-FBW*0.5f, dy=y-400.0f;
    float a=fast_atan2(dy,dx)*28.0f/TAU; a-=floorf(a);
    float d=sqrtf(dx*dx/(700.0f*700.0f)+dy*dy/(460.0f*460.0f));
    float v=clampf(1.0f-d,0,1);
    float ray=(a<0.5f?1.0f:0.35f)*v*v*0.20f;
    fb_add(x,y,(int)(ray*230),(int)(ray*90),(int)(ray*200));
  }
  for(int i=0;i<60;i++){
    float bx=hash2(i,21,9u)*FBW, by=hash2(i,22,9u)*FBH, br=6.0f+hash2(i,23,9u)*22.0f;
    uint32_t bc=(i%3==0)?0xFFD070:((i%3==1)?0xFF60C0:0xA070FF);
    int cr=(bc>>16)&255, cg=(bc>>8)&255, cb=bc&255;
    for(int y=(int)(by-br);y<=(int)(by+br);y++) for(int x=(int)(bx-br);x<=(int)(bx+br);x++){
      if(x<0||y<0||x>=FBW||y>=FBH) continue;
      float d=sqrtf((x-bx)*(x-bx)+(y-by)*(y-by))/br; if(d>1.0f) continue;
      float v=clampf((1.0f-d)*5.0f,0,1)*(0.75f+0.25f*smooth01(0.7f,0.95f,d))*0.14f;
      fb_add(x,y,(int)(cr*v),(int)(cg*v),(int)(cb*v));
    }
  }
  /* the board's glass bed */
  int bw=3*PK_PW+2*PK_GX+40, bh=3*PK_PH+2*PK_GY+36;
  fb_softshadow(PK_BX-20,PK_BY-18,bw,bh,22,10,160);
  fb_glass(PK_BX-20,PK_BY-18,bw,bh,22,0x1A0830,0x05020C,190);
  fb_moulding(PK_BX-22,PK_BY-20,bw+4,bh+4,24,5.0f,1,255);
  /* readout plates along the top */
  static const int PX[3]={120,FBW/2-150,FBW-420}, PWd[3]={300,300,300};
  for(int k=0;k<3;k++){
    fb_softshadow(PX[k],80,PWd[k],62,14,6,140);
    fb_glass(PX[k],80,PWd[k],62,14,0x1C0A30,0x06020E,215);
    fb_moulding(PX[k]-2,78,PWd[k]+4,66,16,4.0f,1,255);
  }
  text("COLLECTED",PX[0]+PWd[0]/2,85,1,0xFFE9A8,1,1);
  text("MULTIPLIER - TIMES ALL COLLECTED",PX[1]+PWd[1]/2,85,1,0xFFE9A8,1,1);
  text("STOPS",PX[2]+PWd[2]/2,85,1,0xFFE9A8,1,1);
  led_window(PX[0]+14,96,PWd[0]-28,40);
  fb_moulding(6,4,FBW-12,FBH-8,18,6.0f,1,255);
  const spr_t*t=&title[TT_PICK];
  blit(t,FBW/2-t->w/2,42-t->h/2,0,FBH,255,0,0.0f);
  banner_plate(FBW/2,FBH-30,560,34,0xC060FF);
}

/*  The stage behind the pick board, baked once in build_pick_assets(). */
static uint32_t *pkStage;

static void paint_bonus(void){
  if(pkStage){                           /* this band's rows of the stage */
    int y0=0, y1=FBH; clip_rows(&y0,&y1);
    if(y1>y0) memcpy(fb+(size_t)y0*FBW,pkStage+(size_t)y0*FBW,(size_t)(y1-y0)*FBW*4);
  } else paint_bonus_bg();
  char b[48];
  /* collected, on an LED readout */
  seg_num(G.pickTotal,120+300-24,102,9,13,28,0xFFC040,0x3A2804,1);
  /* the multiplier, big */
  int m=G.pickMult>0?G.pickMult:1;
  snprintf(b,sizeof b,"X%d",m);
  static const uint32_t MG[5]={0xFFFFFF,0xE0FFD8,0x6AE060,0x1E8A2A,0x9AF090};
  textb(b,FBW/2,98,5,m>1?MG:SILVERG,m>1?5:4,1);
  /* stops: three lamps, lit as they are found */
  for(int i=0;i<3;i++){
    int lit = i < G.pickStops;
    int cx=FBW-420+70+i*80, cy=112;
    for(int j=-17;j<=17;j++) for(int k=-17;k<=17;k++){
      float d=sqrtf((float)(j*j+k*k));
      if(d>17.0f) continue;
      uint32_t c;
      if(d>14.0f) c=env_map(env_up(k/17.0f*0.9f,j/17.0f*0.9f,0.4f),1);
      else {
        float nz=sqrtf(1.0f-(d/14.0f)*(d/14.0f)*0.8f);
        c = lit ? mixc(0x8A0010,0xFFB0A0,clampf(nz*1.1f-0.2f+(-j-k)/40.0f,0,1))
                : mixc(0x140408,0x4A2030,clampf(nz-0.3f+(-j-k)/40.0f,0,1));
      }
      fb_blend(cx+k,cy+j,c,(int)(255*clampf(17.5f-d,0,1)));
    }
  }
  text("THIRD STOP ENDS THE ROUND",FBW-420+150,131,1,0xC8B8E0,1,1);
  (void)b;
  /* the board */
  for(int i=0;i<NPICK;i++){
    int x,y; pk_tile_xy(i,&x,&y);
    int kind = !G.pickDone[i] ? TILE_CLOSED
             : G.pickKind[i]==PICK_STOP ? TILE_STOP
             : G.pickKind[i]==PICK_MULT ? TILE_MULT : TILE_CREDIT;
    blit(&tileSpr[kind],x,y,0,FBH,255,0,0.0f);
    if(!G.pickDone[i]) continue;
    char v[16];
    if(kind==TILE_STOP) textb("STOP",x+PK_PW/2,y+PK_PH/2-24,7,REDG,4,1);
    else if(kind==TILE_MULT){ snprintf(v,sizeof v,"X%d",G.pickVal[i]); textb(v,x+PK_PW/2,y+36,10,GREENG,4,1); }
    else { commas(v,sizeof v,G.pickVal[i]); textb(v,x+PK_PW/2,y+42,strlen(v)>3?7:9,GOLDG,5,1);   /* 4+ chars: a comma */ }
  }
  text("D-PAD TO MOVE          A TO PICK",FBW/2,FBH-37,2,0xF0E0FF,1,1);
}

/* what the board looks like is a function of exactly these */
static uint32_t bonus_key(void){
  uint32_t k=(uint32_t)(G.pickStops*131u);
  for(int i=0;i<NPICK;i++) k = k*33u + (uint32_t)(G.pickDone[i]*(i+1));
  k = k*33u + (uint32_t)G.pickTotal;
  k = k*33u + (uint32_t)G.pickMult;
  return k;
}

/*  A panel turning over: squash the closed face to nothing, then open
 *  the revealed one out of it.  Horizontal scale only, nearest-column,
 *  so it costs one panel's pixels.                                    */
static void blit_hsquash(const uint32_t*src,int sw,int sx,int sy,int w,int h,
                         int dx,int dy,float sc,const spr_t*spr){
  int dw=(int)(w*sc); if(dw<1) return;
  int x0=dx+(w-dw)/2;
  for(int j=0;j<h;j++){
    int y=dy+j;
    if(y<clip_y0||y>=clip_y1||y<0||y>=FBH) continue;
    for(int i=0;i<dw;i++){
      int u=(int)((i+0.5f)/sc);
      if(u>=w) u=w-1;
      if(spr){
        const uint8_t*p=spr->px+((size_t)j*spr->w+u)*4;
        if(!p[3]) continue;
        fb_blend(x0+i,y,RGB(p[0],p[1],p[2]),p[3]);
      } else {
        uint32_t c=src[(size_t)(sy+j)*sw+sx+u];
        fb[(size_t)y*FBW+x0+i]=c;
      }
    }
  }
}

static void draw_bonus(void){
  /* band-aware: this band's rows come from the cache, or are painted
     and stored into it (see frame_cache) */
  frame_cache(&bnimg,bonus_key(),paint_bonus);

  /* the panel just picked turns over */
  const float FLIP=0.42f;
  if(flipIdx>=0 && G.pickT<FLIP+0.5f && pkStage && bnimg.px){
    int x,y; pk_tile_xy(flipIdx,&x,&y);
    float u=G.pickT/FLIP;
    if(u<1.0f){
      /* clear the panel's slot back to the stage */
      for(int j=0;j<PK_PH;j++){
        int yy=y+j; if(yy<clip_y0||yy>=clip_y1) continue;
        memcpy(fb+(size_t)yy*FBW+x,pkStage+(size_t)yy*FBW+x,PK_PW*4);
      }
      if(u<0.5f) blit_hsquash(NULL,0,0,0,PK_PW,PK_PH,x,y,1.0f-u*2.0f,&tileSpr[TILE_CLOSED]);
      else       blit_hsquash(bnimg.px,FBW,x,y,PK_PW,PK_PH,x,y,(u-0.5f)*2.0f,NULL);
    }
    /* a flash as it lands */
    float f=clampf(1.0f-(G.pickT-FLIP*0.5f)/0.5f,0,1);
    if(G.pickT>FLIP*0.5f && f>0){
      int kind=G.pickKind[flipIdx];
      uint32_t fc = kind==PICK_STOP?0xFF3020:(kind==PICK_MULT?0x60FF80:0xFFD060);
      (void)fc;
      blit_add(&pkGlow,x-22,y-22,(int)(255*f));
      blit_add(&sparkspr,x-7,y-7,(int)(255*f)); blit_add(&sparkspr,x+PK_PW-7,y+PK_PH-7,(int)(255*f));
    }
  }

  /* glints drifting over the closed panels */
  for(int i=0;i<NPICK;i++){
    if(G.pickDone[i]) continue;
    int x,y; pk_tile_xy(i,&x,&y);
    float ph=fmodf(artT*0.8f+dhash(i,3)*4.0f,4.0f);
    if(ph<0.7f){
      float pos=-40.0f+ph/0.7f*(PK_PW+120.0f);
      shine_sprite(&tileSpr[TILE_CLOSED],x,y,0,FBH,pos,22,90,200);
    }
  }
  /* the cursor: a breathing neon frame with sparks running round it */
  { int x,y; pk_tile_xy(G.pickCur,&x,&y);
    float pl=0.5f+0.5f*sinf(artT*8.0f);
    blit_add(&pkGlow,x-22,y-22,(int)(150+106*pl));
    float per=2.0f*(PK_PW+PK_PH+24);
    for(int i=0;i<6;i++){
      float d=fmodf(artT*420.0f+i*per/6.0f,per);
      int w=PK_PW+12, h=PK_PH+12, sx,sy;
      if(d<w){ sx=x-6+(int)d; sy=y-6; }
      else if(d<w+h){ sx=x-6+w; sy=y-6+(int)(d-w); }
      else if(d<2*w+h){ sx=x-6+w-(int)(d-w-h); sy=y-6+h; }
      else { sx=x-6; sy=y-6+h-(int)(d-2*w-h); }
      blit_add(&sparkspr,sx-7,sy-7,230);
    } }
}

/* baked at init: the panel faces, and the stage, so no frame of the
   bonus ever has to paint a full-screen gradient */
static void build_pick_assets(void){
  { const int M=22, w=PK_PW+2*M, h=PK_PH+2*M;
    pkGlow.w=w; pkGlow.h=h;
    pkGlow.px=(uint8_t*)calloc((size_t)w*h,4);
    if(pkGlow.px){
      for(int j=0;j<h;j++) for(int i=0;i<w;i++){
        float d=rr_sdf(i+0.5f,j+0.5f,w*0.5f,h*0.5f,PK_PW*0.5f+5,PK_PH*0.5f+5,22.0f);
        float halo=d>0?expf(-d*0.22f):expf(d*0.9f);
        float tube=clampf(1.6f-fabsf(d)*0.8f,0,1);
        float r=255*(halo*0.85f+tube), g=210*halo*0.85f+255*tube, b=90*halo*0.85f+255*tube;
        if(r+g+b<6) continue;
        uint8_t*o=pkGlow.px+((size_t)j*w+i)*4;
        o[0]=(uint8_t)clampi((int)r,0,255); o[1]=(uint8_t)clampi((int)g,0,255); o[2]=(uint8_t)clampi((int)b,0,255); o[3]=255;
      }
      spr_bounds(&pkGlow);
    } }
  uint32_t*save=(uint32_t*)malloc(sizeof bg);
  if(!save) return;
  memcpy(save,fb,sizeof bg);
  for(int k=0;k<NTILE;k++){
    fb_rrect(100,100,PK_PW,PK_PH,18,0x000000,255);
    paint_tile(100,100,k);
    grab_rr(&tileSpr[k],100,100,PK_PW,PK_PH,18.0f);
  }
  if(!pkStage){
    paint_bonus_bg();
    pkStage=(uint32_t*)malloc(sizeof bg);
    if(pkStage) memcpy(pkStage,fb,sizeof bg);
  }
  memcpy(fb,save,sizeof bg);
  free(save);
}

static void draw_overlays(void){
  if(G.inFree && (G.state==ST_IDLE||G.state==ST_SPIN||G.state==ST_EVAL||G.state==ST_SHOWWIN))
    draw_fsbar();                       /* the top box turns into the feature's status bar */
  switch(G.state){
  case ST_ATTRACT:  draw_attract();  break;
  case ST_FSINTRO:  draw_fsintro();  break;
  case ST_BONUSEND: draw_bonusend(); break;
  case ST_BROKE:    draw_broke();    break;
  case ST_ADDCR:
    draw_addcr();
    break;
  }
  if(G.multUp>0 && G.inFree &&
     (G.state==ST_IDLE||G.state==ST_SPIN||G.state==ST_EVAL||G.state==ST_SHOWWIN)){
    /* short, loud, and gone: the meter climbing is the best news in the
       feature, so it gets the middle of the screen - rays, the number
       slamming in, its sparks flying off to the meter (w7_fx.c) */
    fx_multup_draw();
  }
  /* BIG / SUPER / MEGA / EPIC WIN, while the count rolls (w7_fx.c) */
  fx_bigwin_draw();
  if(G.state==ST_JACKPOT)  draw_jackpot();
  if(G.state==ST_PAYTABLE) draw_paytable();
  if(G.state==ST_BONUS)    draw_bonus();
}

/* ═══ THE MARQUEE ══════════════════════════════════════════════════
 *  A slot's top box is the thing that pulls a player across the room, so
 *  nothing on it stands still: a ticker with the live ULTIMATE pot scrolls
 *  behind a lit title sign, a shine sweeps the sign, the lettering pulses
 *  gold to white-hot, two colours of bulb chase round the edge and
 *  sparkles twinkle across the face.  Its own clock, so it never pauses
 *  when a state timer resets.                                          */
static float mqT;

/*  The art's own clocks and caches.  They advance here, from update(),
 *  so no draw function ever writes state: the band renderer runs every
 *  draw once per band, and a clock bumped in a draw would run three
 *  times as fast.  None of this is game state, so none of it is saved.  */
static int   bgShown = 0;       /* which cabinet look is in bg[]        */
static int   pickSeen[NPICK];   /* the pick board as last seen          */
static void art_update(void){
  artT += DT;
  mqT  += DT;
  for(int i=0;i<NBTN;i++) if(btnFlash[i]>0) btnFlash[i]-=DT;
  /* free spins get their own cabinet: night sky, gilded drums */
  int fs = G.inFree || G.state==ST_FSINTRO || (G.state==ST_BONUSEND && G.banner==3);
  if(fs!=bgShown && bgBase && bgFree){ memcpy(bg,fs?bgFree:bgBase,sizeof bg); bgShown=fs; }
  /* the pick board: time the turn of the panel just picked */
  if(G.state==ST_BONUS || G.state==ST_BONUSEND){
    G.pickT += DT;
    for(int i=0;i<NPICK;i++){
      if(G.pickDone[i] && !pickSeen[i]) flipIdx=i;
      pickSeen[i]=G.pickDone[i];
    }
  } else {
    for(int i=0;i<NPICK;i++) pickSeen[i]=0;
    flipIdx=-1;
  }
}

/* fixed-size text with no auto-fit, glyphs off screen skipped: the ticker */
static void text_run(const char*s,int x,int y,int px,uint32_t col,int shadow){
  int n=(int)strlen(s), adv=px*6;
  if(!rows_visible(y, y+px*7+(shadow?px:0))) return;
  for(int pass=(shadow?0:1); pass<2; pass++){
    uint32_t c = pass==0 ? 0x000000 : col;
    int off = pass==0 ? px : 0;
    for(int i=0;i<n;i++){
      int gx0=x+i*adv;
      if(gx0+px*5<0 || gx0>=FBW) continue;
      glyph_blocks(glyph_of((unsigned char)s[i]),gx0+off,y+off,px,c);
    }
  }
}

static void commas(char*out,size_t n,long long v){
  char raw[24]; snprintf(raw,sizeof raw,"%lld",v);
  int len=(int)strlen(raw), o=0;
  for(int i=0;i<len && (size_t)o+1<n;i++){
    out[o++]=raw[i];
    int left=len-1-i;
    if(left>0 && left%3==0 && (size_t)o+1<n) out[o++]=',';
  }
  out[o]=0;
}

/*  The sign in the middle of the top box: a lit plate with the logo on
 *  it, baked once with rounded, transparent corners so the ticker can
 *  pass behind it.  The logo's halo is kept apart as an additive sprite
 *  so it can breathe.                                                 */
static spr_t mqPlate, logoGlow;
static uint8_t beamLUT[161];
static int logoX, logoY;               /* where the logo sits in the plate */

static void build_marquee(void){
  int x=MQPX, y=MQPY, w=MQPW, h=MQPH;
  uint32_t*save=(uint32_t*)malloc((size_t)(w+8)*(h+8)*4);
  if(!save) return;
  for(int j=0;j<h+8;j++) for(int i=0;i<w+8;i++){
    int fx=x-4+i, fy=y-4+j;
    save[j*(w+8)+i]=((unsigned)fx<FBW&&(unsigned)fy<FBH)?fb[fy*FBW+fx]:0;
  }
  fb_rrectg(x,y,w,h,14,0x4A0818,0x0C0104,255);
  for(int j=0;j<h;j++) for(int i=0;i<w;i++){          /* a lamp behind the lettering */
    float dx=(i-w*0.5f)/(w*0.5f), dy=(j-h*0.55f)/(h*0.8f), d=dx*dx+dy*dy;
    float v=clampf(1.0f-d,0,1); v*=v;
    fb_add(x+i,y+j,(int)(v*110),(int)(v*36),(int)(v*10));
  }
  for(int j=0;j<h/2;j++) for(int i=6;i<w-6;i++) fb_blend(x+i,y+2+j,0xFFFFFF,(h/2-j)/2);
  fb_moulding(x,y,w,h,14,4.0f,1,255);
  const spr_t*L=&title[TT_LOGO];
  logoX=x+w/2-L->w/2; logoY=y+h/2-L->h/2+1;
  blit(L,logoX,logoY,y+3,y+h-3,255,0,0.0f);
  /* grab it with the plate's rounded outline as its alpha */
  mqPlate.w=w; mqPlate.h=h;
  mqPlate.px=(uint8_t*)malloc((size_t)w*h*4);
  if(mqPlate.px){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++){
      uint32_t c=fb[(y+j)*FBW+x+i];
      float d=rr_sdf(i+0.5f,j+0.5f,w*0.5f,h*0.5f,w*0.5f,h*0.5f,14);
      uint8_t*o=mqPlate.px+((size_t)j*w+i)*4;
      o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
      o[3]=(uint8_t)(clampf(0.5f-d,0,1)*255);
    }
    spr_bounds(&mqPlate);
  }
  for(int j=0;j<h+8;j++) for(int i=0;i<w+8;i++){
    int fx=x-4+i, fy=y-4+j;
    if((unsigned)fx<FBW&&(unsigned)fy<FBH) fb[fy*FBW+fx]=save[j*(w+8)+i];
  }
  free(save);
  /* the logo's breathing halo: its alpha, blurred, in a hot orange */
  { int gw=L->w, gh=L->h;
    float*a=(float*)calloc((size_t)gw*gh,4), *t=(float*)malloc((size_t)gw*gh*4);
    logoGlow.w=gw; logoGlow.h=gh;
    logoGlow.px=(uint8_t*)calloc((size_t)gw*gh,4);
    if(a&&t&&logoGlow.px){
      for(int i=0;i<gw*gh;i++) a[i]=L->px[i*4+3]>200?1.0f:0.0f;
      box_blur_f(a,t,gw,gh,3,3);
      for(int i=0;i<gw*gh;i++){
        float v=clampf(a[i]*1.6f,0,1)*(L->px[i*4+3]>200?0.35f:1.0f);
        uint8_t*o=logoGlow.px+i*4;
        o[0]=(uint8_t)(255*v); o[1]=(uint8_t)(120*v); o[2]=(uint8_t)(40*v); o[3]=v>0.01f?255:0;
      }
      spr_bounds(&logoGlow);
    }
    free(a); free(t); }
  for(int i=0;i<161;i++){ float u=(i-80)/80.0f, v=1.0f-u*u; beamLUT[i]=(uint8_t)(v*v*255); }
}

static void draw_marquee(void){
  float lim = opt_limiter ? 0.6f : 1.0f;
  int y0=8>clip_y0?8:clip_y0, y1=(MQH-8)<clip_y1?(MQH-8):clip_y1;

  /* 1. two searchlights sweeping the face, leaning with their swing */
  for(int b=0;b<2 && y0<y1;b++){
    float ph=mqT*(0.42f+b*0.17f)+b*2.4f;
    float cxb=FBW*0.5f+sinf(ph)*(FBW*0.5f-40.0f), slope=cosf(ph)*2.2f*(b?-1:1);
    int kr=(int)(62*lim), kg=(int)(54*lim), kb=(int)(40*lim);
    for(int y=y0;y<y1;y++){
      int c=(int)(cxb+(y-MQH*0.5f)*slope);
      int xa=c-80, xb=c+80;
      if(xa<12) xa=12;
      if(xb>FBW-13) xb=FBW-13;
      uint32_t*row=fb+(size_t)y*FBW;
      for(int x=xa;x<=xb;x++){
        int v=beamLUT[x-c+80];
        uint32_t d=row[x];
        int r=((d>>16)&255)+(kr*v>>8), g=((d>>8)&255)+(kg*v>>8), bl=(d&255)+(kb*v>>8);
        row[x]=RGB(r>255?255:r,g>255?255:g,bl>255?255:bl);
      }
    }
  }

  /* 2. the ticker, passing behind the sign (free spins put their own
     status bar over it, so it rests then) */
  char pot[24], tk[520];
  if(!G.inFree){
  commas(pot,sizeof pot,jp_value(JP_ULT));
  snprintf(tk,sizeof tk,
    "  *  WAYS PAY - LEFT TO RIGHT, REEL TO REEL  *  EVERY WILD 7 DOUBLES THE WIN  *  "
    "FREE SPINS - THE MULTIPLIER CLIMBS TO X5  *  LUCKY 7 PICK BONUS  *  "
    "HOLD & SPIN - 6 OR MORE LUCKY COINS  *  WHEEL OF 7'S - 3 WHEELS ON REELS 2, 3 AND 4  *  "
    "7 STRIKE - RANDOM WILDS  *  GAMBLE ANY WIN  *  4 PROGRESSIVE JACKPOTS - ULTIMATE NOW %s  ",pot);
  int tw=(int)strlen(tk)*12;
  int off=(int)fmodf(mqT*110.0f,(float)tw);
  text_run(tk,20-off,   MQH/2-7,2,0xFFF4D0,1);
  text_run(tk,20-off+tw,MQH/2-7,2,0xFFF4D0,1);
  }

  /* 3. the sign, its halo breathing, a shine crossing the logo */
  blit(&mqPlate,MQPX,MQPY,0,FBH,255,0,0.0f);
  float pulse=0.5f+0.5f*sinf(mqT*3.4f);
  blit_add(&logoGlow,logoX,logoY,(int)((70+150*pulse)*lim));
  float sx=fmodf(mqT*260.0f,(float)(title[TT_LOGO].w+700))-120.0f;
  shine_sprite(&title[TT_LOGO],logoX,logoY,MQPY,MQPY+MQPH,sx,18,(int)(190*lim),150);

  /* 4. bulbs round the edge: chase, then all-blink, then a fill sweep */
  int n=(FBW-40)/26, mode=((int)(mqT/5.0f))%3;
  float mt=fmodf(mqT,5.0f);
  for(int i=0;i<n;i++){
    float v;
    if(mode==0)      v=0.5f+0.5f*sinf(mqT*6.0f-i*0.60f);
    else if(mode==1) v=((((int)(mqT*4.0f))+i)&1)?1.0f:0.15f;
    else             v=(i <= (int)(mt/4.0f*(n+1))) ? 1.0f : 0.2f;
    if(opt_limiter) v=0.25f+v*0.55f;
    int x=22+i*26;
    int k=(int)(v*255);
    blit_add(&bulbspr[i&1],x-9,MQH-9-9,k);
    if(x<MQPX-10||x>MQPX+MQPW+10) blit_add(&bulbspr[i&1],x-9,9-9,k);
  }

  /* 5. glints wandering the face */
  for(int i=0;i<7;i++){
    float ph=mqT*1.7f+i*2.39f;
    float tw2=sinf(ph*3.1f); if(tw2<0.35f) continue;
    float x=fmodf(i*181.0f + mqT*(16.0f+i*3.0f), (float)(FBW-40))+20.0f;
    float y=14.0f+fmodf(i*11.0f, (float)(MQH-28));
    if(x>MQPX-10&&x<MQPX+MQPW+10) continue;
    blit_add(&sparkspr,(int)x-7,(int)y-7,(int)(tw2*230*lim));
  }
}

/*  Everything the art bakes at init, freed on the way out.            */
static void spr_free(spr_t*s){ free(s->px); free(s->rx0); free(s->rx1); memset(s,0,sizeof *s); }
static void art_free(void){
  for(int i=0;i<NSYM;i++){ spr_free(&symb2[i]); spr_free(&symBig[i]); }
  for(int k=0;k<NFLAME;k++) spr_free(&symfl[k]);
  free(bgBase); bgBase=NULL; free(bgFree); bgFree=NULL; bgShown=0;
  free(s7bg); free(s7fg); s7bg=s7fg=NULL; s7cached=0;
  free(medalRing); medalRing=NULL;
  spr_free(&domehalo); spr_free(&bulbspr[0]); spr_free(&bulbspr[1]); spr_free(&sparkspr);
  for(int i=0;i<NBTN;i++){ spr_free(&btnspr[i][0]); spr_free(&btnspr[i][1]); }
  spr_free(&mqPlate); spr_free(&logoGlow);
  spr_free(&fireStrip);
  spr_free(&pickEmblem);
  free(pkStage); pkStage=NULL;
  for(int k=0;k<NTILE;k++) spr_free(&tileSpr[k]);
  spr_free(&pkGlow);
  free(rayAng); free(rayFall); rayAng=rayFall=NULL;
  for(int k=0;k<NFF;k++) spr_free(&fireFade[k]);
  free_titles();
}

/* ═══ FEATURE MODULES (unity build) ═══════════════════════════════
 *  Included here so they can use every primitive above; their
 *  prototypes were declared by the headers near game_t.              */
#include "w7_fx.c"
#include "w7_hold.c"
#include "w7_wheel.c"
#include "w7_extra.c"

/*  The draw list.  It runs once per band, on several threads at once,
 *  each with its own clip rows (see src/w7_thread.c and the render
 *  contract in DEVELOPING.md): nothing reachable from here may change
 *  state, and the frame is only whole again after the bands join.     */
static void draw_frame(void){
  /* The wheel scene covers every pixel, so neither the cabinet nor the
     backdrop copy is drawn under it (see render_band); wheel_draw()
     draws the particles itself.  The pay table and the pick board
     repaint every row from a cached frame (draw_overlays ->
     draw_paytable / draw_bonus), so the cabinet under them would be
     drawn only to be overwritten: skip it too.  If one of those screens
     ever lets the cabinet show through, drop its state from the test. */
  if(G.state==ST_WHEEL) wheel_draw();
  else {
    int opaque = (G.state==ST_PAYTABLE || G.state==ST_BONUS);
    if(!opaque){
      draw_marquee();
      if(G.state==ST_HOLD) hold_draw();   /* the bonus owns the reel window */
      else {
        draw_reels();
        hold_draw_cells();                /* coin values over landed coins  */
        extra_draw_reels();               /* 7 STRIKE over the reels        */
      }
      draw_features();
      draw_wins();
      draw_parts();
      fx_draw();                          /* world-layer effects            */
      draw_meters();
    }
    draw_overlays();
  }
  if(G.state==ST_GAMBLE) gamble_draw();   /* full-screen feature scenes     */
  fx_draw_top();                          /* transitions, top-layer effects */
  if(G.flash>0.001f){
    float f=G.flash; if(opt_limiter && f>0.35f) f=0.35f;
    screen_tint(0xFFFFFF,(int)(f*110));
  }
}

/*  One band: the backdrop's rows, then the whole draw list clipped to
 *  them.  The clip goes back to the full frame afterwards, so serial
 *  code after the join (fx_post) sees all of it.                      */
static void render_band(int y0,int y1){
  clip_y0=y0; clip_y1=y1;
  if(G.state!=ST_WHEEL)                   /* the wheel covers every pixel */
    memcpy(fb+(size_t)y0*FBW,bg+(size_t)y0*FBW,(size_t)(y1-y0)*FBW*sizeof(uint32_t));
  draw_frame();
  clip_y0=0; clip_y1=FBH;
}

static void render(void){
  bp_run(render_band);                    /* every band; 1 thread = 1 call  */
  render_commit();                        /* caches the bands filled        */
  fx_post();                              /* whole-frame post pass (shake)  */
}

/* ═══ LIBRETRO API ════════════════════════════════════════════════ */
static int assets_ready = 0;

static void reset_game(void){
  memset(&G,0,sizeof G);
  G.credits=5000; G.betIdx=2;               /* bet 50 */
  G.state=ST_ATTRACT; G.t=3.0f;
  G.seed=rngs;
  jp_seed();
  for(int r=0;r<NREEL;r++){ G.rpos[r]=(float)irnd(STRIPLEN); G.rstate[r]=0; }
  snapshot_grid();
}

void retro_init(void){
  { const char*e;
    if((e=getenv("WILD7_AUTOPILOT"))) dbg_pilot=atoi(e);
    if((e=getenv("WILD7_FORCE")))
      dbg_force = !strcmp(e,"free")?1:(!strcmp(e,"pick")?2:
                  (!strcmp(e,"win")?3:(!strcmp(e,"mega")?4:
                  (!strcmp(e,"minor")?5:(!strcmp(e,"ult")?6:
                  (!strcmp(e,"hold")?7:(!strcmp(e,"wheel")?8:(!strcmp(e,"storm")?9:0))))))));
    /* big-win presentation tests: genuine stops worth 10x / 25x / 50x /
       100x the bet or more (force_win_search) */
    if(e && !strcmp(e,"big"))     dbg_force=20;
    if(e && !strcmp(e,"super"))   dbg_force=21;
    if(e && !strcmp(e,"megawin")) dbg_force=22;
    if(e && !strcmp(e,"epic"))    dbg_force=23;
    if(e && !strcmp(e,"fsmult"))  dbg_force=10;   /* the multiplier pop */
    if((e=getenv("WILD7_THREADS"))) dbg_threads=atoi(e);
    if((e=getenv("WILD7_BANDS")))   dbg_bands=atoi(e);
    if((e=getenv("WILD7_PROFILE"))) dbg_profile=atoi(e);
  }
  if(!assets_ready){
    build_strips();
    build_sprites();
    build_dome(0);
    build_dome(1);
    build_bg();
    extra_init();               /* 7 STRIKE clouds, gamble cards and table */
    fx_init();                              /* after the symbol sprites */
    assets_ready=1;
  }
  reset_game();
}
void retro_deinit(void){
  bp_shutdown();                          /* join the band workers first */
  fcache_free(&ptbg); fcache_free(&bnbg);
  fcache_free(&ptimg[0]); fcache_free(&ptimg[1]); fcache_free(&ptimg[2]);
  fcache_free(&bnimg);
  for(int i=0;i<TBC;i++){ free(tbc[i].fill); free(tbc[i].out);
                          tbc[i].fill=tbc[i].out=NULL; tbc[i].used=0; tbc[i].pins=0; }
  for(int i=0;i<NSYM;i++){
    free(sym[i].px);   sym[i].px=NULL;  free(sym[i].rx0);  free(sym[i].rx1);  sym[i].rx0=sym[i].rx1=NULL;
    free(symb[i].px);  symb[i].px=NULL; free(symb[i].rx0); free(symb[i].rx1); symb[i].rx0=symb[i].rx1=NULL;
  }
  free(glowspr.px); glowspr.px=NULL;
  free(washspr.px); washspr.px=NULL;
  art_free();                             /* everything the art baked */
  for(int i=0;i<2;i++){ free(domespr[i].px); domespr[i].px=NULL; }
  fx_deinit();
  assets_ready=0;
}
unsigned retro_api_version(void){ return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info*info){
  memset(info,0,sizeof(*info));
  info->library_name     = "Wild 7's";
  info->library_version  = "3.0.1";
  info->valid_extensions = "w7|wild7";
  info->need_fullpath    = false;
  info->block_extract    = true;
}
void retro_get_system_av_info(struct retro_system_av_info*info){
  memset(info,0,sizeof(*info));
  info->timing.fps         = 60.0;
  info->timing.sample_rate = (double)SRATE;
  info->geometry.base_width   = FBW;
  info->geometry.base_height  = FBH;
  info->geometry.max_width    = FBW;
  info->geometry.max_height   = FBH;
  info->geometry.aspect_ratio = 16.0f/9.0f;
}

static const struct retro_variable VARS[] = {
  { "wild7_sound",    "Sound; on|off" },
  { "wild7_music",    "Music; on|off" },
  { "wild7_turbo",    "Turbo spin; off|on" },
  { "wild7_limiter",  "Flash limiter (photosensitivity); on|off" },
  { "wild7_threads",  "Render threads (auto = CPU cores - 1, up to 3); auto|1|2|3|4" },
  { NULL, NULL }
};
static void check_vars(void){
  struct retro_variable v;
  v.key="wild7_sound"; v.value=NULL;
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&v)&&v.value) opt_sound  = strcmp(v.value,"off")!=0;
  v.key="wild7_music"; v.value=NULL;
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&v)&&v.value) opt_music  = strcmp(v.value,"off")!=0;
  v.key="wild7_turbo"; v.value=NULL;
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&v)&&v.value) opt_turbo  = strcmp(v.value,"on")==0;
  v.key="wild7_limiter"; v.value=NULL;
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&v)&&v.value) opt_limiter= strcmp(v.value,"off")!=0;
  v.key="wild7_threads"; v.value=NULL;
  opt_threads=0;                               /* auto */
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE,&v)&&v.value) opt_threads=atoi(v.value);
  /* the pool is only ever rebuilt here, between frames */
  bp_config(dbg_threads>0 ? dbg_threads : (opt_threads>0 ? opt_threads : bp_auto_threads()),
            dbg_bands);
}

void retro_set_environment(retro_environment_t cb){
  environ_cb=cb;
  bool nogame=true;
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME,&nogame);
  cb(RETRO_ENVIRONMENT_SET_VARIABLES,(void*)VARS);
  static const struct retro_input_descriptor desc[] = {
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_START, "Spin / Stop"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A,     "Spin / Pick"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_B,     "Skip / Cancel"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_X,     "Max bet + spin"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_Y,     "Add credits"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_UP,    "Bet +"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_DOWN,  "Bet -"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_RIGHT, "Bet +"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_LEFT,  "Bet -"},
    {0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_SELECT,"Pay table"},
    {0},
  };
  cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,(void*)desc);
}
void retro_set_video_refresh(retro_video_refresh_t cb){ video_cb=cb; }
void retro_set_audio_sample(retro_audio_sample_t cb){ (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb){ audio_batch_cb=cb; }
void retro_set_input_poll(retro_input_poll_t cb){ input_poll_cb=cb; }
void retro_set_input_state(retro_input_state_t cb){ input_state_cb=cb; }

bool retro_load_game(const struct retro_game_info*info){
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,&fmt)){
    if(log_cb) log_cb(RETRO_LOG_ERROR,"XRGB8888 not supported\n");
    return false;
  }
  struct retro_log_callback lg;
  if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE,&lg)) log_cb=lg.log;
  check_vars();
  /* the .w7 "ROM" is a plain-text cabinet settings blob; absent is fine */
  if(info && info->data && info->size){
    char buf[513]; size_t m = info->size<512?info->size:512;
    memcpy(buf,info->data,m); buf[m]=0;
    for(char*p=buf;*p;p++) if(*p>='A'&&*p<='Z') *p+=32;
    /*  One "key=value" per line; ';' and '#' lines are comments.  This
     *  used to search the whole text for "turbo=on", which the file's
     *  own comment "; turbo=on|off" contains - so every cabinet ran in
     *  turbo - and "credits=" matched the comment "; credits=N" first. */
    char*save=NULL;
    for(char*ln=strtok_r(buf,"\r\n",&save); ln; ln=strtok_r(NULL,"\r\n",&save)){
      while(*ln==' '||*ln=='\t') ln++;
      if(*ln==';'||*ln=='#'||!*ln) continue;
      char*eq=strchr(ln,'='); if(!eq) continue;
      *eq=0; char*k=ln, *v=eq+1;
      for(char*e=eq-1; e>=k && (*e==' '||*e=='\t'); e--) *e=0;
      while(*v==' '||*v=='\t') v++;
      for(char*e=v+strlen(v)-1; e>=v && (*e==' '||*e=='\t'); e--) *e=0;
      /* the file can only move a setting away from its default, so it
         never undoes what the player chose in RetroArch's core options */
      int on = !strcmp(v,"on"), off = !strcmp(v,"off");
      if(!strcmp(k,"sound")   && off) opt_sound  = 0;
      if(!strcmp(k,"music")   && off) opt_music  = 0;
      if(!strcmp(k,"turbo")   && on)  opt_turbo  = 1;
      if(!strcmp(k,"limiter") && off) opt_limiter= 0;
      if(!strcmp(k,"credits")){ long long n=atoll(v); if(n>0 && n<=1000000000LL) G.credits=n; }
    }
  }
  return true;
}
bool retro_load_game_special(unsigned t,const struct retro_game_info*i,size_t n){
  (void)t;(void)i;(void)n; return false;
}
void retro_unload_game(void){}
unsigned retro_get_region(void){ return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id){ (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id){ (void)id; return 0; }
void retro_reset(void){ reset_game(); }

size_t retro_serialize_size(void){ return sizeof(G); }
bool retro_serialize(void*d,size_t s){ if(s<sizeof(G)) return false; memcpy(d,&G,sizeof(G)); return true; }
bool retro_unserialize(const void*d,size_t s){ if(s!=sizeof(G)) return false;   /* another version's state: refuse, not garbage */ memcpy(&G,d,sizeof(G)); return true; }

void retro_cheat_reset(void){}
void retro_cheat_set(unsigned i,bool e,const char*c){ (void)i;(void)e;(void)c; }
void retro_set_controller_port_device(unsigned p,unsigned d){ (void)p;(void)d; }

/*  WILD7_PROFILE: the core's own frame timing, so a soak on the Pi can
 *  see render cost apart from RetroArch's.  Off unless the env is set. */
static double prof_ms(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec*1e3 + ts.tv_nsec*1e-6;
}
static void prof_frame(double upd,double ren){
  static double us=0, um=0, rs=0, rm=0;
  static long n=0, frame=0;
  us+=upd; rs+=ren; if(upd>um) um=upd; if(ren>rm) rm=ren;
  n++; frame++;
  if(n<300) return;
  fprintf(stderr,"[wild7] frames %ld-%ld  render mean %.2f max %.2f ms  "
          "update mean %.3f max %.3f ms  threads %d bands %d  state %d\n",
          frame-n,frame-1,rs/n,rm,us/n,um,bp_nthreads,bp_nbands,G.state);
  us=um=rs=rm=0; n=0;
}

void retro_run(void){
  bool upd=false;
  if(environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&upd) && upd) check_vars();
  poll_input();
  if(dbg_profile){
    double t0=prof_ms(); update();
    double t1=prof_ms(); render();
    prof_frame(t1-t0,prof_ms()-t1);
  } else {
    update();
    render();
  }
  audio_frame();
  if(audio_batch_cb) audio_batch_cb(abuf,SPF);
  video_cb(fb,FBW,FBH,FBW*sizeof(uint32_t));
}
