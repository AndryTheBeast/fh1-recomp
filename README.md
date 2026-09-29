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

See [ROADMAP.md](ROADMAP.md). The game boots on Windows through the title screen and into the intro drive;
current work is crash fixes and performance.

## Layout

| Folder | Contents |
| --- | --- |
| `fh1/` | The Forza Horizon project: manifest, overrides, app sources (in progress) |
| `sdk/` | ReXGlue SDK with nfsmw-nx's Horizon (Switch) layer and codegen changes |
| `shaders/` | XenosRecomp with nfsmw-nx's changes, and the shader library tools |
| `mesa/` | Patch for mesa-switch (NVK on the Switch) |
| `tools/` | Code generation, gap-finding and build scripts |
| `docs/` | nfsmw-nx's documentation. Start with `docs/porting-another-game.md` |
| `reference/` | nfsmw-nx's game-specific app, kept as a worked example (native renderer, hooks, audio) |

## Building on Windows

1. `tools\setup_windows.bat` installs the tools (Git, CMake, Ninja, Python, LLVM, VS 2022 Build Tools).
2. Put your ISO, `toolsuild_fh1.bat` and `toolsun_fh1.bat` in one folder next to this clone.
3. `run_fh1.bat` once extracts the disc; `build_fh1.bat` builds `fh1.exe`; `run_fh1.bat` plays it.

## Credits

- **Playground Games / Turn 10 / Microsoft** — creators of Forza Horizon. This is an unofficial fan project with no affiliation.
- **[stevensnd — nfsmw-nx](https://github.com/stevensnd)** — the Switch port this project starts from.
- **[madelrandel-blip — NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled)**
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, built on the work of the **[Xenia](https://xenia.jp)** team.
- **[hedge-dev — XenosRecomp](https://github.com/hedge-dev/XenosRecomp)**, **mesa-switch** (danfromtico, NaGaa95),
  **devkitPro / switchbrew**, and the libraries in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

"Forza" and "Forza Horizon" are trademarks of Microsoft. Source code is GPL-3.0 (see [LICENSE](LICENSE)), inherited
from nfsmw-nx and NFSMW Recompiled; SDK changes are BSD-3-Clause, shader translator and Mesa changes MIT.
