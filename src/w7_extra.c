/* w7_extra.c - 7 STRIKE and GAMBLE.  STUB: neither ever happens. */
static void extra_on_spin_start(void){ G.extra.stormArmed=0; }
static void extra_on_snapshot(void){ G.extra.stormMask=0; }
static int  extra_storm_pending(void){ return 0; }
static void extra_storm_begin(void){ G.state=ST_EVAL; G.t=0; }
static void extra_storm_update(void){ G.state=ST_EVAL; G.t=0; }
static void extra_draw_reels(void){}
static int  gamble_allowed(void){ return 0; }
static void gamble_begin(void){}
static void gamble_update(void){}
static void gamble_draw(void){}
