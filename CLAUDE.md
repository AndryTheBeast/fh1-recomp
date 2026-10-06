# CLAUDE.md — Forza Horizon (Xbox 360) recompilation port

Handoff notes for any Claude session working on this repo. Read this first, then `ROADMAP.md` and
`docs/native-renderer-status.md` (the current work).

## Goal and current state

Port Forza Horizon 1 (Xbox 360, NTSC-U, Title ID 4D5309C9, default.xex v0.0.0.10) to Windows
first, then the Nintendo Switch, by static recompilation with ReXGlue. **This repo is the PC port
only**; the Switch port will be a separate repo started from this one once PC is fully playable.
Keep the inherited Switch pieces (sdk Horizon layer, shaders/, mesa/, tools/switch, reference/)
untouched until then. The base is the nfsmw-nx
project (NFS Most Wanted for Switch). Its game-specific app was kept in `reference/nfsmw-app/` as a
worked example until 2026-10-04 (deleted at the user's request, still in git history); the better
worked example now is nfsc-recomp (`..\repos\nfsc-recomp-main`).

Status (2026-10-04): **playable on PC** - boot, festival, free roam, races, garage, buying and
repainting cars (photos correct), saving. Legion Go (Ryzen Z1 Extreme): 30 fps (game cap) in
normal play, 26-28 in the busiest spots. Default graphics = the SDK's emulated Xbox 360 GPU: D3D12
(default) and Vulkan (`--gpu_backend=vulkan`), both correct. In progress: the native Vulkan renderer
(`--fh1_renderer=native`): boot, videos, menus and loading screens match the emulated picture; the festival
in daylight and at evening is close to it; driving at night works since 2026-10-05 (headlights light the road,
no blue on the car, map selector); reflections soft and textures on the
console's gamma curve since the third session of 2026-10-05; smooth edges (four samples per pixel) since the
same day; first-person view, brightness jumps and the darker picture fixed in the fourth session (brightness now
equal to the emulated picture at the festival); design creator fixed in the fifth and sixth sessions (booth, wheels, paint, dialog, ambient light, tyres); the photos of a saved car since 2026-10-06; still open: night colors, races, garage, car damage. User, 2026-10-06: those wait; next is the offline shader library for the PC, then the first pre-release. ROADMAP.md has the current plan (short); docs/README.md lists the documents;
docs/history/roadmap-history.md is the detailed log of everything done; docs/performance-review.md where
the frame time goes and what to cut for the Switch.

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
- Everything lives in `%USERPROFILE%\Desktop\FH1-recomp\`:
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
2. In `%USERPROFILE%\Desktop\FH1-recomp\`: put the ISO there, run
   `git clone https://github.com/AndryTheBeast/Forza-Horizon-Windows-Port.git fh1-recomp`, and copy
   `fh1-recomp\tools\build_fh1.bat` and `run_fh1.bat` next to the ISO.
3. Run `fh1-recomp\tools\setup_windows.bat` (winget installs Git, CMake, Ninja, Python 3.13, LLVM,
   VS 2022 Build Tools C++ workload, VC++ redist, GitHub CLI; accept the UAC prompts).
4. Copy `fh1-recomp\tools\claude_settings.json` to `FH1-recomp\.claude\settings.json` so Claude can
   build, read logs and commit without asking each time (since the repository is public, 2026-10-06, the file
   holds `%USERPROFILE%` and `YOUR_EMAIL`: put the real folder and address into the copy). Set git's identity:
   `git config --global user.name AndryTheBeast` and `git config --global user.email <the owner's e-mail>`.
5. Run `run_fh1.bat` once (extracts the disc into game_root), then `build_fh1.bat`.
6. Bring the user's saves: `%USERPROFILE%\OneDrive\Documentos\fh1\` (OneDrive syncs it on the same
   account; otherwise copy it over by hand). Copy `FH1-recomp\claude_memory\*` to
   `%USERPROFILE%\.claude\projects\C--Users-andre-Desktop-FH1-recomp\memory\`.

## Layout

| Path | What |
| --- | --- |
| `fh1/fh1_manifest.toml` | ReXGlue manifest: default.xex + two run-time modules (XMediaFacade, SpeechFacade) |
| `fh1/overrides.toml` | Hand-made codegen declarations, each with its reason, plus `[rexcrt]` hooks |
| `fh1/gaps.toml`, `fh1/*_gaps.toml` | Code gaps declared as functions (generated, then cleaned by tools) |
| `fh1/*_gaps_exclude.txt` | Gaps that are data (never declare them) |
| `fh1/src/fh1_app.h` | App: GPU plugin default `xenos`, merged controllers, crash report install |
| `fh1/src/fh1_crash_report.cpp` | Windows crash report: symbolized stack to `<log>.crash.txt` |
| `sdk/` | ReXGlue SDK (nfsmw-nx fork). `sdk/thirdparty` only holds changed files; `tools/fetch_thirdparty.py` fetches the rest |
| `tools/` | Build, extraction and gap tools (below) |
| `docs/` | Our documents (index: `docs/README.md`); `docs/history/` = finished work; `docs/nfsmw-nx/` = the inherited nfsmw-nx docs (`porting-another-game.md` is the plan we follow) |

## How a fix cycle works

1. User runs `build_fh1.bat` then `run_fh1.bat`, presses Start etc.
2. Read `build_logs\run-*.log` and, on a crash, `run-*.log.crash.txt`. Frames named
   `sub_XXXXXXXX` are translated game functions at that Xbox address.
3. The generated files on the PC are partitioned differently from any other checkout, so read
   the crash line in the PC's own `fh1-recomp\fh1\generated\default\fh1_recomp.N.cpp`.
4. Fix in `fh1/overrides.toml` / gap files / app code, run codegen, verify, commit, push.

Codegen: `rexglue codegen fh1_manifest.toml` from `fh1/` (~3 min). After editing gap files,
`tools/gaps_iterate.sh` repeats codegen + clean-up until stable. Always finish with
`python tools/check_symbols.py fh1` (every registered function defined) and
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
- `tools/gaps.py` (from nfsmw-nx) — lists code gaps.
- `tools/gaps_pass.py` — drops data / import-area gaps, splits thunk runs, `--rests` declares
  the rest of partly covered gaps.
- `tools/merge_continuations.py` — merges gaps that are tails of the previous function
  (`{ end = ... }`) from codegen.log's unresolved-branch lines. Per module with `--gen/--gaps`.
- `tools/gaps_iterate.sh` — loops the two tools above with codegen.
- `tools/check_symbols.py` — registered-but-undefined check (catches link errors early).

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
  transfer kinds a scene needs (see docs/history/native-render-targets.md).
- Native renderer shaders (2026-10-02): `tools/build_shader_library.ps1` builds
  build_logs/shaders/fh1_shaders.nfsp from the disc (XenosRecomp in shaders/, now used and changed
  for FH1 under NFSMW_RECOMP; DXC with SPIR-V from FH1-recomp/tools_dxc - the Windows SDK's has none).
  Bash heredocs mangle backslash escapes (backslash-n, backslash-t) in Python/C++ snippets: write
  patch scripts with the Write tool or use chr(92).
- Upstream rexglue issue #420 is the thunk-pool crash fixed here (FunctionDispatcher::AllocateThunk).
- Never run `sed -i` (or any rewrite) over every tracked file: on 2026-10-04 it turned CRLF into LF in ~1,000
  files and changed their dates, which forces a full rebuild. Rewrite only the files that contain the text
  (`grep -l` first).

- English names (2026-10-04): all code is in English (docs/history/handoff-english-rename.md has the list of renamed
  files and options).
- FH1's own native renderer (2026-10-04): `fh1/src/native/fh1_*`, namespace `fh1::native`, every setting `fh1_*`;
  run it with `--fh1_renderer=native`, shader library `fh1_shaders.nfsp` next to fh1.exe. It started as
  nfsc-recomp's renderer (his files are `nfsc_*`): to compare with his repo, replace `nfsc_`/`NFSC_` by
  `fh1_`/`FH1_` in his file first. `fh1/src/native/README.md` lists which of his fixes were taken and when. The
  earlier black-screen attempt (`fh1/src/fh1_native_*.cpp`) and its copy of the library reader were removed.

## Next steps

**Read docs/native-renderer-status.md first.** User decision 2026-10-04: the native renderer must be FH1's own
(`--fh1_renderer=native`), taking from nfsc-recomp the fixes that work for FH1 instead of running his renderer
as is; done the same day. Boot, videos, menus and loading screens were fixed on 2026-10-04 and checked by the
user. The same night the 3D scene got its first fixes (smear, shadows, crowd and trees; the user also saw the
giant polygon flashes gone). Late that night, unattended: **crowd animations and the over-sharp picture were
fixed** (animated people, FXAA outlines, HDR resolved textures, exposure; the user has not seen them yet: ask).
**2026-10-05: the green car at evening is fixed** (the game clears the shadow / headlight mask through a depth
buffer on the same EDRAM; `--fh1_native_depth_fill_color`). Next: the rest of that document's open list (ask the
user about the specks at night and for a short drive, then the slightly dark picture, 4x MSAA). The red glow of
the tail lights was fixed the same day (occlusion queries answered like the emulated GPU,
`--fh1_native_occlusion=0`). The shader library and fh1.exe must be built from the same sources (the shared
constants block changed size that night). ROADMAP.md is the checklist of
everything done and still open (keep it that way: one line per item, `[x]` / `[ ]`, details in
docs/history/roadmap-history.md); docs/native-renderer-fh1.md has the phases (N0-N5).

- EDRAM aliasing (2026-10-05): on the console a color target and a depth buffer with the same base are the same
  memory, and the game uses it (a depth-only rectangle with Z = 1 and stencil FF = a white color target). The
  emulated GPU does this by itself; the native renderer has separate images and needs each case handled. When
  a native picture is wrong only in some channels or keeps old contents, compare that render target after each
  pass with a RenderDoc capture of the emulated GPU (tools/rdc_*.py) before reading shaders.
- 2026-10-05, second session: the three things of the user's night drive are fixed and seen by the user.
  Headlights on the road: the shaders now get all 256 boolean constants (the scenery tests pixel b100; only
  b0-b15 per stage were passed). Map selector: k_DXT3A textures. Blue outline / blue window and bumper: the
  clear value of 10-bit and 7e3 float targets was read as four bytes, so the reflection cube map was wiped
  bright blue. Details and numbers in docs/native-renderer-status.md; the prompt for the next session is in
  docs/next-session-prompt.md.
- A fault that shows only at some places and is a flat color: think of a clear color before any shader. A
  clear value is packed in the target's own format (7e3 floats for k_2_10_10_10_FLOAT).
- Spots the unattended test does not reach (the road, night, the map screen): the user starts
  `run_native_capture.bat`, `run_emulated_capture.bat` or `run_native_skip.bat` (copies of tools\run_*.bat
  next to run_fh1.bat), drives there, stops and types "now"; then `tools\capture_now.ps1` (frame dump + trace,
  or a RenderDoc capture of the emulated GPU) or `tools\skip_cycle.ps1` (leaves out one shader at a time and
  photographs the window). Night falls about 5 minutes after launch; the car parked at the festival reaches
  night unattended, but the festival does not show every fault (it did not show the blue).
- The user may pick up the pad and drive a test window that Claude started: say that a window is a test before
  it opens. Claude cannot build while the user's game is open: ask them to close it.
- The first run after a new shader library froze on the loading screen once (2026-10-05; three runs after it
  were fine; cause not confirmed).
- A test window started with a temporary option looks like the normal game to the user: say so before they
  look (on 2026-10-05 a glow seen in such a window was taken for an effect of the build).
- 2026-10-05, third session: the reflection cube map has its nine levels (the game renders them; each fetch now
  honors its level of detail), textures with the gamma sign use the console's piecewise-linear curve in the shader
  (the host's sRGB formats were darker), and the loading-screen freeze is explained: the game stops sending
  commands for good after one frame of about 3.2 s or more (it was the pipelines of a new shader library being
  compiled; they are now compiled by several threads during the logo videos).
- Same session, part 2: the scene's 4x MSAA passes are drawn at twice the size and averaged by the resolve
  (`--fh1_native_ssaa`, smooth edges, still 30 fps parked); the shared constants block is 256 words; AMD has no blit
  for depth formats (the scene's resolved depth stays at twice the size). Measured occlusion queries are prepared
  behind `--fh1_native_occlusion=1` (the tail lights lose most of their halo with it: not the default).
- End of that session, from the user's drive: edges good, night colors fine, glows through walls accepted as the
  game's own behavior. Two faults to fix first next time: the first-person view breaks the picture (white world,
  broken interior) and the picture is too bright for ~2.5 s after a loading screen (native only, not every run).
- One comparison of two runs is not a result when the thing compared comes and goes: on 2026-10-05 an option was
  blamed for the brightness overshoot from one run each, and the next run showed the overshoot with it off.
- A resolve only gives the pitch of its texture (a multiple of 32). A texture narrower than that (the cube levels
  of 16x16 and less) must get its width from the fetch constant, or it is read with empty columns.
- Before chasing a difference between two screenshots of the festival, check that it is not the moment: lights and
  reflections change within seconds around 92-95 s, and runs differ by a second or two (take a shot every 3 s).
- Do not edit sources while a build is running: a header changed in the middle gives a mixed build (it happened on
  2026-10-04; rebuild after the last edit). `build_windows.ps1 -SkipCodegen` still runs the code generator when
  rexglue.exe was relinked.

- 2026-10-05, fourth session: the first-person view, the brightness jump after loading / view switches and the
  darker picture are fixed (rectangle lists use the front stencil state; texture sign modes are 1 = signed, 2 =
  biased; vertex shader textures get their own slot, `g_VsSlots`). The festival's brightness percentiles now equal
  the emulated ones. Details in docs/native-renderer-status.md.
- RenderDoc works on the native renderer too (`--fh1_native_renderdoc=true`, started through RenderDoc, trigger file
  `capture_now`): with a capture of each renderer at the same second, `tools/rdc_tex_stats.py` gives the numbers of
  every texture a pass reads and writes. Compare pass by pass before guessing: it found three faults in one day
  where leaving shaders out found none.
- The shader library needs the vertex shaders the game uploads but the disc's shader files lack
  (`build_logs\shaders\synth*`, made by `--fh1_dump_ring_shaders` + `tools/fh1_synth_containers.py`): copy them into
  `containers` before packing. A draw traced as `VS n-1 ... no registration` is such a shader.
- Editing anything in `shaders\XenosRecomp` makes the next app build relink rexglue.exe and run the code generator
  (about 14 minutes instead of 5), and changes every shader's HLSL (re-translation: about 6 minutes; both can run
  at the same time).
- The game remembers the camera view between runs, and the unattended tests change it (`tools\view_capture.ps1`
  presses RB): check the view of a shot before comparing two runs, and tell the user their camera may have changed.

- 2026-10-05, fifth session: the design creator (booth, wheels, paint color, dialog text) is fixed except for the
  car's black sides and tyres in the booth (vertex constants c37-c39, the ambient light the game computes, are 0 on
  the native renderer: docs/native-renderer-status.md, item 0). Details there, "fifth session".
- A draw inside a visibility query that kills its pixels (PA_SC_VIZ_QUERY bits 0 and 7) paints nothing on the
  console: the emulated GPU drops it, and so does the native renderer now (the wheels' magenta boxes).
- A fill through the depth buffer (the green car's trick) is also how Direct3D clears the tile-aligned part of a
  4x target: it must reach the color target before a resolve as well as before a draw, only inside its own
  rectangle, and its bytes are known from the rectangle's Z and stencil reference.
- A resolve can target a rectangle anywhere inside a larger texture: the destination address is then a tile in the
  middle of it (the livery's sides), not only a row of tiles further down (the scene's strips).
- Inherited speed-ups can be faults in FH1: `fh1_shadows_without_vegetation` dropped every colorless draw with an
  alpha test (stencil masks of dialogs, tree shadows). It is off. Suspect the Carbon / Most Wanted special cases
  first when a draw is missing.
- The shader translator had three faults in cube lookups and loops (nested loops shared `aL`, `cube()` kept two
  directions, the cube instruction's operand order was ignored for 240 of 940 lookups). When a shader's output is
  wrong and its textures and constants are right, read its HLSL for loops and for the helpers of shader_common.h.
- When a draw is wrong and its textures match, compare its constants on both renderers (tools/rdc_constants.py
  against --fh1_native_diag_constants_ps / _vs) before reading any shader: on 2026-10-05 a lighting cube was
  blamed four times before the constants showed three vertex constants at 0.
- The first run with a new option or a new shader library can freeze on a loading screen (a frame of more than
  about 3.2 s while pipelines compile): run it again before believing it.

- 2026-10-05, sixth session: the paint booth is right (ambient light, tyres, rims). The game reads a 256x128
  sphere map of 16-bit floats back on the CPU to compute a menu scene's ambient light: such resolves now reach
  guest memory. Tyres, rims and car parts read their morph shapes from vertex streams outside the declaration
  (fetch constants 29-31, 457 shaders): the renderer uploads them, and the translator keeps a full fetch's index
  for its mini fetches (the full fetch may overwrite the register it was indexed by).
- When a value the game computes on the CPU is wrong on one renderer (a constant that is 0), do not compare GPU
  inputs for long: dump the game's memory on both (`--fh1_dump_memory`), find the good numbers, and trap the write
  (`--fh1_trap_writes_to`); the crash report names the function, and its code shows what it reads.
- `std::search` between a `uint8_t` buffer and a `std::string` needle never matches a byte above 7F (char is
  signed): use a `std::vector<uint8_t>` needle.
- Python patch scripts go in a file made with the Write tool: a bash heredoc turned a backslash-n inside a C++
  string into a real line break again on 2026-10-05, and a quote in the text ended another heredoc early. A
  script that rewrites a document must keep its line endings (ROADMAP.md is LF; check `git diff --stat`).
- Other repos next to this one (`..\repos\`): `nfsc-recomp-main` (GoatHonks' Carbon PC port, the source of the
  native renderer) and, since 2026-10-05, `nfsc-nx-main` (his Switch port of it: toolchain file, Mesa / NVK patch,
  Switch documents): the reference for our first Switch build. The user also named
  https://github.com/arcanite24/pinyon-shift as a source for later (trainer, fps unlock, fixes): ROADMAP.md,
  "Other projects to borrow from".
- User, 2026-10-05: an offline shader library for the PC and for the Switch is on the roadmap (no shader or
  pipeline building while playing; the Switch cannot do it at all).

Local folder layout (2026-10-04): `build_logs\reference\` = the screenshots, traces and logs the documents
refer to; `build_logs\archive-2026-10-04\` = every earlier test, run and build log (nothing there is needed
to work; its RenderDoc captures and the old shader sets were removed on 2026-10-04 with the user's yes). New test output lands in `build_logs\`.

- 2026-10-06, seventh session: the photos of a saved car are right on the native renderer (a picture resolved
  once is written to guest memory, `fh1_native_read_one_off`; the large ones at once, the small ones a frame
  later, `fh1_native_read_one_off_wait_texels`: a measured limit, see the status document).
- Before fixing a native-only fault in something the game reads back, look for the same fault in the emulated
  GPU's history (its log lines and SDK comments): the photos had been fixed there five days earlier.
- The native renderer now creates its targets and draws, and builds the pipeline list, before the game's code
  starts (`Fh1App::LaunchModule`, "Preparing shaders" screen): with the unattended test on a fresh PC every
  second of the route moves by that wait (about 20 s; the boot keys then go at 56-61 s instead of 33-38 s).
- 2026-10-06, eighth session: the offline pipeline list for the PC, steps 1 to 3 of 5 (the status document's
  "Offline pipeline list" has the plan, the numbers and what is left). The list ships as
  `fh1/data/fh1_pipelines.nfpl` and grows by merging what the user plays (`tools/fh1_pipelines.py merge`); an
  unknown pipeline is compiled off the ring (`fh1_native_pipelines_background`).
- A "fresh install" test must set the graphics driver's own cache aside too (`tools/fresh_pc_test.ps1`): with only
  our cache file moved away, 401 "new" pipelines took 0.6 ms each and the run proved nothing.
- Before saying the game is closed, print an explicit word for both cases: an empty `Get-Process` line was read as
  "closed" once while the user was playing (a guard in the build command caught it).
- The unattended route depends on where the user's save starts: after the user plays, look at a shot before
  comparing pictures (on 2026-10-06 the save moved from the festival at night to a highway by day).
- The first pre-release is a Windows GUI installer with a pre-built fh1.exe (the user's decision, 2026-10-06), not
  source-only: ROADMAP.md, "Maybe later".
- 2026-10-06, ninth session: the pre-release plan is agreed (status document, "The plan the user agreed to");
  the mouse pointer hides (`--fh1_hide_cursor`) and F3 opens GoatHonks' monitor on the native renderer
  (`fh1/src/fh1_perf_overlay.cpp`). Adding a source file to fh1/CMakeLists.txt gave the long build (rexglue.exe
  relinked, code generator run: 14 minutes).
- A game option given twice on the command line makes fh1.exe drop all of them ("--game_data_root was not
  provided"): never repeat in `auto_test.ps1 -ExtraArgs` what the script already passes, and `-ExtraArgs ""`
  fails too (use `--fh1_renderer=xenos` for a plain emulated run).
- The installer (`installer/`, C# 5 for the compiler inside Windows, `installer\build_installer.ps1`): steps 1
  and 2 done (window, ISO and folder checks, disc copy into `<folder>\game`). fh1.exe for it: the `game` folder
  next to the exe is the default game folder, `--fh1_unpack_image=<file>` writes default.xex's image and exits,
  and the binaries hold no path of the build PC (`-ffile-prefix-map` in fh1/CMakeLists.txt: source names in logs
  now start at `fh1-recomp/`). Test folder: `%USERPROFILE%\Desktop\FH1-install-test` (+ `-saves` next to it for
  `--user_data_root`); never test an installed game on the user's real saves.
  Steps 3 and 4 too: `installer\make_package.ps1` gathers `installer\out\package` (port + shader tools; it warns
  when a file names this PC's user folder), Install copies it and builds the library (`ShaderLibrary.cs`, the
  C# form of the Python scripts). `FH1Installer.exe --install ISO FOLDER` does all of it without the window.
  A zip's entry count has 16 bits: `media\tracks\colorado\bin.zip` holds 230,057 files (the count says 33,449),
  so a zip reader must walk the whole central directory.
  Step 5: a shader the library does not know is made on the PC while the game runs
  (`fh1/src/native/fh1_extra_shaders.cpp`: container from the microcode, `fh1_hlsl.exe`, `dxc.exe`, saved in
  `shaders_extra` next to fh1.exe; log lines `C5c`). It needs the tools folder: `tools` next to fh1.exe (the
  installer's layout) or `--fh1_native_shader_tools=<repo>\installer\out\package\tools` on the developer's
  build, which has none. It also found a vertex shader the developer's own library lacks (v_B6AA15E7F187FBAF).
  `fh1\out\win-release\shaders_extra.test-20261006` is that test's output (game-derived: never commit). When several test windows open, say for each one
  whose saves it uses (the user took a picture test on the real save for the empty-saves test).
- **The native renderer is the default since 2026-10-06** (the user's decision for the pre-release):
  `fh1_renderer` defaults to `native`, and fh1.exe falls back to the emulated GPU when `fh1_shaders.nfsp` is
  not next to it. The emulated GPU is now `--fh1_renderer=xenos` (Vulkan: add `--gpu_backend=vulkan`): give
  it to `auto_test.ps1 -ExtraArgs` for every emulated reference shot (a plain run is native now). Older notes
  in this file that call the emulated GPU "the default" are from before that day.
- Icons (2026-10-06): `tools/fh1_make_icon.py` draws the repository's icons; with `--image <picture>` it writes
  `fh1/res/fh1_local.ico` and `installer/fh1_installer_local.ico` (ignored by git), which the builds prefer
  (fh1/CMakeLists.txt fills `res/fh1.rc.in`; `installer\build_installer.ps1`). The user's picture is
  `FH1-recomp\icon-source.png`; its use is the user's settled decision. Any change to fh1/CMakeLists.txt gives
  the long build (rexglue.exe relinked, code generator run: about 14 minutes).
- The user's two run logs of one test can be two runs (the save, then a second start to look at the result):
  check which log holds the event (`[save] ... flushed`) before reading the newest one.
