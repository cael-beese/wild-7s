/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission) */
/* =====================================================================
 *  w7_fx.h - cosmetic effects: particles, glows, rays, screen shake,
 *  title transitions, and the win presentation built from them.
 *  Cosmetic state lives in w7_fx.c as statics and is deliberately NOT
 *  part of game_t / the save state.
 *
 *  Any module may call these.  Spawning (fx_burst, fx_fountain*,
 *  fx_rain, fx_firework, fx_home, fx_shake, fx_flash, fx_transition,
 *  fx_stop) belongs in UPDATE code.  The draw helpers (fx_glow,
 *  fx_rays*, fx_shade, fx_ring, fx_coin, fx_number) are for RENDER code:
 *  they change no state and write only rows [clip_y0, clip_y1).
 *
 *  Particles spawned by the public calls draw on the TOP layer (over
 *  every overlay and full-screen scene), so a bonus module's burst is
 *  always visible.  The win-cell bursts use the world layer (over the
 *  reels, under the overlays).
 * ===================================================================== */
/* Every call is "static" (one translation unit); unused-safe, because
   the modules that call a given one may not be merged yet. */
#define FX_API static __attribute__((unused))

enum { FXK_COIN, FXK_SPARK, FXK_STAR, FXK_CONFETTI };

FX_API void fx_init(void);            /* bake sprites/tables, from retro_init  */
FX_API void fx_deinit(void);
FX_API void fx_update(void);          /* once per frame, from update()          */
FX_API void fx_draw(void);            /* world layer: over reels, under overlays */
FX_API void fx_draw_top(void);        /* top layer: over everything             */
FX_API void fx_post(void);            /* whole-frame post pass (shake), serial  */

/* ---- spawning (update code) ---- */
FX_API void fx_burst(float x,float y,int n,int kind);     /* a one-off burst  */
FX_API void fx_fountain(float x,float y,float dur);       /* coin fountain    */
FX_API void fx_fountain_ex(float x,float y,float dur,float rate,int top);
FX_API void fx_rain(float dur,float rate);                /* coins from the top edge */
FX_API void fx_firework(float x,float y,uint32_t col);    /* a rocket from (x,y) up  */
FX_API void fx_home(float x,float y,float tx,float ty,int n,uint32_t col); /* sparks that fly to a target */
FX_API void fx_stop(void);            /* stop every emitter; live particles finish */
FX_API void fx_shake(float amp,float dur);                /* pixels, seconds  */
FX_API void fx_flash(float amt);      /* white flash 0..1, damped by the limiter */
FX_API void fx_transition(const char*title,const char*sub,uint32_t col);
FX_API int  fx_transition_busy(void); /* 1 while a transition is on screen    */

/* ---- draw helpers (render code) ---- */
FX_API void fx_glow(int cx,int cy,int r,uint32_t col,int alpha);   /* additive */
FX_API void fx_rays(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,int alpha);
/* rays squashed by ysc, optional rainbow colouring and a glow core (0..255) */
FX_API void fx_rays_ex(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,
                       int alpha,float ysc,int rainbow,int core);
FX_API void fx_shade(int cx,int cy,int rx,int ry,uint32_t col,int alpha); /* soft blended ellipse */
FX_API void fx_ring(int cx,int cy,float r,float th,uint32_t col,int alpha);/* additive shockwave */
FX_API void fx_coin(int cx,int cy,int d,float spin,int alpha);  /* a spinning gold coin, d = 14..40 */
FX_API void fx_number(long long v,float cx,float y,float sc,int alpha,int white); /* gold bubble digits, centred; sc 1 = 72px cap */

/* ---- the win presentation (called from the core) ---- */
FX_API int       fx_bw_tier(long long win,long long bet);   /* 0 small, 1 BIG .. 4 EPIC */
FX_API float     fx_bw_count_end(long long win,long long bet);
FX_API long long fx_bw_shown(long long win,long long bet,float t);
FX_API void fx_bigwin_slam(int tier);                 /* update: tier reached       */
FX_API void fx_bigwin_tick(int tier,int done);        /* update: every frame of one */
FX_API void fx_win_burst(uint32_t mask,uint32_t col,int big);  /* update: coins off cells */
FX_API float fx_jackpot_run(int tier);                /* seconds the meter counts   */
FX_API float fx_jackpot_hold(int tier);               /* then held before auto-pay  */
FX_API void fx_jackpot_begin(int tier);
FX_API void fx_jackpot_tick(int tier,float t);
FX_API void fx_multup_begin(void);
FX_API void fx_burst_col(float x,float y,int n,uint32_t col,int top);
FX_API void fx_light_cluster(uint32_t m,uint32_t c,float pulse,int heavy,float popT);
FX_API void fx_wins_draw(void);
FX_API void fx_bigwin_draw(void);
FX_API void fx_jackpot_draw(void);
FX_API void fx_multup_draw(void);
