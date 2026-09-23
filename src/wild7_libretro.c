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

/*  The rows the current render thread may write, [clip_y0, clip_y1).
 *  Single-threaded this is the whole frame; the band renderer narrows it
 *  per thread.  Any loop that writes fb[] directly must honour it.     */
static int clip_y0 __attribute__((unused)) = 0, clip_y1 __attribute__((unused)) = FBH;

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

/* ── test hooks ─────────────────────────────────
 *  Off unless the matching environment variable is set, so they can
 *  never touch normal play.  They drive the real input/update/render
 *  path rather than short-circuiting it — WILD7_FORCE picks genuine
 *  reel stops that happen to show the trigger symbol, it does not
 *  fake the grid afterwards.
 *    WILD7_AUTOPILOT=1  spin loop   =2 pay table   =5 features page
 *                    =3  one spin   =4 add credits =6 climb the bet
 *                    =3  a single spin, then no further input
 *    WILD7_FORCE=free|pick             land a bonus trigger
 *    WILD7_FORCE=mega|minor           force the jackpot roll to hit
 * ──────────────────────────────────────────── */
static int dbg_pilot = 0, dbg_force = 0;
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
  /* straight-alpha over */
  int da = p[3];
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
static void cv_poly_outline(const pt_t*p,int n,float grow,uint32_t col,int alpha){
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

/* ═══ FRAMEBUFFER OPS ══════════════════════════════════════════════ */
static inline void fb_px(int x,int y,uint32_t c){
  if((unsigned)x<FBW && (unsigned)y<FBH) fb[y*FBW+x]=c;
}
static inline void fb_blend(int x,int y,uint32_t c,int a){
  if((unsigned)x>=FBW || (unsigned)y>=FBH || a<=0) return;
  if(a>=255){ fb[y*FBW+x]=c; return; }
  uint32_t d=fb[y*FBW+x];
  int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
  int sr=(c>>16)&255, sg=(c>>8)&255, sb=c&255;
  fb[y*FBW+x] = RGB(dr+((sr-dr)*a>>8), dg+((sg-dg)*a>>8), db+((sb-db)*a>>8));
}
static inline void fb_add(int x,int y,int r,int g,int b){
  if((unsigned)x>=FBW || (unsigned)y>=FBH) return;
  uint32_t d=fb[y*FBW+x];
  int dr=((d>>16)&255)+r, dg=((d>>8)&255)+g, db=(d&255)+b;
  fb[y*FBW+x] = RGB(dr>255?255:dr, dg>255?255:dg, db>255?255:db);
}
static void fb_rect(int x,int y,int w,int h,uint32_t c,int a){
  for(int j=0;j<h;j++) for(int i=0;i<w;i++) fb_blend(x+i,y+j,c,a);
}
static void fb_frame(int x,int y,int w,int h,int t,uint32_t c,int a){
  for(int k=0;k<t;k++){
    for(int i=0;i<w;i++){ fb_blend(x+i,y+k,c,a); fb_blend(x+i,y+h-1-k,c,a); }
    for(int j=0;j<h;j++){ fb_blend(x+k,y+j,c,a); fb_blend(x+w-1-k,y+j,c,a); }
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
 *  primitives in the renderer, so the saving shows up everywhere.     */
static void fb_rrectg(int x,int y,int w,int h,float r,
                      uint32_t top,uint32_t bot,int alpha){
  if(w<=0||h<=0) return;
  float cx=x+w*0.5f, cy=y+h*0.5f, hw=w*0.5f, hh=h*0.5f;
  int endband=(int)(r+2.0f);
  if(endband*2 > h) endband = h/2;
  for(int j=0;j<h;j++){
    uint32_t col=mixc(top,bot,(float)j/(h>1?h-1:1));
    if(j>=endband && j<h-endband){
      /* straight-sided row: solid, with the two edge pixels antialiased */
      for(int i=0;i<w;i++){
        if(i>1 && i<w-2){ fb_blend(x+i,y+j,col,alpha); continue; }
        float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
        float c=clampf(0.5f-d,0,1);
        if(c>0.002f) fb_blend(x+i,y+j,col,(int)(c*alpha));
      }
    } else {
      for(int i=0;i<w;i++){
        float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
        float c=clampf(0.5f-d,0,1);
        if(c>0.002f) fb_blend(x+i,y+j,col,(int)(c*alpha));
      }
    }
  }
}
static void fb_rrect(int x,int y,int w,int h,float r,uint32_t c,int a){
  fb_rrectg(x,y,w,h,r,c,c,a);
}
static void fb_rframe(int x,int y,int w,int h,float r,float t,uint32_t col,int alpha){
  if(w<=0||h<=0) return;
  float cx=x+w*0.5f, cy=y+h*0.5f, hw=w*0.5f, hh=h*0.5f;
  int endband=(int)(r+t+2.0f);
  int side   =(int)(t+2.5f);
  if(endband*2 > h) endband = h/2;
  if(side*2 > w)    side = w/2;
  for(int j=0;j<h;j++){
    int mid = (j>=endband && j<h-endband);
    for(int i=0;i<w;i++){
      if(mid && i>=side && i<w-side){ i = w-side-1; continue; }
      float d=rr_sdf(x+i+0.5f,y+j+0.5f,cx,cy,hw,hh,r);
      float c=clampf(0.5f-fabsf(d+t*0.5f)+t*0.5f,0,1);
      if(c>0.002f) fb_blend(x+i,y+j,col,(int)(c*alpha));
    }
  }
}

/* thick line, used for payline paths */
static void fb_line(float x0,float y0,float x1,float y1,int t,uint32_t c,int a){
  float dx=x1-x0, dy=y1-y0;
  int n=(int)(sqrtf(dx*dx+dy*dy))+1;
  for(int i=0;i<=n;i++){
    float u=(float)i/n, px=x0+dx*u, py=y0+dy*u;
    for(int j=-t/2;j<=t/2;j++) for(int k=-t/2;k<=t/2;k++)
      if(j*j+k*k <= (t/2)*(t/2)+1) fb_blend((int)px+k,(int)py+j,c,a);
  }
}

/* alpha blit with vertical clipping (reel window) and optional tint/fade.
   Walks only each row's occupied span, and takes a straight-store path for
   fully opaque pixels, which is most of a symbol's interior.            */
static void blit(const spr_t*s,int dx,int dy,int cy0,int cy1,int alpha,uint32_t tint,float tintAmt){
  if(!s->px) return;
  int y0=0, y1=s->h;
  if(dy+y0 < cy0) y0 = cy0-dy;
  if(dy+y1 > cy1) y1 = cy1-dy;
  if(y0<0) y0=0;
  if(y1>s->h) y1=s->h;
  int plain = (alpha>=255 && tintAmt<=0.0f);
  for(int y=y0;y<y1;y++){
    int fy=dy+y;
    if((unsigned)fy>=FBH) continue;
    int xa = s->rx0? s->rx0[y] : 0;
    int xb = s->rx1? s->rx1[y] : s->w-1;
    if(xb<xa) continue;
    if(dx+xa < 0)      xa = -dx;
    if(dx+xb >= FBW)   xb = FBW-1-dx;
    if(xb<xa) continue;
    const uint8_t*row = s->px + (size_t)y*s->w*4;
    uint32_t*dst = fb + (size_t)fy*FBW + dx;
    for(int x=xa;x<=xb;x++){
      int a=row[x*4+3];
      if(!a) continue;
      uint32_t c = RGB(row[x*4],row[x*4+1],row[x*4+2]);
      if(plain && a>=255){ dst[x]=c; continue; }
      if(tintAmt>0.0f) c = mixc(c,tint,tintAmt);
      int ea = a*alpha/255;
      if(ea>=255){ dst[x]=c; continue; }
      uint32_t d=dst[x];
      int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
      int sr=(c>>16)&255, sg=(c>>8)&255, sb=c&255;
      dst[x] = RGB(dr+((sr-dr)*ea>>8), dg+((sg-dg)*ea>>8), db+((sb-db)*ea>>8));
    }
  }
}

/*  Blend a single colour through a sprite's alpha.  Used for the cell
 *  washes, which used to evaluate a square root per pixel per symbol per
 *  frame — with expanding wilds on screen that was twelve of them.     */
static void blit_wash(const spr_t*s,int dx,int dy,int cy0,int cy1,
                      uint32_t col,float amt){
  if(!s->px||amt<=0.0f) return;
  int y0=0,y1=s->h;
  if(dy+y0<cy0) y0=cy0-dy;
  if(dy+y1>cy1) y1=cy1-dy;
  if(y0<0) y0=0;
  if(y1>s->h) y1=s->h;
  int sr=(col>>16)&255, sg=(col>>8)&255, sb=col&255;
  int k=(int)(amt*256.0f);
  for(int y=y0;y<y1;y++){
    int fy=dy+y;
    if((unsigned)fy>=FBH) continue;
    int xa = s->rx0? s->rx0[y] : 0;
    int xb = s->rx1? s->rx1[y] : s->w-1;
    if(xb<xa) continue;
    if(dx+xa<0)    xa=-dx;
    if(dx+xb>=FBW) xb=FBW-1-dx;
    if(xb<xa) continue;
    const uint8_t*row=s->px+(size_t)y*s->w*4;
    uint32_t*dst=fb+(size_t)fy*FBW+dx;
    for(int x=xa;x<=xb;x++){
      int a=(row[x*4+3]*k)>>8;
      if(a<=0) continue;
      uint32_t d=dst[x];
      int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
      dst[x]=RGB(dr+((sr-dr)*a>>8), dg+((sg-dg)*a>>8), db+((sb-db)*a>>8));
    }
  }
}


/* ── text (5x7 cell font, integer scale, with drop shadow) ──────── */
static void text(const char*s,int x,int y,int px,uint32_t col,int align,int shadow){
  int n=(int)strlen(s);
  while(px>1 && n*px*6-px > FBW-10) px--;      /* auto-fit */
  int adv=px*6, wtot=n*adv-px;
  int ox = align==1 ? x-wtot/2 : (align==2 ? x-wtot : x);
  for(int pass=(shadow?0:1); pass<2; pass++){
    uint32_t c = pass==0 ? 0x000000 : col;
    int off = pass==0 ? px : 0;
    for(int i=0;i<n;i++){
      int ch=(unsigned char)s[i];
      if(ch>='a'&&ch<='z') ch-=32;      /* font is upper case only */
      if(ch<32||ch>127) ch='?';
      const uint8_t*gl = FONT[ch-32];
      for(int r=0;r<7;r++){
        uint8_t bits=gl[r];
        for(int cbit=0;cbit<5;cbit++){
          if(!(bits & (0x10>>cbit))) continue;
          int gx = ox+i*adv+cbit*px+off, gy = y+r*px+off;
          for(int j=0;j<px;j++) for(int k=0;k<px;k++) fb_px(gx+k,gy+j,c);
        }
      }
    }
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
static void cv_polyN(const pt_t*p,int n,const uint32_t*st,int ns,int alpha){
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
static void cv_sphere(float cx,float cy,float r,uint32_t lo,uint32_t mid,uint32_t hi){
  int x0=(int)(cx-r-1), x1=(int)(cx+r+1);
  int y0=(int)(cy-r-1), y1=(int)(cy+r+1);
  if(x0<0)x0=0; if(y0<0)y0=0;
  if(x1>CANW)x1=CANW; if(y1>CANH)y1=CANH;
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
static void cv_ellipse_lit(float cx,float cy,float rx,float ry,
                           uint32_t lo,uint32_t mid,uint32_t hi){
  int x0=(int)(cx-rx-1), x1=(int)(cx+rx+1);
  int y0=(int)(cy-ry-1), y1=(int)(cy+ry+1);
  if(x0<0)x0=0; if(y0<0)y0=0;
  if(x1>CANW)x1=CANW; if(y1>CANH)y1=CANH;
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
 *  cached and only the three composite passes run per frame.          */
#define TBC 8
typedef struct {
  char     str[40];
  int      px, bw, bh, pad, capH, used;
  uint8_t *fill, *out;
} tbcache_t;
static tbcache_t tbc[TBC];
static int tbc_clock;

static tbcache_t* tb_render(const char*str,int px,int n,int adv,int wtot,int capH){
  for(int i=0;i<TBC;i++)
    if(tbc[i].used && tbc[i].px==px && !strcmp(tbc[i].str,str)){
      tbc[i].used=++tbc_clock;
      return &tbc[i];
    }
  /* evict the least recently used slot */
  int slot=0;
  for(int i=1;i<TBC;i++) if(tbc[i].used < tbc[slot].used) slot=i;
  tbcache_t*c=&tbc[slot];

  float Rin  = px*0.60f;
  float Rout = Rin + (px*0.30f > 1.5f ? px*0.30f : 1.5f);
  int pad=(int)(Rout+3.0f);
  int bw=wtot+pad*2, bh=capH+pad*2;
  if(bw>TBW) bw=TBW;
  if(bh>TBH) bh=TBH;

  uint8_t*f=(uint8_t*)realloc(c->fill,(size_t)bw*bh);
  uint8_t*o=(uint8_t*)realloc(c->out ,(size_t)bw*bh);
  if(!f||!o){ free(f); free(o); c->fill=c->out=NULL; c->used=0; return NULL; }
  c->fill=f; c->out=o;
  memset(c->fill,0,(size_t)bw*bh);
  memset(c->out ,0,(size_t)bw*bh);

  for(int i=0;i<n;i++){
    int ch=(unsigned char)str[i];
    if(ch>='a'&&ch<='z') ch-=32;
    if(ch<32||ch>127) ch='?';
    const uint8_t*gl=FONT[ch-32];
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

  tbcache_t*c=tb_render(str,px,n,adv,wtot,capH);
  if(!c) return;
  int bw=c->bw, bh=c->bh, pad=c->pad;
  int bx=ox-pad, by=y-pad;
  int so=(int)(px*0.34f); if(so<2) so=2;

  for(int j=0;j<bh;j++) for(int i=0;i<bw;i++){
    int a2=c->out[j*bw+i];
    if(a2) fb_blend(bx+i+so,by+j+so,0x000000,a2*160/255);
  }
  for(int j=0;j<bh;j++) for(int i=0;i<bw;i++){
    int a2=c->out[j*bw+i];
    if(a2) fb_blend(bx+i,by+j,0x180C02,a2);
  }
  for(int j=0;j<bh;j++){
    float t=clampf((float)(j-pad)/(float)(capH>1?capH-1:1),0,1);
    uint32_t col=ramp(st,ns,t);
    if(t<0.34f) col=mixc(col,0xFFFFFF,(0.34f-t)/0.34f*0.55f);
    for(int i=0;i<bw;i++){
      int a2=c->fill[j*bw+i];
      if(a2) fb_blend(bx+i,by+j,col,a2);
    }
  }
}

/* ═══ SYMBOL ART ═══════════════════════════════════════════════════
 *  All coordinates are in 0..92 design units; U() lifts them onto the
 *  supersampled canvas.  Every symbol is built the same way:
 *      dark outline  ->  bright rim  ->  gradient body  ->  highlight
 *  which is what keeps them readable against the dark reel window.
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

/* ── the medallion ─────────────────────────────────────────────────
 *  Every symbol sits on a chrome-ringed, domed disc, the way the premium
 *  symbols on a modern cabinet do.  It is what turns a flat fruit into
 *  a game piece: the ring gives a hard, bright edge against the cream
 *  drum, the dome gives it depth, and the gloss arc sells the plastic.
 * ---------------------------------------------------------------- */
static void cv_medal(uint32_t lo,uint32_t mid,uint32_t hi){
  const float cx=U(46), cy=U(46);
  cv_circle(cx+U(1.2f),cy+U(2.2f),U(45),0x000000,0x000000,120);   /* drop shadow */
  static const uint32_t chrome[7]={0xFFFFFF,0xE4E9F4,0x7E88A2,0xF6F9FF,0xB6BDCC,0x59627A,0x2C3242};
  pt_t p[48];
  for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=cx+cosf(a)*U(44.5f); p[i].y=cy+sinf(a)*U(44.5f); }
  cv_polyN(p,48,chrome,7,255);                                    /* chrome ring */
  cv_circle(cx,cy,U(40.8f),0x1C2030,0x05060A,255);                /* bezel step  */
  cv_ellipse_lit(cx,cy,U(38.6f),U(38.6f),lo,mid,hi);              /* domed disc  */
  cv_ellipse(cx,cy-U(15),U(27),U(12),0xFFFFFF,0xFFFFFF,55);       /* gloss arc   */
}

/* the 7 glyph, authored on 8..84 x 8..89, placed scaled about (ox,oy) */
static void seven_shape(float ox,float oy,float s,const uint32_t*body,int nb,int rim){
  pt_t p[7]={{8,8},{84,8},{84,26},{59,89},{29,89},{61,27},{8,27}};
  for(int i=0;i<7;i++){ p[i].x=U(ox+(p[i].x-46)*s); p[i].y=U(oy+(p[i].y-48)*s); }
  if(rim){
    cv_poly_outline(p,7,U(5.6f*s),0x1A0004,255);          /* contact shadow  */
    cv_poly_outline(p,7,U(3.9f*s),0x6A4A08,255);          /* dark gold under */
    cv_poly_outline(p,7,U(2.3f*s),0xFFE9A8,255);          /* bright gold rim */
  }
  cv_polyN(p,7,body,nb,255);
  pt_t hl[4]={{13,11},{79,11},{77,17},{13,17}};
  for(int i=0;i<4;i++){ hl[i].x=U(ox+(hl[i].x-46)*s); hl[i].y=U(oy+(hl[i].y-48)*s); }
  cv_poly(hl,4,0xFFFFFF,0xFFC0B0,150);                     /* top specular    */
  pt_t sh[4]={{61,27},{67,27},{40,89},{33,89}};
  for(int i=0;i<4;i++){ sh[i].x=U(ox+(sh[i].x-46)*s); sh[i].y=U(oy+(sh[i].y-48)*s); }
  cv_poly(sh,4,0x000000,0x000000,60);                      /* inner shade     */
}

/* a licking flame: a few wavy tongues, yellow at the tip, red at the root */
static void flames(float cx,float cy,float s){
  static const uint32_t fire[4]={0xFFFAC0,0xFFC81E,0xFF5A0C,0x9A0E00};
  static const float fl[6][3]={{-24,4,30},{-13,0,40},{-1,-3,34},{11,-2,42},{22,2,28},{-19,14,20}};
  for(int k=0;k<6;k++){
    float x=cx+fl[k][0]*s, y=cy+fl[k][1]*s, h=fl[k][2]*s, w=7.5f*s;
    pt_t t[8]={{x-w,y},{x-w*0.9f,y-h*0.35f},{x-w*0.25f,y-h*0.55f},{x+w*0.15f,y-h},
               {x+w*0.35f,y-h*0.5f},{x+w*0.95f,y-h*0.3f},{x+w,y},{x,y+w*0.4f}};
    for(int i=0;i<8;i++){ t[i].x=U(t[i].x); t[i].y=U(t[i].y); }
    cv_poly_outline(t,8,U(1.4f),0x5A0800,150);
    cv_polyN(t,8,fire,4,240);
  }
}

/* a small rounded ribbon with a label, for the feature symbols */
static void ribbon(float y,float w,const char*label,uint32_t top,uint32_t bot,uint32_t ink){
  float x=46-w*0.5f;
  cv_rrect(U(x-1),U(y-1),U(w+2),U(12),U(3.5f),0x000000,0x000000,170);
  cv_rrect(U(x),U(y),U(w),U(10),U(3),top,bot,255);
  cv_rrect(U(x+1.5f),U(y+0.8f),U(w-3),U(3.6f),U(2),0xFFFFFF,0xFFFFFF,80);
  cv_text(label,U(46),U(y+5),U(1.55f),ink,255);
}

/* a small leaf, lit */
static void leaf(float cx,float cy,float rx,float ry){
  cv_ellipse_lit(cx,cy,rx,ry,0x0E4A12,0x3FA640,0xC4F5A8);
  cv_ellipse(cx,cy,rx*0.9f,ry*0.18f,0x0E4A12,0x0E4A12,120);   /* midrib */
}

/* ---- individual pieces ---- */

static void art_seven(void){
  static const uint32_t body[5]={0xFFC4A8,0xFF6A4A,0xE81C2C,0x9A0C1A,0x54060E};
  cv_medal(0x062A0A,0x1F8A30,0x9AF080);
  flames(47,30,0.52f);                       /* tips lick up behind the top bar */
  seven_shape(46,49,0.64f,body,5,1);
  ribbon(79,34,"WILD",0xFFE9A8,0xC08A10,0x3A1400);
}

static void art_diamond(void){
  static const uint32_t topf[3]={0xFFFFFF,0xD6F6FF,0x74D2F4};
  static const uint32_t lft[3] ={0xBEEFFF,0x53BCE6,0x0B5F8C};
  static const uint32_t rgt[3] ={0x8FDEF8,0x2E9BC8,0x063F60};
  cv_medal(0x04123A,0x123E92,0x74B0FF);
  const float s=0.74f, ox=46, oy=49;
  #define DP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-52)*s)}
  pt_t all[5]={DP(24,16),DP(68,16),DP(84,38),DP(46,88),DP(8,38)};
  cv_poly_outline(all,5,U(5.0f*s),0x000B12,255);
  cv_poly_outline(all,5,U(2.4f*s),0xF2FEFF,255);
  pt_t L[3]={DP(8,38),DP(46,38),DP(46,88)};
  pt_t R[3]={DP(46,38),DP(84,38),DP(46,88)};
  cv_polyN(L,3,lft,3,255);
  cv_polyN(R,3,rgt,3,255);
  pt_t T[4]={DP(24,16),DP(68,16),DP(84,38),DP(8,38)};
  cv_polyN(T,4,topf,3,255);
  pt_t s1[4]={DP(24,16),DP(28,16),DP(18,38),DP(13,38)};
  pt_t s2[4]={DP(64,16),DP(68,16),DP(79,38),DP(74,38)};
  cv_poly(s1,4,0xFFFFFF,0xFFFFFF,120); cv_poly(s2,4,0xFFFFFF,0xFFFFFF,120);
  pt_t s3[4]={DP(36,16),DP(40,16),DP(46,38),DP(43,38)};
  cv_poly(s3,4,0xFFFFFF,0xFFFFFF,80);
  pt_t sp[8]={DP(33,24),DP(36,30),DP(42,32),DP(36,34),DP(33,41),DP(30,34),DP(24,32),DP(30,30)};
  cv_poly(sp,8,0xFFFFFF,0xFFFFFF,225);
  #undef DP
}

static void art_bell(void){
  static const uint32_t gold[5]={0xFFFBE0,0xFFE07A,0xE0A81C,0x9A6A08,0x5E4004};
  cv_medal(0x3A0408,0xA81222,0xFF8A78);
  const float s=0.72f, ox=46, oy=47;
  #define BP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-48)*s)}
  pt_t p[12]={BP(46,9),BP(60,15),BP(69,30),BP(74,52),BP(80,66),BP(84,74),
              BP(8,74),BP(12,66),BP(18,52),BP(23,30),BP(32,15),BP(46,9)};
  cv_poly_outline(p,12,U(5.0f*s),0x140C00,255);
  cv_poly_outline(p,12,U(2.4f*s),0xFFF6C8,255);
  cv_polyN(p,12,gold,5,255);
  pt_t hl[6]={BP(37,17),BP(44,14),BP(39,29),BP(33,52),BP(29,67),BP(24,67)};
  cv_poly(hl,6,0xFFFFFF,0xFFF2C0,135);
  pt_t dk[4]={BP(64,29),BP(74,53),BP(79,67),BP(67,67)};
  cv_poly(dk,4,0x3A2600,0x3A2600,90);
  cv_rrect(U(ox-38*s),U(oy+19*s),U(76*s),U(9*s),U(3*s),0xFFF6C8,0xB07C10,255);  /* lip */
  cv_sphere(U(ox),U(oy+36*s),U(9*s),0x4A3200,0xD8A020,0xFFF0B0);             /* clapper */
  #undef BP
}

static void art_bar(void){
  static const uint32_t gold[6]={0xFFFDF0,0xFFE8A0,0xF2B622,0xC88A10,0xFFD860,0x8A5A08};
  cv_medal(0x0C0E16,0x2A2E3E,0x7A8098);
  /* the plaque: dark bevel, gold face, black lettering, like the reference */
  cv_rrect(U(9),U(31),U(74),U(34),U(6),0x000000,0x000000,180);
  cv_rrect(U(8),U(29),U(76),U(34),U(6),0x4A2E04,0x2A1A02,255);
  pt_t q[52]; int n=0; const int Q=10;
  float xx=U(10.5f), yy=U(31.5f), ww=U(71), hh=U(29), rr=U(4.5f);
  for(int i=0;i<=Q;i++){ float a=-TAU/4+TAU/4*i/Q; q[n].x=xx+ww-rr+cosf(a)*rr; q[n].y=yy+rr+sinf(a)*rr; n++; }
  for(int i=0;i<=Q;i++){ float a=TAU/4*i/Q;        q[n].x=xx+ww-rr+cosf(a)*rr; q[n].y=yy+hh-rr+sinf(a)*rr; n++; }
  for(int i=0;i<=Q;i++){ float a=TAU/4+TAU/4*i/Q;  q[n].x=xx+rr+cosf(a)*rr;    q[n].y=yy+hh-rr+sinf(a)*rr; n++; }
  for(int i=0;i<=Q;i++){ float a=TAU/2+TAU/4*i/Q;  q[n].x=xx+rr+cosf(a)*rr;    q[n].y=yy+rr+sinf(a)*rr; n++; }
  cv_polyN(q,n,gold,6,255);
  cv_rrect(U(12),U(32.5f),U(68),U(10),U(3),0xFFFFFF,0xFFFFFF,110);   /* gloss */
  cv_text("BAR",U(46.6f),U(46.6f),U(3.4f),0x2A1A02,140);            /* engrave */
  cv_text("BAR",U(46),U(46),U(3.4f),0x120A00,255);
}

static void art_grapes(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  /* stem and leaf first, the bunch hangs over them */
  for(int i=0;i<=20;i++){ float t=i/20.0f; cv_circle(U(48+t*6),U(20-t*10),U(2.2f),0x5A3A10,0x3A2408,255); }
  leaf(U(60),U(18),U(13),U(6.5f));
  static const float gp[10][2]={{31,32},{46,30},{61,32},{38,44},{54,44},{30,54},{46,56},{62,54},{38,67},{54,67}};
  for(int i=0;i<10;i++){
    cv_circle(U(gp[i][0])+U(0.8f),U(gp[i][1])+U(1.2f),U(9),0x000000,0x000000,110);
    cv_sphere(U(gp[i][0]),U(gp[i][1]),U(8.8f),0x22063A,0x7A28C0,0xE8B0FF);
  }
  cv_sphere(U(46),U(78),U(8.2f),0x22063A,0x7A28C0,0xE8B0FF);
}

static void art_orange(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  cv_sphere(U(46),U(50),U(30),0x6A2400,0xFF7C12,0xFFE0A8);
  /* peel dimples, kept off the hot spot so the sphere still reads */
  uint32_t rs=0x0A4A9Eu;
  for(int i=0;i<48;i++){
    rs^=rs<<13; rs^=rs>>17; rs^=rs<<5;
    float a=(rs&0xFFFF)/65536.0f*TAU, rr=sqrtf(((rs>>16)&0xFFFF)/65536.0f)*U(26);
    float x=U(46)+cosf(a)*rr, y=U(50)+sinf(a)*rr;
    cv_circle(x,y,U(0.9f),0xB04A00,0xB04A00,70);
  }
  cv_circle(U(46),U(21),U(3.2f),0x6A3A08,0x3A2004,255);                  /* nub  */
  leaf(U(58),U(20),U(12),U(5.5f));
}

static void art_plum(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  cv_ellipse_lit(U(46),U(52),U(27),U(29),0x16032A,0x6A1C9A,0xE6A8FF);
  for(int i=0;i<=30;i++){                                                 /* seam */
    float t=i/30.0f, y=U(24+t*56), x=U(46)-sinf(t*3.14159f)*U(9);
    cv_circle(x,y,U(1.3f),0x2A0640,0x2A0640,120);
  }
  cv_ellipse(U(36),U(38),U(8),U(11),0xFFFFFF,0xFFFFFF,90);              /* bloom */
  for(int i=0;i<=14;i++){ float t=i/14.0f; cv_circle(U(46+t*3),U(24-t*9),U(1.8f),0x5A3A10,0x3A2408,255); }
  leaf(U(57),U(17),U(11),U(5));
}

static void art_cherry(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  for(int i=0;i<=40;i++){
    float t=i/40.0f;
    float x=lerpf(U(33),U(50),t), y=lerpf(U(52),U(18),t)-sinf(t*3.14159f)*U(6);
    cv_circle(x,y,U(2.3f),0x1B5E20,0x0E3D12,255);
    cv_circle(x-U(0.6f),y-U(0.6f),U(0.9f),0x7BD86A,0x4CAF50,190);
    float x2=lerpf(U(60),U(53),t), y2=lerpf(U(58),U(19),t)+sinf(t*3.14159f)*U(4);
    cv_circle(x2,y2,U(2.1f),0x1B5E20,0x0E3D12,255);
    cv_circle(x2-U(0.6f),y2-U(0.6f),U(0.9f),0x7BD86A,0x4CAF50,190);
  }
  leaf(U(64),U(18),U(13),U(6));
  cv_circle(U(34),U(58),U(19),0x000000,0x000000,90);
  cv_sphere(U(33),U(56),U(18.5f),0x3A0008,0xD81030,0xFFA0A8);
  cv_circle(U(61),U(64),U(18),0x000000,0x000000,90);
  cv_sphere(U(60),U(62),U(17.5f),0x3A0008,0xD81030,0xFFA0A8);
}

static void art_lemon(void){
  cv_medal(0x0A1030,0x1C2E70,0x6A86D8);
  cv_ellipse_lit(U(46),U(51),U(32),U(23),0x5A4000,0xF0C010,0xFFFAC8);
  cv_ellipse_lit(U(14),U(51),U(5),U(4),0x5A4000,0xE8B808,0xFFF6B0);      /* nubs */
  cv_ellipse_lit(U(78),U(51),U(5),U(4),0x5A4000,0xE8B808,0xFFF6B0);
  uint32_t rs=0x1E30Cu;
  for(int i=0;i<30;i++){
    rs^=rs<<13; rs^=rs>>17; rs^=rs<<5;
    float a=(rs&0xFFFF)/65536.0f*TAU, rr=sqrtf(((rs>>16)&0xFFFF)/65536.0f)*U(27);
    cv_circle(U(46)+cosf(a)*rr, U(51)+sinf(a)*rr*0.68f, U(0.9f), 0xB88A00,0xB88A00,90);
  }
  leaf(U(64),U(30),U(11),U(5));
}

static void art_star(void){
  static const uint32_t gold[5]={0xFFFFFF,0xFFF3A8,0xFFC02A,0xE07800,0x8A3C00};
  cv_medal(0x03301A,0x0E8A48,0x80FFB8);
  for(int i=0;i<16;i++){                       /* radiating burst */
    float a=TAU*i/16.0f + 0.19f;
    pt_t ray[3]={{U(46)+cosf(a)*U(14),U(45)+sinf(a)*U(14)},
                 {U(46)+cosf(a+0.12f)*U(38),U(45)+sinf(a+0.12f)*U(38)},
                 {U(46)+cosf(a-0.12f)*U(38),U(45)+sinf(a-0.12f)*U(38)}};
    cv_poly(ray,3,0xFFF6B0,0x0E8A48,95);
  }
  pt_t p[10];
  for(int i=0;i<10;i++){
    float a=-TAU/4+TAU*i/10.0f;
    float r=(i&1)?U(13.5f):U(32);
    p[i].x=U(46)+cosf(a)*r; p[i].y=U(44)+sinf(a)*r;
  }
  cv_poly_outline(p,10,U(4.5f),0x160A00,255);
  cv_poly_outline(p,10,U(2.0f),0xFFFFFF,255);
  cv_polyN(p,10,gold,5,255);
  cv_sphere(U(46),U(43),U(9.5f),0xB86A00,0xFFD24A,0xFFFFFF);
  ribbon(74,54,"SCATTER",0xFFFFFF,0xB8E8C8,0x083A18);
}

static void art_crown(void){
  static const uint32_t gold[5]={0xFFFBE0,0xFFDE78,0xD8A01C,0x8E6208,0x503802};
  cv_medal(0x24044A,0x6A18B0,0xD898FF);
  for(int i=0;i<14;i++){
    float a=TAU*i/14.0f;
    pt_t ray[3]={{U(46)+cosf(a)*U(16),U(44)+sinf(a)*U(16)},
                 {U(46)+cosf(a+0.14f)*U(37),U(44)+sinf(a+0.14f)*U(37)},
                 {U(46)+cosf(a-0.14f)*U(37),U(44)+sinf(a-0.14f)*U(37)}};
    cv_poly(ray,3,0xF0C0FF,0x3A0060,70);
  }
  const float s=0.70f, ox=46, oy=44;
  #define CP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-48)*s)}
  pt_t p[7]={CP(13,76),CP(79,76),CP(79,26),CP(63,47),CP(46,19),CP(29,47),CP(13,26)};
  cv_poly_outline(p,7,U(5.0f*s),0x140C00,255);
  cv_poly_outline(p,7,U(2.4f*s),0xFFF6C8,255);
  cv_polyN(p,7,gold,5,255);
  pt_t band[4]={CP(13,58),CP(79,58),CP(79,66),CP(13,66)};
  static const uint32_t bg2[3]={0xFFFBE0,0xE8B830,0x8E6208};
  cv_polyN(band,4,bg2,3,255);
  cv_sphere(U(ox-17*s),U(oy+23*s),U(5.5f*s),0x4A0010,0xE02040,0xFFB0B8);
  cv_sphere(U(ox),      U(oy+23*s),U(5.5f*s),0x003A14,0x18B048,0xB0FFC8);
  cv_sphere(U(ox+17*s),U(oy+23*s),U(5.5f*s),0x001A4A,0x2060D8,0xB0D0FF);
  cv_sphere(U(ox-33*s),U(oy-24*s),U(5*s),0x8E6208,0xE8C040,0xFFF8D0);
  cv_sphere(U(ox),      U(oy-31*s),U(5*s),0x8E6208,0xE8C040,0xFFF8D0);
  cv_sphere(U(ox+33*s),U(oy-24*s),U(5*s),0x8E6208,0xE8C040,0xFFF8D0);
  #undef CP
  ribbon(75,44,"BONUS",0xF4D8FF,0x9A50D8,0x2A0448);
}

static void art_jackpot(void){
  cv_medal(0x3A0000,0xB01414,0xFF7A60);
  for(int i=0;i<12;i++){                       /* gold burst behind the coin */
    float a=TAU*i/12.0f+0.26f;
    pt_t ray[3]={{U(46)+cosf(a)*U(18),U(42)+sinf(a)*U(18)},
                 {U(46)+cosf(a+0.11f)*U(38),U(42)+sinf(a+0.11f)*U(38)},
                 {U(46)+cosf(a-0.11f)*U(38),U(42)+sinf(a-0.11f)*U(38)}};
    cv_poly(ray,3,0xFFF0A0,0xB01414,90);
  }
  cv_circle(U(47),U(44),U(25),0x000000,0x000000,120);
  cv_sphere(U(46),U(42),U(24.5f),0x7A4A00,0xF2B622,0xFFF6C0);            /* the coin */
  cv_circle(U(46),U(42),U(20),0x8A5A08,0x8A5A08,110);                     /* milled rim */
  cv_circle(U(46),U(42),U(18.5f),0xFFD860,0xE0A020,255);
  cv_text("$",U(46.8f),U(42.8f),U(4.6f),0x5A3400,255);
  cv_text("$",U(46),U(42),U(4.6f),0xFFF8D0,255);
  ribbon(75,58,"JACKPOT",0xFFE9A8,0xC08A10,0x3A1400);
}

static void art_ult(void){
  static const uint32_t body[5]={0xFFC4A8,0xFF6A4A,0xE81C2C,0x9A0C1A,0x54060E};
  cv_medal(0x1C1C2E,0x8A8CA8,0xFFFFFF);
  /* rainbow sheen: twelve translucent wedges around the disc */
  static const uint32_t hue[12]={0xFF3030,0xFF8A20,0xFFE020,0x80FF30,0x20FF80,0x20FFE0,
                                 0x2090FF,0x4040FF,0x9030FF,0xE030FF,0xFF30A0,0xFF3060};
  for(int i=0;i<12;i++){
    float a0=TAU*i/12.0f, a1=TAU*(i+1)/12.0f;
    pt_t w[4]={{U(46),U(46)},{U(46)+cosf(a0)*U(38),U(46)+sinf(a0)*U(38)},
               {U(46)+cosf((a0+a1)*0.5f)*U(38.6f),U(46)+sinf((a0+a1)*0.5f)*U(38.6f)},
               {U(46)+cosf(a1)*U(38),U(46)+sinf(a1)*U(38)}};
    cv_poly(w,4,hue[i],hue[i],70);
  }
  cv_ellipse(U(46),U(31),U(27),U(12),0xFFFFFF,0xFFFFFF,70);
  (void)body;
  /* one platinum seven, so it cannot be mistaken for the red wild */
  static const uint32_t plat[5]={0xFFFFFF,0xEAF6FF,0x9CCCF0,0x3C7CB0,0x123A5C};
  seven_shape(46,52,0.60f,plat,5,1);
  /* a small gold crown perched on top */
  { static const uint32_t gold[5]={0xFFFBE0,0xFFDE78,0xD8A01C,0x8E6208,0x503802};
    const float s=0.30f, ox=46, oy=19;
    #define UP(px,py) {U(ox+((px)-46)*s),U(oy+((py)-48)*s)}
    pt_t p[7]={UP(13,76),UP(79,76),UP(79,26),UP(63,47),UP(46,19),UP(29,47),UP(13,26)};
    cv_poly_outline(p,7,U(4.0f*s),0x140C00,255);
    cv_poly_outline(p,7,U(2.0f*s),0xFFF6C8,255);
    cv_polyN(p,7,gold,5,255);
    cv_sphere(U(ox-33*s),U(oy-24*s),U(5*s),0x8E6208,0xE8C040,0xFFF8D0);
    cv_sphere(U(ox),      U(oy-31*s),U(5*s),0x8E6208,0xE8C040,0xFFF8D0);
    cv_sphere(U(ox+33*s),U(oy-24*s),U(5*s),0x8E6208,0xE8C040,0xFFF8D0);
    #undef UP
  }
  ribbon(79,60,"ULTIMATE",0xFFFFFF,0xB8C8FF,0x101838);
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


/* build the crisp sprites, their motion-blurred twins, and the glow */
static void art_coin(void);     /* w7_hold.c  */
static void art_wheel(void);    /* w7_wheel.c */
static void build_sprites(void){
  void(*art[NSYM])(void) = { art_seven,art_diamond,art_bell,art_bar,art_grapes,
                             art_orange,art_plum,art_cherry,art_lemon,
                             art_star,art_crown,art_jackpot,art_ult,
                             art_coin,art_wheel };
  for(int i=0;i<NSYM;i++){
    cv_clear(); art[i](); cv_resolve(&sym[i]);
    add_contour(&sym[i], SYMW/30, 0x0A0510, 255);   /* hard dark edge */
    spr_bounds(&sym[i]);
  }
  /* vertical box blur -> what you see while the reel is at speed.  Built
     from the un-shadowed art, so before the shadow is baked in.       */
  for(int i=0;i<NSYM;i++){
    symb[i].w=SYMW; symb[i].h=SYMH;
    symb[i].px=(uint8_t*)calloc(SYMW*SYMH,4);
    if(!symb[i].px) continue;
    const int K=7;
    for(int x=0;x<SYMW;x++) for(int y=0;y<SYMH;y++){
      int r=0,g=0,b=0,a=0,n=0;
      for(int k=-K;k<=K;k++){
        int yy=y+k; if(yy<0||yy>=SYMH) continue;
        const uint8_t*p=sym[i].px+(yy*SYMW+x)*4;
        r+=p[0]*p[3]; g+=p[1]*p[3]; b+=p[2]*p[3]; a+=p[3]; n++;
      }
      uint8_t*o=symb[i].px+(y*SYMW+x)*4;
      if(a>0){ o[0]=(uint8_t)(r/a); o[1]=(uint8_t)(g/a); o[2]=(uint8_t)(b/a); }
      o[3]=(uint8_t)(n? (a/n)*4/5 : 0);
    }
    spr_bounds(&symb[i]);
  }
  /* cast shadows are composited straight into the symbol sprites: same
     look, half the per-frame blits */
  for(int i=0;i<NSYM;i++) bake_shadow(&sym[i], SYMW/25, SYMH/20, 4, 200);

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
 *  Values are per 5 credits of total bet, so a win is PAY * ways * bet / 5.
 *  Every bet on the ladder is a multiple of 5, so that is always exact.
 *  Tuned against the Monte-Carlo in src/sim.c, which runs this very
 *  evaluator over millions of spins.
 * ================================================================= */
#define MINCHAIN 3
static const char*BANDNAME[3]={"3 REELS","4 REELS","5 REELS"};

static const int PAY[NSYM][6] = {
/*                 0  1  2    3    4    5   reels */
/* SEVEN   */    { 0, 0, 0,   4,  15, 148 },
/* DIAMOND */    { 0, 0, 0,   3,   9,  48 },
/* BELL    */    { 0, 0, 0,   2,   6,  28 },
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
 *  reels 2, 3 and 4 only, one showing per reel at most.  These counts are
 *  PROVISIONAL - taken out of the low fruit - until the maths pass.     */
/*        7   D   B  BAR  GR  OR  PL  CH  LE  ST  CR  JP  UL  CO  WH  */
  {       2,  8,  9, 10, 11, 11, 12, 11,  9,  2,  3,  0,  2,  6,  0 },  /* reels 1,5 */
  {       2,  8,  9, 10, 11, 11, 11, 11, 10,  1,  0,  3,  1,  6,  2 },  /* reels 2,4 */
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
  int   ptPage;               /* 0 pay table, 1 features               */

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
 *  Cosmetic only, so deliberately NOT part of game_t / the save state.
 * ---------------------------------------------------------------- */
#define NPART 64
typedef struct { float x,y,vx,vy,life; uint32_t col; } part_t;
static part_t parts[NPART];

static void spawn_burst(float x,float y,int n,uint32_t col){
  for(int i=0;i<NPART && n>0;i++){
    if(parts[i].life>0) continue;
    float a=frnd()*TAU, sp=40.0f+frnd()*150.0f;
    parts[i].x=x; parts[i].y=y;
    parts[i].vx=cosf(a)*sp; parts[i].vy=sinf(a)*sp-70.0f;
    parts[i].life=0.5f+frnd()*0.7f;
    parts[i].col=col;
    n--;
  }
}
static void update_parts(void){
  for(int i=0;i<NPART;i++){
    if(parts[i].life<=0) continue;
    parts[i].life-=DT;
    parts[i].vy += 320.0f*DT;
    parts[i].x  += parts[i].vx*DT;
    parts[i].y  += parts[i].vy*DT;
  }
}
static void draw_parts(void){
  for(int i=0;i<NPART;i++){
    float L=parts[i].life;
    if(L<=0) continue;
    int a=(int)(clampf(L*1.6f,0,1)*255);
    int px=(int)parts[i].x, py=(int)parts[i].y;
    int r=(L>0.5f)?2:1;
    for(int j=-r;j<=r;j++) for(int k=-r;k<=r;k++)
      fb_blend(px+k,py+j,parts[i].col,a);
    fb_add(px,py,60,50,10);
  }
}

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
  if(G.inFree && G.fsMult>1) G.winTotal *= G.fsMult;

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
  G.credits=(int)c;
  G.lastWin=(int)(amt>2000000000LL?2000000000LL:amt);
  if(G.inFree){
    long long f=(long long)G.fsWon+amt;
    G.fsWon=(int)(f>2000000000LL?2000000000LL:f);
  }
}

/* how many scatters, or crowns, are already showing on the reels that
   have stopped — the anticipation hold keys off the larger count */
static int partial_special(void){
  int st=0, cr=0;
  for(int r=0;r<NREEL;r++){
    if(G.rstate[r]!=3) continue;
    int base=(int)floorf(G.rpos[r]+0.5f);
    for(int row=0;row<NROW;row++){
      int s=stripAt(r,base-row);
      if(s==SY_STAR) st++;
      if(s==SY_CROWN) cr++;
    }
  }
  return st>cr?st:cr;
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
  case 3:  *symo=SY_CHERRY; rows[0]=1; rows[1]=2; rows[2]=3; return 3;         /* win   */
  case 4:  /* mega: reels 2,3,4 stacks aligned = 2+3+2 */
    if(r==1||r==3){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; return 2; }
    if(r==2){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; rows[2]=3; return 3; }
    return 0;
  case 5:  /* minor: reels 2 and 3 = 2+3 */
    if(r==1){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; return 2; }
    if(r==2){ *symo=SY_JACKPOT; rows[0]=1; rows[1]=2; rows[2]=3; return 3; }
    return 0;
  case 6:  *symo=SY_ULT; rows[0]=2; return 1;                                  /* ult   */
  }
  return 0;
}

/* ═══ UPDATE ══════════════════════════════════════════════════════ */
static inline int cellcx(int r);
static inline int cellcy(int row);
static void flash_btn(int i);

static void update(void){
  G.t += DT;
  if(G.btn) G.idle=0; else G.idle += DT;
  if(G.flash>0) G.flash -= DT*3.0f;
  if(G.flash<0) G.flash=0;
  if(G.bannerT>0) G.bannerT -= DT;
  if(G.multUp>0) G.multUp -= DT;
  update_parts();
  fx_update();

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
          if(dbg_force){
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
        for(int k=0;k<3;k++)
          spawn_burst(LRX+RAILW-67+(frnd()-0.5f)*90.0f, FEATY+40.0f, 8, 0xFFD24A);
        sfx_mult(G.fsMult);
        G.flash = opt_limiter?0.28f:0.55f;
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
    if(G.t>0.20f){
      if(G.jpWon>=0){
        G.state=ST_JACKPOT; G.t=0; G.jpT=0;
        G.flash = opt_limiter?0.35f:0.8f;
        sfx_jackpot(G.jpWon);
        break;
      }
      G.pend |= pend_from_grid();          /* idempotent: EVAL can re-enter */
      if(G.winTotal>0){
        G.state=ST_SHOWWIN; G.t=0; G.showIdx=0; G.showT=0; G.winShown=0;
        G.flash = opt_limiter?0.35f:0.7f;
        if(G.winTotal >= TOTBET*40){ G.banner=2; G.bannerT=3.4f; sfx_big_win_tier(G.winTotal>=TOTBET*150?2:1); }
        else if(G.winTotal >= TOTBET*10){ G.banner=1; G.bannerT=2.4f; }
      }
      else next_feature();
    }
    break;

  case ST_SHOWWIN: {
    int step = 1 + G.winTotal/45;
    if(G.winShown < G.winTotal){
      G.winShown += step;
      if(G.winShown > G.winTotal) G.winShown = G.winTotal;
      if(((int)(G.t*60))%3==0) sfx_win_tick((float)G.winShown/(float)G.winTotal);
    }
    G.showT += DT;
    if(G.nWin>0 && G.showT>0.85f){
      G.showT=0; G.showIdx=(G.showIdx+1)%G.nWin;
      uint32_t m=G.winMask[G.showIdx];
      for(int c=0;c<NCELL;c++) if((m>>c)&1u){
        spawn_burst((float)cellcx(c/NROW),(float)cellcy(c%NROW),8,0xFFD24A);
        break;
      }
    }
    /* X on a shown win: gamble it instead of collecting (w7_extra.c) */
    if(G.winShown>=G.winTotal && G.t>0.5f && hit(B_X) && gamble_allowed()){
      gamble_begin();
      break;
    }
    if((G.winShown>=G.winTotal && G.t>1.1f && anyhit()) || G.t>6.5f){
      award(G.winTotal);
      next_feature();
    }
    break; }

  case ST_HOLD:   hold_update();   break;
  case ST_WHEEL:  wheel_update();  break;
  case ST_GAMBLE: gamble_update(); break;

  case ST_JACKPOT:
    G.jpT += DT;
    { float run = (G.jpWon==JP_ULT)?6.0f:3.0f;
      if(((int)(G.t*60))%5==0 && G.t<run){
        spawn_burst(FBW*0.5f + (frnd()-0.5f)*420.0f, 300.0f, 6, 0xFFD24A);
        /* the coin ticks climb while the amount rolls up */
        float k=clampf(G.t/run,0,1);
        sfx_jackpot_tick(k);
      } }
    if(G.t>(G.jpWon==JP_ULT?7.0f:4.2f) || (G.t>1.4f && anyhit())){
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
      if(hit(B_SELECT)){ G.ptPage^=1; G.t=0; sfx_ui_page(); }
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
  if(horiz){
    for(int j=0;j<(int)th;j++){
      float dy = j-(th-1)*0.5f;
      float inset = fabsf(dy);
      float x0=x+inset, x1=x+len-inset;
      float sh = (ybase-(y+j))*slant;
      for(int i=(int)x0;i<(int)x1;i++) fb_blend(i+(int)sh,(int)y+j,c,a);
    }
  } else {
    for(int j=0;j<(int)len;j++){
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
  if(!m) return;
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

/*  A black inset window for a readout to sit in.                      */
static void led_window(int x,int y,int w,int h){
  fb_rrect(x,y,w,h,7,0x000000,255);
  fb_rframe(x,y,w,h,7,2.0f,0x2A2E38,255);
  for(int j=0;j<5;j++){                                   /* inner shade */
    int a=(5-j)*22;
    for(int i=0;i<w;i++) fb_blend(x+i,y+j,0x000000,a);
  }
  for(int j=0;j<h;j+=3)                                   /* faint raster */
    for(int i=0;i<w;i++) fb_blend(x+i,y+j,0x000000,40);
}

/*  Moulded keycap: light top face, shaded sides, engraved label.      */
static void keycap(int x,int y,int w,int h,const char*l1,const char*l2,
                   int pressed,uint32_t face){
  int dy = pressed?3:0;
  fb_rrect(x+2,y+6,w,h,10,0x000000,190);                  /* cast shadow */
  if(!pressed){                                            /* body side  */
    fb_rrectg(x,y+dy+4,w,h,10,scalec(face,0.42f),scalec(face,0.28f),255);
  }
  fb_rrectg(x,y+dy,w,h-(pressed?0:4),10,
            mixc(face,0xFFFFFF,0.34f),scalec(face,0.72f),255);
  for(int j=0;j<h/3;j++){                                  /* top gloss  */
    int a=(h/3-j)*3;
    for(int i=5;i<w-5;i++) fb_blend(x+i,y+dy+2+j,0xFFFFFF,a);
  }
  fb_rframe(x,y+dy,w,h-(pressed?0:4),10,1.5f,scalec(face,0.35f),200);
  int ty = y+dy+(l2? h/2-16 : h/2-11);
  text(l1,x+w/2,ty,2,0x14141C,1,0);
  if(l2) text(l2,x+w/2,ty+18,2,0x14141C,1,0);
}

/*  The big domed SPIN button.  The lighting model runs once per state
 *  at init — evaluating a power function per pixel every frame cost
 *  more than the reels did.                                           */
static void build_dome(int which){
  int r=SPINR, w=r*2+16, h=r*2+16, cx=w/2, cy=h/2;
  spr_t*sp=&domespr[which];
  sp->w=w; sp->h=h;
  sp->px=(uint8_t*)calloc((size_t)w*h,4);
  if(!sp->px) return;
  int pressed = which;
  for(int j=-r;j<=r;j++) for(int i=-r;i<=r;i++){
    if(i*i+j*j>r*r) continue;
    float nx=i/(float)r, ny=j/(float)r;
    float nz=sqrtf(fmaxf(0.0f,1.0f-nx*nx-ny*ny));
    float lam=clampf(nx*-0.45f + ny*-0.55f + nz*0.70f,0,1);
    float spec=powf(clampf(nx*-0.42f+ny*-0.52f+nz*0.74f,0,1),22.0f);
    uint32_t base = pressed?0x0E7A22:0x14A62E;
    uint32_t c = mixc(scalec(base,0.30f), mixc(base,0xC8FF9A,0.55f), lam);
    c = mixc(c,0xFFFFFF,spec*0.92f);
    uint8_t*o=sp->px+(((size_t)(cy+j))*w+(cx+i))*4;
    o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255);
    o[2]=(uint8_t)(c&255);       o[3]=255;
  }
  /* chrome ring */
  for(int k=0;k<3;k++){
    int rr=r+2+k;
    for(int a=0;a<900;a++){
      float th2=a*(TAU/900.0f);
      float v=0.45f+0.55f*(0.5f+0.5f*cosf(th2+2.3f));
      int x=cx+(int)(cosf(th2)*rr), y=cy+(int)(sinf(th2)*rr);
      if((unsigned)x>=(unsigned)w||(unsigned)y>=(unsigned)h) continue;
      uint32_t c=mixc(0x4A5060,0xF0F4FF,v);
      uint8_t*o=sp->px+(((size_t)y)*w+x)*4;
      o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255);
      o[2]=(uint8_t)(c&255);       o[3]=235;
    }
  }
  spr_bounds(sp);
}

static void spin_dome(int cx,int cy,int r,int pressed,float pulse){
  const spr_t*sp=&domespr[pressed?1:0];
  if(!sp->px) return;
  blit(sp,cx-sp->w/2,cy-sp->h/2,0,FBH,255,0,0.0f);
  if(pulse>0.0f){
    int rr=r+6;
    for(int a=0;a<720;a++){
      float th2=a*(TAU/720.0f);
      fb_blend(cx+(int)(cosf(th2)*rr),cy+(int)(sinf(th2)*rr),
               0xDFFFC0,(int)(150*pulse));
    }
  }
  text("SPIN",cx,cy-12,3,0x05240A,1,0);
  text("SPIN",cx,cy-13,3,0xFFFFFF,1,0);
  text("PRESS",cx,cy+12,1,0xDDFFC8,1,1);
}

/* ═══ BACKGROUND (built once, then memcpy'd every frame) ═══════════ */
static void vgrad(int x,int y,int w,int h,uint32_t top,uint32_t bot){
  for(int j=0;j<h;j++){
    uint32_t c=mixc(top,bot,(float)j/(h>1?h-1:1));
    for(int i=0;i<w;i++) fb_px(x+i,y+j,c);
  }
}

static const uint32_t SILVERG[4]= {0xFFFFFF,0xE0E8F8,0x9AA6C0,0x525C78};
static const uint32_t GOLDG[5] = {0xFFFDF0,0xFFEBA8,0xF0B420,0xA8760C,0x5E4206};
static const uint32_t REDG[4]   = {0xFFE4D8,0xFF7A5A,0xC81828,0x6A0A14};
static const uint32_t GREENG[4] = {0xEEFFE6,0x9CF57A,0x2E9A2E,0x115011};
static const uint32_t ICEG[4]   = {0xFFFFFF,0xCFEEFF,0x4FA8D8,0x14506E};
static const uint32_t RAINBOWG[6]={0xFFFFFF,0xFFE0A0,0xFF8AC8,0xA0C0FF,0x80FFD0,0x2A5A80};

/*  Reel drum shading.  A physical reel is a cylinder seen edge on, so
 *  it is brightest across the middle and falls into shadow at the top
 *  and bottom of the window.  Cream rather than white: the reference
 *  cabinet's drums are warm, and the chrome medallions read better on
 *  a warm ground than on a clinical one.                              */
static uint32_t drum_shade(int y){
  float t=(float)(y-GY)/(float)(GH-1);
  float d=fabsf(t-0.5f)*2.0f;                    /* 0 centre, 1 at a lip */
  float roll=clampf((d-0.78f)/0.22f,0,1);
  uint32_t c=mixc(0xFFF9EC,0xEEE2C6,clampf(d*0.6f,0,1));
  c=mixc(c,0x5E5644,roll*roll*0.78f);
  return c;
}

/* rail panel: black surround, gradient face, gold or steel rim, a header */
static void rail_panel(int x,int y,int w,int h,uint32_t hi,uint32_t lo,uint32_t rim,
                       const char*label,uint32_t lc){
  fb_rrect(x,y,w,h,12,0x000000,215);
  fb_rrectg(x+3,y+3,w-6,h-6,10,hi,lo,255);
  fb_rframe(x,y,w,h,12,2.0f,rim,255);
  if(label) text(label,x+w/2,y+8,1,lc,1,1);
}


/* button deck geometry, shared by the static art and the live redraw */
#define NBTN 5
static const int BTNX[NBTN] = {  24, 236, 448, 660, 872 };
static const int BTNW[NBTN] = { 196, 196, 196, 196, 196 };
static const char*BTNL1[NBTN]= {"PAYS","BET","BET","BET","ADD"};
static const char*BTNL2[NBTN]= {NULL, "LESS","MORE","MAX","CREDITS"};

static void build_bg(void){
  /* ── backdrop: deep cabinet blue, spotlit behind the reels ──────── */
  vgrad(0,0,FBW,FBH,0x141A3A,0x03040E);
  for(int y=0;y<FBH;y++) for(int x=0;x<FBW;x++){
    float dx=(x-FBW*0.5f)/620.0f, dy=(y-320.0f)/420.0f;
    float v=clampf(1.0f-sqrtf(dx*dx+dy*dy),0,1); v=v*v;
    if(v>0.002f) fb_add(x,y,(int)(v*34),(int)(v*30),(int)(v*62));
    float vx=(x-FBW*0.5f)/(FBW*0.5f), vy=(y-FBH*0.5f)/(FBH*0.5f);
    fb_blend(x,y,0x000000,(int)(clampf((vx*vx+vy*vy)*0.52f,0,0.72f)*255));
  }
  /* faint diagonal cabinet weave */
  for(int y=0;y<FBH;y++) for(int x=(y%6);x<FBW;x+=6) fb_blend(x,y,0xFFFFFF,5);

  /* ── marquee ───────────────────────────────────────────────────── */
  fb_rrect(4,2,FBW-8,MQH-4,15,0x000000,210);
  fb_rrectg(9,6,FBW-18,MQH-13,12,0xB4142C,0x35040C,255);
  for(int j=0;j<22;j++)
    for(int i=0;i<FBW-18;i++) fb_blend(9+i,6+j,0xFFFFFF,(22-j)*2);
  fb_rframe(4,2,FBW-8,MQH-4,15,2.0f,0x5E4206,255);
  fb_rframe(6,4,FBW-12,MQH-8,14,3.0f,0xF0C24A,255);
  fb_rframe(10,8,FBW-20,MQH-16,11,1.0f,0x8A5A10,255);
  /* the lettering, ticker and lights are live: see draw_marquee() */

  /* ── reel window: chrome bezel around a black recess ───────────── */
  fb_rrect(GX-16,GY-16,GW+32,GH+32,20,0x000000,255);
  for(int k=0;k<7;k++){                       /* brushed chrome bezel   */
    float v=0.30f+0.70f*fabsf(cosf(k*0.62f));
    fb_rframe(GX-15+k,GY-15+k,GW+30-k*2,GH+30-k*2,18.0f-k,
              1.0f,mixc(0x333A4C,0xF2F6FF,v),255);
  }
  fb_rframe(GX-8,GY-8,GW+16,GH+16,13,3.0f,0xF0C24A,255);
  fb_rframe(GX-4,GY-4,GW+8,GH+8,11,2.0f,0x3A2804,255);

  /* the drums themselves — bright, which is the whole point */
  for(int y=GY;y<GY+GH;y++){
    uint32_t c=drum_shade(y);
    for(int r=0;r<NREEL;r++){
      int x0=GX+r*CW+2, x1=GX+(r+1)*CW-2;
      for(int x=x0;x<x1;x++){
        float e=fabsf((x-(x0+x1)*0.5f)/((x1-x0)*0.5f));
        fb_px(x,y, e>0.93f? mixc(c,0x9A9484,(e-0.93f)/0.07f*0.45f) : c);
      }
    }
  }
  /* cell frames: a thin gold rule between every cell, so the 5x5 reads as
     a grid of game pieces rather than five strips of paper */
  for(int r=0;r<NREEL;r++) for(int row=0;row<NROW;row++)
    fb_rframe(GX+r*CW+4,GY+row*CH+3,CW-8,CH-6,10,1.2f,0xC9B47A,150);
  /* drum gaps: deep shadow so five separate reels read as five reels */
  for(int r=0;r<=NREEL;r++){
    int gx=GX+r*CW;
    for(int y=GY;y<GY+GH;y++)
      for(int k=-2;k<=1;k++){
        int a = (k==-2||k==1)?90:215;
        fb_blend(gx+k,y,0x0A0C16,a);
      }
  }

  /* ── left rail: progressive jackpot ladder ─────────────────────── */
  rail_panel(LRX,JPY,RAILW,JPH,0x2A2F52,0x090A18,0xF0C24A,"JACKPOTS - THEY GROW WITH YOUR BET",0xFFD98A);
  /* ULTIMATE gets the big window; the three below share one size */
  led_window(LRX+8,JPY+24,RAILW-16,46);
  text("ULTIMATE",LRX+12,JPY+74,1,0xFFE9A8,0,1);
  text("5 ULTIMATE 7s  -  100,000 X BET",LRX+RAILW-12,JPY+74,1,0xC9D2FF,2,1);
  {
    static const char*JN[3]={"MEGA","MAJOR","MINOR"};
    static const char*JH[3]={"7+ JACKPOTS  -  200 X BET","6 JACKPOTS  -  40 X BET","5 JACKPOTS  -  5 X BET"};
    for(int i=0;i<3;i++){
      int y=JPY+88+i*54;
      led_window(LRX+8,y,RAILW-16,34);
      text(JN[i],LRX+12,y+38,1,0xC9D2FF,0,1);
      text(JH[i],LRX+RAILW-12,y+38,1,0x8A93B8,2,1);
    }
  }
  /* bulb strip down the ladder's flanks */
  for(int i=0;i<9;i++){
    int y=JPY+14+i*28;
    for(int sd=0;sd<2;sd++){
      int x= sd? LRX+RAILW-5 : LRX+5;
      for(int j=-2;j<=2;j++) for(int k=-2;k<=2;k++)
        if(j*j+k*k<=4) fb_blend(x+k,y+j,0x6A4A10,220);
    }
  }
  /* resting feature panels — the lit versions are drawn over these */
  rail_panel(LRX,FEATY,134,FEATH,0x1B2038,0x070A16,0x5A6080,"FREE SPINS",0x7A82A8);
  rail_panel(LRX+RAILW-134,FEATY,134,FEATH,0x1B2038,0x070A16,0x5A6080,"MULTIPLIER",0x7A82A8);

  /* how to win */
  {
    int h=GY+GH-HOWY;
    rail_panel(LRX,HOWY,RAILW,h,0x1B2038,0x070A16,0x5A6080,"HOW TO WIN",0xFFD98A);
    static const char*HW[6]={"WINS READ LEFT TO RIGHT","3+ REELS, ONE EACH","STEP UP, DOWN OR ACROSS",
                             "EVERY WILD 7 DOUBLES IT","3 STARS = FREE SPINS","3 CROWNS = PICK BONUS"};
    for(int i=0;i<6;i++) text(HW[i],LRX+RAILW/2,HOWY+30+i*26,2,i==0?0xFFFFFF:0xC8D2F0,1,1);
  }

  /* ── right rail: meters ────────────────────────────────────────── */
  static const char*MN[3]={"CREDITS","BET","WIN"};
  for(int i=0;i<3;i++){
    int y=METY+i*METH;
    rail_panel(RRX,y,RAILW,METH-4,0x2A2F52,0x090A18,0xF0C24A,MN[i],0xFFD98A);
    led_window(RRX+8,y+20,RAILW-16,44);
  }

  /* controls crib, wholly static */
  {
    rail_panel(RRX,CTLY,RAILW,CTLH,0x1B2038,0x070A16,0x5A6080,"CONTROLS",0xFFD98A);
    static const char*CK[6]={"START / A","L / R","X","Y","SELECT","B"};
    static const char*CV[6]={"SPIN","BET","MAX BET","ADD CREDITS","PAYS","SLAM"};
    for(int i=0;i<6;i++){
      int y=CTLY+30+i*23;
      text(CK[i],RRX+12,y,2,0x9FE8FF,0,1);
      text(CV[i],RRX+RAILW-12,y,2,0xE8ECF8,2,1);
    }
  }
  /* last win panel, filled in live */
  rail_panel(RRX,LWY,RAILW,GY+GH-LWY,0x1B2038,0x070A16,0x5A6080,"LAST WIN",0x7A82A8);

  /* ── button deck ───────────────────────────────────────────────── */
  fb_rrect(4,DECKY,FBW-8,FBH-DECKY-4,15,0x000000,215);
  fb_rrectg(8,DECKY+4,FBW-16,FBH-DECKY-12,12,0x2B3358,0x080B18,255);
  fb_rframe(4,DECKY,FBW-8,FBH-DECKY-4,15,2.0f,0x5E4206,255);
  fb_rframe(6,DECKY+2,FBW-12,FBH-DECKY-8,13,3.0f,0xF0C24A,255);
  /* red/white/blue rake under the deck, as on the reference cabinet */
  for(int j=0;j<6;j++){
    uint32_t c = (j<2)?0xC81828:((j<4)?0xE8ECF8:0x1A3A9A);
    for(int i=14;i<FBW-14;i++) fb_blend(i,DECKY+6+j,c,190);
  }
  for(int i=0;i<NBTN;i++)
    keycap(BTNX[i],BTNY,BTNW[i],BTNH,BTNL1[i],BTNL2[i],0,0xB9BECC);

  memcpy(bg,fb,sizeof bg);
}

/* half-size blit, 2x2 box filtered — used by the paytable */
static void blit_half(const spr_t*s,int dx,int dy){
  if(!s->px) return;
  for(int y=0;y+1<s->h;y+=2) for(int x=0;x+1<s->w;x+=2){
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

static void draw_reels(void){
  for(int r=0;r<NREEL;r++){
    int rx0=GX+r*CW+2, rx1=GX+(r+1)*CW-2;

    /* a reel gone fully wild in free spins is lit end to end */
    if(G.expand[r]){
      float pulse=0.55f+0.45f*sinf(G.t*7.0f);
      if(opt_limiter) pulse=0.6f+pulse*0.4f;
      for(int y=GY;y<GY+GH;y++){
        float ey=1.0f-fabsf((y-(GY+GH*0.5f))/(GH*0.5f));
        int a=(int)(120*pulse*(0.35f+ey*0.65f));
        for(int x=rx0;x<rx1;x++) fb_blend(x,y,0xFFC83A,a);
      }
      fb_frame(rx0,GY+1,rx1-rx0,GH-2,3,0xFFE08A,(int)(230*pulse));
    }

    int base=(int)floorf(G.rpos[r]);
    float frac=G.rpos[r]-base;
    int blurred = G.reelBlur[r] > 0.35f;
    for(int j=0;j<NROW+1;j++){
      int idx = stripAt(r, base - j + 1);
      int sy  = GY + (j-1)*CH + (int)(frac*CH) + SOY;
      int sx  = GX + r*CW + SOX;
      if(G.expand[r] && !blurred) idx = SY_SEVEN;
      if(!blurred && is_special(idx) && !G.expand[r]){
        uint32_t ac=special_col(idx);
        float pl=0.60f+0.40f*sinf(G.t*4.4f + r*0.7f + j*0.5f);
        if(opt_limiter) pl=0.70f+pl*0.30f;
        int cyy=sy-SOY;
        /* tinted cell backing + lit frame, both clipped to the window */
        if(cyy>=GY-CH && cyy<=GY+GH){
          cell_wash(sx+SYMW/2, sy+SYMH/2, ac, 0.30f*pl);
          if(cyy>=GY-2 && cyy+CH<=GY+GH+2)
            fb_rframe(GX+r*CW+4,cyy+3,CW-8,CH-6,10,2.5f,ac,(int)(225*pl));
        }
        /* slow sparkle cross */
        float ang=G.t*1.6f + r + j;
        float rad=(float)SYMW*0.50f*(0.85f+0.15f*pl);
        int ccx=sx+SYMW/2, ccy=sy+SYMH/2;
        for(int k=0;k<4;k++){
          float aa=ang+k*(TAU/4.0f);
          float ex=ccx+cosf(aa)*rad, ey=ccy+sinf(aa)*rad;
          if(ey<GY+3||ey>GY+GH-3) continue;
          for(int t2=0;t2<3;t2++) for(int t3=0;t3<3;t3++)
            fb_blend((int)ex+t2-1,(int)ey+t3-1,0xFFFFFF,(int)(190*pl));
        }
      }
      blit(blurred?&symb[idx]:&sym[idx], sx,sy, GY,GY+GH, 255,0,0.0f);
    }
  }

  /* glass: a diagonal reflection and a soft inner shadow at the lip */
  for(int j=0;j<10;j++){
    int a=(10-j)*9;
    for(int i=0;i<GW;i++){
      fb_blend(GX+i,GY+j,0x000814,a);
      fb_blend(GX+i,GY+GH-1-j,0x000814,a);
    }
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
static void light_cluster(uint32_t m,uint32_t c,float pulse,int heavy){
  for(int i=0;i<NCELL;i++){
    if(!((m>>i)&1u)) continue;
    int rr=i/NROW, row=i%NROW;
    int cx=cellcx(rr), cy=cellcy(row);
    cell_wash(cx,cy,c,0.36f*pulse);
    fb_rframe(GX+rr*CW+4,GY+row*CH+3,CW-8,CH-6,10,heavy?4.0f:3.0f,c,(int)(235*pulse));
    fb_rframe(GX+rr*CW+8,GY+row*CH+7,CW-16,CH-14,7,1.5f,0xFFFFFF,(int)(150*pulse));
  }
  /* the wilds in the win wear their multiplier, so the doubling is
     visible rather than something to work out from the total */
  for(int i=0;i<NCELL;i++){
    if(!((m>>i)&1u)) continue;
    if(G.grid[i/NROW][i%NROW]!=SY_SEVEN) continue;
    int bx=cellcx(i/NROW)+CW/2-40, by=cellcy(i%NROW)+CH/2-26;
    fb_rrect(bx,by,34,20,6,0x1A0C00,(int)(230*pulse));
    fb_rframe(bx,by,34,20,6,1.5f,0xFFD24A,(int)(255*pulse));
    text("X2",bx+17,by+6,2,0xFFE9A8,1,0);
  }
  for(int i=0;i<NCELL;i++){
    if(!((m>>i)&1u)) continue;
    int rr=i/NROW, row=i%NROW;
    if(rr+1>=NREEL) continue;
    for(int dw=-1;dw<=1;dw++){
      int nw=row+dw;
      if(nw<0||nw>=NROW) continue;
      int j=(rr+1)*NROW+nw;
      if(!((m>>j)&1u)) continue;
      float x0=(float)cellcx(rr), y0=(float)cellcy(row);
      float x1=(float)cellcx(rr+1), y1=(float)cellcy(nw);
      fb_line(x0,y0,x1,y1,7,0x101018,190);
      fb_line(x0,y0,x1,y1,3,c,(int)(170*pulse)+80);
    }
  }
}

static void draw_wins(void){
  if(G.state!=ST_SHOWWIN || G.nWin<=0) return;
  int w=G.showIdx;
  float pulse=0.55f+0.45f*sinf(G.t*9.0f);
  /* every other winning cluster stays faintly lit behind the featured one */
  for(int i=0;i<G.nWin;i++) if(i!=w) light_cluster(G.winMask[i],WINCOL[i&7],0.35f,0);
  light_cluster(G.winMask[w],WINCOL[w&7],pulse,1);
}

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
   *  number pops up a size and drops back each time it climbs.        */
  int m = G.inFree? G.fsMult : G.pickMult;
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
  textb(b,LRX+RAILW-67,FEATY+26-(px-4)*4,px,mg,m>=4?5:4,1);

  /* last win: the featured cluster while a win shows, else the last total */
  int show = (G.state==ST_SHOWWIN && G.nWin>0);
  if(show){
    int w=G.showIdx;
    uint32_t c=WINCOL[w&7];
    rail_panel(RRX,LWY,RAILW,GY+GH-LWY,0x1B2038,0x070A16,c,"LAST WIN",0xFFFFFF);
    blit_half(&sym[G.winSym[w]],RRX+14,LWY+30);
    text(SYMNAME[G.winSym[w]],RRX+76,LWY+34,2,0xFFFFFF,0,1);
    snprintf(b,sizeof b,"%d REELS  X %d WAY%s",G.winCnt[w],G.winWays[w],G.winWays[w]==1?"":"S");
    text(b,RRX+76,LWY+56,2,c,0,1);
    if(G.winWt[w]>G.winWays[w]){
      float pl=0.6f+0.4f*sinf(G.t*8.0f);
      snprintf(b,sizeof b,"WILD X%d",G.winWt[w]/G.winWays[w]);
      text(b,RRX+RAILW-14,LWY+56,2,mixc(0xFFC24A,0xFFFFFF,pl*0.6f),2,1);
    }
    snprintf(b,sizeof b,"PAYS %d",G.winAmt[w]);
    text(b,RRX+76,LWY+78,2,0xFFE9A8,0,1);
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
    uint32_t base = i==0 ? 0xFFE080 : 0xFFB020;
    if(i==0){ float sh=0.5f+0.5f*sinf(G.t*1.3f); base=mixc(0xFFD860,0xFFF8E0,sh*0.5f); }
    uint32_t on = hot ? mixc(base,0xFFFFFF,pl) : base;
    long long v = hot?G.jpAmt:jp_value(i);
    if(i==0) seg_num(v, LRX+RAILW-13, y+8, 12, 10, 26, on, 0x4A3406, 1);
    else     seg_num(v, LRX+RAILW-13, y+6, 10,  8, 20, on, 0x4A3406, 1);
    if(hot) fb_rframe(LRX+6,y-2,RAILW-12,h+4,8,2.0f,0xFFFFFF,(int)(255*pl));
  }
  /* the ladder's bulbs chase, and run hot while a jackpot is live */
  for(int i=0;i<9;i++){
    float sp2 = (G.state==ST_JACKPOT)?9.0f:3.0f;
    float v=0.30f+0.70f*(0.5f+0.5f*sinf(G.t*sp2 - i*0.5f));
    if(opt_limiter) v=0.45f+v*0.45f;
    int y=JPY+14+i*28;
    uint32_t c=mixc(0x6A4A10,0xFFE9A0,v);
    for(int sd=0;sd<2;sd++){
      int x= sd? LRX+RAILW-5 : LRX+5;
      for(int j=-2;j<=2;j++) for(int k=-2;k<=2;k++)
        if(j*j+k*k<=4) fb_blend(x+k,y+j,c,(int)(v*235));
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
  for(int i=0;i<NBTN;i++){
    if(btnFlash[i]<=0) continue;
    btnFlash[i]-=DT;
    keycap(BTNX[i],BTNY,BTNW[i],BTNH,BTNL1[i],BTNL2[i],1,0xE2E6F0);
  }
  float sp = (G.state==ST_IDLE||G.state==ST_ATTRACT)
             ? 0.5f+0.5f*sinf(G.t*3.2f) : 0.0f;
  spin_dome(SPINX,SPINY,SPINR,G.state==ST_SPIN,sp);
}

/*  Full-screen dim.  fb_rect would run the general per-pixel blend with
 *  its bounds checks over 900k pixels; this is the same result in a
 *  straight loop, and it is on screen whenever an overlay is up.      */
static void screen_tint(uint32_t c,int a){
  if(a<=0) return;
  if(a>255) a=255;
  int sr=(c>>16)&255, sg=(c>>8)&255, sb=c&255;
  uint32_t*q=fb;
  for(int i=FBW*FBH;i>0;i--,q++){
    uint32_t d=*q;
    int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
    *q = RGB(dr+((sr-dr)*a>>8), dg+((sg-dg)*a>>8), db+((sb-db)*a>>8));
  }
}
static void dim(int a){ screen_tint(0x000000,a); }

/*  Cached full-screen backdrops.  Built on first use so a player who
 *  never opens the pay table never pays for it.                       */
static uint32_t *ptbg = NULL, *bnbg = NULL;
static uint32_t *ptimg[2] = {NULL,NULL}, *bnimg = NULL;
static uint32_t bnkey = 0xFFFFFFFFu;
static void bonus_invalidate(void){ bnkey=0xFFFFFFFFu; }

static void cache_backdrop(uint32_t**slot, void(*paint)(void)){
  if(*slot){ memcpy(fb,*slot,(size_t)FBW*FBH*4); return; }
  paint();
  *slot=(uint32_t*)malloc((size_t)FBW*FBH*4);
  if(*slot) memcpy(*slot,fb,(size_t)FBW*FBH*4);
}

/* pay table rows: 13 symbols in two columns */
#define PTX0 40
#define PTX1 660
#define PTY0 78
#define PTRH 74

static void paint_paytable_bg(void){
  vgrad(0,0,FBW,FBH,0x12173A,0x03040C);
  fb_rframe(14,10,FBW-28,FBH-20,14,3.0f,0xE8B93C,255);
  for(int i=0;i<NSYM;i++){
    int col=i/7, row=i%7;
    int x=(col?PTX1:PTX0), y=PTY0+row*PTRH;
    fb_rrect(x-8,y-4,588,PTRH-6,10,0x1E2450,150);
  }
}

static void paint_paytable(void){
  cache_backdrop(&ptbg,paint_paytable_bg);
  textb("PAY TABLE",FBW/2,14,5,GOLDG,5,1);
  char b[96];
  /* the band header over each column */
  for(int col=0;col<2;col++){
    int x=(col?PTX1:PTX0)+72;
    for(int k=0;k<3;k++) text(BANDNAME[k],x+120+k*130,PTY0-14,1,0xFFC24A,1,1);
    text("X BET, PER WAY",x,PTY0-14,1,0xFFC24A,0,1);
  }
  for(int i=0;i<NSYM;i++){
    int col=i/7, row=i%7;
    int x=(col?PTX1:PTX0), y=PTY0+row*PTRH;
    blit_half(&sym[i],x+2,y+6);
    text(SYMNAME[i],x+72,y+6,2,0xFFFFFF,0,1);
    if(i<NPAYSYM){
      /* pays as multiples of the total bet, one decimal where it needs it */
      for(int k=0;k<3;k++){
        float mult=PAY[i][k+3]/10.0f;
        if(mult>=10.0f) snprintf(b,sizeof b,"%.0f",mult);
        else            snprintf(b,sizeof b,"%.1f",mult);
        text(b,x+192+k*130,y+28,2,0xFFE9A8,1,1);
      }
      if(i==SY_SEVEN) text("STANDS IN FOR ANY SYMBOL - AND DOUBLES EVERY WIN IT JOINS",x+72,y+52,1,0xFFC24A,0,1);
      else            text("X TOTAL BET, TIMES THE NUMBER OF WAYS",x+72,y+52,1,0x8A93B8,0,1);
    } else if(i==SY_STAR){
      snprintf(b,sizeof b,"3 / 4 / 5 ANYWHERE = %d / %d / %d X BET",SCATPAY[3],SCATPAY[4],SCATPAY[5]);
      text(b,x+72,y+30,2,0xC8D8FF,0,1);
      text("3+ = 8 FREE SPINS, EXPANDING WILDS",x+72,y+50,2,0xFFC24A,0,1);
    } else if(i==SY_CROWN){
      text("3 CROWNS = LUCKY 7 PICK BONUS",x+72,y+30,2,0xFFC24A,0,1);
      text("THEY LAND ON REELS 1, 3 AND 5",x+72,y+50,2,0xC8D8FF,0,1);
    } else if(i==SY_JACKPOT){
      text("5 = MINOR, 5 X BET.  6 = MAJOR, 40 X.  7+ = MEGA, 200 X",x+72,y+30,2,0xFFC24A,0,1);
      text("LANDS ON REELS 2, 3 AND 4",x+72,y+50,2,0xC8D8FF,0,1);
    } else if(i==SY_ULT){
      text("5 TOUCHING = THE ULTIMATE, 100,000 X BET",x+72,y+30,2,0xFFC24A,0,1);
      text("1,000,000 AT THE SMALLEST BET, AND UP FROM THERE",x+72,y+50,2,0xC8D8FF,0,1);
    }
  }
  text("A WIN READS LEFT TO RIGHT FROM REEL 1: ONE SYMBOL PER REEL, SAME ROW OR ONE UP OR DOWN",
       FBW/2,602,2,0xFFFFFF,1,1);
  text("A TENTH OF EVERY BET FEEDS THE JACKPOTS, AND ALL FOUR ARE A MULTIPLE OF THE BET YOU PLAY",
       FBW/2,624,2,0xFFC24A,1,1);
  text("EVERY PATH IS A WAY AND PAYS AGAIN, AND EVERY WILD IN IT DOUBLES THE WIN.   BET 10 TO 10,000 A SPIN.",FBW/2,646,2,0xAFAFC8,1,1);
}


/*  Page two of the pay table: how the features are won AND how they are
 *  played, so nobody has to learn the pick round by losing it.        */
static void paint_features(void){
  vgrad(0,0,FBW,FBH,0x12173A,0x03040C);
  fb_rframe(14,10,FBW-28,FBH-20,14,3.0f,0xE8B93C,255);
  textb("FEATURES",FBW/2,14,5,GOLDG,5,1);

  struct band { const char*title; uint32_t col; int sy1, sy2; const char*ln[5]; };
  static const struct band B[4] = {
    { "WILD 7 - THE MULTIPLIER SYMBOL", 0xFFC24A, SY_SEVEN, -1, {
      "WILD 7 STANDS IN FOR EVERY PAYING SYMBOL, AND DOUBLES EVERY WIN IT IS PART OF",
      "TWO WILDS IN ONE WIN PAY FOUR TIMES, THREE PAY EIGHT TIMES, AND SO ON",
      "THE WILDS IN A WIN LIGHT UP WEARING THEIR X2 SO YOU CAN SEE WHERE IT CAME FROM",
      NULL, NULL } },
    { "FREE SPINS", 0x7CFF6A, SY_STAR, -1, {
      "3 OR MORE SCATTERS AWARD 8 FREE SPINS - 3 MORE DURING THE FEATURE ADD 4 MORE",
      "EXPANDING WILDS: A WILD 7 LANDING ON REEL 2, 3 OR 4 GROWS TO FILL ITS WHOLE REEL",
      "AND NOTCHES THE MULTIPLIER UP ONE - IT NEVER FALLS BACK, AND CLIMBS TO X5",
      "IT TIMES THE WHOLE SPIN, ON TOP OF THE X2 EVERY WILD ALREADY PAYS. SCATTERS PAY TOO",
      NULL } },
    { "LUCKY 7 PICK", 0xC060FF, SY_CROWN, -1, {
      "3 CROWNS (THEY LAND ON REELS 1, 3 AND 5) OPEN A BOARD OF NINE HIDDEN PANELS",
      "D-PAD MOVES THE CURSOR, A TURNS A PANEL.  PANELS HIDE CREDITS, A X2 MULTIPLIER, OR A STOP",
      "THE ROUND ENDS ON THE THIRD STOP, SO YOU USUALLY GET FOUR OR FIVE PICKS",
      "THE MULTIPLIER APPLIES TO EVERYTHING YOU COLLECTED.  A COLLECTS AT THE END",
      NULL } },
    { "PROGRESSIVE JACKPOTS", 0xFFB020, SY_JACKPOT, SY_ULT, {
      "JACKPOT LANDS ON REELS 2, 3 AND 4.  5 TOUCHING = MINOR, 6 = MAJOR, 7 OR MORE = MEGA",
      "ULTIMATE LANDS ONE PER REEL.  ALL FIVE TOUCHING = THE ULTIMATE, 100,000 TIMES YOUR BET",
      "EVERY POT IS A MULTIPLE OF YOUR BET - 5X, 40X, 200X, 100,000X - PLUS EVERYTHING FED IN",
      "A TENTH OF EVERY BET FEEDS THEM, SO RAISING YOUR BET RAISES ALL FOUR METERS AT ONCE",
      NULL } },
  };
  for(int i=0;i<4;i++){
    int y=70+i*152, h=142;
    fb_rrect(30,y,FBW-60,h,14,0x1E2450,150);
    fb_rframe(30,y,FBW-60,h,14,2.0f,B[i].col,200);
    blit_half(&sym[B[i].sy1],46,y+14);
    if(B[i].sy2>=0) blit_half(&sym[B[i].sy2],46,y+76);
    text(B[i].title,120,y+12,3,B[i].col,0,1);
    for(int k=0;k<4 && B[i].ln[k];k++) text(B[i].ln[k],120,y+44+k*24,2,k==0?0xFFFFFF:0xC8D8FF,0,1);
  }
}

static void paint_bonus_bg(void){
  vgrad(0,0,FBW,FBH,0x2E0C40,0x05020A);
  for(int y=0;y<FBH;y++) for(int x=0;x<FBW;x++){
    float dx=(x-FBW*0.5f)/460.0f, dy=(y-380.0f)/380.0f;
    float v=clampf(1.0f-sqrtf(dx*dx+dy*dy),0,1); v=v*v;
    if(v>0.003f) fb_add(x,y,(int)(v*76),(int)(v*36),0);
  }
  fb_rframe(12,8,FBW-24,FBH-16,14,3.0f,0xF0C24A,255);
}

static void paint_bonus(void){
  cache_backdrop(&bnbg,paint_bonus_bg);
  textb("LUCKY 7 PICK",FBW/2,18,6,GOLDG,5,1);

  char b[80];
  snprintf(b,sizeof b,"COLLECTED %d",G.pickTotal);
  text(b,FBW/2-190,80,3,0xFFFFFF,1,1);
  snprintf(b,sizeof b,"MULTIPLIER X%d",G.pickMult>0?G.pickMult:1);
  text(b,FBW/2+190,80,3,0xC060FF,1,1);

  text("STOPS",FBW/2-104,116,2,0xAFAFC8,2,1);
  for(int i=0;i<3;i++){
    int lit = i < G.pickStops;
    int cx=FBW/2-70+i*34, cy=122;
    for(int j=-11;j<=11;j++) for(int k=-11;k<=11;k++){
      int d=j*j+k*k;
      if(d<=121) fb_blend(cx+k,cy+j,lit?0xFF3A3A:0x2A1020,255);
      if(d<=121 && d>=96) fb_blend(cx+k,cy+j,0xF0C24A,255);
    }
    if(lit) for(int j=-5;j<=5;j++) for(int k=-5;k<=5;k++)
      if(j*j+k*k<=25) fb_blend(cx+k,cy+j,0xFFC0C0,200);
  }
  text("THIRD STOP ENDS THE ROUND",FBW/2+50,116,2,0xAFAFC8,0,1);

  const int PW=260, PH=148, GAPX=26, GAPY=18;
  int bx0=(FBW-(3*PW+2*GAPX))/2, by0=166;
  for(int i=0;i<NPICK;i++){
    int cc=i%3, rr=i/3;
    int x=bx0+cc*(PW+GAPX), y=by0+rr*(PH+GAPY);
    int cur=(i==G.pickCur);
    fb_rrect(x+4,y+7,PW,PH,18,0x000000,160);
    if(!G.pickDone[i]){
      fb_rrectg(x,y,PW,PH,18,0x54307E,0x1C0D32,255);
      for(int j=0;j<PH/3;j++)
        for(int k=8;k<PW-8;k++) fb_blend(x+k,y+3+j,0xFFFFFF,(PH/3-j)*2);
      textb("7",x+PW/2,y+30,13,REDG,4,1);
    } else if(G.pickKind[i]==PICK_STOP){
      fb_rrectg(x,y,PW,PH,18,0x76121C,0x2A050A,255);
      text("STOP",x+PW/2,y+PH/2-17,6,0xFF9A9A,1,1);
    } else if(G.pickKind[i]==PICK_MULT){
      fb_rrectg(x,y,PW,PH,18,0x10682F,0x042810,255);
      char m[8]; snprintf(m,sizeof m,"X%d",G.pickVal[i]);
      textb(m,x+PW/2,y+38,10,GREENG,4,1);
    } else {
      fb_rrectg(x,y,PW,PH,18,0x1E4478,0x08122A,255);
      char m[16]; snprintf(m,sizeof m,"%d",G.pickVal[i]);
      textb(m,x+PW/2,y+40,9,GOLDG,5,1);
    }
    fb_rframe(x,y,PW,PH,18,cur?5.0f:2.5f,cur?0xFFFFFF:0xB08A20,255);
  }
  text("D-PAD TO MOVE          A TO PICK",FBW/2,FBH-38,3,0xC8C8E0,1,1);
}

/*  Both of these screens are static between player actions, and both
 *  were repainting every rounded rectangle and every string sixty times
 *  a second: the bonus board measured 37 ms a frame on its own.  Paint
 *  once into a buffer, then blit it and draw only what animates.      */
static void draw_paytable(void){
  int pg = G.ptPage&1;
  if(!ptimg[pg]){
    if(pg) paint_features(); else paint_paytable();
    ptimg[pg]=(uint32_t*)malloc((size_t)FBW*FBH*4);
    if(ptimg[pg]) memcpy(ptimg[pg],fb,(size_t)FBW*FBH*4);
  } else memcpy(fb,ptimg[pg],(size_t)FBW*FBH*4);
  if(((int)(G.t*2.0f))&1)
    text(pg ? "SELECT = PAY TABLE          ANY OTHER BUTTON = BACK TO THE GAME"
            : "SELECT = FEATURES AND JACKPOTS          ANY OTHER BUTTON = BACK TO THE GAME",
         FBW/2,684,2,0xFFFFFF,1,1);
}

/* what the board looks like is a function of exactly these */
static uint32_t bonus_key(void){
  uint32_t k=(uint32_t)(G.pickCur*7u + G.pickStops*131u);
  for(int i=0;i<NPICK;i++) k = k*33u + (uint32_t)(G.pickDone[i]*(i+1));
  k = k*33u + (uint32_t)G.pickTotal;
  k = k*33u + (uint32_t)G.pickMult;
  return k;
}

static void draw_bonus(void){
  uint32_t k=bonus_key();
  if(!bnimg) bnimg=(uint32_t*)malloc((size_t)FBW*FBH*4);
  if(!bnimg){ paint_bonus(); return; }
  if(k!=bnkey){ paint_bonus(); memcpy(bnimg,fb,(size_t)FBW*FBH*4); bnkey=k; }
  else memcpy(fb,bnimg,(size_t)FBW*FBH*4);

  /* the one thing that animates: the cursor's halo */
  const int PW=260, PH=148, GAPX=26, GAPY=18;
  int bx0=(FBW-(3*PW+2*GAPX))/2, by0=166;
  int x=bx0+(G.pickCur%3)*(PW+GAPX), y=by0+(G.pickCur/3)*(PH+GAPY);
  float pl=0.5f+0.5f*sinf(G.t*9.0f);
  fb_rframe(x-5,y-5,PW+10,PH+10,22,2.5f,0xFFE08A,(int)(220*pl));
}

/* the jackpot celebration */
static void draw_jackpot(void){
  float pl=0.5f+0.5f*sinf(G.t*8.0f);
  int ult = (G.jpWon==JP_ULT);
  dim(ult?150:200);
  /* the winning cells come back to full brightness through the dim, so the
     player sees exactly what did it */
  for(int c=0;c<NCELL;c++) if((G.jpMask>>c)&1u){
    int r=c/NROW,row=c%NROW;
    int idx=G.grid[r][row];
    blit(&sym[idx],GX+r*CW+SOX,GY+row*CH+SOY,GY,GY+GH,255,0,0.0f);
  }
  light_cluster(G.jpMask, ult?0x9AF0FF:0xFF6A30, 0.7f+0.3f*pl, 1);
  for(int i=0;i<40;i++){
    float a=G.t*0.55f + i*(TAU/40.0f);
    float ca=cosf(a), sa=sinf(a);
    for(int d=110;d<640;d+=1){
      int x=(int)(FBW*0.5f+ca*d), y=(int)(312+sa*d*0.55f);
      if((unsigned)x>=FBW||y<150||y>=474) continue;
      float f=(1.0f-(d-110)/530.0f);
      if(ult) fb_add(x,y,(int)(30*pl*f),(int)(48*pl*f),(int)(60*pl*f));
      else    fb_add(x,y,(int)(52*pl*f),(int)(38*pl*f),(int)(6*pl*f));
    }
  }
  char b[64];
  fb_rect(0,196,FBW,232,0x000000,220);
  uint32_t rule = ult ? mixc(0x9AF0FF,0xFFFFFF,pl) : 0xFFD24A;
  for(int j=0;j<3;j++)
    for(int i=0;i<FBW;i++){ fb_blend(i,196+j,rule,255); fb_blend(i,427-j,rule,255); }
  snprintf(b,sizeof b,"%s JACKPOT",JP_NAME[G.jpWon<0?JP_MINOR:G.jpWon]);
  textb(b,FBW/2,214,ult?8:7,ult?RAINBOWG:GOLDG,ult?6:5,1);
  seg_num(G.jpAmt, FBW/2+seg_width(12,21,58,1)/2, 322, 12, 21, 58,
          mixc(ult?0xB0F0FF:0xFFB020,0xFFFFFF,pl*0.7f), 0x4A3406, 1);
  if(((int)(G.t*2.4f))&1) text("PRESS ANY BUTTON TO COLLECT",FBW/2,442,3,0xFFFFFF,1,1);
}

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

static void draw_overlays(void){
  char b[96];
  if(G.inFree && (G.state==ST_IDLE||G.state==ST_SPIN||G.state==ST_EVAL||G.state==ST_SHOWWIN)){
    int bw2=GW+12, bx2=GX-6;
    fb_rrect(bx2,GY-34,bw2,28,8,0x04140A,240);
    fb_rframe(bx2,GY-34,bw2,28,8,2.0f,0x7CFF6A,255);
    snprintf(b,sizeof b,"FREE SPINS  -  %d LEFT  -  MULTIPLIER X%d  -  WON %d",
             G.freeSpins,G.fsMult<1?1:G.fsMult,G.fsWon);
    text(b,GX+GW/2,GY-27,2,0x7CFF6A,1,1);
  }
  switch(G.state){
  case ST_ATTRACT:
    dim(160);
    textb("WILD 7's",FBW/2,120,10,GOLDG,5,1);
    text("WINS READ LEFT TO RIGHT   -   3, 4 OR 5 REELS   -   EVERY PATH PAYS",FBW/2,262,3,0xFFFFFF,1,1);
    text("EVERY WILD DOUBLES   -   FREE SPINS TO X5   -   JACKPOTS SCALE WITH YOUR BET",FBW/2,302,3,0x9FE8FF,1,1);
    snprintf(b,sizeof b,"ULTIMATE JACKPOT   %lld",jp_value(JP_ULT));
    textb(b,FBW/2,340,5,RAINBOWG,6,1);
    if(((int)(G.t*2.0f))&1) textb("PRESS START",FBW/2,420,6,SILVERG,4,1);
    text("SELECT = PAY TABLE       X = MAX BET       Y = ADD CREDITS",FBW/2,508,2,0xAFAFC8,1,1);
    break;
  case ST_FSINTRO: {
    dim(200);
    float sc=1.0f+0.12f*sinf(G.t*7.0f);
    int px=(int)(11*sc); if(px<7)px=7;
    textb("FREE SPINS",FBW/2,200,px,GREENG,4,1);
    snprintf(b,sizeof b,"%d SCATTERS",G.scatCount);
    text(b,FBW/2,348,4,0xFFFFFF,1,1);
    text(G.inFree?"4 MORE FREE SPINS":"8 FREE SPINS  -  EXPANDING WILDS  -  EVERY ONE CLIMBS THE MULTIPLIER",
         FBW/2,404,2,0x9FE8FF,1,1);
    break; }
  case ST_BONUSEND:
    dim(200);
    if(G.banner==3){
      textb("FREE SPINS COMPLETE",FBW/2,200,6,GREENG,4,1);
      snprintf(b,sizeof b,"TOTAL WON  %d",G.fsWon);
      textb(b,FBW/2,326,7,GOLDG,5,1);
    } else {
      textb("BONUS COMPLETE",FBW/2,200,7,GOLDG,5,1);
      snprintf(b,sizeof b,"%d  X%d  =  %d",G.pickTotal,G.pickMult>0?G.pickMult:1,
               G.pickTotal*(G.pickMult>0?G.pickMult:1));
      textb(b,FBW/2,326,6,SILVERG,4,1);
    }
    break;
  case ST_BROKE:
    dim(190);
    textb("OUT OF CREDITS",FBW/2,248,7,REDG,4,1);
    if(((int)(G.t*2.0f))&1) text("PRESS START TO ADD CREDITS",FBW/2,382,4,0xFFFFFF,1,1);
    break;
  case ST_ADDCR:
    draw_addcr();
    break;
  }
  if(G.multUp>0 && G.inFree &&
     (G.state==ST_IDLE||G.state==ST_SPIN||G.state==ST_EVAL||G.state==ST_SHOWWIN)){
    /* short, loud, and gone: the meter climbing is the best news in the
       feature, so it gets the middle of the screen for a second */
    float u=1.0f-G.multUp/1.9f;                  /* 0 at the notch, 1 at the end */
    float rise=1.0f-(1.0f-u)*(1.0f-u);
    int by2=GY+GH/2-58-(int)(rise*70.0f);
    int a=(int)(255*clampf(G.multUp*1.3f,0,1));
    int bw2=520, bx2=FBW/2-bw2/2;
    fb_rrect(bx2,by2,bw2,116,20,0x0A0518,(int)(a*0.88f));
    fb_rframe(bx2,by2,bw2,116,20,3.0f,0xFFD24A,a);
    text("MULTIPLIER UP",FBW/2,by2+14,3,0xFFE9A8,1,1);
    float pop=1.0f+0.35f*(1.0f-u)*(1.0f-u);
    snprintf(b,sizeof b,"X%d",G.fsMult);
    textb(b,FBW/2,by2+40,(int)(7*pop),GOLDG,5,1);
    for(int k=0;k<10;k++){                       /* rays behind the number */
      float ang=G.t*2.2f+k*(TAU/10.0f);
      for(int d=48;d<150;d+=2){
        int x=(int)(FBW*0.5f+cosf(ang)*d), y=(int)(by2+64+sinf(ang)*d*0.42f);
        float f=(1.0f-(d-48)/102.0f)*(a/255.0f);
        fb_add(x,y,(int)(50*f),(int)(38*f),(int)(6*f));
      }
    }
  }
  if(G.bannerT>0 && (G.banner==1||G.banner==2)){
    const char*t = G.banner==2?"MEGA WIN":"BIG WIN";
    float sc=1.0f+0.08f*sinf(G.t*10.0f);
    int px=(int)((G.banner==2?11:9)*sc);
    int bw2=660, bh2=170, bx2=FBW/2-bw2/2, by2=GY+GH/2-bh2/2;
    /* a soft halo built from 26 stacked rounded rects cost 40 ms a frame,
       each one evaluating a distance field over the whole plate.  One
       ring reads the same against a lit reel window. */
    fb_rrect(bx2-16,by2-16,bw2+32,bh2+32,36,0x000000,105);
    fb_rrectg(bx2,by2,bw2,bh2,22,0x2A1030,0x06020A,236);
    fb_rframe(bx2,by2,bw2,bh2,22,3.0f,0xFFD24A,255);
    fb_rframe(bx2+6,by2+6,bw2-12,bh2-12,17,1.0f,0x8A6A10,220);
    textb(t,FBW/2,by2+18,px,G.banner==2?ICEG:GOLDG,4,1);
    int shown2=(G.state==ST_SHOWWIN)?G.winShown:G.winTotal;
    seg_num(shown2, FBW/2+seg_width(7,22,54,1)/2, by2+96, 7, 22, 54,
            0xFFB020, 0x4A3406, 1);
  }
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

/* fixed-size text with no auto-fit, glyphs off screen skipped: the ticker */
static void text_run(const char*s,int x,int y,int px,uint32_t col,int shadow){
  int n=(int)strlen(s), adv=px*6;
  for(int pass=(shadow?0:1); pass<2; pass++){
    uint32_t c = pass==0 ? 0x000000 : col;
    int off = pass==0 ? px : 0;
    for(int i=0;i<n;i++){
      int gx0=x+i*adv;
      if(gx0+px*5<0 || gx0>=FBW) continue;
      int ch=(unsigned char)s[i];
      if(ch>='a'&&ch<='z') ch-=32;
      if(ch<32||ch>127) ch='?';
      const uint8_t*gl=FONT[ch-32];
      for(int r=0;r<7;r++){
        uint8_t bits=gl[r];
        for(int cbit=0;cbit<5;cbit++){
          if(!(bits&(0x10>>cbit))) continue;
          int gx=gx0+cbit*px+off, gy=y+r*px+off;
          for(int j=0;j<px;j++) for(int k=0;k<px;k++) fb_px(gx+k,gy+j,c);
        }
      }
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

static void draw_marquee(void){
  mqT += DT;
  float lim = opt_limiter ? 0.6f : 1.0f;

  /* 1. a slow shimmer sweeping the red face */
  for(int y=8;y<MQH-8;y++){
    for(int x=12;x<FBW-12;x++){
      float ph=fmodf((float)x + y*1.4f - mqT*240.0f, 520.0f);
      if(ph<0) ph+=520.0f;
      if(ph>=110.0f) continue;
      float v=sinf(ph/110.0f*3.14159f)*lim;
      fb_add(x,y,(int)(v*46),(int)(v*22),(int)(v*8));
    }
  }

  /* 2. the ticker, passing behind the sign */
  char pot[24], tk[400];
  commas(pot,sizeof pot,jp_value(JP_ULT));
  snprintf(tk,sizeof tk,
    "  *  WINS READ LEFT TO RIGHT, REEL TO REEL  *  3, 4 OR 5 REELS PAY  *  EVERY PATH IS A WAY  *  "
    "EVERY WILD 7 DOUBLES THE WIN  *  FOUR JACKPOTS, EVERY ONE A MULTIPLE OF YOUR BET  *  ULTIMATE NOW %s  *  "
    "FREE SPINS: THE MULTIPLIER CLIMBS TO X5 AND NEVER FALLS BACK  *  LUCKY 7 PICK BONUS  *  "
    "BET 10 TO 10,000  ",pot);
  int tw=(int)strlen(tk)*12;
  int off=(int)fmodf(mqT*120.0f,(float)tw);
  text_run(tk,20-off,   MQH/2-7,2,0xFFF0C0,1);
  text_run(tk,20-off+tw,MQH/2-7,2,0xFFF0C0,1);

  /* 3. the title sign */
  const int pw=300, ph2=MQH-12, px0=FBW/2-pw/2, py0=6;
  fb_rrect(px0-4,py0-2,pw+8,ph2+4,14,0x000000,160);
  fb_rrectg(px0,py0,pw,ph2,12,0x3A0810,0x120206,255);
  float pulse=0.5f+0.5f*sinf(mqT*3.4f);
  uint32_t rm[5];
  for(int k=0;k<5;k++) rm[k]=mixc(GOLDG[k],0xFFFFFF,pulse*lim*(k<2?0.55f:0.25f));
  textb("WILD 7's",FBW/2,py0+3,4,rm,5,1);
  /* shine: a diagonal white band sweeping the sign every couple of seconds */
  float sx=fmodf(mqT*300.0f,(float)(pw+520))-260.0f+px0;
  for(int y=py0+2;y<py0+ph2-2;y++){
    float cx=sx+(y-py0)*0.9f;
    for(int x=(int)(cx-26);x<=(int)(cx+26);x++){
      if(x<px0+2||x>=px0+pw-2) continue;
      float v=(1.0f-fabsf(x-cx)/26.0f)*lim;
      fb_add(x,y,(int)(v*120),(int)(v*110),(int)(v*80));
    }
  }
  fb_rframe(px0,py0,pw,ph2,12,2.5f,mixc(0xF0C24A,0xFFFFFF,pulse*0.5f),255);

  /* 4. two colours of bulb chasing round the edge, faster than before */
  int n=(FBW-40)/26;
  for(int i=0;i<n;i++){
    float phz=mqT*5.0f - i*0.55f;
    float v=0.5f+0.5f*sinf(phz);
    if(opt_limiter) v=0.35f+v*0.5f;
    uint32_t warm=(i&1)?0xFFE9A0:0xFF5A3A, dark=(i&1)?0x5A3A08:0x4A1008;
    uint32_t c=mixc(dark,warm,v);
    int x=22+i*26, a=(int)(90+v*165);
    for(int j=-3;j<=3;j++) for(int k=-3;k<=3;k++)
      if(j*j+k*k<=9){ fb_blend(x+k,MQH-9+j,c,a); if(x<px0-8||x>px0+pw+8) fb_blend(x+k,9+j,c,a); }
    if(v>0.85f){ fb_add(x,MQH-9,60,40,10); }
  }

  /* 5. sparkles: a dozen twinkling crosses wandering the face */
  for(int i=0;i<12;i++){
    float ph=mqT*1.7f+i*2.39f;
    float tw2=sinf(ph*3.1f); if(tw2<0.3f) continue;
    float x=fmodf(i*137.0f + mqT*(18.0f+i*3.0f), (float)(FBW-40))+20.0f;
    float y=14.0f+fmodf(i*11.0f, (float)(MQH-28));
    if(x>px0-10&&x<px0+pw+10) continue;
    int r=1+(int)(tw2*4), a=(int)(tw2*220*lim);
    for(int k=-r;k<=r;k++){ fb_blend((int)x+k,(int)y,0xFFFFFF,a); fb_blend((int)x,(int)y+k,0xFFFFFF,a); }
    fb_blend((int)x,(int)y,0xFFFFFF,255);
  }
}

/* ═══ FEATURE MODULES (unity build) ═══════════════════════════════
 *  Included here so they can use every primitive above; their
 *  prototypes were declared by the headers near game_t.              */
#include "w7_fx.c"
#include "w7_hold.c"
#include "w7_wheel.c"
#include "w7_extra.c"

static void render(void){
  memcpy(fb,bg,sizeof fb);
  draw_marquee();
  if(G.state==ST_HOLD) hold_draw();       /* the bonus owns the reel window */
  else {
    draw_reels();
    hold_draw_cells();                    /* coin values over landed coins  */
    extra_draw_reels();                   /* 7 STRIKE over the reels        */
  }
  draw_features();
  draw_wins();
  draw_parts();
  fx_draw();                              /* world-layer effects            */
  draw_meters();
  draw_overlays();
  if(G.state==ST_WHEEL)  wheel_draw();    /* full-screen feature scenes     */
  if(G.state==ST_GAMBLE) gamble_draw();
  fx_draw_top();                          /* transitions, top-layer effects */
  if(G.flash>0.001f){
    float f=G.flash; if(opt_limiter && f>0.35f) f=0.35f;
    screen_tint(0xFFFFFF,(int)(f*110));
  }
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
                  (!strcmp(e,"minor")?5:(!strcmp(e,"ult")?6:0)))));
  }
  if(!assets_ready){
    build_strips();
    build_sprites();
    build_dome(0);
    build_dome(1);
    build_bg();
    assets_ready=1;
  }
  reset_game();
}
void retro_deinit(void){
  free(ptbg);  ptbg=NULL;
  free(bnbg);  bnbg=NULL;
  free(ptimg[0]); free(ptimg[1]); ptimg[0]=ptimg[1]=NULL;
  for(int i=0;i<TBC;i++){ free(tbc[i].fill); free(tbc[i].out);
                          tbc[i].fill=tbc[i].out=NULL; tbc[i].used=0; }
  free(bnimg); bnimg=NULL;
  for(int i=0;i<NSYM;i++){
    free(sym[i].px);   sym[i].px=NULL;  free(sym[i].rx0);  free(sym[i].rx1);  sym[i].rx0=sym[i].rx1=NULL;
    free(symb[i].px);  symb[i].px=NULL; free(symb[i].rx0); free(symb[i].rx1); symb[i].rx0=symb[i].rx1=NULL;
  }
  free(glowspr.px); glowspr.px=NULL;
  free(washspr.px); washspr.px=NULL;
  for(int i=0;i<2;i++){ free(domespr[i].px); domespr[i].px=NULL; }
  assets_ready=0;
}
unsigned retro_api_version(void){ return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info*info){
  memset(info,0,sizeof(*info));
  info->library_name     = "Wild 7's";
  info->library_version  = "2.0";
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
    if(strstr(buf,"sound=off"))   opt_sound=0;
    if(strstr(buf,"music=off"))   opt_music=0;
    if(strstr(buf,"turbo=on"))    opt_turbo=1;
    if(strstr(buf,"limiter=off")) opt_limiter=0;
    const char*c=strstr(buf,"credits=");
    if(c){ long long v=atoll(c+8); if(v>0 && v<=1000000000LL) G.credits=v; }
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
bool retro_unserialize(const void*d,size_t s){ if(s<sizeof(G)) return false; memcpy(&G,d,sizeof(G)); return true; }

void retro_cheat_reset(void){}
void retro_cheat_set(unsigned i,bool e,const char*c){ (void)i;(void)e;(void)c; }
void retro_set_controller_port_device(unsigned p,unsigned d){ (void)p;(void)d; }

void retro_run(void){
  bool upd=false;
  if(environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&upd) && upd) check_vars();
  poll_input();
  update();
  render();
  audio_frame();
  if(audio_batch_cb) audio_batch_cb(abuf,SPF);
  video_cb(fb,FBW,FBH,FBW*sizeof(uint32_t));
}
