/* =====================================================================
 *  w7_hold.h - HOLD & SPIN bonus (LUCKY COIN symbol).  Saved state and
 *  prototypes.  The struct is part of game_t, so it must stay flat: no
 *  pointers, save states memcpy it.
 * ===================================================================== */
typedef struct {
  int  val[NCELL];      /* credit value of the coin on each cell, 0 = none */
  int  lab[NCELL];      /* 0 = plain credits, else a label (module-defined) */
  int  spare[64];       /* module-owned working state                      */
} hold_state_t;

static void art_coin(void);            /* sprite art, called at init         */
static void hold_on_snapshot(void);    /* grid just landed: value the coins  */
static int  hold_triggered(void);      /* after evaluate(): 1 = start bonus  */
static void hold_begin(void);          /* enter ST_HOLD                      */
static void hold_update(void);         /* per frame in ST_HOLD; award() then
                                          feature_done() when finished       */
static void hold_draw(void);           /* ST_HOLD: owns the reel window      */
static void hold_draw_cells(void);     /* other states: values on landed coins */
static long long hold_sim_play(void);  /* sim: play it out, return the prize  */
