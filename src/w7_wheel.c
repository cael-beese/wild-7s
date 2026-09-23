/* w7_wheel.c - WHEEL bonus.  STUB: never triggers. */
static void art_wheel(void){
  cv_medal(0x3A0A40,0xB030C0,0xFFB0FF);
  cv_text("W",U(46),U(46),U(5),0xFFFFFF,255);
}
static int  wheel_triggered(void){ return 0; }
static void wheel_begin(void){ feature_done(); }
static void wheel_update(void){}
static void wheel_draw(void){}
static long long wheel_sim_play(void){ return 0; }
