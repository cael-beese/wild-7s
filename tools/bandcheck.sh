#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission)
# =====================================================================
#  bandcheck.sh - prove the band renderer draws exactly what the
#  single-threaded renderer draws, and optionally exactly what an older
#  revision drew.
#
#    tools/bandcheck.sh                     threads 1 vs 2,3,4
#    REF=41315e7 tools/bandcheck.sh         ... and threads=1 vs that rev
#    REF=/path/to/tree tools/bandcheck.sh   (a tree holding src/, for
#                                            when git is not installed)
#    FRAMES=1800 EVERY=10 tools/bandcheck.sh
#    SHOTCFLAGS="-O3 -ffast-math -mcpu=native" tools/bandcheck.sh   (as the Pi
#                                            builds the core)
#    CC=aarch64-linux-gnu-gcc RUN="qemu-aarch64 -L /usr/aarch64-linux-gnu" \
#      SHOTCFLAGS="-O3 -ffast-math -mcpu=cortex-a72" tools/bandcheck.sh
#                                           (the Pi's code, on x86)
#
#  Every scenario runs the real retro_run() loop in w7shot, hashes every
#  EVERY-th frame and folds the hashes into one digest.  Matching digests
#  mean every sampled frame is bit-identical.  Exit status 1 on any
#  mismatch.  Runs the scenarios in parallel; run it from the repository
#  root, in WSL or on the Pi.
# =====================================================================
set -u
FRAMES=${FRAMES:-960}
EVERY=${EVERY:-30}
THREADS=${THREADS:-"2 3 4"}
SHOTCFLAGS=${SHOTCFLAGS:--O2}
REF=${REF:-}
CC=${CC:-cc}
RUN=${RUN:-}          # e.g. "qemu-aarch64 -L /usr/aarch64-linux-gnu" with CC=aarch64-linux-gnu-gcc

tmp=$(mktemp -d)
$CC $SHOTCFLAGS -DW7SHOT_NOPNG -Isrc -o "$tmp/w7shot_bc" tools/w7shot.c -lm -lpthread || exit 2

if [ -n "$REF" ]; then
  mkdir -p "$tmp/ref/tools"
  if [ -d "$REF/src" ]; then cp -r "$REF/src" "$tmp/ref/"       # an exported tree
  else git archive "$REF" src | tar -x -C "$tmp/ref" || exit 2; fi
  cp tools/w7shot.c "$tmp/ref/tools/"
  $CC $SHOTCFLAGS -DW7SHOT_NOPNG -I"$tmp/ref/src" -o "$tmp/w7shot_ref" "$tmp/ref/tools/w7shot.c" -lm -lpthread || exit 2
fi

# autopilot:force pairs.  1 spin loop, 2 pay table, 4 add credits,
# 5 features page, 6 bet ladder; the forces land every bonus and
# jackpot tier.
SCEN=${SCEN:-"1: 2: 4: 5: 6: 1:free 1:pick 1:mega 1:minor 1:ult"}

run(){  # bin threads pilot force out
  WILD7_AUTOPILOT=$3 WILD7_FORCE=$4 W7SHOT_OPTS="wild7_threads=$2" \
    $RUN "$1" -n "$FRAMES" -H "$EVERY" -q -s -1 -o "$tmp" | awk '/^digest/{print $2}' > "$5"
}

for s in $SCEN; do
  p=${s%%:*}; f=${s#*:}; k="$p-$f"
  run "$tmp/w7shot_bc" 1 "$p" "$f" "$tmp/$k.t1" &
  for t in $THREADS; do run "$tmp/w7shot_bc" "$t" "$p" "$f" "$tmp/$k.t$t" & done
  [ -n "$REF" ] && run "$tmp/w7shot_ref" 1 "$p" "$f" "$tmp/$k.ref" &
done
wait

fail=0
for s in $SCEN; do
  p=${s%%:*}; f=${s#*:}; k="$p-$f"
  base=$(cat "$tmp/$k.t1")
  [ -n "$base" ] || { echo "pilot=$p: no digest"; fail=1; continue; }
  line="pilot=$p force=${f:--}  t1=$base"
  for t in $THREADS; do
    d=$(cat "$tmp/$k.t$t")
    if [ "$d" = "$base" ]; then line="$line  t$t=ok"; else line="$line  t$t=DIFF($d)"; fail=1; fi
  done
  if [ -n "$REF" ]; then
    d=$(cat "$tmp/$k.ref")
    if [ "$d" = "$base" ]; then line="$line  ref=ok"; else line="$line  ref=DIFF($d)"; fail=1; fi
  fi
  echo "$line"
done
rm -rf "$tmp"
if [ $fail = 0 ]; then echo "bandcheck: all identical ($FRAMES frames, every $EVERY)"; else echo "bandcheck: MISMATCH"; fi
exit $fail
