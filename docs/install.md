# Installing the pre-release (Windows)

This is an early, unofficial port of **Forza Horizon** (2012, Xbox 360) to Windows. It is a pre-release: it is
playable from start to finish on the PC it was made on, and it still has faults (see [Known issues](#known-issues)).

Nothing of the game comes with the port. You need **your own disc image (.iso) of the game**; the installer reads
it on your computer and nothing leaves your computer.

## Before you start

- **Windows 10 or 11, 64-bit**, a graphics card with a current driver (the game draws with Vulkan by default;
  Direct3D 12 for one of the other launchers), about **9 GB** of free space, and a controller (an Xbox pad, or a PlayStation pad through DS4Windows
  or Steam Input).
- **Only the USA version of the game (NTSC-U) works right now.** The installer tells you which disc it found and
  warns you about any other.
- The port has been made and tested on one PC (a Lenovo Legion Go: AMD Ryzen Z1 Extreme with Radeon graphics).
  Other AMD and NVIDIA cards should work but have not been tried; Intel graphics have known picture faults.
- **The programs are not signed.** That has two consequences:
  - **Smart App Control** (Windows 11, *Windows Security > App & browser control > Smart App Control*) blocks
    unsigned programs completely: with it on, neither the installer nor the game will start. It has to be off.
    **Windows cannot turn Smart App Control back on without resetting Windows**, so decide for yourself whether
    this port is worth that to you. On most PCs it is already off.
  - **SmartScreen** shows "Windows protected your PC" the first time you start the installer: click *More info*,
    then *Run anyway*. You can check the files against `SHA256SUMS.txt` on the release page first.
- Some antivirus programs dislike small unsigned installers. The installer's source is in this repository
  (`installer/`), and it only does what this page describes.

## Installing

1. Download **`FH1Installer.exe`** from the release page and start it.
2. **Needed on this PC**: the first line must be green. If the *Microsoft Visual C++ runtime* is missing, click
   **Install runtime**: the installer fetches Microsoft's own installer from Microsoft and starts it (Windows asks
   for permission).
3. **Your disc image**: click **Choose ISO...** and pick your Forza Horizon `.iso`. In green it should say
   "Forza Horizon found ... the USA version".
4. **Where to install**: click **Choose folder...** and pick an **empty** folder (make a new one).
5. Click **Install** and wait. It copies the game's files out of your disc image (about 7 GB), downloads the port
   (about 50 MB) and prepares the game's shaders for your PC. On the test PC all of it takes under four minutes;
   on a slower PC or drive, allow ten or more. The PC is busy meanwhile, and the game's window may flash up for a
   second: that is normal. **Cancel** stops it; **Install** later goes on from where it stopped.
6. When it says "Installed", you are done. You can delete the installer and keep your `.iso` wherever you like:
   the game no longer needs it.

## Starting the game

| Start it with | What it does |
| --- | --- |
| The **Forza Horizon** shortcut on the desktop, or `FH1.exe` in the folder | The default: the port's own **native Vulkan renderer**. Lighter on the PC; it still has some picture faults (see below). The first start shows "Preparing shaders" for some tens of seconds. |
| `FH1 (emulated Direct3D 12).bat` | The Xbox 360's exact picture, drawn through an emulation of its graphics chip on Direct3D 12. **Use this one if something looks wrong.** |
| `FH1 (emulated Vulkan).bat` | The same exact picture drawn through Vulkan. |

The native renderer is the one this project is going on with (the planned Nintendo Switch port is built on it),
so **reports about it are what helps most**. The two "emulated" launchers are there to compare with and to fall
back on; they will be removed at some point.

The game runs at **30 frames per second**: that is the game's own limit on the Xbox 360. **F3** shows a frame
rate and frame time monitor, **F4** the port's settings.

Your saves are kept in your Documents folder, in `fh1`. They stay there when you update or delete the game.

## Updating

Start the newer `FH1Installer.exe`, choose the same `.iso` and the **same folder**: the button reads **Update**.
It replaces the port and prepares the shaders again only if that is needed.

## Removing

Delete the folder you installed into and the desktop shortcut. Your saves (Documents, `fh1`) are yours to keep or
delete.

## Known issues

Everywhere:

- Only the USA disc works. Xbox Live, Kinect and downloadable content are not supported.
- The busiest race scenes can dip to 22-28 frames per second on the test PC.

The default, the native renderer (`FH1.exe`). The two "emulated" launchers do not have these faults:

- **The ground is missing at the Horizon Outpost of Montano Plains** (a dark hole under the tents and the stage),
  and a large flat dark patch lies beside the road just east of it.
- **In a mountain road race, patches of the hillside show an upside-down picture** of sky, mountains and trees.
- At night the colors are warmer and greyer than they should be.
- The garage and car damage have not been checked.
- The first time the game meets something new, an object can appear a moment late (its shaders are prepared on
  your PC right then, once).
- It has only been tried on AMD graphics.

The two "emulated" launchers:

- **Intel graphics**: a dark square in the bottom-right corner and a hard-edged shadow under the car.
- A soft rectangle can show under the car.

## Reporting a problem

Please open an issue on GitHub (the *Issues* tab of this repository) and attach the log of the run that went
wrong. Logging is always on in the pre-releases:

- In the folder you installed into, open **`logs`**. Each start of the game writes one file there
  (`fh1_001.log`, `fh1_002.log`, ...; the newest 20 are kept). Attach **the newest one**, or the one from the run
  with the problem.
- If the game crashed, **`logs\fh1.crash.txt`** is written as well: attach it too.
- Say which launcher you used, your graphics card, where in the game it happened, and what you saw. A
  screenshot helps a lot.

A log holds technical lines only (your graphics card, what the game loaded, frame rates, errors). It does contain
**file paths of your PC, which can include your Windows user name**: open it in Notepad first if that matters to
you. Never attach files of the game itself.

## Credits

The installer is modelled on **StevensND**'s installer for nfsmw-nx, the project this port started from. The
native renderer started from **GoatHonks**' Need for Speed: Carbon port. The game's code is translated with
**ReXGlue**. Full credits and licenses: [README](../README.md#credits) and
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
