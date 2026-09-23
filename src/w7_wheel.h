/* =====================================================================
 *  w7_wheel.h - WHEEL bonus (WHEEL symbol, reels 2-4).  Saved state and
 *  prototypes.  Flat struct, part of game_t.
 * ===================================================================== */
typedef struct {
  int   spare[64];      /* module-owned working state */
  float fspare[16];
} wheel_state_t;

static void art_wheel(void);           /* sprite art, called at init         */
static int  wheel_triggered(void);     /* after evaluate(): 1 = start bonus  */
static void wheel_begin(void);         /* enter ST_WHEEL                     */
static void wheel_update(void);        /* per frame in ST_WHEEL; award() then
                                          feature_done() when finished       */
static void wheel_draw(void);          /* ST_WHEEL: full-screen scene        */
static long long wheel_sim_play(void); /* sim: play it out, return the prize */
