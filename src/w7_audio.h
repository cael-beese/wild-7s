/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_audio.h - the sound engine's public face.
 *
 *  Everything here is called from UPDATE code on the main thread (never
 *  from a draw function), and audio_frame() runs once per retro_run on
 *  the main thread too, so none of it needs a lock.
 *
 *  The old four-waveform calls keep their exact signatures and meaning:
 *
 *    snd(f0,f1,dur,type,vol)          a note sweeping f0 -> f1 Hz
 *    snd_at(delay, f0,f1,dur,type,vol) the same, starting `delay` s later
 *    snd_noise(dur,vol,lp)            noise through a low-pass at lp Hz
 *    snd_noise_at(delay,dur,vol,lp)
 *    snd_chord(t0,a,b,c,dur,vol)      a triad
 *
 *  Types 0-3 are the originals (now band-limited and stereo-placed);
 *  4-8 are new.  Unknown types play as a triangle.
 *
 *  Levels: `vol` is linear, 0.05 is a click, 0.2 a lead note.  The master
 *  bus has a limiter and a soft clip, so a pile of notes can never
 *  crack the speaker, but it will squash the mix - keep phrases lean.
 * ===================================================================== */

/*  Every entry point is marked unused-safe: a module that has not adopted
 *  a sound yet must not cost a -Wall warning.                           */
#define AUDAPI static __attribute__((unused))

enum {
  SND_SQUARE = 0,    /* PolyBLEP square                                  */
  SND_TRI    = 1,     /* triangle                                          */
  SND_SAW    = 2,     /* PolyBLEP saw                                     */
  SND_NOISE  = 3,     /* white noise, one-pole low-pass at f0 (-> f1)      */
  SND_BELL   = 4,     /* two-operator FM bell, the casino "ding"           */
  SND_SINE   = 5,     /* pure sine                                        */
  SND_PLUCK  = 6,     /* Karplus-Strong plucked string                    */
  SND_WHOOSH = 7,     /* band-passed noise, centre swept f0 -> f1 (log)   */
  SND_BRASS  = 8      /* two detuned saws through an enveloped low-pass   */
};

/*  Instruments for snd_inst(): full envelopes, filter settings and
 *  reverb sends, the same ones the music uses.                          */
enum {
  W7I_EP, W7I_UPRIGHT, W7I_VIBES, W7I_PLUCK, W7I_SAWBASS, W7I_PAD,
  W7I_BRASS, W7I_BELL, W7I_MARIMBA, W7I_TIMP, W7I_STRING, W7I_SUB,
  W7I_DRUM,           /* lanes only: tokens are drum letters             */
  W7I_N
};

/* ---- the original calls (signatures frozen) ---- */
AUDAPI void snd(float f0,float f1,float dur,int type,float vol);
AUDAPI void snd_at(float del,float f0,float f1,float dur,int type,float vol);
AUDAPI void snd_noise(float dur,float vol,float lp);
AUDAPI void snd_noise_at(float del,float dur,float vol,float lp);
AUDAPI void snd_chord(float t0,float a,float b,float c,float dur,float vol);

/* ---- richer building blocks ---- */
/*  An instrument note: hz, held for len seconds (the release comes
 *  after), vel ~0..1.5, pan -1 left .. +1 right.                         */
AUDAPI void snd_inst(float del,int inst,float hz,float len,float vel,float pan);
/*  One drum hit.  k kick, s snare, c clap, h closed hat, o open hat,
 *  b brush, y ride, a shaker, r roll pair, C crash, l/d heartbeat,
 *  T taiko, t tom.                                                       */
AUDAPI void snd_drum(float del,char code,float vel,float pan);

/* ---- the game's own sounds (the core calls these) ---- */
AUDAPI void sfx_spin_start(void);
AUDAPI void sfx_reel_stop(int r);          /* thunk, panned and pitched per reel,
                                              plus scatter/crown landing dings  */
AUDAPI void sfx_anticipation(int r);       /* riser while reel r is held back   */
AUDAPI void sfx_bet_up(int idx);
AUDAPI void sfx_bet_down(int idx);
AUDAPI void sfx_bet_limit(void);           /* already at the end of the ladder  */
AUDAPI void sfx_bet_trim(void);            /* bet cut down to what the bank has */
AUDAPI void sfx_ui_move(int dir);          /* chooser / cursor step, dir +1 / -1 */
AUDAPI void sfx_ui_page(void);             /* pay-table page flip               */
AUDAPI void sfx_ui_open(void);             /* a panel opens                     */
AUDAPI void sfx_add_credits(int amount);   /* the cha-ching                    */
AUDAPI void sfx_win(int total);            /* jingle scaled to total / bet      */
AUDAPI void sfx_win_tick(float progress);  /* roll-up tick, pitch climbs 0..1   */
AUDAPI void sfx_rollup_done(void);
AUDAPI void sfx_big_win_tier(int t);       /* 0 BIG, 1 HUGE, 2 EPIC             */
AUDAPI void sfx_jackpot(int tier);         /* JP_MINOR .. JP_ULT                */
AUDAPI void sfx_jackpot_tick(float k);     /* coin ticks while a jackpot rolls  */
AUDAPI void sfx_mult(int lvl);             /* the multiplier climbing           */
AUDAPI void sfx_fs_intro(void);
AUDAPI void sfx_fs_end(void);
AUDAPI void sfx_bonus_start(void);         /* the pick round opens              */
AUDAPI void sfx_pick_reveal(int val);      /* a credit panel                    */
AUDAPI void sfx_pick_stop(int n);          /* the n-th STOP panel               */
AUDAPI void sfx_pick_end(void);
AUDAPI void sfx_broke(void);

/* ---- for the feature modules (not called by the core yet) ---- */
AUDAPI void sfx_coin_land(int tier,int col);  /* coin on the hold grid; tier 0
                                                 plain .. 3 jackpot coin; col 0-4 */
AUDAPI void sfx_respin_reset(void);        /* respins back to full              */
AUDAPI void sfx_countdown(int left);       /* respins left: tick, tenser at 1   */
AUDAPI void sfx_wheel_tick(float speed);   /* one peg past the pointer, 0..1    */
AUDAPI void sfx_wheel_stop(int tier);      /* the wheel lands, tier 0..3        */
AUDAPI void sfx_thunder(float power);      /* crack and rumble, 0..1            */
AUDAPI void sfx_strike(int col);           /* lightning onto a reel             */
AUDAPI void sfx_card_flip(void);
AUDAPI void sfx_gamble_win(void);
AUDAPI void sfx_gamble_lose(void);
AUDAPI void sfx_transition(void);          /* whoosh + boom for a title card    */
AUDAPI void sfx_feature_start(int kind);   /* 0 hold, 1 wheel, 2 other          */
AUDAPI void sfx_whoosh(float dur,int up);
AUDAPI void sfx_riser(float dur);          /* tension build over dur seconds    */
AUDAPI void sfx_impact(float power);       /* boom + crash, 0..1                */
AUDAPI void sfx_coin_shower(float dur,int n);

/* ---- music ---- */
AUDAPI void music_duck(float level,float secs);  /* hold music at level for secs */
AUDAPI void music_intensity(float x);      /* 0..1 hint for this frame (HOLD etc.) */

AUDAPI void audio_frame(void);             /* retro_run, once per frame         */
