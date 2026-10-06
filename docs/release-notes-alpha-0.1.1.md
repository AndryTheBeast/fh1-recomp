# Forza Horizon recomp - Alpha - 0.1.1 (tag alpha-0.1.1)

The text of the GitHub release is the part below the line. Tag `alpha-0.1.1`, named "Alpha - 0.1.1", marked as
a pre-release. Files: `FH1Installer.exe`, `fh1-win64.zip`, `fh1-shader-tools.zip`, `SHA256SUMS.txt` (all made by
`installer\make_package.ps1` in `installer\out\release`). **Nothing is published without the owner's yes at
that moment.**

---

**Alpha - 0.1.1** of the unofficial Windows port of **Forza Horizon** (2012, Xbox 360).

**You need your own disc image (.iso) of the game, USA version (NTSC-U).** Nothing of the game is in this
download; the installer reads your disc image on your computer.

## New: 60 frames per second (experimental)

The game runs at 30 frames per second, its limit on the Xbox 360. The installer now has an option,
**"60 frames per second (experimental)"**: with it the game draws and moves 60 times a second where your PC is
fast enough, at the game's normal speed.

- It is **off unless you tick it**, and it works with the default native renderer.
- **How much you get depends on your graphics chip.** On the test PC (Lenovo Legion Go, AMD Ryzen Z1 Extreme)
  the opening runs at 50-58 frames per second and driving at 32-40: more than 30, not 60. A stronger graphics
  chip should hold more.
- **It is new and little tested.** Checked so far: the game's speed against the clock, the picture and the far
  scenery on the first drive of a new game. Not checked yet: races, the festival's crowd, traffic, buying a
  car, night. If something moves too fast or looks wrong, start the installer again, choose the same folder,
  untick the option and click **Update** (a second; your saves are not touched), and please open an issue.
- The method comes from [pinyon-shift](https://github.com/arcanite24/pinyon-shift), another recompilation of
  the same game (BSD 3-Clause).

Everything else is as in Alpha - 0.1.0.

**If you have an earlier version installed:** start this `FH1Installer.exe` and choose the same `.iso` and the
same folder. It replaces the port; the shaders are not prepared again and your saves are not touched.

## Install

1. Download **`FH1Installer.exe`** below and start it (Windows shows "Windows protected your PC" because the
   program is not signed: *More info*, then *Run anyway*).
2. Choose your `.iso`, choose an empty folder, tick the 60 frames option if you want it, click **Install**. It
   copies the game's files, downloads the port and prepares the shaders for your PC: a few minutes.
3. Start the game with the **Forza Horizon** shortcut on your desktop.

**The first run is rough.** The first time the game shows something new (a place, a menu, a race), it stutters
and some objects appear a moment late, because the shaders are prepared on your PC right then. Each is prepared
only once: the same place is fine the next time, and the game gets smoother the more you play.

You only download `FH1Installer.exe`; it fetches the two zips itself. The full guide, with what to do about
Smart App Control, is in [docs/install.md](https://github.com/AndryTheBeast/fh1-recomp/blob/main/docs/install.md).

## What works

Playable from a new game through the intro, the festival, free roam, races, the garage, buying and painting cars,
and saving. Made and tested on a Lenovo Legion Go (AMD Ryzen Z1 Extreme).

`FH1.exe` draws with the port's own **native Vulkan renderer**, which is lighter on the PC and is the base of the
future Nintendo Switch port; it still has some picture faults (below). Two more launchers in the folder show the
**Xbox 360's exact picture** through an emulation of its graphics chip, on Direct3D 12 or on Vulkan: use one of
them if something looks wrong. Reports about the native renderer are what helps most; the emulated launchers
will be removed at some point.

## Known issues

- With the 60 frames option: not every part of the game is checked (above); the frame rate moves between 30
  and 60 with the scene.
- On a first run, scenery can look wrong for a moment while its shaders are prepared (a piece of hillside
  hanging over the road, a blurry car): it heals by itself and is gone the next time.
- Only the USA disc. No Xbox Live, Kinect or downloadable content.
- The default native renderer: night colors too warm; garage and car damage not checked; a car's small picture
  (thumbnail) is sometimes wrong; a car's dashboard can stay dark; stutters and late objects the first time
  something is shown (see "The first run is rough" above); tried on AMD graphics only. The emulated launchers
  do not have these faults.
- The emulated launchers on Intel graphics: a dark square in a corner and a hard-edged shadow under the car.
- The busiest races can dip to 22-28 frames per second on the test PC.
- The programs are not signed: Smart App Control must be off (Windows cannot turn it back on without a reset).

## Found a problem?

Open an issue and attach the newest file of the `logs` folder next to `FH1.exe` (and `logs\fh1.crash.txt` after a
crash), and say whether the 60 frames option is on. Logging is always on in the alpha versions. A log can
contain your Windows user name inside file paths.

## Thanks

StevensND (nfsmw-nx, and the installer this one is modelled on), GoatHonks (the Need for Speed: Carbon port the
native renderer started from), the Pinyon Shift contributors (the 60 frames method), the ReXGlue and Xenia
teams, hedge-dev (XenosRecomp). Not affiliated with Microsoft, Turn 10 or Playground Games.
