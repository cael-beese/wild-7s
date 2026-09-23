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
- Rendering is being split into horizontal bands across **3 threads**,
  leaving one core for RetroArch (video, audio, input) and thermal headroom.
- Budget, measured single-threaded in `w7shot` on x86: **mean <= 5 ms,
  max <= 8 ms for any scene** you add or change.  That is ~20 ms serial on
  the Pi, ~8-9 ms once banded - half the 16.7 ms frame, which is the safe
  overhead we want.  Report the numbers for your scenes.
- Big per-pixel full-screen effects are the expensive thing (921,600 px).
  Pre-render into sprites at init; blit at run time.  Additive glows and
  rays: bake a sprite, don't evaluate sqrt per pixel per frame.

## The render contract (this is what makes threading possible)

1. **Draw code must not change state.**  Nothing in a `draw_*` / `*_draw`
   / `paint_*` function may write `G`, module statics, particles, timers
   or call `frnd()` / `irnd()` (the RNG is game state and not thread
   safe).  Advance animation clocks and spawn effects in UPDATE code.
   Any randomness in a draw must be a hash of stable inputs
   (cell index, particle id, time).
2. **Write pixels through the primitives** (`fb_px`, `fb_blend`, `fb_add`,
   `fb_rect`, `fb_rrect*`, `fb_rframe`, `fb_line`, `blit*`, `text*`,
   `seg_num`, `screen_tint`...).  If you write `fb[]` directly, clamp
   rows to `[clip_y0, clip_y1)` first - the band renderer runs every draw
   function once per band with a different clip.
3. **Caches are built outside the draw**: at init, or in update code.
   (`textb` and the existing backdrop caches are handled by the engine.)
4. `render()` runs the draw list; `fx_post()` runs after all bands join,
   single-threaded, for whole-frame effects such as shake.

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
