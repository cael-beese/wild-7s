/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_hold.c - HOLD & SPIN, the LUCKY COIN feature.
 *
 *  Every LUCKY COIN that lands carries a value: a multiple of the bet,
 *  or now and then a MINOR or MAJOR jackpot label.  Coins pay nothing on
 *  their own.  Six or more anywhere start HOLD & SPIN: the coins lock,
 *  every other cell becomes its own little reel, and there are three
 *  respins.  Any coin that lands locks too and puts the respins back to
 *  three.  It ends when the respins run out or all 25 cells are full -
 *  and a full board is the GRAND, which also takes the MEGA pot.  Then
 *  every coin is counted into the win, one at a time.
 *
 *  The maths is a handful of small functions - hold_setup, hold_roll,
 *  hold_lock, hold_after_spin, hold_settle - and both the game and the
 *  simulator (hold_sim_play) drive exactly those, so the return the
 *  simulator measures is the return the cabinet pays.
 *
 *  The show is kept apart from the maths: hold_update() only decides
 *  WHEN the player sees each result, never what it is.
 * ===================================================================== */

/* === THE MATHS =================================================== */
#define HOLD_NEED 6                        /* coins that start the feature */
#define HOLD_SPINS 3                       /* respins, and the reset value */

/*  What a coin carries, in HALF bets (every bet is a multiple of 10, so
 *  half of it is always whole), and how often: 0.5x 1x 1.5x 2x 3x 5x 10x
 *  and 25x.  Weighted hard toward the bottom.  Small coins are what let
 *  a board fill up - the feature is about coins landing, and a respin
 *  that lands one is worth more to the player than a fat value on a
 *  board that never grows - and they make a 25x coin an event.        */
#define HV_N 8
static const int HV_HALF[HV_N] = {   1,   2,   3,   4,   6,  10,  20,  50 };
static const int HV_WT[HV_N]   = { 460, 290, 105,  70,  38,  21,  11,   5 };

/*  Variables rather than constants so the simulator can sweep them, as
 *  with the free-spin dials.  The values here are the ones that ship.
 *    HOLD_Q_MINOR / MAJOR  chance a coin is a jackpot coin instead
 *    HOLD_P0               chance an empty cell lands a coin on a respin
 *                          with the six-coin board of a fresh trigger
 *    HOLD_PK               ... multiplied by this for every extra coin: a
 *                          board warms up as it fills, so a good start
 *                          is worth something
 *    HOLD_HOT              ... and by this once HOLD_HOTN coins are down,
 *                          which is when the heartbeat starts.  Fewer
 *                          cells are left, so without it a GRAND would be
 *                          a rumour rather than a prize.                  */
#define HOLD_HOTN 20
static float HOLD_Q_MINOR = 0.0110f;
static float HOLD_Q_MAJOR = 0.0010f;
static float HOLD_P0      = 0.0270f;
static float HOLD_PK      = 1.0300f;
static float HOLD_HOT     = 2.0000f;

static float hold_land_chance(int locked){
  int extra = locked-HOLD_NEED; if(extra<0) extra=0;
  float p=HOLD_P0*powf(HOLD_PK,(float)extra);
  if(locked>=HOLD_HOTN) p*=HOLD_HOT;
  return p;
}

/*  Test hook, for capturing the rare parts of the show: with
 *  WILD7_FORCE=hold, setting WILD7_HOLD=grand makes respins land coins
 *  freely and jackpot coins common.  Only ever set by hold_begin() under
 *  WILD7_FORCE, so it cannot reach normal play or the simulator.      */
static int hold_dbg = 0;

/* a fresh coin: a bet multiple, or rarely a jackpot label */
static void hold_roll_coin(int*val,int*lab){
  float u=frnd();
  float qma=HOLD_Q_MAJOR, qmi=HOLD_Q_MINOR;
  if(hold_dbg){ qma=0.06f; qmi=0.10f; }
  if(u<qma){ *val=0; *lab=HL_MAJOR; return; }
  if(u<qma+qmi){ *val=0; *lab=HL_MINOR; return; }
  int tot=0; for(int i=0;i<HV_N;i++) tot+=HV_WT[i];
  int k=irnd(tot), i=0;
  while(i<HV_N-1 && k>=HV_WT[i]){ k-=HV_WT[i]; i++; }
  *val=HV_HALF[i]*(TOTBET/2); *lab=HL_NONE;
}

/* every coin on the grid gets its value the moment the reels land */
static void hold_on_snapshot(void){
  for(int c=0;c<NCELL;c++){
    G.hold.val[c]=0; G.hold.lab[c]=HL_NONE;
    if(G.grid[c/NROW][c%NROW]==SY_COIN) hold_roll_coin(&G.hold.val[c],&G.hold.lab[c]);
  }
}

static int hold_triggered(void){ return G.coinCount>=HOLD_NEED; }

static inline int hold_has(int c){ return G.hold.bval[c]>0; }

/*  Lock a coin onto the board.  A jackpot coin takes its pot as it
 *  locks and resets it, exactly as a reel jackpot does in evaluate(), so
 *  a second MINOR in the same feature is worth the bet multiple alone. */
static void hold_lock(int c,int val,int lab){
  hold_state_t*H=&G.hold;
  if(lab==HL_MINOR||lab==HL_MAJOR){
    int t = lab==HL_MINOR ? JP_MINOR : JP_MAJOR;
    H->bval[c]=jp_value(t); G.jpAcc[t]=0;
  } else H->bval[c]=val;
  H->blab[c]=lab;
  H->locked++;
}

/* the triggering grid becomes the board */
static void hold_setup(void){
  hold_state_t*H=&G.hold;
  memset(H->bval,0,sizeof H->bval); memset(H->blab,0,sizeof H->blab);
  memset(H->land,0,sizeof H->land);
  H->locked=0; H->newThis=0; H->grand=0; H->grandAmt=0; H->total=0;
  H->respins=HOLD_SPINS;
  for(int c=0;c<NCELL;c++)
    if(G.grid[c/NROW][c%NROW]==SY_COIN) hold_lock(c,H->val[c],H->lab[c]);
}

/*  One respin: spend a respin, then decide every empty cell.  The results
 *  are only decided here; hold_lock() puts them on the board, which the
 *  game does cell by cell as each little reel stops.                  */
static void hold_roll(void){
  hold_state_t*H=&G.hold;
  H->respins--;
  H->newThis=0;
  float p=hold_land_chance(H->locked);
  if(hold_dbg) p=0.30f;                /* WILD7_HOLD=grand: fill the board */
  for(int c=0;c<NCELL;c++){
    H->land[c]=0;
    if(hold_has(c)) continue;
    if(frnd()<p){
      H->land[c]=1; H->newThis++;
      hold_roll_coin(&H->nval[c],&H->nlab[c]);
    }
  }
}

/* after every cell of a respin has stopped: 1 = the feature is over */
static int hold_after_spin(void){
  hold_state_t*H=&G.hold;
  if(H->newThis>0) H->respins=HOLD_SPINS;
  if(H->locked>=NCELL){ H->grand=1; return 1; }
  return H->respins<=0;
}

/* the prize: every coin, plus the MEGA pot for a full board */
static void hold_settle(void){
  hold_state_t*H=&G.hold;
  long long t=0;
  for(int c=0;c<NCELL;c++) t+=H->bval[c];
  if(H->grand){ H->grandAmt=jp_value(JP_MEGA); G.jpAcc[JP_MEGA]=0; t+=H->grandAmt; }
  H->total=t;
}

/* -- the simulator's view ------------------------------------------
 *  Same functions, no clocks.  The counters are simulator statistics,
 *  not game state, so they live outside G.                            */
static long long hsN, hsEnd[NCELL+1], hsMinor, hsMajor, hsGrand, hsSpins;
static double    hsX, hsPotX;

static long long hold_sim_play(void){
  hold_state_t*H=&G.hold;
  hold_setup();
  for(;;){
    hold_roll(); hsSpins++;
    for(int c=0;c<NCELL;c++) if(H->land[c]){ hold_lock(c,H->nval[c],H->nlab[c]); H->land[c]=0; }
    if(hold_after_spin()) break;
  }
  hold_settle();
  hsN++; hsEnd[H->locked]++;
  hsX += (double)H->total/TOTBET;
  int mi=0, ma=0; double pot=0;
  for(int c=0;c<NCELL;c++){
    if(H->blab[c]==HL_MINOR){ mi=1; pot+=(double)H->bval[c]/TOTBET; }
    if(H->blab[c]==HL_MAJOR){ ma=1; pot+=(double)H->bval[c]/TOTBET; }
  }
  if(H->grand){ hsGrand++; pot+=(double)H->grandAmt/TOTBET; }
  hsMinor+=mi; hsMajor+=ma; hsPotX+=pot;
  return H->total;
}

static void hold_sim_report(void){
  if(!hsN){ printf("  hold & spin    never triggered\n"); return; }
  long long a=0,b=0,c=0;
  for(int n=0;n<=8;n++) a+=hsEnd[n];
  for(int n=9;n<=14;n++) b+=hsEnd[n];
  for(int n=15;n<NCELL;n++) c+=hsEnd[n];
  double N=(double)hsN;
  printf("  hold & spin    mean prize %.1f x bet (pots %.1f x), %.1f respins\n",
         hsX/N, hsPotX/N, (double)hsSpins/N);
  printf("                 ends with 6-8 coins %.1f%%  9-14 %.1f%%  15-24 %.2f%%  GRAND %.3f%% (1 in %.0f features)\n",
         100.0*a/N, 100.0*b/N, 100.0*c/N, 100.0*hsEnd[NCELL]/N,
         hsEnd[NCELL]? N/hsEnd[NCELL] : 0.0);
  printf("                 a MINOR coin in %.2f%% of features, a MAJOR in %.2f%%\n",
         100.0*hsMinor/N, 100.0*hsMajor/N);
}

/*  WILD7_FORCE=hold: coins are singles, so demand them on chosen rows.
 *  Every reel shows one; the first two reels (of 1, 5, 3, 2, 4) whose
 *  strip holds two coins close enough to share a window show both,
 *  which makes seven - a genuine set of reel stops.                    */
static int hold_force_gap(int r){
  int best=0;
  for(int s=0;s<STRIPLEN;s++){
    if(strip[r][s]!=SY_COIN) continue;
    for(int g=1;g<NROW;g++) if(stripAt(r,s+g)==SY_COIN){ if(!best||g<best) best=g; break; }
  }
  return best;
}
static int hold_force_rows(int r,int*rows){
  static const int pref[NREEL]={0,4,2,1,3};
  int pairs=0;
  for(int i=0;i<NREEL && pairs<2;i++){
    int q=pref[i], g=hold_force_gap(q);
    if(!g) continue;
    pairs++;
    if(q==r){ rows[0]=0; rows[1]=g; return 2; }
  }
  rows[0]=2; return 1;
}

/* === ART =========================================================
 *  Everything is baked at init: coins in three finishes at two sizes,
 *  the spinning ghost, glows, lamps, a chunky display font, and the
 *  whole dark board behind the grid.  At run time it is blits.
 * ================================================================= */
#define HHEAD 80                          /* header band atop the window */
#define HGY   (GY+HHEAD)                  /* board top                   */
#define HCH   ((GH-HHEAD)/NROW)           /* 92: board cell height       */
#define HCOIN_S 0.84f                     /* board coins, vs reel coins  */
#define LAMPX(i) (GX+46+(i)*62)           /* respin lamps                */
#define LAMPY    (GY+46)
#define METX  (GX+GW-204)                 /* the TOTAL / WIN meter       */
#define METY2 (GY+9)
#define METW  194
#define METH2 62

static spr_t hcoin[3];      /* board coins: gold, MINOR, MAJOR            */
static spr_t bcoin[3];      /* reel-size MINOR / MAJOR, over the strip coin */
static spr_t hghost;        /* smeared coin for a turning cell            */
/*  A rim glow is a thin annulus, so a square sprite would spend most of
 *  its loop on transparent pixels - with 25 coins that was the single
 *  biggest cost on a full board.  These keep only the pixels that glow. */
typedef struct { int n; int16_t *dx, *dy; uint8_t *a; int r,g,b; } hring_t;
static hring_t hring[3];    /* additive rim glow, board size, per finish  */
static hring_t bring;       /* additive rim glow, reel size               */
static spr_t hcoinD[3];     /* board coins, dimmed: already counted       */
static uint32_t *hbkD;      /* the backdrop at 3/8, behind the win slam   */
static spr_t hlamp[2];      /* respin lamp, off and on                    */
static spr_t hhalo;         /* additive halo round a lit lamp             */
static spr_t hspark;        /* additive spark for the collect flight      */
static spr_t hspin;         /* a turning cell's lit drum, socket-sized    */
static uint32_t *hbk;       /* the board backdrop, GW x GH                */

/* chunky display type: the bubble lettering, baked per glyph */
enum { HF_G2, HF_G3, HF_G4, HF_G9, HF_I2, HF_N };
static const int HF_PX[HF_N]  = { 2, 3, 4, 9, 2 };
static const int HF_PAL[HF_N] = { 0, 0, 0, 0, 1 };
static spr_t hglyph[HF_N][64];
static int   hfpad[HF_N];

static void hspr_free(spr_t*s){
  free(s->px); free(s->rx0); free(s->rx1);
  s->px=NULL; s->rx0=s->rx1=NULL; s->w=s->h=0;
}

/* straight-alpha "over" into an RGBA byte quad */
static void hpx_over(uint8_t*p,uint32_t col,int a){
  if(a<=0) return;
  if(a>255) a=255;
  int da=p[3], oa=a+da*(255-a)/255;
  if(oa<=0) return;
  p[0]=(uint8_t)((((col>>16)&255)*a + p[0]*da*(255-a)/255)/oa);
  p[1]=(uint8_t)((((col>>8)&255)*a  + p[1]*da*(255-a)/255)/oa);
  p[2]=(uint8_t)(((col&255)*a       + p[2]*da*(255-a)/255)/oa);
  p[3]=(uint8_t)oa;
}

static void hold_bake_font(int f){
  static const uint32_t PAL[2][5]={
    {0xFFFDF0,0xFFEBA8,0xF0B420,0xA8760C,0x5E4206},        /* gold        */
    {0xFFFFFF,0xE8F6FF,0x9CD2F4,0x3C82BC,0x143A62} };      /* ice silver  */
  int px=HF_PX[f];
  float Rin=px*0.64f, Rout=Rin+(px*0.42f>1.5f?px*0.42f:1.5f);
  int so=(int)(px*0.34f+0.5f); if(so<1) so=1;
  int pad=(int)(Rout+2.0f);
  hfpad[f]=pad;
  int w=5*px+2*pad+so, h=7*px+2*pad+so;
  uint8_t*fm=(uint8_t*)calloc((size_t)w*h,1), *om=(uint8_t*)calloc((size_t)w*h,1);
  if(!fm||!om){ free(fm); free(om); return; }
  for(int ch=32;ch<96;ch++){
    spr_t*s=&hglyph[f][ch-32];
    hspr_free(s);
    memset(fm,0,(size_t)w*h); memset(om,0,(size_t)w*h);
    const uint8_t*gl=FONT[ch-32];
    #define HSET(rr,cc) ((rr)>=0&&(rr)<7&&(cc)>=0&&(cc)<5&&(gl[rr]&(0x10>>(cc))))
    for(int r=0;r<7;r++) for(int c2=0;c2<5;c2++){
      if(!HSET(r,c2)) continue;
      float cx=pad+c2*px+px*0.5f, cy=pad+r*px+px*0.5f;
      tb_stamp(om,w,h,cx,cy,Rout); tb_stamp(fm,w,h,cx,cy,Rin);
      static const int NB[4][2]={{0,1},{1,0},{1,1},{1,-1}};
      for(int k=0;k<4;k++){
        int nr=r+NB[k][0], nc=c2+NB[k][1];
        if(!HSET(nr,nc)) continue;
        float nx=pad+nc*px+px*0.5f, ny=pad+nr*px+px*0.5f;
        tb_cap(om,w,h,cx,cy,nx,ny,Rout); tb_cap(fm,w,h,cx,cy,nx,ny,Rin);
      }
    }
    #undef HSET
    s->w=w; s->h=h;
    s->px=(uint8_t*)calloc((size_t)w*h,4);
    if(!s->px){ s->w=s->h=0; continue; }
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
      uint8_t*o=s->px+((size_t)y*w+x)*4;
      if(x>=so && y>=so) hpx_over(o,0x000000,om[(y-so)*w+(x-so)]*170/255);
      hpx_over(o,0x1A0C02,om[y*w+x]);
      int fa=fm[y*w+x];
      if(fa){
        float t=clampf((float)(y-pad)/(float)(7*px-1),0,1);
        uint32_t col=ramp(PAL[HF_PAL[f]],5,t);
        if(t<0.34f) col=mixc(col,0xFFFFFF,(0.34f-t)/0.34f*0.55f);
        hpx_over(o,col,fa);
      }
    }
    spr_bounds(s);
  }
  free(fm); free(om);
}

static int htext_w(const char*s,int f){
  int n=(int)strlen(s), px=HF_PX[f];
  return n>0 ? n*6*px-px : 0;
}
/* baked display type; align 0 left, 1 centre, 2 right; clipped to rows */
static void htext(const char*s,int x,int y,int f,int align,int cy0,int cy1){
  int px=HF_PX[f], w=htext_w(s,f);
  int ox = align==1 ? x-w/2 : (align==2 ? x-w : x);
  for(int i=0;s[i];i++){
    int ch=(unsigned char)s[i];
    if(ch>='a'&&ch<='z') ch-=32;
    if(ch<32||ch>95) ch='?';
    if(ch==' ') continue;
    blit(&hglyph[f][ch-32], ox+i*6*px-hfpad[f], y-hfpad[f], cy0,cy1,255,0,0.0f);
  }
}
/* the biggest of the given sizes that fits, centred on (x, cy) */
static void htext_fit(const char*s,int x,int cy,int maxw,const int*fonts,int nf,int cy0,int cy1){
  int f=fonts[nf-1];
  for(int i=0;i<nf;i++) if(htext_w(s,fonts[i])<=maxw){ f=fonts[i]; break; }
  htext(s,x,cy-HF_PX[f]*7/2,f,1,cy0,cy1);
}

/* credits on a coin: whole up to 9999, then K and M so it fits the band */
static void hold_fmt(long long v,char*b,size_t n){
  if(v<0) v=0;
  if(v<10000) snprintf(b,n,"%d",(int)v);
  else if(v<1000000){
    int k=(int)(v/1000), d=(int)((v%1000)/100);
    if(v>=100000 || !d) snprintf(b,n,"%dK",k); else snprintf(b,n,"%d.%dK",k,d);
  } else if(v<1000000000LL){
    int m=(int)(v/1000000), d=(int)((v%1000000)/100000);
    if(v>=100000000LL || !d) snprintf(b,n,"%dM",m); else snprintf(b,n,"%d.%dM",m,d);
  } else snprintf(b,n,"%dB",(int)(v/1000000000LL>999?999:v/1000000000LL));
}

/* -- the coin ------------------------------------------------------
 *  A heavy milled gold piece rather than a symbol on a medallion: no
 *  chrome ring, a serrated edge, a beaded border, an embossed emblem up
 *  top, and a dark enamel cartouche across the middle where the value
 *  is printed at run time.  kind 0 plain gold with a star, 1 the MINOR
 *  (silver and blue, a cut gem), 2 the MAJOR (red and gold, a crown).
 *  s scales it about the centre so the board can use a smaller piece. */
static void coin_paint(float s,int kind){
  #define CX(v) U(46.0f+((v)-46.0f)*s)
  #define CR(v) U((v)*s)
  static const uint32_t RIM[3][7]={
    {0xFFFBE0,0xFFE27A,0xD8A01C,0x8E6208,0xFFD860,0x7A5006,0x3A2402},
    {0xFFFFFF,0xE8EEF8,0x9AA6C0,0x5A6480,0xDDE4F2,0x6A7490,0x2A3044},
    {0xFFFBE0,0xFFE27A,0xD8A01C,0x8E6208,0xFFD860,0x7A5006,0x3A2402} };
  static const uint32_t FACE[3][3]={
    {0x6A3E00,0xE8A818,0xFFF4C0},
    {0x0A1E4A,0x3A6AC0,0xDCEEFF},
    {0x4A0008,0xC41424,0xFFB4A0} };
  static const uint32_t GROOVE[3]={0x3A2402,0x1A2034,0x3A1402};
  static const uint32_t BAND[3][2]={{0x6A0A12,0x1E0206},{0x0C2A6A,0x020A22},{0x2A0A02,0x080200}};
  const int k=kind<0?0:(kind>2?2:kind);

  cv_circle(CX(47.6f),CX(48.8f),CR(45.2f),0x000000,0x000000,130);        /* shadow   */
  /* milled edge: a 120-point ring whose radius alternates, rim-lit */
  { pt_t p[120];
    for(int i=0;i<120;i++){
      float a=TAU*i/120.0f, r=(i&1)?CR(43.6f):CR(44.8f);
      p[i].x=CX(46)+cosf(a)*r; p[i].y=CX(46)+sinf(a)*r;
    }
    cv_polyN(p,120,RIM[k],7,255); }
  cv_circle(CX(46),CX(46),CR(41.4f),GROOVE[k],GROOVE[k],255);                /* groove   */
  { uint32_t lo=RIM[k][6], mid=RIM[k][2], hi=RIM[k][0];                       /* inner rim */
    cv_ellipse_lit(CX(46),CX(46),CR(40.2f),CR(40.2f),lo,mid,hi); }
  cv_circle(CX(46),CX(46),CR(36.6f),GROOVE[k],GROOVE[k],200);
  cv_ellipse_lit(CX(46),CX(46),CR(35.8f),CR(35.8f),FACE[k][0],FACE[k][1],FACE[k][2]);
  /* beaded border: a ring of little raised dots */
  for(int i=0;i<44;i++){
    float a=TAU*i/44.0f, r=CR(32.6f);
    float x=CX(46)+cosf(a)*r, y=CX(46)+sinf(a)*r;
    cv_circle(x+CR(0.35f),y+CR(0.45f),CR(1.05f),0x000000,0x000000,90);
    cv_circle(x,y,CR(0.95f),RIM[k][1],RIM[k][3],235);
  }
  /* the emblem up top, embossed: dark offset, light rim, gradient body */
  if(k==0){
    static const uint32_t gold[5]={0xFFFFFF,0xFFF3A8,0xFFC02A,0xE07800,0x8A3C00};
    pt_t p[10];
    for(int i=0;i<10;i++){
      float a=-TAU/4+TAU*i/10.0f, r=(i&1)?CR(4.2f):CR(10.2f);
      p[i].x=CX(46)+cosf(a)*r; p[i].y=CX(22.5f)+sinf(a)*r;
    }
    pt_t q[10]; for(int i=0;i<10;i++){ q[i].x=p[i].x+CR(0.8f); q[i].y=p[i].y+CR(1.0f); }
    cv_poly(q,10,0x3A1A00,0x3A1A00,170);
    cv_poly_outline(p,10,CR(1.4f),0xFFF6D0,255);
    cv_polyN(p,10,gold,5,255);
  } else if(k==1){
    static const uint32_t gem[4]={0xFFFFFF,0xB8E8FF,0x3A9AE0,0x0A3A7A};
    pt_t p[6]={{CX(38),CX(17.5f)},{CX(54),CX(17.5f)},{CX(58.5f),CX(22.5f)},
               {CX(46),CX(31)},{CX(33.5f),CX(22.5f)},{CX(38),CX(17.5f)}};
    cv_poly_outline(p,5,CR(1.6f),0xFFFFFF,255);
    cv_polyN(p,5,gem,4,255);
    pt_t f2[4]={{CX(33.5f),CX(22.5f)},{CX(58.5f),CX(22.5f)},{CX(57),CX(24)},{CX(35),CX(24)}};
    cv_poly(f2,4,0xFFFFFF,0xFFFFFF,120);
  } else {
    static const uint32_t gold[5]={0xFFFBE0,0xFFDE78,0xD8A01C,0x8E6208,0x503802};
    const float cs=0.26f, ox=46, oy=23;
    #define KP(px,py) {CX(ox+((px)-46)*cs),CX(oy+((py)-48)*cs)}
    pt_t p[7]={KP(13,76),KP(79,76),KP(79,26),KP(63,47),KP(46,19),KP(29,47),KP(13,26)};
    cv_poly_outline(p,7,CR(1.5f),0xFFF6C8,255);
    cv_polyN(p,7,gold,5,255);
    cv_sphere(CX(ox-33*cs),CX(oy-24*cs),CR(1.6f),0x8E6208,0xE8C040,0xFFF8D0);
    cv_sphere(CX(ox),      CX(oy-31*cs),CR(1.6f),0x8E6208,0xE8C040,0xFFF8D0);
    cv_sphere(CX(ox+33*cs),CX(oy-24*cs),CR(1.6f),0x8E6208,0xE8C040,0xFFF8D0);
    #undef KP
  }
  /* the cartouche: gold frame, dark enamel, a lick of gloss */
  { uint32_t ft=RIM[k][0], fb2=RIM[k][3];
    cv_rrect(CX(14.6f),CX(36.2f),CR(62.8f),CR(21.6f),CR(7.5f),0x000000,0x000000,150);
    cv_rrect(CX(14.0f),CX(35.4f),CR(64.0f),CR(21.8f),CR(7.5f),ft,fb2,255);
    cv_rrect(CX(16.0f),CX(37.2f),CR(60.0f),CR(18.2f),CR(5.8f),BAND[k][0],BAND[k][1],255);
    cv_rrect(CX(18.0f),CX(38.0f),CR(56.0f),CR(4.6f),CR(2.3f),0xFFFFFF,0xFFFFFF,38); }
  /* LUCKY struck into the bottom of the face */
  { const char*w = k==0 ? "LUCKY" : "JACKPOT";
    float tp = k==0 ? 1.55f : 1.25f;
    static const uint32_t INK[3]={0x3E2002,0x061430,0x2E0204};
    cv_text(w,CX(46.45f),CX(66.75f),CR(tp),0xFFF6D0,190);              /* lower lip */
    cv_text(w,CX(46),CX(66.2f),CR(tp),INK[k],255); }
  cv_ellipse(CX(46),CX(24),CR(25),CR(10),0xFFFFFF,0xFFFFFF,40);         /* gloss arc */
  #undef CX
  #undef CR
}

static void hold_bake_coin(spr_t*s,float sc,int kind,spr_t*ghost){
  hspr_free(s);
  cv_clear(); coin_paint(sc,kind); cv_resolve(s);
  add_contour(s,SYMW/30,0x0A0510,255);
  spr_bounds(s);
  if(ghost){
    /* the ghost: the coin smeared down the reel and darkened, as seen
       through a turning cell; built before the shadow goes in */
    hspr_free(ghost);
    ghost->w=s->w; ghost->h=s->h;
    ghost->px=(uint8_t*)calloc((size_t)s->w*s->h,4);
    if(ghost->px){
      const int K=12;
      for(int x=0;x<s->w;x++) for(int y=0;y<s->h;y++){
        int r=0,g=0,b=0,a=0,n=0;
        for(int d=-K;d<=K;d++){
          int yy=y+d; if(yy<0||yy>=s->h) continue;
          const uint8_t*p=s->px+((size_t)yy*s->w+x)*4;
          r+=p[0]*p[3]; g+=p[1]*p[3]; b+=p[2]*p[3]; a+=p[3]; n++;
        }
        uint8_t*o=ghost->px+((size_t)y*s->w+x)*4;
        if(a>0){ o[0]=(uint8_t)(r/a); o[1]=(uint8_t)(g/a*7/8); o[2]=(uint8_t)(b/a*5/8); }
        o[3]=(uint8_t)(n?(a/n)*17/20:0);
      }
      spr_bounds(ghost);
    }
  }
  bake_shadow(s,SYMW/25,SYMH/20,4,200);
}

/* additive radial sprites: a ring (rim glow) or a disc (halo, spark) */
static void hold_bake_glow(spr_t*s,int size,float ring,float width,uint32_t col){
  hspr_free(s);
  s->w=s->h=size;
  s->px=(uint8_t*)calloc((size_t)size*size,4);
  if(!s->px){ s->w=s->h=0; return; }
  float c=size*0.5f;
  for(int y=0;y<size;y++) for(int x=0;x<size;x++){
    float d=sqrtf((x+0.5f-c)*(x+0.5f-c)+(y+0.5f-c)*(y+0.5f-c));
    float v;
    if(ring>0){ float q=(d-ring)/width; v=expf(-q*q) + 0.35f*clampf(1.0f-fabsf(d-ring)/(width*3.2f),0,1); }
    else { v=clampf(1.0f-d/c,0,1); v=v*v; }
    v=clampf(v,0,1);
    if(d>c-1) v=0;
    uint8_t*o=s->px+((size_t)y*size+x)*4;
    o[0]=(uint8_t)((col>>16)&255); o[1]=(uint8_t)((col>>8)&255); o[2]=(uint8_t)(col&255);
    o[3]=(uint8_t)(v*255);
  }
  spr_bounds(s);
}

static void lamp_paint(int on){
  cv_circle(U(46.8f),U(48.2f),U(20.5f),0x000000,0x000000,150);
  static const uint32_t ring[5]={0xFFFBE0,0xFFE27A,0xD8A01C,0x8E6208,0x4A3004};
  pt_t p[48];
  for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=U(46)+cosf(a)*U(19.4f); p[i].y=U(46)+sinf(a)*U(19.4f); }
  cv_polyN(p,48,ring,5,255);
  cv_circle(U(46),U(46),U(16.6f),0x2A1802,0x100800,255);
  if(on){
    cv_sphere(U(46),U(46),U(15.6f),0x8A1000,0xFF4A18,0xFFF6D0);
    cv_circle(U(46),U(47),U(8.5f),0xFFFBE0,0xFFC050,120);
  } else {
    cv_sphere(U(46),U(46),U(15.6f),0x0A0203,0x3A0C0E,0x9A6464);
  }
}

/* the board: deep crimson falling to black, a faint gold sunburst and
   lattice, a header band, and twenty-five recessed gold-rimmed sockets.
   Painted through the ordinary primitives into fb, then kept.  Called
   at init (fb is scratch then) and, defensively, from update code.   */
static void hold_build_backdrop(void){
  if(hbk) return;
  uint32_t*buf=(uint32_t*)malloc((size_t)GW*GH*4);
  if(!buf) return;
  float cx=GX+GW*0.5f, cy=HGY+(GH-HHEAD)*0.5f;
  for(int y=GY;y<GY+GH;y++) for(int x=GX;x<GX+GW;x++){
    float dx=(x-cx)/380.0f, dy=(y-cy)/310.0f;
    float d=sqrtf(dx*dx+dy*dy), v=clampf(1.0f-d,0,1);
    uint32_t c=mixc(0x070103,0x96121E,clampf(v*v*1.25f,0,1));
    float a=atan2f(dy,dx);
    float rr=0.5f+0.5f*cosf(a*20.0f);
    rr=rr*rr; rr=rr*rr; rr=rr*rr;
    c=mixc(c,0xFFB848,rr*(0.10f+v*0.34f));
    if(((x+y)%26)==0 || ((x-y+2600)%26)==0) c=mixc(c,0xE8B040,0.06f+v*0.10f);
    fb_px(x,y,c);
  }
  /* a gold frame round the board */
  fb_rframe(GX+2,HGY+1,GW-4,GH-HHEAD-3,10,2.0f,0xF0C24A,210);
  fb_rframe(GX+5,HGY+4,GW-10,GH-HHEAD-9,8,1.0f,0x6A4A10,200);
  /* header */
  fb_rrectg(GX+4,GY+4,GW-8,HHEAD-8,12,0x3E080E,0x0C0204,255);
  for(int j=0;j<14;j++) for(int i=10;i<GW-10;i++) fb_blend(GX+i,GY+6+j,0xFFFFFF,(14-j)*2);
  fb_rframe(GX+4,GY+4,GW-8,HHEAD-8,12,1.5f,0xB8862A,230);
  fb_rect(GX+10,HGY-3,GW-20,2,0xF0C24A,255);
  fb_rect(GX+10,HGY-1,GW-20,1,0x5A3A08,255);
  /* lamp plate */
  fb_rrectg(GX+12,GY+10,190,62,11,0x1E0407,0x060102,255);
  fb_rframe(GX+12,GY+10,190,62,11,1.5f,0xC8962E,230);
  for(int i=0;i<3;i++) fb_rrect(LAMPX(i)-25,LAMPY-25,50,50,25,0x000000,190);
  text("RESPINS",GX+107,GY+14,1,0xFFD98A,1,1);
  /* meter */
  led_window(METX,METY2,METW,METH2);
  /* sockets */
  for(int c2=0;c2<NCELL;c2++){
    int r=c2/NROW, row=c2%NROW;
    int sx=GX+r*CW+6, sy=HGY+row*HCH+4, w=CW-12, h=HCH-8;
    fb_rrect(sx-2,sy-1,w+4,h+4,15,0x000000,170);
    fb_rrectg(sx,sy,w,h,14,0x040001,0x2A070B,222);          /* the sunburst glints through */
    for(int j=0;j<8;j++) for(int i=8;i<w-8;i++) fb_blend(sx+i,sy+2+j,0x000000,(8-j)*18);
    /* where a coin will sit: a faint struck ring */
    int ccx=sx+w/2, ccy=sy+h/2;
    fb_rframe(ccx-37,ccy-37,74,74,37,1.4f,0x8A5A22,60);
    fb_rframe(ccx-31,ccy-31,62,62,31,1.0f,0x5A3A18,45);
    fb_rframe(sx,sy,w,h,14,1.6f,0xC8962E,215);
    fb_rframe(sx+3,sy+3,w-6,h-6,11,1.0f,0x5A1A10,150);
  }
  for(int y=0;y<GH;y++) memcpy(buf+(size_t)y*GW,fb+(size_t)(GY+y)*FBW+GX,(size_t)GW*4);
  hbk=buf;
  /* and its twin at 3/8 brightness, the stage for the final win */
  free(hbkD);
  hbkD=(uint32_t*)malloc((size_t)GW*GH*4);
  if(hbkD) for(size_t i=0;i<(size_t)GW*GH;i++){
    uint32_t d=hbk[i];
    hbkD[i]=((d>>2)&0x3F3F3Fu)+((d>>3)&0x1F1F1Fu);
  }
}

static void hring_free(hring_t*s){
  free(s->dx); free(s->dy); free(s->a);
  s->dx=s->dy=NULL; s->a=NULL; s->n=0;
}
/* a glowing annulus: a gaussian at radius `ring`, with a faint halo */
static void hold_bake_ring(hring_t*s,float ring,float width,uint32_t col){
  hring_free(s);
  int R=(int)(ring+width*3.4f)+1, cap=(2*R+1)*(2*R+1);
  s->dx=(int16_t*)malloc(cap*sizeof(int16_t));
  s->dy=(int16_t*)malloc(cap*sizeof(int16_t));
  s->a =(uint8_t*)malloc(cap);
  if(!s->dx||!s->dy||!s->a){ hring_free(s); return; }
  s->r=(col>>16)&255; s->g=(col>>8)&255; s->b=col&255;
  for(int y=-R;y<=R;y++) for(int x=-R;x<=R;x++){
    float d=sqrtf((x+0.5f)*(x+0.5f)+(y+0.5f)*(y+0.5f));
    float q=(d-ring)/width;
    float v=expf(-q*q)+0.35f*clampf(1.0f-fabsf(d-ring)/(width*3.2f),0,1);
    int a=(int)(clampf(v,0,1)*255);
    if(a<6) continue;
    s->dx[s->n]=(int16_t)x; s->dy[s->n]=(int16_t)y; s->a[s->n]=(uint8_t)a; s->n++;
  }
}
static void hring_add(const hring_t*s,int cx,int cy,int amt,int cy0,int cy1){
  if(!s->n||amt<=0) return;
  if(amt>255) amt=255;
  int ylo=cy0, yhi=cy1;
  if(ylo<clip_y0) ylo=clip_y0;
  if(yhi>clip_y1) yhi=clip_y1;
  if(ylo<0) ylo=0;
  if(yhi>FBH) yhi=FBH;
  if(yhi<=ylo) return;
  int kr=s->r*amt, kg=s->g*amt, kb=s->b*amt;
  for(int i=0;i<s->n;i++){
    int y=cy+s->dy[i], x=cx+s->dx[i];
    if(y<ylo||y>=yhi||(unsigned)x>=FBW) continue;
    int a=s->a[i];
    uint32_t*p=fb+(size_t)y*FBW+x, d=*p;
    int r=((d>>16)&255)+((kr*a)>>16), g=((d>>8)&255)+((kg*a)>>16), b=(d&255)+((kb*a)>>16);
    *p=RGB(r>255?255:r,g>255?255:g,b>255?255:b);
  }
}

/* a copy of a coin, dimmed and warmed toward black: a coin already counted */
static void hold_dim_copy(const spr_t*src,spr_t*dst){
  hspr_free(dst);
  if(!src->px) return;
  dst->w=src->w; dst->h=src->h;
  dst->px=(uint8_t*)malloc((size_t)src->w*src->h*4);
  if(!dst->px){ dst->w=dst->h=0; return; }
  for(size_t i=0;i<(size_t)src->w*src->h;i++){
    const uint8_t*p=src->px+i*4; uint8_t*o=dst->px+i*4;
    uint32_t c=mixc(RGB(p[0],p[1],p[2]),0x140604,0.58f);
    o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255); o[3]=p[3];
  }
  spr_bounds(dst);
}

/*  A turning cell is lit from inside like a reel drum: dark at the lips,
 *  a hot crimson band across the middle, and faint vertical speed
 *  streaks.  Sized to sit just inside a socket's gold rim.            */
#define HSPW (CW-16)
#define HSPH (HCH-12)
static void hold_bake_spin(void){
  spr_t*s=&hspin;
  hspr_free(s);
  s->w=HSPW; s->h=HSPH;
  s->px=(uint8_t*)calloc((size_t)HSPW*HSPH,4);
  if(!s->px){ s->w=s->h=0; return; }
  float hw=HSPW*0.5f, hh=HSPH*0.5f;
  for(int y=0;y<HSPH;y++){
    float t=(y+0.5f)/HSPH;
    float f=sinf(t*3.14159f); f=f*sqrtf(f);
    uint32_t base=mixc(0x0E0103,0x6A1210,f);
    base=mixc(base,0xE0602A,f*f*f*f*f*f*0.42f);
    for(int x=0;x<HSPW;x++){
      float d=rr_sdf(x+0.5f,y+0.5f,hw,hh,hw,hh,12.0f);
      float a=clampf(0.5f-d,0,1);
      if(a<=0) continue;
      uint32_t c=base;
      uint32_t h=(uint32_t)x*0x9E3779B1u; h^=h>>13; h*=0x85EBCA77u; h^=h>>16;
      if((h&7u)==0) c=mixc(c,0xFFC070,0.10f+0.14f*f);          /* speed streaks */
      float e=fabsf(x+0.5f-hw)/hw;                                /* drum curvature */
      if(e>0.80f) c=scalec(c,1.0f-(e-0.80f)*1.6f);
      uint8_t*o=s->px+((size_t)y*HSPW+x)*4;
      o[0]=(uint8_t)((c>>16)&255); o[1]=(uint8_t)((c>>8)&255); o[2]=(uint8_t)(c&255);
      o[3]=(uint8_t)(a*255);
    }
  }
  spr_bounds(s);
}

static void art_coin(void){
  hold_bake_spin();
  for(int k=0;k<3;k++){
    hold_bake_coin(&hcoin[k],HCOIN_S,k,k==0?&hghost:NULL);
    hold_dim_copy(&hcoin[k],&hcoinD[k]);
    if(k) hold_bake_coin(&bcoin[k],1.0f,k,NULL);
  }
  hold_bake_ring(&hring[0],46.0f,4.5f,0xFFC850);
  hold_bake_ring(&hring[1],46.0f,4.5f,0x6AC8FF);
  hold_bake_ring(&hring[2],46.0f,4.5f,0xFF5030);
  hold_bake_ring(&bring,   55.0f,5.0f,0xFFC850);
  hold_bake_glow(&hhalo,    96,0,0,0xFF7A30);
  hold_bake_glow(&hspark,   56,0,0,0xFFE8A0);
  for(int on=0;on<2;on++){
    hspr_free(&hlamp[on]);
    cv_clear(); lamp_paint(on); cv_resolve(&hlamp[on]); spr_bounds(&hlamp[on]);
  }
  for(int f=0;f<HF_N;f++) hold_bake_font(f);
  free(hbk); hbk=NULL;
  hold_build_backdrop();
  /* and last, the reel symbol itself, which build_sprites resolves */
  cv_clear(); coin_paint(1.0f,0);
}

/* additive blit, through the sprite's alpha; honours the band clip */
static void hblit_add(const spr_t*s,int dx,int dy,int amt,int cy0,int cy1){
  if(!s->px||amt<=0) return;
  if(amt>255) amt=255;
  int y0=dy, y1=dy+s->h;
  if(y0<cy0) y0=cy0;
  if(y1>cy1) y1=cy1;
  if(y0<clip_y0) y0=clip_y0;
  if(y1>clip_y1) y1=clip_y1;
  if(y0<0) y0=0;
  if(y1>FBH) y1=FBH;
  for(int y=y0;y<y1;y++){
    int sy=y-dy;
    int xa=s->rx0?s->rx0[sy]:0, xb=s->rx1?s->rx1[sy]:s->w-1;
    if(dx+xa<0) xa=-dx;
    if(dx+xb>=FBW) xb=FBW-1-dx;
    const uint8_t*row=s->px+(size_t)sy*s->w*4;
    uint32_t*dst=fb+(size_t)y*FBW+dx;
    for(int x=xa;x<=xb;x++){
      int a=(row[x*4+3]*amt)>>8;
      if(!a) continue;
      uint32_t d=dst[x];
      int r=((d>>16)&255)+((row[x*4]*a)>>8);
      int g=((d>>8)&255)+((row[x*4+1]*a)>>8);
      int b=(d&255)+((row[x*4+2]*a)>>8);
      dst[x]=RGB(r>255?255:r,g>255?255:g,b>255?255:b);
    }
  }
}

/* a thin ring of points, for shock waves */
static void hold_ring(int cx,int cy,float r,uint32_t col,int a,int cy0,int cy1){
  if(a<=0||r<1) return;
  int n=(int)(r*TAU*0.9f); if(n>1400) n=1400;
  for(int i=0;i<n;i++){
    float t=TAU*i/n;
    int x=cx+(int)(cosf(t)*r), y=cy+(int)(sinf(t)*r);
    if(y<cy0||y>=cy1) continue;
    /* each point also lights the pixel below it, so the band above this
       one's first row still owes that row a pixel: take y = clip_y0-1
       and let fb_blend's own clip keep what belongs to this band      */
    if(y<clip_y0-1||y>=clip_y1) continue;
    fb_blend(x,y,col,a); fb_blend(x+1,y,col,a/2); fb_blend(x,y+1,col,a/2);
  }
}

/* === THE SHOW: UPDATE =========================================== */
static inline int hold_cx(int c){ return GX+(c/NROW)*CW+CW/2; }
static inline int hold_cy(int c){ return HGY+(c%NROW)*HCH+HCH/2; }
static inline int hold_rd(int k){ return (k%NREEL)*NROW + k/NREEL; }  /* reading order */
#define HMETCX (METX+METW/2)
#define HMETCY (METY2+METH2/2)

static void sfx_hold_slam(int n){
  float f=523.0f*powf(1.0595f,(float)(n-6));
  if(f>2400.0f) f=2400.0f;
  snd_noise(0.18f,0.22f,2400);
  snd(170,48,0.32f,2,0.20f);
  snd_at(0.02f,f,f,0.34f,1,0.17f);
  snd_at(0.02f,f*1.5f,f*1.5f,0.30f,0,0.05f);
  snd_at(0.10f,f*2.0f,f*2.0f,0.28f,1,0.07f);
}
static void sfx_hold_relight(void){
  static const float n[3]={784,988,1175};
  for(int i=0;i<3;i++) snd_at(i*0.07f,n[i],n[i],0.16f,1,0.14f);
  snd_chord(0.22f,1568,1976,2349,0.40f,0.10f);
}

static void hold_spin_start(void){
  hold_state_t*H=&G.hold;
  hold_roll();
  /* the little reels stop column by column, top to bottom, like reels.
     With four or fewer cells left, each one is held back for a slow,
     tense stop of its own. */
  int list[NCELL], e=0;
  for(int c=0;c<NCELL;c++){ H->spinning[c]=0; if(!hold_has(c)) list[e++]=c; }
  H->antic = (e<=4);
  H->lastStop=0;
  for(int i=0;i<e;i++){
    int c=list[i];
    H->stopAt[c] = H->antic ? 0.55f+i*0.80f : 0.42f+i*0.062f;
    H->spinning[c]=1;
    if(H->stopAt[c]>H->lastStop) H->lastStop=H->stopAt[c];
  }
  H->relit=0;
  H->phase=HP_SPIN; H->pt=0;
  snd_noise(0.30f,0.07f,1400);
  snd(260,150,0.16f,1,0.07f);                       /* a lamp goes out */
}

static void hold_collect_next(void);
static void hold_collect_start(void){
  hold_state_t*H=&G.hold;
  H->phase=HP_COLLECT; H->pt=0;
  H->colK=0; H->cur=-1; H->colT=0; H->colGrand=0;
  memset(H->colDone,0,sizeof H->colDone);
  H->shown=0; H->target=0;
  hold_collect_next();
}
static void hold_collect_next(void){
  hold_state_t*H=&G.hold;
  while(H->colK<NCELL && !hold_has(hold_rd(H->colK))) H->colK++;
  H->colT=0;
  if(H->colK<NCELL){
    H->cur=hold_rd(H->colK);
    H->colDur = H->blab[H->cur] ? 1.30f : (H->locked>14 ? 0.24f : 0.32f);
    return;
  }
  H->cur=-1;
  if(H->grand && H->colGrand==0){ H->colGrand=1; H->colDur=2.2f; return; }
  /* everything is in: the slam */
  H->phase=HP_SLAM; H->pt=0; H->shown=H->target=H->total;
  fx_fountain(GX+GW*0.5f,HGY+230.0f,1.8f);
  fx_shake(9.0f,0.45f);
  fx_burst(GX+GW*0.5f,HGY+230.0f,40,FXK_COIN);
  G.flash = opt_limiter?0.28f:0.6f;
  snd_noise(0.35f,0.20f,2600);
  snd_chord(0.00f,523,659,784,0.80f,0.20f);
  snd_chord(0.28f,1046,1318,1568,1.30f,0.22f);
  snd_at(0.28f,131,131,1.40f,2,0.12f);
}

static void hold_begin(void){
  hold_state_t*H=&G.hold;
  if(dbg_force==7){ const char*e=getenv("WILD7_HOLD"); hold_dbg=(e && !strcmp(e,"grand")); }
  hold_build_backdrop();
  hold_setup();
  H->phase=HP_TRIGGER; H->pt=0; H->tt=0;
  H->lampT=9.0f; H->relit=0; H->lampsLit=0; H->antic=0;
  H->trigK=0; H->beat=-1; H->cur=-1;
  for(int c=0;c<NCELL;c++){ H->lockT[c]=9.0f; H->spinning[c]=0; }
  H->shown=0; H->target=0;
  for(int c=0;c<NCELL;c++) H->target+=H->bval[c];
  G.state=ST_HOLD; G.t=0;
}

static void hold_update(void){
  hold_state_t*H=&G.hold;
  hold_build_backdrop();
  H->pt+=DT; H->tt+=DT;
  if(H->lampT<9.0f) H->lampT+=DT;
  for(int c=0;c<NCELL;c++) if(H->lockT[c]<9.0f) H->lockT[c]+=DT;
  if(H->shown<H->target){
    long long d=(H->target-H->shown)/7; if(d<1) d=1;
    H->shown+=d; if(H->shown>H->target) H->shown=H->target;
  }
  /* the heartbeat: with twenty coins down the whole board throbs */
  if(H->locked>=20 && H->locked<NCELL && (H->phase==HP_SPIN||H->phase==HP_READY)){
    int b=(int)(H->tt/0.86f);
    if(b!=H->beat){ H->beat=b; snd(72,40,0.16f,1,0.24f); snd_at(0.17f,64,36,0.16f,1,0.19f); }
  }

  switch(H->phase){
  case HP_TRIGGER: {
    /* every triggering coin rings out, in reading order */
    int upto=(int)(H->pt/0.07f);
    for(;H->trigK<NCELL && H->trigK<=upto;H->trigK++){
      int c=hold_rd(H->trigK);
      if(!hold_has(c)) continue;
      int n=0; for(int k=0;k<=H->trigK;k++) if(hold_has(hold_rd(k))) n++;
      float f=660.0f*powf(1.0595f,(float)(n*2));
      snd(f,f,0.20f,1,0.12f);
      fx_burst((float)cellcx(c/NROW),(float)cellcy(c%NROW),10,FXK_SPARK);
    }
    if(H->pt>=1.25f){
      fx_transition("HOLD & SPIN","COINS LOCK  -  3 RESPINS  -  EVERY NEW COIN RESETS THEM",0xFFD24A);
      snd_noise(0.40f,0.20f,1800);
      snd(98,49,0.60f,2,0.20f);
      snd_chord(0.05f,392,494,587,0.70f,0.18f);
      H->phase=HP_INTRO; H->pt=0;
    }
    break; }
  case HP_INTRO:
    if((H->pt>0.15f && !fx_transition_busy()) || H->pt>5.0f){
      H->phase=HP_READY; H->pt=0; H->lampsLit=0;
    }
    break;
  case HP_READY:
    if(H->pt>=0.50f && H->pt-DT<0.50f){ snd(120,50,0.25f,2,0.20f); snd_noise(0.12f,0.14f,1600); fx_shake(4.0f,0.2f); }
    while(H->lampsLit<HOLD_SPINS && H->pt>=0.75f+H->lampsLit*0.24f){
      H->lampsLit++;
      float f=784.0f*powf(1.26f,(float)(H->lampsLit-1));
      snd(f,f,0.22f,1,0.15f); snd(f*2,f*2,0.18f,0,0.04f);
      fx_burst((float)LAMPX(H->lampsLit-1),(float)LAMPY,8,FXK_SPARK);
    }
    if(H->pt>=1.75f) hold_spin_start();
    break;
  case HP_SPIN: {
    for(int c=0;c<NCELL;c++){
      if(!H->spinning[c] || H->pt<H->stopAt[c]) continue;
      H->spinning[c]=0;
      if(H->land[c]){
        hold_lock(c,H->nval[c],H->nlab[c]); H->land[c]=0;
        H->lockT[c]=0;
        H->target+=H->bval[c];
        float x=(float)hold_cx(c), y=(float)hold_cy(c);
        fx_burst(x,y,H->blab[c]?30:18,FXK_COIN);
        fx_burst(x,y,10,FXK_STAR);
        fx_shake(H->blab[c]?10.0f:6.0f,0.28f);
        sfx_hold_slam(H->locked);
        if(H->blab[c]) sfx_jackpot(H->blab[c]==HL_MAJOR?JP_MAJOR:JP_MINOR);
        if(!H->relit){ H->relit=1; H->lampT=0; sfx_hold_relight(); }
        if(H->locked>=20) G.flash = opt_limiter?0.18f:0.35f;
      } else {
        snd(190,120,0.05f,2,0.06f);
        if(H->antic){ snd(300,240,0.25f,1,0.06f); }
      }
      /* the next slow stop winds up */
      if(H->antic){
        float nx=99; int any=0;
        for(int k=0;k<NCELL;k++) if(H->spinning[k] && H->stopAt[k]<nx){ nx=H->stopAt[k]; any=1; }
        if(any) snd(220,520,nx-H->pt,1,0.07f);
      }
    }
    if(H->pt>=H->lastStop+0.55f){
      if(hold_after_spin()){
        hold_settle();
        if(H->grand){
          H->phase=HP_GRAND; H->pt=0;
          fx_transition("GRAND","ALL 25 COINS  -  THE MEGA POT IS YOURS",0xFF5A3A);
          sfx_jackpot(JP_MEGA);
          fx_fountain(GX+GW*0.5f,HGY+230.0f,2.5f);
          fx_shake(12.0f,0.6f);
          G.flash = opt_limiter?0.35f:0.8f;
        } else hold_collect_start();
      } else hold_spin_start();
    }
    break; }
  case HP_GRAND:
    if(H->pt>3.6f && !fx_transition_busy()) hold_collect_start();
    break;
  case HP_COLLECT:
    H->colT+=DT;
    if(H->cur>=0 && !H->colDone[H->cur] && H->colT>=0.24f){
      int c=H->cur;
      H->colDone[c]=1;
      H->target+=H->bval[c];
      int n=0; for(int k=0;k<NCELL;k++) n+=H->colDone[k];
      float f=880.0f*powf(1.0595f,(float)n);
      if(f>3000.0f) f=3000.0f;
      snd(f,f*1.5f,0.08f,0,0.07f); snd(f,f,0.14f,1,0.10f);
      fx_burst((float)HMETCX,(float)HMETCY,6,FXK_SPARK);
      if(H->blab[c]){
        sfx_jackpot(H->blab[c]==HL_MAJOR?JP_MAJOR:JP_MINOR);
        fx_burst((float)HMETCX,(float)HMETCY,26,FXK_COIN);
        G.flash = opt_limiter?0.25f:0.5f;
      }
    }
    if(H->colGrand==1 && H->colT>=0.30f){
      H->colGrand=2;
      H->target+=H->grandAmt;
      sfx_jackpot(JP_MEGA);
      fx_burst((float)HMETCX,(float)HMETCY,40,FXK_COIN);
      fx_shake(8.0f,0.4f);
      G.flash = opt_limiter?0.3f:0.6f;
    }
    if(H->colT>=H->colDur){
      if(H->cur>=0) H->colK++;
      hold_collect_next();
    }
    break;
  case HP_SLAM:
    if(((int)(H->pt*60))%9==0 && H->pt<1.6f)
      fx_burst(GX+60.0f+frnd()*(GW-120.0f),HGY+120.0f+frnd()*200.0f,8,FXK_COIN);
    if(H->pt>3.8f || (H->pt>1.3f && anyhit())){
      long long t=H->total;
      H->phase=HP_TRIGGER; H->pt=0;
      award(t);
      feature_done();
    }
    break;
  }
}

/* === THE SHOW: DRAW =============================================
 *  Pure functions of G.hold and the baked art: nothing here writes
 *  state or draws a random number.                                   */
static uint32_t hold_hash(uint32_t a,uint32_t b){
  uint32_t h=a*0x9E3779B1u ^ (b+0x7F4A7C15u)*0x85EBCA77u;
  h^=h>>15; h*=0x2C1B3C6Du; h^=h>>12;
  return h;
}

/* value or label on a coin whose cartouche is centred on (cx, by) */
static void hold_coin_text(long long v,int lab,int cx,int by,int maxw,int cy0,int cy1){
  if(lab==HL_MINOR){ htext("MINOR",cx,by-7,HF_I2,1,cy0,cy1); return; }
  if(lab==HL_MAJOR){ htext("MAJOR",cx,by-7,HF_G2,1,cy0,cy1); return; }
  char b[16]; hold_fmt(v,b,sizeof b);
  static const int F[3]={HF_G4,HF_G3,HF_G2};
  htext_fit(b,cx,by,maxw,F,3,cy0,cy1);
}

/* -- coins on the base reels -------------------------------------- */
static void hold_cells_base(int hot){
  if(G.state==ST_ATTRACT) return;
  const hold_state_t*H=&G.hold;
  for(int r=0;r<NREEL;r++){
    if(G.reelBlur[r]>0.35f) continue;           /* draw_reels shows the smear */
    /* walk the strip exactly as draw_reels does, so a settling coin is
       labelled where it is actually drawn */
    int base=(int)floorf(G.rpos[r]);
    float frac=G.rpos[r]-base;
    int still = (G.rstate[r]==0||G.rstate[r]==3) && frac==0.0f;
    for(int j=0;j<NROW+1;j++){
      if(stripAt(r,base-j+1)!=SY_COIN) continue;
      int row=j-1;
      int sx=GX+r*CW+SOX, sy=GY+row*CH+(int)(frac*CH)+SOY, cx=sx+SYMW/2, by=sy+54;
      if(sy+SYMH<=GY || sy>=GY+GH) continue;
      int c = row>=0 ? r*NROW+row : r*NROW;
      if(!still || row<0 || G.state==ST_SPIN || G.grid[r][row]!=SY_COIN){
        /* landed, not yet valued: the value arrives when every reel is down */
        float pl=0.5f+0.5f*sinf(G.t*9.0f+c);
        if(still) hring_add(&bring,cx,sy+SYMH/2,(int)(60+60*pl),GY,GY+GH);
        htext("?",cx,by-10,HF_G3,1,GY,GY+GH);
        continue;
      }
      int lab=H->lab[c];
      long long v=H->val[c];
      if(hot || G.coinCount>=HOLD_NEED){
        /* a trigger: the coins burn */
        float pl=0.5f+0.5f*sinf(G.t*8.0f+c*0.7f);
        int amt=(int)(120+110*pl);
        if(G.state==ST_HOLD){
          float since=H->pt-(float)((c%NROW)*NREEL+c/NROW)*0.07f;
          if(since>=0 && since<0.4f) amt=255;
          lab=H->blab[c]; v=H->bval[c];
        }
        hring_add(&bring,cx,sy+SYMH/2,amt,GY,GY+GH);
      }
      if(lab) blit(&bcoin[lab],sx,sy,GY,GY+GH,255,0,0.0f);
      hold_coin_text(v,lab,cx,by,74,GY,GY+GH);
    }
  }
}
static void hold_draw_cells(void){ hold_cells_base(0); }

/* -- the board ---------------------------------------------------- */
static void hold_paint_window(int dark){
  int y0=GY>clip_y0?GY:clip_y0, y1=(GY+GH)<clip_y1?(GY+GH):clip_y1;
  if(!hbk){ for(int y=y0;y<y1;y++) for(int x=GX;x<GX+GW;x++) fb_px(x,y,0x100204); return; }
  for(int y=y0;y<y1;y++){
    const uint32_t*src = (dark && hbkD && y>=HGY) ? hbkD : hbk;   /* the header stays lit */
    memcpy(fb+(size_t)y*FBW+GX,src+(size_t)(y-GY)*GW,(size_t)GW*4);
  }
}

/* heartbeat envelope: lub-dub every 0.86 s */
static float hold_heart(float t){
  float p=fmodf(t,0.86f);
  float a=(p-0.04f)/0.045f, b=(p-0.21f)/0.05f;
  return expf(-a*a)+0.7f*expf(-b*b);
}

/* how far a turning cell's strip has run: full speed, then a linear
   run-down over the last 0.28 s to its stop */
static float hold_scroll(float t,float stop){
  const float V=1150.0f, D=0.28f;
  float td=stop-D;
  if(t<=td) return V*t;
  if(t>stop) t=stop;
  float u=t-td;
  return V*td + V*u - V*u*u/(2.0f*D);
}

static void hold_draw_coin(int c,int x,int y,int cy0,int cy1){
  const hold_state_t*H=&G.hold;
  int lab=H->blab[c];
  const spr_t*s=&hcoin[lab];
  float lt=H->lockT[c];
  int collected = (H->phase==HP_COLLECT||H->phase==HP_SLAM) && H->colDone[c] && H->cur!=c;
  int current   = (H->phase==HP_COLLECT && H->cur==c);
  /* rim glow, breathing, each coin on its own phase */
  float pl=0.5f+0.5f*sinf(H->tt*3.4f+c*1.3f);
  if(opt_limiter) pl=0.3f+pl*0.5f;
  int amt = (int)(110+120*pl);
  if(current) amt=255;
  if(H->phase==HP_GRAND) amt=(int)(170+85*pl);
  if(!collected) hring_add(&hring[lab],x,y,amt,cy0,cy1);
  /* a new coin drops into its socket with a bounce */
  int dy=0;
  if(lt<0.20f){ float u=lt/0.20f; dy=(int)(-(1.0f-u)*(1.0f-u)*64.0f); }
  else if(lt<0.34f){ float u=(lt-0.20f)/0.14f; dy=(int)(sinf(u*3.14159f)*-7.0f); }
  int sx=x-53, sy=y-53+dy;
  blit(collected?&hcoinD[lab]:s,sx,sy,cy0,cy1,255,0,0.0f);
  if(lt<0.30f) blit_wash(s,sx,sy,cy0,cy1,0xFFFFFF,(0.30f-lt)/0.30f*0.85f);
  if(current && H->colT<0.24f) blit_wash(s,sx,sy,cy0,cy1,0xFFFFFF,(0.24f-H->colT)/0.24f*0.6f);
  hold_coin_text(H->bval[c],lab,x,sy+53,60,cy0,cy1);
  /* the slam: a flash of light and a shock ring */
  if(lt<0.55f){
    float u=lt/0.55f;
    hblit_add(&glowspr,x-glowspr.w/2,y-glowspr.h/2,(int)((1.0f-u)*255),cy0,cy1);
    hold_ring(x,y,44.0f+u*150.0f,lab==HL_MINOR?0x9AE0FF:0xFFE08A,(int)((1.0f-u)*230),GY,GY+GH);
    hold_ring(x,y,40.0f+u*90.0f,0xFFFFFF,(int)((1.0f-u)*150),GY,GY+GH);
  }
}

static void hold_draw_spin(int c,int x0,int y0){
  const hold_state_t*H=&G.hold;
  int sx=x0+(CW-HSPW)/2, sy=y0+(HCH-HSPH)/2;
  int cy0=sy+2, cy1=sy+HSPH-2;
  blit(&hspin,sx,sy,GY,GY+GH,255,0,0.0f);
  float off=hold_scroll(H->pt,H->stopAt[c]) + (float)(hold_hash(c,7)%HCH);
  int slot=(int)(off/HCH);
  float fr=off-slot*HCH;
  for(int k=-1;k<=1;k++){
    uint32_t hh=hold_hash((uint32_t)c,(uint32_t)(slot-k));
    if(hh%3u) continue;                       /* blanks between the coins */
    int yc=y0+HCH/2+(int)fr+k*HCH;
    blit(&hghost,x0+CW/2-53,yc-53,cy0,cy1,255,0,0.0f);
  }
  /* shimmer: a soft band of light sliding down the glass */
  int band=(int)fmodf(H->tt*520.0f+c*37.0f,(float)(HCH+60))-30;
  for(int j=-10;j<=10;j++){
    int y=y0+band+j;
    if(y<cy0||y>=cy1) continue;
    int a=(10-abs(j))*3;
    for(int i=12;i<CW-12;i++) fb_blend(x0+i,y,0xFFD890,a);
  }
  /* the anticipation: a nearly-full board makes each last cell burn */
  if(H->antic){
    float left=H->stopAt[c]-H->pt;
    float nx=99;
    for(int k=0;k<NCELL;k++) if(H->spinning[k] && H->stopAt[k]<nx) nx=H->stopAt[k];
    int next=(H->stopAt[c]<=nx+0.001f);
    float sp = next ? 16.0f : 5.0f;
    float pl=0.5f+0.5f*sinf(H->tt*sp);
    if(opt_limiter) pl=0.4f+pl*0.4f;
    uint32_t col = next ? 0xFFE060 : 0xC89030;
    fb_rframe(x0+4,y0+2,CW-8,HCH-4,15,next?3.5f:2.0f,col,(int)((next?150:90)+100*pl));
    if(next && left<0.8f)
      hring_add(&hring[0],x0+CW/2,y0+HCH/2,(int)(80+140*pl),cy0-6,cy1+6);
  }
}

static void hold_draw_header(void){
  const hold_state_t*H=&G.hold;
  /* the lamps */
  int lit=H->respins;
  if(H->phase==HP_READY) lit=H->lampsLit;
  else if(H->phase==HP_SPIN && H->relit) lit=HOLD_SPINS;
  else if(H->phase>=HP_GRAND) lit=0;
  if(lit<0) lit=0;
  float fl = H->lampT<0.7f ? 1.0f-H->lampT/0.7f : 0.0f;
  for(int i=0;i<HOLD_SPINS;i++){
    int x=LAMPX(i), y=LAMPY, on=(i<lit);
    if(on){
      float pl=0.75f+0.25f*sinf(H->tt*5.0f+i*1.1f);
      hblit_add(&hhalo,x-hhalo.w/2,y-hhalo.h/2,(int)(150*pl+100*fl),GY,HGY);
    }
    blit(&hlamp[on],x-53,y-53,GY,HGY,255,0,0.0f);
    if(on && fl>0){
      blit_wash(&hlamp[1],x-53,y-53,GY,HGY,0xFFFFFF,fl*0.8f);
      hold_ring(x,y,22.0f+(1.0f-fl)*40.0f,0xFFE8A0,(int)(fl*230),GY,HGY);
    }
  }
  /* title and status */
  htext("HOLD & SPIN",GX+GW/2,GY+12,HF_G3,1,GY,HGY);
  /* two lines under the title, at most 19 characters of the px-2 type
     fit between the lamps and the meter */
  char b[48]; const char*hint="FILL ALL 25 FOR THE GRAND";
  int hot=0;
  if(H->phase==HP_COLLECT){
    snprintf(b,sizeof b,"%s",H->colGrand?"+ THE MEGA POT":"COLLECTING");
    hint="EVERY COIN PAYS";
  } else if(H->phase==HP_SLAM){ snprintf(b,sizeof b,"%d COINS",H->locked); hint=""; }
  else if(H->phase==HP_GRAND){ snprintf(b,sizeof b,"ALL 25 COINS"); hint="THE MEGA POT IS YOURS"; }
  else {
    snprintf(b,sizeof b,"%d OF 25 COINS",H->locked);
    if(H->locked>=HOLD_HOTN){ hint="GRAND IN REACH"; hot=1; }
  }
  text(b,GX+GW/2,GY+40,2,0xFFE9C8,1,1);
  if(hot){
    float hb=hold_heart(H->tt); if(opt_limiter) hb*=0.6f;
    text(hint,GX+GW/2,GY+59,2,mixc(0xFF4A3A,0xFFFFFF,clampf(hb,0,1)),1,1);
  } else if(hint[0]) text(hint,GX+GW/2,GY+62,1,H->phase==HP_GRAND?0xFFD24A:0xC89A6A,1,1);
  /* the meter */
  int win=(H->phase==HP_COLLECT||H->phase==HP_SLAM);
  text(win?"WIN":"TOTAL",METX+10,METY2+6,1,win?0xFFE9A8:0xFFB08A,0,1);
  uint32_t on=win?mixc(0xFFB020,0xFFFFFF,0.25f+0.25f*sinf(H->tt*9.0f)):0xFF8A30;
  seg_num(H->shown,METX+METW-10,METY2+19,9,13,32,on,0x4A2406,1);
}

/* the collect flight: a comet from the coin to the meter */
static void hold_draw_flight(void){
  const hold_state_t*H=&G.hold;
  if(H->phase!=HP_COLLECT) return;
  float x0,y0;
  if(H->cur>=0){ x0=(float)hold_cx(H->cur); y0=(float)hold_cy(H->cur); }
  else if(H->colGrand){ x0=GX+GW*0.5f; y0=HGY+230.0f; }
  else return;
  for(int k=4;k>=0;k--){
    float u=(H->colT-k*0.025f)/0.24f;
    if(u<0||u>1) continue;
    float e=u*u*(3-2*u);
    float x=lerpf(x0,(float)HMETCX,e), y=lerpf(y0,(float)HMETCY,e)-sinf(u*3.14159f)*70.0f;
    int amt=255-k*45;
    if(k==0) hblit_add(&glowspr,(int)x-glowspr.w/2,(int)y-glowspr.h/2,190,GY,GY+GH);
    hblit_add(&hspark,(int)x-hspark.w/2,(int)y-hspark.h/2,amt,GY,GY+GH);
    if(k==0) hblit_add(&hspark,(int)x-hspark.w/2,(int)y-hspark.h/2,amt,GY,GY+GH);
  }
  /* the value just banked pops under the meter */
  long long v=-1; float u=1;
  if(H->cur>=0 && H->colDone[H->cur]){ v=H->bval[H->cur]; u=(H->colT-0.24f)/0.5f; }
  else if(H->colGrand==2){ v=H->grandAmt; u=(H->colT-0.30f)/1.2f; }
  if(v>=0 && u<1){
    char b[24], t[16]; hold_fmt(v,t,sizeof t);
    snprintf(b,sizeof b,"+%s",t);
    htext(b,HMETCX,METY2+METH2+3+(int)(u*8),HF_G3,1,GY,GY+GH);
  }
}

/* each channel to 3/8, straight on the rows this band may touch */
static void hold_darken(int x,int y,int w,int h){
  int y0=y, y1=y+h;
  if(y0<clip_y0) y0=clip_y0;
  if(y1>clip_y1) y1=clip_y1;
  if(y0<0) y0=0;
  if(y1>FBH) y1=FBH;
  if(x<0){ w+=x; x=0; }
  if(x+w>FBW) w=FBW-x;
  for(int j=y0;j<y1;j++){
    uint32_t*p=fb+(size_t)j*FBW+x;
    for(int i=0;i<w;i++){ uint32_t d=p[i]; p[i]=((d>>2)&0x3F3F3Fu)+((d>>3)&0x1F1F1Fu); }
  }
}

/* additive wedge rays that widen with distance, kept inside the window */
static void hold_rays(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,int alpha){
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  int ya=HGY>clip_y0?HGY:clip_y0, yb=(GY+GH)<clip_y1?(GY+GH):clip_y1;
  if(yb<=ya) return;
  for(int i=0;i<n;i++){
    float a=ang+i*(TAU/n), ca=cosf(a), sa=sinf(a);
    for(int d=r0;d<r1;d++){
      int f=(r1-d)*alpha/(r1-r0);                  /* 0..alpha, fading out */
      int hw=2+d*3/40;
      float bx=cx+ca*d, by=cy+sa*d;
      for(int w=-hw;w<=hw;w++){
        int x=(int)(bx-sa*w), y=(int)(by+ca*w);
        if(y<ya||y>=yb||x<GX||x>=GX+GW) continue;
        int e=f*(hw+1-abs(w))/(hw+1);              /* soft edges */
        uint32_t*p=fb+(size_t)y*FBW+x, dd=*p;
        int r=((dd>>16)&255)+((cr*e)>>8), g=((dd>>8)&255)+((cg*e)>>8), b=(dd&255)+((cb*e)>>8);
        *p=RGB(r>255?255:r,g>255?255:g,b>255?255:b);
      }
    }
  }
}

static void hold_draw_slam(void){
  const hold_state_t*H=&G.hold;
  if(H->phase!=HP_SLAM) return;
  int cx=GX+GW/2, cy=HGY+224;
  float pl=0.5f+0.5f*sinf(H->tt*6.0f);
  if(opt_limiter) pl=0.3f+pl*0.4f;
  hold_rays(cx,cy,40,330,18,H->tt*0.45f,0xFFB030,(int)(120+70*pl));
  /* the band */
  int bh=220, by=cy-bh/2;
  hold_darken(GX,by,GW,bh);
  for(int j=0;j<3;j++){
    uint32_t rc=mixc(0xF0C24A,0xFFFFFF,pl*0.5f);
    fb_rect(GX,by+j,GW,1,j==1?rc:0x8A5A10,255);
    fb_rect(GX,by+bh-1-j,GW,1,j==1?rc:0x8A5A10,255);
  }
  htext("HOLD & SPIN WIN",cx,by+16,HF_G3,1,GY,GY+GH);
  char b[32]; commas(b,sizeof b,H->total);
  float u=clampf(H->pt/0.32f,0,1);
  int dy=(int)((1.0f-easeOutBack(u))*-160.0f);
  int f = htext_w(b,HF_G9)<=GW-60 ? HF_G9 : HF_G4;
  int ty = f==HF_G9 ? by+58 : by+84;
  hblit_add(&glowspr,cx-glowspr.w/2,by+bh/2-glowspr.h/2,(int)(110+90*pl),by+3,by+bh-3);
  htext(b,cx,ty+dy,f,1,by+3,by+bh-3);
  if(H->pt<0.50f){
    float v=1.0f-H->pt/0.50f;
    int w2=htext_w(b,f)/2;
    hblit_add(&glowspr,cx-glowspr.w/2-w2,by+bh/2-glowspr.h/2,(int)(v*255),GY,GY+GH);
    hblit_add(&glowspr,cx-glowspr.w/2+w2,by+bh/2-glowspr.h/2,(int)(v*255),GY,GY+GH);
    hold_ring(cx,by+bh/2,60.0f+(1.0f-v)*300.0f,0xFFE08A,(int)(v*230),GY,GY+GH);
    hold_ring(cx,by+bh/2,40.0f+(1.0f-v)*180.0f,0xFFFFFF,(int)(v*160),GY,GY+GH);
  }
  /* twinkles scattered round the band, a hash of their index and time */
  for(int i=0;i<14;i++){
    uint32_t h=hold_hash((uint32_t)i,(uint32_t)(H->tt*3.0f));
    float tw=0.5f+0.5f*sinf(H->tt*9.0f+i*1.7f);
    int x=GX+20+(int)(h%(GW-40)), y=HGY+16+(int)((h>>12)%(GH-HHEAD-32));
    if(y>by-6 && y<by+bh+6) continue;
    int r=2+(int)(tw*5), a=(int)(tw*230);
    for(int k=-r;k<=r;k++){ fb_blend(x+k,y,0xFFF4D0,a); fb_blend(x,y+k,0xFFF4D0,a); }
  }
  char m[48];
  long long bet=TOTBET;
  long long x10=bet>0 ? H->total*10/bet : 0;
  if(x10%10) snprintf(m,sizeof m,"%lld.%lld X BET",x10/10,x10%10);
  else       snprintf(m,sizeof m,"%lld X BET",x10/10);
  text(m,cx,by+bh-50,3,0xFFE9C8,1,1);
  if(H->pt>1.3f && (((int)(H->pt*2.2f))&1))
    text("PRESS START TO COLLECT",cx,by+bh+14,2,0xFFFFFF,1,1);
}

/* a diagonal sweep of light across the board */
static void hold_shine(float t,int alpha){
  int ya=HGY>clip_y0?HGY:clip_y0, yb=(GY+GH)<clip_y1?(GY+GH):clip_y1;
  float head=fmodf(t*760.0f,(float)(GW+700))-350.0f;
  for(int y=ya;y<yb;y++){
    int xc=GX+(int)(head+(y-HGY)*0.55f);
    for(int k=-34;k<=34;k++){
      int x=xc+k;
      if(x<GX||x>=GX+GW) continue;
      int a=alpha*(35-abs(k))/35;
      uint32_t*p=fb+(size_t)y*FBW+x, d=*p;
      int r=((d>>16)&255)+(a*255>>8), g=((d>>8)&255)+(a*230>>8), b=(d&255)+(a*160>>8);
      *p=RGB(r>255?255:r,g>255?255:g,b>255?255:b);
    }
  }
}

/* GRAND: after the title card, and again while the MEGA pot is counted */
static void hold_draw_grand(void){
  const hold_state_t*H=&G.hold;
  int show = (H->phase==HP_GRAND && !fx_transition_busy()) ||
             (H->phase==HP_COLLECT && H->colGrand);
  if(H->phase==HP_GRAND) hold_shine(H->pt,70);
  if(!show) return;
  int cx=GX+GW/2, cy=HGY+224, bh=190, by=cy-bh/2;
  float pl=0.5f+0.5f*sinf(H->tt*7.0f);
  if(opt_limiter) pl=0.3f+pl*0.4f;
  hold_darken(GX,by,GW,bh);
  hold_rays(cx,cy,30,250,14,-H->tt*0.6f,0xFF5A2A,(int)(60+50*pl));
  for(int j=0;j<3;j++){
    uint32_t rc=mixc(0xFF5A3A,0xFFFFFF,pl*0.6f);
    fb_rect(GX,by+j,GW,1,j==1?rc:0x8A1A10,255);
    fb_rect(GX,by+bh-1-j,GW,1,j==1?rc:0x8A1A10,255);
  }
  hblit_add(&glowspr,cx-glowspr.w/2,cy-40-glowspr.h/2,(int)(120+100*pl),by+3,by+bh-3);
  htext("GRAND",cx,by+22,HF_G9,1,by+3,by+bh-3);
  char b[48], v[32]; commas(v,sizeof v,H->grandAmt);
  snprintf(b,sizeof b,"MEGA POT  %s",v);
  htext(b,cx,by+bh-50,HF_G3,1,by+3,by+bh-3);
}

/* half brightness over one reel cell, for the trigger's spotlight */
static void hold_halve(int x,int y,int w,int h){
  int y0=y>clip_y0?y:clip_y0, y1=(y+h)<clip_y1?(y+h):clip_y1;
  for(int j=y0;j<y1;j++){
    uint32_t*p=fb+(size_t)j*FBW+x;
    for(int i=0;i<w;i++) p[i]=(p[i]>>1)&0x7F7F7Fu;
  }
}

static void hold_draw(void){
  const hold_state_t*H=&G.hold;
  if(H->phase<=HP_INTRO){
    /* the trigger: every other cell falls into shadow and the coins burn */
    draw_reels();
    if(H->phase==HP_TRIGGER)          /* under the title card nobody sees it */
      for(int c=0;c<NCELL;c++)
        if(G.grid[c/NROW][c%NROW]!=SY_COIN) hold_halve(GX+(c/NROW)*CW+2,GY+(c%NROW)*CH,CW-4,CH);
    hold_cells_base(1);
    return;
  }
  hold_paint_window(H->phase==HP_SLAM);

  /* heartbeat: the board's edge throbs red - straight bands, twelve
     pixels deep, fading inward (a stack of rounded frames cost ~1 ms) */
  if(H->locked>=HOLD_HOTN && H->locked<NCELL && (H->phase==HP_SPIN||H->phase==HP_READY)){
    float hb=hold_heart(H->tt); if(opt_limiter) hb*=0.6f;
    if(hb>0.02f){
      int y0=HGY>clip_y0?HGY:clip_y0, y1=(GY+GH)<clip_y1?(GY+GH):clip_y1;
      for(int k=0;k<12;k++){
        int a=(int)(hb*(210-k*17)); if(a<=0) break;
        int t=HGY+k, b=GY+GH-1-k;
        if(t>=y0 && t<y1) for(int i=k;i<GW-k;i++) fb_blend(GX+i,t,0xFF1A1A,a);
        if(b>=y0 && b<y1) for(int i=k;i<GW-k;i++) fb_blend(GX+i,b,0xFF1A1A,a);
        for(int y=(HGY+k>y0?HGY+k:y0);y<(GY+GH-k<y1?GY+GH-k:y1);y++){
          fb_blend(GX+k,y,0xFF1A1A,a); fb_blend(GX+GW-1-k,y,0xFF1A1A,a);
        }
      }
    }
  }

  float mk = H->phase==HP_READY ? easeOutBack(clampf(H->pt/0.50f,0,1)) : 1.0f;
  for(int c=0;c<NCELL;c++){
    int r=c/NROW, row=c%NROW;
    int x0=GX+r*CW, y0=HGY+row*HCH;
    if(hold_has(c)){
      int x=x0+CW/2, y=y0+HCH/2;
      if(mk<1.0f){                   /* coins sweep in from their reel cells */
        x=(int)lerpf((float)cellcx(r),(float)x,mk);
        y=(int)lerpf((float)cellcy(row),(float)y,mk);
      }
      hold_draw_coin(c,x,y,GY,GY+GH);
    } else if(H->phase==HP_SPIN && H->spinning[c]){
      hold_draw_spin(c,x0,y0);
    } else if(H->phase==HP_SPIN && H->pt-H->stopAt[c]<0.22f && H->pt>=H->stopAt[c]){
      float u=(H->pt-H->stopAt[c])/0.22f;
      fb_rframe(x0+6,y0+4,CW-12,HCH-8,14,2.0f,0xE8B050,(int)((1.0f-u)*200));
    }
  }
  hold_draw_grand();
  hold_draw_header();
  hold_draw_flight();
  hold_draw_slam();
}
