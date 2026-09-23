# WILD 7's — design notes

The long-form design document: how each rule came to be, what the
simulator caught, how the graphics, sound and renderer work, and the
performance history. The project front page is [the README](../README.md).

An original five-reel, five-row **adjacent ways** video slot, written as a
libretro core. Appears in EmulationStation under **Ports**, runs through
RetroArch with your controller config, save states and CRT shader.

```
make                       # about 3.5 minutes on the Pi 4 (one big -O3 unit)
sudo ./install-retropie.sh
```

Renders **1280x720, 16:9**. The core declares its own aspect and the installer
sets `aspect_ratio_index = 22` (core provided), so RetroArch takes the shape
from the core rather than forcing 4:3.

**Version 3** is the "Hollywood" build: four new features (HOLD & SPIN,
WHEEL OF 7'S, the 7 STRIKE wild storm and a GAMBLE), BIG / SUPER / MEGA /
EPIC win celebrations with 3-D coin showers, new art throughout, stereo
sound with music, and a renderer that draws each frame on three of the
Pi 4's four cores.

## Documentation

| | |
|---|---|
| [GAME_GUIDE.md](GAME_GUIDE.md) | the player's guide: controls, every feature, **every multiplier and when it goes up and down**, the jackpots, the odds |
| [ARCHITECTURE.md](ARCHITECTURE.md) | how the code works: state machine, reels and evaluation, modules, renderer, audio, the maths |
| [OPERATIONS.md](OPERATIONS.md) | running the cabinet: build, deploy, launch, soak, backups, rollback |
| [DEVELOPING.md](../DEVELOPING.md) | the rules for changing it: build commands, frame budget, the render contract |
| this file | the design and the history of how the game got here |

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
| WILD 7  | 0.4 | 1.5 | 14.8 |
| DIAMOND | 0.3 | 0.9 | 4.2  |
| BELL    | 0.2 | 0.6 | 2.5  |
| BAR     | 0.2 | 0.5 | 1.7  |
| GRAPES  | 0.1 | 0.3 | 1.1  |
| ORANGE  | 0.1 | 0.3 | 0.9  |
| PLUM    | 0.1 | 0.2 | 0.5  |
| CHERRY  | 0.1 | 0.2 | 0.4  |
| LEMON   | 0.1 | 0.2 | 0.3  |

Those are the figures before the multipliers. A five-reel diamond win with
four ways and two wilds in it pays 4.2 x 4 ways x 4 for the wilds, which is
67 times the bet. The feature symbols - SCATTER, CROWN, JACKPOT, ULTIMATE,
LUCKY COIN and WHEEL - pay nothing on a way; they start things.

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

**HOLD & SPIN** — six or more LUCKY COINS anywhere. Every coin shows its
value as it lands: half the bet up to 25 times it, or now and then a MINOR
or MAJOR coin that pays that jackpot. The coins lock, every other cell
becomes its own little reel, and you get **three respins; every new coin
locks in and resets them to three**. It ends when they run out. Fill all 25
cells and it is the GRAND: the MEGA jackpot on top of every coin. The coins
are then counted into the meter one by one. About one spin in 250; the mean
prize is about 13 times the bet.

**WHEEL OF 7'S** — a WHEEL on each of reels 2, 3 and 4 brings on a 600-pixel
wheel ringed with chasing bulbs. A, START or B spins it (it spins itself
after six seconds, three and a half on the SUPER WHEEL). Twenty-four wedges pay 5x to
250x the bet, the MINOR and MAJOR jackpots, or SUPER, which swaps in the
SUPER WHEEL (25x to 500x, the MAJOR and the MEGA) for another spin. The
outcome is drawn from a weighted table the moment it starts spinning, and
the wheel is then driven to stop inside that wedge, so what you see is
what you get. About one spin in 390.

**7 STRIKE** — at random, about one base-game spin in a hundred, a storm
gathers over the reels while they spin. When they stop, three to eight
lightning bolts strike, and every cell they hit becomes a WILD 7. Wilds
double every path through them, so random wilds would pay wildly; the
storm scores candidate placements through the real evaluator and keeps one
near a target that rises with the bolt count. Nearly every storm pays at
least the bet.

**GAMBLE** — after any base-game win up to 50 times the bet, press X.
LEFT calls RED and RIGHT calls BLACK for double; UP / DOWN chooses a suit
and X plays it for four times. A, START or B collects. Up to five rounds.
The card is an exactly uniform draw, so the gamble is a fair bet and does
not move the return.

A spin can trigger several of these at once. Its own wins are shown and
paid first, then the features run one after another: HOLD & SPIN, the
WHEEL, the PICK, then FREE SPINS. (Before version 3 a spin that opened the
pick round or the free spins never paid its own line wins or scatter pay.)

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
| MEGA     | 1 in 13,011 |
| MAJOR    | 1 in 2,930 |
| MINOR    | 1 in 401 |

The simulator plays the ladder end to end and reports the return at each
rung; it comes out identical at all of them.

## Controls

| RetroPad | Action |
|---|---|
| Start / A | Spin, and slam the reels down mid-spin |
| Left / Right (or Up / Down) | Bet |
| X | Highest rung the bank covers, and spin. On a counted win: GAMBLE |
| Y | Add credits — choose 100 to 5,000 |
| Select | Pay table; Select again for FEATURES, again for MORE FEATURES |
| B | Slam / cancel |
| D-pad, A | Move and pick in the pick round; A (or START, B) spins the wheel |
| Any button | During a big-win count, jump to the total; again to collect |

If the bet is more than the bank, Start trims it to the highest rung you can
afford rather than refusing. Out of credits? The OUT OF CREDITS screen comes
up; START, A or Y there opens the ADD CREDITS chooser: pick 100, 250, 500,
1,000, 2,500 or 5,000 and press A.

## The spin

The reels turn at half the speed they used to, and they stop **one at a
time with an uneven gap between them: half a second to two seconds, re-rolled
every spin**. Nothing about a spin is on a fixed rhythm, so the last reel is
always worth waiting for, and a spin runs about six seconds end to end. If
that is too slow, START, A or B slams the lot down at any point, and the
turbo core option halves the gaps rather than removing them.

The anticipation hold still sits on top: when two scatters, two crowns or
two wheels are already showing, the last reels are held back an extra
second, and the held reel burns: fire at its foot, electric arcs, the other
reels dropped into shadow.

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

**Version 3** keeps the medallion look and lights all of it properly. The
symbols are built from masks with a real bevelled, lit surface - a molten
7 with fire round it, a faceted diamond with coloured fire, embossed gold
bell, BAR, star and crown, glossy fruit - and the feature symbols shine and
glint on the reels. The cabinet is a lit stage with light beams and bokeh,
smoked-glass rails in chrome mouldings, a neon progressive sign in tier
colours, illuminated buttons, and a marquee with searchlights and three
bulb patterns; free spins swap in a night-sky cabinet with gold drums.

Wins are staged the way a casino floor does it (`src/w7_fx.c`): a thousand
particles, including gold coins pre-rendered in sixteen 3-D rotation frames
that tumble and bounce on the deck; the winning symbols pop and beads of
light run along each path; and above ten times the bet the count climbs
through **BIG, SUPER, MEGA and EPIC WIN**, stalling at each line before
the title slams up a tier with a shockwave, shake and a heavier coin
fountain. Jackpots get god rays, coin rain and, for the ULTIMATE,
fireworks. Every bonus opens with a title slam.

What follows describes the version 2 foundations, which still hold.

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
`bonus_fill_panels`, same jackpot trigger - and each new feature plays out
through its own module's `*_sim_play()`, which uses the very functions the
game does. Eight million spins at bet 10 (version 3):

```
RTP              93.9%     long run, identical at every bet from 10 to 100,000
  base game      37.8%     including 7 STRIKE, one spin in 100
  free spins     26.4%     one spin in 210
  pick bonus      4.8%     one spin in 262
  hold & spin     5.4%     one spin in 246
  wheel           4.0%     one spin in 386
  + the pot seeds the reels, coins and wedges pay, the tenth of every bet
    that feeds the pots, and the ULTIMATE at its exact odds
hit frequency    24.7%     1 in 4.1 spins
```

**How that figure is counted.** HOLD & SPIN and the WHEEL can pay the pots
too, and a Monte-Carlo run that lets the pots grow counts that money twice:
once as the feature's prize, and again in the tenth of every bet that fed
the pot. It also swings by whole percent on whether the sample happened to
hit an ULTIMATE. So the simulator holds every pot at its seed (the bet
multiple), then adds the contributions, which every pot eventually pays
out, and the ULTIMATE's seed at its exact enumerated odds.
`W7SIM_POTS=grow ./w7sim` gives the old as-they-fell figure.

Getting there from 96.5%: 7 STRIKE went from one spin in 80 to one in 100,
and the five-reel DIAMOND and BELL pays came down (4.8 to 4.2, 2.8 to 2.5).

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

**Version 3** draws each frame as nine horizontal bands pulled by three
threads (`src/w7_thread.c`); every draw function runs once per band with
its own clip rows, and `tools/bandcheck.sh` proves the result is identical,
pixel for pixel, to drawing it on one thread. Auto uses three of the Pi 4's
four cores, leaving one for RetroArch. Soaked on the Pi 4 with
`tools/pi_soak.sh`, paced at 60 fps, against a 16.67 ms budget:

| | render mean | max | RetroArch CPU |
|---|---|---|---|
| spin loop, 1 thread | 9.1 ms | 30.3 ms | 78% |
| spin loop, 3 threads | 6.6 ms | 21.0 ms | 137% |
| HOLD & SPIN, 3 threads | 7.0 ms | 20.4 ms | 141% |
| EPIC win coin shower, 3 threads | 9.2 ms | 24.5 ms | 180% |

The maxima are single frames - a caption or cached screen being built the
first time - and steady play stays at 9-13 ms. Unpaced it runs at 104 fps.
The SoC peaked at 56 C with the clock held at 1,800 MHz: it never
throttled. On the cabinet itself, with the 3440x1440 display and the CRT
shader, RetroArch uses about two cores.

The version 2 figures, single-threaded, for comparison:

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

RetroArch Quick Menu -> Core Options. `wild7.w7` can also set `sound=off`,
`music=off`, `turbo=on`, `limiter=off` and `credits=N` (the starting bank),
one per line; `;` lines are comments. It can only move a setting away from
its default, so it never undoes a Core Options choice. (Until 3.0.1 the
file's own comment line switched turbo on.)

- **Sound** (default on)
- **Music** (default on) — the background tunes; the effects stay on
- **Turbo spin** (default off) — faster reels (20 symbols a second instead
  of 13), a quicker settle, and half the gaps between reel stops
- **Flash limiter** (default on) — damps the win flashes, the shake and the
  storm's lightning
- **Render threads** (default auto = 3 on a Pi 4) — how many cores draw the
  frame; auto leaves one for RetroArch

## Test hooks

Off unless the environment variable is set, so they cannot affect normal play.
They drive the real input/update/render path; `WILD7_FORCE` picks a genuine
reel stop that happens to show the demanded symbols.

```
WILD7_AUTOPILOT=1   spin loop        WILD7_FORCE=free    land 3 scatters
WILD7_AUTOPILOT=2   open pay table   WILD7_FORCE=pick    land 3 crowns
WILD7_AUTOPILOT=3   a single spin,   WILD7_FORCE=win     a 3-reel cherry line
                    then no input    WILD7_FORCE=minor   5 JACKPOTs  -> MINOR
WILD7_AUTOPILOT=4   open ADD CREDITS WILD7_FORCE=mega    7 JACKPOTs  -> MEGA
WILD7_AUTOPILOT=5   features page    WILD7_FORCE=ult     5 ULTIMATEs -> ULTIMATE
WILD7_AUTOPILOT=6   climb the ladder WILD7_FORCE=hold    6+ LUCKY COINS -> HOLD & SPIN
                                     WILD7_FORCE=wheel   3 WHEELs -> WHEEL OF 7'S
                                     WILD7_FORCE=storm   a 7 STRIKE on every base spin
                                     WILD7_FORCE=big|super|megawin|epic  a genuine stop in that win band
                                     WILD7_FORCE=fsmult  free spins with a wild every spin
WILD7_HOLD=grand    with FORCE=hold: coins land freely, to reach the GRAND
WILD7_THREADS=1..4  override the thread option     WILD7_PROFILE=1  render/update ms to stderr
WILD7_BANDS=N       bands per frame (tuning)       FORCE=free retriggers every free spin: use fsmult for one that ends
```

On the Windows box, `make shot` builds `w7shot`, a headless host that runs
the real `retro_run()` loop and writes PNG frames (and `-w` a WAV) - see
`DEVELOPING.md`. `tools/bandcheck.sh` proves the threaded renderer draws
exactly what one thread draws; `tools/pi_soak.sh` soaks it on the Pi while
logging temperature, throttling and per-core load.

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
setsid -f nohup ~/w7launch.sh >/dev/null 2>&1 < /dev/null
```
