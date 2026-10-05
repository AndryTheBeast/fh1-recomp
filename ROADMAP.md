# Roadmap

Forza Horizon (Xbox 360) → PC by static recompilation (ReXGlue), then the Nintendo Switch in a
separate repository.

How to read this file: `[x]` = done and checked, `[ ]` = still to do. One line per item; the full
story of each (addresses, measurements, dead ends) is in `docs/history/roadmap-history.md`, and the
native renderer's details are in `docs/native-renderer-status.md`.

## Where we are (2026-10-04)

**Playable on PC**: boot, festival, free roam, races, garage, buying and repainting cars (photos
correct), saving. Lenovo Legion Go (Ryzen Z1 Extreme): **30 fps** (the game's own cap) in normal
play, 26-28 in the busiest spots.

| graphics path | how to run | state |
| --- | --- | --- |
| Emulated Xbox 360 GPU, D3D12 (default) | `run_fh1.bat` | correct, 30 fps; F3 frame monitor |
| Emulated Xbox 360 GPU, Vulkan | `run_fh1.bat --gpu_backend=vulkan` | correct, 28-30 fps |
| **Native renderer** (Vulkan, in progress) | `run_fh1.bat --fh1_renderer=native` | boot, videos, menus, loading screens correct; festival in daylight close to the emulated picture at 30 fps (animated crowd, soft edges, right exposure); evening car color and the glow of lights fixed 2026-10-05; the slightly dark picture, 4x MSAA and the driving check still to do |

## Stage 1 — Translate the game (done)

- [x] Build the ReXGlue code generator; extract `default.xex` (NTSC-U, 4D5309C9, v0.0.0.10)
- [x] First code generation (16 tail-call targets declared)
- [x] Run-time modules XMediaFacade and SpeechFacade added to the manifest
- [x] Switch at `0x82AD80D0` ended one instruction early
- [x] Function at `0x830ED900` ran past `KeBugCheck` into the next one
- [x] Code gaps: 796 found; data excluded, thunks split, 58 truncated functions extended; generator
      output clean (no unresolved branches, no fatal stubs)
- [x] XDK fibers hooked to host fibers (`SwitchToFiber` cannot work as translated code)
- [x] `setjmp` / `longjmp` declared (libjpeg and Lua-style error handlers)
- [ ] The uninitialized read behind the save-loading crash (upstream of `sub_82A7D730`): only guarded
      today (see Stage 2)
- [ ] `vupkd3d128` / `vpkd3d128` type 6 translate wrongly (FH1 never uses them: 0 sites)

## Stage 2 — Running on Windows with the emulated Xbox 360 GPU ("xenos") (done, playable)

### Crashes and blockers fixed

- [x] No graphics at all: the SDK defaulted to no GPU emulation; `xenos` plugin turned on
- [x] Null read right after pressing Start (fibers, see Stage 1)
- [x] Fatal "dirty disc" error leaving the intro drive: module unload looked up the wrong path, so
      XMediaFacade could not be loaded again
- [x] Crash at the start of the `forza_tone.wmv` intro video: thunk pool of the main module filled up
      (one new thunk per lookup); thunks are now reused
- [x] Crash entering an event: `cache:` (ghost and replay streams) had no device; utility partition
      mounted, emptied at every start
- [x] Crash loading a save (4 of 6 starts): new stacks were filled with Xenia's 0xBE debug pattern;
      stacks are zeroed like the console's, plus a guard on the table index
- [x] Crash entering the paint shop / design creator: libjpeg's error path (`longjmp`)
- [x] Run logs of 90 MB in 15 minutes: repeated errors rate-limited

### Gameplay bugs fixed

- [x] Cars passed through each other, signs and bonus boards did not react: `vmsum3fp128` /
      `vmsum4fp128` must give NaN on float overflow as the console does (collision GJK depends on it)
- [x] Car floating above the road: an effect of 7-9 fps, gone at 30 fps
- [x] PS4 controller seen twice through DS4Windows: controllers merged (`fh1_merge_controllers`)
- [x] Saves kept "online-only" by OneDrive on a new PC: pinned to the device

### Picture bugs fixed (emulated GPU)

- [x] 1-2 fps on a laptop with two GPUs: D3D12 picked the integrated one; high-performance adapter first
- [x] Intel: the slow ROV render-target path was forced (65 s of pipeline compiling, draws ~15x slower);
      host render targets are the default everywhere
- [x] Tear lines: presenter synced to the display
- [x] Missing geometry (car side mirrors): draws with an "invalid" vertex fetch constant were dropped
- [x] "Audio cut" in the intro video: the video was playing slowly; gone with the frame rate
- [x] Garage: car lights bloomed into white streaks (CPU read of a resolve with read-back off;
      `readback_resolve=fast` by default)
- [x] Car Select photos of newly bought or repainted cars were garbage or stale: one-off resolves are
      read synchronously
- [x] Vulkan backend: the 3D world was black (texture fetch exponent bias read from the wrong word)
- [x] Blank rear number plates: not a bug (the console shows none either)
- [x] GPU settings set by the app were silently ignored before the plugin loaded (`SetFlagAppDefault`)

### Picture bugs still open (emulated GPU)

- [ ] Intel GPUs only: dark square in the bottom-right corner and a hard-edged car shadow with host
      render targets (correct on AMD; correct but 1-2 fps with ROV)
- [ ] HUD flashes sometimes: seen at 7-9 fps, not re-checked at 30 fps
- [ ] Soft rectangle under the car (same on RTV and ROV): compare with console footage before calling
      it a bug
- [ ] Busiest race scenes dip to 22-28 fps (draw count; the native renderer is the answer)

### Speed of the emulation (done)

- [x] Frame rate log (`[fps]` lines) with a per-frame breakdown of the GPU thread; F3 frame monitor
- [x] Register writes from the ring in bulk
- [x] Shadow depth buffers drawn into their 1x alias instead of copied at every switch
      (`--gpu_msaa_depth_as_1x`)
- [x] Clears instead of EDRAM ownership transfers where a resolve had just cleared the range
- [x] Only resolves the CPU reads are copied back
- [x] The game's render thread yields instead of spinning
- [x] Vulkan backend of the emulated GPU built on Windows and correct

### Other

- [ ] List the kernel / XAM calls the game makes that ReXGlue lacks (Kinect, Xbox Live, DLC paths)

## Stage 3 — Native renderer (now)

The renderer that draws the game directly with Vulkan, without emulating the Xbox 360 GPU. It is
what the Switch needs. State, open problems, how to test: **`docs/native-renderer-status.md`**.

- [x] **N0 shader library**: 3,849 shaders pre-translated (`fh1_shaders.nfsp`)
- [x] **N1 own graphics system**: the game runs without the emulated GPU
- [x] **N2 shader identity**: the shaders the game uploads are matched to the library (missing ones
      are added from ring dumps)
- [x] **N3 draws and textures: boot, videos, title, menus, loading screens, map** — same picture as
      the emulated GPU
  - [x] Teal trademark screen
  - [x] White videos; banding in dark video areas
  - [x] Grey tint (gamma ramp on the output; gamma textures)
  - [x] Boxes behind UI text and the minimap frame
  - [x] Black loading-screen background
  - [x] Jagged text and map roads
- [ ] **N4 the 3D scene**
  - [x] Smear over the whole scene and speckled edges (motion-blur velocity: rectangles culled,
        depth fetched as bytes, half-covered clears)
  - [x] Random giant polygon flashes (user, 2026-10-04: gone)
  - [x] Shadows in the scene (Direct3D's 4x clears)
  - [x] Crowd, trees and vegetation (billboards fetched with index / 4; packed positions)
  - [x] Crowd cut-outs (alpha to mask)
  - [x] Crowd animations (the moving people are 3D characters whose bones are fetched by a computed index: read
        from memory by the shader)
  - [x] Over-sharp picture: bright outlines on edges (texture exponent bias, used by the game's FXAA), highlights
        cut at white (float resolved textures)
  - [x] Picture a third too bright (vertex-shader textures: the composite's exposure)
  - [x] 1D textures (bound as one-row 2D textures)
  - [ ] Driving check after these fixes (motion blur while moving, frame drops) — the user drives
  - [x] Evening: flat green car (the game clears the shadow / headlight mask through a depth buffer on the same
        EDRAM; the fill now reaches the color target) — 2026-10-05
  - [x] Blue outline on the car, blue rear window and bumper (day and night, next to some verges): the clear
        color of 10-bit and 7e3 float targets was read as four bytes, so the reflection cube map was wiped
        bright blue instead of nearly black and showed it in the gaps of its scene (2026-10-05, seen by the user)
  - [x] Night: the headlights light the road (the shaders now get all 256 boolean constants, not b0-b15 per
        stage: the scenery tests b100) — 2026-10-05, seen by the user's capture at night
  - [x] Map screen: the circle selector (k_DXT3A textures, widened to BC2 blocks) — 2026-10-05
  - [x] Diagnostics on demand at a spot the user drove to: `run_native_capture.bat` / `run_emulated_capture.bat`
        + `tools\capture_now.ps1` (frame dump and trace, or a RenderDoc capture) — 2026-10-05
  - [x] Glow around lights (the red glow of the tail lights): not the bloom but the game's occlusion queries;
        answered with 1000 samples like the emulated GPU (`--fh1_native_occlusion=0`, the default) — 2026-10-05
  - [ ] Occlusion queries measured for real (a glow hidden behind an object; needs the 4x samples and the sum
        over the three scene strips)
  - [ ] Picture slightly darker than the emulated one (median 30 against 40 at evening, 36 against 40 by day)
  - [ ] Light outlines on edges, stair-stepped shadow edges (scene drawn with 1 sample instead of 4)
  - [ ] Crowd brighter than the emulated one, hard cut-out edges
  - [ ] 2x MSAA reflection cube map and its resolves
  - [ ] Computed vertex index outside quad lists (rejected today)
  - [ ] Small formats: k_24_8 not from a resolve, 1D textures, other texture sign modes
- [ ] **N5 the rest of the game and speed**
  - [x] Reflection cube map with its nine levels, level of detail of every fetch, small resolved pictures at their
        real width (soft chrome and paint) — 2026-10-05
  - [x] Textures with the gamma sign use the console's piecewise-linear curve (raw scene now matches the emulated
        one within a few percent) — 2026-10-05
  - [x] Loading screen frozen for good after a new shader library: pipelines prewarmed by several threads (the
        game stops after a frame of ~3.2 s; that time-out itself is not found) — 2026-10-05
  - [x] Smooth edges: the scene's 4x MSAA passes drawn at twice the size and averaged by the resolve
        (`--fh1_native_ssaa`); texture sign modes (biased, signed DXN) — 2026-10-05
  - [x] First-person view (white world, speckled interior, mirrors without scenery): rectangles use the front
        stencil state, texture sign modes 1 and 2 were swapped, eight vertex shaders added to the library —
        2026-10-05 (fourth session)
  - [x] Picture too bright for ~2.5 s after a loading screen and after a view switch: the adapted luminance was
        read as value * 2 - 1 (the same swapped sign modes); the user confirmed — 2026-10-05
  - [x] Picture a little darker than the emulated one: the bloom's bright pass read the scene as its luminance
        (vertex shader textures now get their own slot, `g_VsSlots`); 58 s: 5/20/35/92/129 against 5/19/35/92/129
        — 2026-10-05
  - [x] RenderDoc capture of the native renderer's own frame (`--fh1_native_renderdoc`, `tools\view_capture.ps1`,
        `auto_test.ps1 -Triggers`) and number tools for both renderers (`tools\rdc_tex_stats.py`,
        `rdc_pick.py`, `rdc_draw_textures.py`) — 2026-10-05
  - [x] Crowd cut-out edges: alpha to mask covers 0 to 4 samples with the game's dither offsets
        (`--fh1_native_alpha_to_mask_samples`); not judged up close by the user yet — 2026-10-05
  - [x] Paint shop: the blurred background (depth of field): fetches addressed in texels, the doubled scene depth
        reports its guest size — 2026-10-05
  - [x] Glows through walls: on both renderers, the user takes it for the game's own behavior; measured mode kept
        behind `--fh1_native_occlusion=1` — 2026-10-05
  - [x] Smooth edges seen by the user while driving ("edges look good now") — 2026-10-05
  - [ ] **Design creator (paint shop > Paint car): wheels as magenta boxes, flat car body, booth without shading,
        empty "Leaving paint shop" dialog** (user's pictures, 2026-10-05): first thing next session
  - [ ] Thumbnail wrong again when a car is saved with a new modification (user, 2026-10-05)
  - [ ] Night colors at the festival: the native picture is warm grey where the emulated one is blue (340 s mean
        color 69 55 49 against 63 60 56, brightness equal): the night color grading; next thing to look at
  - [ ] Races, garage, car photos on the native renderer (the user drives there)
  - [ ] Frame time while driving (185-230 ms frames were seen; render-target copies rejected)
  - [ ] Remove the Carbon / Most Wanted special cases left in the draw code, one at a time
  - [ ] Make the native renderer the default once it matches the emulated picture

## Tools (done)

- [x] Disc extraction, Windows build script, fresh-PC setup script
- [x] Gap tools (`gaps.py`, `gaps_pass.py`, `merge_continuations.py`, `gaps_iterate.sh`,
      `check_symbols.py`)
- [x] Crash report with a symbolized stack
- [x] Unattended test runs with screenshots (`tools/auto_test.ps1`), boot timetable for the pad
- [x] Sampling profiler (`--fh1_profile`), render-target frame log, call graph, D3D call census
- [x] RenderDoc capture scripts
- [x] PowerPC instruction test suite runner (`tools/run_ppc_tests.ps1`)
- [x] Shader library builder, partial retranslation, shader-number lookup
- [x] Native renderer diagnostics: one-frame trace, per-copy image dumps, constant logs

## Tried and dropped

- Single-pass drawing inside the EDRAM emulation (one strip instead of three): broke the picture
- 60 fps unlock with vsync off: broke distant rendering
- Forcing 1x MSAA on the emulated GPU: surfaces overlap (glowing garbage on the car)
- Skipping the MSAA-switch transfers: shadows vanish
- Editing the game's data files: the game verifies them (change settings in memory only)
- Scripted autoplay with route replay: driving tests are done by hand
- Paused experiments on the emulated GPU: `docs/history/native-render-targets.md`

## Later — Nintendo Switch (separate repository)

Started from this repo once PC is done. The Tegra X1 has ~20-50x less GPU and much slower CPU
cores than the Legion Go, so it needs the native renderer, the pre-translated shaders, no CPU
spinning, audio decoding on a worker, and probably lower handheld settings. What costs time on PC
today: `docs/performance-review.md`. The Switch platform itself: `docs/nfsmw-nx/`.

- [ ] Build with devkitA64 and the Horizon layer in `sdk/`
- [ ] Memory map, threads, audio out
- [ ] Native renderer and shader library on the console
- [ ] Performance on the console

## Maybe later

- [ ] Source-only pre-release (tag + install guide + known issues; users build from their own disc)
- [ ] 60 fps unlock (needs a much faster renderer, and checking the game's timing at 60)
