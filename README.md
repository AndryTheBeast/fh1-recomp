# Forza Horizon (Xbox 360) — recompilation port

An in-progress, unofficial port of **Forza Horizon** (2012, Xbox 360) to **Windows (PC)**.

This repository is the PC port only. The Nintendo Switch port will live in a separate repository, started from
this one once the game is fully playable on PC. The Switch-side pieces inherited from nfsmw-nx (`sdk/` Horizon
layer, `shaders/`, `mesa/`, `tools/switch/`, `reference/`) stay here until then and are not built for PC.

The game's PowerPC program (`default.xex`) is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). The project is built on the SDK, tools and documentation of
[nfsmw-nx](https://github.com/stevensnd) (Need for Speed: Most Wanted for Switch), which already solved many of the
Switch-side problems (memory, threads, Vulkan on NVK, shader pre-translation).

> [!IMPORTANT]
> **No game data lives in this repository**: no game files, no `default.xex`, and none of the C++ generated from it.
> You need your own legally obtained copy of the game. `.gitignore` keeps `fh1/assets/` and `fh1/generated/` out of git.

## Status

**Playable on PC at full speed (since September 2026).** From a new game through the intro, the festival,
loading a save, races, the garage and the paint shop, with no crashes, at a steady **30 fps** (the game's own
frame cap on the Xbox 360).

**The default since October 2026: a native Vulkan renderer** that draws the game directly, without emulating the
Xbox 360 GPU. It is what the Switch port will need, so it is the one we want reports about. It runs at 30 fps
(an experimental option of the installer, since Alpha - 0.1.1, goes up to 60 where the PC is fast enough)
and still has some picture faults ([known issues](docs/install.md#known-issues)). Plan in [ROADMAP.md](ROADMAP.md),
details in [docs/native-renderer-status.md](docs/native-renderer-status.md).

The earlier path, ReXGlue's emulation of the Xbox 360 GPU (`--fh1_renderer=xenos`: Direct3D 12, or Vulkan with
`--gpu_backend=vulkan`), shows the console's exact picture. It is kept for comparison for now and will be
retired at some point.

### Test hardware

Development and testing now happen on a **Lenovo Legion Go** (handheld PC) with an **AMD Ryzen Z1 Extreme**
APU: Zen 4 CPU and an RDNA 3 integrated GPU, running Windows 11.

| Machine | GPU | Festival / free roam |
| --- | --- | --- |
| Lenovo Legion Go, Ryzen Z1 Extreme | Radeon (RDNA 3, integrated) | **28-30 fps**, correct picture |
| Microsoft Surface, Core i7-1065G7 (earlier) | Intel Iris Plus G7 | ~7 fps, shadow/corner glitches |
| Laptop, Core i7 (earlier) | Intel HD 630 | ~7 fps |

## Layout

| Folder | Contents |
| --- | --- |
| `fh1/` | The Forza Horizon project: manifest, overrides, app sources; `fh1/src/native/` is the native renderer |
| `sdk/` | ReXGlue SDK with nfsmw-nx's Horizon (Switch) layer and codegen changes |
| `shaders/` | XenosRecomp with nfsmw-nx's changes, and the shader library tools |
| `mesa/` | Patch for mesa-switch (NVK on the Switch) |
| `tools/` | Code generation, gap-finding and build scripts |
| `installer/` | The Windows installer of the pre-releases (C#) and the scripts that make a release's files |
| `docs/` | This project's documents ([index](docs/README.md)); nfsmw-nx's documentation is in `docs/nfsmw-nx/` |
| `reference/` | Leftovers of nfsmw-nx kept for the Switch port (its README, profile data) |

## Installing (pre-release)

Pre-releases come with a small Windows installer, `FH1Installer.exe`, on the
[Releases](https://github.com/AndryTheBeast/fh1-recomp/releases) page: you choose your own disc image (.iso, USA
version) and a folder, and it copies the game's files, downloads the pre-built port and prepares the shaders on
your PC. The guide, the known issues and how to report a problem (with the log the game always writes) are in
**[docs/install.md](docs/install.md)**. The installer is modelled on StevensND's installer for nfsmw-nx; its
source is in `installer/`.

## Building on Windows

1. `tools\setup_windows.bat` installs the tools (Git, CMake, Ninja, Python, LLVM, VS 2022 Build Tools).
2. Put your ISO, `tools\build_fh1.bat` and `tools\run_fh1.bat` in one folder next to this clone.
3. `run_fh1.bat` once extracts the disc; `build_fh1.bat` builds `fh1.exe`; `run_fh1.bat` plays it.

## Credits

- **Playground Games / Turn 10 / Microsoft** — creators of Forza Horizon. This is an unofficial fan project with no affiliation.
- **[stevensnd — nfsmw-nx](https://github.com/stevensnd)** — the Switch port this project starts from.
- **[GoatHonks](https://github.com/GoatHonks)** — nfsc-recomp (Need for
  Speed: Carbon Recompiled): Forza Horizon's native Vulkan renderer started from his, and his fixes keep being
  ported; his Switch port of it is the model for our Switch build.
- **[madelrandel-blip — NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled)**
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, built on the work of the **[Xenia](https://xenia.jp)** team.
- **[hedge-dev — XenosRecomp](https://github.com/hedge-dev/XenosRecomp)**, **mesa-switch** (danfromtico, NaGaa95),
  **devkitPro / switchbrew**, and the libraries in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

"Forza" and "Forza Horizon" are trademarks of Microsoft. Source code is GPL-3.0 (see [LICENSE](LICENSE)), inherited
from nfsmw-nx and NFSMW Recompiled; SDK changes are BSD-3-Clause, shader translator and Mesa changes MIT.
