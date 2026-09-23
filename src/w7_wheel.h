/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_wheel.h - WHEEL OF 7's (WHEEL symbol on reels 2, 3 and 4).
 *  Saved state and prototypes.  Flat struct, part of game_t as G.wheel,
 *  so a save state taken mid-spin resumes mid-spin.
 * ===================================================================== */
typedef struct {
  int   phase;          /* WPH_* in w7_wheel.c                            */
  int   face;           /* 0 the WHEEL OF 7's, 1 the SUPER WHEEL          */
  int   target;         /* wedge the spin lands on, chosen at spin start  */
  int   landed;         /* wedge under the pointer once stopped, else -1  */
  int   lastPeg;        /* peg last clacked past, for the tick            */
  int   potTier;        /* JP_* tier won, -1 for a credit prize           */
  int   frame, nFrames; /* main-spin frame counter and its length         */
  int   spins;          /* spins taken this feature                       */
  int   tense;          /* the crawl's tension note has sounded           */
  long long prize;      /* credits the feature pays                       */
  long long shown;      /* the counter rolling up to it                   */
  float t;              /* seconds in the phase                           */
  float anim;           /* free-running clock for lights                  */
  float ang;            /* wheel rotation, degrees clockwise              */
  float vel;            /* degrees per frame, picks the motion blur       */
  float ang0, dAng;     /* start of the main spin and its total travel    */
  float vSum;           /* normaliser: the spin lands exactly on target   */
  float ptr, ptrV;      /* pointer deflection (degrees, + = tip right)    */
  float hot;            /* phase time the prize counter finished rolling  */
} wheel_state_t;

static void art_wheel(void);           /* sprite art, called at init         */
static int  wheel_triggered(void);     /* after evaluate(): 1 = start bonus  */
static void wheel_begin(void);         /* enter ST_WHEEL                     */
static void wheel_update(void);        /* per frame in ST_WHEEL; award() then
                                          feature_done() when finished       */
static void wheel_draw(void);          /* ST_WHEEL: full-screen scene        */
static long long wheel_sim_play(void); /* sim: play it out, return the prize */
