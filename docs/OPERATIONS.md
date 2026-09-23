# WILD 7's - operations

Running the game on the arcade cabinet: where it lives, how a new build gets
there, how to start it from SSH, how to soak it, how to back it up and how to
roll it back. `docs/ARCHITECTURE.md` explains the code; `DEVELOPING.md` the
rules for changing it.

Nothing in this document contains a password. The Pi's password is the owner's to
give; put it in a scratch file for the session and delete the file after.

---

## 1. The machine

| | |
|---|---|
| board | Raspberry Pi 4 Model B, 4x Cortex-A72; held 1,800 MHz under the v3 soak |
| memory | 905 MB total; DEVELOPING.md budgets about 390 MB free with EmulationStation up |
| OS | Debian 13 "trixie", aarch64 |
| frontend | RetroPie: EmulationStation 2.11.2rp, RetroArch 1.19.1 |
| address | `<pi-address>` on your LAN, SSH port 22, user `pi` |
| auth | **password only, deliberately.** No SSH key goes on this box and `~/.ssh` should not exist there. |
| sudo | `pi` has passwordless sudo |
| display | 3440x1440 cabinet screen; the port runs RetroArch's `zfast_crt_curve` shader |
| tools on the Pi | `git`, `gcc`, `make`, `curl`, `python3`, `zellij`. No Node, no Claude Code. |

The core renders 1280x720 and declares 16:9; the port config sets
`aspect_ratio_index = "22"` (core provided) so RetroArch keeps that shape.

### Paths

| path | what |
|---|---|
| `~/Wild7s/` | the source tree on the Pi; `make` builds `wild7_libretro.so` here |
| `/opt/retropie/libretrocores/lr-wild7/wild7_libretro.so` | **the core RetroArch loads** |
| `~/RetroPie/roms/ports/wild7/` | the "ROM" `wild7.w7`, a copy of the core and the `.info`, source tarballs |
| `~/RetroPie/roms/ports/Wild 7s.sh` | the Ports launcher EmulationStation lists (`runcommand.sh 0 _PORT_ wild7 .../wild7.w7`) |
| `/opt/retropie/configs/ports/wild7/retroarch.cfg` | port RetroArch config (includes `all/retroarch.cfg`, `video_smooth = false`, aspect 22, the CRT shader) |
| `/opt/retropie/configs/ports/wild7/emulators.cfg` | `wild7 = ".../retroarch -L <core> --config <port cfg> %ROM%"` |
| `/opt/retropie/emulators/retroarch/share/libretro/info/wild7_libretro.info` | core info |
| `/opt/retropie/emulators/retroarch/bin/retroarch` | RetroArch |
| `~/w7launch.sh` | launch-from-SSH script (a copy of the repo's `w7launch.sh`) |
| `~/Wild7s-v2.2/` | the v2.2 tree, kept for rollback |
| `/etc/emulationstation/es_systems.cfg` | ES systems; the `ports` block was added by hand (backup `es_systems.cfg.pre-polybius.bak`) |

**`~/RetroPie/roms` is the USB stick.** `/dev/sda1` (115 GB, vfat) is
bind-mounted there (the same files are under
`/media/usb0/retropie-mount/roms/`). vfat means no symlinks, and `chmod` and
`chown` silently do nothing rather than failing - including the installer's
`chmod 0755` on the launcher and its `chown`. The home directory itself is on
the SD card, so `~/Wild7s` and `~/w7launch.sh` behave normally.

Helpers left in the Pi's home by earlier sessions (not in the repo, contents
not re-checked for v3): `~/cap.sh` (null-driver screenshots to
`/tmp/w7shots`), `~/prof.sh` (ablation profiling), `~/probe.sh`.

### zellij on the Pi

Long work (a build, a soak) runs inside a named zellij session so a dropped
SSH connection does not kill it. The Pi's config sets
`on_force_close "detach"`. Layout `w7` (in `~/.config/zellij/layouts/`) opens
a build/sim/capture tab and an ES-log tab in `~/Wild7s`.

```
ssh -t retropie 'zellij attach --create main'    # attach or create
ssh -t retropie 'zellij -s w7 -n w7'             # new session from the w7 layout
ssh retropie 'zellij list-sessions'
```

`-n` creates a session from a layout; `-l` only adds tabs to an existing one
and fails with "Session not found". These `ssh` calls prompt for the
password; they are for a person at a terminal.

---

## 2. Three copies of the core that must match

After a good install these three files are byte-identical:

| copy | role |
|---|---|
| `~/Wild7s/wild7_libretro.so` | build output; what `tools/pi_soak.sh` tests by default |
| `~/RetroPie/roms/ports/wild7/wild7_libretro.so` | spare copy on the stick, beside the ROM; nothing loads it |
| `/opt/retropie/libretrocores/lr-wild7/wild7_libretro.so` | what `emulators.cfg` and `~/w7launch.sh` load |

The installer copies the first to the other two. Check them:

```
md5sum ~/Wild7s/wild7_libretro.so \
       ~/RetroPie/roms/ports/wild7/wild7_libretro.so \
       /opt/retropie/libretrocores/lr-wild7/wild7_libretro.so
ls -l --time-style=long-iso ~/Wild7s/wild7_libretro.so \
       /opt/retropie/libretrocores/lr-wild7/wild7_libretro.so
```

If the build copy differs from the other two, the build was not installed.
If the `/opt` copy differs from the stick copy, the installer stopped part
way (it runs with `set -euo pipefail`; re-run it and read its output).

---

## 3. Build and deploy from the Windows box

The source of truth is the local git repo `C:\path\to\Wild7s`.
Work and test there (WSL: `make && make sim shot`, see DEVELOPING.md), commit,
then ship the commit to the Pi and build it there with the Pi's own flags
(`-O3 -ffast-math -mcpu=native`). Do not copy an x86 `.so` across; it will not
load.

The Pi is reached with PuTTY's `plink` and `pscp` using `-pwfile`. Git Bash
has no `sshpass`, and no key-based login exists for this box.

PowerShell, from the repo:

```powershell
# a scratch directory for this session; pw.txt holds the Pi password (one line)
$SP = "$env:TEMP\w7deploy"; New-Item -ItemType Directory -Force $SP | Out-Null
# ... create $SP\pw.txt by hand ...
$PI = "pi@<pi-address>"

# 1. package the committed tree (uncommitted edits are NOT included)
git archive --format=tar.gz -o "$SP\wild7-src.tar.gz" HEAD

# 2. copy it over and unpack into ~/Wild7s
pscp  -batch -pwfile "$SP\pw.txt" "$SP\wild7-src.tar.gz" "${PI}:/home/pi/"
plink -batch -pwfile "$SP\pw.txt" $PI "mkdir -p ~/Wild7s && tar -xzmf ~/wild7-src.tar.gz -C ~/Wild7s"
```

Unpack with `-m` (or run `make clean` before building). `git archive`
stamps every file with the commit time; if the Pi's existing
`wild7_libretro.so` is newer than that, `make` decides it is up to date and
builds nothing. `-m` gives the files the time of extraction instead.

If files were deleted from the repo, unpack into a fresh directory instead
(`tar` does not remove old files).

### Build (about 3.5 minutes)

Interactively, in zellij (recommended):

```
ssh -t retropie 'zellij attach --create main'
cd ~/Wild7s && make
```

Or scripted from Windows, detached, with a log:

```powershell
plink -batch -pwfile "$SP\pw.txt" $PI "cd ~/Wild7s && setsid -f nohup make > /tmp/w7make.log 2>&1 < /dev/null"
# poll until the last line reads "built wild7_libretro.so for aarch64"
plink -batch -pwfile "$SP\pw.txt" $PI "tail -3 /tmp/w7make.log"
```

`make` must finish with no warnings (`-Wall -Wextra`); an error ends the log
with the compiler message and no "built" line.

### Install

```powershell
plink -batch -pwfile "$SP\pw.txt" $PI "cd ~/Wild7s && sudo bash ./install-retropie.sh"
```

Run it from the directory that holds the new `wild7_libretro.so`; it refuses
to run without one, without root, or without `/opt/retropie`. Call it through
`bash`: the execute bit is committed, but it does not survive every way of
copying the tree (and never on the vfat stick).

What it does, every time:

- installs the core to `/opt/retropie/libretrocores/lr-wild7/` and the
  `.info` to RetroArch's info directory;
- **rewrites** `/opt/retropie/configs/ports/wild7/retroarch.cfg` and
  `emulators.cfg` - any hand edits to the port config are lost;
- copies `wild7.w7`, the `.info` and the core to
  `~/RetroPie/roms/ports/wild7/` and writes the `Wild 7s.sh` launcher.

It does not install `w7launch.sh`. If that script changed:

```
cp ~/Wild7s/w7launch.sh ~/w7launch.sh && chmod 755 ~/w7launch.sh
```

### Check, then clean up

```powershell
plink -batch -pwfile "$SP\pw.txt" $PI "md5sum ~/Wild7s/wild7_libretro.so ~/RetroPie/roms/ports/wild7/wild7_libretro.so /opt/retropie/libretrocores/lr-wild7/wild7_libretro.so"
Remove-Item "$SP\pw.txt"
```

A fresh install shows up under Ports in EmulationStation (restart ES if the
entry is new). Replacing the core of an existing install needs no ES
restart - but see the next section.

---

## 4. A running game keeps the old core

RetroArch loads the `.so` once, when the game starts. Installing a new core
does nothing to a game already on the screen: it keeps running the copy it
loaded until it is quit. So after an install, **quit the running game** (the
player's exit hotkey, or `w7launch.sh`, which kills any running RetroArch)
and start it again. When checking that a change reached the cabinet, confirm
that the game was restarted after the install, not only that the install
succeeded.

---

## 5. Launching the game from SSH

`~/w7launch.sh` puts WILD 7's on the cabinet screen without anyone at the
controls:

1. deletes `/tmp/es-restart`, stops any earlier run of itself (one still
   waiting on the game it started would restart tty1 the moment that game
   is killed, and the EmulationStation that brings up steals the display
   from the new game - this happened on 2026-09-23), then kills any
   running RetroArch and EmulationStation (without the restart flag, ES's wrapper loop exits and
   tty1's autologin shell stays at a prompt, which frees the DRM display);
2. waits up to 20 s for both to go, then 3 s more;
3. starts `retroarch -L /opt/retropie/libretrocores/lr-wild7/wild7_libretro.so
   --config /opt/retropie/configs/ports/wild7/retroarch.cfg
   ~/RetroPie/roms/ports/wild7/wild7.w7 --verbose --log-file /tmp/ra.log`;
4. if RetroArch exits with an error inside 10 s (it lost the race for the
   display: "[KMS]: Error when switching mode"), kills ES again and
   retries, up to 4 attempts. A clean exit, or a run of 10 s or more,
   counts as played (before 3.0.1 a failure after exactly 3 s counted as
   played, so it gave up);
5. when the player quits, `sudo systemctl restart getty@tty1`, whose
   autologin re-runs RetroPie's autostart and brings EmulationStation back.

Run it **detached, in a plink call of its own**:

```powershell
plink -batch -pwfile "$SP\pw.txt" $PI "setsid -f nohup ~/w7launch.sh >/dev/null 2>&1 </dev/null"
```

The trap: without `setsid -f nohup` and all three redirections, or chained
with other commands in the same call (a trailing `&` inside a longer
command line does it), the SSH channel stays open on the script's stdio and
`plink` hangs until someone quits the game. `setsid -f` forks the script
into its own session and returns at once. Check how it went in a
separate call: `tail /tmp/ra.log` (the script appends
`retroarch exit RC after Ns (attempt n)` lines).

Do not use `openvt -f` or `su pi -c` on tty1 to start a game: that hangs up
the autologin shell, `getty@tty1` (Restart=always) logs in again and starts a
second EmulationStation, which takes the display, and RetroArch dies with
"[KMS]: Error when switching mode".

### Getting EmulationStation back

```
sudo systemctl restart getty@tty1
```

This is also the fix whenever ES is gone and the screen sits at a console.

To restart ES in place instead (for example after editing its config):

```
touch /tmp/es-restart
pkill -f "supplementary/emulationstation/emulationstation$"
```

With the flag the wrapper loop relaunches ES. Without it the wrapper exits
and ES stays down until `getty@tty1` is restarted (or the Pi rebooted).

---

## 6. EmulationStation and RetroArch traps

- **ES rewrites `es_settings.cfg` and the gamelists when it exits**
  (`SaveGamelistsMode = "on exit"`). Edits made while ES runs are silently
  reverted. Stop ES first (kill it without `/tmp/es-restart`), edit, then
  bring it back (`systemctl restart getty@tty1` or reboot).
- **The Ports system can vanish.** `/etc/emulationstation/es_systems.cfg`
  had no `ports` block until one was added by hand (backup
  `es_systems.cfg.pre-polybius.bak`). RetroPie regenerates this file when
  packages are installed through `retropie_setup`, so after any RetroPie
  package work check that the block is still there, or WILD 7's is invisible
  even though it is installed.
- **Never point RetroArch at a real config for a test run.**
  `retroarch --config <file>` saves its whole runtime config back over that
  file on exit. Test against a throwaway copy with
  `config_save_on_exit = "false"` (as `tools/pi_soak.sh` does).
- **The ROM directory is vfat** (section 1): no symlinks, no permissions.
- **The `.w7` settings file is read by substring** and, as shipped, its
  comment line turns turbo on and its `credits=` line is never reached (see
  ARCHITECTURE section 14). Core options set in RetroArch's Quick Menu apply
  as soon as they change.

---

## 7. Soak testing on the Pi

`tools/pi_soak.sh` runs the core headless in RetroArch (null video, audio
and input drivers, a throwaway copy of `all/retroarch.cfg`, nothing written
back) on autopilot, while sampling the SoC temperature, throttle flags, ARM
clock, RetroArch's CPU and every core's load once a second. The core adds
its own timing (`WILD7_PROFILE=1`).

Run it inside zellij, with the cabinet sitting idle in EmulationStation (not
in a game, so the soak has the CPU a game would have):

```
ssh -t retropie 'zellij attach --create w7soak'
cd ~/Wild7s
bash tools/pi_soak.sh                        # 3 threads, 3600 frames (60 s)
bash tools/pi_soak.sh -t "1 2 3" -n 7200     # one soak per thread count
bash tools/pi_soak.sh -f hold -n 3600        # a forced feature
bash tools/pi_soak.sh -F                     # unpaced: as fast as it goes
bash tools/pi_soak.sh -c /opt/retropie/libretrocores/lr-wild7/wild7_libretro.so
```

Options: `-t` thread counts, `-n` frames, `-p` autopilot (default 1),
`-f` force (`free|pick|mega|minor|ult|hold|wheel|storm|...`), `-c` core
(default `./wild7_libretro.so` - the build copy), `-g` content, `-r`
RetroArch, `-C` config to copy, `-w` cool-down between soaks (60 s), `-o`
output directory, `-F` unpaced.

Results go to `/tmp/w7soak-YYYYmmdd-HHMMSS/` (Debian 13 normally keeps `/tmp`
in RAM, so copy them off before a reboot):

| file | contents |
|---|---|
| `summary.txt` | the run header and, per thread count, the lines below |
| `samples-T.csv` | once a second: `t, temp_c, throttled, arm_mhz, ra_cpu, cpu0..cpu3` |
| `profile-T.txt` | the core's `[wild7]` lines, one per 300 frames |
| `retroarch-T.log` | RetroArch's output |

Reading `summary.txt`, per thread count:

```
threads 3: exit 0, 61.2 s wall = 58.8 fps
  temp mean .. max .. C   min ARM .. MHz   retroarch CPU ..%
  per-core busy %: .. .. .. ..
  throttle flags: never set
  core: render mean .. max .. ms, update mean .. max .. ms (N windows, threads 3 bands 9)
```

- **fps** near 60 means it kept pace (paced runs use `vrr_runloop_enable`).
- **retroarch CPU** is in cores: 100% = one core. 137% means about 1.4.
- **throttle flags** are `vcgencmd get_throttled` values seen during the
  run; "never set" is the goal. Bits 0-3 are current (under-voltage, ARM
  frequency capped, throttled, soft temperature limit), bits 16-19 the same
  "has occurred since boot".
- **min ARM** should stay at 1,800 MHz; a drop means thermal or power
  trouble.
- **core render** is the band renderer's time per frame; the budget is
  16.67 ms and the target is to stay near half of it. Max values are single
  frames (a caption or cached screen built for the first time).
- `profile-T.txt` lines: `[wild7] frames a-b  render mean X max Y ms  update
  mean U max V ms  threads T bands B  state S` - `state` is the `ST_*`
  number at the end of each window, which tells which scene a spike belongs
  to.

The v3 reference (README, Performance): spin loop 3 threads render mean
6.6 ms, max 21.0 ms, RetroArch 137%; 1 thread 9.1 / 30.3 ms, 78%; HOLD &
SPIN 7.0 / 20.4 ms, 141%; EPIC coin shower 9.2 / 24.5 ms, 180%; 104 fps
unpaced; SoC peak 56 C at 1,800 MHz, never throttled. On the real display
with the CRT shader RetroArch uses about two cores.

---

## 8. Headless screenshots on the Pi

A core's own frame can be captured from SSH without touching the screen or
EmulationStation: run RetroArch with the null video driver and ask for a
screenshot after a fixed number of frames.

```
C=/tmp/w7null.cfg
{ cat /opt/retropie/configs/all/retroarch.cfg
  echo 'video_driver = "null"'
  echo 'audio_driver = "null"'
  echo 'input_driver = "null"'
  echo 'joypad_driver = "null"'
  echo 'menu_driver = "null"'
  echo 'video_shader_enable = "false"'
  echo 'config_save_on_exit = "false"'
  echo 'savestate_auto_load = "false"'
  echo 'savestate_auto_save = "false"'
} > $C
WILD7_AUTOPILOT=1 WILD7_FORCE=free \
  /opt/retropie/emulators/retroarch/bin/retroarch --config $C \
  -L ~/Wild7s/wild7_libretro.so ~/Wild7s/wild7.w7 \
  --max-frames=600 --max-frames-ss --max-frames-ss-path=/tmp/w7.png
```

Then fetch it: `pscp -batch -pwfile "$SP\pw.txt" "${PI}:/tmp/w7.png" .`

Limits: one frame per run (the last); the CRT shader cannot run under the
null driver (a run that tries fails silently with an empty log), and nothing
else on the display can be captured (`/dev/fb0` is blank because ES renders
through KMS/DRM, and no screen grabber is installed). What the cabinet
screen looks like with the shader has to be checked by eye.

For many frames, or frames at chosen moments with scripted input, use
`w7shot` - in WSL (see DEVELOPING.md), or on the Pi without zlib:

```
cd ~/Wild7s
gcc -O2 -DW7SHOT_NOPNG -Isrc tools/w7shot.c -o /tmp/w7shot -lm -lpthread
WILD7_AUTOPILOT=1 /tmp/w7shot -n 600 -s 120,599 -o /tmp    # writes .ppm
```

Frames from w7shot are the same pixels RetroArch would get from the core;
timings on the Pi are the real per-frame cost without RetroArch.

---

## 9. Backups

State of the copies after the v3.0.1 deploy on 2026-09-23:

| where | what | status |
|---|---|---|
| `C:\path\to\Wild7s` | the git repo: v2.2 baseline, the 7 feature branches, integration, docs | **current**; no git remote is configured |
| `~/Wild7s` on the Pi | the tree the live core was built from | current as of the last deploy |
| `~/Wild7s-v2.2` on the Pi | the v2.2 tree | rollback copy |
| `~/RetroPie/roms/ports/wild7/wild7-source-v2.2.tar.gz` (stick) | v2.2 source | rollback copy |
| `~/RetroPie/roms/ports/wild7/wild7-source.tar.gz` (stick) | v3.0.1 source (`git archive` of the deployed commit), with `README.md` and `docs/` beside it | current |
| `<cloud-drive>\Wild7s-v3.0.1-2026-09-23.bundle` | the whole git repo, every branch and all history (`git bundle --all`) | **current**; restore with `git clone <bundle> Wild7s` |
| `<cloud-drive>\Wild7s-v3.0.1-2026-09-23.tar.gz` | source tarball of the deployed commit | current |
| `<cloud-drive>\Wild7s` | mirror of the tree | refreshed to v3.0.1 on 2026-09-23 (files added and overwritten; nothing removed) |
| `<cloud-drive>\Wild7s-v2.3-2026-09-06.tar.gz` | dated tarball | 2026-09-06, version 2 era |
| `<usb-disk>\Wild7s-v2.0-<date>.tar.gz` | tarball on the external USB disk | v2.0; the disk was not attached when this was written, so not re-checked |

To refresh the off-machine copy after a change - a complete, restorable
copy of the repo with all its history is one command:

```powershell
git -C C:\path\to\Wild7s bundle create "<cloud-drive>\Wild7s-<version>-<date>.bundle" --all
# restore anywhere with:  git clone Wild7s-<version>-<date>.bundle Wild7s
```

A plain source tarball for the stick, next to the ROM, can be made the same
way as the deploy archive (`git archive --format=tar.gz HEAD`), copied with
`pscp` to `~/RetroPie/roms/ports/wild7/` under a versioned name.

---

## 10. Rolling back to v2.2

Version 2.2 is a complete, working game (5x5 ways, free spins with the
multiplier, pick bonus, four bet-multiple jackpots; no HOLD & SPIN, WHEEL,
7 STRIKE, GAMBLE, music or threaded renderer).

Fastest, on the Pi, from the kept tree:

```
cd ~/Wild7s-v2.2
ls -l wild7_libretro.so       # built already? if not, or unsure:
make
sudo bash ./install-retropie.sh
```

Then quit any running game (section 4) and start WILD 7's again.

From the git repo instead (the v2.2 baseline is commit `b9e335e`), package
it and build it in its own directory so the v3 tree is left alone:

```powershell
git archive --format=tar.gz -o "$SP\wild7-v2.2.tar.gz" b9e335e
pscp  -batch -pwfile "$SP\pw.txt" "$SP\wild7-v2.2.tar.gz" "${PI}:/home/pi/"
plink -batch -pwfile "$SP\pw.txt" $PI "mkdir -p ~/Wild7s-v2.2-git && tar -xzmf ~/wild7-v2.2.tar.gz -C ~/Wild7s-v2.2-git && cd ~/Wild7s-v2.2-git && make && sudo bash ./install-retropie.sh"
```

(v2.2 is one smaller file and builds much faster than v3.) The stick's
`wild7-source-v2.2.tar.gz` is a third source for the same tree.

Rolling forward again needs no rebuild while `~/Wild7s` still holds the v3
build: `cd ~/Wild7s && sudo bash ./install-retropie.sh`, then restart the
game.

Things that do not roll back cleanly:

- **Save states are not portable between versions.** A save state is a raw
  copy of the game struct, with no version check. A v3 state is larger than
  v2.2's struct, so v2.2 accepts it and loads garbage; a v2.2 state is
  refused by v3. Do not load a state made by the other version (RetroArch
  keeps them in its savestate directory, which on RetroPie is normally next
  to the content, `~/RetroPie/roms/ports/wild7/`).
- The installer rewrites the port config on every run, so the port config
  always matches whichever version was installed last.
- Core options for v3 (`wild7_music`, `wild7_threads`) stay in RetroArch's
  core options file; v2.2 ignores them.

---

## 11. Quick reference

```
# on the Pi
cd ~/Wild7s && make                                  # build (~3.5 min, in zellij)
cd ~/Wild7s && sudo bash ./install-retropie.sh       # install
md5sum ~/Wild7s/wild7_libretro.so ~/RetroPie/roms/ports/wild7/wild7_libretro.so /opt/retropie/libretrocores/lr-wild7/wild7_libretro.so
setsid -f nohup ~/w7launch.sh >/dev/null 2>&1 </dev/null   # own plink call
sudo systemctl restart getty@tty1                    # bring ES back
touch /tmp/es-restart; pkill -f "supplementary/emulationstation/emulationstation$"   # restart ES
bash tools/pi_soak.sh -t "1 3"                       # soak, in zellij
tail /tmp/ra.log                                     # last w7launch run
```
