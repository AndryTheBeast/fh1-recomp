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

Status (2026-09-30, end of the Surface session): playable from a new game through the intro,
the festival, loading a save and a full race (the user finished an event). Fixed that day: a crash
at the intro video (thunk pool), the crash loading a save (0xBE stack fill), the crash entering an
event (no cache: device). Rendering uses the SDK's xenos GPU emulation (D3D12, host render
targets = RTV). Everything since the fiber fix (dc8f275) is in ROADMAP.md Stage 2.

Known problems: the car drives above the road and clips into other cars; the HUD flashes; on
Intel only (fine on AMD), RTV shows a dark square bottom-right and a hard-edged car shadow (ROV draws both right).

Performance so far only on Intel iGPUs: HD 630 and Iris Plus G7 both ~7 fps in the drive and in
the festival (~3,800 draws per frame, the frame drawn in 3 strips), ~20 fps in videos. Speed
experiments (all off by default) are in ROADMAP "Stop-gap speed".

**Machine change (2026-10-01): the project moves to a Lenovo Legion Go (AMD Ryzen Z1 Extreme:
Zen 4 CPU + RDNA 3 iGPU, 1920x1200 screen).** Baseline there (2026-09-30 evening, commit dea7f49,
test-base / test-rov logs): festival + free-roam driving on RTV **28-30 fps** (the game's own
30 fps cap; Surface 7.4), host GPU ~28 ms/frame, GPU thread busy 100%, ~3,300 draws/frame. On AMD
the RTV picture is correct: no dark square, soft car shadow. ROV: 6-10 fps, GPU-bound (~100-120 ms
of host GPU per frame, almost all draws) - still only a reference. Full build ~14 min.

**Decision (user, 2026-09-30): make the game fully working with the current GPU emulation first,
then build the native renderer on PC.** User's order after that: a stop-gap speed-up first, then
the gameplay/visual bugs.

## Legal rule (never break it)

No game data in git: no `.xex`, no disc files, and **none of the C++ generated from the game**
(`fh1/generated/`, `fh1/assets/` are gitignored). The user supplies their own disc image.

## The user's setup

- Windows 11 on a Lenovo Legion Go (AMD Ryzen Z1 Extreme) from 2026-10-01; before that a Surface
  (Intel Iris Plus G7) and a laptop with an Intel HD 630. PS4 controller through DS4Windows (can
  show up twice: real pad + virtual Xbox 360 pad — handled by `fh1_merge_controllers`). The user
  is not a programmer: short plain explanations, and say exactly what to run.
- Everything lives in `C:\Users\andre\Desktop\FH1-recomp\`:
  - the ISO, `game_root\` (full extracted disc), `README.txt`, `claude_memory\` (backup copy of
    Claude's memory notes), `.claude\settings.json` (permissions, from tools\claude_settings.json)
  - `fh1-recomp\` = this repo (clone), built by `build_fh1.bat`
  - `build_fh1.bat` — fetch + `reset --hard origin/main`, then `tools\build_windows.ps1`
  - `run_fh1.bat` — extracts the disc on first run, then runs fh1.exe with a log
  - `build_logs\` — every build step's log, `run-<date>.log`, and `run-<date>.log.crash.txt`;
    `archive-2026-09-29_30.zip` (HD 630 laptop) and `archive-2026-09-30-surface\` (Iris Plus:
    logs zip + key screenshots, see its README.txt)
- The user's saves: `%USERPROFILE%\OneDrive\Documentos\fh1\B13EBABEBABEBABE\` — never delete.
  The emulated Xbox cache partition is `...\fh1\cache\xbox_utility\` (emptied at every start).
- Toolchain: LLVM/clang (on PATH), CMake, Ninja, Python 3.13, Git, GitHub CLI, VS 2022 Build Tools
  (VCTools), VC++ redistributable, RenderDoc (optional, `winget install BaldurKarlsson.RenderDoc`).
- The user's internet can drop (phone hotspot): build with `-SkipFetch` once sdk\thirdparty exists.

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
6. Bring the user's saves: `%USERPROFILE%\OneDrive\Documentos\fh1\` (OneDrive syncs it on the same
   account; otherwise copy it over by hand). Copy `FH1-recomp\claude_memory\*` to
   `%USERPROFILE%\.claude\projects\C--Users-andre-Desktop-FH1-recomp\memory\`.

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
(`build_logs\test-<name>-<date>-<s>s.png`, read them with the Read tool) and prints the `[fps]`
lines. `-Autoplay` passes `--fh1_autoplay` (fh1/src/fh1_autoplay.h): a virtual pad that holds
buttons on a timetable. Route: title (Start) -> A to confirm -> forza_tone intro video (~82 s)
-> controls screen -> in-engine cutscene (same camera every run at ~160-176 s: best frames for
before/after screenshots) -> the drive. With nobody touching the pad:

    powershell -ExecutionPolicy Bypass -File tools\auto_test.ps1 -Name cut -Seconds 182 `
      -Shots "160,168,176" -Autoplay "34+0.3=start;36+0.3=start;38+0.3=start;41+0.3=a;43+0.3=a;46+0.3=a;124+0.3=a;127+0.3=a;131+0.3=a"

Append ";180+60=rt" (and a longer -Seconds) to hold the accelerator in the drive. Nothing steers
yet, so the car leaves the road; compare graphics on the cutscene frames, not the drive.

Use it for every graphics/performance change instead of asking the user to play.
The screenshots copy the screen: if Windows has locked (user away) they show the lock screen,
though the game and its `[fps]` lines still run. Look at one shot before trusting a batch. Load
times shrink on repeated runs (files cached by Windows), so the cutscene can start ~15 s earlier:
take shots every few seconds from ~110 s rather than trusting fixed seconds.
With a save present (the user's progress lives in `%USERPROFILE%\OneDrive\Documentos\fh1\
B13EBABEBABEBABE\` - never delete it) the game skips the intro and loads the festival instead:
the old intro-route timings no longer apply.

Frame captures: RenderDoc is installed (winget). `auto_test.ps1 -RenderDoc -ExtraArgs
"--renderdoc_capture_seconds=125,145"` records single guest frames into
`build_logs\rdc-<name>-*.rdc`; `tools/rdc_dump.py` (run with qrenderdoc --python, see its header)
lists the actions and saves the render targets as PNG.

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
- A crash whose bad value is 0xBE / 190 (or 0xBEBEBEBE) was an uninitialized stack read: Xenia
  filled new stacks with 0xBE. Stacks are now zeroed like the console's (XThread::AllocateStack).
- The game verifies its data files: changing even one byte of media\*.zip (CRCs updated or not)
  makes it call XamShowDirtyDiscErrorUI at boot. Change settings in memory, never on disk.
- Run logs rotate at 5 MB (`run-*.1.log`, `.2.log`, ...): read all of them.
- Hooking a game function from the app: `REX_EXTERN(__imp__sub_X); REX_HOOK_RAW(sub_X) { ...
  __imp__sub_X(ctx, base); }` (see fh1/src/fh1_trace_load.cpp).
- OneDrive may keep the saves "online-only" on a new PC: `attrib +P -U <fh1 folder> /S /D` pins
  them to the device so the game never waits on a download (done on the Legion Go).
- `auto_test.ps1` used to print only the newest log part; it now reads the rotated parts too.
- Upstream rexglue issue #420 is the thunk-pool crash fixed here (FunctionDispatcher::AllocateThunk).

## Next steps

1. [done 2026-09-30] Legion Go set up and built; `DXGI adapter: AMD Radeon Graphics (0x1002)`.
2. [done, results in "Goal and current state"] Baseline on the Z1 Extreme. With a save present the
   game boots straight into the festival (~50 s on the Legion Go), so the unattended test is:

       powershell -ExecutionPolicy Bypass -File tools\auto_test.ps1 -Name base -Seconds 140 `
         -Shots "110,125,138" -Autoplay "34+0.3=start;36+0.3=start;38+0.3=start;41+0.3=a;43+0.3=a;46+0.3=a"

   Run it twice: default (RTV) and with `-ExtraArgs "--render_target_path_d3d12=rov"`. Compare
   `[fps]`, `[fps] host GPU per frame` and the screenshots with the Surface
   (`build_logs\archive-2026-09-30-surface\`: festival 7.4 fps, ~130 ms host GPU per frame).
   Questions: is ROV (correct picture) fast enough now? Does RTV still show the dark square and the
   hard car shadow on AMD? (On Intel the stencil-reference export is missing and xenia falls back;
   AMD has it.) Is the frame CPU-bound (`GPU thread busy` ~100% with low host-GPU time) or GPU-bound?
3. Stop-gap speed: on the Z1 Extreme RTV already reaches the game's 30 fps cap, so this matters
   only for weaker GPUs (and the Switch). Was: finish the single-strip mode (ROADMAP "Stop-gap speed":
   force window offset 0 and a full scissor for draws, strip 1's copy destination, one resolve of
   all rows), or real screen extents, depending on what the baseline shows.
4. Gameplay/visual bugs: car above the road and clipping into cars (collision / ground queries;
   see the save-loading crash notes in ROADMAP), HUD flashing, then whatever RTV still gets wrong.
5. Then the native renderer on PC (`reference/nfsmw-app/src/nfsmw_nativo_*`, XenosRecomp shaders).
   The Switch port is a separate repository, later.

Done and verified (details in ROADMAP): log health check (harmless warnings listed there), the
intro-video crash, the save-loading crash, the event crash (cache: mount), first event finished.
