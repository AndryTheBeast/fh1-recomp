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

Status (2026-10-02): **playable on PC** - boot, festival, free roam, races, garage, buying and
repainting cars (photos correct), saving. Legion Go (Ryzen Z1 Extreme): 30 fps (game cap) in
normal play, 26-28 in the busiest spots. Graphics = the SDK's emulated Xbox 360 GPU: D3D12 (default)
and Vulkan (`--gpu_backend=vulkan`), both correct. ROADMAP.md has the current plan (short);
docs/roadmap-history.md the detailed log of everything done; docs/performance-review.md where the
frame time goes and what to cut for the Switch.

Earlier machines: Intel HD 630 and Iris Plus G7 (~7 fps). Since 2026-10-01 a Lenovo Legion Go
(AMD Ryzen Z1 Extreme: Zen 4 + RDNA 3, 1920x1200). Full build ~14 min, app-only rebuild ~4 min.

User's decisions: game working with the GPU emulation first (done), then the native renderer on
PC (Vulkan, Stage 3); the Switch port later in a separate repo. 60 fps unlock and in-emulation
single-pass drawing were tried and dropped (they broke the picture).

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
      -Shots "160,168,176" -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a;124+0.3=a;127+0.3=a;131+0.3=a"

Append ";180+60=rt" (and a longer -Seconds) to hold the accelerator in the drive. Nothing steers
yet, so the car leaves the road; compare graphics on the cutscene frames, not the drive.

Use it for every graphics/performance change instead of asking the user to play. Boot with
the user's save (festival in ~40 s): -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a"
(the single-player A presses were moved 4 s earlier at the user's request). The user plays
manually for driving/race tests; a script/record/replay autoplay was tried on 2026-10-02 and
removed at the user's request (git history ffdb286..e8b043f).
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
- Instruction semantics must match the console, not just IEEE: vmsum3fp128/vmsum4fp128 give
  QNaN on float32 overflow (float64 sum), and FH1's collision GJK depends on it. When a guest
  algorithm misbehaves, compare our builder with Xenia's x64 sequences (x64_sequences.cc,
  x64_seq_vector.cc) - they encode many such console quirks. tools/run_ppc_tests.ps1 runs the
  instruction suite (upstream binutils in FH1-recomp\ppc_binutils, not in git); add a test for
  every quirk fixed.
- Performance work: --fh1_profile=N logs the busiest threads' top functions every N s (wait
  functions excluded from "working"); --gpu_log_waits shows what WAIT_REG_MEM waits on. In FH1
  the game main thread waits for the GPU command thread, so per-draw CPU cost there is the lever.
- Finding a game subsystem without symbols: --fh1_dump_image + the strings/xref scripts approach
  (ROADMAP, car collisions): strings -> lis/addi cross-references -> function -> hook and log.
- GPU settings (readback_resolve, vsync, gpu_*) belong to the xenos plugin DLL, which loads after
  OnPreSetup: rex::cvar::SetFlagByName on them is silently rejected there. Use
  rex::cvar::SetFlagAppDefault (deferred until the flag registers; command line still wins). Until
  2026-10-01 22:15 the readback_resolve=fast default never applied (log line "Settings:" at GPU
  setup shows the effective values) - car photos and garage bloom depended on it.
- auto_test.ps1 screenshots use PrintWindow (PW_RENDERFULLCONTENT), so another window covering the
  game (e.g. the Claude app popping up) no longer spoils the shot. tools/img_diff.py measures the
  festival-start shot (dome warmth: ~80 correct, below 0 = far scenery fogged).
- tools/compile_only.ps1 compiles single object files (ninja targets) while the game is running
  (the full build cannot relink a running exe/DLL). tools/bisect_transfers.py finds which EDRAM
  transfer kinds a scene needs (see docs/native-render-targets.md).
- Native renderer shaders (2026-10-02): `tools/build_shader_library.ps1` builds
  build_logs/shaders/fh1_shaders.nfsp from the disc (XenosRecomp in shaders/, now used and changed
  for FH1 under NFSMW_RECOMP; DXC with SPIR-V from FH1-recomp/tools_dxc - the Windows SDK's has none).
  Bash heredocs mangle backslash escapes (backslash-n, backslash-t) in Python/C++ snippets: write
  patch scripts with the Write tool or use chr(92).
- Upstream rexglue issue #420 is the thunk-pool crash fixed here (FunctionDispatcher::AllocateThunk).

## Next steps

See ROADMAP.md and docs/native-renderer-fh1.md: a native renderer built the nfsmw-nx way (user's
decision 2026-10-02; the emulated-GPU experiments in docs/native-render-targets.md are paused).
N0 shader library done for the loose .fxobj files (2,918 shaders); next: the LZX track archives,
then N1 (own graphics system, `fh1_renderer=native`).
