#!/bin/bash
# Launch WILD 7's on the cabinet from SSH, then restore the normal
# EmulationStation session by restarting tty1's autologin.
#
#   setsid nohup ~/w7launch.sh >/dev/null 2>&1 < /dev/null &
#
# Whatever is on the screen has to let go of the DRM master first, so this
# quits a running game AND EmulationStation (without /tmp/es-restart, so
# its wrapper loop exits and the tty1 shell stays alive), waits for both
# to die, then starts RetroArch.  If RetroArch loses a race for the
# display and dies inside three seconds it waits and tries again.
cd /home/pi
RA=/opt/retropie/emulators/retroarch/bin/retroarch
CORE=/opt/retropie/libretrocores/lr-wild7/wild7_libretro.so
CFG=/opt/retropie/configs/ports/wild7/retroarch.cfg
ROM=/home/pi/RetroPie/roms/ports/wild7/wild7.w7

rm -f /tmp/es-restart
pkill -f "^$RA"                                   # a game already playing
pkill -f "supplementary/emulationstation/emulationstation$"
for i in $(seq 1 20); do
  pgrep -f "^$RA" >/dev/null && { sleep 1; continue; }
  pgrep -f "supplementary/emulationstation/emulationstation$" >/dev/null || break
  sleep 1
done
sleep 3

for attempt in 1 2 3 4; do
  T0=$(date +%s)
  "$RA" -L "$CORE" --config "$CFG" "$ROM" --verbose --log-file /tmp/ra.log
  RC=$?
  T1=$(date +%s)
  echo "retroarch exit $RC after $((T1-T0))s (attempt $attempt)" >> /tmp/ra.log
  [ $((T1-T0)) -ge 3 ] && break          # it ran; the player quit it
  pkill -f "supplementary/emulationstation/emulationstation$"
  sleep 4                                 # lost the display: try again
done

sudo systemctl restart getty@tty1
