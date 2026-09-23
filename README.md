# WILD 7's — RetroPie build

An original five-reel, five-row **adjacent ways** video slot, written as a
libretro core. Builds on the Pi in a few seconds, appears in EmulationStation
under **Ports**, runs through RetroArch with your controller config, save
states and CRT shader.

```
make
sudo ./install-retropie.sh
```

Renders **1280x720, 16:9**. The core declares its own aspect and the installer
sets `aspect_ratio_index = 22` (core provided), so RetroArch takes the shape
from the core rather than forcing 4:3.

## How it pays

A 5x5 grid, and no paylines. **A win reads left to right from reel 1: one
symbol per reel, each on the next reel to the right, in the same row as the
last or one row up or down.** It never steps within a reel and never back to
the left. Three, four or five reels pay, and where a reel offers two
connecting symbols each path is its own way, so the pay multiplies by the
number of paths. Every symbol is scored on its longest reach.

WILD 7 stands in for every paying symbol, **and every wild in a win doubles
it**. Two wilds in one win pay four times over, three pay eight times. The
doubling is applied per path rather than per win, so paths that run through a
wild are boosted and paths that avoid it are not, even when both feed the
same cell. A run of pure wilds that shares no cell with another win pays as
sevens, without doubling itself.

One total bet per spin, on a 1-2-5 ladder that starts at 10 and **has no top
rung of its own**:

```
10  20  50  100  200  500  1000  2000  5000  10000  20000  50000  100000
```

BET MORE keeps climbing to 100,000 a spin. That ceiling is arithmetic rather
than a rule of the game: it is the point where the top jackpot, a hundred
thousand times the bet, still fits its meter exactly. Every rung is a
multiple of 10, because pays are quoted per 10 credits of bet and divide
exactly at all of them.

## Symbols

Pays are multiples of the total bet, per way.

| | 3 reels | 4 reels | 5 reels |
|---|---|---|---|
| WILD 7  | 0.4 | 1.8 | 17.5 |
| DIAMOND | 0.3 | 1.0 | 5.6  |
| BELL    | 0.2 | 0.7 | 3.3  |
| BAR     | 0.2 | 0.6 | 2.0  |
| GRAPES  | 0.1 | 0.4 | 1.3  |
| ORANGE  | 0.1 | 0.3 | 1.1  |
| PLUM    | 0.1 | 0.2 | 0.6  |
| CHERRY  | 0.1 | 0.2 | 0.5  |
| LEMON   | 0.1 | 0.2 | 0.4  |

Those are the figures before the multipliers. A five-reel diamond win with
four ways and two wilds in it pays 5.6 x 4 ways x 4 for the wilds, which is
90 times the bet.

## Bonus rounds

**FREE SPINS** — three or more SCATTER stars anywhere award six free spins
with **expanding wilds and a climbing multiplier**. A single wild landing on
reel 2, 3 or 4 takes that whole reel, all five cells, and **notches the
multiplier meter up one**. The meter never falls back during the feature, so
wilds early are worth the rest of the spins; it tops out at **x5**, and it
times the whole spin on top of the x2 each wild already pays. Three more
scatters inside the feature add three spins. Scatters also pay 2x / 10x / 50x
the total bet for 3 / 4 / 5.

Where the meter ends up over a feature, measured:

| x1 | x2 | x3 | x4 | x5 |
|---|---|---|---|---|
| 36% | 33% | 19% | 8% | 4% |

**LUCKY 7 PICK** — three CROWNS (they only land on reels 1, 3 and 5) open a
nine-panel pick round. Panels hide credits, a x2 multiplier, or one of three
STOPs; the round runs until the third stop, so you usually get four or five
picks. The multiplier applies to everything you collected.

## Progressive jackpots

Four progressives run down the left rail, and all four are **won from the
reels**, so what you are chasing is on the pay table rather than in a hidden
roll:

**Every pot is a multiple of the bet being played, plus everything fed into
it since that tier last paid.** Raise the bet and all four meters jump on the
spot.

| | won by | worth | fed by |
|---|---|---|---|
| ULTIMATE | five ULTIMATE symbols touching, one on every reel | **100,000 x bet** | 3.0% of each bet |
| MEGA     | 7 or more JACKPOT symbols touching | 200 x bet | 2.5% of each bet |
| MAJOR    | 6 JACKPOT symbols touching | 40 x bet | 2.0% of each bet |
| MINOR    | 5 JACKPOT symbols touching | 5 x bet | 2.5% of each bet |

**A tenth of every bet feeds the pots.** At the smallest bet of 10 the
ULTIMATE starts at exactly 1,000,000; at 100,000 a spin it starts at ten
billion.

The bet multiple is the fix for something that made the previous build
absurd. The pots used to be seeded at a flat 50 / 500 / 5,000, so winning
the MINOR on a 10,000 bet paid fifty credits, and the return quietly fell as
the bet rose. Because a symbol trigger lands at the same rate whatever the
bet, a prize proportional to the bet contributes the same percentage at
every rung: **the machine now returns the same figure whether it is played
at 10 or at 100,000.**

The jackpot tiers are the one place "touching" means any direction, side or
corner: JACKPOT lives on reels 2, 3 and 4 only, so a left-to-right chain
could never reach five. Confining it to that band of fifteen cells is also
what makes a seven-cluster so much rarer than a five-cluster. Reel 3 carries
one stack of three, the reels either side a stack of two and a single, so
every tier is reachable and the test hooks can land them on purpose.
ULTIMATE is a single symbol per reel (two on the outer reels), so five
touching means every reel showing its one in a chain — which is what a
1,000,000 seed has to cost.

Because JACKPOT only ever appears on three reels, `src/sim.c` does not have to
guess at these odds: it **enumerates every combination of those reels'
stops** (96^3) and every ULTIMATE chain exactly.

| | exact odds |
|---|---|
| ULTIMATE | 1 in 7,870,393 spins |
| MEGA     | 1 in 18,432 |
| MAJOR    | 1 in 5,745 |
| MINOR    | 1 in 397 |

The simulator plays the ladder end to end and reports the return at each
rung; it comes out identical at all of them.

## Controls

| RetroPad | Action |
|---|---|
| Start / A | Spin, and slam the reels down mid-spin |
| Left / Right (or Up / Down) | Bet |
| X | Highest rung the bank covers, and spin |
| Y | Add credits — choose 100 to 5,000 |
| Select | Pay table; Select again for the FEATURES page (the wild multiplier, both bonuses, the jackpots) |
| B | Slam / cancel |
| D-pad, A | Move and pick in the bonus round |

If the bet is more than the bank, Start trims it to the highest rung you can
afford rather than refusing. Out of credits? Start opens the ADD CREDITS
chooser: pick 100, 250, 500, 1,000, 2,500 or 5,000 and press A.

## The spin

The reels turn at half the speed they used to, and they stop **one at a
time with an uneven gap between them: half a second to two seconds, re-rolled
every spin**. Nothing about a spin is on a fixed rhythm, so the last reel is
always worth waiting for, and a spin runs about six seconds end to end. If
that is too slow, START, A or B slams the lot down at any point, and the
turbo core option halves the gaps rather than removing them.

The anticipation hold still sits on top: when two scatters or two crowns are
already showing, the last reels are held back an extra second.

## Sound

Everything you hear is synthesised in `src/w7_audio.c`: no samples, no
dependencies, stereo at 44.1 kHz.

**The synth.** One voice type plays everything: band-limited (PolyBLEP)
square and saw, triangle, sine, a two-operator FM bell for the casino
"ding", a Karplus-Strong plucked string, swept band-pass noise for whooshes
and risers, and a "brass" of two detuned saws through a low-pass that opens
with the envelope. Each voice has an attack/decay/sustain/release envelope,
a pitch sweep (linear, exponential drop, or musical glide), a place in the
stereo field and a send to a stereo hall reverb. The reel stops are panned
to their reel. Sixty-four voices for effects and thirty-two for music, in
separate pools so neither can starve the other; when a pool is full the
quietest voice is stolen and faded out over 3 ms rather than cut. The
master bus is DC-blocked, then a peak limiter, then a soft clip whose
ceiling is -0.35 dBFS, so the biggest fanfare squashes instead of
cracking. The synth has its own random numbers: sound on or off can never
change what the reels do.

A voice can be handed a **start delay**, so a phrase is written the way a
music box is punched: all of its notes queued at once at their offsets, and
the mixer holds each one back until its moment.

**The music** is a small sequencer playing tunes written as text in the
source, one lane per instrument, and the game state picks the tune every
frame, crossfading between them and ducking under loud effects:

| | |
|---|---|
| base game | a quiet ii-V lounge in F: tine piano, walking upright bass, brushed ride, vibes |
| FREE SPINS | four on the floor, Am F C G, pumping octave bass and a pluck arpeggio; a shaker joins at x2 and brass stabs at x3 |
| LUCKY 7 PICK | a playful oom-pa in F with a marimba tune |
| HOLD & SPIN | a heartbeat over a pulsing D pedal; the tempo climbs as the grid fills and strings, ticking and taiko come in |
| WHEEL | a snare roll swelling over timpani and a brass suspension |
| jackpots, bonus ends | silence under the fanfare, which is the music |

**The effects.** Spinning up is a whoosh and a lever clunk; each reel lands
with a thunk pitched and panned to its reel, and a slam rolls the five of
them rather than stacking them. A scatter or a crown landing rings a note
that climbs with every one showing, and the third fires the trigger chord.
While a reel is held back for a bonus a riser, a rising string pair and a
heartbeat play until it lands, and the music drops away. Wins get a jingle
scaled to their size - two bells, an arpeggio, a fanfare - and BIG, HUGE
and EPIC fanfares above that; the roll-up ticks climb in pitch and finish
on a register "ding-ding". The bet buttons climb a pentatonic scale with
the bet, and ADD CREDITS is a cash-register cha-ching and coins in the
tray.

Each jackpot has its own phrase, built to be told apart with your back to
the machine. They get longer, lower-rooted and denser as the prize grows,
and they are now brass over timpani, landing on cymbals and a sub drop:

| | |
|---|---|
| MINOR | three notes up and gone, about a second |
| MAJOR | a four-note arpeggio into a held chord over a bass note |
| MEGA | a triple-tongue call, then a chord with two octaves of bass under it |
| ULTIMATE | a crash, an eight-note run, two big chords a bar apart, and ten bells on the way out — about five seconds |

Under all of them the coin ticks climb in pitch while the amount rolls up,
and the ULTIMATE gets six seconds of them rather than three.

The multiplier has its own sound, and **its root rises with the level** — C6
at x2 up to C7 at x5 — so the ear knows how high the meter went without
reading the panel. The x2 panel in the pick round uses the same voice.

`w7shot -w out.wav` records everything the core hands RetroArch and prints
its peak, RMS and DC offset; `make audio` builds `w7audio`, which times the
mixer at its worst case, checks every voice type for clicks at start and
end, and (`w7audio stat a.wav`) measures a recording.

## Graphics

Every symbol sits on a **chrome-ringed, domed medallion** — the language of a
modern cabinet's premium symbols. The ring gives a hard, bright edge against
the cream drum, the dome gives the piece depth, and a gloss arc across the
top sells the plastic. The symbols themselves are lit vector art: a flaming
red 7 with a gold rim on a green disc, a faceted diamond on deep blue, a gold
bell on red, a bevelled gold BAR plaque on steel, and glossy fruit — grapes,
orange, plum, cherries, lemon — each a lambert-shaded sphere or ellipsoid
with a specular hot spot from the upper left. The feature symbols carry a
ribbon naming them: SCATTER, BONUS, JACKPOT, ULTIMATE.

Every symbol is vector art defined in the source and rasterised at load on a
4x supersampled canvas, then box-filtered down to a 106x106 RGBA sprite. Each
sprite gets a uniform outer contour — its alpha is box-blurred and
thresholded, which is a cheap signed-distance field — and a baked cast
shadow. Each also gets a vertically blurred twin, which is what you see
while a reel is at speed.

A win is lit the way it was made: every cell on a winning path is tinted and
framed, and a line is threaded from each lit cell to the lit cells it feeds
on the reel to its right — never within a reel, never leftwards. Every wild
in the win wears a gold **X2** badge, so the doubling is visible rather than
something to work out from the total. The right rail's LAST WIN panel names
the featured win — symbol, reels, ways, pay and the wild boost — and cycles
through them.

The multiplier meter is the loudest thing in the rail once it is live. The
panel takes the colour of its tier, ice blue through green and gold to pink,
the rim breathes, and the number pops up a size and settles back each time it
climbs. The climb itself gets the middle of the screen for a second: a plate
that rises as it fades, the new multiplier in big gold type, gold rays
turning behind it, a coin burst at the meter and a three-note rise.

The marquee is live. A ticker carrying the rules and the current ULTIMATE
pot scrolls behind a lit title sign; a shine sweeps the sign every couple of
seconds; the lettering pulses from gold to white-hot; two colours of bulb
chase round the edge and sparkles twinkle across the face. Nothing on a
slot's top box should stand still, because the top box is what pulls a
player across the room. It runs on its own clock, so it never pauses when a
state timer resets, and the flash limiter damps all of it.

Display type is drawn by stamping an antialiased disc at every set pixel of a
glyph and welding neighbours together with capsules, so the strokes come out
smoothly rounded rather than blocky. The BET, WIN, CREDITS and jackpot
readouts are seven-segment with ghost segments behind; ULTIMATE gets the big
window.

## The maths

`src/sim.c` includes this core's own source, so the evaluator under test is
literally the one that ships — same strips, same way counter, same
`snapshot_grid` (which is where expanding wilds live), same
`bonus_fill_panels`, same jackpot trigger. Eight million spins at bet 10:

```
game             75.25%     base 41.47 / free spins 28.99 / pick bonus 4.79
contributions    10.00%     the tenth of every bet that feeds the four pots
bet multiples     4.31%     what the house puts up: 5x, 40x, 200x, 100,000x
RTP              89.55%     at every bet on the ladder, 10 through 100,000
hit frequency    33.18%     1 in 3.0 spins
free spins       1 in 210 spins
pick bonus       1 in 262 spins
```

Two thirds of the return comes off the reels and the other third through the
jackpots, which is what a tenth of every bet feeding the pots buys. The free
spins carry nearly as much as the base game: they are one spin in two
hundred and pay 39% of everything that comes off the reels.

The way counter is a one-pass dynamic programme: a cell that matches
inherits the path counts of the three cells beside it on the reel to the
left, so 25 cells score in 25 additions and the lights get the cells on any
winning path from a walk back. A second, weighted count rides along in the
same pass, multiplying each path by two for every wild it runs through. Doing
it there rather than afterwards is what makes it exact, because two paths
reaching the same cell can carry different numbers of wilds.

Things the simulation caught that reading the code did not:

- **With cluster scoring, the first strips paid 581%** — stacks of three on
  an eight-connected grid made a five-cluster land on three spins in four —
  and with way scoring the same strips paid **120%**, almost half of it
  lemons: a five-reel lemon chain landed on 9% of spins, because a lemon
  was one cell in five. Lemons are now one in eight, and the low pays are
  tiny per way because the ways multiply.
- **Singles never cluster.** With JACKPOT laid down as loose singles, a
  five-cluster had probability exactly zero — the strips space same symbols
  apart, so a window never shows more than three. The tiers only exist
  because the JACKPOT stacks are seated deliberately.
- **The pick bonus was landing twice as often as the table said**, 1 in 113
  against 1 in 264, because the shuffle happened to drop two crowns within
  five positions on one reel, and that reel then showed two at once. The
  single-cell specials — scatter, crown, ultimate — are now spaced evenly
  round each strip, so a window shows at most one and the trigger odds are a
  property of the count rather than of the seed.
- **The multiplier had never done anything.** `fsMult` was set to 1 when free
  spins began and never touched again, so the MULTIPLIER panel read X1 for
  the whole feature and the one line of code that used it was dead. Giving it
  a job, together with the wild multiplier, took the return from 85% to
  **340%** at a stroke, free spins alone to 192%: an expanded reel is five
  wilds, every path through it doubles, and the meter then multiplied the
  lot. Getting back took the bet ladder onto clean tens (which doubled the
  pay granularity), a 30% cut to the pay table, six free spins instead of
  ten, and a cap of x5.
- **The jackpots were worth less the more you bet.** The pots were seeded at
  fixed credit amounts, so a MINOR won at a 10,000 bet paid fifty credits,
  and the seed's share of the return fell from 6.1% at bet 10 to 0.06% at
  bet 10,000. Making every tier a multiple of the bet, and tripling the feed
  to a tenth of every bet, flattened the return to one figure across the
  whole ladder. It also cost 15% off the pay table to pay for it.

## Performance

Measured on the Pi with `retroarch --max-frames=1800` under the null video
driver, against a 16.67 ms budget:

| | ms/frame | budget |
|---|---|---|
| base game, spinning | 10.0 | 60% |
| pay table | 12.1 | 73% |
| free spins, expanding wilds and the meter | 12.9 | 77% |
| the ULTIMATE celebration | 14.9 | 89% |

The live marquee costs 2.0 ms of that, the wild badges and the multiplier
banner about 0.3 ms. The whole sound engine - forty-eight busy effect
voices, the music and the reverb - is about 0.3 ms a frame on x86 in
`w7audio`, so roughly 1.3 ms on the Pi, against nine hundred
thousand pixels. Twenty-five 106px sprites a frame
come to fewer pixels than the old fifteen at 152px, and every per-frame
optimisation from the 5x3 build carries over: rounded-rect primitives that
only evaluate the distance field at their corners, cached bubble-type masks,
pay table and bonus board painted once and blitted, cast shadows baked into
the sprites, per-row sprite spans.

## Options

RetroArch Quick Menu -> Core Options, or edit `wild7.w7`:

- **Sound** (default on)
- **Music** (default on) — the background tunes; the effects stay on
- **Turbo spin** (default off) — halves the gaps between reel stops
- **Flash limiter** (default on) — damps the win flashes

## Test hooks

Off unless the environment variable is set, so they cannot affect normal play.
They drive the real input/update/render path; `WILD7_FORCE` picks a genuine
reel stop that happens to show the demanded symbols.

```
WILD7_AUTOPILOT=1   spin loop        WILD7_FORCE=free    land 3 scatters
WILD7_AUTOPILOT=2   open pay table   WILD7_FORCE=pick    land 3 crowns
WILD7_AUTOPILOT=3   a single spin,   WILD7_FORCE=win     a 3-deep cherry band (needs stacked cherries)
                    then no input    WILD7_FORCE=minor   5 JACKPOTs  -> MINOR
WILD7_AUTOPILOT=4   open ADD CREDITS WILD7_FORCE=mega    7 JACKPOTs  -> MEGA
WILD7_AUTOPILOT=5   features page    WILD7_FORCE=ult     5 ULTIMATEs -> ULTIMATE
WILD7_AUTOPILOT=6   climb the bet ladder to the top
```

Frames can be captured headlessly, without disturbing EmulationStation, by
running the core under a null video driver and asking RetroArch for a
screenshot at the end of a fixed frame count:

```
retroarch --config <null-driver cfg> -L wild7_libretro.so wild7.w7 \
          --max-frames=300 --max-frames-ss --max-frames-ss-path=/tmp/shot.png
```

The one thing this cannot show is the CRT shader, which needs a real GL
context; that has to be looked at on the cabinet.

## Launching it from SSH

Replacing the installed core does nothing to a game that is already running
— RetroArch keeps the copy it loaded. `~/w7launch.sh` on the Pi quits
EmulationStation cleanly, waits for the display, starts the game, retries if
it lost the race for the screen, and restarts tty1's autologin afterwards so
EmulationStation comes back when the player quits:

```
setsid nohup ~/w7launch.sh >/dev/null 2>&1 < /dev/null &
```
