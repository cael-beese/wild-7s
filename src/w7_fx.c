/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_fx.c - the effects system: particles, glows, god rays, screen
 *  shake, title slams, and the win presentation built from them (win
 *  paths, BIG / SUPER / MEGA / EPIC wins, jackpots, the multiplier pop).
 *
 *  Everything expensive is baked once by fx_init(): sixteen rotation
 *  frames of a lit, embossed gold coin at four sizes, glow and star
 *  masks, the polar tables the god rays are sampled from, bubble-lettered
 *  titles and digits, and enlarged copies of every symbol for the win
 *  pop.  At run time an effect is a blit or a table walk, never a square
 *  root per pixel.
 *
 *  Cosmetic state lives here as statics, not in game_t: particles,
 *  emitters, the shake and the transition.  It has its own random
 *  generator, so a shower of coins never moves the game's RNG and a
 *  forced test outcome stays the same whatever was on screen before it.
 *
 *  Render contract: fx_update() and the fx_*_begin / tick / slam calls
 *  are UPDATE code and may spawn.  Every draw here only reads, takes any
 *  randomness from a hash of stable inputs, and writes rows
 *  [clip_y0, clip_y1) only.  fx_post() runs once, after the bands join.
 * ===================================================================== */

/* -- private RNG and hash ------------------------------------------- */
static uint32_t fxRng = 0xC0FFEE11u;
static inline float fxr(void){
  fxRng^=fxRng<<13; fxRng^=fxRng>>17; fxRng^=fxRng<<5;
  return (float)(fxRng&0xFFFFFF)/16777216.0f;
}
static inline float fxrs(void){ return fxr()*2.0f-1.0f; }
static inline uint32_t fxhash(uint32_t x){
  x^=x>>16; x*=0x7FEB352Du; x^=x>>15; x*=0x846CA68Bu; x^=x>>16;
  return x;
}

/* -- packed pixel arithmetic ------------------------------------------
 *  Additive light is the backbone of every effect here, so it gets a
 *  branch-free saturating add over all three channels at once, rather
 *  than unpacking, clamping and repacking each one.                    */
static inline uint32_t px_add(uint32_t x,uint32_t y){
  uint32_t t0=(x^y)&0x808080u, t1=(x&y)&0x808080u;
  x&=0x7F7F7Fu; y&=0x7F7F7Fu; x+=y;
  t1|=t0&x;
  t1=(t1<<1)-(t1>>7);
  return (x^t0)|t1;
}
/* c times k/256, k in 0..256 */
static inline uint32_t px_scale(uint32_t c,int k){
  return ((((c&0xFF00FFu)*(uint32_t)k)>>8)&0xFF00FFu) |
         ((((c&0x00FF00u)*(uint32_t)k)>>8)&0x00FF00u);
}
/* d toward c by a/256, a in 0..256 */
static inline uint32_t px_mix(uint32_t d,uint32_t c,int a){
  uint32_t ia=(uint32_t)(256-a), ua=(uint32_t)a;
  return ((((c&0xFF00FFu)*ua + (d&0xFF00FFu)*ia)>>8)&0xFF00FFu) |
         ((((c&0x00FF00u)*ua + (d&0x00FF00u)*ia)>>8)&0x00FF00u);
}
/* the rows this band may write, intersected with [y0,y1) */
static inline int fx_clip(int*y0,int*y1){
  if(*y0<clip_y0) *y0=clip_y0;
  if(*y1>clip_y1) *y1=clip_y1;
  if(*y0<0) *y0=0;
  if(*y1>FBH) *y1=FBH;
  return *y0<*y1;
}
static uint32_t fx_hue(float h){
  h-=floorf(h); h*=6.0f;
  int i=(int)h; float f=h-i;
  int q=(int)(255*(1-f)), t=(int)(255*f);
  switch(i){
  case 0:  return RGB(255,t,60);
  case 1:  return RGB(q,255,60);
  case 2:  return RGB(60,255,t);
  case 3:  return RGB(60,q,255);
  case 4:  return RGB(t,60,255);
  default: return RGB(255,60,q);
  }
}

/* === BAKED ASSETS ================================================= */
typedef struct {
  int w,h;
  uint32_t *c;           /* colour, straight alpha                    */
  uint8_t  *a;           /* alpha, or the mask of an additive sprite  */
  uint8_t  *g;           /* optional soft glow mask, same size        */
  int16_t  *x0,*x1;      /* per-row span of non-zero alpha            */
  int16_t  *g0,*g1;      /* per-row span of the glow mask (>= 3)      */
  int ox,oy,tw,th;       /* text: where the caps start, their size;
                            pop symbols: the anchor point            */
} fxspr_t;

static void fxs_free(fxspr_t*s){
  free(s->c); free(s->a); free(s->g); free(s->x0); free(s->x1); free(s->g0); free(s->g1);
  memset(s,0,sizeof *s);
}
static int fxs_alloc(fxspr_t*s,int w,int h,int glow){
  fxs_free(s);
  s->w=w; s->h=h;
  s->c =(uint32_t*)calloc((size_t)w*h,4);
  s->a =(uint8_t*) calloc((size_t)w*h,1);
  s->g = glow ? (uint8_t*)calloc((size_t)w*h,1) : NULL;
  s->x0=(int16_t*)malloc((size_t)h*sizeof(int16_t));
  s->x1=(int16_t*)malloc((size_t)h*sizeof(int16_t));
  if(!s->c||!s->a||(glow&&!s->g)||!s->x0||!s->x1){ fxs_free(s); return 0; }
  return 1;
}
static void fxs_spans(fxspr_t*s){
  for(int y=0;y<s->h;y++){
    int a=-1,b=-1;
    const uint8_t*r=s->a+(size_t)y*s->w;
    for(int x=0;x<s->w;x++) if(r[x]){ if(a<0) a=x; b=x; }
    s->x0[y]=(int16_t)(a<0?0:a);
    s->x1[y]=(int16_t)(b<0?-1:b);
  }
}

/* separable box blur of an 8-bit mask, in place */
static void fx_blur8(uint8_t*m,int w,int h,int r){
  if(r<1) return;
  uint16_t*t=(uint16_t*)malloc((size_t)w*h*2);
  if(!t) return;
  int k=r*2+1;
  for(int y=0;y<h;y++){
    int acc=0;
    for(int x=-r;x<=r;x++) acc+=m[y*w+clampi(x,0,w-1)];
    for(int x=0;x<w;x++){
      t[y*w+x]=(uint16_t)(acc/k);
      acc-=m[y*w+clampi(x-r,0,w-1)];
      acc+=m[y*w+clampi(x+r+1,0,w-1)];
    }
  }
  for(int x=0;x<w;x++){
    int acc=0;
    for(int y=-r;y<=r;y++) acc+=t[clampi(y,0,h-1)*w+x];
    for(int y=0;y<h;y++){
      m[y*w+x]=(uint8_t)(acc/k);
      acc-=t[clampi(y-r,0,h-1)*w+x];
      acc+=t[clampi(y+r+1,0,h-1)*w+x];
    }
  }
  free(t);
}

/* straight-alpha "over" into an accumulator */
static inline void fx_over(int*r,int*g,int*b,int*a,uint32_t c,int sa){
  if(sa<=0) return;
  if(sa>255) sa=255;
  int da=*a, oa=sa+da*(255-sa)/255;
  if(oa<=0) return;
  *r=((int)((c>>16)&255)*sa + *r*da*(255-sa)/255)/oa;
  *g=((int)((c>>8)&255)*sa  + *g*da*(255-sa)/255)/oa;
  *b=((int)(c&255)*sa       + *b*da*(255-sa)/255)/oa;
  *a=oa;
}

/* -- the glow table and the god-ray polar tables --------------------- */
#define GLS 256
static uint8_t  *glowT;
#define RYS 512
static uint16_t *rayA;           /* angle, 0..1023                    */
static uint8_t  *rayR;           /* radius, 0..254, 255 = outside     */

/* -- spinning coin -------------------------------------------------- */
#define NCSZ 4
#define NCFR 16
static const int CSZ[NCSZ]={14,20,28,40};
static fxspr_t coinS[NCSZ][NCFR];

/* -- additive star masks -------------------------------------------- */
#define NSTAR 3
static const int STARR[NSTAR]={6,11,20};
static fxspr_t starS[NSTAR];

/* -- enlarged symbols for the win pop ------------------------------- */
#define NPOP 5
static const float POPSC[NPOP]={1.0f,1.045f,1.09f,1.135f,1.18f};
static fxspr_t popS[NSYM][NPOP];

/* -- titles and digits ---------------------------------------------- */
enum { FXT_BIG,FXT_SUPER,FXT_MEGA,FXT_EPIC,
       FXT_JULT,FXT_JMEGA,FXT_JMAJOR,FXT_JMINOR,      /* in JP_* order */
       FXT_JACKPOT,FXT_X2,FXT_X3,FXT_X4,FXT_X5,FXT_BADGE,NFXT };
static fxspr_t titleS[NFXT];
#define FXDPX 12                 /* digit sprites: bubble type at px 12 */
static fxspr_t digS[11];         /* 0-9 and the comma                   */
static fxspr_t trS;              /* the current transition's title      */

/*  The lounge look (Beese's Poker Lounge's ROYAL FLUSH / MONSTER POT):
 *  every celebration title is gold display type - cream to amber, a
 *  warm brown rim, its own glow - and the tiers are told apart by the
 *  neon round them: the halo, the rays, the ring, the edge of the dark
 *  glass panel the count sits on.  The top tiers (EPIC, ULTIMATE) run
 *  all three of the lounge's neons, honey, magenta and cyan.          */
static const uint32_t FX_GOLDG[5]={0xFFF8D8,0xFFE08A,0xFFC24A,0xEE9424,0xB8600E};
static const uint32_t FX_ROSEG[5]={0xFFF6EC,0xFFDC9C,0xFFAA52,0xF2625E,0xB02A6A};   /* EPIC */
static const uint32_t FX_PLATG[5]={0xFFFFFF,0xFFF6DA,0xFFE09A,0xE4AC4C,0x9E6C1E};   /* ULTIMATE */

/* tier accents: big-win tiers 1..4, jackpot tiers in JP_* order; the
   jackpot ones match the ladder's neon on the left rail */
static const uint32_t BWCOL[5]={LZ_HONEY,LZ_HONEY,LZ_MAGENTA,LZ_CYAN,LZ_GOLD};
static const uint32_t JPCOL[NJP]={LZ_CYAN,LZ_MAGENTA,0xFF6A4A,0x5AF4C0};
static const uint32_t FX_NEON3[3]={LZ_HONEY,LZ_MAGENTA,LZ_CYAN};
/* the searchlights: honey, run pale, as a lamp's beam through haze */
#define FX_BEAM 0xFFD68Au
/* the three neons in turn, blended: one lap per 3 units of t */
static uint32_t fx_neon_cycle(float t){
  float h=t-floorf(t/3.0f)*3.0f; int i=(int)h;
  return mixc(FX_NEON3[i%3],FX_NEON3[(i+1)%3],h-i);
}
/* a lounge colour for a firework or a confetti flake, from 0..1 */
static uint32_t fx_lounge_col(float u){
  static const uint32_t C[6]={LZ_HONEY,LZ_MAGENTA,LZ_CYAN,LZ_GOLD,LZ_GREEN,0xFF7AE0};
  return C[clampi((int)(u*6.0f),0,5)];
}

/*  Bubble type baked into a sprite.  textb() composites three passes of
 *  a cached coverage mask straight onto the screen each frame and can
 *  only draw at an integer size, so a title that punches in through a
 *  dozen sizes would re-rasterise its mask every frame.  Here the same
 *  masks are composited once into an RGBA sprite, with two things added
 *  that a per-frame pass could not afford: a bevel lit from the top left
 *  (a blurred copy of the fill used as a height field), and a soft glow
 *  mask for a coloured halo.  The sprite is then blitted at any scale. */
/* the colour of a halo baked under the next fx_bake_text, 0 for none */
static uint32_t fxHalo;
/*  The dark rim round baked type: a warm brown rather than black, so the
 *  gold reads as the lounge's lit brass (the poker game's text_gold).  */
#define FX_RIM 0x2E1404
static void fx_bake_text(fxspr_t*s,const char*str,int px,const uint32_t*st,int ns,int glowR){
  int n=(int)strlen(str);
  if(n<1 || n>=(int)sizeof(tbc[0].str)) return;
  /* The mask is laid out at the caption's real width.  tb_render sets
     Bungee once the lounge fonts are up, and Bungee is much wider than
     the old 5x7 blocks, so n*6*px clipped the last letters off ("MEGI
     JACKPO").  textb_w() is the width textb() itself lays out.        */
  for(;;){
    int wt=textb_w(str,px), pad2=2*(int)(px*0.90f+4.0f);
    if(px<=2) break;
    if(wt+pad2<=TBW && px*7+pad2<=TBH && wt<=FBW-24) break;
    px--;
  }
  int adv=px*6, wtot=textb_w(str,px), capH=px*7;
  tbcache_t*c=tb_render(str,px,n,adv,wtot,capH);
  if(!c) return;
  int bw=c->bw, bh=c->bh, pad=c->pad;
  int so=(int)(px*0.34f); if(so<2) so=2;
  int M=glowR>0?glowR+2:1, W=bw+so+2*M, H=bh+so+2*M;
  if(!fxs_alloc(s,W,H,glowR>0)){ c->used=0; return; }
  if(s->g){
    for(int j=0;j<bh;j++) for(int i=0;i<bw;i++)
      s->g[(size_t)(j+M)*W+(i+M)]=c->out[j*bw+i];
    fx_blur8(s->g,W,H,glowR/2>0?glowR/2:1);
    fx_blur8(s->g,W,H,glowR/2>0?glowR/2:1);
    s->g0=(int16_t*)malloc((size_t)H*sizeof(int16_t));
    s->g1=(int16_t*)malloc((size_t)H*sizeof(int16_t));
    if(s->g0&&s->g1) for(int y=0;y<H;y++){
      int a=-1,b=-1;
      for(int x=0;x<W;x++) if(s->g[(size_t)y*W+x]>=3){ if(a<0) a=x; b=x; }
      s->g0[y]=(int16_t)(a<0?0:a); s->g1[y]=(int16_t)(b<0?-1:b);
    }
  }
  uint8_t*hf=(uint8_t*)malloc((size_t)bw*bh);
  int br=px/4; if(br<1) br=1;
  if(hf){ memcpy(hf,c->fill,(size_t)bw*bh); fx_blur8(hf,bw,bh,br); fx_blur8(hf,bw,bh,br); }
  float gain=(2*br+1)*0.42f/255.0f;
  for(int j=0;j<H;j++) for(int i=0;i<W;i++){
    int li=i-M, lj=j-M, R=0,Gc=0,B=0,A=0;
    int si=li-so, sj=lj-so;
    if(fxHalo && s->g) fx_over(&R,&Gc,&B,&A,fxHalo,s->g[(size_t)j*W+i]*230/255);
    if(si>=0&&si<bw&&sj>=0&&sj<bh) fx_over(&R,&Gc,&B,&A,0x000000,c->out[sj*bw+si]*170/255);
    if(li>=0&&li<bw&&lj>=0&&lj<bh){
      fx_over(&R,&Gc,&B,&A,FX_RIM,c->out[lj*bw+li]);
      int fa=c->fill[lj*bw+li];
      if(fa){
        float t=clampf((float)(lj-pad)/(float)(capH>1?capH-1:1),0,1);
        uint32_t col=ramp(st,ns,t);
        if(t<0.30f) col=mixc(col,0xFFFFFF,(0.30f-t)/0.30f*0.50f);
        if(hf){
          int x0=clampi(li-1,0,bw-1), y0=clampi(lj-1,0,bh-1);
          int x1=clampi(li+1,0,bw-1), y1=clampi(lj+1,0,bh-1);
          float dl=(float)(hf[y0*bw+x0]-hf[y1*bw+x1])*gain;
          if(dl>0) col=mixc(col,0xFFFFFF,clampf(dl,0,0.75f));
          else     col=scalec(col,1.0f+clampf(dl,-0.55f,0));
        }
        fx_over(&R,&Gc,&B,&A,col,fa);
      }
    }
    size_t o=(size_t)j*W+i;
    s->c[o]=RGB(R,Gc,B); s->a[o]=(uint8_t)A;
  }
  s->ox=M+pad; s->oy=M+pad; s->tw=wtot; s->th=capH;
  fxs_spans(s);
  free(hf);
  c->used=0;                      /* release the slot to textb first  */
}

/*  The coin's relief, in face coordinates (radius 1): a bevelled outer
 *  edge, a raised rim, a ring of beads round a sunken field and a
 *  faceted five-pointed star.  Only ever sampled at init.             */
static float fx_coin_h(float u,float v){
  float r=sqrtf(u*u+v*v);
  if(r>=1.0f) return 0.0f;
  if(r>0.92f) return 1.0f-(r-0.92f)/0.08f*0.75f;
  if(r>0.80f) return 1.0f;
  if(r>0.75f) return 0.40f+(r-0.75f)/0.05f*0.60f;
  float h=0.40f, ph=atan2f(v,u);
  /* bead ring */
  const float BA=TAU/22.0f;
  float bk=ph/BA, bf=bk-floorf(bk)-0.5f;
  float al=bf*BA*r, ac=r-0.675f, bd=sqrtf(al*al+ac*ac);
  if(bd<0.042f) h+=0.28f*sqrtf(1.0f-bd/0.042f);
  /* the star: one point straight up; the facets ride up to the spokes */
  float a=ph+TAU*0.25f;
  float sk=a/(TAU/5.0f), sf=fabsf(sk-floorf(sk+0.5f));
  float rs=0.56f-(0.56f-0.24f)*(sf*2.0f);
  if(r<rs) h=0.40f+0.55f*(1.0f-r/rs)*(1.0f-sf*0.9f)+0.12f;
  return h;
}

static void fx_build_coin(int si){
  const int S=4, D=CSZ[si], W=D+4, H=D+4;
  const float R=D*0.5f, T=fmaxf(D*0.10f,1.6f);
  const float Lx=-0.45f, Ly=-0.65f, Lz=0.62f;
  float ln=sqrtf(Lx*Lx+Ly*Ly+Lz*Lz);
  const float lx=Lx/ln, ly=Ly/ln, lz=Lz/ln;
  static const uint32_t GR[6]={0x2A1802,0x7A4A06,0xC8870F,0xF5C430,0xFFEB8A,0xFFFBE0};
  /* the relief's normals, once, in face space */
  int FM=D*S;
  float*fn=(float*)malloc((size_t)FM*FM*3*sizeof(float));
  if(!fn) return;
  float e=2.0f/FM;
  for(int j=0;j<FM;j++) for(int i=0;i<FM;i++){
    float u=(i+0.5f)/FM*2.0f-1.0f, v=(j+0.5f)/FM*2.0f-1.0f;
    float du=(fx_coin_h(u+e,v)-fx_coin_h(u-e,v))/(2*e);
    float dv=(fx_coin_h(u,v+e)-fx_coin_h(u,v-e))/(2*e);
    float nx=-du*0.13f, ny=-dv*0.13f, nz=1.0f, nl=sqrtf(nx*nx+ny*ny+nz*nz);
    float*o=fn+((size_t)j*FM+i)*3;
    o[0]=nx/nl; o[1]=ny/nl; o[2]=nz/nl;
  }
  for(int f=0;f<NCFR;f++){
    fxspr_t*sp=&coinS[si][f];
    if(!fxs_alloc(sp,W,H,0)) continue;
    float th=(f+0.25f)*(3.14159265f/NCFR);
    float c=cosf(th), s=sinf(th), ac=fabsf(c), sv=c>=0?s:-s;
    float ox=sv*T*0.5f, sgn=sv>=0?1.0f:-1.0f;
    for(int j=0;j<H;j++) for(int i=0;i<W;i++){
      float ar=0,ag=0,ab=0; int cov=0;
      for(int b=0;b<S;b++) for(int a=0;a<S;a++){
        float x=i+(a+0.5f)/S-W*0.5f, y=j+(b+0.5f)/S-H*0.5f;
        float yy=y/R;
        if(fabsf(yy)>=1.0f) continue;
        float wy=sqrtf(1.0f-yy*yy)*R*ac;
        int inFace = ac>0.02f && fabsf(x-ox)<=wy;
        int inBand = fabsf(x)<=fabsf(ox)+wy;
        if(!inFace && !inBand) continue;
        float Nx,Ny,Nz; uint32_t col;
        if(inFace){
          float u=(x-ox)/(ac*R), v=yy;
          int fi=clampi((int)((u*0.5f+0.5f)*FM),0,FM-1), fj=clampi((int)((v*0.5f+0.5f)*FM),0,FM-1);
          const float*n=fn+((size_t)fj*FM+fi)*3;
          Nx=n[2]*sv+n[0]*ac; Ny=n[1]; Nz=n[2]*ac-n[0]*sv;
        } else {
          float cp=sqrtf(1.0f-yy*yy);
          Nx=-sgn*ac*cp; Ny=yy; Nz=fabsf(sv)*cp;
        }
        float lam=fmaxf(0.0f,Nx*lx+Ny*ly+Nz*lz);
        float Rx=2*Nz*Nx, Ry=2*Nz*Ny, Rz=2*Nz*Nz-1.0f;
        float env=clampf(0.5f-0.62f*Ry,0,1);
        float spec=fmaxf(0.0f,Rx*lx+Ry*ly+Rz*lz);
        spec=powf(spec,26.0f);
        if(inFace){
          col=ramp(GR,6,clampf(0.08f+0.52f*lam+0.42f*env,0,1));
          col=mixc(col,0xFFFFFF,clampf(spec*0.95f,0,1));
        } else {
          float reed=0.5f+0.5f*cosf(asinf(yy)*60.0f);
          col=ramp(GR,6,clampf((0.10f+0.45f*lam+0.25f*env)*(0.72f+0.28f*reed),0,1));
        }
        ar+=(col>>16)&255; ag+=(col>>8)&255; ab+=col&255; cov++;
      }
      if(!cov) continue;
      size_t o=(size_t)j*W+i;
      sp->c[o]=RGB((int)(ar/cov),(int)(ag/cov),(int)(ab/cov));
      sp->a[o]=(uint8_t)(cov*255/(S*S));
    }
    fxs_spans(sp);
  }
  free(fn);
}

/* four-point sparkle with a soft core and short diagonal glints */
static void fx_build_star(fxspr_t*s,int r){
  int W=2*r+1;
  if(!fxs_alloc(s,W,W,0)) return;
  for(int y=0;y<W;y++) for(int x=0;x<W;x++){
    float dx=(x-r)/(float)r, dy=(y-r)/(float)r;
    float d=sqrtf(dx*dx+dy*dy);
    float core=fmaxf(0.0f,1.0f-d*2.4f); core*=core;
    float halo=fmaxf(0.0f,1.0f-d); halo=halo*halo*halo*0.30f;
    float th=1.1f/r;
    float sx=powf(fmaxf(0.0f,1.0f-fabsf(dx)),2.0f)*fmaxf(0.0f,1.0f-fabsf(dy)/th);
    float sy=powf(fmaxf(0.0f,1.0f-fabsf(dy)),2.0f)*fmaxf(0.0f,1.0f-fabsf(dx)/th);
    float u=(dx+dy)*0.7071f, v=(dx-dy)*0.7071f;
    float d1=powf(fmaxf(0.0f,1.0f-fabsf(u)*1.9f),2.0f)*fmaxf(0.0f,1.0f-fabsf(v)/th);
    float d2=powf(fmaxf(0.0f,1.0f-fabsf(v)*1.9f),2.0f)*fmaxf(0.0f,1.0f-fabsf(u)/th);
    float val=clampf(core+halo+sx+sy+0.45f*(d1+d2),0,1);
    s->a[y*W+x]=(uint8_t)(val*255);
    s->c[y*W+x]=0xFFFFFF;
  }
  fxs_spans(s);
}

/*  An enlarged copy of a symbol sprite, bilinear and premultiplied, so
 *  the winning symbols can swell out of their cells without a scaler
 *  running per frame.  Scaled about the art's centre, which is recorded
 *  as the anchor.                                                     */
static void fx_build_pop(fxspr_t*d,const spr_t*s,float k){
  if(!s->px) return;
  float ax=SYMW*0.5f, ay=SYMH*0.5f;
  int W=(int)ceilf(s->w*k)+2, H=(int)ceilf(s->h*k)+2;
  float cx=ax*k+1.0f, cy=ay*k+1.0f;
  if(!fxs_alloc(d,W,H,0)) return;
  for(int j=0;j<H;j++) for(int i=0;i<W;i++){
    float sx=(i+0.5f-cx)/k+ax-0.5f, sy=(j+0.5f-cy)/k+ay-0.5f;
    int x0=(int)floorf(sx), y0=(int)floorf(sy);
    float fx=sx-x0, fy=sy-y0, r=0,g=0,b=0,a=0;
    for(int q=0;q<4;q++){
      int xx=x0+(q&1), yy=y0+(q>>1);
      if(xx<0||yy<0||xx>=s->w||yy>=s->h) continue;
      float wgt=((q&1)?fx:1-fx)*((q>>1)?fy:1-fy);
      const uint8_t*p=s->px+((size_t)yy*s->w+xx)*4;
      float pa=p[3]*wgt;
      r+=p[0]*pa; g+=p[1]*pa; b+=p[2]*pa; a+=pa;
    }
    if(a<0.5f) continue;
    size_t o=(size_t)j*W+i;
    d->c[o]=RGB((int)(r/a),(int)(g/a),(int)(b/a));
    d->a[o]=(uint8_t)clampi((int)(a+0.5f),0,255);
  }
  d->ox=(int)cx; d->oy=(int)cy;
  fxs_spans(d);
}

/* === BLITTERS ===================================================== */
#define FX_NOSHINE (-99999.0f)

/*  Scaled RGBA blit, nearest sample, (X,Y) = where the sprite's top-left
 *  lands.  `white` (0..256) washes the art toward white for a slam
 *  flash; `shine` is the x of a diagonal light sweep, FX_NOSHINE for
 *  none.  Only each row's occupied span is visited.                   */
static void fxs_blit(const fxspr_t*s,float X,float Y,float sc,int alpha,int white,float shine){
  if(!s->c||!s->a||sc<=0.01f||alpha<=0) return;
  if(alpha>255) alpha=255;
  int a256=alpha+(alpha>>7);
  float dh=s->h*sc;
  int y0=(int)floorf(Y), y1=(int)ceilf(Y+dh);
  if(!fx_clip(&y0,&y1)) return;
  float inv=1.0f/sc;
  int step=(int)(inv*65536.0f);
  float shw=30.0f*sc;
  for(int y=y0;y<y1;y++){
    int sy=(int)((y+0.5f-Y)*inv);
    if(sy<0||sy>=s->h) continue;
    int sxa=s->x0[sy], sxb=s->x1[sy];
    if(sxb<sxa) continue;
    int dxa=(int)floorf(X+sxa*sc), dxb=(int)ceilf(X+(sxb+1)*sc);
    if(dxa<0) dxa=0;
    if(dxb>FBW) dxb=FBW;
    if(dxa>=dxb) continue;
    const uint32_t*cr=s->c+(size_t)sy*s->w;
    const uint8_t *ar=s->a+(size_t)sy*s->w;
    uint32_t*d=fb+(size_t)y*FBW;
    int fx=(int)((dxa+0.5f-X)*inv*65536.0f);
    for(int x=dxa;x<dxb;x++,fx+=step){
      int sx=fx>>16;
      if((unsigned)sx>=(unsigned)s->w) continue;
      int a=ar[sx];
      if(!a) continue;
      uint32_t c=cr[sx];
      if(white>0) c=px_mix(c,0xFFFFFF,white);
      a=(a*a256)>>8;
      d[x]= a>=255 ? c : px_mix(d[x],c,a+(a>>7));
    }
    if(shine>FX_NOSHINE+1.0f){
      float bx=shine+(y-Y)*0.45f;
      int xa=(int)(bx-shw), xb=(int)(bx+shw);
      if(xa<dxa) xa=dxa;
      if(xb>dxb) xb=dxb;
      for(int x=xa;x<xb;x++){
        int sx=(int)((x+0.5f-X)*inv);
        if((unsigned)sx>=(unsigned)s->w) continue;
        int a=ar[sx];
        if(!a) continue;
        float k=1.0f-fabsf(x-bx)/shw;
        int v=(int)(k*k*a*alpha/255*0.75f);
        if(v>0) d[x]=px_add(d[x],px_scale(0xFFFFFF,v));
      }
    }
  }
}
/* the 1:1 case, at an integer position: coins and popped symbols */
static void fxs_blit1(const fxspr_t*s,int X,int Y,int alpha){
  if(!s->c||alpha<=0) return;
  if(alpha>255) alpha=255;
  int a256=alpha+(alpha>>7);
  int y0=Y, y1=Y+s->h;
  if(!fx_clip(&y0,&y1)) return;
  for(int y=y0;y<y1;y++){
    int sy=y-Y, xa=s->x0[sy], xb=s->x1[sy];
    if(X+xa<0) xa=-X;
    if(X+xb>=FBW) xb=FBW-1-X;
    const uint32_t*cr=s->c+(size_t)sy*s->w;
    const uint8_t *ar=s->a+(size_t)sy*s->w;
    uint32_t*d=fb+(size_t)y*FBW+X;
    for(int x=xa;x<=xb;x++){
      int a=ar[x];
      if(!a) continue;
      if(a256>=256 && a==255){ d[x]=cr[x]; continue; }
      a=(a*a256)>>8;
      d[x]=px_mix(d[x],cr[x],a+(a>>7));
    }
  }
}
/* additive glow mask of a sprite, same mapping as fxs_blit */
static void fxs_glow(const fxspr_t*s,float X,float Y,float sc,uint32_t col,int alpha){
  if(!s->g||!s->g0||sc<=0.01f||alpha<=0) return;
  if(alpha>255) alpha=255;
  float dh=s->h*sc;
  int y0=(int)floorf(Y), y1=(int)ceilf(Y+dh);
  if(!fx_clip(&y0,&y1)) return;
  float inv=1.0f/sc;
  int step=(int)(inv*65536.0f);
  for(int y=y0;y<y1;y++){
    int sy=(int)((y+0.5f-Y)*inv);
    if(sy<0||sy>=s->h) continue;
    int sxa=s->g0[sy], sxb=s->g1[sy];
    if(sxb<sxa) continue;
    int x0=(int)floorf(X+sxa*sc), x1=(int)ceilf(X+(sxb+1)*sc);
    if(x0<0) x0=0;
    if(x1>FBW) x1=FBW;
    const uint8_t*gr=s->g+(size_t)sy*s->w;
    uint32_t*d=fb+(size_t)y*FBW;
    int fx=(int)((x0+0.5f-X)*inv*65536.0f);
    for(int x=x0;x<x1;x++,fx+=step){
      int sx=fx>>16;
      if((unsigned)sx>=(unsigned)s->w) continue;
      int m=gr[sx];
      if(m<3) continue;
      d[x]=px_add(d[x],px_scale(col,(m*alpha)>>8));
    }
  }
}
/* additive mask at 1:1, centred on (cx,cy) */
static void fxs_add(const fxspr_t*s,int cx,int cy,uint32_t col,int alpha){
  if(!s->a||alpha<=0) return;
  if(alpha>255) alpha=255;
  int X=cx-s->w/2, Y=cy-s->h/2, y0=Y, y1=Y+s->h;
  if(!fx_clip(&y0,&y1)) return;
  for(int y=y0;y<y1;y++){
    int sy=y-Y, xa=s->x0[sy], xb=s->x1[sy];
    if(X+xa<0) xa=-X;
    if(X+xb>=FBW) xb=FBW-1-X;
    const uint8_t*ar=s->a+(size_t)sy*s->w;
    uint32_t*d=fb+(size_t)y*FBW+X;
    for(int x=xa;x<=xb;x++){
      int m=ar[x];
      if(m) d[x]=px_add(d[x],px_scale(col,(m*alpha)>>8));
    }
  }
}

/*  An additive mask with its top-left at (X,Y), skipping each row's
 *  empty middle [g0,g1] when the sprite records one: a neon frame is
 *  light round the edge of a panel and nothing across its width.     */
static void fxs_addmask(const fxspr_t*s,int X,int Y,uint32_t col,int alpha){
  if(!s->a||alpha<=0) return;
  if(alpha>255) alpha=255;
  int y0=Y, y1=Y+s->h;
  if(!fx_clip(&y0,&y1)) return;
  for(int y=y0;y<y1;y++){
    int sy=y-Y, xa=s->x0[sy], xb=s->x1[sy];
    int ha=s->g0?s->g0[sy]:1, hb=s->g1?s->g1[sy]:0;
    if(X+xa<0) xa=-X;
    if(X+xb>=FBW) xb=FBW-1-X;
    const uint8_t*ar=s->a+(size_t)sy*s->w;
    uint32_t*d=fb+(size_t)y*FBW+X;
    for(int x=xa;x<=xb;x++){
      if(x==ha && hb>=ha){ x=hb; continue; }
      int m=ar[x];
      if(m) d[x]=px_add(d[x],px_scale(col,(m*alpha)>>8));
    }
  }
}

/* === LOUNGE GLASS PANELS ==========================================
 *  The dark glass plate a count sits on, with a neon tube round its edge
 *  and the tube's glow spilling both ways (the poker game's JACKPOT PAYS
 *  panel).  lz_glass() paints through the rasteriser and allocates, so
 *  it is build-time only; these are baked once at init instead, as two
 *  sprites: the glass (colour and alpha) and the neon (a white additive
 *  mask), so one bake serves every tier's colour and fades at any
 *  alpha.  Per frame that is one copy over the plate and one add
 *  round its rim.                                                     */
typedef struct { fxspr_t body, neon; int w,h,m; float r; } fxpanel_t;
static void fx_rframe(int x,int y,int w,int h,float r,int t,uint32_t col,int alpha);
enum { FXP_WIN, FXP_MULT, FXP_SUB, NFXP };
static fxpanel_t fxPanel[NFXP];

/* signed distance to a rounded box of half-size (hw,hh), corner radius r */
static float fx_rr_sd(float px,float py,float hw,float hh,float r){
  float qx=fabsf(px)-(hw-r), qy=fabsf(py)-(hh-r);
  float ox=fmaxf(qx,0.0f), oy=fmaxf(qy,0.0f);
  return sqrtf(ox*ox+oy*oy)+fminf(fmaxf(qx,qy),0.0f)-r;
}
static void fx_bake_panel(fxpanel_t*P,int w,int h,float r){
  const int M=22;
  int W=w+2*M, H=h+2*M;
  P->w=w; P->h=h; P->m=M; P->r=r;
  if(!fxs_alloc(&P->body,W,H,0) || !fxs_alloc(&P->neon,W,H,0)) return;
  float hw=w*0.5f, hh=h*0.5f;
  for(int j=0;j<H;j++) for(int i=0;i<W;i++){
    float px=i+0.5f-W*0.5f, py=j+0.5f-H*0.5f;
    float d=fx_rr_sd(px,py,hw,hh,r);
    size_t o=(size_t)j*W+i;
    /* the glass: plum-black, a shade lighter at the top, with a soft
       sheen across its upper third.  Opaque, as the poker game's panels
       read, which also makes the per-frame blit a plain copy. */
    float cov=clampf(0.5f-d,0,1);
    if(cov>0){
      float v=clampf((py+hh)/(float)h,0,1);
      uint32_t c=mixc(0x241A2C,0x0A070D,v);
      if(v<0.38f) c=mixc(c,0x3C2E46,(0.38f-v)/0.38f*0.30f);
      P->body.c[o]=c;
      P->body.a[o]=(uint8_t)(cov*255.0f+0.5f);
    }
    /* the neon: a tube 1.5 px inside the edge, and its glow, wider and
       brighter outside than in, fading to nothing at the margin */
    float tube=clampf(2.3f-fabsf(d+1.5f),0,1);
    float glow=d>0 ? 0.85f*expf(-d/7.0f)*clampf(1.0f-d/(float)M,0,1)
                   : 0.45f*expf(d/5.0f);
    float v=fmaxf(tube,glow);
    P->neon.a[o]=(uint8_t)(v*255.0f+0.5f);
    P->neon.c[o]=0xFFFFFF;
  }
  fxs_spans(&P->body);
  fxs_spans(&P->neon);
  /* the neon's dark middle, per row, so the add can step over it */
  P->neon.g0=(int16_t*)malloc((size_t)H*sizeof(int16_t));
  P->neon.g1=(int16_t*)malloc((size_t)H*sizeof(int16_t));
  if(P->neon.g0 && P->neon.g1) for(int j=0;j<H;j++){
    const uint8_t*row=P->neon.a+(size_t)j*W;
    int a=W/2, b=W/2;
    if(row[a]){ P->neon.g0[j]=1; P->neon.g1[j]=0; continue; }
    while(a>0 && !row[a-1]) a--;
    while(b<W-1 && !row[b+1]) b++;
    P->neon.g0[j]=(int16_t)a; P->neon.g1[j]=(int16_t)b;
  }
}
/* the panel with its glass at (x,y,w,h), its tube in `neon` */
static void fx_panel_draw(const fxpanel_t*P,int x,int y,uint32_t neon,int alpha){
  if(!P->body.c||alpha<=0) return;
  fxs_blit1(&P->body,x-P->m,y-P->m,alpha);
  fxs_addmask(&P->neon,x-P->m,y-P->m,neon,alpha);
  /* the tube's pastel-hot core, as a neon sign's glass is */
  fx_rframe(x+1,y+1,P->w-2,P->h-2,P->r-1.0f,2,lz_hot(neon,0.62f),alpha*3/4);
}

/* === LOUNGE TYPE ================================================== */
/* small spaced capitals (Barlow), centred on cx with the line top at y */
static void fx_caption(const char*s,float cx,float y,float size,uint32_t col,float op,float spacing){
  if(op<=0.02f) return;
  lz_style st;
  memset(&st,0,sizeof st);
  st.color=col; st.align=LZ_CENTER; st.spacing=spacing;
  st.shadow=0x000000; st.shadow_k=0.85f;
  st.opacity=clampf(op,0,1);
  lz_text_ex(LZF_UI_M,s,cx,y,size,&st);
}
/* a neon sign (Tilt Neon) centred on (cx,cy), fading with op: the tube
   runs pastel-hot and the halo is the tube's colour */
static void fx_neon_sign(const char*s,float cx,float cy,float size,uint32_t tube,float op){
  if(op<=0.02f) return;
  lz_style st;
  memset(&st,0,sizeof st);
  st.color=lz_hot(tube,0.62f); st.align=LZ_CENTER;
  st.glow=tube; st.glow_k=0.95f;
  st.shadow=0x000000; st.shadow_k=0.9f;     /* the wall behind the sign */
  st.opacity=clampf(op,0,1);
  lz_text_ex(size>40.0f?LZF_NEON_L:LZF_NEON_M,s,cx,cy-size*0.5f,size,&st);
}
/* PRESS ANY BUTTON prompts: Barlow, widely spaced, breathing softly (a
   steady glow under the limiter, which forbids the old blink) */
static void fx_prompt(const char*s,float y,float t){
  float op=opt_limiter?0.9f:0.62f+0.38f*sinf(t*4.2f);
  fx_caption(s,FBW*0.5f,y,26.0f,LZ_IVORY,op,4.0f);
}

/* === DRAW HELPERS ================================================= */
static void fx_glow_ell(int cx,int cy,int rx,int ry,uint32_t col,int alpha,int blend){
  if(!glowT||rx<1||ry<1||alpha<=0) return;
  if(alpha>255) alpha=255;
  int y0=cy-ry, y1=cy+ry;
  if(!fx_clip(&y0,&y1)) return;
  int x0=cx-rx, x1=cx+rx;
  if(x0<0) x0=0;
  if(x1>FBW) x1=FBW;
  if(x0>=x1) return;
  int sx=(GLS<<16)/(2*rx), sy=(GLS<<16)/(2*ry);
  for(int y=y0;y<y1;y++){
    int ty=((y-(cy-ry))*sy+sy/2)>>16;
    if((unsigned)ty>=GLS) continue;
    const uint8_t*row=glowT+ty*GLS;
    uint32_t*d=fb+(size_t)y*FBW;
    int tx=(x0-(cx-rx))*sx+sx/2;
    for(int x=x0;x<x1;x++,tx+=sx){
      int m=row[tx>>16];
      if(!m) continue;
      int k=(m*alpha)>>8;
      d[x] = blend ? px_mix(d[x],col,k) : px_add(d[x],px_scale(col,k));
    }
  }
}
static void fx_glow(int cx,int cy,int r,uint32_t col,int alpha){ fx_glow_ell(cx,cy,r,r,col,alpha,0); }
static void fx_shade(int cx,int cy,int rx,int ry,uint32_t col,int alpha){ fx_glow_ell(cx,cy,rx,ry,col,alpha,1); }

/*  God rays, sampled from polar tables baked at init: per pixel an angle
 *  lookup, a profile lookup and a radial lookup.  Rays are soft, so one
 *  sample feeds two pixels across.  `core` adds a radial glow in the
 *  same pass, which saves drawing a separate glow under the burst.    */
static void fx_rays_ex(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,
                       int alpha,float ysc,int rainbow,int core){
  if(!rayA||r1<8||alpha<=0||n<1) return;
  if(alpha>255) alpha=255;
  int ry=(int)(r1*ysc); if(ry<4) ry=4;
  int y0=cy-ry, y1=cy+ry;
  if(!fx_clip(&y0,&y1)) return;
  int x0=cx-r1, x1=cx+r1;
  if(x0<0) x0=0;
  if(x1>FBW) x1=FBW;
  if(x0>=x1) return;
  uint8_t prof[1024]; uint32_t pc[1024];
  float fn=(float)n;
  for(int a=0;a<1024;a++){
    float f=a*fn/1024.0f; int k=(int)f;
    float d=fabsf((f-k)-0.5f)*2.0f;
    float v=1.0f-d*1.30f; if(v<0) v=0;
    v=v*v*(3.0f-2.0f*v);
    if(k&1) v*=0.55f;
    prof[a]=(uint8_t)(v*255.0f);
    pc[a]=rainbow?fx_hue((float)k/fn+ang*0.05f):col;
  }
  int rot=((int)floorf(ang*(1024.0f/TAU)))&1023;
  int rl[256], cl[256];
  float rin=(float)r0/(float)r1;
  for(int i=0;i<256;i++){
    float t=i/255.0f, v;
    if(t<rin) { v=rin>0.0f?t/rin:1.0f; v*=v; }
    else { v=1.0f-(t-rin)/(1.0f-rin+1e-4f); v=v*v; }
    rl[i]=(int)(v*alpha);
    float g=1.0f-t; cl[i]=core>0?(int)(g*g*g*core):0;
  }
  int half=RYS/2;
  int sx=(half<<16)/r1, sy=(half<<16)/ry;
  for(int y=y0;y<y1;y++){
    int ty=half+(((y-cy)*sy)>>16);
    if((unsigned)ty>=RYS) continue;
    const uint16_t*ar=rayA+(size_t)ty*RYS;
    const uint8_t *rr=rayR+(size_t)ty*RYS;
    uint32_t*d=fb+(size_t)y*FBW;
    int tx=(half<<16)+(x0-cx)*sx;
    for(int x=x0;x<x1;x+=2,tx+=2*sx){
      int ix=tx>>16;
      if((unsigned)ix>=RYS) continue;
      int r=rr[ix];
      if(r==255) continue;
      int ai=(ar[ix]+rot)&1023;
      int v=((prof[ai]*rl[r])>>8)+cl[r];
      if(v<=2) continue;
      if(v>256) v=256;
      uint32_t c=px_scale(rainbow?pc[ai]:col,v);
      d[x]=px_add(d[x],c);
      if(x+1<x1) d[x+1]=px_add(d[x+1],c);
    }
  }
}
static void fx_rays(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,int alpha){
  fx_rays_ex(cx,cy,r0,r1,n,ang,col,alpha,1.0f,0,0);
}

/*  The stage every celebration plays on, in ONE pass over the frame:
 *  an optional uniform dim, a darker elliptical plate behind the title,
 *  a god-ray burst with a glow core, and the lounge's searchlights - two
 *  soft beams swinging down from the top corners onto the title.  Drawn
 *  as separate effects that was several full-screen read-modify-writes,
 *  which is most of what a frame costs.  Here the darkening factor and
 *  the light are worked out once per 4x2 block and applied as one
 *  multiply and one add; with no uniform dim only the spans that are
 *  actually lit (the ellipse and the two cones) are visited.          */
typedef struct {
  int cx,cy;                       /* burst and plate centre            */
  int dim;                         /* uniform darkening, 0..255         */
  int vrx,vry,vig;                 /* plate radii, its extra darkening  */
  int r0,r1,n,alpha,rainbow,core;  /* rays, as fx_rays_ex; rainbow 2 =
                                      the lounge's three neons in turn  */
  float ang,ysc;
  uint32_t col;
  int beam;                        /* searchlight strength, 0 = none    */
  float bt;                        /* the searchlights' sweep clock, s  */
  uint32_t bcol[2];                /* left and right beam colours       */
  int hx0,hx1,hy0,hy1;             /* an opaque panel on top: its rows
                                      and columns (4-aligned) are left
                                      alone, 0,0 for none             */
} fxstage_t;

/* one searchlight: origin, axis, its normal, tan of the half angle,
   reach, and the two edges' slopes (dx per dy) for the row spans      */
typedef struct { float ox,oy,dx,dy,nx,ny,tw,L,s1,s2; int ok; uint32_t col; } fxbeam_t;

static void fx_beam_setup(fxbeam_t*b,float ox,float oy,float ax,float ay,uint32_t col){
  float dx=ax-ox, dy=ay-oy, l=sqrtf(dx*dx+dy*dy);
  memset(b,0,sizeof *b);
  if(l<1.0f) return;
  b->ox=ox; b->oy=oy; b->dx=dx/l; b->dy=dy/l; b->nx=-b->dy; b->ny=b->dx;
  b->tw=0.12f; b->L=l*1.05f; b->col=col;
  /* the cone's edges, the axis turned by +-atan(tw); both point down */
  float c=1.0f/sqrtf(1.0f+b->tw*b->tw), s=b->tw*c;
  float e1x=b->dx*c-b->dy*s, e1y=b->dx*s+b->dy*c;
  float e2x=b->dx*c+b->dy*s, e2y=-b->dx*s+b->dy*c;
  if(e1y<0.05f||e2y<0.05f) return;
  b->s1=e1x/e1y; b->s2=e2x/e2y; b->ok=1;
}
/*  The searchlights' light at (xs,yc): the colour to add (*out) and the
 *  strength it takes from what is under it (returned).  Each cone is a
 *  flat top with soft edges, so it reads as a beam and not a smudge,
 *  with a brighter core down its axis and a fade along its reach.  A
 *  point outside a cone's columns on this row [bx0,bx1) skips it.    */
static int fx_beam_light(const fxbeam_t*bm,int nb,const int*bx0,const int*bx1,
                         float xs,float yc,int bA,uint32_t*out){
  uint32_t c=0; int D=0;
  for(int i=0;i<nb;i++){
    if(xs<bx0[i] || xs>=bx1[i]) continue;
    const fxbeam_t*b=&bm[i];
    float px=xs-b->ox, py=yc-b->oy;
    float al=px*b->dx+py*b->dy;
    if(al<=1.0f || al>=b->L) continue;
    float qq=(px*b->nx+py*b->ny)/(al*b->tw);
    if(qq<=-1.0f || qq>=1.0f) continue;
    float q2=qq*qq, e=1.0f-q2*q2, core=1.0f-q2;
    core*=core; core*=core;
    int v=(int)((0.62f*e+0.38f*core)*(1.0f-0.75f*al/b->L)*bA);
    if(v>2){ c=px_add(c,px_scale(b->col,v)); D+=v; }
  }
  *out=c;
  return D;
}

static void fx_stage_draw(const fxstage_t*s){
  int rays=(s->alpha>0||s->core>0)&&rayA&&s->r1>=8&&s->n>0;
  int vig=s->vig>0&&s->vrx>0&&s->vry>0;
  int dimk=clampi(s->dim,0,255);
  fxbeam_t bm[2]; int nb=0;
  if(s->beam>0){
    /* the lights swing slowly and out of step, as if worked by hand */
    float sw=sinf(s->bt*0.55f), sw2=sinf(s->bt*0.47f+2.1f);
    fxbeam_t l, r;
    fx_beam_setup(&l,70.0f,-220.0f,s->cx-120.0f+130.0f*sw,s->cy+70.0f,s->bcol[0]);
    fx_beam_setup(&r,FBW-70.0f,-220.0f,s->cx+120.0f+130.0f*sw2,s->cy+70.0f,s->bcol[1]);
    if(l.ok) bm[nb++]=l;
    if(r.ok) bm[nb++]=r;
  }
  if(!rays&&!vig&&!dimk&&!nb) return;
  int ry=(int)(s->r1*s->ysc); if(ry<4) ry=4;
  /* the rows anything happens in */
  int y0=0, y1=FBH;
  if(!dimk){
    int ey=0;
    if(rays) ey=ry;
    if(vig && s->vry>ey) ey=s->vry;
    y0=s->cy-ey; y1=s->cy+ey;
    for(int i=0;i<nb;i++){
      int by=(int)(bm[i].oy+bm[i].L*bm[i].dy)+8;
      if(y0>0) y0=0;
      if(by>y1) y1=by;
    }
  }
  if(!fx_clip(&y0,&y1)) return;
  uint8_t prof[1024]; uint32_t pc[1024]; int rl[256], cl[256];
  int half=RYS/2, rsx=0, rsy=0, rot=0;
  if(rays){
    float fn=(float)s->n;
    for(int a=0;a<1024;a++){
      float f=a*fn/1024.0f; int k=(int)f;
      float d=fabsf((f-k)-0.5f)*2.0f, v=1.0f-d*1.30f;
      if(v<0) v=0;
      v=v*v*(3.0f-2.0f*v);
      if(k&1) v*=0.55f;
      prof[a]=(uint8_t)(v*255.0f);
      pc[a]=s->rainbow==2?FX_NEON3[k%3]:s->rainbow?fx_hue((float)k/fn+s->ang*0.05f):s->col;
    }
    rot=((int)floorf(s->ang*(1024.0f/TAU)))&1023;
    float rin=(float)s->r0/(float)s->r1;
    for(int i=0;i<256;i++){
      float t=i/255.0f, v;
      if(t<rin){ v=rin>0.0f?t/rin:1.0f; v*=v; }
      else { v=1.0f-(t-rin)/(1.0f-rin+1e-4f); v=sqrtf(v)*v; }
      rl[i]=(int)(v*clampi(s->alpha,0,255));
      float g=1.0f-t; cl[i]=(int)(g*g*g*clampi(s->core,0,255));
    }
    rsx=(half<<16)/s->r1; rsy=(half<<16)/ry;
  }
  int bA=clampi(s->beam,0,255);
  float irx2=vig?1.0f/((float)s->vrx*s->vrx):0, iry2=vig?1.0f/((float)s->vry*s->vry):0;
  /* per-pixel lanes, refilled every other row from one sample per 4x2
     block: the light and the plate are soft, and the apply loop below is
     then a plain lane-wise multiply-add the compiler can vectorise.  A
     row pair works on up to three spans (the ellipse and the cones),
     4-aligned and merged where they meet. */
  uint32_t kx[FBW], cx[FBW];
  int sp[6][2], ns=0, bx0[2]={1,1}, bx1[2]={0,0}, bdv=0;
  uint32_t bc=0;
  for(int y=y0;y<y1;y++){
    if(y==y0 || !(y&1)){
      float yc=(float)(y+1), dy=yc-s->cy;
      /* each cone's columns on this pair of rows; a block outside them
         skips that beam's arithmetic */
      for(int i=0;i<nb;i++){
        float t=yc-bm[i].oy;
        bx0[i]=1; bx1[i]=0;
        if(t>bm[i].L*bm[i].dy+8.0f) continue;
        float a=bm[i].ox+t*bm[i].s1, b=bm[i].ox+t*bm[i].s2;
        if(a>b){ float q=a; a=b; b=q; }
        bx0[i]=(int)fmaxf(a,-8.0f)-4; bx1[i]=(int)fminf(b,FBW+8.0f)+8;
      }
      ns=0;
      if(dimk){ sp[0][0]=0; sp[0][1]=FBW; ns=1; }
      else {
        /* the widest chord of the lit ellipses on this pair of rows */
        float w=-1.0f;
        if(rays && fabsf(dy)<ry){ float q=dy/ry; w=s->r1*sqrtf(1.0f-q*q); }
        if(vig && fabsf(dy)<s->vry){ float q=dy/s->vry, v=s->vrx*sqrtf(1.0f-q*q); if(v>w) w=v; }
        int iv[3][2], ni=0;
        if(w>=0){ iv[ni][0]=s->cx-(int)w-4; iv[ni][1]=s->cx+(int)w+8; ni++; }
        for(int i=0;i<nb;i++)
          if(bx1[i]>bx0[i]){ iv[ni][0]=bx0[i]; iv[ni][1]=bx1[i]; ni++; }
        /* align, clamp, sort and merge */
        for(int i=0;i<ni;i++){
          int a=iv[i][0]&~3, b=(iv[i][1]+3)&~3;
          if(a<0) a=0;
          if(b>FBW) b=FBW;
          if(a>=b) continue;
          int k=ns;
          while(k>0 && sp[k-1][0]>a){ sp[k][0]=sp[k-1][0]; sp[k][1]=sp[k-1][1]; k--; }
          sp[k][0]=a; sp[k][1]=b; ns++;
        }
        int m=0;
        for(int i=0;i<ns;i++){
          if(m && sp[i][0]<=sp[m-1][1]){ if(sp[i][1]>sp[m-1][1]) sp[m-1][1]=sp[i][1]; }
          else { sp[m][0]=sp[i][0]; sp[m][1]=sp[i][1]; m++; }
        }
        ns=m;
      }
      /* an opaque panel drawn over the stage hides whatever the stage
         would light under it: cut its straight-sided rows out */
      if(s->hx1>s->hx0 && y>=s->hy0 && y+1<s->hy1){
        int m=0, t[6][2];
        for(int i=0;i<ns;i++){
          int a=sp[i][0], b=sp[i][1];
          if(b<=s->hx0 || a>=s->hx1){ t[m][0]=a; t[m][1]=b; m++; continue; }
          if(a<s->hx0){ t[m][0]=a; t[m][1]=s->hx0; m++; }
          if(b>s->hx1){ t[m][0]=s->hx1; t[m][1]=b; m++; }
        }
        for(int i=0;i<m;i++){ sp[i][0]=t[i][0]; sp[i][1]=t[i][1]; }
        ns=m;
      }
      float vy=vig?1.0f-dy*dy*iry2:0.0f;
      const uint16_t*ar=NULL; const uint8_t*rr=NULL;
      if(rays && y>=s->cy-ry && y<s->cy+ry){
        int ty=half+(((y-s->cy)*rsy)>>16);
        if((unsigned)ty<RYS){ ar=rayA+(size_t)ty*RYS; rr=rayR+(size_t)ty*RYS; }
      }
      for(int q=0;q<ns;q++) for(int x=sp[q][0];x<sp[q][1];x+=4){
        int D=dimk, xs=x+2;
        if(vy>0){
          float dx=(float)(xs-s->cx), e=vy-dx*dx*irx2;
          if(e>0) D+=(int)(e*e*s->vig);
        }
        /* the searchlights mix toward their colour rather than add: the
           light takes the room it needs from the darkening, so a beam
           shows outside the plate too, where nothing else is dimmed.
           Softer still than the rays, a beam's light is worked out once
           per 8x2 and shared by the two 4-blocks in it. */
        if(nb && (!(x&7) || x==sp[q][0]))
          bdv=fx_beam_light(bm,nb,bx0,bx1,(float)((x&~7)+4),yc,bA,&bc);
        uint32_t c=bc;
        D+=bdv;
        int k=256-(D>255?255:D);
        if(rr){
          int ix=half+(((xs-s->cx)*rsx)>>16);
          if((unsigned)ix<RYS){
            int r=rr[ix];
            if(r!=255){
              int ai=(ar[ix]+rot)&1023;
              int v=((prof[ai]*rl[r])>>8)+cl[r];
              if(v>2) c=px_add(c,px_scale(pc[ai],v>256?256:v));
            }
          }
        }
        if(c){
          /* the light may fill only the headroom the darkening left, so
             the per-pixel add below can never carry */
          int hr=255-((255*k)>>8);
          int cr=(c>>16)&255, cg=(c>>8)&255, cbb=c&255;
          if(cr>hr) cr=hr;
          if(cg>hr) cg=hr;
          if(cbb>hr) cbb=hr;
          c=RGB(cr,cg,cbb);
        }
        kx[x]=kx[x+1]=kx[x+2]=kx[x+3]=(uint32_t)k;
        cx[x]=cx[x+1]=cx[x+2]=cx[x+3]=c;
      }
    }
    /* scale by k/256 and add the light: the scaled channel plus its
       clamped light never exceeds 255, so no saturation is needed */
    uint32_t*d=fb+(size_t)y*FBW;
    for(int q=0;q<ns;q++){
      int xa=sp[q][0], xb=sp[q][1];
      for(int x=xa;x<xb;x++){
        uint32_t v=d[x], k=kx[x];
        d[x]=(((((v&0xFF00FFu)*k)>>8)&0xFF00FFu)|((((v&0x00FF00u)*k)>>8)&0x00FF00u))+cx[x];
      }
    }
  }
}

/*  A rounded frame of thickness t as row spans: per row, the outer and
 *  inner insets are one square root each, and the pixels between them
 *  are blended.  fb_rframe evaluates a distance field across the whole
 *  corner band of every row; thirty lit cells made it the single most
 *  expensive thing in the win display.                                */
static float fx_inset(int dy,int h,float r){
  if(r<=0) return 0;
  float q;
  if(dy<r) q=r-dy-0.5f;
  else if(dy>=h-r) q=dy-(h-r)+0.5f;
  else return 0;
  return r-sqrtf(fmaxf(0.0f,r*r-q*q));
}
static void fx_rframe(int x,int y,int w,int h,float r,int t,uint32_t col,int alpha){
  if(w<=2*t||h<=2*t||alpha<=0) return;
  if(alpha>255) alpha=255;
  int a256=alpha+(alpha>>7);
  int y0=y, y1=y+h;
  if(!fx_clip(&y0,&y1)) return;
  float ri=r-t>0?r-t:0;
  for(int j=y0;j<y1;j++){
    int dy=j-y;
    float eo=fx_inset(dy,h,r);
    int oa=x+(int)ceilf(eo), ob=x+w-(int)ceilf(eo);
    uint32_t*d=fb+(size_t)j*FBW;
    if(dy<t||dy>=h-t){
      for(int i=(oa<0?0:oa);i<ob&&i<FBW;i++) d[i]=px_mix(d[i],col,a256);
    } else {
      float ei=fx_inset(dy-t,h-2*t,ri);
      int ia=x+t+(int)floorf(ei), ib=x+w-t-(int)floorf(ei);
      for(int i=(oa<0?0:oa);i<ia&&i<FBW;i++) d[i]=px_mix(d[i],col,a256);
      for(int i=(ib<0?0:ib);i<ob&&i<FBW;i++) d[i]=px_mix(d[i],col,a256);
    }
    float fr=ceilf(eo)-eo;                     /* soften the outer ends */
    if(fr>0.05f){
      int k=(int)(fr*a256);
      if(oa-1>=0&&oa-1<FBW) d[oa-1]=px_mix(d[oa-1],col,k);
      if(ob>=0&&ob<FBW)     d[ob]=px_mix(d[ob],col,k);
    }
  }
}

/*  A rounded plate with a vertical gradient: the corner insets are one
 *  square root per row, the rest a straight blend.  fb_rrectg evaluates
 *  a distance field along every edge pixel, which a big celebration
 *  plate drawn every frame does not need.                             */
static void fx_plate(int x,int y,int w,int h,int r,uint32_t top,uint32_t bot,int alpha){
  if(w<=0||h<=0||alpha<=0) return;
  if(alpha>255) alpha=255;
  int a256=alpha+(alpha>>7);
  int y0=y, y1=y+h;
  if(!fx_clip(&y0,&y1)) return;
  for(int j=y0;j<y1;j++){
    int dy=j-y;
    float e=0;
    if(dy<r)          e=r-sqrtf(fmaxf(0.0f,(float)(r*r)-(r-dy-0.5f)*(r-dy-0.5f)));
    else if(dy>=h-r)  e=r-sqrtf(fmaxf(0.0f,(float)(r*r)-(dy-(h-r)+0.5f)*(dy-(h-r)+0.5f)));
    int xa=x+(int)ceilf(e), xb=x+w-(int)ceilf(e);
    if(xa<0) xa=0;
    if(xb>FBW) xb=FBW;
    uint32_t c=mixc(top,bot,(float)dy/(float)(h>1?h-1:1));
    uint32_t*d=fb+(size_t)j*FBW;
    for(int i=xa;i<xb;i++) d[i]=px_mix(d[i],c,a256);
    /* soften the ends of the span by the fractional inset */
    float fr=ceilf(e)-e;
    if(fr>0.05f){
      int k=(int)(fr*a256);
      if(xa-1>=0) d[xa-1]=px_mix(d[xa-1],c,k);
      if(xb<FBW)  d[xb]=px_mix(d[xb],c,k);
    }
  }
}

/* an additive ring, for shockwaves: only the annulus is visited */
static void fx_ring(int cx,int cy,float r,float th,uint32_t col,int alpha){
  if(r<=1||th<=0.5f||alpha<=0) return;
  if(alpha>255) alpha=255;
  float ro=r+th, ri=r-th; if(ri<0) ri=0;
  int y0=(int)(cy-ro), y1=(int)(cy+ro)+1;
  if(!fx_clip(&y0,&y1)) return;
  for(int y=y0;y<y1;y++){
    float dy=y+0.5f-cy, dy2=dy*dy;
    if(dy2>ro*ro) continue;
    float xo=sqrtf(ro*ro-dy2), xi=dy2<ri*ri?sqrtf(ri*ri-dy2):0.0f;
    for(int side=0;side<2;side++){
      int xa,xb;
      if(side==0){ xa=(int)(cx-xo); xb=(int)(cx-xi)+1; }
      else       { xa=(int)(cx+xi); xb=(int)(cx+xo)+1; }
      if(xa<0) xa=0;
      if(xb>FBW) xb=FBW;
      uint32_t*d=fb+(size_t)y*FBW;
      for(int x=xa;x<xb;x++){
        float dx=x+0.5f-cx, dd=sqrtf(dx*dx+dy2);
        float k=1.0f-fabsf(dd-r)/th;
        if(k<=0) continue;
        d[x]=px_add(d[x],px_scale(col,(int)(k*k*alpha)));
      }
      if(xi<=0.0f) break;           /* a full chord: one span */
    }
  }
}

/*  A thick segment drawn as row spans of the capsule it sweeps.  The
 *  old path line stamped a t x t square at every step along the line,
 *  49 blends a step for a 7-pixel line.                              */
static void fx_capsule(float ax,float ay,float bx,float by,float r,uint32_t col,int alpha){
  if(alpha<=0) return;
  if(alpha>255) alpha=255;
  int a256=alpha+(alpha>>7);
  float dx=bx-ax, dy=by-ay, L=sqrtf(dx*dx+dy*dy);
  float ux=L>1e-4f?dx/L:1.0f, uy=L>1e-4f?dy/L:0.0f;
  int y0=(int)floorf(fminf(ay,by)-r), y1=(int)ceilf(fmaxf(ay,by)+r)+1;
  if(!fx_clip(&y0,&y1)) return;
  for(int y=y0;y<y1;y++){
    float fy=y+0.5f, lo=1e9f, hi=-1e9f;
    for(int e=0;e<2;e++){
      float px=e?bx:ax, py=e?by:ay, t=fy-py;
      if(fabsf(t)<=r){ float w=sqrtf(r*r-t*t); lo=fminf(lo,px-w); hi=fmaxf(hi,px+w); }
    }
    float blo=-1e9f, bhi=1e9f;         /* the band |cross| <= r         */
    float q=ux*(fy-ay);
    if(fabsf(uy)>1e-4f){
      float c0=(q-r)/uy, c1=(q+r)/uy;
      blo=ax+fminf(c0,c1); bhi=ax+fmaxf(c0,c1);
    } else if(fabsf(q)>r){ blo=1; bhi=0; }
    float slo=-1e9f, shi=1e9f;         /* the slab 0 <= along <= L       */
    float p=uy*(fy-ay);
    if(fabsf(ux)>1e-4f){
      float c0=(-p)/ux, c1=(L-p)/ux;
      slo=ax+fminf(c0,c1); shi=ax+fmaxf(c0,c1);
    } else if(p<0||p>L){ slo=1; shi=0; }
    float ilo=fmaxf(blo,slo), ihi=fminf(bhi,shi);
    if(ilo<=ihi){ lo=fminf(lo,ilo); hi=fmaxf(hi,ihi); }
    if(hi<lo) continue;
    int xa=(int)ceilf(lo-0.5f), xb=(int)floorf(hi-0.5f)+1;
    if(xa<0) xa=0;
    if(xb>FBW) xb=FBW;
    uint32_t*d=fb+(size_t)y*FBW;
    for(int x=xa;x<xb;x++) d[x]=a256>=256?col:px_mix(d[x],col,a256);
  }
}

static void fx_coin(int cx,int cy,int dia,float spin,int alpha){
  int si=0;
  for(int i=1;i<NCSZ;i++) if(abs(CSZ[i]-dia)<abs(CSZ[si]-dia)) si=i;
  int fr=((int)floorf(spin/(TAU*0.5f)*NCFR))%NCFR;
  if(fr<0) fr+=NCFR;
  const fxspr_t*s=&coinS[si][fr];
  fxs_blit(s,cx-s->w*0.5f,cy-s->h*0.5f,1.0f,alpha,0,FX_NOSHINE);
}

/*  Numbers are set from the baked digit sprites, each advancing by its
 *  own width (tw, the glyph's advance in the display face) plus a little
 *  tracking.  The old fixed 6*px pitch was the 5x7 font's, and Bungee's
 *  digits and comma are not that wide.                                */
#define FX_TRACK 0.35f           /* extra space between digits, in px units */
static float fx_number_w(const char*b,float sc){
  float w=0; int n=0;
  for(int i=0;b[i];i++,n++) w += digS[b[i]==','?10:b[i]-'0'].tw*sc;
  return n?w+(n-1)*FX_TRACK*FXDPX*sc:0;
}
static void fx_number(long long v,float cx,float y,float sc,int alpha,int white){
  char b[40];
  commas(b,sizeof b,v<0?0:v);
  float pen=cx-fx_number_w(b,sc)*0.5f;
  for(int i=0;b[i];i++){
    const fxspr_t*s=&digS[b[i]==','?10:b[i]-'0'];
    fxs_blit(s,pen-s->ox*sc,y-s->oy*sc,sc,alpha,white,FX_NOSHINE);
    pen += s->tw*sc+FX_TRACK*FXDPX*sc;
  }
}
/* the scale that sets v at `sc`, or smaller if it would be wider than maxw */
static float fx_number_fit(long long v,float sc,float maxw){
  char b[40];
  commas(b,sizeof b,v<0?0:v);
  float w=fx_number_w(b,sc);
  return w>maxw ? sc*maxw/w : sc;
}
/* the same number's warm halo, drawn under it (additive, so digits that
   overlap their neighbours' light do not cut into each other) */
static void fx_number_glow(long long v,float cx,float y,float sc,uint32_t col,int alpha){
  char b[40];
  commas(b,sizeof b,v<0?0:v);
  float pen=cx-fx_number_w(b,sc)*0.5f;
  for(int i=0;b[i];i++){
    const fxspr_t*s=&digS[b[i]==','?10:b[i]-'0'];
    fxs_glow(s,pen-s->ox*sc,y-s->oy*sc,sc,col,alpha);
    pen += s->tw*sc+FX_TRACK*FXDPX*sc;
  }
}
/* a baked title, scaled about its centre (cx,cy) */
static void fx_title(const fxspr_t*s,float cx,float cy,float sc,int alpha,int white,float shine){
  if(!s->c) return;
  float X=cx-(s->ox+s->tw*0.5f)*sc, Y=cy-(s->oy+s->th*0.5f)*sc;
  fxs_blit(s,X,Y,sc,alpha,white,shine);
}
static void fx_title_glow(const fxspr_t*s,float cx,float cy,float sc,uint32_t col,int alpha){
  if(!s->c) return;
  float X=cx-(s->ox+s->tw*0.5f)*sc, Y=cy-(s->oy+s->th*0.5f)*sc;
  fxs_glow(s,X,Y,sc,col,alpha);
}
/* the x a shine band should sit at, sweeping a title every `period` s */
static float fx_shine_x(const fxspr_t*s,float cx,float sc,float t,float period,float dur){
  float ph=fmodf(t,period);
  if(ph>dur||!s->c) return FX_NOSHINE;
  float w=s->tw*sc;
  return cx-w*0.5f-160.0f+(ph/dur)*(w+320.0f);
}

/* === PARTICLES ==================================================== */
enum { FXK_ROCKET=8, FXK_EMBER, FXK_FLARE };
#define FXF_TOP    1
#define FXF_HOME   2
#define FXF_BOUNCE 4
#define FXN 1000
#define FXFLOOR ((float)(DECKY+14))
typedef struct {
  float x,y,vx,vy,life,life0,rot,rotv,tx,ty;
  uint32_t col; uint16_t id; uint8_t kind,size,flags,bounces;
} fxp_t;
static fxp_t fxp[FXN];
static int fxCount;
static uint16_t fxId;

static fxp_t* fx_new(int kind,float x,float y,int top){
  if(fxCount>=FXN) return NULL;
  fxp_t*p=&fxp[fxCount++];
  memset(p,0,sizeof *p);
  p->kind=(uint8_t)kind; p->x=x; p->y=y;
  p->flags=(uint8_t)(top?FXF_TOP:0);
  p->id=fxId++;
  return p;
}
static void fx_spawn_coin(float x,float y,float vx,float vy,int top){
  fxp_t*p=fx_new(FXK_COIN,x,y,top); if(!p) return;
  p->vx=vx; p->vy=vy;
  p->life=p->life0=1.9f+fxr()*1.0f;
  p->rot=fxr()*TAU;
  p->rotv=(fxr()<0.5f?-1.0f:1.0f)*(8.0f+fxr()*14.0f);
  float z=fxr();
  p->size=(uint8_t)(z<0.25f?0:(z<0.64f?1:(z<0.92f?2:3)));
  p->flags|=FXF_BOUNCE;
}
static void fx_spawn_spark(float x,float y,float vx,float vy,uint32_t col,float life,int top){
  fxp_t*p=fx_new(FXK_SPARK,x,y,top); if(!p) return;
  p->vx=vx; p->vy=vy; p->col=col; p->life=p->life0=life;
}
static void fx_spawn_star(float x,float y,uint32_t col,float life,int size,int top){
  fxp_t*p=fx_new(FXK_STAR,x,y,top); if(!p) return;
  p->vx=fxrs()*20.0f; p->vy=fxrs()*20.0f-10.0f;
  p->col=col; p->life=p->life0=life; p->size=(uint8_t)size;
  p->rot=fxr()*TAU; p->rotv=8.0f+fxr()*10.0f;
}
/* confetti in the lounge's colours: its three neons, gold, green, pink, ivory */
static const uint32_t CONFC[8]={LZ_MAGENTA,LZ_GOLD,LZ_CYAN,LZ_GREEN,0xFF7AE0,LZ_HONEY,LZ_IVORY,LZ_AMBER};
static void fx_spawn_confetti(float x,float y,float vx,float vy,int top){
  fxp_t*p=fx_new(FXK_CONFETTI,x,y,top); if(!p) return;
  p->vx=vx; p->vy=vy; p->col=CONFC[fxRng&7];
  p->life=p->life0=2.4f+fxr()*1.6f;
  p->rot=fxr()*TAU; p->rotv=fxrs()*9.0f; p->size=(uint8_t)(fxr()<0.3f?1:0);
  p->tx=fxr()*TAU;                /* flutter phase */
}
static void fx_spawn_flare(float x,float y,uint32_t col,float life,int size,int top){
  fxp_t*p=fx_new(FXK_FLARE,x,y,top); if(!p) return;
  p->col=col; p->life=p->life0=life; p->size=(uint8_t)size;
}

/* radial spark burst */
static void fx_sparks(float x,float y,int n,uint32_t col,float v0,float v1,int top){
  for(int i=0;i<n;i++){
    float a=fxr()*TAU, sp=v0+fxr()*(v1-v0);
    uint32_t c=(i%3==0)?0xFFFFFF:col;
    fx_spawn_spark(x,y,cosf(a)*sp,sinf(a)*sp-60.0f,c,0.45f+fxr()*0.55f,top);
  }
}
static void fx_explode(float x,float y,uint32_t col,int top){
  int n=opt_limiter?30:44;
  for(int i=0;i<n;i++){
    float a=TAU*i/n+fxrs()*0.05f, sp=260.0f+fxr()*120.0f;
    fx_spawn_spark(x,y,cosf(a)*sp,sinf(a)*sp,(i&3)?col:0xFFFFFF,0.8f+fxr()*0.6f,top);
  }
  for(int i=0;i<10;i++){
    float a=fxr()*TAU, r=fxr()*110.0f;
    fx_spawn_star(x+cosf(a)*r,y+sinf(a)*r,col,0.6f+fxr()*0.7f,fxr()<0.3f?2:1,top);
  }
  fx_spawn_flare(x,y,col,0.45f,2,top);
}

/* -- emitters ------------------------------------------------------- */
enum { EM_OFF, EM_FOUNTAIN, EM_RAIN };
typedef struct { int kind,top; float x,y,t,rate,acc; } fxem_t;
#define FXEM 8
static fxem_t fxem[FXEM];

static void fx_emit(int kind,float x,float y,float dur,float rate,int top){
  int slot=-1;
  for(int i=0;i<FXEM;i++)
    if(fxem[i].kind==kind && fabsf(fxem[i].x-x)<1.0f && fabsf(fxem[i].y-y)<1.0f){ slot=i; break; }
  if(slot<0) for(int i=0;i<FXEM;i++) if(fxem[i].kind==EM_OFF){ slot=i; break; }
  if(slot<0) return;
  fxem_t*e=&fxem[slot];
  if(e->kind==EM_OFF){ e->acc=fxr(); }
  e->kind=kind; e->top=top; e->x=x; e->y=y;
  if(dur>e->t) e->t=dur;
  e->rate=rate;
}
static void fx_fountain_ex(float x,float y,float dur,float rate,int top){ fx_emit(EM_FOUNTAIN,x,y,dur,rate,top); }
static void fx_fountain(float x,float y,float dur){ fx_emit(EM_FOUNTAIN,x,y,dur,60.0f,1); }
static void fx_rain(float dur,float rate){ fx_emit(EM_RAIN,0,0,dur,rate,1); }
static void fx_stop(void){ for(int i=0;i<FXEM;i++) fxem[i].kind=EM_OFF; }

static void fx_emit_one(const fxem_t*e){
  if(e->kind==EM_FOUNTAIN){
    float vx=(FBW*0.5f-e->x)*0.42f+fxrs()*250.0f;
    float vy=-(760.0f+fxr()*300.0f);
    fx_spawn_coin(e->x+fxrs()*16.0f,e->y,vx,vy,e->top);
    if(fxr()<0.35f)
      fx_spawn_spark(e->x+fxrs()*10.0f,e->y,vx*0.8f+fxrs()*80.0f,vy*(0.7f+fxr()*0.3f),0xFFE08A,0.5f+fxr()*0.4f,e->top);
  } else {
    float x=fxr()*FBW;
    fx_spawn_coin(x,-24.0f-fxr()*30.0f,fxrs()*70.0f,80.0f+fxr()*260.0f,e->top);
  }
}

/* -- the public burst ----------------------------------------------- */
static void fx_burst(float x,float y,int n,int kind){
  switch(kind){
  case FXK_COIN:
    for(int i=0;i<n;i++){
      float a=-TAU*0.25f+fxrs()*1.25f, sp=260.0f+fxr()*420.0f;
      fx_spawn_coin(x+fxrs()*6.0f,y+fxrs()*6.0f,cosf(a)*sp,sinf(a)*sp,1);
    }
    break;
  case FXK_SPARK:    fx_sparks(x,y,n,0xFFD24A,180.0f,620.0f,1); break;
  case FXK_STAR:
    for(int i=0;i<n;i++){
      float a=fxr()*TAU, r=fxr()*60.0f;
      fx_spawn_star(x+cosf(a)*r,y+sinf(a)*r,0xFFF0C0,0.5f+fxr()*0.8f,(int)(fxr()*3.0f),1);
    }
    break;
  default:
    for(int i=0;i<n;i++){
      float a=-TAU*0.25f+fxrs()*1.4f, sp=220.0f+fxr()*420.0f;
      fx_spawn_confetti(x,y,cosf(a)*sp,sinf(a)*sp,1);
    }
    break;
  }
}
/* the core's old coin burst, rerouted: coins if gold, sparks in colour */
static void fx_burst_col(float x,float y,int n,uint32_t col,int top){
  int gold = ((col>>16)&255)>200 && ((col>>8)&255)>150 && (col&255)<140;
  if(gold) for(int i=0;i<(n+1)/2;i++){
    float a=-TAU*0.25f+fxrs()*1.3f, sp=180.0f+fxr()*300.0f;
    fx_spawn_coin(x,y,cosf(a)*sp,sinf(a)*sp,top);
  }
  fx_sparks(x,y,n,col,120.0f,420.0f,top);
}
static void fx_firework(float x,float y,uint32_t col){
  fxp_t*p=fx_new(FXK_ROCKET,x,y,1); if(!p) return;
  p->vx=fxrs()*90.0f; p->vy=-(620.0f+fxr()*260.0f);
  p->col=col; p->life=p->life0=0.55f+fxr()*0.35f;
}
static void fx_home(float x,float y,float tx,float ty,int n,uint32_t col){
  for(int i=0;i<n;i++){
    float a=fxr()*TAU, sp=200.0f+fxr()*420.0f;
    fxp_t*p=fx_new(FXK_SPARK,x+fxrs()*30.0f,y+fxrs()*20.0f,1); if(!p) return;
    p->vx=cosf(a)*sp; p->vy=sinf(a)*sp;
    p->col=(i&1)?0xFFFFFF:col; p->life=p->life0=1.6f;
    p->tx=tx+fxrs()*10.0f; p->ty=ty+fxrs()*6.0f;
    p->flags|=FXF_HOME;
  }
}

/* -- shake and flash ------------------------------------------------ */
static float fxShA, fxShT, fxShD, fxShPh;
static int   fxShX, fxShY;
static void fx_shake(float amp,float dur){
  if(dur<=0||amp<=0) return;
  float cur=fxShT>0?fxShA*(fxShT/fxShD)*(fxShT/fxShD):0.0f;
  if(amp<cur) return;
  fxShA=amp; fxShT=fxShD=dur; fxShPh=fxr()*100.0f;
}
static void fx_flash(float amt){
  if(opt_limiter && amt>0.30f) amt=0.30f;
  if(amt>G.flash) G.flash=amt;
}

/* -- transition ----------------------------------------------------- */
#define FXTR_DUR 1.6f
static float fxTrP=-1.0f;        /* seconds in, <0 when idle           */
static char  fxSub[64];
static uint32_t fxCol;

static void fx_transition(const char*title,const char*sub,uint32_t col){
  /* gold type like every other title in the lounge; the card's own
     colour is its halo, its rays and its ring */
  fxHalo=scalec(col,0.8f);
  fx_bake_text(&trS,title&&title[0]?title:" ",13,FX_GOLDG,5,12);
  fxHalo=0;
  snprintf(fxSub,sizeof fxSub,"%s",sub?sub:"");
  fxCol=col; fxTrP=0.0f;
  fx_flash(0.55f);
}
static int fx_transition_busy(void){ return fxTrP>=0.0f && fxTrP<FXTR_DUR; }

/* === INIT ========================================================= */
static int fxReady;
static void fx_init(void){
  if(fxReady) return;
  glowT=(uint8_t*)malloc(GLS*GLS);
  if(glowT) for(int y=0;y<GLS;y++) for(int x=0;x<GLS;x++){
    float dx=(x+0.5f)/(GLS*0.5f)-1.0f, dy=(y+0.5f)/(GLS*0.5f)-1.0f;
    float d=sqrtf(dx*dx+dy*dy), v=0.0f;
    if(d<1.0f){ float g=1.0f-d; v=0.55f*g*g+0.45f*g*g*g*g*g; }
    glowT[y*GLS+x]=(uint8_t)(v*255.0f);
  }
  rayA=(uint16_t*)malloc((size_t)RYS*RYS*2);
  rayR=(uint8_t*) malloc((size_t)RYS*RYS);
  if(rayA&&rayR) for(int y=0;y<RYS;y++) for(int x=0;x<RYS;x++){
    float dx=x+0.5f-RYS*0.5f, dy=y+0.5f-RYS*0.5f;
    float a=atan2f(dy,dx);
    rayA[y*RYS+x]=(uint16_t)(((int)((a/TAU+1.0f)*1024.0f))&1023);
    float d=sqrtf(dx*dx+dy*dy)/(RYS*0.5f);
    rayR[y*RYS+x]=(uint8_t)(d>=1.0f?255:(int)(d*254.0f));
  }
  for(int i=0;i<NCSZ;i++) fx_build_coin(i);
  for(int i=0;i<NSTAR;i++) fx_build_star(&starS[i],STARR[i]);
  for(int i=0;i<NSYM;i++) for(int k=1;k<NPOP;k++) fx_build_pop(&popS[i][k],&sym[i],POPSC[k]);

  /* the celebration titles: gold display type, each tier with the halo
     of its neon (EPIC in rose gold, the ULTIMATE in pale platinum gold) */
  fxHalo=0xE08A10;
  fx_bake_text(&titleS[FXT_BIG],  "BIG WIN",  14,FX_GOLDG,5,14);
  fxHalo=0xC8189A;
  fx_bake_text(&titleS[FXT_SUPER],"SUPER WIN",14,FX_GOLDG,5,14);
  fxHalo=0x109CC8;
  fx_bake_text(&titleS[FXT_MEGA], "MEGA WIN", 14,FX_GOLDG,5,14);
  fxHalo=0xE0306A;
  fx_bake_text(&titleS[FXT_EPIC], "EPIC WIN", 15,FX_ROSEG,5,16);
  fxHalo=0x18A8D8;
  fx_bake_text(&titleS[FXT_JULT],  JP_NAME[JP_ULT],  13,FX_PLATG,5,16);
  fxHalo=0xC8189A;
  fx_bake_text(&titleS[FXT_JMEGA], JP_NAME[JP_MEGA], 15,FX_GOLDG,5,16);
  fxHalo=0xD84A2A;
  fx_bake_text(&titleS[FXT_JMAJOR],JP_NAME[JP_MAJOR],15,FX_GOLDG,5,16);
  fxHalo=0x20B888;
  fx_bake_text(&titleS[FXT_JMINOR],JP_NAME[JP_MINOR],15,FX_GOLDG,5,16);
  fxHalo=0xD07010;
  fx_bake_text(&titleS[FXT_JACKPOT],"JACKPOT",9,FX_GOLDG,5,10);
  fx_bake_text(&titleS[FXT_X2],"X2",16,FX_GOLDG,5,12);
  fx_bake_text(&titleS[FXT_X3],"X3",16,FX_GOLDG,5,12);
  fx_bake_text(&titleS[FXT_X4],"X4",16,FX_GOLDG,5,12);
  fx_bake_text(&titleS[FXT_X5],"X5",16,FX_GOLDG,5,12);
  fxHalo=0;
  fx_bake_text(&titleS[FXT_BADGE],"X2",3,FX_GOLDG,5,0);
  /* the digits carry a glow mask (no baked halo, which would paint over
     the neighbouring digit): fx_number_glow lights them from behind */
  for(int i=0;i<10;i++){ char b[2]={(char)('0'+i),0}; fx_bake_text(&digS[i],b,FXDPX,FX_GOLDG,5,8); }
  fx_bake_text(&digS[10],",",FXDPX,FX_GOLDG,5,8);
  /* the glass the counts sit on */
  fx_bake_panel(&fxPanel[FXP_WIN],600,152,18.0f);
  fx_bake_panel(&fxPanel[FXP_MULT],700,112,16.0f);
  fx_bake_panel(&fxPanel[FXP_SUB],900,44,12.0f);
  fxReady=1;
}
static void fx_deinit(void){
  free(glowT); glowT=NULL;
  free(rayA); rayA=NULL; free(rayR); rayR=NULL;
  for(int i=0;i<NCSZ;i++) for(int f=0;f<NCFR;f++) fxs_free(&coinS[i][f]);
  for(int i=0;i<NSTAR;i++) fxs_free(&starS[i]);
  for(int i=0;i<NSYM;i++) for(int k=0;k<NPOP;k++) fxs_free(&popS[i][k]);
  for(int i=0;i<NFXT;i++) fxs_free(&titleS[i]);
  for(int i=0;i<11;i++) fxs_free(&digS[i]);
  for(int i=0;i<NFXP;i++){ fxs_free(&fxPanel[i].body); fxs_free(&fxPanel[i].neon); }
  fxs_free(&trS);
  fxCount=0; fx_stop(); fxTrP=-1.0f;
  fxReady=0;
  /* primitives the old win banner and path lines used; this file drew
     those and no longer needs them, but other modules may */
  (void)fb_line; (void)seg_width; (void)fb_rect;
}

/* === UPDATE ======================================================= */
static float fxPrevMU;
#define MULT_METER_X ((float)(LRX+RAILW-67))
#define MULT_METER_Y ((float)(FEATY+40))

static void fx_update(void){
  /* emitters */
  for(int i=0;i<FXEM;i++){
    fxem_t*e=&fxem[i];
    if(e->kind==EM_OFF) continue;
    e->t-=DT;
    if(e->t<=0){ e->kind=EM_OFF; continue; }
    e->acc+=e->rate*DT;
    while(e->acc>=1.0f){ e->acc-=1.0f; fx_emit_one(e); }
  }
  /* particles: swap-remove the dead, so the live ones stay packed */
  for(int i=0;i<fxCount;){
    fxp_t*p=&fxp[i];
    p->life-=DT;
    if(p->life<=0){
      if(p->kind==FXK_ROCKET){
        float x=p->x,y=p->y; uint32_t c=p->col; int top=p->flags&FXF_TOP;
        fxp[i]=fxp[--fxCount];
        fx_explode(x,y,c,top);
        continue;
      }
      fxp[i]=fxp[--fxCount];
      continue;
    }
    switch(p->kind){
    case FXK_COIN:
      p->vy+=900.0f*DT; p->vx*=0.996f;
      p->x+=p->vx*DT; p->y+=p->vy*DT; p->rot+=p->rotv*DT;
      if((p->flags&FXF_BOUNCE) && p->y>FXFLOOR && p->vy>0){
        p->y=FXFLOOR; p->vy*=-0.36f; p->vx*=0.65f; p->rotv*=0.55f;
        if(++p->bounces>=2 && p->life>0.30f) p->life=0.30f;
      }
      if(p->y>FBH+40||p->x<-60||p->x>FBW+60) p->life=0;
      break;
    case FXK_SPARK: case FXK_EMBER:
      if(p->flags&FXF_HOME){
        float dx=p->tx-p->x, dy=p->ty-p->y, d=sqrtf(dx*dx+dy*dy);
        float age=p->life0-p->life;
        if(d<18.0f){
          fx_spawn_flare(p->tx,p->ty,0xFFD24A,0.30f,1,1);
          if(fxr()<0.5f) fx_spawn_star(p->tx+fxrs()*40.0f,p->ty+fxrs()*24.0f,0xFFF0C0,0.5f,1,1);
          p->life=0; break;
        }
        float pull=age<0.18f?0.0f:3600.0f;
        p->vx+=dx/d*pull*DT; p->vy+=dy/d*pull*DT;
        p->vx*=0.935f; p->vy*=0.935f;
      } else {
        p->vy+=(p->kind==FXK_EMBER?60.0f:320.0f)*DT;
        p->vx*=0.975f; p->vy*=0.975f;
      }
      p->x+=p->vx*DT; p->y+=p->vy*DT;
      break;
    case FXK_STAR:
      p->vx*=0.97f; p->vy*=0.97f; p->x+=p->vx*DT; p->y+=p->vy*DT;
      break;
    case FXK_CONFETTI:
      p->vy+=420.0f*DT; p->vx*=0.965f; p->vy*=0.965f;
      p->tx+=DT*7.0f;
      p->x+=(p->vx+sinf(p->tx)*55.0f)*DT; p->y+=p->vy*DT; p->rot+=p->rotv*DT;
      if(p->y>FBH+20) p->life=0;
      break;
    case FXK_ROCKET:
      p->vy+=380.0f*DT; p->x+=p->vx*DT; p->y+=p->vy*DT;
      { fxp_t*e=fx_new(FXK_EMBER,p->x+fxrs()*2.0f,p->y,1);
        if(e){ e->vx=fxrs()*30.0f; e->vy=40.0f+fxr()*40.0f; e->col=0xFFD8A0; e->life=e->life0=0.35f; }
        p=&fxp[i]; }                        /* fx_new may not move the array, but be tidy */
      break;
    default: break;
    }
    i++;
  }
  /* transition: the title lands at 0.30 s */
  if(fxTrP>=0.0f){
    float prev=fxTrP;
    fxTrP+=DT;
    if(prev<0.30f && fxTrP>=0.30f){
      fx_sparks(FBW*0.5f,300.0f,opt_limiter?70:110,fxCol,200.0f,900.0f,1);
      for(int i=0;i<40;i++) fx_spawn_confetti(FBW*0.5f+fxrs()*300.0f,300.0f+fxrs()*40.0f,fxrs()*420.0f,-200.0f-fxr()*520.0f,1);
      for(int i=0;i<14;i++) fx_spawn_star(FBW*0.5f+fxrs()*380.0f,300.0f+fxrs()*80.0f,0xFFFFFF,0.5f+fxr()*0.6f,2,1);
      fx_shake(10.0f,0.40f);
    }
    if(fxTrP>=FXTR_DUR) fxTrP=-1.0f;
  }
  /* shake: two incommensurate sines, decaying, so it rolls rather than
     jitters from one frame to the next */
  if(fxShT>0){
    fxShT-=DT;
    float k=fxShT>0?fxShT/fxShD:0.0f;
    float a=fxShA*k*k*(opt_limiter?0.6f:1.0f);
    fxShPh+=DT;
    fxShX=(int)lrintf(a*sinf(fxShPh*71.0f));
    fxShY=(int)lrintf(a*0.8f*sinf(fxShPh*53.0f+1.3f));
  } else { fxShX=0; fxShY=0; }
  /* the multiplier pop: once the number has landed, its sparks fly to
     the meter on the rail */
  if(G.multUp>0 && G.inFree && fxPrevMU>1.25f && G.multUp<=1.25f)
    fx_home(FBW*0.5f,(float)(GY+GH/2),MULT_METER_X,MULT_METER_Y,opt_limiter?18:28,0xFFD24A);
  fxPrevMU=G.multUp;
}

/* === PARTICLE DRAW ================================================ */
static void fx_draw_spark(const fxp_t*p){
  float sp=sqrtf(p->vx*p->vx+p->vy*p->vy);
  float fade=clampf(p->life/p->life0*1.8f,0,1);
  int home=(p->flags&FXF_HOME)!=0;
  float len=p->kind==FXK_EMBER?1.0f:clampf(sp*(home?0.035f:0.024f),2.0f,home?34.0f:24.0f);
  float ux=sp>1?p->vx/sp:0, uy=sp>1?p->vy/sp:0;
  uint32_t hot=mixc(p->col,0xFFFFFF,0.55f);
  int n=(int)len+1;
  for(int k=0;k<=n;k++){
    float t=(float)k/n;
    int x=(int)(p->x-ux*len*t), y=(int)(p->y-uy*len*t);
    if(y<clip_y0||y>=clip_y1||(unsigned)x>=FBW||(unsigned)y>=FBH) continue;
    int v=(int)((1.0f-t)*fade*255.0f);
    fb[y*FBW+x]=px_add(fb[y*FBW+x],px_scale(k<2?hot:p->col,v));
  }
  int hx=(int)p->x, hy=(int)p->y, hv=(int)(fade*(home?200:140));
  if(home) fx_glow(hx,hy,10,0xFFC040,(int)(fade*230));   /* a comet head */
  static const int O[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
  for(int k=0;k<4;k++){
    int x=hx+O[k][0], y=hy+O[k][1];
    if(y<clip_y0||y>=clip_y1||(unsigned)x>=FBW||(unsigned)y>=FBH) continue;
    fb[y*FBW+x]=px_add(fb[y*FBW+x],px_scale(p->col,hv));
  }
}
static void fx_draw_confetti(const fxp_t*p){
  float hw=p->size?5.0f:4.0f, hh=p->size?2.6f:2.0f;
  float flip=cosf(p->rot*1.7f+p->tx);
  hw*=fmaxf(0.18f,fabsf(flip));
  float ca=cosf(p->rot), sa=sinf(p->rot);
  float fade=clampf(p->life*1.5f,0,1);
  uint32_t col=scalec(p->col,0.55f+0.45f*fabsf(flip));
  if(fabsf(flip)>0.93f) col=mixc(col,0xFFFFFF,0.5f);   /* catches the light */
  int a=(int)(fade*256);
  int ex=(int)(fabsf(hw*ca)+fabsf(hh*sa))+1, ey=(int)(fabsf(hw*sa)+fabsf(hh*ca))+1;
  int cx=(int)p->x, cy=(int)p->y, y0=cy-ey, y1=cy+ey+1;
  if(!fx_clip(&y0,&y1)) return;
  for(int y=y0;y<y1;y++) for(int x=cx-ex;x<=cx+ex;x++){
    if((unsigned)x>=FBW) continue;
    float dx=x+0.5f-p->x, dy=y+0.5f-p->y;
    float lx=dx*ca+dy*sa, ly=-dx*sa+dy*ca;
    if(fabsf(lx)>hw||fabsf(ly)>hh) continue;
    fb[y*FBW+x]=px_mix(fb[y*FBW+x],col,a);
  }
}
static void fx_draw_parts(int top){
  /* coins first, far (small) to near (large), so the depth reads */
  for(int sz=0;sz<NCSZ;sz++)
    for(int i=0;i<fxCount;i++){
      const fxp_t*p=&fxp[i];
      if(p->kind!=FXK_COIN||p->size!=sz||((p->flags&FXF_TOP)?1:0)!=top) continue;
      int fr=((int)floorf(p->rot/(TAU*0.5f)*NCFR))%NCFR;
      if(fr<0) fr+=NCFR;
      const fxspr_t*s=&coinS[sz][fr];
      int a=(int)(clampf(p->life/0.30f,0,1)*255);
      fxs_blit1(s,(int)p->x-s->w/2,(int)p->y-s->h/2,a);
      /* now and then a coin catches the light */
      if(sz>=1 && (fxhash(p->id*977u+(uint32_t)(p->life*7.0f))&15)==0)
        fxs_add(&starS[sz>=3?2:1],(int)p->x-CSZ[sz]/5,(int)p->y-CSZ[sz]/5,0xFFF4D0,230);
    }
  for(int i=0;i<fxCount;i++){
    const fxp_t*p=&fxp[i];
    if(((p->flags&FXF_TOP)?1:0)!=top) continue;
    switch(p->kind){
    case FXK_SPARK: case FXK_EMBER: case FXK_ROCKET:
      fx_draw_spark(p); break;
    case FXK_STAR: {
      float age=p->life0-p->life;
      float tw=0.5f+0.5f*sinf(p->rot+age*p->rotv);
      float env=clampf(p->life/p->life0*2.5f,0,1)*clampf(age*8.0f,0,1);
      float v=tw*env;
      int si=clampi(p->size-(v<0.45f?1:0),0,NSTAR-1);
      fxs_add(&starS[si],(int)p->x,(int)p->y,p->col,(int)(v*255));
      break; }
    case FXK_CONFETTI: fx_draw_confetti(p); break;
    case FXK_FLARE: {
      float k=p->life/p->life0;
      int r=(int)((p->size+1)*40*(1.4f-k*0.4f));
      fx_glow(p->x,p->y,r,p->col,(int)(k*k*(opt_limiter?120:200)));
      break; }
    default: break;
    }
  }
}

static void fx_draw_transition(void){
  if(fxTrP<0.0f||!trS.c) return;
  float p=fxTrP, lim=opt_limiter?0.5f:1.0f;
  float env = p<0.10f ? p/0.10f : (p>FXTR_DUR-0.25f ? (FXTR_DUR-p)/0.25f : 1.0f);
  env=clampf(env,0,1);
  float cx=FBW*0.5f, cy=300.0f;
  { fxstage_t st={ (int)cx,(int)cy, 0, 520,240,(int)(215*env),
                   120,470,18,(int)(130*env),0,(int)(70*env), p*0.9f,0.55f, fxCol,
                   (int)(110*env), p+1.0f, {FX_BEAM,FX_BEAM}, 0,0,0,0 };
    fx_stage_draw(&st); }
  /* the slam: in from huge, accelerating, with a trail of ghosts */
  float sc, white=0;
  if(p<0.30f){ float u=p/0.30f; sc=2.6f-1.6f*u*u; }
  else { float q=p-0.30f; sc=1.0f-0.10f*expf(-q*10.0f)*cosf(q*26.0f); }
  if(p>FXTR_DUR-0.25f) sc*=1.0f+(p-(FXTR_DUR-0.25f))*1.2f;
  if(p>=0.30f && p<0.55f) white=(1.0f-(p-0.30f)/0.25f)*200.0f*lim;
  if(p>=0.28f){
    float r=(p-0.28f)*1500.0f;
    fx_ring((int)cx,(int)cy,r,26.0f,fxCol,(int)(170*clampf(1.0f-(p-0.28f)/0.5f,0,1)));
  }
  if(p>0.26f && p<0.7f) fx_title_glow(&trS,cx,cy,sc,fxCol,(int)(220*(1.0f-(p-0.26f)/0.44f)));
  if(p<0.30f){
    if(sc<1.9f) fx_title(&trS,cx,cy,sc*1.25f,(int)(60*env),0,FX_NOSHINE);
    if(sc<2.2f) fx_title(&trS,cx,cy,sc*1.11f,(int)(110*env),0,FX_NOSHINE);
  }
  float sh= (p>0.45f&&p<1.05f) ? fx_shine_x(&trS,cx,sc,p-0.45f,10.0f,0.6f) : FX_NOSHINE;
  fx_title(&trS,cx,cy,sc,(int)(255*env),(int)white,sh);
  if(fxSub[0] && p>0.35f){
    float a=clampf((p-0.35f)/0.2f,0,1)*env;
    /* on a slim bar of dark glass in the card's neon, so it reads over
       the cream drums */
    const fxpanel_t*P=&fxPanel[FXP_SUB];
    float w=lz_width(LZF_UI_M,fxSub,28.0f,3.0f)+60.0f;
    float sy=cy+trS.th*0.5f+26.0f;
    if(w<=P->w) fx_panel_draw(P,(int)cx-P->w/2,(int)sy,fxCol,(int)(235*a));
    fx_caption(fxSub,cx,sy+6.0f,28.0f,lz_hot(fxCol,0.55f),a,3.0f);
  }
}

static void fx_draw(void){ fx_draw_parts(0); }
static void fx_draw_top(void){
  fx_draw_transition();
  fx_draw_parts(1);
}

/*  Whole-frame shake, run once after the bands join.  The frame moves by
 *  (dx,dy) and the edge it uncovers repeats the last row or column, so
 *  the cabinet appears to jolt rather than a black bar blinking in.   */
static void fx_post(void){
  int dx=fxShX, dy=fxShY;
  if(!dx&&!dy) return;
  dx=clampi(dx,-40,40); dy=clampi(dy,-40,40);
  int ys=dy>0?FBH-1:0, ye=dy>0?-1:FBH, yi=dy>0?-1:1;
  for(int y=ys;y!=ye;y+=yi){
    int sy=clampi(y-dy,0,FBH-1);
    uint32_t*d=fb+(size_t)y*FBW; const uint32_t*s=fb+(size_t)sy*FBW;
    if(dx>=0){
      memmove(d+dx,s,(size_t)(FBW-dx)*4);
      for(int x=0;x<dx;x++) d[x]=d[dx];
    } else {
      memmove(d,s-dx,(size_t)(FBW+dx)*4);
      for(int x=FBW+dx;x<FBW;x++) d[x]=d[FBW+dx-1];
    }
  }
}

/* === WIN PRESENTATION: UPDATE SIDE ================================
 *  Tiers by win / total bet.  BIG shows from the first frame and the
 *  count then climbs through the tier lines one at a time, each segment
 *  eased in and out so the count stalls just short of a line, then the
 *  title SLAMS to the next tier.  The time budget per tier is what sets
 *  the length: about 3 s for a BIG, 12 s for an EPIC.
 * ================================================================= */
static const int BW_X[5]={0,10,25,50,100};
static int fx_bw_tier(long long win,long long bet){
  int t=0;
  for(int k=1;k<5;k++) if(win>=(long long)BW_X[k]*bet) t=k;
  return t;
}
#define BW_SEG 2.3f
static float bw_final(long long win,long long bet,int T){
  if(T>=4) return 3.2f;
  long long a=(long long)BW_X[T]*bet, b=(long long)BW_X[T+1]*bet;
  float f=b>a?(float)(win-a)/(float)(b-a):1.0f;
  return 1.9f+1.1f*clampf(f,0,1);
}
static float fx_bw_count_end(long long win,long long bet){
  int T=fx_bw_tier(win,bet);
  if(T==0){
    float f=bet>0?(float)win/(float)(10*bet):1.0f;
    return 0.45f+0.85f*clampf(f,0,1);
  }
  return (T-1)*BW_SEG+bw_final(win,bet,T);
}
static long long fx_bw_shown(long long win,long long bet,float t){
  int T=fx_bw_tier(win,bet);
  if(t<=0) return 0;
  if(T==0){
    float u=clampf(t/fx_bw_count_end(win,bet),0,1);
    u=1.0f-(1.0f-u)*(1.0f-u);
    return (long long)(win*(double)u);
  }
  /* the tier lines: 0, then SUPER, MEGA, EPIC */
  for(int s=0;s<T-1;s++){
    if(t<BW_SEG){
      long long a=s?(long long)BW_X[s+1]*bet:0, b=(long long)BW_X[s+2]*bet;
      float u=t/BW_SEG;
      u=u<0.5f?2*u*u:1.0f-2.0f*(1.0f-u)*(1.0f-u);
      u*=0.985f;                     /* never quite reaches the line */
      return a+(long long)((b-a)*(double)u);
    }
    t-=BW_SEG;
  }
  long long a=T>1?(long long)BW_X[T]*bet:0;
  float F=bw_final(win,bet,T), u=clampf(t/F,0,1);
  u=1.0f-(1.0f-u)*(1.0f-u)*(1.0f-u);
  return a+(long long)((win-a)*(double)u);
}

static void fx_bigwin_slam(int tier){
  tier=clampi(tier,1,4);
  uint32_t col=BWCOL[tier];
  float cx=FBW*0.5f, cy=250.0f;
  fx_flash(0.30f+0.12f*tier);
  fx_shake(3.0f+tier*3.0f,0.30f+0.08f*tier);
  fx_sparks(cx,cy,(opt_limiter?30:45)+tier*22,col,220.0f,900.0f,1);
  fx_sparks(cx,cy,20,0xFFFFFF,300.0f,1000.0f,1);
  for(int i=0;i<8+tier*5;i++){
    float a=fxr()*TAU, r=120.0f+fxr()*340.0f;
    fx_spawn_star(cx+cosf(a)*r,cy+sinf(a)*r*0.45f,0xFFF6D8,0.5f+fxr()*0.8f,1+(fxr()<0.4f),1);
  }
  for(int i=0;i<8+tier*6;i++){
    float a=-TAU*0.25f+fxrs()*1.4f, sp=300.0f+fxr()*500.0f;
    fx_spawn_coin(cx+fxrs()*200.0f,cy+fxrs()*30.0f,cosf(a)*sp,sinf(a)*sp,1);
  }
  if(tier>=3)
    for(int i=0;i<30+tier*10;i++)
      fx_spawn_confetti(cx+fxrs()*360.0f,cy+fxrs()*40.0f,fxrs()*500.0f,-240.0f-fxr()*560.0f,1);
  fx_spawn_flare(cx,cy,col,0.5f,3,1);
  /* the stinger: a crash and a chord that climbs with the tier */
  static const float ROOT[5]={0,392,523,659,784};
  float r=ROOT[tier];
  snd_noise_at(0.00f,0.22f,0.16f,2600);
  snd_chord(0.00f,r,r*1.26f,r*1.5f,0.55f,0.20f);
  snd_at(0.00f,r*0.5f,r*0.5f,0.60f,2,0.10f);
  snd_at(0.10f,r*2.0f,r*3.0f,0.30f,0,0.06f);
}
static void fx_bigwin_tick(int tier,int done){
  static const float RATE[5]={0,30,46,64,84};
  tier=clampi(tier,0,4);
  float r=RATE[tier]*(done?0.75f:1.0f);
  fx_fountain_ex((float)(GX+60),FXFLOOR-6.0f,0.25f,r*0.5f,1);
  fx_fountain_ex((float)(GX+GW-60),FXFLOOR-6.0f,0.25f,r*0.5f,1);
  if(tier>=3) fx_rain(0.25f,tier>=4?30.0f:14.0f);
  if(fxr()<0.25f+tier*0.1f){
    float a=fxr()*TAU, rr=200.0f+fxr()*280.0f;
    fx_spawn_star(FBW*0.5f+cosf(a)*rr,250.0f+sinf(a)*rr*0.42f,0xFFF6D8,0.5f+fxr()*0.7f,fxr()<0.3f?2:1,1);
  }
  if(tier>=4 && fxr()<0.05f)
    fx_firework(GX+80.0f+fxr()*(GW-160.0f),FXFLOOR,fx_lounge_col(fxr()));
  if(tier>=4 && fxr()<0.20f)
    fx_spawn_confetti(fxr()*FBW,-10.0f,fxrs()*60.0f,40.0f+fxr()*80.0f,1);
}
static void fx_win_burst(uint32_t mask,uint32_t col,int big){
  for(int c=0;c<NCELL;c++){
    if(!((mask>>c)&1u)) continue;
    float x=(float)cellcx(c/NROW), y=(float)cellcy(c%NROW);
    int nc=big?2:1, ns=big?7:4;
    for(int i=0;i<nc;i++){
      float a=-TAU*0.25f+fxrs()*1.1f, sp=240.0f+fxr()*260.0f;
      fx_spawn_coin(x+fxrs()*20.0f,y+fxrs()*16.0f,cosf(a)*sp,sinf(a)*sp,0);
    }
    fx_sparks(x,y,ns,col,120.0f,380.0f,0);
    if(big) fx_spawn_star(x+fxrs()*30.0f,y+fxrs()*30.0f,0xFFFFFF,0.5f,1,0);
  }
}

/* -- jackpots ------------------------------------------------------- */
static float fx_jackpot_run(int tier){
  switch(tier){ case JP_ULT: return 8.0f; case JP_MEGA: return 4.6f; case JP_MAJOR: return 3.6f; default: return 2.8f; }
}
static float fx_jackpot_hold(int tier){ return tier==JP_ULT?3.5f:2.2f; }
static void fx_jackpot_begin(int tier){
  if(tier<0||tier>=NJP) return;
  int ult=(tier==JP_ULT);
  uint32_t col=JPCOL[tier];
  float cx=FBW*0.5f, cy=215.0f;
  fx_flash(ult?0.9f:0.7f);
  fx_shake(ult?20.0f:12.0f,ult?0.9f:0.55f);
  fx_sparks(cx,cy,opt_limiter?90:150,col,250.0f,1100.0f,1);
  fx_sparks(cx,cy,40,0xFFFFFF,300.0f,1200.0f,1);
  for(int i=0;i<(ult?60:36);i++){
    float a=-TAU*0.25f+fxrs()*1.5f, sp=350.0f+fxr()*600.0f;
    fx_spawn_coin(cx+fxrs()*240.0f,cy+fxrs()*40.0f,cosf(a)*sp,sinf(a)*sp,1);
  }
  for(int i=0;i<(ult?120:60);i++)
    fx_spawn_confetti(cx+fxrs()*420.0f,cy+fxrs()*40.0f,fxrs()*640.0f,-260.0f-fxr()*640.0f,1);
  fx_spawn_flare(cx,cy,col,0.7f,3,1);
  if(ult) for(int i=0;i<3;i++) fx_firework(GX+100.0f+i*(GW-200.0f)/2.0f,FXFLOOR,FX_NEON3[i]);
}
static void fx_jackpot_tick(int tier,float t){
  if(tier<0||tier>=NJP) return;
  int ult=(tier==JP_ULT);
  static const float RAIN[NJP]={52,38,26,18};
  float run=fx_jackpot_run(tier);
  float k=t<run?1.0f:0.6f;
  fx_rain(0.25f,RAIN[tier]*k);
  if(tier==JP_ULT||tier==JP_MEGA){
    fx_fountain_ex((float)(GX+60),FXFLOOR-6.0f,0.25f,(ult?22.0f:14.0f)*k,1);
    fx_fountain_ex((float)(GX+GW-60),FXFLOOR-6.0f,0.25f,(ult?22.0f:14.0f)*k,1);
  }
  float fw = ult?0.032f:(tier==JP_MEGA?0.012f:0.0f);
  if(opt_limiter) fw*=0.6f;
  if(fxr()<fw) fx_firework(80.0f+fxr()*(FBW-160.0f),FXFLOOR,ult?fx_lounge_col(fxr()):JPCOL[tier]);
  if(fxr()<0.35f){
    float a=fxr()*TAU, r=220.0f+fxr()*300.0f;
    fx_spawn_star(FBW*0.5f+cosf(a)*r,215.0f+sinf(a)*r*0.38f,0xFFFFFF,0.5f+fxr()*0.7f,fxr()<0.3f?2:1,1);
  }
  if(ult && fxr()<0.3f) fx_spawn_confetti(fxr()*FBW,-10.0f,fxrs()*60.0f,40.0f+fxr()*80.0f,1);
}

/* -- the multiplier pop --------------------------------------------- */
static void fx_multup_begin(void){
  float cx=FBW*0.5f, cy=(float)(GY+GH/2);
  fx_flash(0.45f);
  fx_shake(6.0f,0.35f);
  fx_sparks(cx,cy,opt_limiter?40:60,0xFFD24A,220.0f,800.0f,1);
  for(int i=0;i<14;i++) fx_spawn_star(cx+fxrs()*220.0f,cy+fxrs()*80.0f,0xFFF6D8,0.5f+fxr()*0.5f,1+(fxr()<0.4f),1);
  fx_spawn_flare(cx,cy,0xFFC040,0.45f,3,1);
}

/* === WIN PRESENTATION: DRAW SIDE ================================== */

/*  One lit win: every cell on a path gets a tinted wash and a lit frame;
 *  the featured win's symbols also swell out of their cells, and energy
 *  runs along the path lines, left to right, reel to reel.           */
static void fx_light_cluster(uint32_t m,uint32_t c,float pulse,int heavy,float popT){
  int lim=opt_limiter;
  for(int i=0;i<NCELL;i++){
    if(!((m>>i)&1u)) continue;
    int rr=i/NROW, row=i%NROW;
    int cx=cellcx(rr), cy=cellcy(row);
    fx_shade(cx,cy,CW/2+6,CH/2,c,(int)((heavy?0.48f:0.34f)*pulse*255));
    fx_rframe(GX+rr*CW+4,GY+row*CH+3,CW-8,CH-6,10,heavy?4:3,c,(int)(235*pulse));
    if(heavy) fx_rframe(GX+rr*CW+8,GY+row*CH+7,CW-16,CH-14,7,2,0xFFFFFF,(int)(150*pulse));
  }
  /* path lines under the symbols' pop, so the swollen art sits on top */
  float ph=G.t*1.5f; ph-=floorf(ph);
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
      fx_capsule(x0,y0,x1,y1,heavy?5.0f:4.0f,0x100C18,heavy?200:150);
      fx_capsule(x0,y0,x1,y1,heavy?2.6f:1.6f,c,(int)(150*pulse)+90);
    }
  }
  /* under a big-win celebration the reels sit in the dark: the pop and
     the running light would be drawn for nobody */
  int full=heavy && !(G.state==ST_SHOWWIN && G.banner>=1);
  if(full && popS[0][1].c){
    /* the pop: up to 118% in a tenth of a second, settle, then breathe */
    float s;
    if(popT<0.10f) s=1.0f+0.18f*(popT/0.10f);
    else if(popT<0.34f) s=1.18f-0.12f*((popT-0.10f)/0.24f);
    else s=1.06f+0.025f*sinf(G.t*6.0f);
    int k=clampi((int)((s-1.0f)/0.045f+0.5f),0,NPOP-1);
    int wht=popT<0.16f?(int)((1.0f-popT/0.16f)*(lim?50:90)):0;
    for(int i=0;i<NCELL;i++){
      if(!((m>>i)&1u)) continue;
      int rr=i/NROW, row=i%NROW, sy=G.grid[rr][row];
      int ax=GX+rr*CW+SOX+SYMW/2, ay=GY+row*CH+SOY+SYMH/2;
      if(k==0) continue;
      const fxspr_t*sp=&popS[sy][k];
      if(wht) fxs_blit(sp,(float)(ax-sp->ox),(float)(ay-sp->oy),1.0f,255,wht,FX_NOSHINE);
      else    fxs_blit1(sp,ax-sp->ox,ay-sp->oy,255);
    }
  }
  /* energy: beads of light riding every path segment in step, so the
     eye follows one wave across the reels */
  for(int i=0;i<NCELL && full;i++){
    if(!((m>>i)&1u)) continue;
    int rr=i/NROW, row=i%NROW;
    if(rr+1>=NREEL) continue;
    for(int dw=-1;dw<=1;dw++){
      int nw=row+dw;
      if(nw<0||nw>=NROW) continue;
      if(!((m>>((rr+1)*NROW+nw))&1u)) continue;
      float x0=(float)cellcx(rr), y0=(float)cellcy(row);
      float x1=(float)cellcx(rr+1), y1=(float)cellcy(nw);
      for(int b=0;b<(heavy?2:1);b++){
        float u=ph+b*0.5f; u-=floorf(u);
        for(int tl=3;tl>=0;tl--){
          float uu=u-tl*0.045f;
          if(uu<0) continue;
          float bx=x0+(x1-x0)*uu, by=y0+(y1-y0)*uu;
          int r=heavy?(tl?9-tl*2:12):(tl?6-tl:8);
          fx_shade((int)bx,(int)by,r,r,c,tl?120-tl*25:220);
          if(!tl) fx_glow((int)bx,(int)by,r-3,0xFFFFFF,heavy?255:180);
        }
      }
    }
  }
  /* the wilds in the win wear their multiplier, so the doubling is
     visible rather than something to work out from the total */
  for(int i=0;i<NCELL;i++){
    if(!((m>>i)&1u)) continue;
    if(G.grid[i/NROW][i%NROW]!=SY_SEVEN) continue;
    float bob=heavy?sinf(G.t*5.0f+i)*2.0f:0.0f;
    int bx=cellcx(i/NROW)+CW/2-46, by=cellcy(i%NROW)+CH/2-30+(int)bob;
    int a=(int)(255*clampf(pulse+0.3f,0,1));
    fx_plate(bx+2,by+3,40,24,8,0x000000,0x000000,a/2);
    fx_plate(bx-2,by-2,44,28,10,0xFFF0A0,0xB07810,a);   /* gold rim  */
    fx_plate(bx,by,40,24,8,0xE83424,0x5A0808,a);        /* red enamel */
    const fxspr_t*t=&titleS[FXT_BADGE];
    float sh=heavy?fx_shine_x(t,bx+20.0f,1.0f,G.t+i*0.3f,1.8f,0.5f):FX_NOSHINE;
    fx_title(t,bx+20.0f,by+12.0f,1.0f,a,0,sh);
    if(heavy){
      float tw=sinf(G.t*4.0f+i*1.7f);
      if(tw>0.2f) fxs_add(&starS[1],bx+37,by+3,0xFFF6D0,(int)((tw-0.2f)*1.25f*255*(lim?0.6f:1.0f)));
    }
  }
}

static void fx_wins_draw(void){
  /* a big-win celebration darkens the reels and owns the screen */
  if(G.state!=ST_SHOWWIN || G.nWin<=0 || G.banner>=1) return;
  int w=G.showIdx;
  float pulse=0.55f+0.45f*sinf(G.t*9.0f);
  if(opt_limiter) pulse=0.7f+pulse*0.3f;
  /* every other winning cluster stays faintly lit behind the featured
     one (not under a celebration, which darkens the reels anyway) */
  if(G.banner<1)
    for(int i=0;i<G.nWin;i++) if(i!=w) fx_light_cluster(G.winMask[i],WINCOL[i&7],0.35f,0,0.0f);
  fx_light_cluster(G.winMask[w],WINCOL[w&7],pulse,1,G.showT);
  /* the featured win's pay pops out over the middle of its cells */
  if(G.banner>=1) return;
  uint32_t m=G.winMask[w]; float sx=0,sy=0; int n=0;
  for(int c=0;c<NCELL;c++) if((m>>c)&1u){ sx+=cellcx(c/NROW); sy+=cellcy(c%NROW); n++; }
  if(!n) return;
  sx/=n; sy/=n;
  float t=G.showT;
  float sc=t<0.12f?0.30f+0.26f*(t/0.12f):0.50f+0.06f*expf(-(t-0.12f)*8.0f)*cosf((t-0.12f)*30.0f);
  float rise=fminf(t,0.8f)*14.0f;
  /* what it actually pays: in free spins the meter multiplies every win,
     so the pop shows the multiplied amount, matching the WIN meter and
     the LAST WIN panel's sum */
  long long amt=(long long)G.winAmt[w]*((G.inFree && G.fsMult>1)?G.fsMult:1);
  fx_number(amt,sx,sy-30.0f-rise,sc,255,t<0.12f?120:0);
}

static void fx_slam_title(const fxspr_t*s,float cx,float cy,float p,float t,float breathe,
                          uint32_t col,int glowA,float shine,const fxspr_t*old){
  float lim=opt_limiter?0.5f:1.0f, sc, white=0;
  if(p<0.16f){ float u=p/0.16f; sc=1.75f-0.81f*u*u; }
  else { float q=p-0.16f; sc=1.0f-0.07f*expf(-q*8.0f)*cosf(q*26.0f); }
  sc+=breathe*sinf(t*5.0f);
  if(p<0.36f) white=(1.0f-p/0.36f)*230.0f*lim;
  if(old && old->c && p<0.2f){
    float u=p/0.2f;
    fx_title(old,cx,cy,1.0f+u*0.5f,(int)((1.0f-u)*200),0,FX_NOSHINE);
  }
  if(p>0.12f && p<0.45f) fx_title_glow(s,cx,cy,sc,col,(int)(glowA*(1.0f-(p-0.12f)/0.33f)));
  if(p<0.16f){
    if(sc<1.45f) fx_title(s,cx,cy,sc*1.18f,70,0,FX_NOSHINE);
    if(sc<1.60f) fx_title(s,cx,cy,sc*1.07f,120,0,FX_NOSHINE);
  }
  fx_title(s,cx,cy,sc,255,(int)white,shine);
}

/*  A neon rule across the screen at row y: a tube in `col` with its glow
 *  above and below, and honey pulses chasing along it.  The chase is a
 *  pure function of t.  Colours come from two small tables built per
 *  call, so the pixel loops are an index, a blend and an add.        */
static void fx_neon_rule(int y,uint32_t col,int alpha,float t){
  if(alpha<=0) return;
  if(alpha>255) alpha=255;
  int y0=y-9, y1=y+10;
  if(!fx_clip(&y0,&y1)) return;
  uint32_t core[64], glow[64];
  uint32_t hot=lz_hot(col,0.62f), hhot=lz_hot(LZ_HONEY,0.55f);
  for(int i=0;i<64;i++){
    float b=i<16?1.0f-fabsf(i-7.5f)/8.0f:0.0f;
    core[i]=mixc(hot,hhot,b);
    glow[i]=mixc(col,LZ_HONEY,b);
  }
  int ph=(int)(t*(opt_limiter?140.0f:380.0f));
  int a256=alpha+(alpha>>7);
  for(int yy=y0;yy<y1;yy++){
    int dy=abs(yy-y);
    uint32_t*d=fb+(size_t)yy*FBW;
    if(dy<=1){
      for(int x=0;x<FBW;x++) d[x]=px_mix(d[x],core[((x+ph)>>2)&63],a256);
    } else {
      int g=(int)(alpha*0.60f*expf(-(dy-1)/2.6f));
      if(g<2) continue;
      for(int x=0;x<FBW;x++) d[x]=px_add(d[x],px_scale(glow[((x+ph)>>2)&63],g));
    }
  }
}

static void fx_bigwin_draw(void){
  if(G.state!=ST_SHOWWIN || G.banner<1) return;
  int T=clampi(G.banner,1,4);
  float t=G.t, a=clampf(t/0.30f,0,1);
  float p=1.0f-clampf(G.bannerT,0,1);            /* 0..1 since the last slam */
  /* the tier's neon; EPIC runs all three of the lounge's in turn */
  uint32_t col=T>=4?fx_neon_cycle(t*0.9f):BWCOL[T];
  const float cx=FBW*0.5f, cy=222.0f;
  /* the stage: a dark plate, god rays in the tier's neon behind the
     title, and the honey searchlights; a slam kicks the rays brighter */
  const fxpanel_t*P=&fxPanel[FXP_WIN];
  int px=FBW/2-P->w/2, py=300;
  float kick=p<0.5f?(1.0f-p/0.5f):0.0f;
  { fxstage_t st={ (int)cx,(int)cy+30, 0, 470,240,(int)(225*a),
                   120,400+T*15,14+T*4,(int)((90+20*T)*a+80*kick),T>=4?2:0,(int)(70*a+60*kick),
                   t*(0.22f+0.08f*T),0.6f, col,
                   (int)((116+14*T)*a), t, {FX_BEAM,FX_BEAM},
                   a>=1.0f?px+4:0, a>=1.0f?px+P->w-4:0, py+20, py+P->h-20 };
    fx_stage_draw(&st); }
  if(p<0.6f) fx_ring((int)cx,(int)cy,p*1100.0f,30.0f+p*30.0f,col,(int)(200*(1.0f-p/0.6f)));
  /* title */
  const fxspr_t*s=&titleS[FXT_BIG+T-1];
  const fxspr_t*old=T>1?&titleS[FXT_BIG+T-2]:NULL;
  float sh=fx_shine_x(s,cx,1.0f,t,2.4f,0.8f);
  fx_slam_title(s,cx,cy,p,t,0.02f,col,(int)(150+60*sinf(t*6.0f)),sh,old);
  /* the count on dark glass with the tier's neon round it */
  fx_panel_draw(P,px,py,col,(int)(255*a));
  fx_caption("TOTAL WIN",cx,(float)py+10.0f,24.0f,lz_hot(col,0.30f),a,5.0f);
  float ns=fx_number_fit(G.winShown,0.78f,520.0f)*(1.0f+(G.bannerT>0.7f?0.22f*(G.bannerT-0.7f)/0.3f:0.0f));
  float ny=py+74.0f-FXDPX*3.5f*ns;             /* centred on the glass */
  int done=G.winShown>=G.winTotal;
  fx_number_glow(G.winShown,cx,ny,ns,LZ_AMBER,(int)(150*a));
  fx_number(G.winShown,cx,ny,ns,(int)(255*a),p<0.2f?(int)(150*(1.0f-p/0.2f)):0);
  if(t>0.8f){
    float y=(float)(py+114);
    if(!done) fx_caption("PRESS ANY BUTTON TO SKIP",cx,y,24.0f,lz_hot(LZ_DIM,0.35f),0.9f,4.0f);
    else      fx_prompt("PRESS ANY BUTTON TO COLLECT",y,t);
  }
}

static void fx_jackpot_draw(void){
  int tier=G.jpWon<0?JP_MINOR:G.jpWon;
  int ult=(tier==JP_ULT);
  float t=G.t, a=clampf(t/0.25f,0,1), lim=opt_limiter?0.5f:1.0f;
  /* the tier's neon, as on the ladder; the ULTIMATE runs all three */
  uint32_t col=ult?fx_neon_cycle(t*0.9f):JPCOL[tier];
  const float cx=FBW*0.5f, cy=150.0f;
  const fxpanel_t*P=&fxPanel[FXP_WIN];
  int px=FBW/2-P->w/2, py=292;
  float pa=clampf((t-0.2f)/0.2f,0,1);
  { fxstage_t st={ (int)cx,(int)cy+60, 0, 540,290,(int)(235*a),
                   130,ult?520:470,ult?24:18,(int)((ult?150:120)*a),ult?2:0,(int)(100*a),
                   t*(ult?0.45f:0.3f),0.60f, col,
                   (int)((ult?170:150)*a), t, {FX_BEAM,FX_BEAM},
                   pa>=1.0f?px+4:0, pa>=1.0f?px+P->w-4:0, py+20, py+P->h-20 };
    fx_stage_draw(&st); }
  /* the winning cells come back through the dim, so the player sees
     exactly what did it */
  for(int c=0;c<NCELL;c++) if((G.jpMask>>c)&1u){
    int r=c/NROW,row=c%NROW;
    blit(&sym[G.grid[r][row]],GX+r*CW+SOX,GY+row*CH+SOY,GY,GY+GH,255,0,0.0f);
  }
  float pl=0.5f+0.5f*sinf(t*8.0f);
  fx_light_cluster(G.jpMask,col,0.7f+0.3f*pl,1,t);
  if(t<0.7f) fx_ring((int)cx,(int)cy,t*1300.0f,40.0f,col,(int)(220*(1.0f-t/0.7f)));
  if(ult && t>0.5f && t<1.3f) fx_ring((int)cx,(int)cy,(t-0.5f)*1300.0f,30.0f,0xFFFFFF,(int)(160*lim*(1.0f-(t-0.5f)/0.8f)));
  /* the band the celebration sits in, ruled by two neon tubes with
     honey light chasing along them */
  int by0=76, by1=472;
  fx_neon_rule(by0,col,(int)(235*a),t);
  fx_neon_rule(by1,col,(int)(235*a),t+1.7f);
  /* title: the tier word slams, JACKPOT follows it in */
  const fxspr_t*s=&titleS[FXT_JULT+tier];
  float sh=fx_shine_x(s,cx,1.0f,t-0.4f,2.0f,0.8f);
  fx_slam_title(s,cx,cy,clampf(t/1.0f,0,1),t,0.025f,col,(int)(170+70*sinf(t*6.0f)),sh,NULL);
  float jt=clampf((t-0.22f)/0.3f,0,1);
  if(jt>0){
    float js=jt<1.0f?1.0f+(1.0f-jt)*1.6f:1.0f;
    const fxspr_t*j=&titleS[FXT_JACKPOT];
    fx_title(j,cx,cy+90.0f,js,(int)(255*jt),jt<1.0f?(int)(160*lim):0,fx_shine_x(j,cx,1.0f,t-0.9f,2.0f,0.6f));
  }
  /* the meter rolls up to the prize, on dark glass in the tier's neon */
  fx_panel_draw(P,px,py,col,(int)(255*pa));
  fx_caption("JACKPOT PAYS",cx,(float)py+10.0f,24.0f,lz_hot(col,0.30f),pa,5.0f);
  float run=fx_jackpot_run(tier), u=clampf((t-0.3f)/(run-0.3f),0,1);
  u=1.0f-(1.0f-u)*(1.0f-u)*(1.0f-u);
  long long v=(long long)(G.jpAmt*(double)u);
  if(t>=run) v=G.jpAmt;
  float ns=fx_number_fit(v,0.78f,540.0f), ny=py+74.0f-FXDPX*3.5f*ns;
  fx_number_glow(v,cx,ny,ns,LZ_AMBER,(int)(150*pa));
  fx_number(v,cx,ny,ns,(int)(255*pa),0);
  if(t>1.4f) fx_prompt("PRESS ANY BUTTON TO COLLECT",(float)(py+114),t);
}

static void fx_multup_draw(void){
  float u=1.0f-G.multUp/1.9f;                 /* 0 at the notch, 1 at the end */
  float env=u<0.06f?u/0.06f:(u>0.80f?(1.0f-u)/0.20f:1.0f);
  env=clampf(env,0,1);
  const float cx=FBW*0.5f, cy=(float)(GY+GH/2);
  { fxstage_t st={ (int)cx,(int)cy+20, 0, 460,250,(int)(235*env),
                   100,360,16,(int)(130*env),0,(int)(70*env), G.t*1.3f,0.55f, LZ_HONEY, 0,0.0f,{0,0}, 0,0,0,0 };
    fx_stage_draw(&st); }
  if(u<0.35f) fx_ring((int)cx,(int)cy,u*1400.0f,24.0f,LZ_HONEY,(int)(200*(1.0f-u/0.35f)));
  /* the headline is a neon sign that drops in and lights */
  float ty=cy-92.0f-(1.0f-clampf(u/0.12f,0,1))*40.0f;
  fx_neon_sign("MULTIPLIER UP",cx,ty,56.0f,LZ_MAGENTA,env);
  int m=clampi(G.fsMult,2,5);
  const fxspr_t*s=&titleS[FXT_X2+m-2];
  /* the number slams, and once its sparks have left for the meter it
     shrinks away after them */
  float p=clampf(u/0.55f,0,1), shrink=u>0.62f?1.0f-(u-0.62f)*1.4f:1.0f;
  float sc, lim=opt_limiter?0.5f:1.0f, white=0;
  if(p<0.16f){ float q=p/0.16f; sc=2.4f-1.4f*q*q; }
  else { float q=p-0.16f; sc=1.0f-0.10f*expf(-q*7.0f)*cosf(q*24.0f); }
  if(p<0.4f) white=(1.0f-p/0.4f)*230.0f*lim;
  sc*=shrink;
  if(p>0.10f && p<0.40f) fx_title_glow(s,cx,cy+14.0f,sc,LZ_HONEY,(int)(220*env*(1.0f-(p-0.10f)/0.30f)));
  if(p<0.16f) fx_title(s,cx,cy+14.0f,sc*1.25f,(int)(80*env),0,FX_NOSHINE);
  fx_title(s,cx,cy+14.0f,sc,(int)(255*env),(int)white,FX_NOSHINE);

  /*  Say what just happened, in words: which way it moved and by how
   *  much (two or three wild reels in one spin climb two or three
   *  steps), what it multiplies, and that it will not come back down.
   *  On dark glass with a honey tube, fading in once the number has
   *  landed and out with the rest.                                    */
  float ta=clampf((u-0.08f)/0.06f,0,1)*env;
  if(ta>0.02f){
    char b[80];
    int from=clampi(G.multFrom,1,5), to=clampi(G.fsMult,1,5), up=to-from;
    const fxpanel_t*P=&fxPanel[FXP_MULT];
    int px=(int)cx-P->w/2, py=(int)cy+104;
    fx_panel_draw(P,px,py,LZ_HONEY,(int)(235*ta));
    if(up>1) snprintf(b,sizeof b,"%d WILD REELS:  X%d  >  X%d",up,from,to);
    else     snprintf(b,sizeof b,"WILD REEL:  X%d  >  X%d",from,to);
    fx_caption(b,cx,(float)py+8.0f,36.0f,LZ_GOLD,ta,2.0f);
    if(to>=FS_MAXMULT) snprintf(b,sizeof b,"MAXIMUM!  EVERY WIN PAYS X%d UNTIL THE END",to);
    else               snprintf(b,sizeof b,"THIS WIN AND EVERY WIN AFTER IT PAYS X%d",to);
    fx_caption(b,cx,(float)py+50.0f,26.0f,LZ_IVORY,ta,1.5f);
    fx_caption("THE MULTIPLIER NEVER GOES DOWN UNTIL THE FREE SPINS END",cx,(float)py+82.0f,19.0f,LZ_DIM,ta,3.0f);
  }
}
