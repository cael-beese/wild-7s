#!/bin/bash
# =====================================================================
#  pi_soak.sh - soak the WILD 7's core on the Pi and watch the silicon.
#
#  Runs RetroArch headless (null video, audio and input drivers, against
#  a throwaway copy of the config) with the core on autopilot, while
#  sampling the SoC temperature, the throttle flags, the ARM clock and
#  every core's load once a second.  The core prints its own frame
#  timing (WILD7_PROFILE=1: mean / max render and update ms every 300
#  frames), so render cost can be read apart from RetroArch's.
#
#    tools/pi_soak.sh                       3 threads, 3600 frames
#    tools/pi_soak.sh -t "1 2 3" -n 7200    one soak per thread count
#    tools/pi_soak.sh -p 5 -t 3             the features page instead
#    tools/pi_soak.sh -F                    unpaced: as fast as it goes
#
#    -t LIST   thread counts to soak, in turn (default 3)
#    -n N      frames per soak (default 3600 = 60 s paced)
#    -p N      WILD7_AUTOPILOT (default 1, the spin loop)
#    -f NAME   WILD7_FORCE (free|pick|mega|minor|ult; default none)
#    -c SO     core (default ./wild7_libretro.so)
#    -g FILE   content (default ./wild7.w7)
#    -r BIN    retroarch (default: on PATH, else RetroPie's)
#    -C CFG    config to copy (default RetroPie's all/retroarch.cfg)
#    -w SEC    cool-down between soaks (default 60)
#    -o DIR    results (default /tmp/w7soak-DATE)
#    -F        do not pace to 60 fps (a worst-case heat test)
#
#  Run it on the Pi with the cabinet sitting in EmulationStation, not
#  while a game is up: the soak should have the CPU the game would have.
#  It touches nothing but its own temp copy of the config.
#  Results: summary.txt, and per soak samples-T.csv, profile-T.txt,
#  retroarch-T.log.
# =====================================================================
set -u
THREADS="3"; FRAMES=3600; PILOT=1; FORCE=""; CORE=./wild7_libretro.so
GAME=./wild7.w7; RA=""; CFG=""; COOL=60; OUT=""; PACE=1

while getopts "t:n:p:f:c:g:r:C:w:o:F" o; do
  case $o in
    t) THREADS=$OPTARG;; n) FRAMES=$OPTARG;; p) PILOT=$OPTARG;; f) FORCE=$OPTARG;;
    c) CORE=$OPTARG;; g) GAME=$OPTARG;; r) RA=$OPTARG;; C) CFG=$OPTARG;;
    w) COOL=$OPTARG;; o) OUT=$OPTARG;; F) PACE=0;;
    *) sed -n '2,36p' "$0"; exit 2;;
  esac
done

if [ -z "$RA" ]; then
  RA=$(command -v retroarch || true)
  [ -z "$RA" ] && RA=/opt/retropie/emulators/retroarch/bin/retroarch
fi
if [ -z "$CFG" ]; then
  for c in /opt/retropie/configs/all/retroarch.cfg "$HOME/.config/retroarch/retroarch.cfg"; do
    [ -f "$c" ] && { CFG=$c; break; }
  done
fi
[ -x "$RA" ]   || { echo "no retroarch at $RA (use -r)"; exit 1; }
[ -f "$CORE" ] || { echo "no core at $CORE (use -c, or run make)"; exit 1; }
[ -f "$GAME" ] || { echo "no content at $GAME (use -g)"; exit 1; }
CORE=$(readlink -f "$CORE"); GAME=$(readlink -f "$GAME")
OUT=${OUT:-/tmp/w7soak-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

VC=$(command -v vcgencmd || true)
temp_c(){
  if [ -n "$VC" ]; then $VC measure_temp | sed 's/[^0-9.]//g'
  else awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp; fi
}
throttled(){ [ -n "$VC" ] && $VC get_throttled | sed 's/.*=//' || echo "n/a"; }
arm_mhz(){
  if [ -n "$VC" ]; then $VC measure_clock arm | awk -F= '{printf "%d", $2/1000000}'
  else awk '{printf "%d", $1/1000}' /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0; fi
}

# The throwaway config: a copy of the real one with the drivers nulled
# and nothing written back.  RetroArch uses the LAST value of a key.
make_cfg(){
  local c=$OUT/retroarch-soak.cfg
  { [ -n "$CFG" ] && cat "$CFG"
    echo 'video_driver = "null"'
    echo 'audio_driver = "null"'
    echo 'input_driver = "null"'
    echo 'joypad_driver = "null"'
    echo 'menu_driver = "null"'
    echo 'config_save_on_exit = "false"'
    echo 'savestate_auto_load = "false"'
    echo 'savestate_auto_save = "false"'
    echo 'pause_nonactive = "false"'
    echo 'video_vsync = "false"'
    if [ $PACE = 1 ]; then echo 'vrr_runloop_enable = "true"'      # 60 fps by timer
    else echo 'vrr_runloop_enable = "false"'; fi
    echo 'fastforward_ratio = "0.0"'
  } > "$c"
  echo "$c"
}

# One line per second: time, temp, throttle flags, ARM MHz, the
# RetroArch process's CPU (100 = one core), and each core's busy %.
sample(){  # pid csv
  local pid=$1 csv=$2 hz; hz=$(getconf CLK_TCK)
  local prev_stat prev_p t0; t0=$(date +%s.%N)
  prev_stat=$(grep '^cpu[0-9]' /proc/stat)
  prev_p=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
  local ncpu; ncpu=$(grep -c '^cpu[0-9]' /proc/stat)
  { printf "t,temp_c,throttled,arm_mhz,ra_cpu"
    for i in $(seq 0 $((ncpu-1))); do printf ",cpu%d" "$i"; done; echo; } > "$csv"
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    local now_stat now_p t
    now_stat=$(grep '^cpu[0-9]' /proc/stat)
    now_p=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo "$prev_p")
    t=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
    local loads
    loads=$(paste -d' ' <(echo "$prev_stat") <(echo "$now_stat") | awk '{
        n=(NF)/2; tp=0; tn=0;
        for(i=2;i<=n;i++){ tp+=$i; tn+=$(i+n) }
        ip=$5+$6; in_=$(5+n)+$(6+n);
        d=tn-tp; printf ",%.0f", d>0 ? 100*(d-(in_-ip))/d : 0 }')
    local ra
    ra=$(awk -v a="$prev_p" -v b="$now_p" -v hz="$hz" 'BEGIN{printf "%.0f", 100*(b-a)/hz}')
    echo "$t,$(temp_c),$(throttled),$(arm_mhz),$ra$loads" >> "$csv"
    prev_stat=$now_stat; prev_p=$now_p
  done
}

CFGFILE=$(make_cfg)
{
  echo "WILD 7's soak  $(date)"
  echo "host $(hostname)  $(tr -d '\0' < /proc/device-tree/model 2>/dev/null)"
  echo "retroarch $RA"
  echo "core $CORE"
  echo "frames $FRAMES  autopilot $PILOT  force ${FORCE:-none}  paced $PACE"
  echo "idle before: temp $(temp_c) C  throttled $(throttled)  arm $(arm_mhz) MHz"
} | tee "$OUT/summary.txt"

first=1
for T in $THREADS; do
  if [ $first = 0 ] && [ "$COOL" -gt 0 ]; then
    echo "cooling down ${COOL}s ..."; sleep "$COOL"
  fi
  first=0
  log=$OUT/retroarch-$T.log; csv=$OUT/samples-$T.csv; prof=$OUT/profile-$T.txt
  echo "--- threads $T: start temp $(temp_c) C" | tee -a "$OUT/summary.txt"
  start=$(date +%s.%N)
  WILD7_AUTOPILOT=$PILOT WILD7_FORCE=$FORCE WILD7_THREADS=$T WILD7_PROFILE=1 \
    "$RA" --config "$CFGFILE" -L "$CORE" "$GAME" --max-frames="$FRAMES" \
    > "$log" 2>&1 &
  pid=$!
  sample "$pid" "$csv" &
  spid=$!
  wait "$pid"; rc=$?
  wait "$spid" 2>/dev/null
  end=$(date +%s.%N)
  grep '^\[wild7\]' "$log" > "$prof"

  awk -v T="$T" -v s="$start" -v e="$end" -v F="$FRAMES" -v rc="$rc" -F, '
    NR==1{ next }
    { n++; if($2>mt) mt=$2; st+=$2; if($3!="0x0" && $3!="n/a") th=th" "$3;
      if(minc==""||$4<minc) minc=$4; ra+=$5;
      for(i=6;i<=NF;i++){ cs[i]+=$i; if(i>nc) nc=i } }
    END{
      w=e-s; printf "threads %s: exit %d, %.1f s wall = %.1f fps\n", T, rc, w, F/w;
      if(n){ printf "  temp mean %.1f max %.1f C   min ARM %d MHz   retroarch CPU %.0f%%\n", st/n, mt, minc, ra/n;
             printf "  per-core busy %%:"; for(i=6;i<=nc;i++) printf " %.0f", cs[i]/n; printf "\n";
             printf "  throttle flags: %s\n", th==""?"never set":th }
    }' "$csv" | tee -a "$OUT/summary.txt"
  awk '{ for(i=1;i<=NF;i++){ if($i=="render"){ rm+=$(i+2); if($(i+4)>rx) rx=$(i+4) }
                             if($i=="update"){ um+=$(i+2); if($(i+4)>ux) ux=$(i+4) }
                             if($i=="threads") th=$(i+1); if($i=="bands") bd=$(i+1) } n++ }
       END{ if(n) printf "  core: render mean %.2f max %.2f ms, update mean %.3f max %.3f ms (%d windows, threads %s bands %s)\n",
                   rm/n, rx, um/n, ux, n, th, bd;
            else print "  core: no [wild7] profile lines - is this build older than the band renderer?" }' \
      "$prof" | tee -a "$OUT/summary.txt"
done
echo "results in $OUT"
