# Forza Horizon recomp - pre-release 1 (v0.1.0-pre1)

The text of the GitHub release. Tag `v0.1.0-pre1`, marked as a pre-release. Files: `FH1Installer.exe`,
`fh1-win64.zip`, `fh1-shader-tools.zip`, `SHA256SUMS.txt` (all made by `installer\make_package.ps1` in
`installer\out\release`). **Nothing is published without the owner's yes at that moment.** The two zips and the
checksums were replaced on 2026-10-06 at the owner's request; the first upload's files are
kept in `build_logs\release-v0.1.0-pre1-first-upload`.

---

The first pre-release of the unofficial Windows port of **Forza Horizon** (2012, Xbox 360).

**You need your own disc image (.iso) of the game, USA version (NTSC-U).** Nothing of the game is in this
download; the installer reads your disc image on your computer.

## Updated on 6 October 2026

The downloads below were replaced with fixed ones (same version, same installer). What changed, all in the
default native renderer:

- **Characters are drawn**: the deer of the opening, the driver in the car, the presenter and the people at
  the diner were missing, see-through or scrambled.
- **The ground beside the road is no longer one flat color on a first run** (seen on the first drive of a new
  game).
- **Fewer long stutters on a first run**: shaders the game meets for the first time are prepared in the
  background again, so an object can show a moment late the first time instead of the game standing still.

**If you installed before this update:** start `FH1Installer.exe` again and choose the same `.iso` and the same
folder. It replaces the port and prepares the shaders again; your saves are not touched.

## Install

1. Download **`FH1Installer.exe`** below and start it (Windows shows "Windows protected your PC" because the
   program is not signed: *More info*, then *Run anyway*).
2. Choose your `.iso`, choose an empty folder, click **Install**. It copies the game's files, downloads the port
   and prepares the shaders for your PC: a few minutes.
3. Start the game with the **Forza Horizon** shortcut on your desktop.

**The first run is rough.** The first time the game shows something new (a place, a menu, a race), it stutters
and some objects appear a moment late, because the shaders are prepared on your PC right then. Each is prepared
only once: the same place is fine the next time, and the game gets smoother the more you play.

You only download `FH1Installer.exe`; it fetches the two zips itself. The full guide, with what to do about
Smart App Control, is in [docs/install.md](https://github.com/AndryTheBeast/fh1-recomp/blob/main/docs/install.md).

## What works

Playable from a new game through the intro, the festival, free roam, races, the garage, buying and painting cars,
and saving, at the game's own 30 frames per second. Made and tested on a Lenovo Legion Go (AMD Ryzen Z1
Extreme).

`FH1.exe` draws with the port's own **native Vulkan renderer**, which is lighter on the PC and is the base of the
future Nintendo Switch port; it still has some picture faults (below). Two more launchers in the folder show the
**Xbox 360's exact picture** through an emulation of its graphics chip, on Direct3D 12 or on Vulkan: use one of
them if something looks wrong. Reports about the native renderer are what helps most; the emulated launchers
will be removed at some point.

## Known issues

- On a first run, scenery can look wrong for a moment while its shaders are prepared (a piece of hillside
  hanging over the road, a blurry car): it heals by itself and is gone the next time.
- Only the USA disc. No Xbox Live, Kinect or downloadable content.
- The default native renderer: no ground at the Horizon Outpost of Montano Plains and a dark patch beside the
  road east of it; upside-down scenery patches in a mountain race; night colors too warm; garage and car damage
  not checked; a car's small picture (thumbnail) is sometimes wrong; a car's
  dashboard can stay dark; stutters and late objects the first time
  something is shown (see "The first run is rough" above); tried on AMD graphics only. The emulated launchers do not have these
  faults.
- The emulated launchers on Intel graphics: a dark square in a corner and a hard-edged shadow under the car.
- The busiest races can dip to 22-28 frames per second on the test PC.
- The programs are not signed: Smart App Control must be off (Windows cannot turn it back on without a reset).

## Found a problem?

Open an issue and attach the newest file of the `logs` folder next to `FH1.exe` (and `logs\fh1.crash.txt` after a
crash). Logging is always on in the pre-releases. A log can contain your Windows user name inside file paths.

## Thanks

StevensND (nfsmw-nx, and the installer this one is modelled on), GoatHonks (the Need for Speed: Carbon port the
native renderer started from), the ReXGlue and Xenia teams, hedge-dev (XenosRecomp). Not affiliated with
Microsoft, Turn 10 or Playground Games.
