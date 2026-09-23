/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_extra.h - base-game extras: the 7 STRIKE wild storm (a random
 *  mystery feature) and the GAMBLE double-or-nothing.  Saved state and
 *  prototypes.  Flat struct, part of game_t: no pointers, save states
 *  memcpy it.  Everything purely cosmetic (sprites, particles) lives in
 *  w7_extra.c as statics instead.
 * ===================================================================== */
#define EX_MAXSTRIKE 8          /* most bolts one storm can throw         */
#define EX_NHIST     5          /* cards kept in the gamble history strip  */

typedef struct {
  /* ---- 7 STRIKE ---- */
  int      stormArmed;          /* this spin carries a 7 STRIKE             */
  uint32_t stormMask;           /* cells the storm turned wild              */
  int      nStrike;             /* bolts in this storm                      */
  int      order[EX_MAXSTRIKE]; /* the cell each bolt hits, in strike order */
  float    boltT[EX_MAXSTRIKE]; /* storm-clock time each bolt lands         */
  float    flickT[3];           /* sheet lightning while the reels spin     */
  float    st;                  /* storm clock (runs fast when skipped)     */
  float    endT;                /* storm clock time the storm hands over    */
  float    cloud0;              /* spin time when it began: cloud drift     */
  int      landed;              /* bolts that have struck so far            */
  int      fast;                /* the player asked to hurry it along       */
  uint32_t seed;                /* bolt shapes: the draw hashes this        */
  int      stormStop[NREEL];    /* reel stops the wilds belong to           */

  /* ---- GAMBLE ---- */
  int      gPhase;              /* GP_* in w7_extra.c                       */
  float    gT;                  /* seconds in the phase                     */
  int      gPot;                /* what COLLECT would pay right now         */
  int      gFrom;               /* the pot before the last round            */
  int      gRound;              /* rounds played this gamble                */
  int      gChoice;             /* 0 red, 1 black, 2+s = suit s             */
  int      gSuit;               /* the suit cursor                          */
  int      gCard;               /* suit*16+rank on the table, -1 face down  */
  int      gWon;                /* last round: 1 won, 0 lost                */
  int      gEnd;                /* GE_* why the gamble is ending            */
  int      gHist[EX_NHIST];     /* last cards dealt, newest first, -1 none  */
  float    gIdle;               /* seconds without a choice                 */
} extra_state_t;

static void extra_init(void);          /* retro_init(): bake clouds, cards   */
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
