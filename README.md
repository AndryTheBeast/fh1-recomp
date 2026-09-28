# Forza Horizon (Xbox 360) — recompilation port

An in-progress, unofficial port of **Forza Horizon** (2012, Xbox 360) to Windows and, later, the Nintendo Switch.

The game's PowerPC program (`default.xex`) is statically recompiled to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). The project is built on the SDK, tools and documentation of
[nfsmw-nx](https://github.com/stevensnd) (Need for Speed: Most Wanted for Switch), which already solved many of the
Switch-side problems (memory, threads, Vulkan on NVK, shader pre-translation).

> [!IMPORTANT]
> **No game data lives in this repository**: no game files, no `default.xex`, and none of the C++ generated from it.
> You need your own legally obtained copy of the game. `.gitignore` keeps `fh1/assets/` and `fh1/generated/` out of git.

## Status

See [ROADMAP.md](ROADMAP.md). Current stage: **1 — getting the code generator to translate the game on PC.**

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

## Building the code generator

```sh
python tools/fetch_thirdparty.py
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/host --target rexglue
```

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
