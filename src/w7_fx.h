/* =====================================================================
 *  w7_fx.h - cosmetic effects: particles, glows, rays, screen shake,
 *  title transitions.  Cosmetic state lives in w7_fx.c as statics and
 *  is deliberately NOT part of game_t / the save state.
 *
 *  Any module may call these.  Spawning (fx_burst, fx_fountain,
 *  fx_shake, fx_transition) belongs in UPDATE code; the draw helpers
 *  (fx_glow, fx_rays) are for RENDER code and must not change state.
 * ===================================================================== */
enum { FXK_COIN, FXK_SPARK, FXK_STAR, FXK_CONFETTI };

static void fx_update(void);          /* once per frame, from update()          */
static void fx_draw(void);            /* world layer: over reels, under overlays */
static void fx_draw_top(void);        /* top layer: over everything             */
static void fx_post(void);            /* whole-frame post pass (shake), serial  */

static void fx_burst(float x,float y,int n,int kind);     /* a one-off burst  */
static void fx_fountain(float x,float y,float dur);       /* coin fountain    */
static void fx_shake(float amp,float dur);                /* pixels, seconds  */
static void fx_transition(const char*title,const char*sub,uint32_t col);
static int  fx_transition_busy(void); /* 1 while a transition is on screen    */

static void fx_glow(int cx,int cy,int r,uint32_t col,int alpha);   /* additive */
static void fx_rays(int cx,int cy,int r0,int r1,int n,float ang,uint32_t col,int alpha);
