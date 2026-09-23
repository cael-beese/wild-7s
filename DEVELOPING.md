# WILD 7's - developing it

Shared rules for everyone working on the core (people and agents alike).
The game is one C translation unit: `src/wild7_libretro.c` includes the
feature modules (`src/w7_*.c`), and `src/sim.c` / `tools/w7shot.c` include
the core, so the simulator and the frame dumper run the code that ships.

## Build and look (WSL Debian on the Windows box, or the Pi itself)

Call WSL from PowerShell, not Git Bash (Git Bash rewrites `/mnt/c` paths):

```
wsl -d Debian -- bash -lc "cd /mnt/c/path/to/Wild7s && make && make sim shot"
./w7sim 2000000 0                       # RTP at bet 10 (betIdx 0)
WILD7_AUTOPILOT=1 ./w7shot -n 600 -s 120,300,599 -o out
WILD7_FORCE=free  WILD7_AUTOPILOT=1 ./w7shot -n 900 -s every:60 -o out
./w7shot -n 400 -i "40:start;110:a;300:x" -s 399 -o out    # scripted buttons
```

`w7shot` writes PNGs you can open, and prints mean / max ms per frame.
`make` must build with **no new warnings** (`-Wall -Wextra`).

## Frame budget - the Pi 4 is the target

- The Pi 4 (4x Cortex-A72 @1.5-1.8 GHz) runs about **4x slower per core**
  than WSL here: v2.2 was 3.0 ms/frame in `w7shot`, ~10-13 ms on the Pi.
- Rendering is split into horizontal bands across **3 threads** (the
  `wild7_threads` core option, `auto` = min(3, cores - 1)), leaving one
  core for RetroArch (video, audio, input) and thermal headroom.
  `W7SHOT_OPTS=wild7_threads=N` (or `WILD7_THREADS=N`) picks it in
  `w7shot`.  Measured in `w7shot` built like the Pi build (`-O3
  -ffast-math`), best of 3, mean ms per frame:

  | scene            | v2.2 | 1 thread | 2    | 3    | 4    |
  |------------------|------|----------|------|------|------|
  | spin loop        | 3.04 | 2.99     | 1.37 | 1.07 | 1.09 |
  | free spins       | 4.57 | 3.75     | 1.81 | 1.72 | 1.56 |
  | MEGA jackpot     | 4.79 | 4.22     | 2.13 | 1.67 | 1.70 |
  | ULTIMATE jackpot | 4.05 | 3.73     | 1.81 | 1.49 | 1.43 |
  | ADD CREDITS      | 7.14 | 5.00     | 2.62 | 1.98 | 1.83 |
  | pick bonus       | 3.91 | 1.24     | 0.66 | 0.59 | 0.55 |
  | pay table        | 3.40 | 0.79     | 0.71 | 0.53 | 0.50 |

- Budget, measured **single-threaded** in `w7shot` on x86
  (`W7SHOT_OPTS=wild7_threads=1`): **mean <= 5 ms, max <= 8 ms for any
  scene** you add or change.  That is ~20 ms serial on the Pi, ~8 ms
  once banded - half the 16.7 ms frame, which is the safe overhead we
  want.  Report the numbers for your scenes; `w7shot` also prints p50 and
  p95, which shrug off the odd preempted frame on a busy box.
- On the Pi itself, `tools/pi_soak.sh` runs the core headless in
  RetroArch while logging temperature, throttle flags, clocks, per-core
  load and the core's own render timing (`WILD7_PROFILE=1`).
- Big per-pixel full-screen effects are the expensive thing (921,600 px).
  Pre-render into sprites at init; blit at run time.  Additive glows and
  rays: bake a sprite, don't evaluate sqrt per pixel per frame.

## The render contract (this is what makes threading possible)

`render()` cuts the frame into bands (9 for 3 threads) and every thread
runs the WHOLE draw list, `draw_frame()`, once per band it takes, with
its own thread-local clip rows `[clip_y0, clip_y1)` (`src/w7_thread.c`).
Draw code does not know it is threaded, but it must play by these rules.
(While the pay table or the pick board is up, `draw_frame()` skips the
cabinet under it, since those screens repaint every row; a new opaque
full-screen state can join that test.)

1. **Draw code must not change state.**  Nothing in a `draw_*` / `*_draw`
   / `paint_*` function may write `G`, module statics, particles, timers
   or call `frnd()` / `irnd()` (the RNG is game state and not thread
   safe).  No `static` scratch buffers in draw functions either - every
   thread shares them.  Advance animation clocks and spawn effects in
   UPDATE code (or in `render()` before the bands, like the marquee's
   `mqT`).  Any randomness in a draw must be a hash of stable inputs
   (cell index, particle id, time).
2. **Write pixels through the primitives** (`fb_px`, `fb_blend`, `fb_add`,
   `fb_rect`, `fb_rrect*`, `fb_rframe`, `fb_line`, `blit*`, `text*`,
   `seg_num`, `screen_tint`, `vgrad`...).  They all clip to the band's rows
   before their loops, so a band pays only for its own rows.  If you
   write `fb[]` directly you MUST keep to the band:
   `int y0=top, y1=bottom; if(!clip_rows(&y0,&y1)) return;` then loop
   `y0..y1`.  Only read `fb` pixels in your own band.
3. **Narrow your own row loops too.**  A loop over every row that calls
   `fb_blend` per pixel is still correct, but it runs in full once per
   band (9 times) and only the writes are clipped.  If it does real work
   per pixel (`sinf`, `fmodf`, `sqrtf`), wrap its rows in `clip_rows()`
   - see the shimmer in `draw_marquee()`.  `rows_visible(y0,y1)` skips a
   whole object, but only when `[y0,y1)` truly bounds everything it draws.
4. **Caches are built outside the draw**: at init, or in update code.
   The engine handles two kinds for you, safely from any band:
   `textb()` masks (shared, locked, 16 slots - keep a frame's distinct
   captions well under that) and full-frame caches, `cache_backdrop(&c,
   paint)` / `frame_cache(&c, key, paint)`, where `paint` repaints the
   WHOLE frame through the primitives; each band paints and stores its
   own rows and the cache commits after the join.  Each is 3.6 MB and
   the Pi has ~390 MB free: keep them few.
5. `fx_post()` runs once, serially, after all bands join, with the full
   clip: the place for anything that reads other rows (shake, blur).
6. **Prove it.**  `sh tools/bandcheck.sh` renders a spread of scenes at 1
   and at 2/3/4 threads and compares a hash of every sampled frame; it
   must end `all identical`.  Add your scene with
   `SCEN="1:free 5:" sh tools/bandcheck.sh` (autopilot:force pairs), and
   `REF=<rev>` compares against an older revision as well.

## Game-state rules

- `game_t G` is saved by `memcpy` (save states): flat data, no pointers.
  Module state lives in its own struct inside `G` (`G.hold`, `G.wheel`,
  `G.extra`).  Purely cosmetic state (particles) stays out of `G`.
- Features queue: `evaluate()` counts triggers, `ST_EVAL` shows and pays
  the spin's own wins first, then `next_feature()` runs HOLD, WHEEL, PICK,
  FREE SPINS in that order.  A module ends its feature with `award(x)`
  then `feature_done()`.
- Every feature's maths must be reachable by `src/sim.c` through the
  module's `*_sim_play()` (same functions as the game, never a copy).
- Test hooks (`WILD7_AUTOPILOT`, `WILD7_FORCE`) never touch normal play.

## Style

Match the existing file: C99, two-space indent, comments that explain
*why* in full sentences, `static` everything, no new dependencies beyond
libm and pthreads.  New code and comments in ASCII.  The Makefile must still cross-build
for Android (`make android`).
