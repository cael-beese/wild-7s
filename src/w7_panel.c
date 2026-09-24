/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md */
/* w7_panel.c - where WILD 7's buttons are on the cabinet's panel.
 *
 * The cabinet's panel has two sides (player 1 left, player 2 right), each a
 * stick, two rows of three buttons, and SELECT and START. WILD 7's lays its
 * button deck out on player 1's side BY POSITION, as a slot machine's deck
 * reads:
 *
 *      top:     PAYS          BET LESS      BET MORE
 *      bottom:  ADD CREDITS   BET MAX       SPIN
 *               SELECT = PAYS                START = SPIN
 *
 * (BET MAX also GAMBLEs a win; SPIN also slams the reels; the stick bets
 * and moves in the bonuses.) So the labels on the deck, the CONTROLS page
 * and the crib can say where a button is, whatever the wiring.
 *
 * RetroArch turns the encoder into a RetroPad. Which RetroPad button sits
 * in which position depends on how the panel was wired and set up, so it is
 * learned: LEARN PANEL (offered on the first start, and from the CONTROLS
 * page by holding any button for 5 seconds) asks for each position in turn
 * and saves the answer to wild7_panel.cfg in RetroArch's save directory.
 * Until then the guess is RetroPie's usual 6-button layout, Y X L over
 * B A R - the same guess as Beese's Poker Lounge (the two games do not
 * share files).
 *
 * poll_input() asks wp_read() for the game's buttons: each position's
 * RetroPad button drives what that position does (B_* bits), and the stick
 * passes through. WILD7_RAWPAD=1 (w7shot sets it) reads the RetroPad
 * letters directly, as before, so scripted runs keep their meaning. */

enum { PNL_T1, PNL_T2, PNL_T3, PNL_B1, PNL_B2, PNL_B3, PNL_SELECT, PNL_START, PNL_N };

/* What each position does, as the game's button bits. */
static const int PNL_DOES[PNL_N] = { B_SELECT, B_L, B_R, B_Y, B_X, B_A, B_SELECT, B_START };
static const char *const PNL_LABEL[PNL_N] = {
  "PAYS", "BET LESS", "BET MORE", "ADD CREDITS", "BET MAX", "SPIN", "PAYS", "SPIN" };
static const char *const PNL_NAME[PNL_N] = {
  "TOP LEFT", "TOP MIDDLE", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM MIDDLE", "BOTTOM RIGHT", "SELECT", "START" };
static const int PNL_GUESS[PNL_N] = {
  RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_L,
  RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_R,
  RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START };

static int  wpPad[PNL_N];                 /* the RetroPad button in each position, -1 none */
static int  wpLearned, wpOffered, wpShown, wpRaw, wpInit;
static char wpPath[1024];
static uint32_t wpNow, wpPrev;           /* raw RetroPad buttons (bit = id), this / last frame */
static float wpHoldT;                    /* CONTROLS page: one button held this long */

/* The RetroPad buttons a panel can send; the d-pad is the stick. */
static inline int wp_is_button(int id){
  return id != RETRO_DEVICE_ID_JOYPAD_UP && id != RETRO_DEVICE_ID_JOYPAD_DOWN &&
         id != RETRO_DEVICE_ID_JOYPAD_LEFT && id != RETRO_DEVICE_ID_JOYPAD_RIGHT;
}

/* The panel icon's bits (w7_lounge.c lz_icon: bit = player 1's position)
 * for every position that does any of the game bits in `bits`. */
static uint32_t wp_mask(int bits){
  uint32_t m = 0;
  for(int p = 0; p < PNL_N; p++) if(PNL_DOES[p] & bits) m |= 1u << p;
  return m;
}
static inline int wp_pos_down(int p){ return wpPad[p] >= 0 && ((wpNow >> wpPad[p]) & 1u); }

/* ── the file ─────────────────────────────────────────────────────── */
static void wp_save(void){
  if(!wpPath[0]) return;
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.tmp", wpPath);
  FILE *f = fopen(tmp, "w");
  if(!f) return;
  fprintf(f, "# WILD 7's - where each panel button is wired (see src/w7_panel.c)\n"
             "learned=%d\nwizard_offered=%d\ncontrols_shown=%d\n", wpLearned, wpOffered, wpShown);
  for(int p = 0; p < PNL_N; p++) fprintf(f, "pos%d=%d\n", p, wpPad[p]);
  int ok = fflush(f) == 0;
  ok = (fclose(f) == 0) && ok;
  if(ok) rename(tmp, wpPath);
  else remove(tmp);
}

static void wp_load(void){
  for(int p = 0; p < PNL_N; p++) wpPad[p] = PNL_GUESS[p];
  wpLearned = wpOffered = wpShown = 0;
  if(!wpPath[0]) return;
  FILE *f = fopen(wpPath, "r");
  if(!f) return;
  char line[128];
  int pad[PNL_N], learned = 0;
  for(int p = 0; p < PNL_N; p++) pad[p] = PNL_GUESS[p];
  while(fgets(line, sizeof line, f)){
    int p, v;
    if(sscanf(line, "learned=%d", &v) == 1) learned = v;
    else if(sscanf(line, "wizard_offered=%d", &v) == 1) wpOffered = v;
    else if(sscanf(line, "controls_shown=%d", &v) == 1) wpShown = v;
    else if(sscanf(line, "pos%d=%d", &p, &v) == 2 && p >= 0 && p < PNL_N && v >= -1 && v < 16) pad[p] = v;
  }
  fclose(f);
  if(learned){ memcpy(wpPad, pad, sizeof wpPad); wpLearned = 1; }
}

/* Once, when the game is loaded: where the file lives, and what is in it. */
static void wp_init(void){
  const char *e = getenv("WILD7_RAWPAD");
  wpRaw = e && *e && *e != '0';
  const char *dir = NULL;
  wpPath[0] = 0;
  if(!wpRaw && environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir)
    snprintf(wpPath, sizeof wpPath, "%s/wild7_panel.cfg", dir);
  wp_load();
  wpInit = 1;
}

/* ── reading the pad ─────────────────────────────────────────────── */
static int wp_read(void){
  wpPrev = wpNow;
  wpNow = 0;
  for(int id = 0; id < 16; id++)
    if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) wpNow |= 1u << id;
  int b = 0;
  #define RB(id, bit) if((wpNow >> (id)) & 1u) b |= (bit)
  RB(RETRO_DEVICE_ID_JOYPAD_UP, B_UP);     RB(RETRO_DEVICE_ID_JOYPAD_DOWN, B_DOWN);
  RB(RETRO_DEVICE_ID_JOYPAD_LEFT, B_LEFT); RB(RETRO_DEVICE_ID_JOYPAD_RIGHT, B_RIGHT);
  if(wpRaw || !wpInit){
    RB(RETRO_DEVICE_ID_JOYPAD_A, B_A);         RB(RETRO_DEVICE_ID_JOYPAD_B, B_B);
    RB(RETRO_DEVICE_ID_JOYPAD_X, B_X);         RB(RETRO_DEVICE_ID_JOYPAD_Y, B_Y);
    RB(RETRO_DEVICE_ID_JOYPAD_START, B_START); RB(RETRO_DEVICE_ID_JOYPAD_SELECT, B_SELECT);
    RB(RETRO_DEVICE_ID_JOYPAD_L, B_L);         RB(RETRO_DEVICE_ID_JOYPAD_R, B_R);
  } else {
    for(int p = 0; p < PNL_N; p++) if(wp_pos_down(p)) b |= PNL_DOES[p];
  }
  #undef RB
  return b;
}

/* ── LEARN PANEL ─────────────────────────────────────────────────── */
#define WPL_WAIT   12.0f        /* no press: that position has no button  */
#define WPL_CANCEL  3.0f        /* one button held this long: cancel      */
#define WPL_SHOW    5.0f        /* the result, then back                  */
static struct {
  int   step, got[PNL_N], autorun, back, backPage, saved;
  float t, msgT, holdT, doneT;
  int   holdId;
  char  msg[64];
} WL;

static void wp_learn_begin(int autorun, int back, int backPage){
  memset(&WL, 0, sizeof WL);
  for(int p = 0; p < PNL_N; p++) WL.got[p] = -1;
  WL.autorun = autorun;
  WL.back = back;
  WL.backPage = backPage;
  WL.holdId = -1;
  G.state = ST_LEARN;
  G.t = 0;
}

static void wp_learn_end(int keep){
  if(keep){
    memcpy(wpPad, WL.got, sizeof wpPad);
    wpLearned = 1;
  }
  wpOffered = 1;
  wp_save();
  if(keep && WL.autorun && !wpShown){
    /* after the wizard, the CONTROLS page, so the player sees the result */
    G.state = ST_PAYTABLE; G.ptPage = 3;
  } else {
    G.state = WL.back; G.ptPage = WL.backPage;
  }
  G.t = 0;
}

static void wp_learn_update(void){
  WL.t += DT;
  if(WL.msgT > 0) WL.msgT -= DT;
  if(WL.step >= PNL_N){
    WL.doneT += DT;
    if(WL.doneT >= WPL_SHOW) wp_learn_end(1);
    return;
  }
  /* one button held: cancel */
  if(WL.holdId >= 0 && ((wpNow >> WL.holdId) & 1u)){
    WL.holdT += DT;
    if(WL.holdT >= WPL_CANCEL){ wp_learn_end(0); return; }
  } else { WL.holdId = -1; WL.holdT = 0; }
  uint32_t pressed = wpNow & ~wpPrev;
  for(int id = 0; id < 16; id++){
    if(!((pressed >> id) & 1u) || !wp_is_button(id)) continue;
    WL.holdId = id;
    WL.holdT = 0;
    int dup = -1;
    for(int p = 0; p < WL.step; p++) if(WL.got[p] == id) dup = p;
    if(dup >= 0){
      snprintf(WL.msg, sizeof WL.msg, "THAT ONE IS ALREADY %s", PNL_NAME[dup]);
      WL.msgT = 2.0f;
      sfx_ui_move(-1);
      return;
    }
    WL.got[WL.step++] = id;
    WL.t = 0;
    WL.msg[0] = 0;
    sfx_ui_page();
    return;
  }
  if(WL.t >= WPL_WAIT){
    if(WL.step == 0 && WL.autorun){ wp_learn_end(0); return; }   /* nobody at the panel */
    snprintf(WL.msg, sizeof WL.msg, "NO PRESS: %s LEFT UNWIRED", PNL_NAME[WL.step]);
    WL.msgT = 2.5f;
    WL.got[WL.step++] = -1;
    WL.t = 0;
  }
}

/* Offered by itself on the first start (a pad, not w7shot, never asked). */
static void wp_first_start(void){
  if(!wpRaw && !wpLearned && !wpOffered && wpPath[0]) wp_learn_begin(1, ST_ATTRACT, 0);
}

/* ── drawing: the panel, one side, as a picture (draw code) ────────── */
static void dim(int a);

/* Player 1's side at (x, y), s = scale (1 = about 420 x 250): the stick,
 * two rows of three buttons and SELECT / START, each labelled with what it
 * does. mark = the position to press (pulsing), -1 none; lit = positions
 * held (bit per position). */
static void wp_draw_side(float x, float y, float s, int mark, uint32_t lit, int labels, float t){
  /* the stick */
  float sx = x + 60 * s, sy = y + 118 * s;
  lz_dot(sx, sy, 38 * s, 0x1A141E, 255);
  lz_dot(sx, sy, 17 * s, (wpNow & 0xF0u) ? 0xFF5A64 : 0xC82832, 255);
  lz_text_sh(LZF_UI_S, "STICK", sx, sy + 44 * s, 15 * s, 0xAAA0B4, LZ_CENTER);
  if(labels) lz_text_sh(LZF_UI_S, "BET / CHOOSE", sx, sy + 60 * s, 14 * s, LZ_HONEY, LZ_CENTER);
  for(int p = 0; p < PNL_N; p++){
    float cx, cy, r;
    if(p < 6){ cx = x + (170 + (p % 3) * 110) * s; cy = y + (60 + (p / 3) * 118) * s; r = 30 * s; }
    else { cx = x + (225 + (p - 6) * 110) * s; cy = y + 258 * s; r = 13 * s; }
    int on = (lit >> p) & 1u;
    uint32_t neon = (PNL_DOES[p] & (B_A | B_START)) ? LZ_GOLD : LZ_HONEY;
    if(p == mark){
      float pl = 0.5f + 0.5f * sinf(t * 8.0f);
      lz_add_tint(&lzGlow, (int)(cx - 32), (int)(cy - 32), LZ_CYAN, (int)(120 + 130 * pl));
      neon = LZ_CYAN;
    }
    if(on) lz_add_tint(&lzGlow, (int)(cx - 32), (int)(cy - 32), neon, 230);
    lz_dot(cx, cy, r + 3 * s, on ? lz_hot(neon, 0.4f) : neon, 255);
    lz_dot(cx, cy, r, on ? neon : 0x1E1622, 255);
    if(p >= 6){
      lz_text_sh(LZF_UI_S, p == PNL_SELECT ? "SELECT" : "START", cx, cy - 34 * s, 14 * s, 0xAAA0B4, LZ_CENTER);
      if(labels) lz_text_sh(LZF_UI_S, PNL_LABEL[p], cx, cy + 16 * s, 16 * s, LZ_HONEY, LZ_CENTER);
      continue;
    }
    if(labels){
      float ly = p < 3 ? cy - r - 30 * s : cy + r + 8 * s;
      lz_text_sh(LZF_UI_M, PNL_LABEL[p], cx, ly, 20 * s, on ? 0xFFFFFF : LZ_HONEY, LZ_CENTER);
    }
  }
}

/* The wizard, over the dimmed game. */
static void wp_learn_draw(void){
  dim(170);
  lz_neon(LZF_NEON_L, "LEARN THE PANEL", FBW / 2, 64, 54, LZ_CYAN, 1.0f, 0.9f);
  char b[96];
  if(WL.step < PNL_N){
    lz_text_sh(LZF_UI_L, "PLAYER 1 SIDE: PRESS THE", FBW / 2, 106, 36, 0xFFFFFF, LZ_CENTER);
    lz_gold(LZF_DISP_M, PNL_NAME[WL.step], FBW / 2, 178, 50, 1.0f, 0.6f);
    snprintf(b, sizeof b, "BUTTON   (%d OF %d)", WL.step + 1, PNL_N);
    lz_text_sh(LZF_UI_M, b, FBW / 2, 214, 26, 0xFFFFFF, LZ_CENTER);
    uint32_t got = 0;
    for(int p = 0; p < WL.step; p++) if(WL.got[p] >= 0) got |= 1u << p;
    wp_draw_side(FBW / 2 - 225, 262, 1.0f, WL.step, got, 0, WL.t);
    snprintf(b, sizeof b, "NO PRESS IN %d S: THIS POSITION HAS NO BUTTON.   HOLD ANY BUTTON 3 S: CANCEL.",
             (int)(WPL_WAIT - WL.t + 0.99f));
    lz_text_sh(LZF_UI_M, b, FBW / 2, 600, 22, 0xB4AABE, LZ_CENTER);
    if(WL.holdId >= 0 && WL.holdT > 0.5f) lz_text_sh(LZF_UI_M, "CANCELLING...", FBW / 2, 632, 26, 0xFF6A6A, LZ_CENTER);
    else if(WL.msgT > 0) lz_text_sh(LZF_UI_M, WL.msg, FBW / 2, 632, 26, LZ_HONEY, LZ_CENTER);
  } else {
    lz_gold(LZF_DISP_M, "PANEL LEARNED", FBW / 2, 150, 50, 1.0f, 0.6f);
    lz_text_sh(LZF_UI_M, "EVERY BUTTON NOW DOES THIS - PRESS ONE TO CHECK IT", FBW / 2, 196, 26, 0xFFFFFF, LZ_CENTER);
    uint32_t lit = 0;
    for(int p = 0; p < PNL_N; p++) if(WL.got[p] >= 0 && ((wpNow >> WL.got[p]) & 1u)) lit |= 1u << p;
    wp_draw_side(FBW / 2 - 225, 250, 1.0f, -1, lit, 1, WL.t);
  }
}

/* The CONTROLS page (the pay table's fourth page), over the pay table's
 * backdrop (draw_paytable paints the room and a glass panel first). */
static void wp_controls_draw(float t){
  lz_neon(LZF_NEON_L, "CONTROLS", FBW / 2, 52, 56, LZ_CYAN, 1.0f, 0.9f);
  lz_text_sh(LZF_UI_M, "EVERYTHING IS ON PLAYER 1'S SIDE OF THE PANEL - PRESS A BUTTON AND IT LIGHTS UP HERE",
             FBW / 2, 88, 22, 0xE6DCEE, LZ_CENTER);
  uint32_t lit = 0;
  for(int p = 0; p < PNL_N; p++) if(wp_pos_down(p)) lit |= 1u << p;
  wp_draw_side(FBW / 2 - 290, 150, 1.12f, -1, lit, 1, t);
  static const char *const NOTE[5] = {
    "SPIN ALSO STOPS THE REELS.  BET MAX ALSO GAMBLES A WIN.",
    "IN THE BONUSES THE STICK CHOOSES AND SPIN PLAYS.",
    "SELECT + START TOGETHER: BACK TO EMULATIONSTATION.",
    "BUTTONS IN THE WRONG PLACE?  HOLD ANY ONE FOR 5 SECONDS TO LEARN THE PANEL.",
    "SPIN OR START: BACK TO THE GAME" };
  for(int i = 0; i < 5; i++)
    lz_text_sh(LZF_UI_M, NOTE[i], FBW / 2, 526 + i * 28 + (i == 4 ? 10 : 0), 22, i == 4 ? 0xFFFFFF : i == 3 ? LZ_CYAN : 0xC8BED2, LZ_CENTER);
}
