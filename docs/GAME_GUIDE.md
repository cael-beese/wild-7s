# WILD 7's - the player's guide

Everything about playing WILD 7's on the cabinet: the screen, the
controls, how a win is made, every multiplier and exactly when it goes up
and comes back down, every bonus, the jackpots, and the numbers behind
them. (How the code works is in `ARCHITECTURE.md`; running the cabinet is
in `OPERATIONS.md`.)

Contents

1. The screen
2. Controls
3. Credits and the bet
4. How a win is made (adjacent ways)
5. The symbols and what they pay
6. MULTIPLIERS - every one, when it goes up, when it comes down
7. The spin, and what happens after it
8. FREE SPINS
9. LUCKY 7 PICK
10. HOLD & SPIN
11. WHEEL OF 7'S
12. 7 STRIKE
13. GAMBLE
14. The progressive jackpots
15. Big-win celebrations
16. The attract loop, the pay table pages, options
17. The numbers: return, odds, how often things happen
18. Questions

---

## 1. The screen

```
+----------------------- MARQUEE (ticker, logo) ------------------------+
| PROGRESSIVE JACKPOTS |                                | CREDITS        |
|  ULTIMATE            |                                | BET            |
|  MEGA / MAJOR / MINOR|        THE REELS  5 x 5        | WIN            |
|----------------------|                                |----------------|
| FREE SPINS|MULTIPLIER|                                | CONTROLS       |
|----------------------|                                |----------------|
| HOW TO WIN           |                                | LAST WIN       |
+-- PAYS -- BET LESS -- BET MORE -- BET MAX -- ADD CREDITS ----- (SPIN) -+
```

- **Marquee** (top): the ticker scrolls the rules and the live ULTIMATE
  jackpot. During free spins it turns into the free-spins bar: spins
  left, the multiplier (with MAX when it is at the top), and what the
  feature has won so far. During 7 STRIKE a storm rolls over it.
- **Left rail, top**: the four progressive jackpots, live. Every figure
  is a multiple of your bet plus what has built up, so they all jump when
  you change the bet.
- **Left rail, middle**: the FREE SPINS counter and the **MULTIPLIER
  meter** (see section 6 - it has a five-lamp ladder and a caption that
  tells you what it is doing).
- **Left rail, bottom**: HOW TO WIN - the ways rule, the wild, and what
  starts each feature.
- **Right rail**: CREDITS, BET and WIN readouts; the CONTROLS crib; and
  **LAST WIN**, which spells out the win being shown (see section 6).
- **Button deck**: PAYS, BET LESS, BET MORE, BET MAX, ADD CREDITS, and the
  big SPIN dome. They light as you press the matching controller button.

## 2. Controls

| Button | In the base game | Elsewhere |
|---|---|---|
| START or A | Spin. During a spin: slam the reels down | Pick a panel (pick bonus); spin the wheel; collect (gamble) |
| B | Slam the reels down | Cancel (ADD CREDITS); collect (gamble) |
| LEFT / RIGHT (or DOWN / UP) | Bet less / bet more | Move (pick bonus); RED / BLACK (gamble) |
| X | BET MAX: the highest bet your credits cover, and spin | On a win that has finished counting: **GAMBLE** it. In the gamble: play the chosen suit |
| Y | ADD CREDITS | |
| SELECT | Pay table; SELECT again for FEATURES, again for MORE FEATURES | |
| Any button | Leaves the attract loop | During a big-win count: jump to the total, then again to collect |

If your bet is more than your credits, START trims the bet to the
highest one you can afford instead of refusing. At zero credits START
opens ADD CREDITS.

## 3. Credits and the bet

- You start with **5,000 credits** (set in `wild7.w7`).
- **ADD CREDITS** (Y) offers 100, 250, 500, 1,000, 2,500 or 5,000; pick
  with LEFT / RIGHT, A to add, B to cancel. They are play credits - this
  is a home cabinet.
- **One total bet per spin**, on a 1-2-5 ladder:
  `10 20 50 100 200 500 1,000 2,000 5,000 10,000 20,000 50,000 100,000`.
  Every pay in the game is a multiple of the total bet, so the game plays
  exactly the same at every rung; only the size of the numbers changes.

## 4. How a win is made (adjacent ways)

There are no paylines. A win is a chain of the same symbol that:

- starts on **reel 1** (the leftmost),
- takes **one symbol from each reel**, moving one reel to the right each
  step,
- and each step stays in the **same row, or moves one row up or down**.

Three, four or five reels in a chain pay. It never moves within a reel,
and never back to the left.

```
reel:   1   2   3   4   5
row 1   .   .   .   .   .
row 2   B   .   B   .   .        B - B - B - B  is a 4-reel BELL chain:
row 3   .   B   .   B   .        each step goes one reel right and one
row 4   .   .   .   .   .        row up or down (or stays level)
row 5   .   .   .   .   .
```

**Ways.** Where a reel offers two or three bells that each connect, every
separate path is its own "way", and the win pays once per way. The LAST
WIN panel says "5 REELS 12 WAYS". A symbol is scored on its longest
reach: a 5-reel chain pays as 5 reels, not also as 3 and 4.

**Wild.** WILD 7 stands in for any paying symbol (not for SCATTER, CROWN,
JACKPOT, ULTIMATE, LUCKY COIN or WHEEL) - and doubles the win; see
section 6. A chain made only of wilds pays as sevens.

## 5. The symbols and what they pay

Pays are **times your total bet, per way**, for a chain of 3, 4 or 5
reels, before any multiplier.

| Symbol | 3 reels | 4 reels | 5 reels |
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

The feature symbols pay nothing on a way. They start things:

| Symbol | Lands on | Does |
|---|---|---|
| SCATTER (star) | reels 1-5 | 3 / 4 / 5 anywhere pay 2x / 10x / 50x the bet, and 3+ start FREE SPINS |
| CROWN (BONUS) | reels 1, 3, 5 | 3 start the LUCKY 7 PICK |
| JACKPOT | reels 2, 3, 4 | 5 / 6 / 7+ touching win the MINOR / MAJOR / MEGA jackpot |
| ULTIMATE | every reel, one each | 5 touching in a chain win the ULTIMATE jackpot |
| LUCKY COIN | every reel | 6+ anywhere start HOLD & SPIN; each shows a value |
| WHEEL | reels 2, 3, 4 | one on each starts WHEEL OF 7'S |

## 6. MULTIPLIERS - every one, when it goes up, when it comes down

There are four multipliers in the game. They never hide: each one has a
place on the screen that shows it.

| Multiplier | What it multiplies | How big | Goes up | Comes down | Where you see it |
|---|---|---|---|---|---|
| **WILD** | the ways that run through it | x2 per wild on the way | every wild in the way doubles it again | it belongs to that one win | a gold **X2** badge on every wild in the win; **WILDS xN** in LAST WIN |
| **FREE SPINS meter** | every win in the free spins, whole spin | x1 to x5 | +1 for each reel a wild expands on | **never during the feature**; back to x1 when the free spins end | the MULTIPLIER meter and its lamps; the top-box bar; the MULTIPLIER UP moment; **FREE SPINS xN** in LAST WIN |
| **PICK x2** | everything collected in the pick round | x1 or x2 | finding the X2 panel | back to x1 when the next pick round starts | the pick board's MULTIPLIER readout; the rail meter says PICK BONUS |
| **GAMBLE** | the win you are gambling | x2 (colour) or x4 (suit) per round | a right call | a wrong call loses the win | the gamble table |

### The WILD multiplier

- Every WILD 7 on a winning way **doubles** that way. Two wilds on it pay
  x4, three pay x8, and so on.
- It works **per way, not per win**. If a win has 12 ways and only some
  of them run through a wild, only those are doubled. That is why LAST
  WIN sometimes shows an average, such as **WILDS X2.5 AVG**: the wilded
  ways paid x2 or x4 and the rest x1, and that is what it came to overall.
- It is part of that win and nothing else: the next spin starts clean.
- In the base game, 7 STRIKE drops extra wilds on the reels, and they
  double like any other wild.

Example: a 5-reel DIAMOND chain with 4 ways, and 2 wilds on every way,
pays 4.2 x 4 ways x 4 (the two wilds) = **67.2 times the bet**.

### The FREE SPINS meter (x1 to x5)

This is the big one, and the rules are simple:

1. **It starts at X1** when free spins are awarded. The intro screen
   says so: "MULTIPLIER STARTS AT X1 - EVERY WILD REEL ADDS +1, UP TO X5".
2. **It goes up by one for every wild reel.** In free spins a WILD 7 that
   lands on reel 2, 3 or 4 expands to fill that whole reel, and each reel
   that expands adds +1. Two expanding reels in one spin add +2, three add
   +3 (so X1 can jump straight to X3 or X4).
3. **The new figure counts at once**: the spin that raised it is paid at
   the new figure. The screen stops to say so - "WILD REEL: X2 > X3 /
   THIS WIN AND EVERY WIN AFTER IT PAYS X3".
4. **It never goes down during the feature.** Winning, losing, or a
   spin with no wild changes nothing. A retrigger (3 more scatters, +3
   spins) keeps the meter where it is: "THE MULTIPLIER STAYS AT X3 AND
   KEEPS CLIMBING".
5. **It tops out at X5.** At the top the meter reads MAXIMUM! and the
   top-box bar shows MAX.
6. **It multiplies the whole spin**: every way win and the scatter pay,
   on top of the x2 each wild already gives. It does not multiply a
   jackpot, or the prizes of HOLD & SPIN, the WHEEL or the PICK if one of
   those is started from a free spin - those pay their own amounts.
7. **It goes back to X1 when the free spins end.** The FREE SPINS
   COMPLETE screen says "MULTIPLIER REACHED X4 - BACK TO X1 FOR THE BASE
   GAME", and the rail meter shows RESETS TO X1 until the base game
   returns, its lamps going dark.

Reading the meter on the left rail:

```
  +-- MULTIPLIER --+     big number  = the multiplier now
  |      X3        |     five lamps  = X1 X2 X3 X4 X5, lit up to it;
  |  o  o  o  .  . |                   the top lit one breathes
  | WILD REEL = +1 |     caption     = what it is doing
  +----------------+
```

| Caption | Means |
|---|---|
| IN FREE SPINS | base game: the meter is not in play (it sits at X1, lamps dark) |
| WILD REEL = +1 | free spins: each expanding wild reel adds one |
| MAXIMUM! | free spins at X5 |
| RESETS TO X1 | the free spins just ended; it drops back to X1 |
| PICK BONUS | the pick round's multiplier is on show |

How it tends to go (measured over millions of features): the meter is at
X1 for 36% of free spins, X2 33%, X3 19%, X4 8%, X5 4%.

Example: in free spins at X3, a 4-reel BELL win on 5 ways with a wild on
every way pays 0.6 x 5 ways x 2 (wild) x 3 (meter) = **18 times the
bet**. LAST WIN shows it line by line:

```
BELL
4 REELS  5 WAYS
WILDS  X2
FREE SPINS  X3
PAYS 900            (at a bet of 50)
```

### The PICK multiplier

The pick board hides one **X2** panel among its nine. Find it and
everything you have collected - and everything you collect after it - is
doubled when the round ends. The board's readout says "MULTIPLIER -
TIMES ALL COLLECTED". It starts at X1 in every pick round.

### The GAMBLE multiplier

Each right call doubles (colour) or quadruples (suit) the win you are
holding; a wrong call loses it. See section 13.

### Things that are NOT multipliers

- BIG / SUPER / MEGA / EPIC WIN are just the size of the win, shown big.
- The SUPER wedge on the wheel swaps in a bigger wheel; it does not
  multiply anything.
- The jackpots are multiples of the bet (5x to 100,000x), fixed at the
  moment they are won.

## 7. The spin, and what happens after it

- The reels stop **one at a time with an uneven gap** of half a second
  to two seconds, different every spin. START, A or B slams them all down.
  The Turbo option halves the gaps.
- **Anticipation**: when two scatters, two crowns or two wheels are
  already showing, the remaining reels are held back an extra second and
  burn - fire, arcs, the other reels dimmed.
- Once the reels stop, things happen in this order:
  1. a 7 STRIKE storm, if one gathered during the spin;
  2. a jackpot, if the reels won one;
  3. **the spin's own wins are shown and paid**;
  4. any features it started, one after another: **HOLD & SPIN, then the
     WHEEL, then the PICK, then FREE SPINS**.

  So one spin can pay its line wins, then run a HOLD & SPIN, then open
  the free spins.

## 8. FREE SPINS

- **3 or more SCATTERS anywhere**: 6 free spins. The scatters also pay
  2x / 10x / 50x the bet for 3 / 4 / 5.
- In free spins, a WILD 7 on **reel 2, 3 or 4 expands** to fill the whole
  reel, and every expanding reel adds +1 to the multiplier (section 6).
- **3 more scatters** during the feature add **3 spins**; the multiplier
  stays where it is.
- Free spins play themselves; the top box shows spins left, the
  multiplier and the running total.
- When they end, FREE SPINS COMPLETE shows the total and where the
  multiplier got to, and the meter goes back to X1.
- About 1 spin in 210.

## 9. LUCKY 7 PICK

- **3 CROWNS** (they land on reels 1, 3 and 5).
- Nine hidden panels: five hold credits (0.7x, 1x, 1.7x, 2.5x and 3.4x the
  bet), one holds **X2**, three hold **STOP**.
- D-pad moves, A turns a panel. The round ends on the **third STOP**, so
  you usually get four or five picks.
- At the end, everything collected is multiplied by the multiplier (X1,
  or X2 if you found it) and paid.
- About 1 spin in 262.

## 10. HOLD & SPIN

- **6 or more LUCKY COINS anywhere** on the reels. Every coin shows its
  value the moment it lands: 0.5x, 1x, 1.5x, 2x, 3x, 5x, 10x or 25x the
  bet (the small ones far more often), or now and then a **MINOR** or
  **MAJOR** coin, which is worth that jackpot.
- The coins lock in place and every other cell becomes its own little
  reel. You get **3 RESPINS** (the three lamps above the board).
- **Every new coin locks in and resets the respins to 3.** A respin that
  brings no coin uses one up. The feature ends when they run out.
- Fill **all 25 cells** and it is the **GRAND**: the MEGA jackpot on top
  of every coin.
- Then the coins are counted into the meter one by one and the total is
  paid.
- Near the end the board speeds up the tension: the last few cells stop
  slowly, and at 20+ coins the edge throbs and the header says GRAND IN
  REACH.
- About 1 spin in 246; the average prize is about 13 times the bet.
  Features end with 6-8 coins about 46% of the time, 9-14 about 43%,
  15-24 about 12%. The GRAND is about 1 feature in 2,000.

## 11. WHEEL OF 7'S

- **One WHEEL on each of reels 2, 3 and 4.**
- A giant wheel with chasing bulbs comes up. **Press A to spin** (it spins
  itself after about 6 seconds).
- 24 wedges: **5x to 250x the bet**, the **MINOR** and **MAJOR**
  jackpots, and **SUPER**.
- **SUPER** swaps in the black-and-gold **SUPER WHEEL** - 25x to 500x, the
  MAJOR and the **MEGA** jackpot - for another spin.
- Where it stops is decided the moment it starts, and the wheel is driven
  to stop in that wedge: what you see is what you win.
- About 1 spin in 386; the average prize is about 17 times the bet.

## 12. 7 STRIKE

- At random, about **1 base-game spin in 100** (never in free spins), a
  storm gathers while the reels spin: the reels darken, cloud rolls over
  the top box, lightning flickers.
- When the reels stop, **3 to 8 lightning bolts** strike, one after
  another, and every cell hit becomes a **WILD 7**. They never strike a
  feature symbol.
- Then the spin is scored with its new wilds - and every wild doubles the
  ways through it, so storms pay well: nearly all pay at least the bet,
  and about 1 in 40 pays 20x or more.
- START, A or B plays the storm three times faster.

## 13. GAMBLE

- After any **base-game** win of up to **50 times the bet** has finished
  counting, **press X** (the reel bezel shows "X = GAMBLE").
- A card is dealt face down:
  - **LEFT = RED, RIGHT = BLACK**: a right call **doubles** the win;
  - **UP / DOWN** chooses a suit, **X** plays it: a right call
    **quadruples** it;
  - **A, START or B collects** what you hold (30 seconds idle collects too).
- A wrong call loses the win.
- Up to **5 rounds**, and you can go again only while the win is still
  within 50x the bet. The last five cards show along the top.
- The draw is exactly fair: red or black is exactly one in two, a suit
  exactly one in four. Over time the gamble neither costs nor pays.
- Not offered in free spins.

## 14. The progressive jackpots

| Jackpot | Won by | Worth |
|---|---|---|
| ULTIMATE | 5 ULTIMATE symbols touching in a chain, one per reel | 100,000 x bet + the pot |
| MEGA | 7 or more JACKPOT symbols touching; the GRAND in HOLD & SPIN; the MEGA wedge on the SUPER WHEEL | 200 x bet + the pot |
| MAJOR | 6 JACKPOT symbols touching; a MAJOR coin; a MAJOR wedge | 40 x bet + the pot |
| MINOR | 5 JACKPOT symbols touching; a MINOR coin; a MINOR wedge | 5 x bet + the pot |

- "Touching" for the jackpots means any direction, corners included -
  unlike ordinary wins, which read left to right.
- **A tenth of every bet feeds the pots** (ULTIMATE 3%, MEGA 2.5%, MAJOR
  2%, MINOR 2.5%). A pot empties back to its bet multiple when it is won.
- Because each jackpot is a multiple of the bet, **raising the bet raises
  all four meters at once**, and the ULTIMATE is 1,000,000 at the smallest
  bet of 10.
- Odds from the reels alone: MINOR about 1 in 400 spins, MAJOR 1 in 2,900,
  MEGA 1 in 13,000, ULTIMATE 1 in 7.9 million.

## 15. Big-win celebrations

| Win (times the bet) | Title |
|---|---|
| under 10x | a quick count, coins burst off the winning cells, the pay pops over them |
| 10x | BIG WIN |
| 25x | SUPER WIN |
| 50x | MEGA WIN |
| 100x | EPIC WIN |

The count climbs, stalls just short of each line, then the title slams up
a tier with a shockwave and a heavier coin fountain. The first button
press jumps to the total, the second collects. Jackpots have their own
celebration; the ULTIMATE adds fireworks.

## 16. The attract loop, the pay table pages, options

- **Attract**: after 25 seconds without a button, the machine shows off:
  the logo and the ULTIMATE pot, the features one by one, the jackpots.
  Any button returns to the game.
- **Pay table** (SELECT): three pages, SELECT to turn, any other button to
  go back. Page 1 PAY TABLE (every symbol), page 2 FEATURES (the wild,
  free spins, the pick, the jackpots), page 3 MORE FEATURES (HOLD & SPIN,
  the WHEEL, 7 STRIKE, the GAMBLE).
- **Options** (RetroArch Quick Menu > Core Options, or `wild7.w7`):
  Sound, Music, Turbo spin, Flash limiter (on by default: damps flashes,
  shake and lightning), Render threads.

## 17. The numbers

Measured with the game's own code over millions of spins:

| | |
|---|---|
| Return to player | about **93.9%**, the same at every bet |
| A win of some kind | 1 spin in 4 |
| FREE SPINS | 1 in 210 |
| LUCKY 7 PICK | 1 in 262 |
| HOLD & SPIN | 1 in 246 |
| WHEEL OF 7'S | 1 in 386 |
| 7 STRIKE | 1 in 100 base spins |
| MINOR / MAJOR / MEGA from the reels | 1 in 400 / 2,900 / 13,000 |
| ULTIMATE | 1 in 7.9 million |

Where the return comes from: the base game about 38 points (7 STRIKE
included), free spins 26, pick 5, HOLD & SPIN 5.4, the wheel 4, and the
jackpots' seeds and the tenth of every bet that feeds them the rest.

## 18. Questions

**The MULTIPLIER meter says X1 in the base game. Is something wrong?**
No. It only works in free spins (and shows the pick round's multiplier
during a pick). In the base game it waits at X1 and says IN FREE SPINS.
Wilds still double wins in the base game - that is the wild's own x2,
shown on the wild itself.

**The multiplier went from X1 to X3 in one spin.** Two reels expanded in
that spin; each adds one. The MULTIPLIER UP screen says "2 WILD REELS".

**Why did the multiplier go back to X1?** The free spins ended. It never
drops during them.

**The WIN meter says more than LAST WIN's PAYS.** A spin can make
several wins at once (different symbols, each with its own ways), and a
scatter pay on top. The WIN meter is the whole spin; LAST WIN and the pay
that pops over the reels show one win at a time, cycling through them
("1 OF 3" in the panel's corner). In free spins all of them already
include the meter.

**Can I gamble a free-spins win?** No - only base-game wins up to 50x
the bet.

**Does turning the sound off change the game?** No. The sound has its
own random numbers and cannot affect the reels.
