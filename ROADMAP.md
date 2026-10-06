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
| Emulated Xbox 360 GPU, D3D12 | `run_fh1.bat --fh1_renderer=xenos` | correct, 30 fps; F3 frame monitor |
| Emulated Xbox 360 GPU, Vulkan | `run_fh1.bat --fh1_renderer=xenos --gpu_backend=vulkan` | correct, 28-30 fps |
| **Native renderer** (Vulkan, in progress; **the default since 2026-10-06**) | `run_fh1.bat` | boot, videos, menus, loading screens correct; festival in daylight close to the emulated picture at 30 fps (animated crowd, soft edges, right exposure); evening car color and the glow of lights fixed 2026-10-05; the slightly dark picture, 4x MSAA and the driving check still to do |

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
  - [x] Design creator: booth without shading (a vertex shader uploaded at run time, added to the library) — 2026-10-05
  - [x] Design creator: magenta boxes on the wheels (draws inside a pixel-killing visibility query are not drawn,
        as on the emulated GPU) — 2026-10-05
  - [x] Design creator: cyan car body (livery pieces resolved into a rectangle of the 2048x2048 texture; a fill
        through the depth buffer reaches its target before a resolve, with the bytes of its own Z and stencil)
        — 2026-10-05
  - [x] Empty "Leaving paint shop" dialog (`fh1_shadows_without_vegetation` off: it dropped stencil masks; trees
        cast shadows again) — 2026-10-05
  - [x] Shader translator: loops inside loops, cube lookups in a loop, cube lookups with another operand order
        (240 of 940 were looked up in permuted directions) — 2026-10-05
  - [x] Design creator / paint booth: black sides, no gloss, no badge: the game computes the ambient light on the
        CPU from a 256x128 sphere map of 16-bit floats it resolves; such resolves now reach guest memory
        (`fh1_native_read_resolved_half_texels`) — 2026-10-05, sixth session
  - [x] Tyres in the booth (flat black, then too high inside the rim): vertex streams a shader reads without
        declaring them (morph shapes of tyres, rims and car parts, damage grid; 457 shaders) are now uploaded
        (`fh1_native_raw_fetches`), and the translator keeps a fetch's index for the fetches that follow it
        (502 shaders re-translated) — 2026-10-05, sixth session
  - [ ] Car damage on the native renderer: not looked at (it uses the same undeclared streams: check a crash)
  - [x] Photos of a car after saving a paint job (they showed another car's picture): a picture the game
        resolves once is written to guest memory like on the emulated GPU (`fh1_native_read_one_off`); checked by
        the user with three paint jobs on two cars - 2026-10-06, seventh session
  - [ ] The first 25 s at the festival have a few late frames since that fix (28.4-29.6 fps, then 30): pictures
        that come back every few seconds are waited for until the renderer has seen them return twice
  - [ ] Night colors at the festival: the native picture is warm grey where the emulated one is blue (340 s mean
        color 69 55 49 against 63 60 56, brightness equal): the night color grading; next thing to look at
  - [ ] Races, garage, car photos on the native renderer (the user drives there)
  - [ ] Frame time while driving (185-230 ms frames were seen; render-target copies rejected)
  - [ ] Remove the Carbon / Most Wanted special cases left in the draw code, one at a time (user's item 5; the
        first one found by a fault: `fh1_shadows_without_vegetation`)
  - [x] Carbon's fps counter and frame time viewer on F3 for the native renderer (user's item 6): GoatHonks'
        monitor (`fh1/src/fh1_perf_overlay.cpp`; graph from 15 to 30 FPS; `--fh1_perf_overlay=true` opens it at
        the start); 30.0 fps with it open. Also on the emulated GPU's Vulkan backend (30.0 fps); the emulated
        Direct3D 12 keeps the SDK's monitor on F3 (user: his monitor cost it 4 fps, 25.6 against 30) - 2026-10-06
  - [x] The Windows mouse pointer hides over the game after a second without moving (`--fh1_hide_cursor`, both
        renderers; the user checks it by hand) - 2026-10-06
  - [ ] 13 shaders still on an old translation (loop constants i0 / i16 not declared: DXC rejects the new HLSL)
  - [ ] The game stops sending commands after one frame of about 3.2 s (new pipelines compiled in the ring: seen
        again on 2026-10-05 with a new option's first run)
  - [ ] **Offline shader library for the PC (user, 2026-10-05)**: the game must not stutter while it prepares shaders
        as it runs. Today the shaders themselves are already translated offline (`fh1_shaders.nfsp`); what is
        still built while playing is the pipeline of each shader pair with its render state (about 500 known ones
        are built during the logo videos, the rest when first drawn: that is the stutter, and one such frame of
        3.2 s freezes the game). To do: record every pipeline a play-through needs (festival, roads, day and
        night, races, garage, paint shop, menus) into a list shipped next to the library, build all of them before
        the title screen with a progress display, and keep the driver's pipeline cache on disk between runs
    - [x] A list shipped with the port (`fh1/data/fh1_pipelines.nfpl`, 796 pipelines; `tools/fh1_pipelines.py`):
          a fresh PC builds it in 22.5 s during the logo videos and then meets 0 new ones on the recorded route
    - [x] A pipeline no list knows is compiled off the ring and its draw waits (scene, shadows, reflection only)
    - [x] The list is built before the game's code starts, with a "Preparing shaders n / total" screen when it
          lasts more than half a second (20.3 s on a fresh PC, 0.3 s and no screen otherwise)
    - [ ] The full list: the user's tour (every area by day and night, each kind of race, garage, shops, map)
    - [ ] Hand check on a fresh cache (`tools/fresh_pc_test.ps1` sets our file and the AMD driver's cache aside)
  - [x] **User, 2026-10-06, end of the twelfth session: fine now** (with the crowd of the festival; no capture was
        made, so which fix did it is not known). The item as it was written: the ground is not drawn at the Horizon Outpost of Montano
        Plains** (native renderer, at night, step 1 build of the shipped pipeline list, so before the background
        compiler existed): the road and the gravel in front of the stage are there, the ground under the tents and
        the stage is a dark hole. Screenshots: `build_logs\reference\user-outpost-ground-missing-20261006-night.webp`
        and `-map.webp` (where it is). **A second spot the same session**, at dusk, on the road just east of that
        outpost (Montano Plains, towards Clear Springs): a large flat dark grey patch where the ground beside the
        road should be, with sharp straight edges (`user-ground-missing-2-20261006-dusk.webp`, `-map.webp`). Two
        spots close together, day and night: one cause is likely (a ground material of that area). Not looked at:
        check the same spots on the emulated GPU first
  - [x] **Characters with a skeleton were not drawn** (the animal and the presenter of the opening, the driver,
        the festival's people; cause 317, every build): fixed 2026-10-06, eleventh session (status document,
        "After pre-release 1"). The user checks them in the game
  - [x] **Fixed 2026-10-06, eleventh session: it only happened on a first run of an installed copy** (the
        renderer kept "no shader" for the whole session when a shader was still being made on the PC; it now asks
        again once the shader is there: status document, "After pre-release 1"). The item as it was written: some ground textures are not
        drawn (native renderer). Screenshot `build_logs\reference\user-ground-flat-newgame-20261006-native.webp`:
        the new game's first drive, 1.7 mi from the festival, by day; the strip between the road and the leaves
        is one flat brown color with straight edges, and the ground further right has its texture. The same
        stretch on the emulated GPU has gravel and grass there
        (`ground-newgame-1.7mi-20261006-emulated.png`, a few metres further on). Likely the same fault as the
        Montano Plains item above (a flat patch with straight edges beside the road). **It can be reached without
        the user**: the new-game route with empty saves (Start / A every few seconds, the drive from about 140 s)
        passes there, so both renderers can be captured with RenderDoc at that spot. The screenshot is
        from a copy installed from the release (log: `user-ground-flat-newgame-20261006-native.log`, 30 shaders
        made on the PC, none failed, 206 draws rejected with cause 317): check first whether the developer's
        build shows it too
  - [x] **User, 2026-10-06, end of the twelfth session: fine now.** The item as it was written: textures do not load in races** (native renderer, same
        session and build as the item above). Screenshot `build_logs\reference\user-race-scenery-wrong-20261006.webp`
        (a mountain road race by day, red Mustang, 46 % progress): the hillside at the left is dark and almost
        without texture, and a slab of forest hangs in the sky above the road, upside down and stretched, as if
        scenery were drawn with the wrong position or a wrong texture. The road, the car, the crowd, the signs and
        the far hills are right. Ask the user whether it stays or flickers. May be the same fault as the missing
        ground. Second screenshot, same race at 82 % (`user-race-scenery-wrong-2-20261006.webp`): a patch of the
        hillside behind the crowd shows snowy mountains, sky and trees upside down. A clue (a guess, not checked):
        an upside-down scene is what the game's mirrored reflection picture looks like, so these surfaces may be
        drawn with the reflection texture (or an old resolved picture at the same address) in place of their own;
        the forest in the sky of the first screenshot is upside down too
  - [ ] **After the first pre-release (user, 2026-10-06, installed copy): a car's thumbnail picture is sometimes
        wrong and sometimes right** (native renderer; the user's guess: timing). The photos of a saved car were
        fixed that morning with a measured wait (`fh1_native_read_one_off_wait_texels`): start there
  - [ ] **After the first pre-release (user, 2026-10-06, installed copy): the car's dashboard never lights up**
        (native renderer; two runs, the second with every shader read back, 0 draws rejected, so not the shader
        or pipeline preparing). Not looked at. A lead, not checked: one log line per run "texture format not
        supported yet: an empty one is used (cause 422)". Ask first whether it lights up on the emulated GPU
  - [ ] After the first pre-release (user, 2026-10-06): with a shader the game makes on the PC for the first time,
        the object's texture takes a long time to show (the release texts warn that the first run is rough);
        and the "Preparing shaders" text does not show on the two emulated launchers (the user's impression)
  - [x] Shaders the library lacks are made several at once (up to four), and the list's records that waited
        for one go to the helper threads when it is taken in (2026-10-06, twelfth session; status document,
        "Twelfth session": mean wait 1.5 s -> 0.8 s, longest 6.6 s -> 3.6 s, no frame above 0.4 s). Not in a
        release yet
  - [x] The shipped pipeline list: 1067 (the user's 38-minute session of 2026-10-06 added 180)
  - [x] The developer's build makes the shaders its library lacks (`run_fh1.bat` passes the installer's tools
        folder): flat ground by the lake on the first drive, never on an installed copy
  - [ ] **A black window for a whole run, rarely** (2 of 319 starts on 2026-10-05 / 06): the SDK's presenter goes
        to "paint mode -> none" in the first seconds and never paints again while the game runs (status
        document, "Twelfth session", item 6). Not looked at beyond the log line
  - [ ] User, 2026-10-06 (developer's build, second drive): the Volkswagen's dashboard is dark with dim dials
        while the Subaru's is lit (so not every dashboard: add to the dashboard item above); the pink triangles
        on the tyre icons of the upgrade menu (check against the emulated picture first)
  - [ ] User, 2026-10-06 (opening cutscene of a new game, frame 9255 of the first 60 fps test): a sharp
        rectangle of ground under and around the car, with a texture unlike the blurred road next to it
        (`build_logs\reference\user-rectangle-under-car-20261006.webp`). Saved for later; not checked against
        the emulated picture or with the 60 fps setting off
  - [x] The native renderer is the default (user, 2026-10-06, for the first pre-release, with its known faults
        listed in docs/install.md); the emulated GPU is `--fh1_renderer=xenos`

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
- 60 fps unlock with vsync off (2026-10-01, emulated GPU, guest refresh at 1 kHz): broke distant rendering.
  Replaced on 2026-10-06 by `--fh1_fps60` (a steady 120 Hz guest refresh, "Maybe later")
- Forcing 1x MSAA on the emulated GPU: surfaces overlap (glowing garbage on the car)
- Skipping the MSAA-switch transfers: shadows vanish
- Editing the game's data files: the game verifies them (change settings in memory only)
- Scripted autoplay with route replay: driving tests are done by hand
- Paused experiments on the emulated GPU: `docs/history/native-render-targets.md`

## Later — Nintendo Switch (separate repository)

Started from this repo once PC is done (model: GoatHonks' nfsc-nx, see "Other projects to borrow from"). The Tegra X1 has ~20-50x less GPU and much slower CPU
cores than the Legion Go, so it needs the native renderer, the pre-translated shaders, no CPU
spinning, audio decoding on a worker, and probably lower handheld settings. What costs time on PC
today: `docs/performance-review.md`. The Switch platform itself: `docs/nfsmw-nx/`.

- [ ] Build with devkitA64 and the Horizon layer in `sdk/`
- [ ] Memory map, threads, audio out
- [ ] Native renderer and shader library on the console
- [ ] **Offline shader library for the Switch (user, 2026-10-05)**: the console cannot compile shaders and
      pipelines while playing the way the PC does, so everything must be ready beforehand: the translated
      shaders in the form its GPU driver takes, and the full pipeline list of the PC item above built at
      install time or at first boot, never during play
- [ ] Performance on the console
- No 60 fps on the Switch (user, 2026-10-06): `--fh1_fps60` is for the PC version only; the Switch port keeps
  the game's 30

## Other projects to borrow from

- nfsc-recomp (GoatHonks; his repository is private, so no link here; local copy `..\repos\nfsc-recomp-main`):
  the native renderer FH1's started from
- nfsc-nx (GoatHonks; his repository is private, so no link here; local copy `..\repos\nfsc-nx-main`), added by the
  user on 2026-10-05: the Switch side of his Carbon port (devkitA64 / libnx toolchain file, the Mesa / NVK
  driver patch and build notes, the Switch documents; it takes the PC port in as a git submodule). **The
  reference for our first Switch build**: read its README, ROADMAP and `docs/` before starting ours
- pinyon-shift (https://github.com/arcanite24/pinyon-shift), added by the user on 2026-10-05: a source for
  things we may want later, such as its trainer, its fps unlock and some of its fixes. Not looked at yet: read
  it before starting the 60 fps unlock or any cheat / trainer feature

## Maybe later

- [x] **First pre-release as a Windows installer app (user, 2026-10-06; replaces "source-only")** - **published
      on 2026-10-06 as `v0.1.0-pre1`** (https://github.com/AndryTheBeast/fh1-recomp/releases/tag/v0.1.0-pre1;
      the repository went public that day): a simple GUI
      like StevensND's installer page for nfsmw-nx: downloads the pre-built fh1.exe (the user's decision), the
      user chooses their ISO and a folder, it extracts the disc and builds the shader library there. To solve:
      the synth vertex shaders are not on the disc, unsigned programs and Smart App Control, install guide, known
      issues, which renderer is the default, a tag
      **Plan agreed by the user on 2026-10-06** (details: docs/native-renderer-status.md, "The first
      pre-release"): a C# Windows Forms app `FH1Installer.exe`; it downloads `fh1-win64.zip` (fh1.exe, its DLLs,
      the pipeline list: 133 MB, 38 MB zipped) and `fh1-shader-tools.zip` (translator, unpacker, packer, DXC:
      11 MB zipped), extracts the ISO, builds the shader library (about 10 minutes); the emulated GPU is the
      default and the native renderer a second launcher; unsigned (the guide says Smart App Control must be
      off); tag `v0.1.0-pre1`
      **Changed by the user later that day: the native renderer is the default** (`fh1_renderer` defaults to
      `native`; FH1.exe without its shader library falls back to the emulated GPU), and the two .bat files
      start the emulated Direct3D 12 (`--fh1_renderer=xenos`) and the emulated Vulkan
      (`--fh1_renderer=xenos --gpu_backend=vulkan`)
    - [x] fh1.exe: an "unpack only" mode that writes default.xex's image for the installer
          (`--fh1_unpack_image=<file>`, 1 s; the same 478 shaders as the image dumped from the running game; the
          window shows for that second) - 2026-10-06
    - [x] fh1.exe finds the disc's files in the folder `game` next to itself when `--game_data_root` is not
          given (a double click works in an installed folder) - 2026-10-06
    - [x] No path of the build PC in fh1.exe and its DLLs (`-ffile-prefix-map`, `/pdbaltpath`) - 2026-10-06
    - [x] fh1.exe: the vertex shaders Direct3D rewrites at run time (the 42 of `build_logs\shaders\synth*`) are
          translated and compiled on the user's PC when first uploaded, and saved (`fh1_extra_shaders.cpp`,
          `shaders_extra` next to fh1.exe, tools in `tools` or `--fh1_native_shader_tools`). With the
          installer's disc-only library: 29 shaders made in the first 100 s at the festival (0.2-0.9 s each,
          30 fps kept, containers byte-identical to the Python tool's), read back at the second start, picture
          equal to the own library's (59 s: 8/29/89/168/246 on both) - 2026-10-06. Not tried yet: first-person
          view and the paint booth with such a library (the user's check)
    - [ ] A window title without the SDK's build name ("fh1 [rexglue-v0.10.0.0-dev...]") (the VC++ runtime check
          is in the installer, step 6); only if the user asks
    - [x] The installer, step 1: the window, the ISO check (title ID and version from the disc's default.xex) and
          the folder check (`installer/`, built by `installer\build_installer.ps1`, 19 KB) - 2026-10-06; the
          user checks it by hand
    - [x] The installer, step 2: Install copies the disc's files into `<folder>\game` with a progress bar and
          Cancel (a cancelled copy goes on where it stopped; `fh1_install.txt` marks the folder). The user's ISO:
          2432 files, 7.32 GB in 11 s, every file identical to game_root (SHA-1) - 2026-10-06
    - [x] The installer, steps 3 and 4: Install also puts the port and the shader tools into the folder (from
          the folder `package` next to the installer for now, made by `installer\make_package.ps1`: 151 MB) and
          builds the shader library there (`installer/ShaderLibrary.cs`): 3,849 shaders found, 3,816 compiled,
          the same as the disc part of the user's own library; a fresh installation takes 194 s on the Legion Go
          (11 s disc copy), a second run on the same folder nothing - 2026-10-06; the user tries the window
    - [x] The installer, step 6 (`installer/Setup.cs`): "Needed on this PC" (Visual C++ runtime with a button
          that fetches and starts Microsoft's own installer, Direct3D 12, Vulkan); the two zips are downloaded
          from the release when no `package` folder is next to the installer (`--source` for tests); the
          program is installed as `FH1.exe` with a desktop shortcut and two .bat files (emulated Vulkan, native
          Vulkan); a folder that holds an installation shows "Update"; a standing note and a warning that only
          the USA disc works (media ID 2DC7007B). The installer alone, zips from a local folder: 200 s -
          2026-10-06. Not tried: a real download from GitHub (no release yet), the runtime button on a PC
          without the runtime
    - [x] `installer\make_package.ps1` also makes `installer\out\release` (fh1-win64.zip 38.1 MB,
          fh1-shader-tools.zip 10.7 MB, FH1Installer.exe 47 KB, SHA256SUMS.txt); nothing is uploaded
    - [x] Launchers (user, 2026-10-06; done the same day, see step 6): the main FH1.exe runs the emulated Direct3D 12 and gets a desktop
          shortcut; two .bat files in the game folder, one for the emulated Vulkan (`--gpu_backend=vulkan`) and
          one for the native renderer (`--fh1_renderer=native`). For that fh1.exe must find the game's files in
          a folder next to itself when `--game_data_root` is not given (today: an error box)
    - [x] Install guide and known issues (`docs/install.md`), release text
          (`docs/release-notes-v0.1.0-pre1.md`), README section, issue form asking for the log
          (`.github/ISSUE_TEMPLATE/bug_report.yml`, a form with required fields since the repository went public), `Read me.txt` written into the game's folder - 2026-10-06
    - [x] Logging in the pre-releases (user, 2026-10-06): on by default (`logs\fh1_NNN.log` next to FH1.exe,
          info level, the newest 20); the crash report now goes to `logs\fh1.crash.txt` too - 2026-10-06
    - [x] An installed copy uses the shipped pipeline list (it skipped all 813 records: shaders were named by
          their numbers in the developer's library; now found by fingerprint) - 2026-10-06, the user's doubt
    - [x] Shaders made on the PC carry a fingerprint (it was 0: their pipeline records never matched again, 133
          shipped records skipped for good, about 39 pipelines rebuilt at every start) - 2026-10-06
    - [x] A pipeline no list knows is compiled at once, with a stutter, instead of the object showing late
          (user, 2026-10-06; `fh1_native_pipelines_ring_ms`, the helper threads only above 1 s in 3 s). Fresh
          PC test of an installed copy: 19 at once, none deferred, longest frame 1.5 s
    - [x] The installer says in plain words when a download fails; the issue form is a guided form
          (`.github/ISSUE_TEMPLATE/bug_report.yml`) - 2026-10-06
    - [ ] After the pre-release: make the shaders that are not on the disc several at a time (one worker thread
          today, about 1 s each: the last cause of objects showing late in a first run); merge the user's played
          cache into the shipped list (`tools/fh1_pipelines.py merge`) for the next release
    - [x] Icons for FH1.exe and the installer (`tools/fh1_make_icon.py`): the repository holds a drawn one
          (`fh1/res/fh1.ico`, `installer/fh1_installer.ico`); the user's builds use the user's own picture
          (`--image`, written as `*_local.ico`, ignored by git; the user's decision) - 2026-10-06
    - [ ] The user's checks: the new installer window (Update on `Downloads\FH1`), first-person view and the
          paint booth on the native launcher of that installed copy (its library has no run-time shaders)
    - [ ] The release itself (tag `v0.1.0-pre1`, the four files of `installer\out\release`): only with the
          user's yes at that moment. After it: one real download test with the installer alone
- [ ] (old wording) Source-only pre-release (tag + install guide + known issues; users build from their own disc). **User,
      2026-10-06: this is the focus now**, after the offline shader library for the PC; the other native renderer
      fixes (night colors, races / garage / damage, Carbon clean-up, F3 viewer) wait
- [ ] **60 fps while driving: `--fh1_fps60`, off by default (2026-10-06; the user decides whether it stays)**.
      The game shows each picture of the world for two refreshes and steps its simulation once per two: the
      option runs the refresh the game counts at 120 Hz (pinyon-shift's method, credited in
      THIRD_PARTY_NOTICES.md). Legion Go, new game: cutscene 50-58 fps, driving 32-40 (the graphics chip needs
      about 20 ms a frame; 60 needs 16.7), game speed 1.000, picture and far scenery right. Details: status
      document, "Thirteenth session"
    - [ ] The user's drive with it: speedometer / race timer against a stopwatch, crowd, people, traffic,
          particles, a car purchase, the HUD, night
    - [ ] Reaching 60 on the Legion Go needs a faster frame: the scene's copies (12-16 ms of the 20 with the
          double-size scene) first, then the processor side (40-50 fps with `--fh1_native_ssaa=false`, which
          the user finds too rough to use)
    - [ ] **Pre-release 0.1.1 with the option in the installer (user, 2026-10-06): started, not built or
          published.** The installer has the checkbox (it writes `fh1_fps60 = true` into `fh1.toml`) and
          version `0.1.1-pre1`; left: package, test on the installed test copy, release texts, the upload
          with the user's yes (docs/next-session-prompt.md has the list)
    - User, 2026-10-06: the PC version will have the 60 fps patch; the Switch port will not
