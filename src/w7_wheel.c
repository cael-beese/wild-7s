/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_wheel.c - WHEEL OF 7's, the showpiece bonus.
 *
 *  Three WHEEL symbols, one on each of reels 2, 3 and 4, bring up a
 *  600-pixel wheel in the lounge's honeycomb room, between its glass
 *  and neon panels (pots, how it plays, bet, win, the status and the
 *  prize plate, all in the w7_lounge.c kit): 24 glossy wedges, gold
 *  dividers and pegs, a chrome and gold rim ringed with chasing bulbs,
 *  and a gold flapper at the top that the pegs knock aside one by one.
 *
 *  One spin, chosen at the moment it starts from a weighted table.  The
 *  weights are not the wedge sizes - that is how real wheels are built -
 *  but the wheel is then driven to stop inside the chosen wedge, so what
 *  the player sees under the pointer is always what is paid.
 *
 *  The SUPER wedge upgrades the feature: the face is swapped for the
 *  black-and-gold SUPER WHEEL, every wedge worth more and the MEGA
 *  jackpot on it, and the player spins again.  Pot wedges pay the live
 *  MINOR / MAJOR / MEGA progressive and reset it, as the reels do.
 *
 *  Rendering: both faces are painted once at init, per pixel in polar
 *  coordinates with analytic antialiasing (dividers, rings) and
 *  supersampled bubble lettering, plus two angularly blurred copies of
 *  each.  An angular blur is rotation invariant, so the blurred copy
 *  rotated is exactly the blurred rotation: motion blur at speed costs
 *  nothing per frame.  Per frame the face is inverse-mapped in 16.16
 *  fixed point, one span per row clipped to the disc and the band, with
 *  bilinear filtering when sharp; a static lighting map (rim shadow,
 *  hub shadow, glass glare) is applied in the same pass so the gloss
 *  stays put while the wheel turns under it.  Stage, rim and bulb
 *  sockets are one pre-rendered backdrop.
 * ===================================================================== */

/* ---- geometry ---- */
#define WH_CX     640
#define WH_CY     376
#define WH_RF     240                 /* visible radius of the face      */
#define WH_RS     250                 /* face sprite radius, with margin */
#define WH_FW     (WH_RS*2+2)         /* face sprite width               */
#define WH_FC     ((float)(WH_RS+1))  /* sprite centre, continuous       */
#define WH_RHUB   70                  /* static hub covers r < this      */
#define WH_RTRIM  80                  /* gold trim ring outside the hub  */
#define WH_RPEG   229
#define WH_RLIP   247                 /* chrome lip -> gold rim          */
#define WH_RRIM   300
#define WH_RBULB  273
#define WH_NBULB  32
#define WH_NW     24
#define WH_WDEG   (360.0f/WH_NW)
#define WH_PIVY   54                  /* pointer pivot, screen y         */
#define WH_GW     (WH_RF*2+3)         /* lighting map, square            */

/* ---- the lounge furniture either side: shared by the stage and the draw ---- */
#define WPL_X     18                  /* left column: title, pots, how it plays */
#define WPR_X     962                 /* right column: bet, win, wheels, status */
#define WCOL_W    300
#define WCX_L     (WPL_X+WCOL_W/2)
#define WCX_R     (WPR_X+WCOL_W/2)
#define WT_Y      18                  /* the title sign                  */
#define WT_H      142
#define WJ_Y      180                 /* JACKPOT WEDGES                  */
#define WJ_H      252
#define WJ_ROW(i) (WJ_Y+28+(i)*74)    /* a pot's caption; its well 22 below */
#define WHP_Y     452                 /* HOW IT PLAYS                    */
#define WHP_H     (FBH-18-WHP_Y)
#define WB_Y      18                  /* BET                             */
#define WB_H      96
#define WW_Y      132                 /* WHEEL WIN                       */
#define WW_H      112
#define WTAB_X    986                 /* the two wheels, the live one lit */
#define WTAB_W    252
#define WTAB_H    44
#define WTAB_Y(i) (266+(i)*58)
#define WTAB_M    30                  /* how far a lit tab's glow spills */
#define WS_Y      394                 /* the status: prompts and results */
#define WS_H      (FBH-18-WS_Y)
#define WPLATE_W  560                 /* the prize plate over the wheel  */
#define WPLATE_H  164
#define WPLATE_Y  478
#define WPLATE_M  26                  /* its glass sprite's glow margin  */

/* ---- the spin ---- */
#define WH_WIND    7.0f               /* wind-up, degrees backwards      */
#define WH_WINDT   0.45f
#define WH_CONTACT 5.0f               /* a peg this close bends the flapper */
#define WH_PMAX    22.0f              /* flapper deflection at full push */

enum { WPH_INTRO, WPH_READY, WPH_WIND, WPH_SPIN, WPH_LANDED,
       WPH_UPGRADE, WPH_PRIZE, WPH_DONE };
enum { WK_CR=0, WK_POT, WK_UP };

/* ---- wedge palettes: lo / mid / hi ----
 *  The lounge's jewel tones: ruby, indigo, green, plum, amber, cyan and
 *  magenta, so the wheel sits in the room's palette rather than beside it. */
enum { WP_RED, WP_BLUE, WP_GREEN, WP_PURPLE, WP_ORANGE, WP_TEAL, WP_MAGENTA,
       WP_CRIMSON, WP_ONYX, WP_WINE, WP_NAVY, WP_EMERALD,
       WP_MINOR, WP_MAJOR, WP_MEGA, WP_SUPER, WP_N };
static const uint32_t WH_PAL[WP_N][3] = {
  {0x3A0012,0xD01A48,0xFF8AA0}, {0x0A0A40,0x3242D0,0xA8B6FF},
  {0x022A14,0x16A052,0xA8F4B8}, {0x24082E,0x8A2AB8,0xE0A8FF},
  {0x401802,0xFF8010,0xFFD89A}, {0x02303A,0x10B4D0,0xA0F2FF},
  {0x40062E,0xE0229C,0xFFA8E0}, {0x200006,0x8A0A24,0xE02A4A},
  {0x020204,0x18181F,0x585866}, {0x160006,0x5A0A24,0xB0405E},
  {0x02041A,0x0C1C64,0x4068C8}, {0x011A0C,0x0A5A30,0x40AE74},
  {0x283040,0xB8C4DA,0xFFFFFF}, {0x4A2E04,0xE8A820,0xFFF4C0},
  {0x000000,0x000000,0x000000}, {0x000000,0x0A0A10,0x2C2C38},
};

typedef struct { uint8_t kind, pal; int16_t val, wt; } wedge_t;

/*  The WHEEL OF 7's.  Clockwise from the top at rest angle 0.  Values
 *  are multiples of the total bet.  Weights are out of their sum.      */
static const wedge_t WH_A[WH_NW] = {
  {WK_UP ,WP_SUPER  ,0       ,60}, {WK_CR ,WP_BLUE   ,10 ,66},
  {WK_CR ,WP_GREEN  ,40      ,18}, {WK_CR ,WP_RED    ,8  ,80},
  {WK_CR ,WP_PURPLE ,20      ,30}, {WK_CR ,WP_ORANGE ,5  ,66},
  {WK_POT,WP_MINOR  ,JP_MINOR,36}, {WK_CR ,WP_TEAL   ,15 ,42},
  {WK_CR ,WP_CRIMSON,75      , 7}, {WK_CR ,WP_BLUE   ,8  ,80},
  {WK_CR ,WP_MAGENTA,25      ,32}, {WK_CR ,WP_GREEN  ,10 ,66},
  {WK_POT,WP_MAJOR  ,JP_MAJOR, 9}, {WK_CR ,WP_PURPLE ,5  ,66},
  {WK_CR ,WP_ORANGE ,30      ,24}, {WK_CR ,WP_TEAL   ,8  ,80},
  {WK_CR ,WP_RED    ,50      ,12}, {WK_CR ,WP_BLUE   ,15 ,42},
  {WK_CR ,WP_CRIMSON,250     , 2}, {WK_CR ,WP_GREEN  ,10 ,66},
  {WK_CR ,WP_MAGENTA,20      ,30}, {WK_CR ,WP_ORANGE ,5  ,66},
  {WK_CR ,WP_CRIMSON,100     , 4}, {WK_CR ,WP_PURPLE ,15 ,42},
};
/*  The SUPER WHEEL: black and gold, the MEGA on it.                    */
static const wedge_t WH_B[WH_NW] = {
  {WK_POT,WP_MEGA   ,JP_MEGA , 9}, {WK_CR ,WP_ONYX   ,30 ,55},
  {WK_CR ,WP_WINE   ,100     ,32}, {WK_CR ,WP_NAVY   ,25 ,70},
  {WK_CR ,WP_EMERALD,60      ,30}, {WK_CR ,WP_ONYX   ,40 ,45},
  {WK_POT,WP_MAJOR  ,JP_MAJOR,36}, {WK_CR ,WP_NAVY   ,30 ,55},
  {WK_CR ,WP_WINE   ,250     , 7}, {WK_CR ,WP_ONYX   ,25 ,70},
  {WK_CR ,WP_EMERALD,75      ,24}, {WK_CR ,WP_NAVY   ,50 ,42},
  {WK_CR ,WP_WINE   ,150     ,18}, {WK_CR ,WP_ONYX   ,25 ,70},
  {WK_CR ,WP_EMERALD,40      ,45}, {WK_CR ,WP_NAVY   ,30 ,55},
  {WK_CR ,WP_WINE   ,500     , 3}, {WK_CR ,WP_ONYX   ,25 ,70},
  {WK_CR ,WP_EMERALD,60      ,30}, {WK_CR ,WP_NAVY   ,40 ,45},
  {WK_CR ,WP_ONYX   ,50      ,42}, {WK_CR ,WP_EMERALD,30 ,55},
  {WK_CR ,WP_WINE   ,200     ,11}, {WK_CR ,WP_NAVY   ,75 ,24},
};

static const wedge_t* wh_table(int face){ return face ? WH_B : WH_A; }

/* ---- THE MATHS (shared by the game and the simulator) ---- */

/*  Which wedge a spin lands on.  Called once, when the spin starts.    */
static int wheel_pick(int face){
  const wedge_t*T=wh_table(face);
  int tot=0;
  for(int i=0;i<WH_NW;i++) tot+=T[i].wt;
  int r=irnd(tot);
  for(int i=0;i<WH_NW;i++){ r-=T[i].wt; if(r<0) return i; }
  return 0;
}
/*  What a wedge is worth right now.                                    */
static long long wh_value(int face,int w){
  const wedge_t*e=&wh_table(face)[w];
  if(e->kind==WK_POT) return jp_value(e->val);
  if(e->kind==WK_CR)  return (long long)e->val*TOTBET;
  return 0;
}
/*  Pay a wedge: a pot wedge empties its pot, as a reel jackpot does.   */
static long long wh_take(int face,int w){
  long long v=wh_value(face,w);
  const wedge_t*e=&wh_table(face)[w];
  if(e->kind==WK_POT) G.jpAcc[e->val]=0;
  return v;
}

static int wheel_triggered(void){ return G.wheelCount>=3; }

/*  The simulator's play: exactly the game's pick and pay, without the
 *  show.  SUPER swaps to the second table and spins again.             */
static long long __attribute__((unused)) wheel_sim_play(void){
  int face=0;
  for(int n=0;n<4;n++){
    int w=wheel_pick(face);
    if(wh_table(face)[w].kind==WK_UP){ face=1; continue; }
    return wh_take(face,w);
  }
  return 0;
}

/* ---- ASSETS ---- */
static uint32_t *whStage;             /* full-screen stage backdrop       */
static uint32_t *whFace[2][3];        /* [face][blur 0 sharp,1 mid,2 fast] */
static uint16_t *whGloss;             /* static light: mul<<8 | white    */
static spr_t whHub, whPtr, whKnob, whPeg, whBulb;
#define WH_HALO 40
static uint8_t whHalo[WH_HALO*WH_HALO];
static int whBuilt;
static float whPtrPX;                 /* flapper pivot x in its sprite   */
/* A lit wheel tab as it looks on the stage, glow and all: the rectangle
   [WTAB_X-WTAB_M, +WTAB_W+2*WTAB_M) clipped to the screen, rows from
   WTAB_Y(i)-WTAB_M.  Copied over the stage for the live wheel.        */
#define WTAB_X0 (WTAB_X-WTAB_M)
#define WTAB_X1 ((WTAB_X+WTAB_W+WTAB_M)<FBW?(WTAB_X+WTAB_W+WTAB_M):FBW)
#define WTAB_RH (WTAB_H+2*WTAB_M)
static uint32_t *whTabLit[2];
static spr_t whPlate[3];              /* prize plate glass: honey, cyan, magenta */

/* trim a sprite's empty columns; returns how many came off the left */
static int wh_crop_x(spr_t*s){
  if(!s->px) return 0;
  int x0=s->w, x1=-1;
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++)
    if(s->px[((size_t)y*s->w+x)*4+3]){ if(x<x0) x0=x; if(x>x1) x1=x; }
  if(x1<x0) return 0;
  int nw=x1-x0+1;
  uint8_t*n=(uint8_t*)malloc((size_t)nw*s->h*4);
  if(!n) return 0;
  for(int y=0;y<s->h;y++) memcpy(n+(size_t)y*nw*4,s->px+((size_t)y*s->w+x0)*4,(size_t)nw*4);
  free(s->px); s->px=n; s->w=nw;
  spr_bounds(s);
  return x0;
}

/* canvas region -> sprite, box filtered by f (the art is authored on the
   shared supersampled canvas at whatever scale suits it)               */
static void wh_resolve(spr_t*s,int w,int h,int f){
  s->w=w; s->h=h;
  s->px=(uint8_t*)calloc((size_t)w*h,4);
  if(!s->px){ s->w=s->h=0; return; }
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    int r=0,g=0,b=0,a=0;
    for(int j=0;j<f;j++) for(int i=0;i<f;i++){
      int cx=x*f+i, cy=y*f+j;
      if(cx>=CANW||cy>=CANH) continue;
      const uint8_t*p=canvas+((size_t)cy*CANW+cx)*4;
      r+=p[0]*p[3]; g+=p[1]*p[3]; b+=p[2]*p[3]; a+=p[3];
    }
    uint8_t*o=s->px+((size_t)y*w+x)*4;
    if(a>0){ o[0]=(uint8_t)(r/a); o[1]=(uint8_t)(g/a); o[2]=(uint8_t)(b/a); }
    o[3]=(uint8_t)(a/(f*f));
  }
  spr_bounds(s);
}

static float wh_smooth(float a,float b,float x){
  float t=clampf((x-a)/(b-a),0,1); return t*t*(3.0f-2.0f*t);
}
static uint32_t wh_hsv(float h,float s,float v){
  h-=floorf(h);
  float f=h*6.0f; int i=(int)f; f-=i;
  float p=v*(1-s), q=v*(1-s*f), t=v*(1-s*(1-f)), r,g,b;
  switch(i%6){
  case 0: r=v;g=t;b=p; break;  case 1: r=q;g=v;b=p; break;
  case 2: r=p;g=v;b=t; break;  case 3: r=p;g=q;b=v; break;
  case 4: r=t;g=p;b=v; break;  default: r=v;g=p;b=q; break;
  }
  return RGB((int)(r*255),(int)(g*255),(int)(b*255));
}
static uint32_t wh_hash(uint32_t x){
  x^=x>>16; x*=0x7FEB352Du; x^=x>>15; x*=0x846CA68Bu; x^=x>>16; return x;
}

/* ---- wedge lettering ----
 *  Each wedge's label is stamped as bubble type (the same disc-and-
 *  capsule welding as textb) into a mask in the wedge's own frame:
 *  t across the wedge, q out along its centre line.  Numbers stack
 *  upright and shrink toward the hub the way a real wheel's do; words
 *  read down the radius.  A fill-gradient channel rides along.        */
#define LB_T  40
#define LB_Q0 80
#define LB_Q1 234
#define LB_K  2
#define LB_W  (LB_T*2*LB_K)
#define LB_H  ((LB_Q1-LB_Q0)*LB_K)
typedef struct { uint8_t out[LB_W*LB_H], fill[LB_W*LB_H], v[LB_W*LB_H]; } lbl_t;
typedef struct { float t0,q0, ct,cq, rt,rq, s; } gmap_t;

static void lb_xy(const gmap_t*g,float row,float col,float*x,float*y){
  float t=g->t0+col*g->ct+row*g->rt, q=g->q0+col*g->cq+row*g->rq;
  *x=(t+LB_T)*LB_K; *y=(LB_Q1-q)*LB_K;
}
/* coverage of a capsule a-b of radius r; the fill pass also records the
   gradient position v along (ux,uy) from (ox,oy) over length len     */
static void lb_cap(uint8_t*m,uint8_t*vm,float ax,float ay,float bx,float by,float r,
                   float ox,float oy,float ux,float uy,float len){
  int x0=(int)(fminf(ax,bx)-r-1), x1=(int)(fmaxf(ax,bx)+r+2);
  int y0=(int)(fminf(ay,by)-r-1), y1=(int)(fmaxf(ay,by)+r+2);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>LB_W) x1=LB_W;
  if(y1>LB_H) y1=LB_H;
  float dx=bx-ax, dy=by-ay, l2=dx*dx+dy*dy, far=(r+0.5f)*(r+0.5f);
  for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){
    float px=x+0.5f-ax, py=y+0.5f-ay;
    float t=l2>0.0001f?clampf((px*dx+py*dy)/l2,0,1):0.0f;
    float qx=px-dx*t, qy=py-dy*t, q2=qx*qx+qy*qy;
    if(q2>=far) continue;
    float c=clampf(r-sqrtf(q2)+0.5f,0,1);
    int v=(int)(c*255);
    if(v>m[y*LB_W+x]){
      m[y*LB_W+x]=(uint8_t)v;
      if(vm){
        float g=((x+0.5f-ox)*ux+(y+0.5f-oy)*uy)/len;
        vm[y*LB_W+x]=(uint8_t)(clampf(g,0,1)*255);
      }
    }
  }
}
static void lb_glyph(lbl_t*L,int ch,const gmap_t*g){
  if(ch>='a'&&ch<='z') ch-=32;
  if(ch<32||ch>127) ch='?';
  const uint8_t*gl=FONT[ch-32];
  float s=g->s;
  float rin=0.62f*s*LB_K, rout=rin+fmaxf(0.32f*s,1.1f)*LB_K;
  float ox,oy,bx,by;
  lb_xy(g,-0.5f,2.0f,&ox,&oy);                    /* top edge of the glyph */
  lb_xy(g, 6.5f,2.0f,&bx,&by);
  float ux=bx-ox, uy=by-oy, len=sqrtf(ux*ux+uy*uy);
  if(len<0.001f) return;
  ux/=len; uy/=len;
  #define GSET(rr,cc) ((rr)>=0&&(rr)<7&&(cc)>=0&&(cc)<5&&(gl[rr]&(0x10>>(cc))))
  for(int r=0;r<7;r++) for(int c=0;c<5;c++){
    if(!GSET(r,c)) continue;
    float x,y; lb_xy(g,(float)r,(float)c,&x,&y);
    /* a dot with any neighbour is the end of a capsule already */
    int alone=1;
    for(int j=-1;j<=1;j++) for(int i=-1;i<=1;i++) if((i||j) && GSET(r+j,c+i)) alone=0;
    if(alone){
      lb_cap(L->out ,NULL ,x,y,x,y,rout,0,0,0,0,1);
      lb_cap(L->fill,L->v ,x,y,x,y,rin ,ox,oy,ux,uy,len);
    }
    static const int NB[4][2]={{0,1},{1,0},{1,1},{1,-1}};
    for(int k=0;k<4;k++){
      int nr=r+NB[k][0], nc=c+NB[k][1];
      if(!GSET(nr,nc)) continue;
      float x2,y2; lb_xy(g,(float)nr,(float)nc,&x2,&y2);
      lb_cap(L->out ,NULL ,x,y,x2,y2,rout,0,0,0,0,1);
      lb_cap(L->fill,L->v ,x,y,x2,y2,rin ,ox,oy,ux,uy,len);
    }
  }
  #undef GSET
}
/*  Room across a wedge at distance q from the centre, less the divider */
static float lb_room(float q){ return 2.0f*q*tanf(WH_WDEG*0.5f*(TAU/360.0f)) - 9.0f; }

static void lb_layout(lbl_t*L,const wedge_t*e){
  memset(L,0,sizeof *L);
  float qtop=LB_Q1-7;
  if(e->kind==WK_CR){
    char d[8]; snprintf(d,sizeof d,"%d",e->val);
    int n=(int)strlen(d);
    for(int i=0;i<=n;i++){
      /* the biggest glyph whose foot still fits the narrowing wedge */
      float s=6.6f;
      while(s>1.2f && 5.88f*s > lb_room(qtop-7.88f*s)) s-=0.05f;
      if(i==n){ if(s>2.7f) s=2.7f; if(s<1.4f) break; }
      gmap_t g={-2.0f*s, qtop-0.94f*s, s,0, 0,-s, s};
      lb_glyph(L, i<n?d[i]:'X', &g);
      qtop -= 7.88f*s + 0.30f*s + 1.5f;
    }
  } else {
    const char*w = e->kind==WK_UP ? "SUPER" :
                   (e->val==JP_MEGA ? "MEGA" : (e->val==JP_MAJOR ? "MAJOR" : "MINOR"));
    int n=(int)strlen(w);
    float s=4.4f;
    while(s>1.5f){
      float qin=qtop-(6.0f*n-1.0f)*s-1.88f*s;
      if(7.88f*s <= lb_room(qin)) break;
      s-=0.05f;
    }
    for(int i=0;i<n;i++){
      gmap_t g={3.0f*s, qtop-0.94f*s-i*6.0f*s, 0,-s, -s,0, s};
      lb_glyph(L,w[i],&g);
    }
  }
}

/* fill and outline colours for a wedge's lettering */
static void lb_style(const wedge_t*e,int face,const uint32_t**st,int*ns,uint32_t*outc){
  static const uint32_t WHITE[4]={0xFFFFFF,0xFFFDF0,0xFFF0B8,0xFFD050};
  static const uint32_t GOLD[5] ={0xFFFDF0,0xFFEBA8,0xF8C030,0xC08A10,0x7A5206};
  static const uint32_t ICE[4]  ={0xFFFFFF,0xE0F0FF,0x7AAEF0,0x2A5AB0};
  static const uint32_t RED[4]  ={0xFFE8E0,0xFF6A50,0xD01020,0x7A0610};
  static const uint32_t RAIN[5] ={0xFFFFFF,0xFFF0A0,0xFF90D0,0xA0B8FF,0x60F0D0};
  *outc=0x0C0610;
  if(e->kind==WK_UP){ *st=RAIN; *ns=5; *outc=0x000000; return; }
  if(e->kind==WK_POT){
    if(e->val==JP_MINOR){ *st=ICE;   *ns=4; *outc=0x061030; return; }
    if(e->val==JP_MAJOR){ *st=RED;   *ns=4; *outc=0x2A0400; return; }
    *st=WHITE; *ns=4; *outc=0x2A0650; return;
  }
  if(face || e->pal==WP_CRIMSON){ *st=GOLD; *ns=5; *outc=0x120600; return; }
  *st=WHITE; *ns=4;
}

/* the colour of a wedge's vinyl at radius fraction u, angle a (rad) */
static uint32_t wh_vinyl(int pal,float u,float adeg,float r,float dx,float dy){
  const uint32_t*P=WH_PAL[pal];
  if(pal==WP_MINOR){                          /* polished silver bands */
    static const uint32_t SLV[7]={0xFFFFFF,0xDCE4F4,0x8A96B4,0xF4F8FF,0xB4BED4,0x6A7694,0xE8EEFA};
    return mixc(ramp(SLV,7,fmodf(u*1.35f+0.05f,1.0f)),0x9AC0FF,0.12f);
  }
  if(pal==WP_MAJOR){                          /* burnished gold        */
    static const uint32_t GD[6]={0x7A4E06,0xE8AC22,0xFFF0B0,0xF0B830,0xA06E0A,0xFFE08A};
    return ramp(GD,6,fmodf(u*1.2f+0.1f,1.0f));
  }
  if(pal==WP_MEGA){                           /* rainbow, with glitter */
    uint32_t c=wh_hsv(0.02f+u*0.92f+adeg*0.004f,0.78f,1.0f);
    c=mixc(c,0xFFFFFF,0.10f+0.25f*wh_smooth(0.8f,1.0f,u));
    uint32_t h=wh_hash((uint32_t)((int)(dx*1.0f)+1000)*7919u ^ (uint32_t)((int)(dy*1.0f)+1000)*104729u);
    if((h&255)<9) c=mixc(c,0xFFFFFF,0.75f);
    return c;
  }
  if(pal==WP_SUPER){                          /* black, gold sunburst  */
    float ray=0.5f+0.5f*cosf((adeg+7.5f)*TAU/3.75f);
    uint32_t c=mixc(P[1],P[2],u*0.6f);
    c=mixc(c,0xC08A10,ray*ray*0.35f*(0.3f+0.7f*u));
    uint32_t h=wh_hash((uint32_t)((int)dx+1000)*7919u ^ (uint32_t)((int)dy+1000)*104729u);
    if((h&255)<5) c=mixc(c,0xFFE9A0,0.8f);
    return c;
  }
  (void)r;
  static uint32_t st[4];
  st[0]=P[0]; st[1]=P[1]; st[2]=mixc(P[1],P[2],0.35f); st[3]=P[2];
  return ramp(st,4,0.12f+0.88f*u);
}

/* one pixel of a face, at offset (dx,dy) from its centre */
static uint32_t wh_face_px(int face,const lbl_t*L,float dx,float dy){
  const float DEG=360.0f/TAU;
  static float wcs[WH_NW][2], hs, hc;          /* wedge centre directions */
  if(hs==0.0f){
    for(int i=0;i<WH_NW;i++){ float a=(i+0.5f)*WH_WDEG/DEG; wcs[i][0]=sinf(a); wcs[i][1]=cosf(a); }
    hs=sinf(WH_WDEG*0.5f/DEG); hc=cosf(WH_WDEG*0.5f/DEG);
  }
  float r=sqrtf(dx*dx+dy*dy);
  if(r<1.0f) r=1.0f;
  float phi=atan2f(dx,-dy)*DEG; if(phi<0) phi+=360.0f;
  int w=(int)(phi/WH_WDEG); if(w>=WH_NW) w=WH_NW-1;
  float adeg=phi-(w+0.5f)*WH_WDEG;
  /* the wedge's own frame: t across it, q out along its centre line */
  float t=dx*wcs[w][1]+dy*wcs[w][0], q=dx*wcs[w][0]-dy*wcs[w][1];
  const wedge_t*e=&wh_table(face)[w];
  float u=clampf((r-WH_RTRIM)/(WH_RF-WH_RTRIM),0,1);
  uint32_t c=wh_vinyl(e->pal,u,adeg,r,dx,dy);

  /* the wedge is inset: shade toward its dividers */
  float dd=q*hs-fabsf(t)*hc;                   /* distance to the nearer divider */
  if(dd<8.0f) c=scalec(c,0.62f+0.38f*wh_smooth(0,8,dd));
  else if(dd<12.0f) c=mixc(c,0xFFFFFF,0.06f*(1.0f-fabsf(dd-10.0f)/2.0f));

  /* lettering */
  if(q>LB_Q0 && q<LB_Q1-1 && fabsf(t)<LB_T-1){
    const lbl_t*lb=&L[w];
    float mx=(t+LB_T)*LB_K-0.5f, my=(LB_Q1-q)*LB_K-0.5f;
    int ix=(int)mx, iy=(int)my; float fx=mx-ix, fy=my-iy;
    if(ix>=0 && iy>=0 && ix+1<LB_W && iy+1<LB_H){
      #define LBS(m) ( (m[iy*LB_W+ix]*(1-fx)+m[iy*LB_W+ix+1]*fx)*(1-fy) + \
                       (m[(iy+1)*LB_W+ix]*(1-fx)+m[(iy+1)*LB_W+ix+1]*fx)*fy )
      float co=LBS(lb->out)/255.0f, cf=LBS(lb->fill)/255.0f;
      if(co>0.002f){
        const uint32_t*st; int ns; uint32_t oc;
        lb_style(e,face,&st,&ns,&oc);
        c=mixc(c,oc,co);
        if(cf>0.002f){
          int k=iy*LB_W+ix;
          int vv=lb->v[k]; if(lb->fill[k+1]>lb->fill[k]) vv=lb->v[k+1];
          if(lb->fill[k+LB_W]>lb->fill[k]) vv=lb->v[k+LB_W];
          float v=vv/255.0f;
          uint32_t fc=ramp(st,ns,v);
          if(v<0.30f) fc=mixc(fc,0xFFFFFF,(0.30f-v)/0.30f*0.55f);
          c=mixc(c,fc,cf);
        }
      }
      #undef LBS
    }
  }

  /* gold dividers, analytically antialiased */
  if(r>WH_RTRIM-2){
    const float hw=2.7f;
    float cov=clampf(hw+0.5f-dd,0,1);
    if(cov>0.0f){
      static const uint32_t DG[4]={0xFFF8D8,0xF8C848,0xB07A10,0x4A3004};
      c=mixc(c,ramp(DG,4,clampf(dd/hw,0,1)),cov);
    }
  }
  /* the trim ring round the hub, and the face's outer bead */
  if(r<WH_RTRIM+1.5f){
    float k=clampf((r-(WH_RHUB-4))/(WH_RTRIM-(WH_RHUB-4)),0,1);
    static const uint32_t TR[5]={0x5A3A06,0xFFF0B0,0xE8B030,0x8A5A08,0x2A1802};
    uint32_t tc=ramp(TR,5,k);
    c=mixc(c,tc,clampf(WH_RTRIM+0.5f-r,0,1));
  }
  if(r>WH_RF-7.0f){
    float k=clampf((r-(WH_RF-7.0f))/7.0f,0,1);
    c=mixc(c,0x100804,k*0.85f);
  }
  return c;
}

/* ---- building a face, in steps ----
 *  A face is built as a sequence of small steps - lay out one label,
 *  paint one row, blur one row - so that the SUPER face, which is only
 *  wanted after an upgrade, is built a few rows a frame while the first
 *  wheel is on screen instead of lengthening the core's start-up.     */
typedef struct {
  int face, step;
  uint32_t *F, *P, *B1, *B2;
  lbl_t *pool;
} fbuild_t;
static fbuild_t whFB[2];
enum { FB_ROWS=WH_NW, FB_PEGS=FB_ROWS+WH_FW, FB_B1=FB_PEGS+1,
       FB_B2=FB_B1+WH_FW, FB_DONE=FB_B2+WH_FW };

/*  Angular blur of one row: each pixel averages the face along its own
 *  arc, `deg` degrees long.  Rotation invariant, so it is exactly the
 *  motion blur of the face turning, baked once.  `taps` fixes the sample
 *  count (0 = one per pixel of arc).  The fast level is the 3-degree
 *  level sampled four times 2.5 degrees apart: a box convolved with a
 *  comb finer than the box is a smooth ~10 degree blur, for a tenth of
 *  the work of sampling it directly.                                  */
static void wh_blur_row(const uint32_t*S,uint32_t*B,int y,float deg,int taps){
  float arc=deg*(TAU/360.0f);
  float c0=cosf(-arc*0.5f), s0=sinf(-arc*0.5f);
  for(int x=0;x<WH_FW;x++){
    float dx=x+0.5f-WH_FC, dy=y+0.5f-WH_FC;
    float r=sqrtf(dx*dx+dy*dy);
    int n=taps?taps:(int)(r*arc)+1; if(n>48) n=48;
    if(n<2 || r>WH_RS){ B[(size_t)y*WH_FW+x]=S[(size_t)y*WH_FW+x]; continue; }
    float st=arc/(n-1), cs=cosf(st), ss=sinf(st);
    float vx=dx*c0-dy*s0, vy=dx*s0+dy*c0;
    int R=0,Gc=0,Bc=0;
    for(int k=0;k<n;k++){
      int sx=(int)(vx+WH_FC), sy=(int)(vy+WH_FC);
      if(sx<0) sx=0;
      if(sy<0) sy=0;
      if(sx>=WH_FW) sx=WH_FW-1;
      if(sy>=WH_FW) sy=WH_FW-1;
      uint32_t p=S[(size_t)sy*WH_FW+sx];
      R+=(p>>16)&255; Gc+=(p>>8)&255; Bc+=p&255;
      float nx=vx*cs-vy*ss; vy=vx*ss+vy*cs; vx=nx;
    }
    B[(size_t)y*WH_FW+x]=RGB(R/n,Gc/n,Bc/n);
  }
}

/*  One step of a face build; returns its rough cost in row-units, 0 when
 *  the face is complete.  The blurred copies carry the pegs painted in,
 *  so at speed the pegs smear into a gold ring with everything else; at
 *  rest the pegs are sprites, lit from a light that stays put.        */
static int wh_face_step(fbuild_t*b){
  if(b->step>=FB_DONE) return 0;
  size_t n=(size_t)WH_FW*WH_FW;
  if(b->step==0 && !b->F){
    b->F=(uint32_t*)malloc(n*4);  b->P=(uint32_t*)malloc(n*4);
    b->B1=(uint32_t*)malloc(n*4); b->B2=(uint32_t*)malloc(n*4);
    b->pool=(lbl_t*)calloc(WH_NW,sizeof(lbl_t));
    if(!b->F||!b->P||!b->B1||!b->B2||!b->pool){
      free(b->F); free(b->P); free(b->B1); free(b->B2); free(b->pool);
      b->F=b->P=b->B1=b->B2=NULL; b->pool=NULL; b->step=FB_DONE; return 0;
    }
  }
  int s=b->step++, cost=1;
  if(s<FB_ROWS){ lb_layout(&b->pool[s],&wh_table(b->face)[s]); cost=6; }
  else if(s<FB_PEGS){
    int y=s-FB_ROWS;
    for(int x=0;x<WH_FW;x++){
      float dx=x+0.5f-WH_FC, dy=y+0.5f-WH_FC;
      b->F[(size_t)y*WH_FW+x] = (dx*dx+dy*dy > (WH_RS+0.5f)*(WH_RS+0.5f)) ? 0x100804
                               : wh_face_px(b->face,b->pool,dx,dy);
    }
  } else if(s==FB_PEGS){
    free(b->pool); b->pool=NULL;
    whFace[b->face][0]=b->F;
    memcpy(b->P,b->F,n*4);
    for(int k=0;k<WH_NW;k++){
      float a=k*WH_WDEG*(TAU/360.0f);
      float px=WH_FC+sinf(a)*WH_RPEG, py=WH_FC-cosf(a)*WH_RPEG;
      for(int y=(int)py-9;y<=(int)py+9;y++) for(int x=(int)px-9;x<=(int)px+9;x++){
        float dx=x+0.5f-px, dy=y+0.5f-py, d=sqrtf(dx*dx+dy*dy);
        float cov=clampf(7.0f-d,0,1);
        if(cov<=0) continue;
        float l=clampf(1.0f-d/7.0f,0,1);
        uint32_t*o=&b->P[(size_t)y*WH_FW+x];
        *o=mixc(*o,mixc(0x6A4406,0xFFF0B0,l),cov);
      }
    }
  }
  else if(s<FB_B2) wh_blur_row(b->P ,b->B1,s-FB_B1,3.0f,0);
  else             wh_blur_row(b->B1,b->B2,s-FB_B2,7.5f,4);
  if(b->step>=FB_DONE){
    free(b->P); b->P=NULL;
    whFace[b->face][1]=b->B1; whFace[b->face][2]=b->B2;
  }
  return cost;
}

/*  The light that does not turn with the wheel: the rim's shadow on the
 *  face, the hub's, a warm key from the upper left and a glass glare.  */
static void wh_build_gloss(void){
  whGloss=(uint16_t*)malloc((size_t)WH_GW*WH_GW*2);
  if(!whGloss) return;
  for(int y=0;y<WH_GW;y++) for(int x=0;x<WH_GW;x++){
    float dx=x+0.5f-(WH_RF+1.5f), dy=y+0.5f-(WH_RF+1.5f);
    float r=sqrtf(dx*dx+dy*dy), rr=r/WH_RF;
    float mul=1.0f;
    mul*=1.0f-0.50f*wh_smooth(0.90f,1.0f,rr);
    mul*=1.0f-0.45f*(1.0f-wh_smooth(0,16,r-WH_RTRIM+2));
    mul*=0.94f-0.10f*(dx/WH_RF*0.55f+dy/WH_RF*0.75f);
    float gx=dx/WH_RF+0.34f, gy=dy/WH_RF+0.40f;
    float gd=sqrtf(gx*gx+gy*gy);
    float add=0.20f*powf(clampf(1.0f-gd/0.62f,0,1),2.0f);
    /* a curved window reflection: an arc band across the upper left */
    float bx=dx/WH_RF+0.95f, by=dy/WH_RF+1.05f;
    float bd=sqrtf(bx*bx+by*by);
    add+=0.13f*wh_smooth(0.035f,0.0f,fabsf(bd-1.10f))*wh_smooth(0.2f,0.75f,1.0f-rr*0.6f);
    /* a crescent where the lip catches the key light */
    float cosk=(-dx*0.62f-dy*0.78f)/(r+0.001f);
    add+=0.20f*wh_smooth(0.86f,0.955f,rr)*wh_smooth(0.99f,0.955f,rr)*clampf(cosk,0,1);
    int m=(int)(clampf(mul,0,1)*255.0f), s=(int)(clampf(add,0,0.9f)*255.0f);
    whGloss[(size_t)y*WH_GW+x]=(uint16_t)((m<<8)|s);
  }
}

/* shaded torus section between r0 and r1 at (dx,dy): chrome or gold */
static uint32_t wh_tube(float dx,float dy,float r,float r0,float r1,const uint32_t*st,int ns,float bulge){
  float u=clampf((r-r0)/(r1-r0),0,1);
  float nr=(2.0f*u-1.0f)*bulge, nz=sqrtf(fmaxf(0.0f,1.0f-nr*nr));
  float ex=dx/r, ey=dy/r;
  float nx=ex*nr, ny=ey*nr;
  const float lx=-0.46f, ly=-0.64f, lz=0.61f;       /* key light, normalised */
  float lam=clampf(nx*lx+ny*ly+nz*lz,0,1);
  float hx=lx, hy=ly, hz=lz+1.0f, hl=sqrtf(hx*hx+hy*hy+hz*hz);
  float sp=clampf((nx*hx+ny*hy+nz*hz)/hl,0,1);
  float s2=sp*sp, s4=s2*s2, s8=s4*s4, s32=s8*s8*s8*s8;
  sp=s32*s4;                                        /* ^36, without powf */
  uint32_t c=ramp(st,ns,lam);
  return mixc(c,0xFFFFFF,sp*0.85f);
}

/* the size at which s fits in maxw px (text scales linearly with size) */
static float wh_fit(int f,const char*s,float size,float maxw){
  float w=lz_width(f,s,size,0);
  return w>maxw?size*maxw/w:size;
}

/* The lounge's glass panel (lz_glass) as a sprite, the glass at (m, m)
   with m = WPLATE_M: for a panel that comes and goes over live art.    */
static void wh_bake_glass(spr_t*out,int w,int h,uint32_t neon,float glow_k){
  const int m=WPLATE_M;
  LCanvas cv;
  memset(out,0,sizeof *out);
  if(!lz_cv_new(&cv,w+2*m,h+2*m)) return;
  float hw=w*0.5f, hh=h*0.5f, cx=m+hw, cy=m+hh;
  LShape box[1]={ { LSH_RBOX, LOP_UNION, { cx, cy, hw, hh, 18 }, NULL, 0 } };
  LFillOpt g={0};
  g.outline=2; g.glow=10; g.opacity=clampf(0.8f*glow_k,0,1); g.blend=LBL_ADD;
  LPaint np=lpaint_solid(lz_col(neon,1));
  lcv_fill(&cv,NULL,box,1,&np,&g);
  LPaint fill=lpaint_linear(lz_col(0x1E1624,0.90f),0,cy-hh,lz_col(0x0C080F,0.94f),0,cy+hh);
  lcv_fill(&cv,NULL,box,1,&fill,NULL);
  LShape shn[1]={ { LSH_RBOX, LOP_UNION, { cx, cy-hh*0.62f, hw-8, hh*0.30f, 12 }, NULL, 0 } };
  LPaint sp=lpaint_linear(lrc(1,1,1,0.08f),0,cy-hh,lrc(1,1,1,0.0f),0,cy-hh*0.3f);
  lcv_fill(&cv,NULL,shn,1,&sp,NULL);
  LFillOpt ol={0};
  ol.outline=3;
  LPaint tube=lpaint_solid(lz_col(lz_hot(neon,0.3f),1));
  lcv_fill(&cv,NULL,box,1,&tube,&ol);
  lz_cv_to_spr(&cv,out);
}

/* the two wheel tabs: their neon, and their caption, dim or lit */
static const uint32_t WTAB_NEON[2]={LZ_HONEY,LZ_MAGENTA};
static void wh_tab_label(int i,int lit){
  lz_style st; memset(&st,0,sizeof st);
  st.color=lit?0xFFFAF0:0x7A6C80; st.align=LZ_CENTER;
  st.shadow=0x000000; st.shadow_k=0.85f;
  lz_text_ex(LZF_DISP_S,i?"SUPER WHEEL":"WHEEL OF 7'S",(float)WCX_R,(float)(WTAB_Y(i)+(lit?2:0)+10),21.0f,&st);
}

static void wh_build_stage(void){
  /* 1. the room: the lounge's honeycomb, with a warm bloom behind the
        wheel, faint honey rays fanning out from it and two soft spots
        from the flies.  Only the strip between the panels is worked;
        the panels' dark glass covers the rest.                        */
  lz_paint_room(fb,0);
  for(int y=0;y<FBH;y++) for(int x=WPL_X+WCOL_W-20;x<WPR_X+20;x++){
    float dx=(float)(x-WH_CX), dy=(float)(y-WH_CY);
    float d2=dx*dx+dy*dy;
    if(d2<(WH_RRIM-2)*(WH_RRIM-2)) continue;
    float d=sqrtf(d2);
    float bloom=powf(clampf(1.0f-d/600.0f,0,1),2.0f);
    float ang=atan2f(dx,-dy);
    float ray=0.5f+0.5f*cosf(ang*16.0f);
    ray=ray*ray*ray*clampf((d-300.0f)/60.0f,0,1)*clampf(1.0f-(d-300.0f)/480.0f,0,1);
    float rr=bloom*96+ray*54, gg=bloom*44+ray*34, bb=bloom*40+ray*10;
    for(int s=0;s<2;s++){
      float sx=s?1180.0f:100.0f, sy=-80.0f;
      float ax=WH_CX-sx, ay=WH_CY-sy, al=sqrtf(ax*ax+ay*ay);
      float px=x-sx, py=y-sy, pl=sqrtf(px*px+py*py)+0.001f;
      float cosb=(px*ax+py*ay)/(pl*al);
      float cone=wh_smooth(0.955f,0.992f,cosb)*clampf(1.0f-pl/1100.0f,0,1);
      rr+=cone*40; gg+=cone*30; bb+=cone*26;
    }
    fb_add(x,y,(int)rr,(int)gg,(int)bb);
  }

  /* 2. pedestal under the wheel, and the wheel's own shadow */
  for(int j=0;j<60;j++){
    int w=150+j*2, y=WH_CY+WH_RRIM-26+j;
    uint32_t c=mixc(0x8A5A10,0x2A1804,j/59.0f);
    for(int i=-w/2;i<w/2;i++){
      float e=fabsf((float)i/(w/2));
      fb_px(WH_CX+i,y,mixc(c,0x000000,e*e*0.6f));
    }
  }
  for(int y=WH_CY-WH_RRIM-24;y<WH_CY+WH_RRIM+30;y++)
    for(int x=WH_CX-WH_RRIM-24;x<WH_CX+WH_RRIM+30;x++){
      float dx=x+0.5f-(WH_CX+8), dy=y+0.5f-(WH_CY+12);
      float d=sqrtf(dx*dx+dy*dy);
      if(d<WH_RRIM-4 || d>WH_RRIM+26) continue;
      float k=1.0f-clampf((d-(WH_RRIM-4))/30.0f,0,1);
      fb_blend(x,y,0x000000,(int)(k*k*200));
    }

  /* 3. the rim: a chrome lip, then a fat gold band, two engraved rules */
  static const uint32_t GOLDT[6]={0x2A1602,0x6A4406,0xB8800E,0xF0BC30,0xFFE590,0xFFF8DC};
  static const uint32_t CHRT[5] ={0x10141C,0x3A4254,0x8C96AC,0xDCE2F0,0xFFFFFF};
  for(int y=WH_CY-WH_RRIM-2;y<=WH_CY+WH_RRIM+2;y++)
    for(int x=WH_CX-WH_RRIM-2;x<=WH_CX+WH_RRIM+2;x++){
      float dx=x+0.5f-WH_CX, dy=y+0.5f-WH_CY;
      float r=sqrtf(dx*dx+dy*dy);
      if(r<WH_RF-1.0f || r>WH_RRIM+1.0f) continue;
      uint32_t c;
      if(r<WH_RLIP) c=wh_tube(dx,dy,r,WH_RF-1.0f,WH_RLIP,CHRT,5,0.95f);
      else {
        c=wh_tube(dx,dy,r,WH_RLIP,WH_RRIM,GOLDT,6,0.80f);
        float g1=fabsf(r-(WH_RLIP+6.0f)), g2=fabsf(r-(WH_RRIM-6.0f));
        if(g1<1.2f) c=mixc(c,0x2A1602,0.6f*(1.2f-g1));
        if(g2<1.2f) c=mixc(c,0x2A1602,0.6f*(1.2f-g2));
      }
      float lip=clampf(WH_RLIP+0.5f-r,0,1)*clampf(r-(WH_RLIP-0.5f),0,1);
      if(lip>0) c=mixc(c,0x000000,0.5f*lip);
      float cov=clampf(WH_RRIM+0.5f-r,0,1)*clampf(r-(WH_RF-1.5f),0,1);
      fb_blend(x,y,c,(int)(cov*255));
    }
  /* bulb sockets: chrome bezel round an unlit amber bulb */
  for(int k=0;k<WH_NBULB;k++){
    float a=(k+0.5f)*TAU/WH_NBULB;
    float bx=WH_CX+sinf(a)*WH_RBULB, by=WH_CY-cosf(a)*WH_RBULB;
    for(int y=(int)by-14;y<=(int)by+14;y++) for(int x=(int)bx-14;x<=(int)bx+14;x++){
      float dx=x+0.5f-bx, dy=y+0.5f-by, r=sqrtf(dx*dx+dy*dy);
      if(r>12.5f) continue;
      uint32_t c;
      if(r>8.5f){ c=wh_tube(dx,dy,r+0.001f,8.5f,12.5f,CHRT,5,0.9f); }
      else {
        float nx=dx/8.5f, ny=dy/8.5f, nz=sqrtf(fmaxf(0.0f,1.0f-nx*nx-ny*ny));
        float l=clampf(-nx*0.45f-ny*0.6f+nz*0.66f,0,1);
        c=mixc(0x1A0C02,0x6A4210,l);
        float sx=nx+0.35f, sy=ny+0.42f;
        float sp=1.0f-clampf(sqrtf(sx*sx+sy*sy)*2.8f,0,1);
        c=mixc(c,0xFFF0D0,sp*sp*0.8f);
      }
      fb_blend(x,y,c,(int)(clampf(12.5f+0.5f-r,0,1)*255));
    }
  }

  /* 4. the pointer's mounting: a gold crest above the rim */
  {
    int w=120, x0=WH_CX-w/2, y0=8;
    fb_rrect(x0-4,y0-2,w+8,86,26,0x000000,170);
    fb_rrectg(x0,y0,w,78,24,0xFFE9A0,0x6A4406,255);
    fb_rrectg(x0+8,y0+6,w-16,22,10,0xFFFFFF,0xFFE090,90);
    fb_rframe(x0,y0,w,78,24,2.5f,0x3A2402,255);
    fb_rframe(x0+5,y0+5,w-10,68,19,1.2f,0xFFF4C8,200);
  }

  /* 5. the lounge's furniture either side: dark glass with neon edges,
        spaced captions, and dark wells for the readouts drawn live.
        Left: the title sign, the three pots, how it plays.            */
  lz_glass(WPL_X,WT_Y,WCOL_W,WT_H,LZ_HONEY,0.55f,1.0f);
  {
    float s=wh_fit(LZF_DISP_L,"WHEEL",84.0f,WCOL_W-48);
    lz_gold(LZF_DISP_L,"WHEEL",WCX_L,WT_Y+52,s,1.0f,0.7f);
    lz_neon(LZF_NEON_L,"OF 7'S",WCX_L,WT_Y+110,50,LZ_MAGENTA,1.0f,0.9f);
  }
  lz_glass(WPL_X,WJ_Y,WCOL_W,WJ_H,LZ_MAGENTA,0.55f,1.0f);
  rail_label(WPL_X,WJ_Y,WCOL_W,"JACKPOT WEDGES",lz_hot(LZ_MAGENTA,0.35f));
  {
    /* each pot, and which wheel carries its wedge */
    static const char*PN[3]={"MEGA","MAJOR","MINOR"};
    static const char*PW[3]={"SUPER WHEEL","BOTH WHEELS","WHEEL OF 7'S"};
    static const uint32_t PC[3]={LZ_MAGENTA,LZ_AMBER,LZ_CYAN};
    for(int i=0;i<3;i++){
      int y=WJ_ROW(i);
      lz_text_sh(LZF_UI_M,PN[i],WPL_X+16,(float)y-3,24,lz_hot(PC[i],0.35f),LZ_LEFT);
      lz_text_sh(LZF_UI_S,PW[i],WPL_X+WCOL_W-16,(float)y+1,18,LZ_DIM,LZ_RIGHT);
      led_window(WPL_X+12,y+22,WCOL_W-24,44);
    }
  }
  lz_glass(WPL_X,WHP_Y,WCOL_W,WHP_H,LZ_CYAN,0.55f,1.0f);
  rail_label(WPL_X,WHP_Y,WCOL_W,"HOW IT PLAYS",lz_hot(LZ_CYAN,0.35f));
  {
    static const char*HW[6]={"WEDGES PAY X YOUR BET","POT WEDGES PAY THE POT","LAND SUPER TO UPGRADE",
                             "TO THE SUPER WHEEL:","BIGGER WEDGES AND","THE MEGA JACKPOT"};
    for(int i=0;i<6;i++)
      lz_text_sh(LZF_UI_M,HW[i],WCX_L,(float)(WHP_Y+34+i*34),25,
                 i==2?LZ_GOLD:(i==3?lz_hot(LZ_MAGENTA,0.45f):LZ_IVORY),LZ_CENTER);
  }
  /* right: bet, win, which wheel is live, and the status panel */
  lz_glass(WPR_X,WB_Y,WCOL_W,WB_H,LZ_AMBER,0.55f,1.0f);
  rail_label(WPR_X,WB_Y,WCOL_W,"BET",lz_hot(LZ_AMBER,0.35f));
  led_window(WPR_X+14,WB_Y+24,WCOL_W-28,60);
  lz_glass(WPR_X,WW_Y,WCOL_W,WW_H,LZ_MAGENTA,0.55f,1.0f);
  rail_label(WPR_X,WW_Y,WCOL_W,"WHEEL WIN",lz_hot(LZ_MAGENTA,0.35f));
  led_window(WPR_X+14,WW_Y+24,WCOL_W-28,76);
  for(int i=0;i<2;i++){
    lz_button(WTAB_X,WTAB_Y(i),WTAB_W,WTAB_H,WTAB_NEON[i],0);
    wh_tab_label(i,0);
  }
  lz_glass(WPR_X,WS_Y,WCOL_W,WS_H,LZ_CYAN,0.45f,1.0f);

  memcpy(whStage,fb,sizeof fb);

  /* The live wheel's tab, lit, glow and all, as it looks over this stage:
     painted, kept aside, and the stage put back.                        */
  for(int i=0;i<2;i++){
    const int w=WTAB_X1-WTAB_X0, y0=WTAB_Y(i)-WTAB_M;
    whTabLit[i]=(uint32_t*)malloc((size_t)w*WTAB_RH*4);
    if(!whTabLit[i]) continue;
    lz_button(WTAB_X,WTAB_Y(i),WTAB_W,WTAB_H,WTAB_NEON[i],1);
    wh_tab_label(i,1);
    for(int j=0;j<WTAB_RH;j++){
      memcpy(whTabLit[i]+(size_t)j*w,fb+(size_t)(y0+j)*FBW+WTAB_X0,(size_t)w*4);
      memcpy(fb+(size_t)(y0+j)*FBW+WTAB_X0,whStage+(size_t)(y0+j)*FBW+WTAB_X0,(size_t)w*4);
    }
  }
}

/* the hub, pointer, knob, peg and bulb sprites, on the shared canvas */
static void wh_build_sprites(void){
  /* hub: chrome, gold, a red dome and the gold-rimmed seven, 141 px */
  cv_clear();
  {
    const float c=212.0f;
    cv_circle(c+4,c+6,209,0x000000,0x000000,130);
    pt_t p[48];
    static const uint32_t chrome[7]={0xFFFFFF,0xE4E9F4,0x7E88A2,0xF6F9FF,0xB6BDCC,0x59627A,0x2C3242};
    for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=c+cosf(a)*208; p[i].y=c+sinf(a)*208; }
    cv_polyN(p,48,chrome,7,255);
    for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=c+cosf(a)*194; p[i].y=c+sinf(a)*194; }
    cv_polyN(p,48,GOLDG,5,255);
    cv_circle(c,c,180,0x2A0404,0x0A0000,255);
    cv_ellipse_lit(c,c,172,172,0x3A0008,0xB01020,0xFF8A70);
    for(int i=0;i<16;i++){                        /* a ring of gold studs */
      float a=TAU*(i+0.5f)/16.0f;
      cv_sphere(c+cosf(a)*187,c+sinf(a)*187,7,0x6A4406,0xE8B030,0xFFF8D8);
    }
    static const uint32_t body[5]={0xFFF8D0,0xFFE070,0xF0B020,0xA86E08,0x5A3A04};
    seven_shape(46,47.5f,0.70f,body,5,1);
    cv_ellipse(c,c-84,118,50,0xFFFFFF,0xFFFFFF,52);
  }
  wh_resolve(&whHub,141,141,3);

  /* the flapper: gold, bevelled, a ruby set in it.  Pivot at (53,9). */
  cv_clear();
  {
    pt_t b[5]={{146,40},{278,40},{306,136},{212,420},{118,136}};
    cv_poly_outline(b,5,12,0x1A0C00,255);
    static const uint32_t gold[5]={0xFFFBE0,0xFFE07A,0xE8A820,0xA06A08,0x5E4004};
    cv_polyN(b,5,gold,5,255);
    pt_t lf[3]={{212,44},{122,136},{212,414}};
    cv_poly(lf,3,0xFFFFFF,0xFFF0C0,80);
    pt_t rt[3]={{212,44},{302,136},{212,414}};
    cv_poly(rt,3,0x000000,0x000000,60);
    cv_circle(212,44,62,0x1A0C00,0x1A0C00,255);
    cv_sphere(212,44,56,0x6A4406,0xE8B030,0xFFF8D8);
    cv_circle(212,160,40,0x1A0000,0x1A0000,255);
    cv_sphere(212,160,34,0x4A0008,0xE01830,0xFFB0B0);
  }
  wh_resolve(&whPtr,106,106,4);
  whPtrPX=53.0f-(float)wh_crop_x(&whPtr);   /* pivot, after the crop */

  cv_clear();                                       /* pivot knob, 32 px */
  cv_circle(66,68,60,0x000000,0x000000,120);
  cv_sphere(64,64,58,0x303644,0xB8C0D0,0xFFFFFF);
  cv_sphere(64,64,30,0x6A4406,0xE8B030,0xFFF8D8);
  wh_resolve(&whKnob,32,32,4);

  cv_clear();                                       /* peg, 16 px         */
  cv_circle(35,37,25,0x000000,0x000000,140);
  cv_sphere(32,32,23,0x5A3A00,0xE8B830,0xFFFBE0);
  wh_resolve(&whPeg,16,16,4);

  cv_clear();                                       /* lit bulb, 20 px    */
  cv_sphere(40,40,33,0xC08020,0xFFF0C0,0xFFFFFF);
  wh_resolve(&whBulb,20,20,4);

  for(int y=0;y<WH_HALO;y++) for(int x=0;x<WH_HALO;x++){
    float dx=(x+0.5f-WH_HALO*0.5f)/(WH_HALO*0.5f), dy=(y+0.5f-WH_HALO*0.5f)/(WH_HALO*0.5f);
    float v=clampf(1.0f-sqrtf(dx*dx+dy*dy),0,1);
    whHalo[y*WH_HALO+x]=(uint8_t)(v*v*255);
  }
}

/*  The flapper pre-rotated every half degree across its swing, each copy
 *  tight-cropped with its pivot recorded, so drawing it is two blits
 *  (shadow and body) whatever angle the pegs have knocked it to.      */
#define WH_PSTEP 0.5f
#define WH_PN    89                   /* 2*WH_PMAX/WH_PSTEP + 1           */
static spr_t   whPtrRot[WH_PN];
static int16_t whPtrPiv[WH_PN][2];

static void wh_build_ptr_rot(void){
  const spr_t*s=&whPtr;
  if(!s->px) return;
  const float pvx=whPtrPX, pvy=9.0f;
  for(int i=0;i<WH_PN;i++){
    float deg=-WH_PMAX+i*WH_PSTEP, a=deg*(TAU/360.0f), c=cosf(a), sn=sinf(a);
    float mnx=1e9f,mny=1e9f,mxx=-1e9f,mxy=-1e9f;
    for(int k=0;k<4;k++){
      float qx=((k&1)?s->w:0)-pvx, qy=((k>>1)?s->h:0)-pvy;
      float sx=c*qx-sn*qy, sy=sn*qx+c*qy;
      if(sx<mnx) mnx=sx;
      if(sx>mxx) mxx=sx;
      if(sy<mny) mny=sy;
      if(sy>mxy) mxy=sy;
    }
    int x0=(int)floorf(mnx)-1, y0=(int)floorf(mny)-1;
    int w=(int)ceilf(mxx)+2-x0, h=(int)ceilf(mxy)+2-y0;
    spr_t*o=&whPtrRot[i];
    o->w=w; o->h=h; o->px=(uint8_t*)calloc((size_t)w*h,4);
    if(!o->px){ o->w=o->h=0; continue; }
    whPtrPiv[i][0]=(int16_t)(-x0); whPtrPiv[i][1]=(int16_t)(-y0);
    for(int y=0;y<h;y++){
      float dy=y+y0+0.5f, dx0=x0+0.5f;
      float u=c*dx0+sn*dy+pvx-0.5f-c, v=-sn*dx0+c*dy+pvy-0.5f+sn;
      for(int x=0;x<w;x++){
      u+=c; v-=sn;
      int iu=(int)(u+16.0f)-16, iv=(int)(v+16.0f)-16;   /* floor, for u > -16 */
      if(iu<-1||iv<-1||iu>=s->w||iv>=s->h) continue;
      float fu=u-iu, fv=v-iv, R=0,Gc=0,B=0,A=0;
      for(int k=0;k<4;k++){
        int px=iu+(k&1), py=iv+(k>>1);
        if(px<0||py<0||px>=s->w||py>=s->h) continue;
        const uint8_t*p=s->px+((size_t)py*s->w+px)*4;
        float wt=((k&1)?fu:1-fu)*((k>>1)?fv:1-fv)*p[3];
        R+=p[0]*wt; Gc+=p[1]*wt; B+=p[2]*wt; A+=wt;
      }
      if(A<1.0f) continue;
      uint8_t*q=o->px+((size_t)y*w+x)*4;
      q[0]=(uint8_t)(R/A); q[1]=(uint8_t)(Gc/A); q[2]=(uint8_t)(B/A); q[3]=(uint8_t)(A>255?255:A);
      }
    }
    spr_bounds(o);
  }
}

/*  Light rays that turn slowly behind the wheel on a big prize.  Only
 *  the strip between the panels and outside the rim can show them, so
 *  that is all that is mapped: per pixel, its angle round the wheel and
 *  how strongly a ray lights it.  The draw is a table lookup and an add. */
#define WR_X0 320
#define WR_X1 960
static uint16_t *whRayMap;
static uint8_t   whRayPat[256];

static void wh_build_rays(void){
  whRayMap=(uint16_t*)malloc((size_t)(WR_X1-WR_X0)*FBH*2);
  if(!whRayMap) return;
  for(int i=0;i<256;i++){
    float v=cosf(i*TAU*12.0f/256.0f); v=v>0?v*v*v:0;
    whRayPat[i]=(uint8_t)(v*255);
  }
  for(int y=0;y<FBH;y++) for(int x=WR_X0;x<WR_X1;x++){
    float dx=x+0.5f-WH_CX, dy=y+0.5f-WH_CY, d=sqrtf(dx*dx+dy*dy);
    uint16_t m=0;
    if(d>WH_RRIM+2){
      float f=clampf((d-WH_RRIM-2)/26.0f,0,1)*clampf(1.0f-(d-WH_RRIM)/520.0f,0,1);
      int a=(int)((atan2f(dx,-dy)/TAU+1.0f)*256.0f)&255;
      m=(uint16_t)((a<<8)|(int)(f*255));
    }
    whRayMap[(size_t)y*(WR_X1-WR_X0)+(x-WR_X0)]=m;
  }
}

static void wh_rays(float rot,uint32_t col,float k){
  if(!whRayMap || k<=0.0f) return;
  int ro=(int)(rot*256.0f)&255, K=(int)(k*256.0f);
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  int y0=clip_y0<0?0:clip_y0, y1=clip_y1>FBH?FBH:clip_y1;
  for(int y=y0;y<y1;y++){
    const uint16_t*m=whRayMap+(size_t)y*(WR_X1-WR_X0);
    for(int x=WR_X0;x<WR_X1;x++){
      uint16_t v=m[x-WR_X0];
      int f=v&255; if(!f) continue;
      int s=(whRayPat[((v>>8)+ro)&255]*f*K)>>16;
      if(s<=2) continue;
      fb_add(x,y,(cr*s)>>8,(cg*s)>>8,(cb*s)>>8);
    }
  }
}

static void wh_build(void){
  if(whBuilt) return;
  whBuilt=1;
  whStage=(uint32_t*)malloc(sizeof fb);
  wh_build_sprites();
  wh_build_ptr_rot();
  whFB[0].face=0; whFB[1].face=1;
  while(wh_face_step(&whFB[0])) {}             /* the SUPER face: see wh_build_more */
  wh_build_gloss();
  wh_build_rays();
  if(whStage) wh_build_stage();
  wh_bake_glass(&whPlate[0],WPLATE_W,WPLATE_H,LZ_HONEY,1.0f);
  wh_bake_glass(&whPlate[1],WPLATE_W,WPLATE_H,LZ_CYAN,1.0f);
  wh_bake_glass(&whPlate[2],WPLATE_W,WPLATE_H,LZ_MAGENTA,1.0f);
}

/* The reel symbol: a little lit wheel on a violet medallion. */
static void art_wheel(void){
  wh_build();
  cv_clear();
  cv_medal(0x24063C,0x6A1AA8,0xD8A0FF);
  const float cx=U(46), cy=U(42), R=U(29);
  cv_circle(cx+U(1.0f),cy+U(1.8f),R+U(4),0x000000,0x000000,130);
  pt_t p[48];
  for(int i=0;i<48;i++){ float a=TAU*i/48.0f; p[i].x=cx+cosf(a)*(R+U(3.6f)); p[i].y=cy+sinf(a)*(R+U(3.6f)); }
  cv_polyN(p,48,GOLDG,5,255);
  static const uint32_t wc[6][2]={{0xFF7A6A,0xB0101A},{0x8AB8FF,0x1A40C0},{0x90F0A0,0x10802C},
                                  {0xFFD890,0xE07010},{0xE0B0FF,0x6A20B0},{0xA0F6FF,0x0A90A8}};
  for(int i=0;i<12;i++){
    float a0=TAU*i/12.0f, a1=TAU*(i+1)/12.0f;
    pt_t w[4]={{cx,cy},{cx+sinf(a0)*R,cy-cosf(a0)*R},
               {cx+sinf((a0+a1)*0.5f)*R*1.02f,cy-cosf((a0+a1)*0.5f)*R*1.02f},
               {cx+sinf(a1)*R,cy-cosf(a1)*R}};
    int k=(i==0)?-1:(i%6);
    if(k<0) cv_poly(w,4,0xFFF4C0,0xD89A18,255);          /* the gold top wedge */
    else    cv_poly(w,4,wc[k][0],wc[k][1],255);
  }
  for(int i=0;i<12;i++){                                  /* dividers + pegs   */
    float a=TAU*i/12.0f, ca=sinf(a), sa=-cosf(a);
    pt_t d[4]={{cx+ca*U(6)-sa*U(0.5f),cy+sa*U(6)+ca*U(0.5f)},{cx+ca*R-sa*U(0.5f),cy+sa*R+ca*U(0.5f)},
               {cx+ca*R+sa*U(0.5f),cy+sa*R-ca*U(0.5f)},{cx+ca*U(6)+sa*U(0.5f),cy+sa*U(6)-ca*U(0.5f)}};
    cv_poly(d,4,0xFFF0B0,0xC08A10,255);
    cv_sphere(cx+ca*R*0.90f,cy+sa*R*0.90f,U(1.5f),0x5A3A00,0xE8B830,0xFFFBE0);
  }
  cv_ellipse(cx,cy-R*0.45f,R*0.75f,R*0.35f,0xFFFFFF,0xFFFFFF,50);   /* gloss */
  cv_sphere(cx,cy,U(7.5f),0x5A0008,0xD01828,0xFFB0A0);             /* hub   */
  cv_text("7",cx+U(0.3f),cy+U(0.4f),U(1.5f),0x3A0000,255);
  cv_text("7",cx,cy,U(1.5f),0xFFF0B0,255);
  pt_t ptr[3]={{cx-U(5.5f),cy-R-U(6.5f)},{cx+U(5.5f),cy-R-U(6.5f)},{cx,cy-R+U(6)}};
  cv_poly_outline(ptr,3,U(1.6f),0x1A0C00,255);
  cv_polyN(ptr,3,GOLDG,5,255);
  ribbon(78,48,"WHEEL",0xFFE9A8,0xC08A10,0x3A1400);
}

/* ---- FLOW (update code: the only place state changes) ---- */
static void wh_cache_update(const wheel_state_t*W);

/* velocity profile of the main spin: a fast kick, then a long, slowing
   crawl whose last pegs take seconds each                            */
static float wh_shape(float u){
  float k=clampf(u/0.06f,0,1); k=k*k*(3.0f-2.0f*k);
  float d=1.0f-u; if(d<0) d=0;
  return k*d*sqrtf(d);
}
static float wh_phi(float ang){ float p=fmodf(-ang,360.0f); if(p<0) p+=360.0f; return p; }
static int   wh_wedge_at(float ang){ int w=(int)(wh_phi(ang)/WH_WDEG); return w>=WH_NW?WH_NW-1:w; }

static void wh_label(int face,int w,char*b,size_t n){
  const wedge_t*e=&wh_table(face)[w];
  if(e->kind==WK_UP) snprintf(b,n,"SUPER WHEEL");
  else if(e->kind==WK_POT) snprintf(b,n,"%s JACKPOT",JP_NAME[e->val]);
  else snprintf(b,n,"%d X BET",e->val);
}

static void wheel_begin(void){
  wheel_state_t*W=&G.wheel;
  float keep=wh_phi(-W->ang);                  /* the wheel stays where it stopped */
  if(keep==0.0f) keep=360.0f-WH_WDEG*0.5f;     /* a fresh machine shows SUPER on top */
  memset(W,0,sizeof *W);
  W->ang=keep; W->landed=-1; W->potTier=-1;
  W->lastPeg=wh_wedge_at(keep);
  G.state=ST_WHEEL; G.t=0;
  fx_transition("WHEEL OF 7's","THREE WHEELS  -  SPIN FOR PRIZES AND JACKPOTS",0xFFD24A);
  static const float n[6]={523,659,784,1046,1318,1568};
  for(int i=0;i<6;i++) snd_at(i*0.07f,n[i],n[i],0.16f,1,0.16f);
  snd_chord(0.46f,1046,1318,1568,0.9f,0.18f);
  snd_at(0.46f,262,262,0.9f,2,0.10f);
  snd_noise_at(0.0f,0.25f,0.12f,2600);
}

static void wh_start(wheel_state_t*W){
  W->target=wheel_pick(W->face);
  /*  Where in the wedge the pointer will rest: anywhere but on the pegs
   *  themselves.  Near 0 it rests bent against the next peg, one tick
   *  from the neighbour - the agonising one.                          */
  float f=0.06f+0.88f*frnd();
  float rest=W->ang;
  W->ang0=rest-WH_WIND;
  float want=-(W->target+f)*WH_WDEG;           /* wheel angle that puts it there */
  float need=fmodf(want-W->ang0,360.0f); if(need<0) need+=360.0f;
  W->dAng=need+360.0f*(W->face?5:4);
  float dur=W->face?(7.2f+1.4f*frnd()):(6.0f+1.4f*frnd());
  W->nFrames=(int)(dur/DT+0.5f);
  float s=0;
  for(int i=1;i<=W->nFrames;i++) s+=wh_shape((i-0.5f)/W->nFrames);
  W->vSum=s>0.0001f?s:1.0f;
  W->frame=0; W->landed=-1; W->spins++; W->tense=0;
  W->phase=WPH_WIND; W->t=0;
  snd(150,90,0.45f,2,0.10f);                   /* the wind-up creak */
  snd_noise(0.35f,0.05f,600);
}

static void wh_tick(wheel_state_t*W){
  float sp=fabsf(W->vel);
  float v=0.20f-clampf(sp/10.0f,0,1)*0.13f;
  float p=2500.0f+(float)(wh_hash((uint32_t)W->lastPeg*31u+(uint32_t)W->spins)&511);
  snd(p,p*0.62f,0.022f,0,v*0.55f);
  snd_noise(0.020f,v,sp<2.0f?5200.0f:7000.0f);
  if(sp<1.2f) snd(170,110,0.05f,2,v*0.5f);      /* the slow ones clunk */
  /* in the crawl every clack also rings a semitone higher than the last */
  if(W->phase==WPH_SPIN && sp<2.2f){
    int n=W->tense<14?W->tense:14; W->tense++;
    float f=392.0f*powf(1.0595f,(float)n);
    snd(f,f,0.16f,1,0.07f);
    snd(f*2.0f,f*2.0f,0.10f,0,0.02f);
  }
}

/* the flapper: pegs push it, a spring swings it back */
static void wh_pointer(wheel_state_t*W){
  float phi=wh_phi(W->ang);
  int peg=(int)(phi/WH_WDEG); if(peg>=WH_NW) peg=WH_NW-1;
  if(peg!=W->lastPeg){ W->lastPeg=peg; wh_tick(W); }
  float fr=phi-peg*WH_WDEG;                    /* degrees above the peg below */
  float push=0;
  if(W->phase==WPH_WIND){                      /* turning back: the other side */
    float d=WH_WDEG-fr; if(d<WH_CONTACT) push=-WH_PMAX*(1.0f-d/WH_CONTACT);
  } else if(fr<WH_CONTACT) push=WH_PMAX*(1.0f-fr/WH_CONTACT);
  for(int i=0;i<4;i++){
    const float h=DT*0.25f;
    W->ptrV += (-900.0f*W->ptr - 7.0f*W->ptrV)*h;
    W->ptr  += W->ptrV*h;
    if(push>0 && W->ptr<push){ W->ptr=push; if(W->ptrV<0) W->ptrV=0; }
    if(push<0 && W->ptr>push){ W->ptr=push; if(W->ptrV>0) W->ptrV=0; }
  }
}

static void wh_land(wheel_state_t*W){
  W->ang=wh_phi(-W->ang);
  W->landed=wh_wedge_at(W->ang);   /* == target: the stop was built for it */
  W->phase=WPH_LANDED; W->t=0;
  const wedge_t*e=&wh_table(W->face)[W->landed];
  G.flash=opt_limiter?0.25f:0.5f;
  fx_burst((float)WH_CX,(float)(WH_CY-WH_RF+30),24,FXK_STAR);
  snd_noise(0.08f,0.14f,3000);
  if(e->kind==WK_POT || e->kind==WK_UP || e->val>=50){
    snd_chord(0.05f,784,988,1175,0.5f,0.16f);
    snd_chord(0.30f,1046,1318,1568,0.8f,0.18f);
  } else {
    snd_chord(0.05f,1046,1318,1568,0.5f,0.15f);
  }
}

static void wh_prize_begin(wheel_state_t*W){
  const wedge_t*e=&wh_table(W->face)[W->landed];
  W->prize=wh_take(W->face,W->landed);
  W->potTier = e->kind==WK_POT ? e->val : -1;
  W->shown=0;
  char title[40], sub[64], num[32];
  commas(num,sizeof num,W->prize);
  if(W->potTier>=0){
    snprintf(title,sizeof title,"%s JACKPOT",JP_NAME[W->potTier]);
    snprintf(sub,sizeof sub,"%s CREDITS",num);
    sfx_jackpot(W->potTier);
    fx_shake(W->potTier==JP_MEGA?14.0f:8.0f,1.0f);
    fx_transition(title,sub,W->potTier==JP_MEGA?0xFF8AC8:0xFFD24A);
  } else {
    int x=e->val;
    snprintf(title,sizeof title,"%s",x>=200?"MONSTER WIN":(x>=75?"HUGE WIN":(x>=25?"BIG WIN":"WINNER")));
    snprintf(sub,sizeof sub,"%d X BET  =  %s CREDITS",x,num);
    fx_transition(title,sub,0xFFD24A);
    static const float n[4]={784,988,1175,1568};
    int m=x>=75?4:(x>=25?3:2);
    for(int i=0;i<m;i++) snd_at(i*0.09f,n[i],n[i],0.18f,1,0.16f);
    snd_chord(m*0.09f,1046,1318,1568,0.8f,0.17f);
    if(x>=25) fx_shake(5.0f,0.5f);
  }
  fx_fountain((float)WH_CX,560.0f,2.5f);
  W->phase=WPH_PRIZE; W->t=0;
}

static void wheel_update(void){
  wheel_state_t*W=&G.wheel;
  W->t+=DT;
  W->anim+=DT*(1.0f+fabsf(W->vel)*0.55f);
  float before=W->ang;
  switch(W->phase){
  case WPH_INTRO:
    if(W->t>1.0f && !fx_transition_busy()){ W->phase=WPH_READY; W->t=0; }
    break;
  case WPH_READY:
    if((W->t>0.25f && (hit(B_A)||hit(B_START)||hit(B_B))) || W->t>(W->face?3.5f:6.0f))
      wh_start(W);
    break;
  case WPH_WIND: {
    float u=clampf(W->t/WH_WINDT,0,1);
    W->ang=W->ang0+WH_WIND-WH_WIND*sinf(u*TAU*0.25f);
    if(W->t>=WH_WINDT+0.06f){
      W->phase=WPH_SPIN; W->t=0; W->frame=0;
      snd_noise(0.5f,0.16f,1500);                     /* the kick */
      snd(160,700,0.40f,1,0.10f);
    }
    break; }
  case WPH_SPIN:
    W->frame++;
    W->ang+=W->dAng*wh_shape((W->frame-0.5f)/W->nFrames)/W->vSum;
    if(W->frame>=W->nFrames){ W->ang=W->ang0+W->dAng; wh_land(W); }
    break;
  case WPH_LANDED:
    if(W->t>1.5f){
      if(wh_table(W->face)[W->landed].kind==WK_UP){
        W->face=1; W->phase=WPH_UPGRADE; W->t=0; W->landed=-1;
        G.flash=opt_limiter?0.35f:0.8f;
        fx_transition("SUPER WHEEL","BIGGER WEDGES  -  THE MEGA JACKPOT IS ON IT",0xFF8AC8);
        fx_shake(10.0f,0.6f);
        for(int i=0;i<10;i++) snd_at(i*0.05f,400.0f*powf(1.19f,(float)i),400.0f*powf(1.19f,(float)i+1),0.07f,1,0.12f);
        snd_chord(0.55f,1175,1480,1760,1.0f,0.2f);
        snd_at(0.55f,147,147,1.0f,2,0.11f);
      } else wh_prize_begin(W);
    }
    break;
  case WPH_UPGRADE:
    if(W->t>1.0f && !fx_transition_busy()){ W->phase=WPH_READY; W->t=0; }
    break;
  case WPH_PRIZE: {
    if(fx_transition_busy() && W->t<3.0f) break;
    if(W->shown<W->prize){
      long long step=1+W->prize/100;
      W->shown+=step; if(W->shown>W->prize) W->shown=W->prize;
      if(((int)(W->t*60))%3==0) snd(1300.0f+irnd(300),1800,0.04f,0,0.05f);
      if(((int)(W->t*60))%16==0) fx_burst(WH_CX+(frnd()-0.5f)*420.0f,470.0f+frnd()*80.0f,10,FXK_COIN);
      W->hot=W->t;                                    /* when the roll finished */
    } else if((W->t-W->hot>1.0f && anyhit()) || W->t-W->hot>4.0f){
      award(W->prize);
      W->phase=WPH_DONE;
      feature_done();
      return;
    }
    break; }
  }
  /* only a turning wheel has speed: landing renormalises the angle */
  W->vel=(W->phase==WPH_WIND||W->phase==WPH_SPIN)?W->ang-before:0.0f;
  wh_pointer(W);
  /* The SUPER face builds a few rows a frame while the first wheel runs
     (done in ~5 s; an upgrade needs ~10).  Should it be wanted sooner -
     a save state, say - the rest is finished on the spot.            */
  if(whFB[1].step<FB_DONE){
    int budget=W->face?(1<<30):6;
    while(budget>0){ int c=wh_face_step(&whFB[1]); if(!c) break; budget-=c; }
  }
  wh_cache_update(W);
}

/* ---- DRAW (reads state, writes pixels, changes nothing) ---- */

static inline uint32_t wh_bil(const uint32_t*p,unsigned fx,unsigned fy){
  uint32_t a=p[0],b=p[1],c=p[WH_FW],d=p[WH_FW+1];
  uint32_t ix=256-fx, iy=256-fy;
  uint32_t rb1=(((a&0xFF00FFu)*ix+(b&0xFF00FFu)*fx)>>8)&0xFF00FFu;
  uint32_t g1 =(((a&0x00FF00u)*ix+(b&0x00FF00u)*fx)>>8)&0x00FF00u;
  uint32_t rb2=(((c&0xFF00FFu)*ix+(d&0xFF00FFu)*fx)>>8)&0xFF00FFu;
  uint32_t g2 =(((c&0x00FF00u)*ix+(d&0x00FF00u)*fx)>>8)&0x00FF00u;
  return ((((rb1*iy+rb2*fy)>>8)&0xFF00FFu) | (((g1*iy+g2*fy)>>8)&0x00FF00u));
}
static inline uint32_t wh_lit(uint32_t c,uint32_t g){
  uint32_t m=g>>8, s=g&255u;
  uint32_t rb=(((c&0xFF00FFu)*m)>>8)&0xFF00FFu, gg=(((c&0x00FF00u)*m)>>8)&0x00FF00u;
  rb+=(((0xFF00FFu-rb)*s)>>8)&0xFF00FFu;
  gg+=(((0x00FF00u-gg)*s)>>8)&0x00FF00u;
  return rb|gg;
}

/* one run of face pixels, x in [xa,xb] of screen row y, into `row`
   (addressed by screen x: the framebuffer row, or the cache's)        */
static void wh_span(uint32_t*row,const uint16_t*gl,const uint32_t*src,int nearest,
                    int y,int xa,int xb,float c,float s){
  if(xb<xa) return;
  float dy=y+0.5f-WH_CY, dx=xa+0.5f-WH_CX;
  int32_t U=(int32_t)((c*dx+s*dy+WH_FC-0.5f)*65536.0f);
  int32_t V=(int32_t)((-s*dx+c*dy+WH_FC-0.5f)*65536.0f);
  int32_t du=(int32_t)(c*65536.0f), dv=(int32_t)(-s*65536.0f);
  if(nearest){
    U+=32768; V+=32768;
    for(int x=xa;x<=xb;x++,U+=du,V+=dv)
      row[x]=wh_lit(src[(size_t)(V>>16)*WH_FW+(U>>16)],gl[x]);
  } else {
    for(int x=xa;x<=xb;x++,U+=du,V+=dv)
      row[x]=wh_lit(wh_bil(src+(size_t)(V>>16)*WH_FW+(U>>16),(unsigned)(U>>8)&255u,(unsigned)(V>>8)&255u),gl[x]);
  }
}

/*  The turning face, rows [y0,y1) into a buffer whose row y, column x is
 *  base[(y-oy)*stride + (x-ox)].  One span per row, clipped to the disc;
 *  the hub's footprint is skipped (it is drawn over); the disc's edge
 *  pixels are blended over what is there by coverage.                 */
static void wh_disc_rows(uint32_t*base,int stride,int ox,int oy,int y0,int y1,
                         int face,float vel,float ang){
  if(!whGloss) return;
  float sp=fabsf(vel);
  int lvl = sp>3.2f?2:(sp>0.9f?1:0);
  const uint32_t*src=whFace[face][lvl];
  if(!src) src=whFace[face][0];
  if(!src) return;
  float a=ang*(TAU/360.0f), c=cosf(a), s=sinf(a);
  if(y0<WH_CY-WH_RF) y0=WH_CY-WH_RF;
  if(y1>WH_CY+WH_RF) y1=WH_CY+WH_RF;
  const float R=(float)WH_RF, HR=WH_RHUB-3.0f;
  for(int y=y0;y<y1;y++){
    float dy=y+0.5f-WH_CY;
    float h2=R*R-dy*dy; if(h2<=0) continue;
    float hw=sqrtf(h2);
    int xa=(int)ceilf(WH_CX-hw+0.5f), xb=(int)floorf(WH_CX+hw-1.5f);   /* fully inside */
    uint32_t*row=base+(long)(y-oy)*stride-ox;
    const uint16_t*gl=whGloss+(size_t)(y-(WH_CY-WH_RF-1))*WH_GW-(WH_CX-WH_RF-1);
    float g2=HR*HR-dy*dy;
    if(g2>0){
      float gw=sqrtf(g2);
      int ga=(int)ceilf(WH_CX-gw), gb=(int)floorf(WH_CX+gw)-1;
      wh_span(row,gl,src,lvl>0,y,xa,ga-1,c,s);
      wh_span(row,gl,src,lvl>0,y,gb+1,xb,c,s);
    } else wh_span(row,gl,src,lvl>0,y,xa,xb,c,s);
    /* antialiased rim: two pixels either end, blended by coverage */
    for(int k=0;k<4;k++){
      int x = k<2 ? xa-1-k : xb+1+(k-2);
      float dx=x+0.5f-WH_CX, r=sqrtf(dx*dx+dy*dy);
      int cov=(int)(clampf(R-r+0.5f,0,1)*256.0f);
      if(cov<=0) continue;
      int32_t U=(int32_t)((c*dx+s*dy+WH_FC)*65536.0f), V=(int32_t)((-s*dx+c*dy+WH_FC)*65536.0f);
      uint32_t col=wh_lit(src[(size_t)(V>>16)*WH_FW+(U>>16)],gl[x]), d=row[x];
      uint32_t rb=((((col&0xFF00FFu)*cov)+((d&0xFF00FFu)*(256-cov)))>>8)&0xFF00FFu;
      uint32_t gg=((((col&0x00FF00u)*cov)+((d&0x00FF00u)*(256-cov)))>>8)&0x00FF00u;
      row[x]=rb|gg;
    }
  }
}

/*  At rest the rotated face does not change from frame to frame, and the
 *  wheel spends most of the feature at rest.  So update code renders the
 *  stage-plus-disc square once whenever the wheel settles somewhere new,
 *  and the draw just copies it.  Cosmetic, so not in the save state.  */
#define WH_CX0 (WH_CX-WH_RF-1)
#define WH_CY0 (WH_CY-WH_RF-1)
static uint32_t *whCache;
static int   whCacheOk, whCacheFace, whCacheHot;
static float whCacheAng;

/* the wedge that won, if the wheel is showing one (after an upgrade the
   new face is spun from scratch, so nothing is lit on it)             */
static int wh_hot(const wheel_state_t*W){
  return (W->phase>=WPH_LANDED && W->phase!=WPH_UPGRADE) ? W->landed : -1;
}

/*  Once it has stopped, every wedge but the winner drops into shadow:
 *  a spotlight on the prize.  Baked into the cache, so it is free.    */
static void wh_cache_spot(int landed,float ang){
  float a0=(landed*WH_WDEG+ang)*(TAU/360.0f), a1=a0+WH_WDEG*(TAU/360.0f);
  float n0x=cosf(a0), n0y=sinf(a0), n1x=-cosf(a1), n1y=-sinf(a1);
  const float R2=(WH_RF+1.0f)*(WH_RF+1.0f);
  for(int j=0;j<WH_GW;j++){
    float dy=WH_CY0+j+0.5f-WH_CY;
    uint32_t*row=whCache+(size_t)j*WH_GW;
    for(int i=0;i<WH_GW;i++){
      float dx=WH_CX0+i+0.5f-WH_CX;
      if(dx*dx+dy*dy>R2) continue;
      float s=fminf(dx*n0x+dy*n0y,dx*n1x+dy*n1y);
      uint32_t k=(uint32_t)(256.0f*(0.40f+0.60f*clampf(s+0.5f,0,1)));
      if(k>=256) continue;
      uint32_t c=row[i];
      row[i]=((((c&0xFF00FFu)*k)>>8)&0xFF00FFu)|((((c&0x00FF00u)*k)>>8)&0x00FF00u);
    }
  }
}

static void wh_cache_update(const wheel_state_t*W){
  if(W->vel!=0.0f || W->phase==WPH_WIND || W->phase==WPH_SPIN || !whStage) return;
  int hot=wh_hot(W);
  if(whCacheOk && whCacheFace==W->face && whCacheAng==W->ang && whCacheHot==hot) return;
  if(!whCache) whCache=(uint32_t*)malloc((size_t)WH_GW*WH_GW*4);
  if(!whCache) return;
  for(int j=0;j<WH_GW;j++)
    memcpy(whCache+(size_t)j*WH_GW,whStage+(size_t)(WH_CY0+j)*FBW+WH_CX0,(size_t)WH_GW*4);
  wh_disc_rows(whCache,WH_GW,WH_CX0,WH_CY0,WH_CY0,WH_CY0+WH_GW,W->face,0.0f,W->ang);
  if(hot>=0) wh_cache_spot(hot,W->ang);
  whCacheOk=1; whCacheFace=W->face; whCacheAng=W->ang; whCacheHot=hot;
}

static void wh_disc(void){
  const wheel_state_t*W=&G.wheel;
  if(whCacheOk && W->vel==0.0f && whCacheFace==W->face && whCacheAng==W->ang && whCacheHot==wh_hot(W)){
    int y0=WH_CY0>clip_y0?WH_CY0:clip_y0, y1=WH_CY0+WH_GW<clip_y1?WH_CY0+WH_GW:clip_y1;
    for(int y=y0;y<y1;y++)
      memcpy(fb+(size_t)y*FBW+WH_CX0,whCache+(size_t)(y-WH_CY0)*WH_GW,(size_t)WH_GW*4);
    return;
  }
  wh_disc_rows(fb,FBW,0,0,clip_y0,clip_y1,W->face,W->vel,W->ang);
}

/* additive glow over the landed wedge, which now sits under the pointer */
static void wh_hot_wedge(float k,uint32_t col){
  const wheel_state_t*W=&G.wheel;
  if(W->landed<0 || k<=0.0f) return;
  float a0=(W->landed*WH_WDEG+W->ang)*(TAU/360.0f);
  float a1=a0+WH_WDEG*(TAU/360.0f);
  /* inward normals of the two dividers */
  float n0x=cosf(a0), n0y=sinf(a0), n1x=-cosf(a1), n1y=-sinf(a1);
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  for(int y=WH_CY-WH_RF;y<WH_CY-WH_RTRIM+4;y++){
    if(y<clip_y0||y>=clip_y1) continue;
    float dy=y+0.5f-WH_CY;
    int span=(int)(-dy*0.28f)+6;
    for(int x=WH_CX-span;x<=WH_CX+span;x++){
      float dx=x+0.5f-WH_CX;
      float s0=dx*n0x+dy*n0y, s1=dx*n1x+dy*n1y;
      float cov=clampf(fminf(s0,s1)+0.5f,0,1);
      if(cov<=0) continue;
      float r=sqrtf(dx*dx+dy*dy);
      if(r<WH_RTRIM || r>WH_RF) continue;
      float e=clampf(fminf(s0,s1)/10.0f,0,1);
      float f=k*cov*(0.35f+0.65f*(1.0f-e))*(0.5f+0.5f*r/WH_RF);
      fb_add(x,y,(int)(cr*f),(int)(cg*f),(int)(cb*f));
    }
  }
}

/* a small additive star: a soft core and a cross */
static void wh_star(int x,int y,int r,uint32_t col,float k){
  if(k<=0.01f) return;
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  for(int j=-2;j<=2;j++) for(int i=-2;i<=2;i++){
    float f=k*(1.0f-(i*i+j*j)/9.0f);
    if(f>0) fb_add(x+i,y+j,(int)(cr*f),(int)(cg*f),(int)(cb*f));
  }
  for(int d=3;d<=r;d++){
    float f=k*0.8f*(1.0f-(float)d/(r+1));
    int a=(int)(cr*f), b=(int)(cg*f), c=(int)(cb*f);
    fb_add(x+d,y,a,b,c); fb_add(x-d,y,a,b,c); fb_add(x,y+d,a,b,c); fb_add(x,y-d,a,b,c);
  }
}

/*  Lights marching round the winning wedge: out along one divider,
 *  across the rim, back down the other.                               */
static void wh_chaser(int w){
  const wheel_state_t*W=&G.wheel;
  const float r0=86.0f, r1=232.0f, dA=WH_WDEG*(TAU/360.0f);
  float a0=(w*WH_WDEG+W->ang)*(TAU/360.0f);
  float side=r1-r0, arc=r1*dA, L=2.0f*side+arc;
  const int N=16;
  float off=fmodf(W->anim*(opt_limiter?70.0f:140.0f),L/N);
  for(int i=0;i<N;i++){
    float s=off+i*(L/N), a, r;
    if(s<side){ a=a0; r=r0+s; }
    else if(s<side+arc){ a=a0+(s-side)/r1; r=r1; }
    else { a=a0+dA; r=r1-(s-side-arc); }
    /* pulled a few pixels inside the wedge so they sit on its face */
    float in=4.0f, ca=cosf(a), sa=sinf(a);
    float x=WH_CX+sa*r, y=WH_CY-ca*r;
    if(s<side){ x+=ca*in; y+=sa*in; }
    else if(s>=side+arc){ x-=ca*in; y-=sa*in; }
    else { x-=sa*in; y+=ca*in; }
    wh_star((int)x,(int)y,6,0xFFF0C0,0.85f);
  }
}

/*  The pot and SUPER wedges glitter: a few stars on each that turn
 *  with the wheel and wink in and out.                                */
static void wh_twinkles(void){
  const wheel_state_t*W=&G.wheel;
  const wedge_t*T=wh_table(W->face);
  for(int k=0;k<WH_NW;k++){
    if(T[k].kind==WK_CR) continue;
    uint32_t col = T[k].kind==WK_UP ? 0xFFE0A0 : (T[k].val==JP_MEGA ? 0xFFFFFF : (T[k].val==JP_MAJOR?0xFFF4C0:0xE0F0FF));
    for(int i=0;i<6;i++){
      uint32_t h=wh_hash((uint32_t)(k*977+i*131+W->face*7919));
      float r=96.0f+(float)(h%128u);
      float a=(k*WH_WDEG+1.5f+(float)((h>>8)%1200u)/100.0f+W->ang)*(TAU/360.0f);
      float ph=W->anim*(2.0f+(float)((h>>16)&3))+(float)((h>>20)&63)*0.1f;
      float v=sinf(ph); if(v<=0) continue;
      v=v*v*v*v;
      wh_star((int)(WH_CX+sinf(a)*r),(int)(WH_CY-cosf(a)*r),5,col,v*(opt_limiter?0.55f:0.9f));
    }
  }
}

/* how lit bulb k is right now */
static float wh_bulb(int k){
  const wheel_state_t*W=&G.wheel;
  float t=W->anim, v;
  switch(W->phase){
  case WPH_WIND: case WPH_SPIN:
    v=0.5f+0.5f*sinf(t*7.0f-k*0.785f); v=v*v; break;
  case WPH_LANDED: case WPH_PRIZE: case WPH_UPGRADE: case WPH_DONE: {
    int on=(((int)(t*(opt_limiter?4.0f:8.0f))+k)&1);
    v=on?1.0f:0.22f; break; }
  default:
    v=0.5f+0.5f*sinf(t*3.0f-k*0.39f); v=0.25f+0.75f*v*v; break;
  }
  if(opt_limiter) v=0.25f+v*0.75f;
  return v;
}

static void wh_bulbs(void){
  const wheel_state_t*W=&G.wheel;
  for(int k=0;k<WH_NBULB;k++){
    float v=wh_bulb(k);
    float a=(k+0.5f)*TAU/WH_NBULB;
    int bx=(int)lrintf(WH_CX+sinf(a)*WH_RBULB), by=(int)lrintf(WH_CY-cosf(a)*WH_RBULB);
    uint32_t col = W->face ? wh_hsv(k/(float)WH_NBULB+W->anim*0.05f,0.65f,1.0f)
                           : ((k&1)?0xFFC848:0xFF48B8);      /* honey and magenta */
    blit(&whBulb,bx-10,by-10,clip_y0,clip_y1,(int)(v*255),col,0.35f);
    int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255, vi=(int)(v*0.6f*256.0f);
    for(int j=0;j<WH_HALO;j++){
      int y=by-WH_HALO/2+j;
      if(y<clip_y0||y>=clip_y1) continue;
      for(int i=0;i<WH_HALO;i++){
        int f=(whHalo[j*WH_HALO+i]*vi)>>8; if(f<=2) continue;
        fb_add(bx-WH_HALO/2+i,y,(cr*f)>>8,(cg*f)>>8,(cb*f)>>8);
      }
    }
  }
}

/* a caption in small spaced capitals, centred on cx, top at y */
static void wh_spaced(const char*s,float cx,int y,uint32_t col){
  lz_style st; memset(&st,0,sizeof st);
  st.color=col; st.align=LZ_CENTER; st.spacing=2.0f;
  st.shadow=0x000000; st.shadow_k=0.8f;
  lz_text_ex(LZF_UI_S,s,cx,(float)y,16.0f,&st);
}
/* a gold readout centred on cx, caps' top at y and h tall (lz_readout's
   look, which only right-aligns) */
static void wh_count(long long v,float cx,int y,float h,uint32_t col){
  char s[32];
  commas(s,sizeof s,v<0?0:v);
  int f=lz_disp_font(h);
  const lz_font*F=&lzf[f];
  float size=h/F->capH*F->base, k=size/F->base;
  lz_style st; memset(&st,0,sizeof st);
  st.align=LZ_CENTER;
  st.color=mixc(col,0xFFFFFF,0.55f); st.color2=col; st.grad=1;
  st.glow=col; st.glow_k=0.35f;
  st.outline=0x000000; st.outline_px=size*0.035f;
  lz_text_ex(f,s,cx,y-F->capTop*k,size,&st);
}

static void wheel_draw(void){
  const wheel_state_t*W=&G.wheel;
  char b[64];
  /* the stage */
  if(whStage){
    int y0=clip_y0<0?0:clip_y0, y1=clip_y1>FBH?FBH:clip_y1;
    if(y1>y0) memcpy(fb+(size_t)y0*FBW,whStage+(size_t)y0*FBW,(size_t)(y1-y0)*FBW*4);
  }
  /* the live wheel's tab, lit: its copy from init goes over the stage's
     unlit one, and a breathing rim over that */
  if(whTabLit[W->face]){
    const int w=WTAB_X1-WTAB_X0, ty=WTAB_Y(W->face)-WTAB_M;
    int y0=ty>clip_y0?ty:clip_y0, y1=ty+WTAB_RH<clip_y1?ty+WTAB_RH:clip_y1;
    for(int y=y0;y<y1;y++)
      memcpy(fb+(size_t)y*FBW+WTAB_X0,whTabLit[W->face]+(size_t)(y-ty)*w,(size_t)w*4);
    float pl=0.5f+0.5f*sinf(W->anim*4.0f);
    fb_rframe(WTAB_X-3,WTAB_Y(W->face)-1,WTAB_W+6,WTAB_H+6,16,2.0f,
              lz_hot(WTAB_NEON[W->face],0.5f),(int)(pl*120));
  }
  wh_disc();
  /* big prizes and every pot get rays fanning out behind the wheel (the
     rays live outside the rim, so they go on after the cached square) */
  if(wh_hot(W)>=0){
    const wedge_t*e=&wh_table(W->face)[W->landed];
    if(e->kind!=WK_CR || e->val>=25){
      float k = W->phase==WPH_LANDED ? clampf(W->t*1.5f,0,1)*0.6f : clampf(0.6f+W->t,0,1);
      k*=opt_limiter?0.45f:0.75f;
      uint32_t rc = e->kind==WK_POT ? (e->val==JP_MEGA ? wh_hsv(W->anim*0.15f,0.55f,1.0f)
                                                       : (e->val==JP_MAJOR?0xFFC040:0x9AD0FF))
                                    : (e->kind==WK_UP ? 0xFF8AC8 : 0xFFD060);
      wh_rays(W->anim*0.05f,rc,k);
    }
  }
  /* the winner glows, and lights run round its border */
  if(wh_hot(W)>=0){
    float pl=0.5f+0.5f*sinf(W->anim*(opt_limiter?5.0f:10.0f));
    const wedge_t*e=&wh_table(W->face)[W->landed];
    uint32_t hc = e->kind==WK_POT ? (e->val==JP_MEGA?0xFF80E0:0xFFD060) : 0xFFE8A0;
    wh_hot_wedge((0.20f+0.45f*pl)*(opt_limiter?0.6f:1.0f),hc);
    wh_chaser(W->landed);
  }
  if(fabsf(W->vel)<1.0f) wh_twinkles();
  /* pegs: sprites at rest; at speed they are in the blurred face */
  for(int k=0;k<WH_NW && fabsf(W->vel)<=0.9f;k++){
    float a=(k*WH_WDEG+W->ang)*(TAU/360.0f);
    int px=(int)lrintf(WH_CX+sinf(a)*WH_RPEG), py=(int)lrintf(WH_CY-cosf(a)*WH_RPEG);
    blit(&whPeg,px-8,py-8,clip_y0,clip_y1,255,0,0.0f);
  }
  blit(&whHub,WH_CX-70,WH_CY-70,clip_y0,clip_y1,255,0,0.0f);
  wh_bulbs();
  /* the flapper, its shadow on the face first */
  {
    int i=(int)lrintf((-W->ptr+WH_PMAX)/WH_PSTEP);
    i=clampi(i,0,WH_PN-1);
    const spr_t*ps=&whPtrRot[i];
    int px=WH_CX-whPtrPiv[i][0], py=WH_PIVY-whPtrPiv[i][1];
    blit_wash(ps,px+7,py+9,clip_y0,clip_y1,0x000000,0.45f);
    blit(ps,px,py,clip_y0,clip_y1,255,0,0.0f);
  }
  blit(&whKnob,WH_CX-16,WH_PIVY-16,clip_y0,clip_y1,255,0,0.0f);

  /* left panel: the live pots, in gold; the one being paid flashes */
  for(int i=0;i<3;i++){
    int tier=JP_MEGA+i, y=WJ_ROW(i)+22;
    int hot=(W->phase>=WPH_PRIZE && W->potTier==tier);
    uint32_t on=LZ_GOLD;
    if(hot){ float pl=0.5f+0.5f*sinf(W->anim*12.0f); on=mixc(on,0xFFFFFF,pl*0.6f); }
    long long v=hot?W->prize:jp_value(tier);
    seg_num(v,WPL_X+WCOL_W-24,y+9,11,11,26,on,0,1);
  }
  /* right panel: bet and win */
  seg_num(TOTBET,WPR_X+WCOL_W-28,WB_Y+39,8,14,30,LZ_GOLD,0,1);
  {
    long long w=W->phase>=WPH_PRIZE?W->shown:0;
    uint32_t wc=w>0?mixc(LZ_GOLD,0xFFF6D0,0.5f+0.5f*sinf(W->anim*9.0f)):0x6A4A10;
    seg_num(w,WPR_X+WCOL_W-28,WW_Y+41,10,15,42,wc,0,1);
  }
  /* the status panel: what the wheel wants, or what it gave */
  const float cy=(float)WS_Y;
  int blink=((int)(W->anim*2.5f))&1;
  switch(W->phase){
  case WPH_READY: {
    /* the panel icon with the SPIN buttons lit, and a neon PRESS SPIN
       that dims to cold glass on the off beat rather than vanishing */
    const float ih=30.0f;
    lz_icon(WCX_R-lz_icon_w(ih)*0.5f,cy+34,ih,wp_mask(B_A|B_START),LZ_GOLD,255);
    lz_neon(LZF_NEON_L,"PRESS SPIN",WCX_R,cy+110,wh_fit(LZF_NEON_L,"PRESS SPIN",50,WCOL_W-36),
            LZ_GREEN,blink?1.0f:0.3f,0.9f);
    lz_text_sh(LZF_UI_M,"TO SPIN THE WHEEL",WCX_R,cy+140,24,LZ_IVORY,LZ_CENTER);
    float lim=W->face?3.5f:6.0f, k=clampf(1.0f-W->t/lim,0,1);
    int by=(int)cy+196;
    fb_rrect(WCX_R-120,by,240,10,5,0x000000,200);
    fb_rrect(WCX_R-120,by,(int)(240*k),10,5,LZ_GREEN,220);
    wh_spaced("AUTO SPIN",WCX_R,by+16,LZ_DIM);
    break; }
  case WPH_WIND: case WPH_SPIN:
    lz_neon(LZF_NEON_L,"GOOD LUCK!",WCX_R,cy+120,wh_fit(LZF_NEON_L,"GOOD LUCK!",50,WCOL_W-36),
            LZ_HONEY,0.75f+0.25f*sinf(W->anim*6.0f),0.9f);
    break;
  case WPH_UPGRADE:
    lz_neon(LZF_NEON_L,"SUPER WHEEL!",WCX_R,cy+120,wh_fit(LZF_NEON_L,"SUPER WHEEL!",50,WCOL_W-36),
            LZ_MAGENTA,0.7f+0.3f*sinf(W->anim*8.0f),1.0f);
    break;
  case WPH_LANDED: case WPH_PRIZE: case WPH_DONE:
    if(W->landed>=0){
      wh_label(W->face,W->landed,b,sizeof b);
      wh_spaced("THE WHEEL LANDS ON",WCX_R,(int)cy+46,LZ_DIM);
      lz_gold(LZF_DISP_M,b,WCX_R,cy+100,wh_fit(LZF_DISP_M,b,40,WCOL_W-40),1.0f,0.6f);
    }
    if(W->phase==WPH_PRIZE && W->prize>0 && W->shown>=W->prize){
      const float ih=24.0f;
      lz_icon(WCX_R-lz_icon_w(ih)*0.5f,cy+150,ih,wp_mask(B_A|B_START),LZ_GOLD,255);
      lz_neon(LZF_NEON_M,"PRESS SPIN TO COLLECT",WCX_R,cy+206,
              wh_fit(LZF_NEON_M,"PRESS SPIN TO COLLECT",30,WCOL_W-36),LZ_GREEN,blink?1.0f:0.3f,0.8f);
    }
    break;
  }
  if(W->spins>0 || W->face) {
    snprintf(b,sizeof b,W->face?"SUPER SPIN":"SPIN %d",W->spins<1?1:W->spins);
    wh_spaced(b,WCX_R,WS_Y+WS_H-34,W->face?lz_hot(LZ_MAGENTA,0.4f):LZ_DIM);
  }

  /* the prize plate, lounge glass with a neon edge in the prize's colour,
     once the title slam has cleared */
  if(W->phase>=WPH_PRIZE && W->prize>0 && !fx_transition_busy()){
    static const uint32_t PLC[3]={LZ_HONEY,LZ_CYAN,LZ_MAGENTA};
    int px=WH_CX-WPLATE_W/2, py=WPLATE_Y;
    int k=W->potTier==JP_MEGA?2:(W->potTier==JP_MINOR?1:0);
    blit(&whPlate[k],px-WPLATE_M,py-WPLATE_M,clip_y0,clip_y1,255,0,0.0f);
    wh_spaced(W->potTier>=0?"JACKPOT PAYS":(W->face?"THE SUPER WHEEL PAYS":"THE WHEEL PAYS"),
              WH_CX,py+12,lz_hot(PLC[k],0.35f));
    if(W->potTier>=0) snprintf(b,sizeof b,"%s JACKPOT",JP_NAME[W->potTier]);
    else snprintf(b,sizeof b,"%d X BET",wh_table(W->face)[W->landed].val);
    lz_gold(LZF_DISP_M,b,WH_CX,(float)py+60,wh_fit(LZF_DISP_M,b,46,WPLATE_W-60),1.0f,0.6f);
    float pl=0.5f+0.5f*sinf(W->anim*9.0f);
    uint32_t oc=W->shown>=W->prize?mixc(LZ_GOLD,0xFFF6D0,pl*0.6f):LZ_GOLD;
    wh_count(W->shown,(float)WH_CX,py+92,56,oc);
  }
  draw_parts();            /* coin bursts land over the stage, not under it */
  fx_draw();
}
