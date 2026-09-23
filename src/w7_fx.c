/* =====================================================================
 *  w7_fx.c - cosmetic effects.  STUB: thin wrappers over the core's
 *  original coin burst, to be replaced by the real effects system.
 * ===================================================================== */
static float fxTransT; static char fxTitle[40], fxSub[64]; static uint32_t fxCol;

static void fx_update(void){ if(fxTransT>0) fxTransT-=DT; }
static void fx_draw(void){}
static void fx_draw_top(void){
  if(fxTransT<=0) return;
  dim(150);
  textb(fxTitle,FBW/2,260,9,GOLDG,5,1);
  if(fxSub[0]) text(fxSub,FBW/2,380,3,fxCol,1,1);
}
static void fx_post(void){}
static void fx_burst(float x,float y,int n,int kind){
  static const uint32_t C[4]={0xFFD24A,0xFFFFFF,0xFFF0A0,0xFF5AA8};
  spawn_burst(x,y,n,C[kind&3]);
}
static void fx_fountain(float x,float y,float dur){ (void)dur; spawn_burst(x,y,24,0xFFD24A); }
static void fx_shake(float amp,float dur){ (void)amp; (void)dur; }
static void fx_transition(const char*title,const char*sub,uint32_t col){
  snprintf(fxTitle,sizeof fxTitle,"%s",title?title:"");
  snprintf(fxSub,sizeof fxSub,"%s",sub?sub:"");
  fxCol=col; fxTransT=1.2f;
}
static int fx_transition_busy(void){ return fxTransT>0; }
static void fx_glow(int cx,int cy,int r,uint32_t col,int alpha){
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  for(int y=-r;y<=r;y++) for(int x=-r;x<=r;x++){
    float d=sqrtf((float)(x*x+y*y))/r; if(d>=1.0f) continue;
    float v=(1.0f-d)*(1.0f-d)*alpha/255.0f;
    fb_add(cx+x,cy+y,(int)(cr*v),(int)(cg*v),(int)(cb*v));
  }
}
static void fx_rays(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,int alpha){
  int cr=(col>>16)&255, cg=(col>>8)&255, cb=col&255;
  for(int i=0;i<n;i++){
    float a=ang+i*(TAU/n), ca=cosf(a), sa=sinf(a);
    for(int d=r0;d<r1;d++){
      float f=(1.0f-(float)(d-r0)/(r1-r0))*alpha/255.0f;
      fb_add(cx+(int)(ca*d),cy+(int)(sa*d),(int)(cr*f),(int)(cg*f),(int)(cb*f));
    }
  }
}
