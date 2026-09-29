# CLAUDE.md — Forza Horizon (Xbox 360) recompilation port

Handoff notes for any Claude session working on this repo. Read this first, then `ROADMAP.md`.

## Goal and current state

Port Forza Horizon 1 (Xbox 360, NTSC-U, Title ID 4D5309C9, default.xex v0.0.0.10) to Windows
first, then the Nintendo Switch, by static recompilation with ReXGlue. The base is the nfsmw-nx
project (NFS Most Wanted for Switch); its game-specific app lives in `reference/nfsmw-app/` as a
worked example.

Status (2026-09-29): the Windows build boots through the trademark screens, intro video and
the title screen ("PRESS START") with audio, using the SDK's xenos GPU emulation (D3D12).
With the XDK fiber functions hooked to the SDK's host fibers (commit dc8f275), Start leads to
the playable intro drive (1-2 fps on the GTX 1050). Leaving it crashed because the SDK refused to
reload XMediaFacade after an unload; fixed in `sdk/src/system/kernel_state.cpp` — **not tested yet**.

## Legal rule (never break it)

No game data in git: no `.xex`, no disc files, and **none of the C++ generated from the game**
(`fh1/generated/`, `fh1/assets/` are gitignored). The user supplies their own disc image.

## The user's setup

- Windows 11, GTX 1050, PS4 controller through DSX (shows up twice: real pad + virtual
  Xbox 360 pad — handled by `fh1_merge_controllers`, see below).
- Everything lives in `C:\Users\andre\Desktop\FH1-recomp\`:
  - the ISO, `game_root\` (full extracted disc), `file_list.txt`
  - `fh1-recomp\` = this repo (clone), built by `build_fh1.bat`
  - `build_fh1.bat` — fetch + `reset --hard origin/main`, then `tools\build_windows.ps1`
  - `run_fh1.bat` — extracts the disc on first run, then runs fh1.exe with a log
  - `build_logs\` — every build step's log, `run-<date>.log`, and `run-<date>.log.crash.txt`
- Toolchain installed: LLVM/clang 23 (on PATH), CMake 4.4, Ninja, Python 3.13, Git,
  VS 2022 Build Tools (VCTools), VC++ redistributable.

## Layout

| Path | What |
| --- | --- |
| `fh1/fh1_manifest.toml` | ReXGlue manifest: default.xex + two run-time modules (XMediaFacade, SpeechFacade) |
| `fh1/overrides.toml` | Hand-made codegen declarations, each with its reason, plus `[rexcrt]` hooks |
| `fh1/huecos.toml`, `fh1/*_huecos.toml` | Code gaps declared as functions (generated, then cleaned by tools) |
| `fh1/*_huecos_excluir.txt` | Gaps that are data (never declare them) |
| `fh1/src/fh1_app.h` | App: GPU plugin default `xenos`, merged controllers, crash report install |
| `fh1/src/fh1_crash_report.cpp` | Windows crash report: symbolized stack to `<log>.crash.txt` |
| `sdk/` | ReXGlue SDK (nfsmw-nx fork). `sdk/thirdparty` only holds changed files; `tools/fetch_thirdparty.py` fetches the rest |
| `tools/` | Build, extraction and gap tools (below) |
| `docs/` | nfsmw-nx docs — `docs/porting-another-game.md` is the plan we follow |

## How a fix cycle works

1. User runs `build_fh1.bat` then `run_fh1.bat`, presses Start etc.
2. Read `build_logs\run-*.log` and, on a crash, `run-*.log.crash.txt`. Frames named
   `sub_XXXXXXXX` are translated game functions at that Xbox address.
3. The generated files on the PC are partitioned differently from any other checkout, so read
   the crash line in the PC's own `fh1-recomp\fh1\generated\default\fh1_recomp.N.cpp`.
4. Fix in `fh1/overrides.toml` / gap files / app code, run codegen, verify, commit, push.

Codegen: `rexglue codegen fh1_manifest.toml` from `fh1/` (~3 min). After editing gap files,
`tools/huecos_iterar.sh` repeats codegen + clean-up until stable. Always finish with
`python tools/comprobar_simbolos.py fh1` (every registered function defined) and
`grep REX_FATAL fh1/generated/*/*.cpp` (must be empty).

## Tools written for this port

- `tools/extract_xiso.ps1` — XDVDFS extractor (XGD2/XGD3), `-OnlyXex` for just default.xex.
- `tools/build_windows.ps1` — full Windows build (VS dev shell, fetch, rexglue, codegen, fh1.exe).
- `tools/huecos.py` (from nfsmw-nx) — lists code gaps.
- `tools/huecos_pasada.py` — drops data / import-area gaps, splits thunk runs, `--restos` declares
  the rest of partly covered gaps.
- `tools/fusionar_continuaciones.py` — merges gaps that are tails of the previous function
  (`{ end = ... }`) from codegen.log's unresolved-branch lines. Per module with `--gen/--huecos`.
- `tools/huecos_iterar.sh` — loops the two tools above with codegen.
- `tools/comprobar_simbolos.py` — registered-but-undefined check (catches link errors early).

## Things learned the hard way

- PowerShell variable names are case-insensitive (`$SECTOR` == `$sector`).
- Git on Windows checks symlinks out as text stubs; `fetch_thirdparty.py` resolves them.
- CMake 4 needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for bundled projects.
- The codegen rewrites a stamp in `fh1_manifest.toml`; `build_fh1.bat` uses `reset --hard`.
- The nfsmw-nx SDK defaults to no GPU (NFS renders natively): fh1 sets `gpu_plugin = xenos`.
- `imgui`/`renderdoc` include paths must be passed to the app when the SDK is a subdirectory.
- Gaps: "not in any code region" = data; "outranked by import" = import thunk area (declaring
  it causes "undefined symbol"). Several small functions can share one gap.
- The game links the XDK fiber functions; they must be hooked as a family via `[rexcrt]`.
- A crash with exception 0x80000003 in sub_82C09F00 is the game's own fatal handler (an
  intentional `b .` loop, compiled to a trap). Look just before it in the log for the reason,
  e.g. `XamShowDirtyDiscErrorUI` = a file or module load failed.
- XexLoadImage/XexUnloadImage now log at debug level; module reload problems show up there.
- Staging files to the user's PC through the bridge sometimes delivers a stale copy: check the
  size on the device after writing, and use a new staged file name if it did not change.

## Next steps

1. Test the XMediaFacade reload fix: finish/leave the intro drive -> profile -> main menu.
2. Keep fixing run-time crashes and missing kernel/XAM behaviour until a race is drivable.
3. Then the Switch: SDK Horizon layer, native renderer (`reference/nfsmw-app/src/nfsmw_nativo_*`),
   shader library via XenosRecomp, performance (see `docs/porting-another-game.md`).
