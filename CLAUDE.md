# CLAUDE.md — Forza Horizon (Xbox 360) recompilation port

Handoff notes for any Claude session working on this repo. Read this first, then `ROADMAP.md`.

## Goal and current state

Port Forza Horizon 1 (Xbox 360, NTSC-U, Title ID 4D5309C9, default.xex v0.0.0.10) to Windows
first, then the Nintendo Switch, by static recompilation with ReXGlue. **This repo is the PC port
only**; the Switch port will be a separate repo started from this one once PC is fully playable.
Keep the inherited Switch pieces (sdk Horizon layer, shaders/, mesa/, tools/switch, reference/)
untouched until then. The base is the nfsmw-nx
project (NFS Most Wanted for Switch); its game-specific app lives in `reference/nfsmw-app/` as a
worked example.

Status (2026-09-30): the Windows build boots through the logos, title screen, intro video and
the in-engine cutscene into the intro drive, and leaves/reloads the XMedia/Speech modules without
crashing. Rendering uses the SDK's xenos GPU emulation (D3D12, host render targets). Everything
since the fiber fix (dc8f275) is in ROADMAP.md Stage 2.

Performance (old laptop, Intel HD 630 - its GTX 1050 was never usable and has since died): logos
60 fps, title/intro video ~19 fps, intro drive ~7 fps. GPU time per drive frame 137 ms: 54 draws, 30 EDRAM render
target transfers, 26 resolves, 19 textures. EDRAM emulation is ~55% of it, so 30 fps needs the
native renderer.

**Decision (user, 2026-09-30): make the game fully working with the current GPU emulation first,
then build the native renderer on PC.** Since 2026-09-30 the project lives on a new machine
(Microsoft Surface, i7-1065G7, Intel Iris Plus G7, no discrete GPU); the first full build there
succeeded.

## Legal rule (never break it)

No game data in git: no `.xex`, no disc files, and **none of the C++ generated from the game**
(`fh1/generated/`, `fh1/assets/` are gitignored). The user supplies their own disc image.

## The user's setup

- Windows 11 Pro on a Microsoft Surface (i7-1065G7, Intel Iris Plus G7 iGPU, 16 GB), PS4
  controller through DS4Windows (can show up twice: real pad + virtual Xbox 360 pad — handled by
  `fh1_merge_controllers`).
- Everything lives in `C:\Users\andre\Desktop\FH1-recomp\`:
  - the ISO, `game_root\` (full extracted disc), `README.txt`, `claude_memory\` (backup copy of
    Claude's memory notes), `_old\` (superseded extraction scripts, disc file list)
  - `fh1-recomp\` = this repo (clone), built by `build_fh1.bat`
  - `build_fh1.bat` — fetch + `reset --hard origin/main`, then `tools\build_windows.ps1`
  - `run_fh1.bat` — extracts the disc on first run, then runs fh1.exe with a log
  - `build_logs\` — every build step's log, `run-<date>.log`, and `run-<date>.log.crash.txt`
- Toolchain installed: LLVM/clang 23 (on PATH), CMake 4.4, Ninja, Python 3.13, Git,
  VS 2022 Build Tools (VCTools), VC++ redistributable.

## Fresh PC setup (after a Windows reinstall)

1. Install the graphics driver from the vendor and check Task Manager shows the GPU. Install
   Claude Code and log in. **Turn off Smart App Control** (Windows Security > App & browser
   control): it blocks every freshly built exe/dll (exit code 0xC0E90002, CodeIntegrity event
   3033). Check: `VerifiedAndReputablePolicyState` = 0 under
   `HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy`.
   If the repo folder was copied from another PC, delete `out\`, `fh1\out\` and `sdk\out\`: their
   CMake caches keep the old PC's tool paths (e.g. a missing ninja.exe).
2. In `C:\Users\andre\Desktop\FH1-recomp\`: put the ISO there, run
   `git clone https://github.com/AndryTheBeast/fh1-recomp.git`, and copy
   `fh1-recomp\tools\build_fh1.bat` and `run_fh1.bat` next to the ISO.
3. Run `fh1-recomp\tools\setup_windows.bat` (winget installs Git, CMake, Ninja, Python 3.13, LLVM,
   VS 2022 Build Tools C++ workload, VC++ redist, GitHub CLI; accept the UAC prompts).
4. Copy `fh1-recomp\tools\claude_settings.json` to `FH1-recomp\.claude\settings.json` so Claude can
   build, read logs and commit without asking each time. Set git's identity:
   `git config --global user.name AndryTheBeast` and `git config --global user.email antigotgvs@gmail.com`.
5. Run `run_fh1.bat` once (extracts the disc into game_root), then `build_fh1.bat`.

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

## Unattended testing (no one at the PC)

`tools/auto_test.ps1` runs fh1.exe for N seconds, screenshots the game window at chosen seconds
(`build_logs	est-<name>-<date>-<s>s.png`, read them with the Read tool) and prints the `[fps]`
lines. `-Autoplay` passes `--fh1_autoplay` (fh1/src/fh1_autoplay.h): a virtual pad that holds
buttons on a timetable. Route: title (Start) -> A to confirm -> forza_tone intro video (~82 s)
-> controls screen -> in-engine cutscene (same camera every run at ~160-176 s: best frames for
before/after screenshots) -> the drive. With nobody touching the pad:

    powershell -ExecutionPolicy Bypass -File toolsuto_test.ps1 -Name cut -Seconds 182 `
      -Shots "160,168,176" -Autoplay "34+0.3=start;36+0.3=start;38+0.3=start;41+0.3=a;43+0.3=a;46+0.3=a;124+0.3=a;127+0.3=a;131+0.3=a"

Append ";180+60=rt" (and a longer -Seconds) to hold the accelerator in the drive. Nothing steers
yet, so the car leaves the road; compare graphics on the cutscene frames, not the drive.

Use it for every graphics/performance change instead of asking the user to play.
The screenshots copy the screen: if Windows has locked (user away) they show the lock screen,
though the game and its `[fps]` lines still run. Look at one shot before trusting a batch. Load
times shrink on repeated runs (files cached by Windows), so the cutscene can start ~15 s earlier:
take shots every few seconds from ~110 s rather than trusting fixed seconds.

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
- `[fps]` lines (every 10 s, sdk/src/graphics/command_processor.cpp) give the real frame rate and
  a per-frame breakdown of the GPU thread (idle / WAIT_REG_MEM / draws / presenting).
- Check the `DXGI adapter:` log line before trusting any performance numbers (the old laptop
  had switchable graphics and D3D12 silently picked the iGPU).
- Staging files to the user's PC through the bridge sometimes delivers a stale copy: check the
  size on the device after writing, and use a new staged file name if it did not change.

## Next steps

0. First run on the Surface: check the `DXGI adapter:` line says Iris Plus, then run the
   unattended cutscene/drive test for new `[fps]` numbers (old HD 630 numbers are above; its raw
   logs are in `build_logs\archive-2026-09-29_30`).
1. ~~Log health check~~ done 2026-09-30 (Surface run test-surface1-20260930-043708, to the
   cutscene): nothing blocking. Harmless: failed opens of media\effects\, stringtables\en\,
   colourgradingmaps\, db\patch\, BadgesAndTitles\ (not on the disc); Kinect XAM message app FE
   msg 2B003; XamXStudioRequest / XamVoiceSetMicArrayIdleUsers / EtxProducerRegister /
   NetDll_getsockopt stubs; 2x BaseHeap::Release (also seen in codegen). `cache:\`: the first
   probe fails, then the game links `cache: => \Device\cache1` itself (NullDevice), flushes it,
   and never opens a file under it up to the drive - no streaming through it; revisit only if a
   later part of the game opens cache:\ paths. "PM4_DRAW_INDX_2 Failed in backend
   (edram_mode=6)" (~6/frame) are copy/resolve packets, one per "Resolve region is empty" -
   the scissored-away tiling strips, not lost geometry (see ROADMAP). `[io] LENTO` slow-open
   warnings: max 139 ms, during the level load only.
2. Glitches: dark square in the bottom-right corner (RTV path on Intel only; ROV is correct) and
   the hard-edged car shadow. Use RenderDoc on the cutscene frame. Re-check both on the Iris Plus
   (Intel again, so the dark square likely still shows).
3. Play past the intro drive (festival, menus, first races): autoplay cannot steer, so ask the
   user to play and send logs, or extend fh1_autoplay.
4. Then the native renderer on PC (`reference/nfsmw-app/src/nfsmw_nativo_*`, XenosRecomp shaders).
   The Switch port is a separate repository, later.
