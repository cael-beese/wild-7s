/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_hold.h - HOLD & SPIN bonus (LUCKY COIN symbol).  Saved state and
 *  prototypes.  The struct is part of game_t, so it must stay flat: no
 *  pointers, save states memcpy it.  Presentation clocks live in here
 *  too, so a save state taken mid-feature resumes exactly where it was.
 * ===================================================================== */
enum { HL_NONE=0, HL_MINOR, HL_MAJOR };            /* coin labels            */
enum { HP_TRIGGER=0, HP_INTRO, HP_READY, HP_SPIN,  /* feature phases         */
       HP_GRAND, HP_COLLECT, HP_SLAM };

typedef struct {
  /* the base game: what every landed coin carries */
  int   val[NCELL];      /* credit value of the coin on each cell, 0 = none */
  int   lab[NCELL];      /* HL_*: a jackpot coin, paid from its pot        */

  /* the feature board - the maths */
  long long bval[NCELL]; /* credits locked on each cell (a pot can pass 2^31) */
  int   blab[NCELL];     /* HL_* of each locked coin                       */
  int   locked;          /* cells holding a coin                           */
  int   respins;         /* respins left, 0..3                             */
  int   land[NCELL];     /* this respin: a coin is coming to this cell     */
  int   nval[NCELL];     /* ... and what it carries                        */
  int   nlab[NCELL];
  int   newThis;         /* coins this respin brought                      */
  int   grand;           /* the board filled                               */
  long long grandAmt;    /* the MEGA pot it took                           */
  long long total;       /* the prize, fixed when the respins run out      */

  /* the feature board - the show */
  int   phase;           /* HP_*                                           */
  float pt;              /* seconds in this phase                          */
  float tt;              /* seconds in the feature                         */
  float stopAt[NCELL];   /* respin clock at which each empty cell stops    */
  int   spinning[NCELL]; /* still turning this respin                      */
  float lockT[NCELL];    /* seconds since the coin locked                  */
  float lastStop;        /* when the last cell of this respin stops        */
  float lampT;           /* seconds since the lamps last re-lit            */
  int   relit;           /* a coin already re-lit the lamps this respin    */
  int   lampsLit;        /* intro: lamps switched on so far                */
  int   antic;           /* this respin stops slowly: the board is nearly full */
  int   trigK;           /* trigger phase: coins announced so far          */
  int   beat;            /* heartbeat: last beat sounded                   */
  int   cur;             /* collect: cell being counted, -1 none           */
  int   colK;            /* collect: next reading-order position           */
  float colT;            /* collect: seconds on the current coin           */
  float colDur;          /* collect: how long the current coin takes       */
  int   colDone[NCELL];  /* collect: already counted                       */
  int   colGrand;        /* collect: 1 = the GRAND pot is the current item */
  long long shown;       /* the meter as displayed                         */
  long long target;      /* what it is rolling toward                      */
} hold_state_t;

static void art_coin(void);            /* sprite art, called at init         */
static void hold_on_snapshot(void);    /* grid just landed: value the coins  */
static int  hold_triggered(void);      /* after evaluate(): 1 = start bonus  */
static void hold_begin(void);          /* enter ST_HOLD                      */
static void hold_update(void);         /* per frame in ST_HOLD; award() then
                                          feature_done() when finished       */
static void hold_draw(void);           /* ST_HOLD: owns the reel window      */
static void hold_draw_cells(void);     /* other states: values on landed coins */
static int  hold_force_rows(int r,int*rows);  /* WILD7_FORCE=hold test hook  */
/* the simulator's entry points: unused by the core itself */
static long long hold_sim_play(void) __attribute__((unused));
static void hold_sim_report(void)    __attribute__((unused));
