# WILD 7's - architecture

How the code works, for whoever has to change it next. Everything is
referred to by name (function, struct, macro), not by line number, because
lines move. `DEVELOPING.md` holds the rules (build, frame budget, render
contract); this document explains the machinery those rules protect.
`docs/OPERATIONS.md` covers the cabinet. `docs/GAME_GUIDE.md` is the
player's view.

Contents

1. File map and the single translation unit
2. The libretro surface and the frame loop
3. Game state (`game_t G`) and save states
4. The state machine
5. The reels: strips, motion, the grid
6. Evaluation: ways, wilds, scatters, jackpots
7. Money: bet ladder, credits, award(), jackpots
8. The feature modules
9. Rendering
10. Audio
11. The maths and `src/sim.c`
12. Core options and the `.w7` file
13. Test hooks
14. Known limits, bugs and gotchas

---

## 1. File map and the single translation unit

| file | what it is |
|---|---|
| `src/wild7_libretro.c` | the core: primitives, art, strips, evaluator, state machine, cabinet renderer, libretro API. About 6,500 lines. The only file the compiler is given. |
| `src/w7_thread.c` | band renderer worker pool (`bp_*`). Generic: knows rows, not the game. |
| `src/w7_audio.c` / `.h` | synth, music sequencer, every sound effect (`snd*`, `sfx_*`, `music_*`, `audio_frame`). |
| `src/w7_fx.c` / `.h` | cosmetic effects: particles, glows, rays, shake, title transitions, the win presentation (BIG..EPIC, jackpot celebration, multiplier pop, lit win paths). |
| `src/w7_hold.c` / `.h` | HOLD & SPIN (LUCKY COIN symbol). |
| `src/w7_wheel.c` / `.h` | WHEEL OF 7's (WHEEL symbol). |
| `src/w7_extra.c` / `.h` | 7 STRIKE wild storm and the GAMBLE. |
| `src/w7_lounge.c` | the lounge look shared with Beese's Poker Lounge (section 9a): the embedded fonts, neon / gold / plain text and its string cache, glass panels, neon buttons, the room, bulbs, the bee, the panel icon; `text()`, `textb()`, `bake_title()` and `seg_num()` set their type through it. |
| `src/w7_raster.c` / `.h`, `src/w7_lart.c` / `.h` | the poker game's CPU vector rasteriser (SDF shapes, paints, blur) and its motifs (bee, brass, honeycomb, suit pips), names prefixed `l`/`L` to fit the unity build. Start-up only. |
| `src/w7_panel.c` | the buttons by panel position, LEARN PANEL (`ST_LEARN`), the CONTROLS page, `wild7_panel.cfg` (section 2a). |
| `src/third_party/stb_truetype.h` | Sean Barrett's font rasteriser (public domain / MIT), static in the core. |
| `assets/fonts/` | Barlow Condensed, Bungee, Tilt Neon, Neonderthaw (SIL OFL, licences beside them), embedded in the core with `.incbin`; `W7_ASSETS` (the Makefile) is their absolute path. |
| `src/libretro.h` | vendored libretro API header (the Makefile downloads it only if missing). |
| `src/sim.c` | RTP simulator `w7sim`; `#include "wild7_libretro.c"`. |
| `tools/w7shot.c` | headless host `w7shot`: runs `retro_run()`, writes PNG/PPM frames, a WAV, frame hashes and timings; includes the core. |
| `tools/w7audio.c` | audio bench `w7audio`; includes the core. |
| `tools/bandcheck.sh` | proves the threaded renderer is bit-identical to one thread. |
| `tools/pi_soak.sh` | headless RetroArch soak on the Pi with temperature/clock/load sampling. |
| `install-retropie.sh` | installs the built core as a RetroPie Port (see OPERATIONS). |
| `w7launch.sh` | starts the game on the cabinet from SSH (see OPERATIONS). |
| `wild7.w7` | the "ROM": a plain-text settings blob (section 12). |
| `wild7_libretro.info` | RetroArch core info file. |
| `Makefile` | `make` (core), `make sim`, `make shot`, `make audio`, `make android`. |

### Include order

`wild7_libretro.c` is a unity build. Everything is `static`. The order
inside it matters:

```
wild7_libretro.c
  <stdio.h> ... "libretro.h"
  FBW/FBH/DT/SRATE/SPF, fb[], bg[], abuf[]
  thread-local clip_y0/clip_y1, clip_rows(), rows_visible(), in_band()
  #include "w7_thread.c"          <- the pool needs only FBH and the clip
  callbacks, opt_*, dbg_* test hooks, frnd()/irnd(), colour helpers, FONT
  sprite canvas (cv_*), framebuffer primitives (fb_*, blit*), text, textb
  #include "w7_lounge.c"           <- the lounge kit (it includes w7_raster.c,
                                      w7_lart.c, stb_truetype.h); needs the
                                      primitives, and text() calls into it
  symbol art (materials, cv_medal, art_*), add_contour, bake_shadow,
    make_streak, build_sprites
  PAY, SCATPAY, CNT, STK, build_strips
  enum ST_*, PEND_*
  #include "w7_fx.h" "w7_hold.h" "w7_wheel.h" "w7_extra.h"
                                   <- module state structs + prototypes
  typedef ... game_t;  static game_t G;
  bet ladder, FS_* dials, jackpot tables and jp_* helpers
  #include "w7_audio.c"            <- needs game_t (music_auto reads G);
                                      update() below calls sfx_*
  snapshot_grid, flood, add_win, score_ways, evaluate, start_spin,
  pick bonus, enum B_*
  #include "w7_panel.c"            <- needs B_* and G; poll_input() calls it
  input, award, feature queue, force hooks
  update()
  cabinet chrome, titles, backdrop painting, draw_reels, draw_features,
  draw_meters, frame_cache, pay table pages, overlays, marquee, art_free
  #include "w7_fx.c" "w7_hold.c" "w7_wheel.c" "w7_extra.c"
                                   <- they use every primitive above
  draw_frame(), render_band(), render()
  libretro API, retro_run()
```

Why this shape:

- **One translation unit** lets `-O3` inline the pixel primitives into every
  caller across modules, lets modules share `static` state without a header
  for every primitive, and keeps the build a single `cc` command. The cost is
  the build time (about 3.5 minutes on the Pi 4, per docs/DESIGN.md) and that every
  unused function warns under `-Wall`; the module APIs are therefore
  declared `static __attribute__((unused))` (`FX_API`, `AUDAPI`).
- **Module headers come before `game_t`** because `game_t` embeds each
  module's saved-state struct (`hold_state_t`, `wheel_state_t`,
  `extra_state_t`) and `update()` calls module functions whose definitions
  come much later. The headers carry the structs and the prototypes.
- **Audio sits between `game_t` and `update()`**: it reads `G` (music
  selection, `aud_watch`) and `update()` calls it. It forward-declares
  `stripAt()`, which is defined just after it.
- **The other modules sit just before `draw_frame()`**: their draw code uses
  the primitives, titles, sprites and layout macros defined above, and
  `draw_frame()` calls their draw functions.
- **`src/sim.c`, `tools/w7shot.c` and `tools/w7audio.c` include the whole
  core** so the simulator measures the evaluator that ships and the frame
  dumper draws the frames that ship. They can also reach `static` state:
  `sim.c` pokes `G`, `CNT`, `STK`, `JP_NEED`, `FS_AWARD`, `FS_MAXMULT`;
  `w7shot` sets `rngs` and reads `G.state`. Anything that must be tunable
  from the simulator is therefore a `static` variable, not a `#define`.

Build-time switches: `-DW7_NOTHREADS` (single-threaded renderer; forced
automatically for Android API < 29, whose TLS is emulated), `-DSTORM_ODDS=N`
(7 STRIKE frequency, 0 = off), `-DW7SHOT_NOPNG` (w7shot writes PPM, no
zlib), `-DW7_AUDIO_PROF` (times `audio_frame`, used by w7shot/w7audio).

Compiler flags differ by target: the core is `-O3 -ffast-math
-fno-math-errno -fPIC` (+ `-mcpu=native` on aarch64); `w7sim` and `w7shot`
are plain `-O2`; `w7audio` uses the core's flags. See section 14 on what that
means for the simulator.

---

## 2. The libretro surface and the frame loop

| entry point | what it does |
|---|---|
| `retro_set_environment` | declares `SUPPORT_NO_GAME`, the core options (`VARS[]`), input descriptors. |
| `retro_init` | reads the `WILD7_*` environment hooks; on first call bakes every asset: `build_strips`, `build_sprites` (which also runs the hold and wheel art, and `wh_build`), `build_dome(0/1)`, `build_bg`, `extra_init`, `fx_init`; then `reset_game()`. Init takes about 1.5 s in w7shot on x86 (the Pi is several times slower). |
| `retro_load_game` | insists on `RETRO_PIXEL_FORMAT_XRGB8888`, gets the log interface, `check_vars()` (options, thread pool), then parses the `.w7` text if content was given. Content is optional. |
| `retro_get_system_av_info` | 1280x720, aspect 16/9, 60 fps, 44,100 Hz. |
| `retro_get_system_info` | name "Wild 7's", extensions `w7|wild7`, `need_fullpath=false`. `library_version` "3.0.1". |
| `retro_run` | one frame, below. |
| `retro_reset` | `reset_game()`: 5,000 credits, bet 50, attract loop, fresh random pots. Does not re-read the `.w7`. |
| `retro_serialize*` | `memcpy` of `G` (section 3). |
| `retro_deinit` | joins the band workers (`bp_shutdown`), frees caches, sprites and everything the art baked (`art_free`, `fx_deinit`). |
| memory, cheats, controller port | stubs. |

The frame:

```
retro_run()
  if GET_VARIABLE_UPDATE -> check_vars()      (options; may rebuild the pool)
  poll_input()                                (G.prevBtn = G.btn; G.btn = pad
                                               bits, or the autopilot script)
  update()                                    (all game logic, one DT = 1/60 s)
  render()                                    (bp_run(render_band) over the
                                               bands; render_commit(); fx_post())
  audio_frame()                               (735 stereo samples into abuf)
  audio_batch_cb(abuf, 735)
  video_cb(fb, 1280, 720, 5120)
```

With `WILD7_PROFILE=1`, `update()` and `render()` are timed and every 300
frames a line goes to stderr:
`[wild7] frames a-b  render mean X max Y ms  update mean ... threads T bands B  state S`.

Time is frame-counted: `DT` is a constant 1/60 s, so the game runs in
frames, not wall time. Everything that animates is driven from `update()`.

`update()` in order: advance `G.t`, `G.idle`, `G.flash`, `G.bannerT`,
`G.multUp`; `fx_update()` (particles, emitters, shake); `art_update()`
(cosmetic clocks `artT` and `mqT`, the free-spins backdrop swap, the pick
panel flip); the reel motion for all five reels; then the `switch` on
`G.state`.

Input: `poll_input()` builds a bit mask (`B_UP`..`B_R`) from pad 0 through
`wp_read()` (section 2a). `hit(m)` is a rising edge, `anyhit()` is any new
press.

### 2a. Controls by panel position - w7_panel.c

The cabinet's panel has two sides, each a stick, two rows of three
buttons, SELECT and START. WILD 7's lays its deck out on player 1's side
**by position**, as a slot deck reads, and every label on screen says
where a button is rather than naming a RetroPad letter:

```
top:     PAYS (B_SELECT)          BET LESS (B_L)     BET MORE (B_R)
bottom:  ADD CREDITS (B_Y)        BET MAX (B_X)      SPIN (B_A)
         SELECT = PAYS (B_SELECT)                    START = SPIN (B_START)
```

`PNL_DOES[]` is that table (position -> game bit), `PNL_LABEL[]` the
names. `wpPad[]` is which RetroPad button RetroArch reports for each
position; `wp_read()` reads all sixteen RetroPad ids into `wpNow` and sets
each position's game bit when its button is down (the d-pad, the stick,
passes through). `wp_mask(bits)` gives the panel icon's lit positions for
any game bits, `wp_pos_down(p)` whether a position is held.

The wiring is learned. `wpPad[]` starts as the guess `PNL_GUESS[]` -
RetroPie's usual 6-button layout, Y X L over B A R, the same guess as the
poker game - and LEARN PANEL (`ST_LEARN`, `wp_learn_update` /
`wp_learn_draw`) asks for each of the eight positions in turn, reading raw
presses from `wpNow`. It runs by itself on the first start
(`wp_first_start()`, from `retro_load_game`, when the file says it was never
offered; no press within 12 s on the first question gives up quietly) and
from the CONTROLS page by holding one button 5 s. The result goes to
`wild7_panel.cfg` in RetroArch's save directory (`wp_save`: temporary
file, rename) with `learned`, `wizard_offered`, `controls_shown`.

The CONTROLS page is the pay table's fourth page (`G.ptPage == 3`,
`wp_controls_draw`): player 1's side drawn large, each button lit while
held; only SPIN / START leave it (every other button is being shown), and
90 s without a press returns to attract. The first player to leave the
attract loop is shown it once (`controls_shown`).

`WILD7_RAWPAD=1` (w7shot sets it) reads the RetroPad letters directly as
before, never reads or writes the file and never offers the wizard, so
scripted runs keep their meaning.

---

## 3. Game state and save states

`static game_t G` holds everything that decides what happens next, as flat
data: no pointers, so `retro_serialize` is `memcpy(d,&G,sizeof G)` and
`retro_unserialize` the reverse, and only for a state of exactly `sizeof G`.
`sizeof(game_t)` is 2,480 bytes on x86-64.

Main fields:

| group | fields |
|---|---|
| flow | `state`, `t` (seconds in state), `idle` (seconds since input), `pend` (PEND_* bits still to run) |
| money | `credits` (long long), `betIdx`, `winTotal`, `winShown`, `lastWin`, `jpAcc[4]` (pots, in thousandths of a credit), `jpWon`, `jpAmt`, `jpMask`, `jpT` |
| reels | `grid[5][5]`, `rpos[5]` (continuous strip position), `rstate[5]` (0 idle, 1 spin, 2 settle, 3 stopped), `rt0/rt1/ru/rdelay[5]`, `reelBlur[5]`, `expand[5]` |
| wins | `nWin`, `winSym/winCnt/winAmt/winWays/winWt/winMask[12]`, `showIdx`, `showT` |
| triggers | `scatCount`, `bonusCount` (crowns), `coinCount`, `wheelCount` |
| free spins | `freeSpins`, `fsMult`, `fsWon`, `inFree`, `multUp` (seconds of the "meter climbed" pop), `multFrom` |
| pick | `pickVal/pickKind/pickDone[9]`, `pickCur`, `pickTotal`, `pickStops`, `pickMult`, `pickT` |
| UI | `addIdx`, `addFrom`, `ptPage`, `banner`, `bannerT`, `flash`, `btn`, `prevBtn` |
| modules | `hold` (`hold_state_t`), `wheel` (`wheel_state_t`), `extra` (`extra_state_t`) |
| vestigial | `seed` (set in `reset_game`, never read), `spinT` (reset, never read) |

Module state lives in its own struct inside `G` so a save state taken
mid-feature resumes mid-feature: the hold board, respin clocks and collect
cursor, the wheel angle and target, the storm's bolt order and timetable,
the gamble pot and card history are all in there.

**Deliberately not in `G` (not saved):**

- the game RNG `rngs` (a global xorshift). After a load, outcomes follow a
  different random sequence from the original timeline;
- particles, emitters, shake and the transition card (`w7_fx.c` statics,
  with their own RNG `fxRng`);
- the synth's voices, reverb and music players (`w7_audio.c` statics, own
  RNG `aud_rng`);
- art clocks `artT`, `mqT`, button flashes `btnFlash[]`, `bgShown`,
  `pickSeen[]`, `flipIdx`;
- every cache and baked asset: the textb masks, the full-frame caches, the
  wheel frame cache, the baked backdrops (`bgBase`, `bgFree`, `hbk`,
  `whStage`, `pkStage`, `ptRoom`, `tblImg`) and the SUPER wheel face, which is built
  incrementally;
- options (`opt_*`) and test hooks (`dbg_*`, `hold_dbg`, `fwStop`).

These are either cosmetic or derived from `G` on the next frame (the
backdrop swap and pick board key off `G`; the SUPER wheel face finishes
building on the spot if a loaded state needs it).

There is no version field, so the size is the check: a state whose size
is not exactly `sizeof(game_t)` is refused (it used to accept any larger
state and load garbage). A change that keeps the size but moves fields
would still load wrongly, so treat any change to `game_t` (or to a module
state struct) as breaking save states.

---

## 4. The state machine

`enum { ST_ATTRACT, ST_IDLE, ST_SPIN, ST_EVAL, ST_SHOWWIN, ST_FSINTRO,
ST_BONUS, ST_BONUSEND, ST_PAYTABLE, ST_BROKE, ST_JACKPOT, ST_ADDCR,
ST_HOLD, ST_WHEEL, ST_GAMBLE, ST_STORM }`. `G.t` is reset on almost every
transition and many states key their timing off it.

### Every state

| state | entered from | what happens | leaves to |
|---|---|---|---|
| `ST_ATTRACT` | `reset_game` (with `t=3` so the reels start at once); `ST_IDLE` after 25 s idle (not in free spins) | reels spin every 3 s for show; `draw_attract` runs a 21 s loop of captions and close-ups | any press -> `ST_IDLE` |
| `ST_IDLE` | attract, `after_result`, add credits, pay table, free-spins intro | base game: bet up (RIGHT/UP) / down (LEFT/DOWN), SELECT pay table, Y add credits, X max affordable bet and spin, START/A spin (trims the bet to the bank, or goes BROKE). In free spins: no input, `start_spin()` after 0.35 s | `ST_SPIN`, `ST_PAYTABLE`, `ST_ADDCR`, `ST_BROKE`, `ST_ATTRACT` |
| `ST_SPIN` | `start_spin()` | reels run; START/A/B after 0.3 s slams them. When all five are stopped: `snapshot_grid(); evaluate();`, the multiplier pop if the meter climbed, `G.pend=0` | `ST_STORM` if a 7 STRIKE is pending, else `ST_EVAL` |
| `ST_STORM` | `extra_storm_begin()` | bolts land on a timetable (START/A/B runs the storm clock 3x) | `ST_EVAL` (t=0) |
| `ST_EVAL` | spin, storm, jackpot (t=0.21) | waits 0.2 s (and for a multiplier pop in free spins). Jackpot first; otherwise `G.pend |= pend_from_grid()` (idempotent, EVAL can be re-entered), then wins or the queue | `ST_JACKPOT`, `ST_SHOWWIN`, or `next_feature()` |
| `ST_JACKPOT` | `ST_EVAL` with `G.jpWon >= 0` | the celebration (`fx_jackpot_*`), coin ticks; auto after `fx_jackpot_run + fx_jackpot_hold` or a press after 1.4 s: `award(G.jpAmt)` | `ST_EVAL` (t=0.21) |
| `ST_SHOWWIN` | `ST_EVAL` with `winTotal > 0` | count-up (`fx_bw_shown`), BIG..EPIC slams, win cells cycle. First press jumps to the total, the next collects: `award(winTotal); next_feature()`. X on a counted win may gamble | `next_feature()` target, or `ST_GAMBLE` |
| `ST_GAMBLE` | `gamble_begin()` from SHOWWIN | `gamble_update()`; ends with `award(pot); feature_done()` | queue |
| `ST_HOLD` | `hold_begin()` via queue | `hold_update()`; ends `award; feature_done()` | queue |
| `ST_WHEEL` | `wheel_begin()` via queue | `wheel_update()`; ends `award; feature_done()` | queue |
| `ST_BONUS` | `begin_bonus()` via queue | the pick board: the stick (and BET LESS / MORE) moves, SPIN (A/START, or B) turns a panel; third STOP ends it | `ST_BONUSEND` (banner 4) |
| `ST_FSINTRO` | queue (`PEND_FS`) | 2.6 s title (press after 0.6 s). Fresh: `freeSpins += FS_AWARD; inFree=1; fsMult=1; fsWon=0`. Retrigger: `freeSpins += FS_RETRIG; after_result()` | `ST_IDLE` |
| `ST_BONUSEND` | end of pick (banner 4), `end_free_spins()` (banner 3) | 2.4 s (press after 0.7 s). Pays the pick: `award(pickTotal * pickMult)` | banner 3: `ST_IDLE`/`ST_BROKE`; banner 4: `feature_done()` |
| `ST_PAYTABLE` | IDLE + SELECT | three cached pages; SELECT flips page, any other press leaves (after 0.3 s) | `ST_IDLE` |
| `ST_BROKE` | `after_result` / free-spins end with credits < 10; START in IDLE with nothing affordable | START/A/Y open add credits | `ST_ADDCR` |
| `ST_ADDCR` | IDLE + Y, BROKE | choose 100..5,000; A/START add; B/SELECT/Y cancel | `ST_IDLE` or back to `ST_BROKE` |

### The spin and the feature queue

```
 ST_IDLE --START/A/X--> start_spin()          (bet taken, pots fed,
    ^                        |                 7 STRIKE maybe armed)
    |                        v
    |                    ST_SPIN --all reels stopped--> snapshot_grid()
    |                        |                          evaluate()
    |          storm armed?  |
    |            +-----------+------------+
    |            v                        v
    |        ST_STORM --bolts done-->  ST_EVAL <---------------------+
    |                                     |                           |
    |                    jpWon>=0 ?  yes  v                           |
    |                                 ST_JACKPOT --award(jpAmt)-------+
    |                                     |  (t=0.21, jpWon=-1)
    |                                     | no
    |                  G.pend |= pend_from_grid()
    |                                     |
    |                 winTotal>0 ?  yes   v
    |                                 ST_SHOWWIN --X, win<=50x bet--> ST_GAMBLE
    |                                     |  collect: award(winTotal)     |
    |                                     v                               |
    |                               next_feature() <---- feature_done() --+
    |                                     |       ^  (award(prize) first)
    |     PEND_HOLD  -> hold_begin()  -> ST_HOLD --+
    |     PEND_WHEEL -> wheel_begin() -> ST_WHEEL -+
    |     PEND_PICK  -> begin_bonus() -> ST_BONUS -> ST_BONUSEND(banner 4) -+
    |     PEND_FS    -> ST_FSINTRO                                          |
    |                     |  fresh: inFree=1, freeSpins=FS_AWARD -> ST_IDLE |
    |                     |  retrigger: freeSpins+=FS_RETRIG -> after_result
    |     nothing pending -> after_result()
    |                                     |
    +-------------- base game: ST_IDLE (or ST_BROKE if credits < 10)
                    free spins: freeSpins--; >0 -> ST_IDLE (auto spin)
                                          ==0 -> end_free_spins()
                                                  -> ST_BONUSEND (banner 3)
                                                  -> ST_IDLE / ST_BROKE
```

Rules that fall out of this:

- **A spin's own wins are paid first**, then its features, in the fixed
  order HOLD, WHEEL, PICK, FREE SPINS (`next_feature()` clears one PEND_*
  bit per call). A reel jackpot is paid before the spin's way wins.
- **Features can trigger inside free spins.** `pend_from_grid()` does not
  look at `inFree`, so a free spin can open HOLD & SPIN, the WHEEL or the
  PICK; their prizes land in `fsWon` through `award()`. The free spin that
  triggered them is counted down by `after_result()` when the queue empties.
  7 STRIKE and GAMBLE are base game only.
- **A retrigger still costs the spin that made it**: `ST_FSINTRO` with
  `inFree` adds `FS_RETRIG` and then calls `after_result()`, which
  decrements.
- **A module ends its feature** with `award(prize)` then `feature_done()`
  (which is just `next_feature()`). It must leave `G.state` alone after that
  call, because the queue has already moved it on.
- `after_result()` is the only place free spins are counted down and the
  only place the base game decides IDLE vs BROKE after a spin.
- The multiplier pop: when `snapshot_grid()` raises `fsMult` it sets
  `G.multUp = 1.9`; `ST_SPIN` fires `fx_multup_begin()`/`sfx_mult()`, and
  `ST_EVAL` waits until `multUp` falls under 0.55 s before showing wins.

Timings worth knowing: EVAL 0.2 s; SHOWWIN holds a finished count 5.4 s in
the base game, 1.6 s in free spins, 3.0 s for a BIG+ win before auto-collect;
jackpot run 2.8 / 3.6 / 4.6 / 8.0 s (MINOR..ULTIMATE) plus 2.2 s hold (3.5 s
ULTIMATE); FSINTRO 2.6 s; BONUSEND 2.4 s; attract after 25 s idle.

---

## 5. The reels

### Symbols

`enum { SY_SEVEN, SY_DIAMOND, SY_BELL, SY_BAR, SY_GRAPES, SY_ORANGE,
SY_PLUM, SY_CHERRY, SY_LEMON, SY_STAR, SY_CROWN, SY_JACKPOT, SY_ULT,
SY_COIN, SY_WHEEL, NSYM }`. `NPAYSYM = 9`: SEVEN..LEMON pay on ways.
`SY_STAR` is the SCATTER, `SY_CROWN` opens the pick, `SY_COIN` is the
LUCKY COIN, `SY_WHEEL` the wheel trigger. Names for display are in
`SYMNAME[]`.

### Strips

`strip[5][STRIPLEN]`, `STRIPLEN = 96`. Built once by `build_strips()` from
two tables, rows = strip type: `[0]` reels 1 and 5, `[1]` reels 2 and 4,
`[2]` reel 3.

- `CNT[3][NSYM]`: how many cells of each symbol a strip carries (each row
  sums to 96).
- `STK[3][NSYM]`: the stack height they are laid in. JACKPOT is stacked
  (2 on reels 2/4, 3 on reel 3); most others 1 or 2.

Current counts (7 D B BAR GR OR PL CH LE ST CR JP UL CO WH):

```
reels 1,5  2 8 9 10 11 11 12 10 8  2 3 0 2 8 0
reels 2,4  2 8 9 10 11 11 11 10 9  1 0 3 1 7 3
reel 3     2 8 9 10 10 11 11 10 8  2 3 3 1 6 2
```

So CROWN is on reels 1, 3, 5 only; JACKPOT and WHEEL on reels 2-4 only.

`build_strips()`:

1. cuts each symbol's count into runs of `STK` height;
2. shuffles the runs with its own xorshift seeded `0x5EED77` - the strips are
   the same on every build and every machine, but **any change to CNT or STK
   reshuffles every strip**, which moves every trigger rate (the COIN trigger
   in particular depends on how often two coins happen to sit within a
   window);
3. spreads the single-cell specials (`SY_STAR, SY_CROWN, SY_ULT, SY_WHEEL`)
   evenly round the strip, offset per symbol, so a 5-row window shows at
   most one of each. Trigger odds are then a property of the counts, not of
   the shuffle;
4. breaks up adjacent runs of the same symbol so no stack grows taller than
   `STK` says;
5. writes the runs out (pads with LEMON if short; it never is).

`stripAt(r,i)` indexes modulo 96 with negative wrap.

### Reel motion

Per reel: `rpos` is a continuous strip position in symbols (it only ever
grows), `rstate` 0 idle / 1 spinning / 2 settling / 3 stopped.

`start_spin()` (after taking the bet and calling `extra_on_spin_start()`)
sets every reel to state 1 with a staggered `rdelay`: the first reel after
0.8 s, each next one a further 0.5 + U(0,1.5) s (turbo: 0.4 s, then
0.25 + U(0,0.75) s). The random gaps are re-rolled every spin.

In `update()`:

- **state 1**: `rpos += spinv*DT` (13 symbols/s, turbo 20). When `rdelay`
  runs out the reel picks its stop: `tgt = floor(rpos) + 4 + irnd(96)`, a
  uniformly random strip position at least four symbols ahead. That draw is
  the outcome for that reel (the test hooks replace it, section 13). State
  becomes 2 with `rt0 = rpos`, `rt1 = tgt`.
- **state 2**: `ru` runs 0..1 over `settle` seconds (0.55, turbo 0.34) and
  `rpos = rt0 + (rt1-rt0)*easeOutBack(ru)`, which overshoots and springs
  back. `reelBlur = 1-ru`. At the end: state 3, `sfx_reel_stop(r)`.
- **anticipation hold**: when reel 4 or 5 (index >= 3) is about to settle,
  `rt1 == 0` and `partial_special() >= 2` (two scatters, two crowns or two
  wheels already on stopped reels), the reel is held another 0.95 s,
  `rt1` is set to 1 as a "held once" flag and `sfx_anticipation(r)` plays.
  `start_spin()` zeroes `rt1` for every reel (until version 3.0.1 it did
  not, and the hold only fired on the first spin after the attract loop).
- **slam**: START/A/B in `ST_SPIN` after 0.3 s sets `rdelay=0` and
  `rt1=1` (no hold) for every reel still in state 1.

Each reel's outcome is drawn when it starts to settle, so the order of RNG
calls depends on timing; nothing relies on that.

### snapshot_grid()

Called when all reels have stopped (and by the simulator after it places
reels at random):

1. `grid[r][row] = stripAt(r, round(rpos[r]) - row)` - row 0 is the top;
   `expand[]` cleared.
2. `extra_on_snapshot()` - if this spin was armed for 7 STRIKE, choose and
   drop the storm wilds into `grid` (section 8).
3. In free spins: any of reels 2-4 (`r = 1..3`) showing a SEVEN becomes all
   SEVEN, `expand[r] = 1`, and `fsMult += (reels expanded this spin)`,
   capped at `FS_MAXMULT`; `multFrom`/`multUp` are set for the pop. The
   meter only rises during a feature and is set back to 1 when the next
   feature starts (`ST_FSINTRO`).
4. `hold_on_snapshot()` - every LUCKY COIN on the grid gets a value (or a
   MINOR/MAJOR label) in `G.hold.val/lab`.

Because the storm and the expanding wilds live here, the simulator sees
exactly what the player sees.

Note for render work: `draw_reels()` draws from the strips (`rpos`,
`stripAt`), not from `G.grid`. Grid changes that are not on the strip - storm
wilds, expanded reels - are drawn separately (`extra_draw_reels`,
`expand_under/over`, and `draw_reels` substitutes SEVEN on an expanded reel).

---

## 6. Evaluation

### score_ways(match, boost, &len, &wt, &mask)

The adjacent-ways counter is a one-pass dynamic programme over the 5x5
grid, cells as bits `r*5+row` of a `uint32_t`:

```
ways[0][row] = match(0,row) ? 1 : 0
wt[0][row]   = ways * (boost(0,row) ? WILD_MULT : 1)
ways[r][row] = match(r,row) ? sum over d in {-1,0,+1} of ways[r-1][row+d] : 0
wt[r][row]   = match(r,row) ? mul(r,row) * sum of wt[r-1][row+d]          : 0
```

It stops at the first reel with no reachable cell. `len` = reels reached;
under `MINCHAIN` (3) there is no win. Ways = sum of `ways` on the last
reel, the weighted count = sum of `wt` there. The winning cells are found by
walking back from the last reel: a cell is on a path if it matches and feeds
a marked cell on the next reel.

`wt` is what pays. Multiplying per cell inside the DP is what makes the wild
doubling exact per path: two paths into the same cell can carry different
numbers of wilds. `WILD_MULT = 2`.

### evaluate()

1. Build `symm[NSYM]`, one bitmask per symbol; `wildm = symm[SY_SEVEN]`.
2. For each paying symbol `s = DIAMOND..LEMON`:
   `score_ways(symm[s] | wildm, wildm, ...)`; the win counts only if its
   path mask includes at least one real `s` cell. `add_win()` records it and
   the cells go into `won`.
3. The wilds' own chain: `score_ways(wildm, 0, ...)` (no boost - the sevens
   are the symbol) paid as SEVEN only if it shares no cell with any other
   win.
4. Count `scatCount`, `bonusCount`, `coinCount`, `wheelCount` anywhere on the
   grid. Three or more scatters add `SCATPAY[n] * TOTBET` (2/10/50x).
5. In free spins, `winTotal *= fsMult` - the whole spin, way wins and
   scatter pay. The per-win `winAmt[]` stays unmultiplied (display code must
   multiply it itself).
6. Progressives: flood-fill (`flood()`, 8-connected, over a bitmask) every
   JACKPOT cluster; the largest decides the tier (5 MINOR, 6 MAJOR, 7+
   MEGA). Any ULTIMATE cluster of 5+ takes the ULTIMATE instead. The tier
   is recorded in `jpWon`, `jpAmt = jp_value(tier)` and **that pot is
   emptied on the spot** (`jpAcc[tier] = 0`).

`add_win()`: `amount = PAY[s][len] * wt * TOTBET / 10`, clamped to 2e9 so
one win fits an `int`; at most `MAXWINS` (12) wins are listed (a grid can
produce at most nine: eight paying symbols and the wild chain), and every
one is added to `winTotal`.

`evaluate()` is called in several places besides the real one - the storm's
candidate scoring and `force_win_search()` - which then restore what it
touched (`grid`, `jpAcc`, `nWin`, `winTotal`, `jpWon`, `jpMask`). **Keep
`evaluate()` free of other side effects** (sound, particles, credits, RNG),
or those callers will repeat them 24 or 800,000 times.

---

## 7. Money

### Bet ladder

`bet_at(i)` generates the 1-2-5 ladder from 10: 10, 20, 50, 100 ... 100,000
at `BETMAXIDX = 12`. `TOTBET` is `bet_at(G.betIdx)`. Every rung is a
multiple of 10, so `PAY/10` and the hold coins' half-bet values are exact.
Start bet is index 2 (50). `max_affordable()` is the highest rung the bank
covers (X spins at it; START trims the bet to it if the bank is short).

### Credits and award()

`G.credits` is `long long`. The bet is taken in `start_spin()` (not in free
spins), which also calls `jp_contribute(TOTBET)`. ADD CREDITS adds 100,
250, 500, 1,000, 2,500 or 5,000 (`ADDS[]`).

`award(amt)` is the one way a prize reaches the bank: it adds `amt`, clamps
to `JP_CLAMP` (999,999,999,999 - twelve digits, what the meters show), sets
`lastWin` (clamped to 2e9) and, in free spins, adds to `fsWon`. See section
14: the current code stores the sum through an `(int)` cast.

### Jackpots

```
enum { JP_ULT, JP_MEGA, JP_MAJOR, JP_MINOR };
JP_MULT = { 100000, 200, 40, 5 }            x total bet
JP_RATE = { 0.030, 0.025, 0.020, 0.025 }    share of every bet (sum 10%)
JP_NEED = { 5, 7, 6, 5 }                    cluster size
jp_value(i) = JP_MULT[i]*TOTBET + jpAcc[i]/1000, clamped to JP_CLAMP
```

`jpAcc` is kept in thousandths of a credit so small bets feed exactly.
`jp_seed()` (at reset) gives each pot a random 0-80% of its multiple, so the
machine looks as if it has been played. The bet multiple is live: change the
bet and all four meters change.

Who pays a pot, and each resets that pot to zero when it pays:

| payer | tiers | where |
|---|---|---|
| reel clusters | all four | `evaluate()` |
| HOLD & SPIN jackpot coins | MINOR, MAJOR | `hold_lock()`, as each coin locks |
| HOLD & SPIN GRAND (25 coins) | MEGA | `hold_settle()` |
| WHEEL pot wedges | MINOR, MAJOR (wheel); MAJOR, MEGA (super wheel) | `wh_take()`, when the prize is taken |

---

## 8. The feature modules

Every module follows the same pattern: a flat state struct in `G`, a
trigger test the queue calls, a `*_begin()` that enters its state, a
`*_update()` called from `update()`'s switch, a `*_draw()` called from
`draw_frame()`, and maths in small functions shared with a `*_sim_play()`.
The show (`*_update`, `*_draw`) only decides when the player sees a result,
never what it is.

### HOLD & SPIN - `w7_hold.c`

Trigger: `hold_triggered()` = `coinCount >= HOLD_NEED` (6).

Coin values (`hold_roll_coin()`): with probability `HOLD_Q_MAJOR` (0.001) a
MAJOR coin, `HOLD_Q_MINOR` (0.011) a MINOR coin, otherwise a value from
`HV_HALF[] = {1,2,3,4,6,10,20,50}` half-bets (0.5x..25x) weighted
`HV_WT[] = {460,290,105,70,38,21,11,5}`. Base-game coins get their values
in `hold_on_snapshot()` and show them on the reels (`hold_draw_cells()`).

Maths functions (used by both game and simulator):

- `hold_setup()` - the triggering grid's coins lock (`hold_lock`); 3 respins.
- `hold_roll()` - spend a respin; each empty cell lands a coin with
  `hold_land_chance(locked)` = `HOLD_P0 * HOLD_PK^(locked-6)`, doubled
  (`HOLD_HOT`) once `HOLD_HOTN` (20) coins are down. `HOLD_P0` 0.027,
  `HOLD_PK` 1.03. Results go into `land/nval/nlab`, not onto the board.
- `hold_lock()` - put a coin on the board. A jackpot coin takes
  `jp_value()` of its tier and empties that pot.
- `hold_after_spin()` - any new coin resets respins to 3; 25 coins = GRAND;
  returns 1 when the feature is over.
- `hold_settle()` - total = every coin, plus the MEGA pot on a GRAND.

Phases (`HP_*` in `G.hold.phase`): `TRIGGER` (coins ring out in reading
order, 1.25 s) -> `INTRO` (transition card) -> `READY` (lamps light) ->
`SPIN` (`hold_spin_start()` rolls, then each empty cell stops on a
schedule, `hold_lock` as it stops; with 4 or fewer cells left each stops
slowly) -> repeat `SPIN` until `hold_after_spin()` -> `GRAND` (3.6 s, full
board) -> `COLLECT` (each coin counted into the meter) -> `SLAM` -> after
3.8 s or a press: `award(total); feature_done()`.

Draw: `hold_draw()` owns the reel window in `ST_HOLD` (a baked board
backdrop `hbk` and a dimmed twin `hbkD`); `hold_draw_cells()` draws coin
values over landed coins in the other reel states. Test hook:
`WILD7_FORCE=hold` (`hold_force_rows()` picks genuine stops showing 7
coins) and `WILD7_HOLD=grand` (only with that force: land chance 0.30 and
common jackpot coins).

Simulator: `hold_sim_play()` runs setup/roll/lock/after_spin/settle with no
clocks; `hold_sim_report()` prints the end-size spread, GRAND rate and
jackpot-coin rates.

### WHEEL OF 7's - `w7_wheel.c`

Trigger: `wheel_triggered()` = `wheelCount >= 3` (one on each of reels 2, 3
and 4 - the spacing makes more impossible).

Tables: `WH_A[24]` (the wheel) and `WH_B[24]` (the SUPER WHEEL), each wedge
`{kind, palette, val, wt}`: `WK_CR` pays `val x bet`, `WK_POT` pays the live
pot `val` (a `JP_*` index), `WK_UP` is SUPER. Wheel: 5x..250x, MINOR,
MAJOR, SUPER (weight 60). Super wheel: 25x..500x, MAJOR, MEGA.

Maths: `wheel_pick(face)` draws a wedge by weight when the spin starts;
`wh_value()` prices it; `wh_take()` pays it and empties a pot.
`wheel_sim_play()` = pick; if SUPER switch to `WH_B` and pick again; take.

Show: `wh_start()` chooses a resting point inside the target wedge
(6%..94% across it), then computes the total travel (4 turns, 5 on the
super wheel, plus what is needed) and a frame count (6-7.4 s, 7.2-8.6 s
super). `WPH_SPIN` advances by `dAng * wh_shape(u) / vSum` each frame, a
velocity profile normalised so the wheel stops exactly on target. The
flapper (`wh_pointer`) is a damped spring pushed by pegs; each peg passed
clicks (`wh_tick`). Phases: `INTRO -> READY` (A/START/B spins; auto-spins
after 6 s, 3.5 s on the super wheel) `-> WIND -> SPIN -> LANDED ->`
(`UPGRADE -> READY` for SUPER) `-> PRIZE -> DONE` with
`award(prize); feature_done()`.

Rendering: both faces are painted once in polar coordinates with two
angularly blurred copies each (`whFace[face][blur]`); an angular blur is
rotation invariant, so motion blur costs nothing per frame. Per frame the
face is inverse-mapped in 16.16 fixed point, one span per row inside the
disc and the band, with a static gloss map. The stage is one pre-rendered
backdrop (`whStage`). The SUPER face is built incrementally a few rows per
frame during the first wheel feature (`wh_face_step`), finished at once if
the upgrade (or a loaded save state) needs it. `wheel_draw()` covers every
pixel, so `render_band()` skips the backdrop copy in `ST_WHEEL`.

### 7 STRIKE and GAMBLE - `w7_extra.c`

**7 STRIKE** (base game only):

- `extra_on_spin_start()` arms a storm with chance 1/`STORM_ODDS` (100;
  compile-time, `-DSTORM_ODDS=0` disables) and fixes the anticipation
  timetable (three sheet-lightning flickers and their sounds).
- `extra_on_snapshot()` (inside `snapshot_grid`): draw the bolt count 3..8
  with weights `STORM_W = {34,26,18,12,7,3}`; eligible cells are anything
  but a feature symbol or an existing wild; try `STORM_K` (24) random
  placements, score each with the real `evaluate()`, keep the one whose win
  is closest in log-ratio to `STORM_T[n] * bet / 10`
  (`{10,17,27,44,70,100}`, i.e. 1x..10x the bet). Restores the pots, writes
  the chosen wilds into `grid`, and records `stormMask`, strike `order[]`
  (roughly top to bottom with a little shuffle) and `stormStop[]`.
- `ST_SPIN` sees `extra_storm_pending()` and calls `extra_storm_begin()`:
  `ST_STORM` with a bolt timetable (first at 0.85 s, gaps shrinking from
  0.52 s to 0.30 s). `extra_storm_update()` strikes each bolt as the storm
  clock passes it (START/A/B triples the clock) and hands over to `ST_EVAL`.
- `extra_draw_reels()` draws clouds, bolts and struck wilds over the reels
  (bolt shapes come from a hash of `seed`, so the draw is pure).

**GAMBLE**: offered in `ST_SHOWWIN` on a counted base-game win with
`gamble_allowed()` = not in free spins and `winTotal <= GAMBLE_CAPX *
bet` (50x). `gamble_begin()` stakes `winTotal` (not yet paid). Phases
`GP_DEAL -> GP_PICK` (LEFT red x2, RIGHT black x2, UP/DOWN choose a suit, X
plays the suit x4; A/START/B or 30 s idle collects) `-> GP_FLIP ->
GP_RESULT` then either `GP_DEAL` again, or `GP_OUT` when `GAMBLE_ROUNDS`
(5) are played or the pot passes the cap; a loss finishes at once with 0.
The card is `irnd(4)`, `irnd(13)`: `irnd(4)` is exactly uniform (a 24-bit
fraction times 4), so every call is a fair bet and the gamble does not move
the RTP; the simulator ignores it. `ex_gamble_finish()` does `award(pot);
feature_done()`.

### FX - `w7_fx.c`

Cosmetic only; state in statics, own RNG `fxRng`. `fx_init()` bakes coin
sprites (4 sizes x 16 rotation frames), stars, glow and ray tables, titles
(BIG/SUPER/MEGA/EPIC...), digit sprites, and enlarged "pop" copies of every
symbol. `FXN` = 1,000 particles, 8 emitters.

- Spawning (update code only): `fx_burst`, `fx_burst_col`, `fx_fountain`,
  `fx_rain`, `fx_firework`, `fx_home`, `fx_shake`, `fx_flash`,
  `fx_transition(title, sub, colour)` (a 1.6 s title card; modules wait on
  `fx_transition_busy()`), `fx_stop()` (stops emitters).
- Draw helpers (render code only): `fx_glow`, `fx_rays`, `fx_rays_ex`,
  `fx_shade`, `fx_ring`, `fx_coin`, `fx_number`.
- Layers: `fx_draw()` (world layer, over reels, under overlays - the win
  bursts) and `fx_draw_top()` (over everything - spawns from modules and the
  transition card). `fx_post()` applies the screen shake to the finished
  frame (serial, after the bands).
- The win presentation, called by the core: `fx_bw_tier(win, bet)` -> 0,
  or 1..4 for 10x/25x/50x/100x (BIG/SUPER/MEGA/EPIC); `fx_bw_count_end` and
  `fx_bw_shown(win, bet, t)` make the count a pure function of time (2.3 s
  per tier line, stalling just short of each); `fx_bigwin_slam`,
  `fx_bigwin_tick`, `fx_win_burst`; `fx_jackpot_run/hold/begin/tick`;
  `fx_multup_begin`; and the draws `fx_light_cluster`, `fx_wins_draw`,
  `fx_bigwin_draw`, `fx_jackpot_draw`, `fx_multup_draw`.

The core keeps three thin wrappers from before the module existed:
`spawn_burst()` (-> `fx_burst_col`), and the empty `update_parts()` /
`draw_parts()`.

### Pick bonus (in the core)

`bonus_fill_panels()`: five credit panels `{7,10,17,25,34} x bet / 10`, one
x2, three STOPs, shuffled with `irnd`. `begin_bonus()` enters `ST_BONUS`.
The round ends on the third STOP; `ST_BONUSEND` pays
`pickTotal * pickMult`. The simulator plays it as a blind player
(`sim_pick()` in `sim.c`, random reveal order) using the same
`bonus_fill_panels()`.

---

## 9. Rendering

Software rendering into `uint32_t fb[1280*720]` (XRGB8888). Nothing is
drawn with a GPU; RetroArch scales the frame and applies the CRT shader.

### The backdrop

`bg[]` is the static cabinet (marquee box, reel window with cream drums,
glass rails and mouldings, the button deck with unlit buttons), copied into
`fb` band by band at the start of every frame. `build_bg()` paints it once
through the ordinary primitives: first theme 1 (the free-spins night set with
gold drums) into `bgFree`, then theme 0 into `bgBase` and `bg`. While
painting theme 0 it also grabs the lit button sprites `btnspr[i][1]`.
`art_update()` copies `bgFree` or `bgBase` into `bg` when the free-spins
look should change (`inFree`, `ST_FSINTRO`, or the free-spins BONUSEND).

Only the values move per frame; panels that never change are in `bg`.

### Sprites and the art pipeline

`spr_t { w, h, px (RGBA, straight alpha), rx0[], rx1[] }` - the per-row
first/last opaque column, so blits skip transparent margins.

Symbol art is authored in 0..92 design units (`U()`) on a 4x supersampled
canvas `canvas[CANW*CANH*4]` (424x424) with `cv_*` (polygons, circles,
rounded rects, spheres, fruit, fire, sparkles, labels), coverage masks `m_*`
and a material shader `cv_shade()` (mask -> blurred height field -> normal
-> lambert, studio environment reflection `env_map`, specular). Every symbol
sits on `cv_medal()`. `render_symbol()` then:

1. `cv_resolve()` box-filters 4x4 down to a 106x106 sprite (`SYMW/SYMH`);
2. `spr_sharpen()` (unsharp mask on the solid interior);
3. `add_contour()` - a uniform dark outline from a box-blurred alpha
   thresholded (a cheap distance field);
4. `spr_bounds()`.

`build_sprites()` makes `sym[]` (crisp), `symfl[4]` (the wild at four flame
phases), `symBig[]` (double-size close-ups of the wild and feature symbols,
via `cv_resolve2`), `symb[]`/`symb2[]` (vertical motion-blur twins from
`make_streak`, for settling and full-speed reels, made before shadows),
then `bake_shadow()` composites a blurred cast shadow into `sym[]` and
`symfl[]`. Also `washspr` (radial alpha for cell tints), `glowspr`, the
pick emblem. The COIN and WHEEL art come from their modules (`art_coin`,
`art_wheel`), which light their own.

### Primitives

All take screen coordinates and clip to the screen and to the current band:
`fb_px`, `fb_blend`, `fb_add`, `fb_rect`, `fb_frame`, `fb_rrectg`/`fb_rrect`
(signed-distance rounded rects that only evaluate the field at the rounded
ends), `fb_rframe`, `fb_line`, `blit` (sprite, vertical clip window,
alpha, tint), `blit_wash`, `blit_add` (additive), `blit_half`,
`blit_scaled`, `shine_sprite`, `screen_tint`/`dim`, `vgrad`, `seg_num`
(seven-segment readouts with ghost segments), `keycap`, `led_window`,
`fb_moulding`, `fb_glass`, `fb_softshadow`, `text`, `text_run`, `textb`.

The blend is one integer formula everywhere,
`(d*(256-a) + s*a) >> 8` with red and blue sharing one multiply
(`blend_rbg`, `blend_px`, `span_blend`, `span_mask`). Using the same formula
in every primitive is what keeps banded frames bit-identical to one-thread
frames.

### Text

Every caption is set in the lounge's faces (Beese's Poker Lounge's), through
`w7_lounge.c`; the old 5x7 `FONT[]` is only the fallback before `lz_init()`.
The game's own calls keep their signatures and metrics - `px` is the size of
a block of the old font, so capitals stand `7*px` tall with their tops at `y`:

- `text(s, x, y, px, col, align, shadow)` and `text_run()` - Barlow
  Condensed (`lz_compat_text`), upper case, fitted to the screen.
- `textb(s, x, y, px, stops, nstops, align)` - the display type: Bungee
  coverage (`lz_caption_mask`) and its outline grown by a distance
  transform (`lz_dilate`), composited as before as shadow, dark outline and
  the gradient body. Cached in `tbc[TBC]` (16 slots, LRU) keyed by string
  and size; under the band renderer the lookup (and a miss's rasterise)
  happens under `bp_lock()`, the entry is pinned, and the composite runs
  unlocked. Strings of 40 characters or more are not drawn at all. Bungee
  is wider than the old blocks: layout code asks `textb_w(s, px)`.
- Baked titles: `bake_title()` renders a caption once into a sprite with
  outline, glow and bevel (`title[TT_*]`, `bigdig[]`, `capSpr[]`), drawn
  with `blit`/`blit_scaled`; the letter shapes are Bungee's.
- `seg_num()` draws a gold Bungee readout (`lz_readout`) instead of seven
  segments.
- The kit's own text (`lz_text`, `lz_text_ex`, `lz_neon`, `lz_gold`; section
  9a) goes through a string cache (`lzc[LZC_N]`, 96 slots, LRU): a string at
  a size is rasterised once into face, outline and glow masks and after
  that only copied, whatever the colours. Same locking as `textb`. Per-glyph
  flicker (`gpow`, the marquee sign) draws uncached.

### Cached full frames

`fcache_t { px, key, pendKey, valid, pend, nomem }`. `frame_cache(c, key,
paint)`: if valid for `key`, copy this band's rows back; otherwise run
`paint()` (which paints the whole frame through the primitives, so it only
writes this band's rows), then store this band's rows. The cache is marked
valid only in `render_commit()`, after all bands have run.
`cache_backdrop(c, paint)` is `frame_cache` with key 0. In use: `ptimg[4]`
(the four pay-table pages; the fourth, CONTROLS, caches only its backdrop
and `wp_controls_draw()` draws the panel live over it) and `bnimg` (the
pick board, keyed by `bonus_key()`: stops, picks, total, multiplier). The
pages and the pick board stand in the lounge's honeycomb room, `ptRoom`,
which `build_pick_assets()` bakes once (`lz_paint_room()` paints a whole
frame, so it cannot run per band); the pick stage `pkStage` is baked from
it. A cached page's glass panels are drawn by `pt_glass()`, lz_glass's
look worked out for the band's rows only (lz_glass rasterises its whole
canvas on every call, once per band). Each is 1280x720x4 = 3.6 MB,
allocated on first use; `NFCPEND` (16) caches may be filled in one frame.

### Draw order - draw_frame()

```
if ST_WHEEL:  wheel_draw()                       (covers every pixel)
else:
  if not (ST_PAYTABLE or ST_BONUS):              (those repaint every row)
     draw_marquee()
     ST_HOLD ? hold_draw()
             : draw_reels(); hold_draw_cells(); extra_draw_reels()
     draw_features()      free spins / multiplier panels, LAST WIN panel
     draw_wins()          fx_wins_draw: lit paths, pops, X2 badges
     draw_parts()         (empty)
     fx_draw()            world-layer particles
     draw_meters()        jackpot ladder, credits/bet/win, deck, SPIN dome
  draw_overlays()         free-spins status bar in the marquee, attract,
                          FS intro, bonus end, broke, add credits, the
                          multiplier pop, BIG..EPIC, jackpot, pay table,
                          pick board
if ST_GAMBLE: gamble_draw()
fx_draw_top()             top-layer particles, transition card
screen flash              G.flash (capped by the limiter)
```

`draw_reels()` draws each reel's five visible cells plus one (from the
strip, offset by the fractional position), using `symb2`/`symb` while
blurred, `symfl[]` for the wild, an accent wash, frame, shine and glint on
special symbols, fire and arcs on the anticipation reel, flame columns on
expanded reels, then the glass sheen over the window.

The multiplier indicators live in `draw_features()` (the rail panel),
`draw_fsbar()` (the free-spins status bar in the marquee) and
`fx_multup_draw()` (the centre-screen pop). They read `G.fsMult`,
`G.pickMult`, `G.multUp` and `G.multFrom`; they change nothing.

### The band renderer - w7_thread.c

```
render()
  bp_run(render_band)     every band, on every drawing thread
  render_commit()         frame caches filled this frame become valid
  fx_post()               screen shake, serial, full frame

render_band(y0, y1)
  clip_y0 = y0; clip_y1 = y1          (thread-local)
  memcpy bg rows y0..y1 into fb       (not in ST_WHEEL)
  draw_frame()
  clip_y0 = 0; clip_y1 = FBH
```

- `bp_config(want, bands)` (from `check_vars()`, between frames only)
  creates `want-1` workers (the main thread is the other one), 512 KB
  stacks, every signal blocked, named `w7band1..` (visible in `top -H`).
  Bands default to 3 per thread (9 for 3 threads), equal heights
  (`bp_layout`); `WILD7_BANDS` overrides. One thread means one band and no
  locks at all.
- `bp_auto_threads()` = min(3, online CPUs - 1), at least 1. The option
  `wild7_threads` = auto|1|2|3|4; `WILD7_THREADS` overrides both.
- `bp_run()` publishes the frame (generation counter + condition variable),
  then the main thread and the workers take bands from an atomic counter
  (`bp_next`) until none are left. A thread that finishes a cheap band
  simply takes another. The main thread spins briefly on the last band
  (4,000 pause/yield) and then sleeps on `bp_done`. Workers sleep between
  frames, so an idle screen does not hold cores busy.
- `bp_active` is 1 only while bands run in parallel; `bp_lock()` guards the
  shared caches (textb, frame caches) only then.
- `clip_y0/clip_y1` are `__thread` (`W7_TLS`). Helpers: `clip_rows(&y0,&y1)`
  narrows a row range to the band, `rows_visible(y0,y1)` tests whether an
  object touches the band, `in_band(y)` tests one row.

### The render contract (summary)

The full text is in `DEVELOPING.md`. In short:

1. Draw code changes no state: no writes to `G`, module statics,
   particles or clocks, no `frnd()`/`irnd()`, no `static` scratch buffers.
   Randomness in a draw must be a hash of stable inputs. Clocks advance in
   update code (`art_update`, module updates) or in `render()` before the
   bands.
2. Write pixels through the primitives. Code that writes `fb[]` directly
   must clip its rows with `clip_rows()` and may only read its own band.
3. Narrow expensive row loops with `clip_rows()` (a loop that relies on
   per-pixel clipping is correct but does its work nine times).
4. Build caches outside the draw, or use `textb()` / `frame_cache()`, which
   are band-safe.
5. Whole-frame work that reads other rows goes in `fx_post()`.
6. Prove it with `sh tools/bandcheck.sh` (all identical).

---

## 9a. The lounge look - w7_lounge.c

WILD 7's shares its look with Tim's other cabinet game, Beese's Poker
Lounge: the "neon honey lounge" - a dark plum honeycomb room, dark glass
panels with a neon tube edge and its glow, neon-sign lettering, Bungee
display type in cream-to-amber gold, Barlow Condensed labels, chasing
bulbs, the bee. The poker game draws it with a GPU; here the same art is
painted on the CPU, once, and blitted.

**Start-up** (`lz_init()`, first in `retro_init()`): the four fonts are
embedded in the core (`LZ_EMBED`, `.incbin` from `W7_ASSETS`), their
glyphs rasterised with stb_truetype at the poker game's sizes (`LZF_*`) and
the glow fonts' glows blurred at half resolution; the sprites (glow, hex
glow, bulbs, the bee) are painted with the poker game's rasteriser
(`w7_raster.c`: signed-distance shapes, paints, blur; `w7_lart.c`: the
motifs). About 90 ms on x86.

**Two kinds of function**, and the difference matters to the render
contract:

- *Build time* - `lz_glass`, `lz_well`, `lz_button`, `lz_bake_dome`,
  `lz_reel_frame`, `lz_paint_room`, and anything on an `LCanvas`. They
  allocate and paint the whole frame; call them from `build_*` / `paint_*`
  code that runs once (or from a frame cache's paint), never per frame.
- *Draw code* - `lz_text*`, `lz_neon`, `lz_gold`, `lz_readout`, `lz_icon`,
  `lz_dot`, `lz_add_tint`, `lz_bulbs_row`: no state, every row loop clipped
  to the band.

The base cabinet uses it (search `lzReady` in `wild7_libretro.c`):
`paint_backdrop` (the room; free spins light it in night blue),
`paint_marquee_box`, `build_marquee` / `draw_marquee` (the "Beese's WILD
7'S" neon sign - the lettering is drawn live so single letters can flicker
from a hash of the clock - and bulbs chasing either side), `paint_rails`
(glass rails, gold readouts, the CONTROLS crib by position), `glass_rail`,
`rail_panel`, `led_window`, `paint_reel_window` (brass and honey round the
window), `paint_deck`, `paint_button` (neon buttons with the panel icon),
`build_dome` / `spin_dome` (the SPIN button).

## 10. Audio - w7_audio.c

Everything runs on the main thread: `sfx_*`/`snd*` from update code,
`audio_frame()` once per `retro_run`. No locks. The synth has its own RNG, so
sound on or off never changes an outcome.

### Voices

One struct, `avoice_t`, plays every sound: oscillator type, envelope, pitch
sweep, optional filter, pan, reverb send, start delay. Types:

| id | type |
|---|---|
| `SND_SQUARE` 0 | PolyBLEP square |
| `SND_TRI` 1 | triangle |
| `SND_SAW` 2 | PolyBLEP saw |
| `SND_NOISE` 3 | white noise through a one-pole low-pass at f0 |
| `SND_BELL` 4 | two-operator FM bell |
| `SND_SINE` 5 | sine (table) |
| `SND_PLUCK` 6 | Karplus-Strong string |
| `SND_WHOOSH` 7 | band-passed noise, centre swept f0 -> f1 |
| `SND_BRASS` 8 | two detuned saws through an enveloped low-pass |

Envelopes: `ENV_LEGACY` (the original fixed attack/release) or `ENV_ADSR`
(`aud_perc()` for struck sounds, `aud_adsr()` for held ones). Sweeps:
`SW_LIN`, `SW_EXP` (drop), `SW_LOG` (musical glide).

Control rate: a frame (735 samples) is 35 blocks of 21 samples (`AUD_SUB`);
envelope, pitch and filter coefficients are computed per block and the
amplitude ramped across it. A voice's start delay is quantised to a block
(0.48 ms). Voices are rendered one at a time over the whole frame.

Pools: 64 effect voices (bus 0) and 32 music voices (buses 1 and 2), so
neither can starve the other. `aud_alloc()` takes a free voice, else steals
the quietest sounding one (a queued note only if everything else is
louder); the stolen voice's last output is faded over ~3 ms by a residue on
its bus, so steals do not click. `aud_release_tag(tag)` fades a group (the
anticipation riser uses `TAG_ANTIC + reel` and is released when the reel
lands); `aud_release_bus()` clears a music player.

### Mix and master (`aud_master`)

```
bus 0 (effects) + g1*bus1 + g2*bus2 (music players, crossfading)
reverb send sum -> 12 ms pre-delay -> 4 damped combs + 2 allpasses per side
(L+R + reverb) * AUD_MASTER (0.5)
DC blocker (~3 Hz)
peak limiter at AUD_LIM 0.80 (-1.9 dBFS), fast attack, ~0.3 s release
soft clip from AUD_KNEE 0.80 to AUD_CEIL 0.96 (-0.35 dBFS)
-> int16 stereo abuf[]
```

Music sits at `AUD_MUSGAIN` (0.4) and is ducked by a side-chain on the
effects bus peak and by explicit `music_duck(level, secs)` holds.

### Music sequencer

`TRK[NTRACK]` - `TR_LOUNGE` (base game), `TR_FREE`, `TR_PICK`, `TR_HOLD`,
`TR_WHEEL`. A track is `{bpm, bpm_hi, swing, level, lanes[8]}`; a lane is
`{instrument, vol, pan, minI, cresc, pattern}`. Patterns are text, one token
per sixteenth note, parsed once by `mus_parse()` into `mus_ev[]`
(`MEVMAX` 4096 events in total, repeats expanded; about 1,700 are used):

```
C4  F#3  Bb2       a note (A..G, #/b, octave)
A3+C4+E4           a chord (up to 4 notes)
.                  rest
-                  tie (extends the previous note)
tok*N              repeat N times
>tok               accent
drum lanes:        letters, several = simultaneous hits:
  k kick, s snare, c clap, h closed hat, o open hat, b brush, y ride,
  a shaker, r roll pair, C crash, l/d heartbeat, T taiko, t tom
```

Lanes loop independently. A lane with `minI > 0` only plays once the
track's intensity reaches it; tempo slides from `bpm` to `bpm_hi` with
intensity; `swing` delays every second eighth; `cresc` ramps velocity over
the lane. Instruments (`W7I_*`: EP, UPRIGHT, VIBES, PLUCK, SAWBASS, PAD,
BRASS, BELL, MARIMBA, TIMP, STRING, SUB, DRUM) are defined once in
`aud_inst()`/`aud_drum()` and shared by music and effects.

`music_auto()` chooses the tune every frame from `G.state`: silence under
`ST_JACKPOT` and `ST_BONUSEND` (the fanfare is the music), `TR_PICK` in
`ST_BONUS`, `TR_HOLD` in `ST_HOLD` (and `ST_GAMBLE`, at intensity 0.35),
`TR_WHEEL` in `ST_WHEEL` (intensity rising over 5 s), `TR_FREE` in free
spins with intensity `(fsMult-1)/4`, the lounge otherwise; attract and
broke play quieter (`mus_scene`). `music_intensity(x)` lets a module hint a
higher intensity for a quarter second. Two players (`mp[2]`) crossfade: the
old one fades over 1.2 s, the new one in over 0.12 s.

`aud_watch()` reacts to state changes instead of threading calls through the
flow: the win jingle when a small win shows, the roll-up "ding" when the
count completes, bonus-end stingers, the broke sound, and a soft tick as
each symbol passes on a spinning reel.

### Adding a sound

1. From update code (never a draw function), call the building blocks:
   `snd(f0, f1, dur, type, vol)`, `snd_at(delay, ...)`,
   `snd_noise(dur, vol, lp)`, `snd_chord(t0, a, b, c, dur, vol)`,
   `snd_inst(delay, W7I_*, hz, len, vel, pan)`,
   `snd_drum(delay, code, vel, pan)`. `vol` is linear: 0.05 is a click, 0.2
   a lead note. Queue a whole phrase at once with delays.
2. For anything reusable, add an `sfx_name()` in `w7_audio.c` (inside the
   file you can use `sv()`/`aud_new()`, which return the voice so you can
   set `q`, `glide`, `send`, envelope and pan) and its prototype, marked
   `AUDAPI`, in `w7_audio.h`.
3. Big moments: `music_duck(level, secs)` to pull the music down under them.
4. Listen and measure: `WILD7_FORCE=... ./w7shot -n N -w out/a.wav`
   prints peak/RMS/DC; `./w7audio stat out/a.wav` gives the loudest 400 ms
   and the largest sample jump (a click); `./w7audio` benches the mixer.
   The master limiter will squash an over-loud phrase rather than clip it,
   so check the level rather than trusting the absence of distortion.

`opt_sound` off silences everything and resets the synth; `opt_music` off
stops only the music buses.

---

## 11. The maths and src/sim.c

### What the simulator does

`./w7sim N [betIdx [FS_AWARD [FS_MAXMULT]]]` plays N base spins at one bet
(seeded `rngs = 0xC0FFEE`, so a run is repeatable). For each spin:

- `extra_on_spin_start()` (may arm a storm), random reel stops
  (`spin_reels()`: `rpos = irnd(96)`, `snapshot_grid()`), `evaluate()`;
- a reel jackpot is taken (`take_jackpot`);
- base wins, scatter pay, per-symbol/per-length hit and pay statistics;
- `hold_sim_play()` if 6+ coins, `wheel_sim_play()` if 3 wheels,
  `sim_pick()` if 3 crowns;
- free spins: `FS_AWARD` spins with `inFree=1`, each spin running the same
  path (hold and wheel can trigger inside; retriggers add `FS_RETRIG`),
  collecting the multiplier histogram.

It does not play the pick bonus inside free spins (the game does) and
ignores the gamble (fair by construction).

### Two ways to count the jackpots

- **SEEDS (default).** Every pot is emptied before each spin, so every
  jackpot, hold coin and wheel pot pays exactly its bet multiple. The
  headline is
  `(everything won except ULTIMATE) + 10% contributions + ULTIMATE multiple
  x its exact probability`. Rationale: a growing pot is only a transfer of
  contributions, and letting pots grow counts that money twice (as the
  contribution and again as a hold/wheel/reel prize); and a finite sample
  either hits the 1-in-7.9-million ULTIMATE or not, which swings the figure
  by whole percent.
- **`W7SIM_POTS=grow`.** Pots grow and pay as they fall, the pre-v3 way.
  2M spins at bet 10 gave 88.2%, low because the sample holds no ULTIMATE.

### Exact enumeration

`enumerate_jackpots()`: JACKPOT only lands on reels 2-4, so all 96^3 =
884,736 combinations of those stops are flood-filled to get the MINOR,
MAJOR and MEGA probabilities exactly. ULTIMATE shows at most once per
window, so the chains are counted directly (consecutive reels within one
row of each other) over 96^5. `./w7sim jp` prints only these;
`./w7sim jp MINOR MAJOR MEGA [cnt24 cnt3 stk24 stk3]` tries other cluster
sizes and JACKPOT counts/stacks. (The exact figures cover the base game
without 7 STRIKE; storms never overwrite feature symbols.)

The final block prints a second, structural figure: game (base + free spins
+ pick + hold + wheel, measured) + contributions (10%) + each reel
jackpot's multiple x its exact probability. It is the same at every bet by
construction, which is the point of bet-multiple jackpots.

### Current numbers

`./w7sim 5000000 0`, run for this document on 2026-09-23 (WSL, current
tree):

```
spins            5000000   at bet 10
RTP              93.25%   LONG RUN: 81.98% prizes and seeds (ULTIMATE aside)
                            + 10.00% contributions + 1.271% ULTIMATE seed, exact odds
  base game      37.90%   of which scatter pays 1.11%
  free spins     25.58%
  pick bonus     4.79%
  hold & spin    5.47%   1 in 246 spins
  wheel bonus    3.99%   1 in 387 spins
  hold & spin    mean prize 13.4 x bet (pots 1.1 x), 8.1 respins
                 ends with 6-8 coins 45.5%  9-14 42.8%  15-24 11.64%  GRAND 0.059% (1 in 1695 features)
                 a MINOR coin in 10.46% of features, a MAJOR in 1.00%
  7 strike       1 in 101 spins
  jackpots       6.42%   (as they fell in the sample)
    ULTIMATE     sampled 1 in 5000000      exact 1 in 7870393        mean pot 1000000
    MEGA         sampled 1 in 11933        exact 1 in 13011          mean pot 2000
    MAJOR        sampled 1 in 2770         exact 1 in 2930           mean pot 400
    MINOR        sampled 1 in 384          exact 1 in 401            mean pot 50
hit frequency    24.66%  (1 in 4.1 spins)
free spins       1 in 210 spins
pick bonus       1 in 262 spins
RTP              game 77.73% + contributions 10.00% + bet multiples:
   ULTIMATE      MEGA     MAJOR     MINOR   multiples        RTP
     1.271%    1.537%    1.365%    1.247%      5.420%     93.15%
free-spin mult   x1:36.4% x2:33.1% x3:18.5% x4:8.0% x5:3.9%
way pays total   36.79%  (base game, before scatter pays)
```

The same run at 8,000,000 spins gives **93.86%** (free spins 26.36%),
which is the 93.9% in the README and docs/DESIGN.md; 1,000,000 spins at the top bet
(`betIdx 12`) gave 94.89%. The spread between these is Monte-Carlo noise
from the free spins, not a bet effect: use 5M+ spins and compare like with
like. At about 2 s per million spins in WSL, a 10M run is cheap.

The per-symbol table at the end of the output has a `per 1` column: the RTP
added by raising that `PAY` entry by one unit (0.1x bet), so retuning the
pay table is arithmetic.

### Retuning - the knobs

| knob | where | notes |
|---|---|---|
| `PAY[sym][len]` | core | per 10 credits of bet (0.1x bet units). Check `per 1` in the sim output. |
| `SCATPAY[]` | core | x total bet for 3/4/5 scatters. |
| `CNT`, `STK` | core | strip make-up. Any change reshuffles every strip: re-check every trigger rate, the coin trigger and `./w7sim jp`. |
| `FS_AWARD`, `FS_RETRIG`, `FS_MAXMULT` | core, variables | sim args 3 and 4 sweep `FS_AWARD` and `FS_MAXMULT` without a rebuild. |
| `WILD_MULT` | core, `#define` | the per-wild path multiplier (2). Very sensitive. |
| `JP_MULT`, `JP_RATE`, `JP_NEED` | core | `JP_NEED` sweepable through `w7sim jp`. |
| pick pool `{7,10,17,25,34}`, x2, 3 stops | `bonus_fill_panels()` | |
| `HOLD_NEED`, `HOLD_SPINS`, `HV_HALF`, `HV_WT`, `HOLD_Q_MINOR/MAJOR`, `HOLD_P0`, `HOLD_PK`, `HOLD_HOT`, `HOLD_HOTN` | `w7_hold.c` | land chance and value table. |
| `WH_A`, `WH_B` values and weights | `w7_wheel.c` | weights are not wedge sizes. |
| `STORM_ODDS`, `STORM_W`, `STORM_T`, `STORM_K` | `w7_extra.c` | measure the storm's share with a `-DSTORM_ODDS=0` build of the sim: `gcc -O2 -DSTORM_ODDS=0 -Isrc -o w7sim_nostorm src/sim.c -lm -lpthread`. |
| `GAMBLE_CAPX`, `GAMBLE_ROUNDS` | `w7_extra.c` | no RTP effect. |

After any change: run the sim at 5M+ spins, check the hold/wheel/free/pick
trigger rates and `./w7sim jp`, update the figures in README.md, docs/DESIGN.md and docs/GAME_GUIDE.md, and (for
display changes) `tools/bandcheck.sh`.

---

## 12. Core options and the .w7 file

Core options (`VARS[]`, RetroArch Quick Menu -> Core Options):

| key | values | default | effect |
|---|---|---|---|
| `wild7_sound` | on/off | on | `opt_sound` |
| `wild7_music` | on/off | on | `opt_music` (effects stay on) |
| `wild7_turbo` | off/on | off | `opt_turbo`: reel speed 13 -> 20 symbols/s, settle 0.55 -> 0.34 s, stop gaps halved |
| `wild7_limiter` | on/off | on | `opt_limiter`: caps flashes, shake, pulsing, lightning |
| `wild7_threads` | auto/1/2/3/4 | auto | band renderer threads; the pool is rebuilt in `check_vars()` |

`check_vars()` runs at load and whenever RetroArch reports an option
change (`GET_VARIABLE_UPDATE` at the top of `retro_run`).

The `.w7` "ROM" is plain text read in `retro_load_game()`, after
`check_vars()`. The parser lower-cases the first 512 bytes and reads one
`key=value` per line (spaces around either side are fine); lines starting
with `;` or `#` are comments. A key can only move a setting AWAY from its
default, so the file never undoes what the player set in Core Options:

| line | effect |
|---|---|
| `sound=off` | sound off |
| `music=off` | music off |
| `turbo=on` | turbo on |
| `limiter=off` | limiter off |
| `credits=N` | starting bank, if 0 < N <= 1,000,000,000 |

There is no threads key. (Until 3.0.1 this was a substring search over the
whole text, so the shipped file's own comment `; turbo=on|off` switched
turbo on on every cabinet.) No content at all (`supports_no_game`) is
fine: defaults apply. `w7shot -g FILE` loads a file and prints what it set.

---

## 13. Test hooks

All read in `retro_init()`; unset means normal play. They drive the real
input/update/render path.

| variable | values | effect |
|---|---|---|
| `WILD7_AUTOPILOT` | 1 | spin loop: START at frame 40, then START every 70 frames and A 35 frames later (slams and collects) |
| | 2 | START at 40, SELECT at 80 (pay table) |
| | 3 | START at 40 (leaves attract) and 110 (one spin), then nothing |
| | 4 | START 40, Y 80, RIGHT 120 and 140 (ADD CREDITS chooser) |
| | 5 | START 40, SELECT 80 and 160 (page 2, FEATURES) |
| | 6 | START 40, RIGHT every 12 frames from 61 to 209 (climbs the bet ladder to the top) |
| | other | same as 1 |
| `WILD7_FORCE` | see below | chooses genuine reel stops that show the demanded symbols |
| `WILD7_HOLD` | `grand` | with `WILD7_FORCE=hold` only: respins land coins at 30% and jackpot coins are common |
| `WILD7_THREADS` | 1..4 | overrides the threads option |
| `WILD7_BANDS` | N | bands per frame (1..64) |
| `WILD7_PROFILE` | 1 | render/update timing line to stderr every 300 frames |

`WILD7_FORCE` values (`dbg_force`):

| value | dbg_force | demand |
|---|---|---|
| `free` | 1 | a SCATTER on row 3 of reels 1-3 (every spin, including free spins) |
| `pick` | 2 | a CROWN on row 3 of reels 1, 3, 5 |
| `win` | 3 | CHERRY on row 3 of reels 1-3 |
| `mega` | 4 | JACKPOT stacks aligned on reels 2-4 (2+3+2 = 7) |
| `minor` | 5 | JACKPOT on reels 2 and 3 (2+3 = 5) |
| `ult` | 6 | ULTIMATE on row 3 of every reel |
| `hold` | 7 | 7 LUCKY COINS (`hold_force_rows`) |
| `wheel` | 8 | WHEEL on row 3 of reels 2-4 |
| `storm` | 9 | arm a 7 STRIKE on every base spin |
| `fsmult` | 10 | scatters into free spins, then a wild on reel 3 every free spin |
| `big` / `super` / `megawin` / `epic` | 20 / 21 / 22 / 23 | when reel 1 settles, `force_win_search()` tries up to 800,000 random full stops through `evaluate()` for a win of 10-25x / 25-50x / 50-100x / 100x+ bet with no jackpot, scatter or crown trigger; the reels then stop there |
| anything else (e.g. `major`) | 0 | nothing |

For demands 1-10, when a reel starts to settle `update()` walks the strip
from the natural stop for the first position that satisfies the demand
(`force_demand()`); there is no MAJOR hook.

Build/runtime knobs outside the core: `W7SHOT_OPTS="wild7_threads=1,..."`
(core options for w7shot), `W7SIM_POTS=grow` (simulator), and the compile
switches in section 1. `tools/bandcheck.sh` takes `FRAMES`, `EVERY`,
`THREADS`, `SCEN` (autopilot:force pairs), `REF` (a git rev or a tree to
compare against), `SHOTCFLAGS`, `CC`, `RUN`.

`w7shot` options: `-n` frames, `-s a,b,c` or `-s every:N` frames to save,
`-o` dir, `-i "frame:btn+btn;..."` scripted input (ignored under
autopilot), `-p` prefix, `-r` seed, `-w` WAV, `-H N` hash every Nth frame
(`-q` digest only). It prints init time and mean/max/p50/p95 ms per frame.

---

## 14. Known limits, bugs and gotchas

### Fixed in 3.0.1

Found while this document was being written, by reading the code and
proving each one with a small harness under `out/`; all fixed the same
day.

- **The anticipation hold almost never fired.** It requires
  `G.rt1[r] == 0`, but `rt1` also holds the stop target, and
  `start_spin()` did not clear it; only the attract loop did. So the extra
  0.95 s hold and `sfx_anticipation()` happened on the first spin after
  attract and then stopped (2 holds in 280 two-scatter spins; 247 in 248
  once cleared). The same code was in v2.2. `start_spin()` now zeroes it.
- **`award()` truncated credits to int** (`G.credits = (int)c`), so a bank
  past 2,147,483,647 went negative - reachable, since the ULTIMATE at bet
  100,000 is ten billion. Now a `long long` store. The free-spins
  multiply of `winTotal` is also done in 64 bits and clamped to 2e9 like
  `add_win()`.
- **The shipped `wild7.w7` turned turbo on and never set credits.** The
  parser searched for substrings, so the comment `; turbo=on|off` switched
  turbo on, and `; credits=N` was found before `credits=5000`. Now a
  line-by-line `key=value` parser (section 12).
- **HOLD & SPIN music did not build.** `music_auto()` counted
  `G.hold.val[]` (the coins that triggered it) instead of the board;
  it now uses `G.hold.locked`, so the heartbeat quickens and the strings
  and taiko come in as the grid fills.
- **A larger save state from another build loaded as garbage**; now only a
  state of exactly `sizeof(game_t)` loads.
- **Stale strings**: the version is 3.0.1 in `retro_get_system_info` and
  the `.info` file (which now also lists the new features and 93.9%), and
  the out-of-date comments (pays per 10 credits, 7 STRIKE one spin in a
  hundred, the coin trigger rate, the Makefile's build time) are corrected.

### Still true

- **`retro_reset` ignores the `.w7`**: it restores 5,000 credits.
- **The game RNG is not in `G`**, and hold/wheel update code uses `frnd()`
  for cosmetic bursts, so the random stream (and the next outcomes) depend
  on how long the player lets presentations run. Harmless, but do not build
  a test that assumes a fixed outcome sequence across different inputs; the
  FX module uses its own RNG for exactly this reason.
- **`textb` limits**: 16 cached masks shared by all threads (each thread pins
  at most one at a time); a frame with many distinct captions thrashes the
  cache and re-rasterises under the lock. Strings of 40+ characters are
  silently not drawn.
- **Full-frame buffers are 3.6 MB each**: `fb`, `bg`, `bgBase`, `bgFree`,
  `whStage`, `pkStage`, `ptRoom` (plus a transient copy while `build_pick_assets`
  runs) and up to five frame caches (`ptimg[0..3]`, `bnimg`) that
  are allocated on first use and kept - about 40 MB of the whole. Measured
  peak RSS of `w7shot` on x86: 75 MB (spin loop) to 88 MB (pick bonus),
  before RetroArch's own footprint. The Pi has 905 MB in total and about
  390 MB free under EmulationStation (DEVELOPING.md). Add new full-frame
  caches sparingly.
- **Build time**: the one-unit `-O3` build takes about 3.5 minutes on the
  Pi 4. Iterate in WSL (`make && make sim shot`) and build on the Pi once.
- **The autopilot cannot finish the pick bonus**: it only presses START/A,
  and `pickCur` never moves off the first panel, so after one pick nothing
  happens. Use `-i` scripted input in w7shot to drive the board.
- **`WILD7_FORCE=free` never ends free spins**: the demand also applies in
  free spins, so every free spin retriggers. Use `fsmult` for a feature that
  ends.
- **`force_win_search()` can take seconds in one frame** (up to 800,000
  evaluations). Test hook only.
- **The simulator is built `-O2` without `-ffast-math`** while the core is
  `-O3 -ffast-math`. The evaluator and triggers are integer maths, so
  results should agree, but the storm's log-ratio choice and the hold land
  chance use floats; nobody has proved the two builds bit-identical.
- **The simulator does not play the pick inside free spins**; the game can.
  The effect on RTP is small (a crown trigger inside a short feature).
- **The first WHEEL feature does extra work**: the SUPER face is built a
  few rows per frame while the first wheel runs, and the upgrade forces the
  rest. w7shot shows 20 ms max frames in that scene on x86 (mean 1.4 ms).
- **The installer rewrites the port's `retroarch.cfg`** every time (see
  OPERATIONS).
