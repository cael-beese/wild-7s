/* w7_hold.c - HOLD & SPIN.  STUB: never triggers. */
static void art_coin(void){
  cv_medal(0x5A3A00,0xE0A020,0xFFF0A0);
  cv_text("$",U(46),U(46),U(5),0x5A3400,255);
}
static void hold_on_snapshot(void){ memset(G.hold.val,0,sizeof G.hold.val); }
static int  hold_triggered(void){ return 0; }
static void hold_begin(void){ feature_done(); }
static void hold_update(void){}
static void hold_draw(void){ draw_reels(); }
static void hold_draw_cells(void){}
static long long hold_sim_play(void){ return 0; }
