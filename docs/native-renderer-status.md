# Native renderer: where it stands and what to fix next

Read this after CLAUDE.md. It is the starting point for the next session. State as of 2026-10-04 (night).

The native renderer is FH1's own (`fh1/src/native/fh1_*`, run with `--fh1_renderer=native`). It started as
GoatHonks' nfsc-recomp renderer; his later fixes are ported by hand when they work for FH1
(`fh1/src/native/README.md` has the table). The emulated Xbox 360 GPU (default, `run_fh1.bat`) is the reference
for the correct picture.

## What works (checked against the emulated GPU)

- Boot: trademark screen, logo videos, intro, title screen with its video background.
- Menus and loading screens: text, the boxes behind it, artwork, map screen (user, 2026-10-04: "looks fine to me
  now").
- Festival in daylight, car standing still: sharp scene, shadows, crowd, trees on the hills, 30 fps. Compare
  `build_logs\reference\test-a2m-*-53s.png` (native) with `test-festX-*-53s.png` (emulated).
- User, 2026-10-04 night: the random giant polygon flashes seen before are gone (not targeted by a specific fix;
  most likely the half-covered clears, see below).

## Open problems, in the order to take them

Compare `build_logs\reference\test-festX-*` (emulated, correct) with `test-a2m-*` (native, same seconds).

1. **Driving has not been checked since the fixes of 2026-10-04 night.** The user drives by hand: ask for a short
   drive first. To look at: motion blur while moving (the velocity pass now gets real depth and stencil; the car's
   own stencil value, 21, picks its matrix), frame drops (185-230 ms frames were seen before), anything that
   flashes.
2. **Evening look** (from ~90 s after launch, `test-a2m-*-95s.png` against `test-eveXenos-*-130s.png`): the car
   turns green / chrome, strong white glare and light beams over the scene. Next: compare the car paint shader's
   constants and textures day against evening; check the 2x MSAA reflection cube map (6 faces 256x256, drawn at one
   sample, resolves not scaled) and the glare / light-beam draws (additive, they may need the depth they test).
3. **Brightness and glow in daylight**: the native picture is brighter and more contrasted (sunlit dome, crowd,
   car paint more metallic), and the tail lights have no red glow around them (bloom). Candidates: the bloom chain
   (`frame_*_320x192` dumps), the gamma curve (sRGB is only close to the console's piecewise-linear one), the
   reflection cube map again. Measure with brightness percentiles on the same second of both renderers.
4. **Thin light outlines on edges** (car, tower, stage rigging) and **stair-stepped shadow edges** on the ground.
   The scene is drawn with one sample where the console uses 4x MSAA; the game's FXAA pass (PS n2283) runs. Check
   what the emulated picture does at the same pixels (crop enlarged with NEAREST) before changing anything.
5. **Crowd brighter than the emulated one** and with hard cut-out edges: alpha to mask is a plain "alpha >= 0.5"
   test here (`--fh1_native_alpha_to_mask`); the console dithers coverage over 4 samples.
6. **Computed vertex index outside quad lists** (log cause 317): rejected. Billboards in quad lists work (each
   stored vertex repeated four times); find which draws these are (one-frame trace) and what their index is.
7. Small: two texture formats not supported (causes 422 = k_24_8 not coming from a resolve, 458 = k_DXT3A), 1D
   textures (cause 30), textures with other sign modes than gamma (cause 31), vertex element without a free
   location (cause 22).
8. Performance while driving (about 25,000 render-target copies rejected in a driving run before today; measure
   again).

## How to run and test

- User: `run_fh1.bat --fh1_renderer=native`. Normal play (emulated GPU): `run_fh1.bat`.
- Unattended, to the festival with the user's save (no driving; the user drives manually for driving tests):

      powershell -ExecutionPolicy Bypass -File tools\auto_test.ps1 -Name fest -Seconds 100 -Shots "53,70,95" `
        -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a" `
        -ExtraArgs "--fh1_renderer=native"

  Without `-ExtraArgs` the same run uses the emulated GPU: take both and compare the same seconds. Timeline of
  that run: logo videos 2-26 s, title from ~28 s, menu ~36 s, loading screen ~38-45 s, festival from ~47 s.
  Evening lighting starts ~90 s.
- Build: `tools\build_windows.ps1 -SkipFetch -SkipCodegen` (app only, ~5 min; it still runs the code generator
  when rexglue.exe was relinked). Run it in the background or with a bounded wait; never wait with an open-ended
  loop. Do not edit sources while a build is running (a header change in the middle gives a mixed build).
- Shader library: `fh1_shaders.nfsp` next to fh1.exe (copy of `build_logs\shaders\fh1_shaders.nfsp`, game-derived,
  not in git). After changing the translator (`shaders/XenosRecomp`): `tools\build_shader_tools.ps1`, then
  `python tools\fh1_retranslate_changed.py --apply` (recompiles only the shaders whose HLSL changes), repack and
  copy (steps in that script's header).
- The renderer's own small shaders (not game-derived, committed as SPIR-V headers): `python tools\fh1_make_rect_gs.py`
  (rectangle lists) and `python tools\fh1_make_depth_pack.py` (depth as bytes); both use `..\tools_dxc`.

## Diagnostics that paid off

- `--fh1_dump_resolved_at_s=N`: at second N, every resolved image, render target and depth-as-bytes image
  (`dump_resolved\NN_<address>_<size>.png` next to fh1.exe), and **for the following frame every color image right
  after its copy** (`frame_NN_dest<address>_<size>.png`, with `alpha_` twins): the post-processing chain step by
  step. This found the empty velocity image in minutes. The dumps are upside down.
- `--fh1_native_diag_frame_s=N`: every draw and copy of one frame in the log (`[trace]` lines: shaders, render
  target, blend, each texture with format / signs / exponent). Shader numbers to files:
  `python tools\fh1_shader_name.py 1212` -> `build_logs\shaders\hlsl\<name>.hlsl` (read only the last ~60 lines).
- `--fh1_native_diag_constants_ps=2520 --fh1_native_diag_constants_ms=0`: the pixel constants a shader gets
  (c0-c11, c32-c43, c132-c135).
- `--fh1_debug_no_cull=true`: all face culling off (how the dropped rectangles were found).
- The log's `(cause N)` lines: everything the renderer rejects or replaces, once per kind. `[fh1] rect-list draw`
  lines: the three corners of each kind of rectangle draw.
- Same seconds on both renderers + brightness percentiles of the picture area (a 20-line PIL script) settles
  "is it brighter / darker" questions quickly; pixel-level crops enlarged with NEAREST settle edge questions.
- `--fh1_dump_ring_shaders=DIR` + `tools/fh1_synth_containers.py`: shaders the game uploads that the library lacks.

## FH1's frame, as the native renderer sees it (festival)

1. Shadow maps (depth, 1040 pitch) resolved as depth textures. 2. Depth pre-pass 1280x720, resolved to 1DAC5000.
3. Shadow mask into 1CE2D000 (boxes drawn with the stencil test). 4. Reflection cube map (2x MSAA, 256x256).
5. Main scene: 4x MSAA at 1280 pitch in **three strips** of 256 rows (window offset 0 / -256 / -512), each resolved
into one 1280x720 color texture (1C4E1000) and one depth texture (1DAC5000). 6. Bloom chain (320x192 down to
20x12). 7. Motion-blur velocity 640x360 (PS n2520, then n2368) from the depth texture fetched as k_8_8_8_8. 8. FXAA
(PS n2283). 9. Final composite in two halves (PS n716: blur, depth of field, bloom, color grading). 10. UI.

Direct3D's clears are rectangle lists (VS n1205, PS n3067) on a **4x MSAA surface of half the pitch** of the
target they clear (the same EDRAM, four samples per pixel drawn).

## What was fixed on 2026-10-04, and why (so the same causes are recognised again)

Night session (the 3D scene):

- **Smear over the whole scene, speckled edges**: the motion-blur velocity texture was empty, so every pixel
  "moved" by the maximum. Three causes, all fixed:
  - Rectangle lists were culled like triangles. The console never culls them (`key.rasterization`).
  - A resolved depth fetched as k_8_8_8_8 gave the depth value in every channel; the shaders rebuild the 24-bit
    float depth from three bytes and take the stencil from the fourth. A small pass now writes those bytes into a
    color image after each resolve, when a draw asks (`TargetsVulkan::TextureResolvedBytes`,
    `--fh1_native_depth_bytes`). Depth resolves also copy the stencil.
  - The rectangle geometry shader assumed the corner was the first vertex; FH1's clears send top-left, top-right,
    bottom-right, so they covered half of the target (a diagonal). The corner is now the vertex opposite the
    longest edge.
- **No shadows**: Direct3D's 4x clears went to an image of their own, so the stencil the shadow-mask passes test
  was never reset. 4x passes of 640 pitch or less now draw into the 1x target of twice the pitch at twice the
  scale (`--fh1_msaa_4x_clears_as_1x`). The scene itself (4x at 1280 pitch) stays at one sample;
  `--fh1_msaa_4x_as_1x` (every 4x pass) still turns the picture pink and stays off.
- **Crowd, trees, vegetation missing (cause 316), and the black festival with packed positions on**: those draws
  are quad lists with one stored vertex per quad; the shader fetches vertex `index / 4` (fetch source r0.y, not
  r0.x) and builds the corners from the index. Fetched by the plain index they read past their data and drew
  screen-sized garbage. Each stored vertex is now repeated four times in the vertex copy
  (`EntryVertices::index_computed`); k_10_11_11 positions are on by default. The packed decoding itself was right.
- **Crowd as solid rectangles**: alpha to mask (RB_COLORCONTROL bit 4) was ignored; now an alpha test at 0.5.
- A pass C2 records itself in the work command buffer must call `DrawsVulkan::ForgetStateBound` afterwards, or
  the draws reuse a pipeline and descriptor sets that are no longer bound (black scene).

Day session (UI, videos):

- **Teal boot screen**: the game resolves the trademark screen once and then presents front buffers it never
  resolved into; Present shows the last screen-sized resolved texture then.
- **White videos**: drawn into a k_8_8_8_8_GAMMA target and resolved as plain k_8_8_8_8; the two formats now
  share one image (`FormatTargetCanonical`).
- **Grey tint, part 1**: the game loads the piecewise-linear gamma ramp (DC_LUT_PWL_DATA), not the 256-entry
  table; it is now applied on the output.
- **Grey tint, part 2 (washed-out menus and festival)**: textures fetched with the gamma sign (signs 3F) were
  read raw; they now use the host's sRGB formats (`--fh1_native_gamma_textures`).
- **"Low bitrate" videos (banding in dark areas)**: draws into gamma targets and fetches of resolved pictures
  with the gamma sign go through sRGB views of the same image (`--fh1_native_gamma_targets`,
  `ImageNative::view_srgb`).
- **Not a bug**: the Playground and Turn 10 intros have a background above black in the video files themselves
  (Y = 19-20 and 29; only Dolby is 16). Both renderers show the same.
- **Boxes behind UI text, minimap frame**: the shader fetches a texture register its constant table does not
  list; the renderer now also binds every register a fetch instruction in the microcode uses
  (`fh1_native_shaders.cpp`).
- **Black loading-screen background**: the game's CPU code writes the artwork into memory at an address where it
  had resolved a screen copy; resolves never reach guest memory here, so the stale copy was sampled. Resolved
  textures now carry a fingerprint of the guest memory at their address (`TargetsVulkan::StampMemory`); when it
  changes, the address is read from memory until the next resolve.
- **Jagged text and map roads**: the translator dropped the microcode's getGradients instruction (screen-space
  derivatives, used to antialias curve edges); it now emits `ddx_coarse` / `ddy_coarse`.
- **Vertex index**: vertex shaders start with `r0.x = SV_VertexID` as on the console (needs
  `shaderDrawParameters`).

Left in on purpose: the `REX_PLATFORM_SWITCH` / `__aarch64__` blocks (the Switch port starts from this code) and
the Carbon / Most Wanted special cases inside the draw code (remove one at a time with a festival screenshot
after each).
