/* =====================================================================
 *  w7_extra.h - base-game extras: the 7 STRIKE wild storm (a random
 *  mystery feature) and the GAMBLE double-or-nothing.  Saved state and
 *  prototypes.  Flat struct, part of game_t.
 * ===================================================================== */
typedef struct {
  int   stormArmed;     /* this spin carries a 7 STRIKE                    */
  uint32_t stormMask;   /* cells the storm turned wild                     */
  int   spare[48];      /* module-owned working state                      */
  float fspare[16];
} extra_state_t;

/* 7 STRIKE */
static void extra_on_spin_start(void); /* start_spin(): maybe arm a storm   */
static void extra_on_snapshot(void);   /* snapshot_grid(): drop the wilds    */
static int  extra_storm_pending(void); /* reels stopped: play the storm?     */
static void extra_storm_begin(void);   /* -> ST_STORM                        */
static void extra_storm_update(void);  /* per frame; ends with ST_EVAL, t=0  */
static void extra_draw_reels(void);    /* over the reels in the reel states  */
/* GAMBLE */
static int  gamble_allowed(void);      /* may this win be gambled?           */
static void gamble_begin(void);        /* stake = G.winTotal (not yet paid)  */
static void gamble_update(void);       /* award() result, then feature_done() */
static void gamble_draw(void);
