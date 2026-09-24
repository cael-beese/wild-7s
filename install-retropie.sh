#!/usr/bin/env bash
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md (commercial use by permission)
# =====================================================================
#  Install WILD 7's into RetroPie as a Port.
#  Run on the Pi, from this directory, after `make`.
# =====================================================================
set -euo pipefail

CORE_SRC="wild7_libretro.so"
CORE_DIR="/opt/retropie/libretrocores/lr-wild7"
CONF_DIR="/opt/retropie/configs/ports/wild7"
ROM_USER="${SUDO_USER:-$USER}"
ROM_HOME="$(getent passwd "$ROM_USER" | cut -d: -f6)"
ROM_DIR="$ROM_HOME/RetroPie/roms/ports"
INFO_DIR="/opt/retropie/emulators/retroarch/share/libretro/info"
SHADER="/opt/retropie/emulators/retroarch/shader/zfast_crt_curve.glslp"

[[ -f "$CORE_SRC" ]] || { echo "no $CORE_SRC here. run: make"; exit 1; }
[[ $EUID -eq 0 ]]     || { echo "needs root to write /opt/retropie. re-run with sudo."; exit 1; }
[[ -d /opt/retropie ]]|| { echo "/opt/retropie not found - not a RetroPie install."; exit 1; }

echo "installing core ..."
install -d "$CORE_DIR"
install -m 0755 "$CORE_SRC" "$CORE_DIR/wild7_libretro.so"
if [[ -f wild7_libretro.info && -d "$INFO_DIR" ]]; then
  install -m 0644 wild7_libretro.info "$INFO_DIR/wild7_libretro.info"
fi

echo "writing port config ..."
install -d "$CONF_DIR"
{
  echo '# WILD 7 s - port-local RetroArch settings.'
  echo '#include "/opt/retropie/configs/all/retroarch.cfg"'
  echo ''
  echo 'video_smooth = "false"'
  echo '# 22 = core provided. The core renders 1280x720 and declares 16:9;'
  echo '# forcing 4:3 here would letterbox it inside a pillarbox.'
  echo 'aspect_ratio_index = "22"'
  if [[ "${WILD7_CRT:-0}" = 1 && -f "$SHADER" ]]; then
    echo ''
    echo '# CRT shader (WILD7_CRT=1). zfast_crt_curve is the one preset on this'
    echo '# box that adds curvature and scanlines without shifting the palette.'
    echo 'video_shader_enable = "true"'
    echo "video_shader = \"$SHADER\""
  else
    echo ''
    echo '# No shader: the lounge look is neon and fine type, crisp at 2x, like'
    echo '# the poker game. Install with WILD7_CRT=1 for the old CRT curve.'
    echo 'video_shader_enable = "false"'
  fi
} > "$CONF_DIR/retroarch.cfg"

cat > "$CONF_DIR/emulators.cfg" <<CFG
wild7 = "/opt/retropie/emulators/retroarch/bin/retroarch -L $CORE_DIR/wild7_libretro.so --config $CONF_DIR/retroarch.cfg %ROM%"
default = "wild7"
CFG

echo "installing the ROM and launcher ..."
install -d "$ROM_DIR/wild7"
[[ -f wild7.w7 ]] && install -m 0644 wild7.w7 "$ROM_DIR/wild7/wild7.w7"
# keep a rebuildable copy beside the rom, since roms live on the USB stick
[[ -f wild7_libretro.info ]] && install -m 0644 wild7_libretro.info "$ROM_DIR/wild7/" || true
install -m 0644 "$CORE_SRC" "$ROM_DIR/wild7/wild7_libretro.so"

cat > "$ROM_DIR/Wild 7s.sh" <<SH
#!/bin/bash
"/opt/retropie/supplementary/runcommand/runcommand.sh" 0 _PORT_ "wild7" "$ROM_DIR/wild7/wild7.w7"
SH
chmod 0755 "$ROM_DIR/Wild 7s.sh"

chown -R "$ROM_USER":"$ROM_USER" "$ROM_DIR/Wild 7s.sh" "$ROM_DIR/wild7" "$CONF_DIR" 2>/dev/null || true

echo
echo "done."
echo "  core   $CORE_DIR/wild7_libretro.so"
echo "  rom    $ROM_DIR/wild7/wild7.w7"
echo "  launch $ROM_DIR/Wild 7s.sh"
echo
echo "restart EmulationStation, then look under Ports for WILD 7's."
