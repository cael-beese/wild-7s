/* =====================================================================
 *  w7_extra.c - the base game's two extras.
 *
 *  7 STRIKE - a mystery wild storm.  About one base spin in eighty is
 *  secretly armed when it starts.  While those reels spin the storm
 *  builds: the reel window darkens, cloud rolls in over the marquee,
 *  sheet lightning flickers inside it, sparks crawl round the bezel and
 *  the rumble grows.  When the reels stop, three to eight bolts strike
 *  one after another, and every cell they hit becomes a WILD 7.  Then the
 *  spin is paid as normal - the wilds were in the grid all along.
 *
 *  GAMBLE - classic double or nothing on a counted base-game win.  A big
 *  face-down card on a felt table: LEFT bets RED, RIGHT bets BLACK (x2),
 *  UP/DOWN choose a suit and X plays it (x4), A/START collects.  Up to
 *  five in a row, with the last five cards dealt shown along the bottom.
 *
 *  Render contract: every *_draw function here reads G and the baked
 *  art only.  Clocks, RNG and effects are all driven from update code;
 *  the draw hashes stable inputs (storm seed, bolt index, time bucket)
 *  wherever it needs something that looks random.
 * ===================================================================== */

/* ══ 7 STRIKE: THE MATHS ═══════════════════════════════════════════
 *  Every wild in a path doubles it, so a handful of wilds dropped at
 *  random is a lottery: measured over the real evaluator, three random
 *  wilds add 8x the bet on average, four add 20x, eight add 290x, and a
 *  third of three-bolt storms still pay nothing.  That is neither
 *  affordable at one spin in eighty nor much fun.
 *
 *  So the storm is shaped.  The bolt count is drawn first (three to
 *  eight, weighted toward fewer).  Then STORM_K candidate placements of
 *  that many wilds are drawn over the eligible cells and each is scored
 *  with the game's own evaluate(); the storm takes the placement whose
 *  win is closest (in ratio) to a target that rises with the count.  More
 *  bolts therefore reliably mean more money, which is what makes every
 *  extra bolt worth cheering, and a storm almost never pays nothing.
 *  It all runs in extra_on_snapshot(), which the simulator calls through
 *  snapshot_grid(), so ./w7sim measures exactly what ships.
 *
 *  Measured (bet 10): a storm spin pays 4.1x the bet on average against
 *  0.35x for the same stops without it (median 1.8x, 1 in 6 over 5x,
 *  1 in 40 over 20x), and 92% of storms pay at least the bet.  At one
 *  base spin in eighty that is about 4.6% of RTP (./w7sim base game
 *  with the storm against -DSTORM_ODDS=0).  Mean payout by bolt count:
 *  3 bolts 1.4x, 4 2.2x, 5 3.7x, 6 6.7x, 7 12x, 8 23x.
 *
 *  Eligible cells: anything but a feature symbol (scatter, crown,
 *  jackpot, ultimate, coin, wheel) or a cell already wild, so the storm
 *  never takes a trigger away from the player.
 *  STORM_ODDS may be overridden at compile time: -DSTORM_ODDS=0 turns
 *  the feature off, which is how its RTP contribution is measured.     */
#ifndef STORM_ODDS
#define STORM_ODDS 80                   /* one base spin in this many */
#endif
#define STORM_K 24                      /* candidate placements scored */
static const int STORM_W[EX_MAXSTRIKE+1] = { 0,0,0, 34,26,18,12, 7, 3 };   /* weight   */
static const int STORM_T[EX_MAXSTRIKE+1] = { 0,0,0, 10,17,27,44,70,100 };  /* x bet/10 */

/* ══ GAMBLE: THE RULES ══════════════════════════════════════════════
 *  Offered on any base-game win up to GAMBLE_CAPX times the bet, and
 *  again after each win while the pot is still inside that cap.  A cap
 *  in bet multiples scales with the bet the way the jackpots do, so the
 *  game is the same at every rung; at 50x a single suit call can reach
 *  200x the bet at most, the MEGA jackpot's multiple, so the gamble never
 *  outshines the progressives.  Five rounds at most.
 *
 *  Odds are exactly fair: the suit is irnd(4), uniform to the bit (the
 *  generator's 24-bit fraction divides by four exactly), so red/black is
 *  exactly 1/2 at x2 and a suit exactly 1/4 at x4.  Every round has an
 *  expected value equal to the stake, and stopping rules cannot change
 *  the expectation of a fair game, so the gamble does not move the RTP:
 *  the simulator rightly ignores it.                                   */
#define GAMBLE_CAPX   50
#define GAMBLE_ROUNDS 5
enum { GP_NONE=0, GP_DEAL, GP_PICK, GP_FLIP, GP_RESULT, GP_OUT };
enum { GE_COLLECT=0, GE_LIMIT, GE_ROUNDS };

/* ── stable hashes, for anything a draw wants to look random ───────── */
static inline uint32_t ex_h(uint32_t x){
  x^=x>>16; x*=0x7FEB352Du; x^=x>>15; x*=0x846CA68Bu; x^=x>>16; return x;
}
static inline float ex_hf(uint32_t a,uint32_t b){
  return (float)(ex_h(a*0x9E3779B1u ^ ex_h(b+0x632BE5ABu)) & 0xFFFFFF)/16777216.0f;
}

/* ══ BAKED ART ══════════════════════════════════════════════════════
 *  Built once by extra_init() from retro_init(), like the symbols.     */
static int ex_ready;

#define CLW 1280                       /* cloud bank, tiles horizontally */
#define CLH 128
static uint8_t *clA, *clS;             /* alpha, shade                   */
static uint8_t clRowMax[CLH];          /* skip rows with nothing in them */
static uint8_t clW[FBW];               /* thinner toward the screen edge */
static spr_t stormWild;                /* WILD 7 on its blue backing, badge */
static spr_t pillSpr, plateSpr;        /* the gamble offer, the 7 STRIKE plate */
#define EGL 128
static uint8_t exGlow[EGL*EGL];        /* radial falloff, for additive glows */

#define CDW 190                        /* the big card                   */
#define CDH 266
#define MCW 46                         /* history cards                  */
#define MCH 64
static spr_t pipBig[4], pipMid[4], pipMini[4], pipSm[4], sevenSm;
static spr_t cardFace[4], cardBack, cardMini[4];
static uint32_t *tblImg;               /* the whole felt table, opaque   */

/* table layout: it sits exactly over the reel window and its bezel */
#define TX0 (GX-16)
#define TY0 (GY-16)
#define TW  (GW+32)
#define TH  (GH+32)
#define CARDY 176
#define PLW 176
#define PLH 170
#define PLY 222
#define REDX (FBW/2-95-24-PLW)
#define BLKX (FBW/2+95+24)
#define CHIPY 454
#define CHIPW 66
#define CHIPH 56
#define CHIPX0 (FBW/2-2*CHIPW-12)
#define LBLX 412                        /* centre of the left-hand labels  */
#define LBRX 868                        /* and of the right-hand ones      */
#define HISTY 532
#define HISTX0 490
#define COLX 762                        /* the COLLECT button               */
#define COLW 188

static const uint32_t ELECG[5] = {0xFFFFFF,0xE8F8FF,0x8FD8FF,0x2F7CF0,0x14307A};
static const char*RANKS[13] = {"A","2","3","4","5","6","7","8","9","10","J","Q","K"};

/* box-filter the symbol canvas down by an integer factor */
static void ex_resolve(spr_t*s,int f){
  int w=CANW/f, h=CANH/f;
  s->w=w; s->h=h; s->rx0=s->rx1=NULL;
  s->px=(uint8_t*)calloc((size_t)w*h,4);
  if(!s->px){ s->w=s->h=0; return; }
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    int r=0,g=0,b=0,a=0;
    for(int j=0;j<f;j++) for(int i=0;i<f;i++){
      const uint8_t*p=canvas+(((y*f+j)*CANW)+(x*f+i))*4;
      r+=p[0]*p[3]; g+=p[1]*p[3]; b+=p[2]*p[3]; a+=p[3];
    }
    uint8_t*o=s->px+((size_t)y*w+x)*4;
    if(a>0){ o[0]=(uint8_t)(r/a); o[1]=(uint8_t)(g/a); o[2]=(uint8_t)(b/a); }
    o[3]=(uint8_t)(a/(f*f));
  }
  spr_bounds(s);
}

static int ex_spr_new(spr_t*s,int w,int h){
  s->w=w; s->h=h; s->rx0=s->rx1=NULL;
  s->px=(uint8_t*)calloc((size_t)w*h,4);
  if(!s->px){ s->w=s->h=0; return 0; }
  return 1;
}
/* straight-alpha "over" into a sprite */
static void ex_px_over(spr_t*s,int x,int y,uint32_t col,int a){
  if(x<0||y<0||x>=s->w||y>=s->h||a<=0) return;
  uint8_t*p=s->px+((size_t)y*s->w+x)*4;
  if(a>255) a=255;
  int da=p[3], oa=a+da*(255-a)/255;
  if(oa<=0) return;
  p[0]=(uint8_t)(((int)((col>>16)&255)*a + p[0]*da*(255-a)/255)/oa);
  p[1]=(uint8_t)(((int)((col>>8)&255)*a  + p[1]*da*(255-a)/255)/oa);
  p[2]=(uint8_t)(((int)(col&255)*a       + p[2]*da*(255-a)/255)/oa);
  p[3]=(uint8_t)oa;
}
static void ex_over(spr_t*d,const spr_t*s,int ox,int oy,int alpha,int rot){
  if(!s->px||!d->px) return;
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++){
    const uint8_t*p=s->px+((size_t)y*s->w+x)*4;
    if(!p[3]) continue;
    int dx=rot? ox+s->w-1-x : ox+x, dy=rot? oy+s->h-1-y : oy+y;
    ex_px_over(d,dx,dy,RGB(p[0],p[1],p[2]),p[3]*alpha/255);
  }
}
/* a rounded card blank: vertical gradient, a crisp darker edge */
static void ex_blank(spr_t*s,int w,int h,float rad,uint32_t top,uint32_t bot){
  if(!ex_spr_new(s,w,h)) return;
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    float d=rr_sdf(x+0.5f,y+0.5f,w*0.5f,h*0.5f,w*0.5f,h*0.5f,rad);
    float cov=clampf(0.5f-d,0,1);
    if(cov<=0) continue;
    uint32_t c=mixc(top,bot,(float)y/(h-1));
    c=mixc(c,0x4A4238,clampf(1.0f+d/1.6f,0,1)*0.8f);
    uint8_t*o=s->px+((size_t)y*w+x)*4;
    o[0]=(uint8_t)(c>>16); o[1]=(uint8_t)(c>>8); o[2]=(uint8_t)c; o[3]=(uint8_t)(cov*255);
  }
}

/* ── the four suits, drawn on the symbol canvas ──────────────────── */
static void ex_heart_pts(pt_t*p,int n,float cx,float cy,float s,int flip){
  for(int i=0;i<n;i++){
    float t=TAU*i/n, st=sinf(t);
    float x=16.0f*st*st*st;
    float y=-(13.0f*cosf(t)-5.0f*cosf(2*t)-2.0f*cosf(3*t)-cosf(4*t));
    if(flip) y=-y;
    p[i].x=U(cx+x*s); p[i].y=U(cy+y*s);
  }
}
static void ex_stem(uint32_t edge,const uint32_t*g){
  pt_t st[5]={{U(46),U(50)},{U(51),U(71)},{U(61),U(87)},{U(31),U(87)},{U(41),U(71)}};
  cv_poly_outline(st,5,U(2.0f),edge,255);
  cv_polyN(st,5,g,4,255);
}
static void ex_pip_art(int suit){
  static const uint32_t RED[4]={0xFFA4A4,0xF02A3C,0xA80C20,0x5A0612};
  static const uint32_t BLK[4]={0x8A90A8,0x353A50,0x12141E,0x020204};
  const uint32_t*g = suit<2?RED:BLK;
  uint32_t edge = suit<2?0x3A0006:0x000000;
  pt_t p[64];
  cv_clear();
  switch(suit){
  case 0:                                            /* hearts   */
    ex_heart_pts(p,60,46,39.5f,2.55f,0);
    cv_poly_outline(p,60,U(2.0f),edge,255);
    cv_polyN(p,60,g,4,255);
    cv_ellipse(U(29),U(25),U(7),U(4.5f),0xFFFFFF,0xFFFFFF,90);
    break;
  case 1: {                                          /* diamonds */
    static const float DC[4][2]={{46,4},{80,46},{46,88},{12,46}};
    int n=0;
    for(int k=0;k<4;k++) for(int j=0;j<4;j++){
      float t=j/4.0f, bow=5.0f*sinf(t*3.14159f);
      float x=DC[k][0]+(DC[(k+1)&3][0]-DC[k][0])*t, y=DC[k][1]+(DC[(k+1)&3][1]-DC[k][1])*t;
      float vx=46-x, vy=46-y, l=sqrtf(vx*vx+vy*vy); if(l<0.01f) l=1;
      p[n].x=U(x+vx/l*bow); p[n].y=U(y+vy/l*bow); n++;
    }
    cv_poly_outline(p,n,U(2.0f),edge,255);
    cv_polyN(p,n,g,4,255);
    cv_ellipse(U(38),U(29),U(5),U(4),0xFFFFFF,0xFFFFFF,90);
    break; }
  case 2: {                                          /* clubs    */
    static const float CC[3][2]={{46,25},{25,52},{67,52}};
    ex_stem(edge,g);
    for(int k=0;k<3;k++) cv_circle(U(CC[k][0]),U(CC[k][1]),U(20.3f),edge,edge,255);
    cv_circle(U(46),U(46),U(13),edge,edge,255);
    for(int k=0;k<3;k++) cv_circle(U(CC[k][0]),U(CC[k][1]),U(18.3f),g[0],g[2],255);
    cv_circle(U(46),U(46),U(11),g[1],g[2],255);
    cv_ellipse(U(40),U(16),U(6),U(4),0xFFFFFF,0xFFFFFF,90);
    break; }
  default:                                           /* spades   */
    ex_stem(edge,g);
    ex_heart_pts(p,60,46,46.0f,2.25f,1);
    cv_poly_outline(p,60,U(2.0f),edge,255);
    cv_polyN(p,60,g,4,255);
    cv_ellipse(U(36),U(32),U(6),U(4),0xFFFFFF,0xFFFFFF,80);
    break;
  }
  ex_resolve(&pipBig[suit],3);
  ex_resolve(&pipMid[suit],8);
  ex_resolve(&pipMini[suit],12);
  ex_resolve(&pipSm[suit],16);
}

static void ex_build_cards(void){
  for(int s=0;s<4;s++) ex_pip_art(s);
  { static const uint32_t gold[5]={0xFFFBE0,0xFFDE78,0xD8A01C,0x8E6208,0x503802};
    cv_clear(); seven_shape(46,48,0.95f,gold,5,0); ex_resolve(&sevenSm,18); }

  /* faces: ivory stock, a gold pinstripe, one big pip, corner pips */
  for(int s=0;s<4;s++){
    spr_t*c=&cardFace[s];
    ex_blank(c,CDW,CDH,15.0f,0xFFFFFF,0xEAE2CF);
    if(!c->px) continue;
    for(int y=0;y<CDH;y++) for(int x=0;x<CDW;x++){
      float d=rr_sdf(x+0.5f,y+0.5f,CDW*0.5f,CDH*0.5f,CDW*0.5f-9,CDH*0.5f-9,8.0f);
      float v=clampf(1.0f-fabsf(d)/0.9f,0,1);
      if(v>0) ex_px_over(c,x,y,0xC49A40,(int)(v*210));
    }
    ex_over(c,&pipBig[s],CDW/2-pipBig[s].w/2,CDH/2-pipBig[s].h/2+10,255,0);
    ex_over(c,&pipSm[s],16,44,255,0);
    ex_over(c,&pipSm[s],CDW-16-pipSm[s].w,CDH-44-pipSm[s].h,255,1);
    spr_bounds(c);

    spr_t*m=&cardMini[s];
    ex_blank(m,MCW,MCH,6.0f,0xFFFFFF,0xE6DDC8);
    ex_over(m,&pipMini[s],MCW/2-pipMini[s].w/2,MCH-pipMini[s].h-6,255,0);
    spr_bounds(m);
  }

  /* the back: crimson lattice of gold sevens, the WILD 7 medallion */
  spr_t*b=&cardBack;
  ex_blank(b,CDW,CDH,15.0f,0xF8F2E4,0xE6DCC6);
  if(!b->px) return;
  const float cx=CDW*0.5f, cy=CDH*0.5f;
  for(int y=0;y<CDH;y++) for(int x=0;x<CDW;x++){
    float d=rr_sdf(x+0.5f,y+0.5f,cx,cy,cx-9,cy-9,9.0f);
    float cov=clampf(0.5f-d,0,1);
    if(cov<=0) continue;
    uint32_t c=mixc(0xB8162A,0x520712,(float)y/CDH);
    float u=(x+y)/22.0f, v=(x-y+400)/22.0f;
    float du=fabsf(u-floorf(u+0.5f))*22.0f*0.7071f, dv=fabsf(v-floorf(v+0.5f))*22.0f*0.7071f;
    float ln=fmaxf(clampf(1.0f-du/1.0f,0,1),clampf(1.0f-dv/1.0f,0,1));
    c=mixc(c,0xF0C860,ln*0.50f);
    c=mixc(c,0xF4CC5A,clampf(1.0f+d/2.4f,0,1));             /* gold rule */
    ex_px_over(b,x,y,c,(int)(cov*255));
  }
  for(int k=-12;k<24;k++) for(int m=-12;m<24;m++){
    float u=k+0.5f, v=m+0.5f;
    float px=((u+v)*22.0f-400.0f)*0.5f, py=((u-v)*22.0f+400.0f)*0.5f;
    if(px<20||px>CDW-20||py<20||py>CDH-20) continue;
    float ex=(px-cx)/70.0f, ey=(py-cy)/92.0f;
    if(ex*ex+ey*ey<1.0f) continue;
    ex_over(b,&sevenSm,(int)px-sevenSm.w/2,(int)py-sevenSm.h/2,150,0);
  }
  for(int y=0;y<CDH;y++) for(int x=0;x<CDW;x++){             /* medallion plate */
    float ex=(x+0.5f-cx)/62.0f, ey=(y+0.5f-cy)/82.0f;
    float r=sqrtf(ex*ex+ey*ey);
    if(r>1.0f) continue;
    float cov=clampf((1.0f-r)*62.0f,0,1);
    uint32_t c=mixc(0x3A0810,0x120204,r);
    if(r>0.90f) c=mixc(0xFFE9A0,0x8A5A10,clampf((r-0.90f)/0.10f,0,1));
    ex_px_over(b,x,y,c,(int)(cov*255));
  }
  if(sym[SY_SEVEN].px) ex_over(b,&sym[SY_SEVEN],(int)cx-sym[SY_SEVEN].w/2+2,(int)cy-sym[SY_SEVEN].h/2+2,255,0);
  for(int y=0;y<CDH;y++) for(int x=0;x<CDW;x++){             /* a sheen */
    uint8_t*p=b->px+((size_t)y*CDW+x)*4;
    if(!p[3]) continue;
    float t=(x+y*0.6f-40.0f)/60.0f;
    if(t<0||t>1) continue;
    ex_px_over(b,x,y,0xFFFFFF,(int)(sinf(t*3.14159f)*34.0f));
  }
  spr_bounds(b);
}

/* ── the cloud bank ─────────────────────────────────────────────────
 *  Built the way a matte painter blocks in cumulus: a few hundred soft
 *  spheres, big and dense along the top, smaller and sparser lower down
 *  so the base breaks into billows.  Each pixel belongs to the nearest
 *  sphere in front (a z-buffer), is lit by that sphere's normal from the
 *  upper left, and gets a little value noise so no two puffs match.
 *  The bank tiles horizontally so it can drift forever.               */
static float ex_vnoise(float x,float y,int per,uint32_t oct){
  int ix=(int)floorf(x), iy=(int)floorf(y);
  float fx=x-ix, fy=y-iy;
  fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy);
  #define LV(a,b) ((float)(ex_h((uint32_t)((((a)%per)+per)%per)*73856093u ^ (uint32_t)(b)*19349663u ^ oct*83492791u)&0xFFFF)/65535.0f)
  float v00=LV(ix,iy), v10=LV(ix+1,iy), v01=LV(ix,iy+1), v11=LV(ix+1,iy+1);
  #undef LV
  return lerpf(lerpf(v00,v10,fx),lerpf(v01,v11,fx),fy);
}
static void ex_build_clouds(void){
  float*Z=(float*)malloc(sizeof(float)*CLW*CLH);
  float*L=(float*)malloc(sizeof(float)*CLW*CLH);
  float*A=(float*)malloc(sizeof(float)*CLW*CLH);
  clA=(uint8_t*)malloc(CLW*CLH); clS=(uint8_t*)malloc(CLW*CLH);
  if(!Z||!L||!A||!clA||!clS){ free(Z); free(L); free(A); free(clA); free(clS); clA=clS=NULL; return; }
  for(int i=0;i<CLW*CLH;i++){ Z[i]=-1e9f; L[i]=0; A[i]=0; }
  const float lx=-0.45f, ly=-0.70f, lz=0.55f;
  /* three tiers of puffs: the ceiling, the body, the hanging billows */
  static const struct { int n; float y0,y1,r0,r1; } TIER[3]={
    { 50, -34, 18, 52, 86 }, { 80, 12, 60, 30, 52 }, { 70, 48, 94, 16, 34 } };
  uint32_t sd=0xC10D0u;
  for(int t=0;t<3;t++) for(int i=0;i<TIER[t].n;i++){
    sd++;
    float cx=ex_hf(sd,1)*CLW, cy=lerpf(TIER[t].y0,TIER[t].y1,ex_hf(sd,2));
    float r=lerpf(TIER[t].r0,TIER[t].r1,ex_hf(sd,3)), z0=ex_hf(sd,4)*20.0f-t*6.0f;
    int ya=(int)(cy-r), yb=(int)(cy+r)+1;
    if(ya<0) ya=0;
    if(yb>CLH) yb=CLH;
    for(int y=ya;y<yb;y++) for(int xx=(int)(cx-r);xx<=(int)(cx+r);xx++){
      float dx=(xx+0.5f-cx)/r, dy=(y+0.5f-cy)/r, d2=dx*dx+dy*dy;
      if(d2>=1.0f) continue;
      float nz=sqrtf(1.0f-d2), z=z0+nz*r;
      int x=((xx%CLW)+CLW)%CLW, k=y*CLW+x;
      float edge=clampf((1.0f-sqrtf(d2))*r/3.0f,0,1);      /* soft rim */
      if(edge>A[k]) A[k]=edge;
      if(z<=Z[k]) continue;
      Z[k]=z;
      L[k]=clampf(0.50f+0.50f*(dx*lx+dy*ly+nz*lz),0,1);   /* wrapped: soft */
    }
  }
  /* soften the seams where one sphere's lighting meets the next: a
     separable box blur of the light, wrapping horizontally */
  for(int pass=0;pass<2;pass++){
    const int R=4;
    for(int y=0;y<CLH;y++){
      float acc=0; float*row=L+y*CLW;
      for(int x=-R;x<=R;x++) acc+=row[(x+CLW)%CLW];
      for(int x=0;x<CLW;x++){
        Z[y*CLW+x]=acc/(2*R+1);
        acc+=row[(x+R+1)%CLW]-row[(x-R+CLW)%CLW];
      }
    }
    for(int x=0;x<CLW;x++){
      float acc=0;
      for(int y=-R;y<=R;y++) acc+=Z[clampi(y,0,CLH-1)*CLW+x];
      for(int y=0;y<CLH;y++){
        L[y*CLW+x]=acc/(2*R+1);
        acc+=Z[clampi(y+R+1,0,CLH-1)*CLW+x]-Z[clampi(y-R,0,CLH-1)*CLW+x];
      }
    }
  }
  for(int y=0;y<CLH;y++) for(int x=0;x<CLW;x++){
    int k=y*CLW+x;
    float fy=(float)y/CLH;
    float n=ex_vnoise(x/32.0f,y/20.0f,CLW/32,7)*0.7f+ex_vnoise(x/10.0f,y/8.0f,CLW/10,8)*0.3f;
    float sh=clampf(-0.12f+0.95f*L[k]*(0.62f+0.76f*n)-0.10f*(1.0f-fy),0,1);
    float a=A[k]*clampf(1.25f-fy*0.55f,0,1);
    clA[k]=(uint8_t)(clampf(a,0,1)*255); clS[k]=(uint8_t)(sh*255);
  }
  free(Z); free(L); free(A);
  for(int y=0;y<CLH;y++){
    int m=0;
    for(int x=0;x<CLW;x++) if(clA[y*CLW+x]>m) m=clA[y*CLW+x];
    clRowMax[y]=(uint8_t)m;
  }
  for(int x=0;x<FBW;x++){
    float u=(x-FBW*0.5f)/(FBW*0.5f);
    clW[x]=(uint8_t)(255*(1.0f-0.30f*u*u));
  }
}

/* ── the felt table, painted once through the normal primitives ───── */
static void ex_plate(int x,int y,int w,int h,uint32_t hi,uint32_t lo,uint32_t rim){
  fb_rrect(x+5,y+7,w,h,16,0x000000,150);
  fb_rrectg(x,y,w,h,16,hi,lo,255);
  for(int j=0;j<h/3;j++) for(int i=8;i<w-8;i++) fb_blend(x+i,y+3+j,0xFFFFFF,(h/3-j)*2);
  fb_rframe(x,y,w,h,16,3.0f,rim,255);
  fb_rframe(x+6,y+6,w-12,h-12,11,1.0f,scalec(rim,0.55f),220);
}
static void ex_build_table(void){
  const int x0=TX0, y0=TY0, w=TW, h=TH;
  fb_rrect(x0,y0,w,h,20,0x000000,255);
  /* felt: a spotlit green with a fine nap */
  for(int y=y0;y<y0+h;y++) for(int x=x0;x<x0+w;x++){
    float d=rr_sdf(x+0.5f,y+0.5f,x0+w*0.5f,y0+h*0.5f,w*0.5f-13,h*0.5f-13,12.0f);
    float cov=clampf(0.5f-d,0,1);
    if(cov<=0) continue;
    float dx=(x-FBW*0.5f)/430.0f, dy=(y-(y0+250.0f))/390.0f;
    float v=clampf(1.0f-(dx*dx+dy*dy),0,1);
    uint32_t c=mixc(0x03200E,0x178C4E,0.08f+0.92f*v*v);
    float sh=clampf(1.0f+d/16.0f,0,1);               /* the rail's shadow */
    c=scalec(c,1.0f-0.62f*sh*sh);
    int n=(int)(ex_h((uint32_t)(x*7919+y*104729))&15)-8;
    if(((x+y)&3)==0) n-=3;
    int r=clampi((int)((c>>16)&255)+n/2,0,255), g=clampi((int)((c>>8)&255)+n,0,255), b=clampi((int)(c&255)+n/2,0,255);
    fb_blend(x,y,RGB(r,g,b),(int)(cov*255));
  }
  /* rail: brushed chrome, a gold rule, a dark inner step - like the reels */
  for(int k=0;k<7;k++){
    float v=0.30f+0.70f*fabsf(cosf(k*0.62f));
    fb_rframe(x0+1+k,y0+1+k,w-2-2*k,h-2-2*k,19.0f-k,1.0f,mixc(0x333A4C,0xF2F6FF,v),255);
  }
  fb_rframe(x0+8,y0+8,w-16,h-16,14,3.0f,0xF0C24A,255);
  fb_rframe(x0+12,y0+12,w-24,h-24,12,1.5f,0x3A2804,255);
  /* printed on the felt */
  fb_rframe(x0+26,y0+26,w-52,h-52,10,1.2f,0xD8B050,70);
  fb_rframe(FBW/2-CDW/2-6,CARDY-6,CDW+12,CDH+12,20,1.5f,0xE8C060,90);
  led_window(FBW/2-126,118,252,46);
  text("YOUR WIN",REDX+PLW/2,134,2,0xE8D8A8,1,1);
  /* the two colour bets */
  ex_plate(REDX,PLY,PLW,PLH,0xE0283A,0x520612,0xFFD24A);
  textb("RED",REDX+PLW/2,PLY+14,5,SILVERG,4,1);
  blit(&pipMid[0],REDX+PLW/2-pipMid[0].w-3,PLY+62,0,FBH,255,0,0.0f);
  blit(&pipMid[1],REDX+PLW/2+3,PLY+62,0,FBH,255,0,0.0f);
  text("PAYS X2",REDX+PLW/2,PLY+124,2,0xFFE9A8,1,1);
  text("<  LEFT",REDX+PLW/2,PLY+PLH+10,2,0xCFE8D8,1,1);
  ex_plate(BLKX,PLY,PLW,PLH,0x4A5068,0x0A0B12,0xFFD24A);
  textb("BLACK",BLKX+PLW/2,PLY+14,5,SILVERG,4,1);
  blit(&pipMid[2],BLKX+PLW/2-pipMid[2].w-3,PLY+62,0,FBH,255,0,0.0f);
  blit(&pipMid[3],BLKX+PLW/2+3,PLY+62,0,FBH,255,0,0.0f);
  text("PAYS X2",BLKX+PLW/2,PLY+124,2,0xFFE9A8,1,1);
  text("RIGHT  >",BLKX+PLW/2,PLY+PLH+10,2,0xCFE8D8,1,1);
  /* the suit call */
  text("SUIT PAYS X4",LBLX,CHIPY+8,2,0xFFE9A8,1,1);
  text("UP / DOWN",LBLX,CHIPY+32,2,0xCFE8D8,1,1);
  text("X PLAYS IT",LBRX,CHIPY+8,2,0xFFE9A8,1,1);
  text("LAST CARDS",LBLX,HISTY+24,2,0xCFE8D8,1,1);
  for(int i=0;i<4;i++){                              /* the four suit chips */
    int x=CHIPX0+i*(CHIPW+8), y=CHIPY;
    fb_rrect(x+3,y+5,CHIPW,CHIPH,12,0x000000,130);
    fb_rrectg(x,y,CHIPW,CHIPH,12,0xFFF6DA,0xD2C6A2,255);
    blit(&pipMini[i],x+CHIPW/2-pipMini[i].w/2,y+CHIPH/2-pipMini[i].h/2,0,FBH,255,0,0.0f);
    fb_rframe(x,y,CHIPW,CHIPH,12,1.5f,0x5A5040,255);
  }
  { int x=COLX, y=HISTY, bw=COLW, bh=MCH;               /* COLLECT */
    fb_rrect(x+3,y+6,bw,bh,14,0x000000,160);
    fb_rrectg(x,y,bw,bh,14,0xFFF0B8,0xC08A10,255);
    for(int j=0;j<bh/3;j++) for(int i=8;i<bw-8;i++) fb_blend(x+i,y+2+j,0xFFFFFF,(bh/3-j)*3);
    fb_rframe(x,y,bw,bh,14,2.0f,0x3A2404,255);
    text("A COLLECT",x+bw/2,y+12,3,0x2A1400,1,0); }

  tblImg=(uint32_t*)malloc((size_t)w*h*4);
  if(tblImg) for(int y=0;y<h;y++) memcpy(tblImg+(size_t)y*w,fb+(size_t)(y0+y)*FBW+x0,(size_t)w*4);
}

/*  Paint a rounded panel with the ordinary primitives on a scratch patch
 *  of the frame (init time, so the next render covers it), then lift it
 *  into a sprite whose alpha is the panel's own outline.  Panels that
 *  never change are then one blit instead of a stack of distance-field
 *  rounded rects every frame.                                          */
static void ex_bake_panel(spr_t*s,int w,int h,float rad,void(*paint)(int,int,int,int)){
  if(!ex_spr_new(s,w,h)) return;
  for(int y=0;y<h;y++) memset(fb+(size_t)y*FBW,0,(size_t)w*4);
  paint(0,0,w,h);
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    float d=rr_sdf(x+0.5f,y+0.5f,w*0.5f,h*0.5f,w*0.5f,h*0.5f,rad);
    float cov=clampf(0.5f-d,0,1);
    uint32_t c=fb[(size_t)y*FBW+x];
    uint8_t*o=s->px+((size_t)y*w+x)*4;
    o[0]=(uint8_t)(c>>16); o[1]=(uint8_t)(c>>8); o[2]=(uint8_t)c; o[3]=(uint8_t)(cov*255);
  }
  spr_bounds(s);
}
/*  Lift anything the primitives can paint into a sprite with true
 *  alpha: paint it once on black and once on white.  Every primitive is
 *  a blend over what is underneath, so the result is linear in the
 *  background - on black it is colour times coverage, and the white
 *  pass shows how much background is left.  Used for the big title,
 *  whose bubble type is far too costly to rasterise mid-storm.         */
static void ex_bake_alpha(spr_t*s,int w,int h,void(*paint)(void)){
  if(!ex_spr_new(s,w,h)) return;
  uint32_t*b0=(uint32_t*)malloc((size_t)w*h*4);
  if(!b0){ free(s->px); s->px=NULL; s->w=s->h=0; return; }
  for(int y=0;y<h;y++) memset(fb+(size_t)y*FBW,0,(size_t)w*4);
  paint();
  for(int y=0;y<h;y++) memcpy(b0+(size_t)y*w,fb+(size_t)y*FBW,(size_t)w*4);
  for(int y=0;y<h;y++) for(int x=0;x<w;x++) fb[(size_t)y*FBW+x]=0xFFFFFF;
  paint();
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    uint32_t c0=b0[(size_t)y*w+x], c1=fb[(size_t)y*FBW+x];
    int left=((int)((c1>>16)&255)-(int)((c0>>16)&255) + (int)((c1>>8)&255)-(int)((c0>>8)&255)
             + (int)(c1&255)-(int)(c0&255))/3;             /* background left */
    int a=clampi(255-left,0,255);
    uint8_t*o=s->px+((size_t)y*w+x)*4;
    if(a>0){
      o[0]=(uint8_t)clampi((int)((c0>>16)&255)*255/a,0,255);
      o[1]=(uint8_t)clampi((int)((c0>>8)&255)*255/a,0,255);
      o[2]=(uint8_t)clampi((int)(c0&255)*255/a,0,255);
    }
    o[3]=(uint8_t)a;
  }
  free(b0);
  spr_bounds(s);
}
#define TITW 600
#define TITH 124
static spr_t titleSpr;                  /* the big 7 STRIKE */
static void ex_paint_title(void){ textb("7 STRIKE",TITW/2,16,11,ELECG,5,1); }

static void ex_paint_pill(int x,int y,int w,int h){
  fb_rrectg(x,y,w,h,h*0.5f,0xFFF6CC,0xC08A10,255);
  for(int j=0;j<h/3;j++) for(int i=h/2;i<w-h/2;i++) fb_blend(x+i,y+2+j,0xFFFFFF,(h/3-j)*6);
  fb_rframe(x,y,w,h,h*0.5f,2.0f,0x5E3A04,255);
  blit(&pipMini[0],x+14,y+h/2-pipMini[0].h/2,0,FBH,255,0,0.0f);
  blit(&pipMini[3],x+w-14-pipMini[3].w,y+h/2-pipMini[3].h/2,0,FBH,255,0,0.0f);
  text("X = GAMBLE",x+w/2-50,y+h/2-10,3,0x2A1400,1,0);
  text("DOUBLE",x+w/2+98,y+h/2-12,1,0x4A2A04,1,0);
  text("OR NOTHING",x+w/2+98,y+h/2+2,1,0x4A2A04,1,0);
}
static void ex_paint_plate(int x,int y,int w,int h){
  fb_rrectg(x,y,w,h,14,0x0C1838,0x02040E,255);
  fb_rframe(x,y,w,h,14,2.5f,0x4F9CFF,255);
  fb_rframe(x+5,y+5,w-10,h-10,10,1.0f,0x1A3A7A,255);
  textb("7 STRIKE",x+186,y+10,4,ELECG,5,1);
}
/* a thick antialiased stroke straight into a sprite (init only) */
static void ex_spr_line(spr_t*s,float x0,float y0,float x1,float y1,float r,uint32_t col){
  float dx=x1-x0, dy=y1-y0, L2=dx*dx+dy*dy; if(L2<0.01f) L2=0.01f;
  for(int y=(int)(fminf(y0,y1)-r-1);y<=(int)(fmaxf(y0,y1)+r+1);y++)
    for(int x=(int)(fminf(x0,x1)-r-1);x<=(int)(fmaxf(x0,x1)+r+1);x++){
      float px=x+0.5f-x0, py=y+0.5f-y0, t=clampf((px*dx+py*dy)/L2,0,1);
      float ex=px-t*dx, ey=py-t*dy, c=clampf(r+0.5f-sqrtf(ex*ex+ey*ey),0,1);
      if(c>0) ex_px_over(s,x,y,col,(int)(c*255));
    }
}
/* the storm's WILD 7: blue-lit backing, the seven, the lightning badge */
static void ex_build_stormwild(void){
  spr_t*s=&stormWild;
  if(!ex_spr_new(s,CW,CH)) return;
  const float R=CW*0.58f;
  for(int y=0;y<CH;y++) for(int x=0;x<CW;x++){
    float dx=(x+0.5f-CW*0.5f)/R, dy=(y+0.5f-CH*0.5f)/R;
    float v=clampf(1.0f-sqrtf(dx*dx+dy*dy),0,1); v*=v;
    ex_px_over(s,x,y,0x6FC8FF,(int)(v*0.46f*255));
  }
  for(int y=0;y<CH;y++) for(int x=0;x<CW;x++){          /* the lit cell rim */
    float d=rr_sdf(x+0.5f,y+0.5f,CW*0.5f,CH*0.5f,CW*0.5f-4,CH*0.5f-3,10.0f);
    float v=clampf(1.75f-fabsf(d+1.4f),0,1);
    if(v>0) ex_px_over(s,x,y,0x8FD8FF,(int)(v*235));
  }
  if(sym[SY_SEVEN].px) ex_over(s,&sym[SY_SEVEN],SOX,SOY,255,0);
  static const float BZ[4][2]={{20,9},{13,20},{19,20},{12,31}};
  for(int k=0;k<3;k++) ex_spr_line(s,BZ[k][0],BZ[k][1],BZ[k+1][0],BZ[k+1][1],2.6f,0x0A1430);
  for(int k=0;k<3;k++) ex_spr_line(s,BZ[k][0],BZ[k][1],BZ[k+1][0],BZ[k+1][1],1.2f,0xFFF27A);
  spr_bounds(s);
}

static void extra_init(void){
  if(ex_ready) return;
  for(int y=0;y<EGL;y++) for(int x=0;x<EGL;x++){
    float dx=(x+0.5f)/(EGL*0.5f)-1.0f, dy=(y+0.5f)/(EGL*0.5f)-1.0f;
    float v=clampf(1.0f-sqrtf(dx*dx+dy*dy),0,1);
    exGlow[y*EGL+x]=(uint8_t)(v*v*255);
  }
  ex_build_clouds();
  ex_build_cards();
  ex_build_table();
  ex_build_stormwild();
  ex_bake_panel(&pillSpr,420,40,20.0f,ex_paint_pill);
  ex_bake_panel(&plateSpr,520,48,14.0f,ex_paint_plate);
  ex_bake_alpha(&titleSpr,TITW,TITH,ex_paint_title);
  /* the cards cast their shadow from the sprite itself, so it turns with
     them and costs nothing extra at run time */
  for(int s=0;s<4;s++) bake_shadow(&cardFace[s],8,10,5,150);
  bake_shadow(&cardBack,8,10,5,150);
  ex_ready=1;
}

/* ══ DRAW HELPERS (no state, rows clipped to the band) ═════════════ */
static inline int ex_row0(int y){ return y<clip_y0?clip_y0:y; }
static inline int ex_row1(int y){ return y>clip_y1?clip_y1:(y>FBH?FBH:y); }

/* pull a rectangle toward a dark tint: keep is 0..256 of the original */
static void ex_shade(int x,int y,int w,int h,int keep,uint32_t tint){
  if(keep>=256) return;
  if(keep<0) keep=0;
  int ya=ex_row0(y), yb=ex_row1(y+h);
  int xa=x<0?0:x, xb=x+w>FBW?FBW:x+w;
  uint32_t trb=((tint&0xFF00FF)*(uint32_t)(256-keep)), tg=((tint&0x00FF00)*(uint32_t)(256-keep));
  for(int yy=ya;yy<yb;yy++){
    uint32_t*q=fb+(size_t)yy*FBW;
    for(int xx=xa;xx<xb;xx++){
      uint32_t c=q[xx];
      uint32_t rb=(((c&0xFF00FF)*(uint32_t)keep+trb)>>8)&0xFF00FF;
      uint32_t g =(((c&0x00FF00)*(uint32_t)keep+tg )>>8)&0x00FF00;
      q[xx]=rb|g;
    }
  }
}

/* per-byte saturating add of two packed pixels (SWAR) */
static inline uint32_t ex_addsat(uint32_t x,uint32_t y){
  uint32_t t0=(x^y)&0x808080u, t1=(x&y)&0x808080u;
  x&=0x7F7F7Fu; y&=0x7F7F7Fu; x+=y;
  t1|=t0&x;
  t1=(t1<<1)-(t1>>7);
  return (x^t0)|t1;
}
/*  Additive radial glow from the baked falloff, any radius.  The column
 *  lookup is built once per call and the colour is scaled and added as
 *  packed words, so a glow costs about as much as a plain blit.        */
static void ex_glow(int cx,int cy,int rad,int r,int g,int b){
  if(rad<2||rad>320||(r<=0&&g<=0&&b<=0)) return;
  r=clampi(r,0,255); g=clampi(g,0,255); b=clampi(b,0,255);
  uint32_t crb=((uint32_t)r<<16)|(uint32_t)b, cg=(uint32_t)g<<8;
  int ya=ex_row0(cy-rad), yb=ex_row1(cy+rad);
  int xa=cx-rad<0?0:cx-rad, xb=cx+rad>FBW?FBW:cx+rad;
  int step=(EGL<<16)/(2*rad);
  uint8_t col[640];
  for(int x=xa;x<xb;x++) col[x-xa]=(uint8_t)(((x-(cx-rad))*step)>>16);
  for(int y=ya;y<yb;y++){
    const uint8_t*row=exGlow+(size_t)(((y-(cy-rad))*step)>>16)*EGL;
    uint32_t*q=fb+(size_t)y*FBW;
    for(int x=xa;x<xb;x++){
      uint32_t v=row[col[x-xa]];
      if(!v) continue;
      uint32_t add=(((crb*v)>>8)&0xFF00FFu)|(((cg*v)>>8)&0x00FF00u);
      q[x]=ex_addsat(q[x],add);
    }
  }
}

/*  One glowing segment of a bolt: a soft halo of radius R plus a hot
 *  core of radius rc, both additive.  The falloff is (1-d^2/R^2)^2, so
 *  there is no square root in the loop.                                */
static void ex_seg(float x0,float y0,float x1,float y1,float R,float rc,
                   int hr,int hg,int hb,int cr,int cg,int cb){
  int bx0=(int)floorf(fminf(x0,x1)-R), bx1=(int)ceilf(fmaxf(x0,x1)+R);
  int by0=(int)floorf(fminf(y0,y1)-R), by1=(int)ceilf(fmaxf(y0,y1)+R);
  if(bx0<0) bx0=0;
  if(bx1>FBW) bx1=FBW;
  by0=ex_row0(by0); by1=ex_row1(by1);
  float dx=x1-x0, dy=y1-y0, L2=dx*dx+dy*dy;
  if(L2<0.0001f) L2=0.0001f;
  float iL2=1.0f/L2, R2=R*R, iR2=1.0f/R2, rc2=rc*rc, irc2=1.0f/(rc2>0.01f?rc2:0.01f);
  for(int y=by0;y<by1;y++){
    uint32_t*q=fb+(size_t)y*FBW;
    float py=y+0.5f-y0;
    /* only the part of the segment within R of this row can reach it */
    int xa=bx0, xb=bx1;
    if(fabsf(dy)>0.5f){
      float ta=(py-R)/dy, tb=(py+R)/dy;
      if(ta>tb){ float s=ta; ta=tb; tb=s; }
      ta=ta<0?0:ta; tb=tb>1?1:tb;
      if(ta>tb) continue;
      float xl=x0+dx*ta, xr=x0+dx*tb;
      if(xl>xr){ float s=xl; xl=xr; xr=s; }
      xa=(int)(xl-R); xb=(int)(xr+R)+1;
      if(xa<bx0) xa=bx0;
      if(xb>bx1) xb=bx1;
    }
    for(int x=xa;x<xb;x++){
      float px=x+0.5f-x0;
      float t=(px*dx+py*dy)*iL2; t=t<0?0:(t>1?1:t);
      float ex=px-t*dx, ey=py-t*dy, d2=ex*ex+ey*ey;
      if(d2>=R2) continue;
      float h=1.0f-d2*iR2; h*=h;
      int r=(int)(hr*h), g=(int)(hg*h), b=(int)(hb*h);
      if(d2<rc2){ float c=1.0f-d2*irc2; r+=(int)(cr*c); g+=(int)(cg*c); b+=(int)(cb*c); }
      uint32_t d=q[x];
      int dr=(int)((d>>16)&255)+r, dg=(int)((d>>8)&255)+g, db=(int)(d&255)+b;
      q[x]=RGB(dr>255?255:dr,dg>255?255:dg,db>255?255:db);
    }
  }
}

/*  A jagged channel from (sx,sy) to (ex,ey) by midpoint displacement,
 *  2^lv+1 points, every offset a hash of the seed: the same bolt every
 *  frame, a different one every strike.                                */
static int ex_bolt_pts(pt_t*p,int lv,float sx,float sy,float ex,float ey,uint32_t sd,float rough){
  int n=1<<lv;
  p[0].x=sx; p[0].y=sy; p[n].x=ex; p[n].y=ey;
  for(int step=n/2;step>=1;step/=2)
    for(int k=step;k<n;k+=2*step){
      pt_t a=p[k-step], b=p[k+step];
      float mx=(a.x+b.x)*0.5f, my=(a.y+b.y)*0.5f;
      float vx=b.x-a.x, vy=b.y-a.y, len=sqrtf(vx*vx+vy*vy);
      if(len<0.01f) len=0.01f;
      float off=(ex_hf(sd,(uint32_t)k*7u+(uint32_t)step)-0.5f)*len*rough;
      p[k].x=mx-vy/len*off; p[k].y=my+vx/len*off;
    }
  return n+1;
}

/*  The halo of a channel, stamped: the baked radial glow laid down every
 *  S pixels along the polyline.  Overlapping stamps sum to an even tube
 *  about 2R/3S times one stamp's peak, which the colour allows for.  It
 *  is integer work from a table, several times cheaper than evaluating
 *  a distance per pixel over the same area.                            */
static void ex_halo(const pt_t*p,int n,float R,float S,int r,int g,int b){
  float carry=0;
  for(int k=0;k+1<n;k++){
    float dx=p[k+1].x-p[k].x, dy=p[k+1].y-p[k].y, len=sqrtf(dx*dx+dy*dy);
    if(len<0.01f) continue;
    float t=carry;
    for(;t<len;t+=S) ex_glow((int)(p[k].x+dx*t/len),(int)(p[k].y+dy*t/len),(int)R,r,g,b);
    carry=t-len;
  }
}
/* a whole bolt with its forks; upto = how far the leader has reached */
static void ex_bolt(float sx,float sy,float ex,float ey,uint32_t sd,float I,float upto,int big){
  if(I<=0.01f) return;
  pt_t p[33]; int n=ex_bolt_pts(p,5,sx,sy,ex,ey,sd,0.55f);
  int last=(int)(upto*(n-1));
  if(last<1) return;
  float R=big?17.0f:12.0f, S=big?9.0f:7.0f, k=0.72f*I;
  ex_halo(p,last+1,R,S,(int)(55*k),(int)(100*k),(int)(255*k));
  for(int j=0;j<last;j++)
    ex_seg(p[j].x,p[j].y,p[j+1].x,p[j+1].y,big?4.5f:3.0f,big?2.4f:1.6f,
           (int)(90*I),(int)(140*I),(int)(255*I),(int)(255*I),(int)(255*I),(int)(255*I));
  int nb=big?2:1;
  for(int b=0;b<nb;b++){
    int k0=6+(int)(ex_hf(sd,900u+b)*18.0f);
    if(k0>=last) continue;
    float ang=atan2f(p[k0+1].y-p[k0].y,p[k0+1].x-p[k0].x);
    ang+=(ex_hf(sd,910u+b)<0.5f?-1.0f:1.0f)*(0.45f+0.5f*ex_hf(sd,920u+b));
    float len=(big?70.0f:40.0f)+ex_hf(sd,930u+b)*(big?110.0f:50.0f);
    pt_t q[9]; ex_bolt_pts(q,3,p[k0].x,p[k0].y,p[k0].x+cosf(ang)*len,p[k0].y+sinf(ang)*len,sd*31u+b,0.6f);
    float f=I*0.6f;
    ex_halo(q,6,R*0.6f,S*0.7f,(int)(40*f),(int)(80*f),(int)(220*f));
    for(int j=0;j<8;j++){
      float g=f*(1.0f-j/9.0f);
      ex_seg(q[j].x,q[j].y,q[j+1].x,q[j+1].y,2.6f,1.2f,
             (int)(70*g),(int)(120*g),(int)(255*g),(int)(230*g),(int)(240*g),(int)(255*g));
    }
  }
}

/* a sprite scaled about its centre, nearest sample, optional wash */
static void ex_blit_sc(const spr_t*s,int cx,int cy,float scx,float scy,int cy0,int cy1,
                       int alpha,uint32_t tint,float amt){
  if(!s->px||scx<=0.01f||scy<=0.01f) return;
  int dw=(int)(s->w*scx), dh=(int)(s->h*scy);
  int x0=cx-dw/2, y0=cy-dh/2;
  int ya=ex_row0(y0>cy0?y0:cy0), yb=ex_row1(y0+dh<cy1?y0+dh:cy1);
  int xa=x0<0?0:x0, xb=x0+dw>FBW?FBW:x0+dw;
  int ix=(int)(65536.0f/scx), iy=(int)(65536.0f/scy);
  int k=(int)(clampf(amt,0,1)*256);
  int tr=(tint>>16)&255, tg=(tint>>8)&255, tb=tint&255;
  for(int y=ya;y<yb;y++){
    int sy=((y-y0)*iy)>>16;
    if(sy<0||sy>=s->h) continue;
    const uint8_t*row=s->px+(size_t)sy*s->w*4;
    uint32_t*q=fb+(size_t)y*FBW;
    if(k==0 && alpha>=255){                  /* plain: the common case */
      for(int x=xa;x<xb;x++){
        int sx=((x-x0)*ix)>>16;
        if(sx<0||sx>=s->w) continue;
        const uint8_t*p=row+sx*4;
        int a=p[3];
        if(!a) continue;
        if(a>=255){ q[x]=RGB(p[0],p[1],p[2]); continue; }
        uint32_t d=q[x];
        int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
        q[x]=RGB(dr+((p[0]-dr)*a>>8),dg+((p[1]-dg)*a>>8),db+((p[2]-db)*a>>8));
      }
      continue;
    }
    for(int x=xa;x<xb;x++){
      int sx=((x-x0)*ix)>>16;
      if(sx<0||sx>=s->w) continue;
      const uint8_t*p=row+sx*4;
      int a=p[3]*alpha/255;
      if(!a) continue;
      int r=p[0]+((tr-p[0])*k>>8), g=p[1]+((tg-p[1])*k>>8), b=p[2]+((tb-p[2])*k>>8);
      if(a>=255){ q[x]=RGB(r,g,b); continue; }
      uint32_t d=q[x];
      int dr=(d>>16)&255, dg=(d>>8)&255, db=d&255;
      q[x]=RGB(dr+((r-dr)*a>>8),dg+((g-dg)*a>>8),db+((b-db)*a>>8));
    }
  }
}

/* text rotated half a turn, for the lower card index; (x,y) top-left */
static void ex_text180(const char*s,int x,int y,int px,uint32_t col){
  int n=(int)strlen(s), adv=px*6, W=n*adv-px, H=7*px;
  for(int i=0;i<n;i++){
    int ch=(unsigned char)s[i];
    if(ch<32||ch>127) ch='?';
    const uint8_t*gl=FONT[ch-32];
    for(int r=0;r<7;r++) for(int c=0;c<5;c++){
      if(!(gl[r]&(0x10>>c))) continue;
      int gx=x+W-(i*adv+c*px)-px, gy=y+H-r*px-px;
      for(int j=0;j<px;j++) for(int k=0;k<px;k++) fb_px(gx+k,gy+j,col);
    }
  }
}

/* filled triangle, for the shards of a lost stake */
static void ex_tri(float ax,float ay,float bx,float by,float cx,float cy,uint32_t col,int a){
  float miny=fminf(ay,fminf(by,cy)), maxy=fmaxf(ay,fmaxf(by,cy));
  int ya=ex_row0((int)ceilf(miny)), yb=ex_row1((int)floorf(maxy)+1);
  pt_t v[3]={{ax,ay},{bx,by},{cx,cy}};
  for(int y=ya;y<yb;y++){
    float fy=y+0.5f, xs[2]; int n=0;
    for(int i=0,j=2;i<3;j=i++){
      if((v[i].y<=fy&&v[j].y>fy)||(v[j].y<=fy&&v[i].y>fy)){
        if(n<2) xs[n++]=v[i].x+(fy-v[i].y)/(v[j].y-v[i].y)*(v[j].x-v[i].x);
      }
    }
    if(n<2) continue;
    int xa=(int)fminf(xs[0],xs[1]), xb=(int)fmaxf(xs[0],xs[1]);
    for(int x=xa;x<=xb;x++) fb_blend(x,y,col,a);
  }
}

/* ══ 7 STRIKE: UPDATE ═══════════════════════════════════════════════ */
static int storm_eligible(int s){
  return s!=SY_SEVEN && s!=SY_STAR && s!=SY_CROWN && s!=SY_JACKPOT &&
         s!=SY_ULT && s!=SY_COIN && s!=SY_WHEEL;
}

static void extra_on_spin_start(void){
  extra_state_t*E=&G.extra;
  E->stormArmed=0; E->stormMask=0; E->nStrike=0; E->landed=0;
  if(G.inFree) return;
  int arm = STORM_ODDS>0 && irnd(STORM_ODDS)==0;
  if(dbg_force==9) arm=1;                    /* WILD7_FORCE=storm */
  if(!arm) return;
  E->stormArmed=1;
  /*  The anticipation.  Nothing in ST_SPIN calls this module per frame,
   *  so the storm's timetable is fixed here: three flickers of sheet
   *  lightning at chosen moments, and the sound for all of it queued at
   *  those same offsets.  The draw reads the same times off G.t.       */
  E->flickT[0]=0.75f+frnd()*0.45f;
  E->flickT[1]=E->flickT[0]+0.70f+frnd()*0.60f;
  E->flickT[2]=E->flickT[1]+0.60f+frnd()*0.60f;
  snd_noise_at(0.10f,3.2f,0.10f,90);                     /* the rumble builds */
  snd_noise_at(0.90f,3.0f,0.12f,120);
  snd_at(0.20f,55,46,3.0f,2,0.045f);
  for(int k=0;k<3;k++){
    snd_noise_at(E->flickT[k],0.05f,0.08f,5000);          /* a crackle      */
    snd_noise_at(E->flickT[k]+0.12f,1.3f,0.13f+k*0.02f,240); /* distant roll */
  }
}

/*  Pick the bolts.  See the maths note at the top: draw the count, try
 *  STORM_K placements with the real evaluator, keep the one nearest the
 *  count's target.  evaluate() zeroes a jackpot pot it finds, so the pots
 *  are put back; the caller evaluates the final grid for real.          */
static void extra_on_snapshot(void){
  extra_state_t*E=&G.extra;
  E->stormMask=0; E->nStrike=0;
  if(!E->stormArmed || G.inFree) return;
  int tot=0;
  for(int k=0;k<=EX_MAXSTRIKE;k++) tot+=STORM_W[k];
  int u=irnd(tot), n=3;
  for(int k=0;k<=EX_MAXSTRIKE;k++){ if(u<STORM_W[k]){ n=k; break; } u-=STORM_W[k]; }
  int base[NCELL], nb=0;
  for(int c=0;c<NCELL;c++) if(storm_eligible(G.grid[c/NROW][c%NROW])) base[nb++]=c;
  if(n>nb) n=nb;
  if(n<=0) return;

  int grid0[NREEL][NROW]; memcpy(grid0,G.grid,sizeof grid0);
  long long acc0[NJP];    memcpy(acc0,G.jpAcc,sizeof acc0);
  double tgt=STORM_T[n]*(double)TOTBET/10.0, best=1e300;
  int bestOrd[EX_MAXSTRIKE];
  for(int k=0;k<STORM_K;k++){
    int cand[NCELL], nc=nb, ord[EX_MAXSTRIKE];
    memcpy(cand,base,sizeof cand);
    for(int i=0;i<n;i++){ int j=irnd(nc); ord[i]=cand[j]; cand[j]=cand[--nc]; }
    for(int i=0;i<n;i++) G.grid[ord[i]/NROW][ord[i]%NROW]=SY_SEVEN;
    evaluate();
    double w=G.winTotal, d=fabs(log((w+0.1*TOTBET)/(tgt+0.1*TOTBET)));
    if(d<best){ best=d; memcpy(bestOrd,ord,sizeof ord); }
    memcpy(G.grid,grid0,sizeof grid0);
  }
  memcpy(G.jpAcc,acc0,sizeof acc0);
  G.nWin=0; G.winTotal=0; G.jpWon=-1; G.jpMask=0;

  /* strike order: top to bottom reads best as a storm rolling in, but a
     little shuffle keeps it from looking mechanical */
  for(int i=0;i<n;i++) for(int j=i+1;j<n;j++)
    if(bestOrd[j]%NROW*8+bestOrd[j]/NROW < bestOrd[i]%NROW*8+bestOrd[i]/NROW){
      int t=bestOrd[i]; bestOrd[i]=bestOrd[j]; bestOrd[j]=t;
    }
  for(int i=0;i+1<n;i++) if(irnd(3)==0){ int t=bestOrd[i]; bestOrd[i]=bestOrd[i+1]; bestOrd[i+1]=t; }
  for(int i=0;i<n;i++){
    int c=bestOrd[i];
    E->order[E->nStrike++]=c; E->stormMask|=1u<<c;
    G.grid[c/NROW][c%NROW]=SY_SEVEN;
  }
  for(int r=0;r<NREEL;r++) E->stormStop[r]=(((int)floorf(G.rpos[r]+0.5f))%STRIPLEN+STRIPLEN)%STRIPLEN;
}

static int extra_storm_pending(void){
  return G.extra.stormArmed && G.extra.nStrike>0 && !G.inFree;
}

static void extra_storm_begin(void){
  extra_state_t*E=&G.extra;
  E->cloud0=G.t;                         /* keep the clouds' drift continuous */
  G.state=ST_STORM; G.t=0;
  G.banner=0; G.bannerT=0;
  E->st=0; E->landed=0; E->fast=0;
  E->seed=(uint32_t)irnd(1<<24)*2654435761u+1u;
  float t=0.85f;
  for(int i=0;i<E->nStrike;i++){
    E->boltT[i]=t;
    float gap=0.52f-0.04f*i;
    t+=gap<0.30f?0.30f:gap;
  }
  E->endT=E->boltT[E->nStrike-1]+1.05f;
  fx_shake(5.0f,0.35f);
  snd_noise(0.10f,0.30f,8000);
  snd_noise_at(0.05f,1.6f,0.22f,200);
  snd_chord(0.08f,392,494,587,0.9f,0.13f);
  snd_at(0.08f,98,98,1.0f,2,0.10f);
}

static void ex_strike(int i){
  const extra_state_t*E=&G.extra;
  int c=E->order[i];
  float cx=(float)cellcx(c/NROW), cy=(float)cellcy(c%NROW);
  fx_burst(cx,cy,22,FXK_SPARK);
  fx_burst(cx,cy,10,FXK_STAR);
  fx_shake(i+1==E->nStrike?9.0f:6.0f,0.28f);
  snd_noise(0.07f,0.34f,9000);                 /* the crack           */
  snd_noise_at(0.03f,0.45f,0.24f,1400);        /* its snap            */
  snd_noise_at(0.06f,1.40f,0.22f,260);         /* the roll            */
  snd(120,34,0.60f,2,0.15f);                   /* the boom            */
  static const float up[8]={523,659,784,880,1046,1318,1568,2093};
  float f=up[i&7];                             /* each wild rings higher */
  snd_at(0.10f,f,f,0.32f,1,0.15f);
  snd_at(0.10f,f*1.5f,f*1.5f,0.26f,0,0.05f);
}

static void extra_storm_update(void){
  extra_state_t*E=&G.extra;
  if(G.t>0.35f && (hit(B_START)||hit(B_A)||hit(B_B))) E->fast=1;
  E->st += DT*(E->fast?3.0f:1.0f);
  while(E->landed<E->nStrike && E->st>=E->boltT[E->landed]){
    ex_strike(E->landed);
    E->landed++;
    if(E->landed==E->nStrike){                 /* the last one: a flourish */
      static const float arp[4]={784,988,1175,1568};
      for(int k=0;k<4;k++) snd_at(0.30f+k*0.07f,arp[k],arp[k],0.22f,1,0.12f);
      snd_chord(0.60f,1046,1318,1568,0.7f,0.13f);
    }
  }
  if(E->st>=E->endT){ G.state=ST_EVAL; G.t=0; }
}

/* ══ 7 STRIKE: DRAW ═════════════════════════════════════════════════ */
/* storm-clock brightness of bolt i: leader, return stroke, restrikes */
static float ex_bolt_I(float dt,float*upto){
  *upto=1.0f;
  if(dt<-0.10f || dt>0.45f) return 0.0f;
  if(dt<0.0f){ *upto=(dt+0.10f)/0.10f; return 0.40f; }
  float f=1.0f-dt/0.45f;
  if(opt_limiter) return f;                     /* no strobing */
  float fl = dt<0.05f?1.0f:(dt<0.09f?0.35f:(dt<0.14f?0.95f:(dt<0.20f?0.45f:0.75f)));
  return f*fl;
}

/* the whole-sky flash that lights clouds and lifts the dark */
static float ex_sky(void){
  const extra_state_t*E=&G.extra;
  float s=0;
  if(G.state==ST_STORM){
    for(int i=0;i<E->nStrike;i++){
      float dt=E->st-E->boltT[i];
      if(dt>=0 && dt<0.18f){ float v=1.0f-dt/0.18f; if(v>s) s=v; }
    }
    if(E->st<0.25f){ float v=1.0f-E->st/0.25f; if(v>s) s=v; }
  } else if(G.state==ST_SPIN){
    for(int k=0;k<3;k++){
      float dt=G.t-E->flickT[k], v=0;
      if(dt<0||dt>0.32f) continue;
      if(opt_limiter) v=1.0f-dt/0.32f;
      else v = dt<0.05f?1.0f:(dt<0.09f?0.25f:(dt<0.15f?0.85f:0.85f*(1.0f-(dt-0.15f)/0.17f)));
      if(v>s) s=v;
    }
  }
  return opt_limiter? s*0.40f : s;
}

/*  One pass of the cloud bank over the frame.  The bank tiles, so each
 *  row is walked as (at most) two straight spans rather than taking a
 *  modulo per pixel, and the blend is the packed two-multiply form: this
 *  runs over a couple of hundred thousand pixels a frame while it is up. */
static void ex_cloud_layer(int yoff,int rows,const uint32_t*lut,const int*colA,int off){
  int ya=ex_row0(yoff<0?0:yoff), yb=ex_row1(yoff+rows);
  for(int y=ya;y<yb;y++){
    int row=y-yoff;
    if(clRowMax[row]<4) continue;
    const uint8_t*A=clA+(size_t)row*CLW, *S=clS+(size_t)row*CLW;
    uint32_t*q=fb+(size_t)y*FBW;
    int x=0;
    while(x<FBW){
      int s0=(x+off)%CLW, run=CLW-s0;
      if(run>FBW-x) run=FBW-x;
      const uint8_t*Ap=A+s0, *Sp=S+s0;
      for(int k=0;k<run;k++,x++,Ap++,Sp++){
        int a=(*Ap)*colA[x]>>8;
        if(a<=2) continue;
        uint32_t c=lut[*Sp];
        if(a>=253){ q[x]=c; continue; }
        uint32_t d=q[x], ia=(uint32_t)(256-a);
        q[x]=((((d&0xFF00FF)*ia+(c&0xFF00FF)*(uint32_t)a)>>8)&0xFF00FF) |
             ((((d&0x00FF00)*ia+(c&0x00FF00)*(uint32_t)a)>>8)&0x00FF00);
      }
    }
  }
}
static void ex_clouds(int yoff,int a256,float flash,float clock){
  if(!clA||a256<=0) return;
  static const uint32_t DK[3]={0x04060E,0x252C4C,0x7A86B8}, LT[3]={0x34407A,0xA8B6F4,0xFFFFFF};
  uint32_t lut[256]; int colA[FBW];
  float f=clampf(flash,0,1);
  for(int i=0;i<256;i++){
    float s=i/255.0f;
    lut[i]=mixc(ramp(DK,3,s),ramp(LT,3,s),f);
  }
  for(int x=0;x<FBW;x++) colA[x]=clW[x]*a256>>8;
  ex_cloud_layer(yoff,CLH,lut,colA,((int)(clock*38.0f))%CLW);
}

/* sparks crawling round the bezel while an armed spin is turning */
static void ex_bezel_arcs(float amt){
  const float X0=GX-11, Y0=GY-11, W=GW+22, H=GH+22, P=2*(W+H);
  uint32_t bucket=(uint32_t)(G.t*14.0f);
  for(int k=0;k<3;k++){
    uint32_t sd=bucket*131u+k*977u+G.extra.seed;
    if(ex_hf(sd,1)>0.65f) continue;
    float d=ex_hf(sd,2)*P, px,py,tx,ty;
    if(d<W){ px=X0+d; py=Y0; tx=1; ty=0; }
    else if(d<W+H){ px=X0+W; py=Y0+(d-W); tx=0; ty=1; }
    else if(d<2*W+H){ px=X0+W-(d-W-H); py=Y0+H; tx=-1; ty=0; }
    else { px=X0; py=Y0+H-(d-2*W-H); tx=0; ty=-1; }
    float lx=px, ly=py, I=amt*(0.55f+0.45f*ex_hf(sd,3));
    for(int s=1;s<=6;s++){
      float j=(ex_hf(sd,10u+s)-0.5f)*14.0f;
      float nx=px+tx*s*11.0f - ty*j, ny=py+ty*s*11.0f + tx*j;
      ex_seg(lx,ly,nx,ny,7.0f,1.3f,(int)(40*I),(int)(90*I),(int)(220*I),(int)(200*I),(int)(230*I),(int)(255*I));
      lx=nx; ly=ny;
    }
  }
}

/* a struck cell: the WILD 7 it became, popping in on its bolt */
static void ex_draw_struck(int c,float dt){
  int r=c/NROW, row=c%NROW;
  int cx=cellcx(r), cy=cellcy(row);
  const spr_t*s=&stormWild;
  if(dt<0.30f){                               /* pops in white-hot */
    float u=dt/0.30f, e=(1.0f-u)*(1.0f-u), sc=1.0f+0.45f*e;
    ex_blit_sc(s,cx,cy,sc,sc,GY,GY+GH,255,0xFFFFFF,0.85f*e);
  } else {
    blit(s,GX+r*CW,GY+row*CH,GY,GY+GH,255,0,0.0f);
  }
  if(dt<0.6f){                                /* impact glow and shock ring */
    float f=1.0f-dt/0.6f;
    ex_glow(cx,cy,(int)(56+dt*50),(int)(50*f),(int)(95*f),(int)(210*f));
    float rr=24.0f+dt*200.0f; int a=(int)(220*f);
    int n=(int)(rr*TAU/3.0f);
    for(int k=0;k<n;k++){
      float an=k*TAU/n; int x=cx+(int)(cosf(an)*rr), y=cy+(int)(sinf(an)*rr);
      fb_add(x,y,a/2,a*3/4,a); fb_add(x+1,y,a/3,a/2,a*2/3); fb_add(x,y+1,a/3,a/2,a*2/3);
    }
  } else {                                    /* now and then, a crackle */
    uint32_t sd=(uint32_t)(G.t*9.0f)*37u+(uint32_t)c*101u;
    if(ex_hf(sd,1)<0.10f){
      float a0=ex_hf(sd,2)*TAU, lx=cx+cosf(a0)*44, ly=cy+sinf(a0)*44;
      for(int s2=1;s2<=4;s2++){
        float a1=a0+s2*0.28f, rr=44.0f+(ex_hf(sd,3u+s2)-0.5f)*14.0f;
        float nx=cx+cosf(a1)*rr, ny=cy+sinf(a1)*rr;
        ex_seg(lx,ly,nx,ny,5.0f,1.2f,40,90,220,200,230,255);
        lx=nx; ly=ny;
      }
    }
  }
}

/*  The storm wilds are drawn over the reels, so they may only show while
 *  the reels still stand where the storm found them - any other module
 *  that moves the reels (a respin, the attract loop) makes them vanish. */
static int ex_reels_still(void){
  for(int r=0;r<NREEL;r++){
    if(G.rstate[r]==1||G.rstate[r]==2) return 0;
    if((((int)floorf(G.rpos[r]+0.5f))%STRIPLEN+STRIPLEN)%STRIPLEN != G.extra.stormStop[r]) return 0;
  }
  return 1;
}

/* the marquee plate: 7 STRIKE and how many wilds it has thrown */
static void ex_marquee_plate(int n,float pl){
  const int x=FBW/2-plateSpr.w/2, y=5;
  blit(&plateSpr,x,y,0,FBH,255,0,0.0f);
  char b[32]; snprintf(b,sizeof b,"%d WILD%s",n,n==1?"":"S");
  text(b,x+plateSpr.w-26,y+15,3,mixc(0xFFE9A8,0xFFFFFF,pl*0.6f),2,1);
}

/* the gamble offer, hung on the bottom of the reel bezel; a shine runs
   across it so the eye finds it */
static void ex_gamble_prompt(void){
  const int w=pillSpr.w, h=pillSpr.h, x=FBW/2-w/2, y=GY+GH-6;
  blit(&pillSpr,x,y,0,FBH,255,0,0.0f);
  float ph=fmodf(G.t*1.1f,1.6f);
  if(ph<1.0f){
    int sx=x-40+(int)(ph*(w+80)), k=opt_limiter?70:120;
    for(int j=4;j<h-4;j++){
      int c0=sx+(j-h/2)/2;
      for(int i=-12;i<=12;i++){
        int xx=c0+i;
        if(xx<x+h/2||xx>=x+w-h/2) continue;
        int v=k*(12-abs(i))/12;
        fb_add(xx,y+j,v,v,v*3/4);
      }
    }
  }
}

static void extra_draw_reels(void){
  const extra_state_t*E=&G.extra;
  float sky=ex_sky();

  /* 1. the storm's weather */
  float dark=0, cloudA=0, clock=G.t; int cloudY=0, arcs=0;
  if(G.state==ST_SPIN && E->stormArmed){
    float b=clampf(G.t/1.4f,0,1), e=1.0f-(1.0f-b)*(1.0f-b);
    dark=0.52f*b; cloudA=e; cloudY=-(int)(CLH*(1.0f-e)); arcs=b>0.45f;
  } else if(G.state==ST_STORM){
    float tail=clampf((E->endT-E->st)/0.6f,0,1);
    dark=0.70f*tail; cloudA=tail; cloudY=-(int)(CLH*0.8f*(1.0f-tail));
    clock=E->cloud0+G.t;
  }
  if(dark>0){
    /* one pass does both: the storm pulls the window toward a deep blue,
       and while a bolt lights the sky it pulls it toward blue-white
       instead - the whole-screen flash would cost a full-frame pass */
    float amt=lerpf(dark,0.55f,sky);
    ex_shade(GX,GY,GW,GH,(int)(256*(1.0f-amt)),mixc(0x050A22,0xDDE8FF,sky));
  }
  if(arcs) ex_bezel_arcs(clampf((G.t-0.6f)/0.8f,0,1));

  /* 2. the wilds it has thrown: from its bolt landing until the next spin */
  if(E->stormMask && G.state!=ST_ATTRACT && G.state!=ST_SPIN && G.state!=ST_GAMBLE &&
     G.state!=ST_PAYTABLE && ex_reels_still()){
    for(int i=0;i<E->nStrike;i++){
      float dt = G.state==ST_STORM ? E->st-E->boltT[i] : 9.0f;
      if(dt<0) continue;
      ex_draw_struck(E->order[i],dt);
    }
  }

  /* 3. cloud over the marquee, lit from inside by every flash */
  if(cloudA>0){
    ex_clouds(cloudY,(int)(cloudA*255),sky,clock);
    if(G.state==ST_SPIN)                        /* lightning inside the cloud */
      for(int k=0;k<3;k++){
        float dt=G.t-E->flickT[k];
        if(dt<0||dt>0.30f) continue;
        uint32_t sd=0xC10D5u+k*7919u+(uint32_t)(E->flickT[k]*1000.0f);
        float sx=180.0f+ex_hf(sd,1)*920.0f, ex=sx+(ex_hf(sd,2)-0.5f)*360.0f;
        ex_bolt(sx,2.0f+cloudY*0.3f,ex,48.0f+ex_hf(sd,3)*40.0f,sd,sky*0.9f,1.0f,0);
      }
  }

  /* 4. the storm itself: the title, then the bolts */
  if(G.state==ST_STORM){
    float pl=0.5f+0.5f*sinf(G.t*8.0f);
    if(E->st<0.80f){
      float a=clampf(E->st/0.08f,0,1)*clampf((0.80f-E->st)/0.15f,0,1);
      int by=GY+GH/2-78;
      ex_shade(GX,by,GW,156,(int)(256*(1.0f-0.75f*a)),0x02040E);
      if(a>0.3f){
        blit(&titleSpr,FBW/2-TITW/2,by+22-16,0,FBH,255,0,0.0f);
        text("LIGHTNING TURNS SYMBOLS WILD",FBW/2,by+120,2,0xCFE8FF,1,1);
      }
      if(E->st<0.30f){                        /* two bolts nail the title up */
        float I=1.0f-E->st/0.30f;
        ex_bolt(GX+150.0f,4.0f,GX+70.0f,(float)by+50,E->seed^0xA1u,I,1.0f,0);
        ex_bolt(GX+GW-150.0f,4.0f,GX+GW-70.0f,(float)by+50,E->seed^0xB2u,I,1.0f,0);
      }
    }
    if(E->st>=0.80f && clampf((E->endT-E->st)/0.6f,0,1)>0.2f) ex_marquee_plate(E->landed,pl);
    for(int i=0;i<E->nStrike;i++){
      float upto, I=ex_bolt_I(E->st-E->boltT[i],&upto);
      if(I<=0) continue;
      int c=E->order[i];
      uint32_t sd=E->seed+(uint32_t)i*0x9E3779B9u;
      float ex=(float)cellcx(c/NROW), ey=(float)cellcy(c%NROW);
      float sx=clampf(ex+(ex_hf(sd,1)-0.5f)*320.0f,(float)GX-30,(float)(GX+GW+30)), sy=6.0f+ex_hf(sd,2)*24.0f;
      ex_bolt(sx,sy,ex,ey,sd,I,upto,1);
      if(upto>=1.0f) ex_glow((int)ex,(int)ey,72,(int)(50*I),(int)(90*I),(int)(200*I));
    }
  } else if(E->stormMask && (G.state==ST_EVAL||G.state==ST_SHOWWIN)){
    ex_marquee_plate(E->nStrike,0.5f+0.5f*sinf(G.t*4.0f));
  }

  /* 5. the gamble offer on a counted win */
  if(G.state==ST_SHOWWIN && G.winShown>=G.winTotal && G.t>0.5f && gamble_allowed())
    ex_gamble_prompt();
}

/* ══ GAMBLE ═════════════════════════════════════════════════════════ */
static int gamble_allowed(void){
  if(G.inFree || G.winTotal<=0) return 0;
  return (long long)G.winTotal <= (long long)GAMBLE_CAPX*TOTBET;
}

static void gamble_begin(void){
  extra_state_t*E=&G.extra;
  G.state=ST_GAMBLE; G.t=0;
  G.banner=0; G.bannerT=0; G.flash=0;
  E->gPot=G.winTotal; E->gFrom=E->gPot;
  E->gRound=0; E->gCard=-1; E->gChoice=-1; E->gWon=0;
  E->gPhase=GP_DEAL; E->gT=0; E->gEnd=GE_COLLECT; E->gIdle=0;
  if(E->gSuit<0||E->gSuit>3) E->gSuit=0;
  snd_noise(0.12f,0.12f,3500);
  snd_chord(0.05f,523,659,784,0.35f,0.12f);
}

static void ex_gamble_finish(int pay){
  extra_state_t*E=&G.extra;
  E->gPhase=GP_NONE;
  award(pay);
  feature_done();
}

static void gamble_update(void){
  extra_state_t*E=&G.extra;
  E->gT+=DT;
  switch(E->gPhase){
  case GP_DEAL:
    if(E->gT>=0.35f){ E->gPhase=GP_PICK; E->gT=0; E->gIdle=0; }
    break;

  case GP_PICK: {
    E->gIdle+=DT;
    if(hit(B_UP))  { E->gSuit=(E->gSuit+3)&3; snd(800,900,0.04f,0,0.07f); E->gIdle=0; }
    if(hit(B_DOWN)){ E->gSuit=(E->gSuit+1)&3; snd(800,900,0.04f,0,0.07f); E->gIdle=0; }
    int ch=-1;
    if(hit(B_LEFT)) ch=0; else if(hit(B_RIGHT)) ch=1; else if(hit(B_X)) ch=2+E->gSuit;
    if(ch<0){
      if(hit(B_A)||hit(B_START)||hit(B_B)||E->gIdle>30.0f){
        E->gEnd=GE_COLLECT; E->gPhase=GP_OUT; E->gT=0;
        fx_fountain(FBW*0.5f,141.0f,1.0f);
        for(int k=0;k<8;k++) snd_at(k*0.06f,1400.0f+k*120.0f,1900.0f+k*120.0f,0.05f,0,0.05f);
      }
      break;
    }
    /* the draw: a fair card, decided now, shown when it turns over */
    int suit=irnd(4), rank=irnd(13);
    E->gChoice=ch; E->gCard=suit*16+rank;
    E->gWon = ch<2 ? ((suit<2)==(ch==0)) : (suit==ch-2);
    E->gFrom=E->gPot;
    if(E->gWon){
      long long np=(long long)E->gPot*(ch<2?2:4);
      E->gPot=(int)(np>2000000000LL?2000000000LL:np);
    } else E->gPot=0;
    for(int k=EX_NHIST-1;k>0;k--) E->gHist[k]=E->gHist[k-1];
    E->gHist[0]=E->gCard+1;
    E->gRound++;
    E->gPhase=GP_FLIP; E->gT=0;
    snd_noise(0.06f,0.10f,4000);
    snd(500,900,0.10f,1,0.10f);
    snd_at(0.25f,700,1100,0.10f,1,0.10f);
    break; }

  case GP_FLIP:
    if(E->gT>=0.55f){
      E->gPhase=GP_RESULT; E->gT=0;
      if(E->gWon){
        fx_burst(FBW*0.5f,(float)CARDY+CDH*0.5f,24,FXK_COIN);
        fx_burst(FBW*0.5f,(float)CARDY+CDH*0.5f,12,FXK_STAR);
        G.flash=opt_limiter?0.18f:0.35f;
        if(E->gChoice<2){ snd_chord(0.0f,784,988,1175,0.45f,0.16f); snd_at(0.12f,1568,2093,0.25f,0,0.06f); }
        else { static const float a[5]={523,659,784,1046,1318};
               for(int k=0;k<5;k++) snd_at(k*0.06f,a[k],a[k],0.18f,1,0.16f);
               snd_chord(0.32f,1046,1318,1568,0.8f,0.18f); }
      } else {
        fx_shake(4.0f,0.25f);
        snd(320,70,0.70f,2,0.16f);
        snd_noise(0.35f,0.20f,1500);
        snd_at(0.10f,196,147,0.5f,1,0.12f);
      }
    }
    break;

  case GP_RESULT: {
    float need = E->gWon ? 1.15f : 1.70f;
    int skip = E->gT>0.35f && (hit(B_A)||hit(B_START)||hit(B_B)||hit(B_LEFT)||hit(B_RIGHT)||hit(B_X));
    if(E->gT<need && !skip) break;
    if(!E->gWon){ ex_gamble_finish(0); break; }
    if(E->gRound>=GAMBLE_ROUNDS || (long long)E->gPot>(long long)GAMBLE_CAPX*TOTBET){
      E->gEnd = E->gRound>=GAMBLE_ROUNDS ? GE_ROUNDS : GE_LIMIT;
      E->gPhase=GP_OUT; E->gT=0;
      fx_fountain(FBW*0.5f,141.0f,1.4f);
      snd_chord(0.0f,1046,1318,1568,0.9f,0.16f);
    } else {
      E->gCard=-1; E->gPhase=GP_DEAL; E->gT=0.10f;
      snd_noise(0.10f,0.10f,3500);
    }
    break; }

  case GP_OUT:
    if(E->gT>=1.30f || (E->gT>0.40f && anyhit())) ex_gamble_finish(E->gPot);
    break;

  default:                                    /* should not happen: bail out */
    ex_gamble_finish(G.winTotal);
    break;
  }
}

/* one card: face or back, squashed horizontally for the turn */
static void ex_card(int cx,int top,float sx,int card,int alpha,float sheen){
  if(sx<0.02f) return;
  const spr_t*s = card<0 ? &cardBack : &cardFace[(card>>4)&3];
  /* the sprite carries its shadow 8 right, 10 down: centre the card, not it */
  ex_blit_sc(s,cx+(int)(4*sx),top+CDH/2+5,sx,1.0f,0,FBH,alpha,0xFFFFFF,sheen);
  if(card>=0 && sx>0.92f && alpha>200){
    int rank=card&15, red=((card>>4)&3)<2;
    uint32_t col=red?0xC8101E:0x101018;
    const char*rs=RANKS[rank>12?12:rank];
    int x0=cx-CDW/2, n=(int)strlen(rs);
    text(rs,x0+17+(n>1?-4:0),top+15,3,col,0,0);
    ex_text180(rs,x0+CDW-17-(n*18-3)+(n>1?4:0),top+CDH-15-21,3,col);
  }
}

static void gamble_draw(void){
  const extra_state_t*E=&G.extra;
  if(!tblImg) return;
  { int ya=ex_row0(TY0), yb=ex_row1(TY0+TH);
    for(int y=ya;y<yb;y++) memcpy(fb+(size_t)y*FBW+TX0,tblImg+(size_t)(y-TY0)*TW,(size_t)TW*4); }
  float t=E->gT, pl=0.5f+0.5f*sinf(G.t*6.0f);
  if(opt_limiter) pl=0.3f+pl*0.4f;
  int ph=E->gPhase, won=E->gWon;
  int res = (ph==GP_RESULT);
  char b[64];

  /* title */
  const char*title="GAMBLE"; const uint32_t*tg=GOLDG; int tn=5;
  if(res && won){ title=E->gChoice<2?"DOUBLE!":"SUIT X4!"; if(((int)(G.t*8.0f))&1){ tg=SILVERG; tn=4; } }
  else if(res){ title="NO LUCK"; tg=REDG; tn=4; }
  else if(ph==GP_OUT) title = E->gEnd==GE_ROUNDS?"FIVE IN A ROW!":(E->gEnd==GE_LIMIT?"TABLE LIMIT":"COLLECTED");
  textb(title,FBW/2,66,6,tg,tn,1);

  /* the pot: counts up on a win, shatters on a loss */
  long long shown=E->gPot;
  if(ph==GP_FLIP) shown=E->gFrom;
  if(res && won) shown=E->gFrom+(long long)((E->gPot-E->gFrom)*clampf(t/0.8f,0,1));
  uint32_t lc = (res&&won) ? mixc(0xFFB020,0xFFFFFF,pl*0.7f) : 0xFFB020;
  if(!(res && !won)) seg_num(shown,FBW/2+116,124,9,17,34,lc,0x4A3406,1);
  else seg_num(0,FBW/2+116,124,9,17,34,0x5A1008,0x3A0C06,1);
  if(ph!=GP_OUT){
    snprintf(b,sizeof b,"ROUND %d OF %d",E->gRound+(ph==GP_PICK||ph==GP_DEAL?1:0),GAMBLE_ROUNDS);
    text(b,BLKX+PLW/2,134,2,0xE8D8A8,1,1);
  }

  /* what each call would pay right now */
  if(ph==GP_PICK||ph==GP_DEAL){
    snprintf(b,sizeof b,"WINS %d",E->gPot*2);
    text(b,REDX+PLW/2,PLY+146,2,0xFFFFFF,1,1);
    text(b,BLKX+PLW/2,PLY+146,2,0xFFFFFF,1,1);
    snprintf(b,sizeof b,"WINS %d",E->gPot*4);
    text(b,LBRX,CHIPY+32,2,0xFFFFFF,1,1);
  }
  /* the call that was made */
  if(ph==GP_FLIP||res){
    uint32_t hc = res ? (won?0x7CFF6A:0xFF4A4A) : 0xFFFFFF;
    int a=(int)(200+55*pl);
    if(E->gChoice<2){
      int x=E->gChoice==0?REDX:BLKX;
      fb_rframe(x-6,PLY-6,PLW+12,PLH+12,20,4.0f,hc,a);
    }
  }

  /* the card, with a halo behind it on a win */
  int cx=FBW/2;
  if(res && won){
    float f=clampf(1.0f-t/1.2f,0.25f,1.0f);
    ex_glow(cx,CARDY+CDH/2,190,(int)(255*f),(int)(190*f),(int)(60*f));
    fx_rays(cx,CARDY+CDH/2,120,320,20,G.t*0.7f,0xFFD24A,(int)(170*f));
  } else if(res){
    ex_glow(cx,CARDY+CDH/2,140,(int)(120*clampf(1.0f-t,0,1)),0,0);
  }
  switch(ph){
  case GP_DEAL: {
    float u=clampf(t/0.35f,0,1), e=1.0f-(1.0f-u)*(1.0f-u)*(1.0f-u);
    ex_card(cx,CARDY-(int)(260*(1.0f-e)),1.0f,-1,(int)(255*u),0.0f);
    break; }
  case GP_PICK:
    ex_card(cx,CARDY+(int)(sinf(G.t*2.2f)*2.0f),1.0f,-1,255,0.0f);
    break;
  case GP_FLIP: {
    float u=clampf(t/0.55f,0,1), s=fabsf(cosf(u*3.14159f));
    int lift=(int)(sinf(u*3.14159f)*12.0f);
    ex_card(cx,CARDY-lift,s,u<0.5f?-1:E->gCard,255,(1.0f-s)*0.6f);
    break; }
  default:
    ex_card(cx,CARDY,1.0f,E->gCard,255,0.0f);
    break;
  }
  if(res && !won){                            /* the stake, in pieces */
    float tt=t;
    for(int k=0;k<18;k++){
      uint32_t sd=0x5A4Du+k*131u+(uint32_t)E->gRound*7u;
      float ox=FBW/2-116+ex_hf(sd,1)*232, oy=124+ex_hf(sd,2)*34;
      float vx=(ex_hf(sd,3)-0.5f)*420.0f, vy=-60.0f-ex_hf(sd,4)*260.0f;
      float x=ox+vx*tt, y=oy+vy*tt+520.0f*tt*tt;
      float sz=8.0f+ex_hf(sd,5)*14.0f, rot=ex_hf(sd,6)*TAU+tt*(ex_hf(sd,7)-0.5f)*14.0f;
      int a=(int)(255*clampf(1.0f-tt/1.3f,0,1));
      if(a<=0) continue;
      ex_tri(x+cosf(rot)*sz,y+sinf(rot)*sz,x+cosf(rot+2.2f)*sz*0.8f,y+sinf(rot+2.2f)*sz*0.8f,
             x+cosf(rot+4.1f)*sz*0.6f,y+sinf(rot+4.1f)*sz*0.6f,k&1?0xFFB020:0xFF5A2A,a);
    }
  }

  /* the suit chips */
  for(int i=0;i<4;i++){
    int x=CHIPX0+i*(CHIPW+8), y=CHIPY;
    int sel=(i==E->gSuit), called=(E->gChoice==2+i && (ph==GP_FLIP||res));
    if(!sel && !called) continue;             /* the chips are in the table */
    uint32_t fc = called ? (res?(won?0x7CFF6A:0xFF4A4A):0xFFFFFF) : mixc(0xFFB020,0xFFFFFF,pl*0.6f);
    fb_rframe(x-2,y-2,CHIPW+4,CHIPH+4,13,3.5f,fc,255);
    if(sel && ph==GP_PICK){
      int ax=x+CHIPW/2, ay=y-7-(int)(pl*3);
      ex_tri((float)ax-8,(float)ay-8,(float)ax+8,(float)ay-8,(float)ax,(float)ay,0xFFD24A,255);
    }
  }

  /* the last cards dealt */
  for(int i=0;i<EX_NHIST;i++){
    int x=HISTX0+i*(MCW+8), y=HISTY, h=E->gHist[i];
    if(i==0 && (ph==GP_FLIP||ph==GP_DEAL)) h=E->gHist[1];   /* not until it lands */
    if(i==0 && (ph==GP_FLIP||ph==GP_DEAL)) continue;
    if(!h){ fb_rframe(x,y,MCW,MCH,6,1.2f,0x5AA070,140); continue; }
    int card=h-1, suit=(card>>4)&3, rank=card&15;
    blit(&cardMini[suit],x,y,0,FBH,255,0,0.0f);
    text(RANKS[rank>12?12:rank],x+5,y+5,2,suit<2?0xC8101E:0x101018,0,0);
    if(i==0) fb_rframe(x-2,y-2,MCW+4,MCH+4,8,2.0f,0xFFD24A,255);
  }

  /* COLLECT: the button is in the table; it glows while it can be pressed */
  { int x=COLX, y=HISTY, w=COLW, h=MCH;
    if(ph==GP_PICK) fb_rframe(x-4,y-4,w+8,h+8,17,2.5f,mixc(0xFFD24A,0xFFFFFF,pl*0.6f),(int)(150+100*pl));
    /* the same figure as the LED: the result must not show before the card */
    snprintf(b,sizeof b,"%lld",(res && !won) ? 0LL : shown);
    text(b,x+w/2,y+40,2,0x3A2004,1,0);
  }
}
