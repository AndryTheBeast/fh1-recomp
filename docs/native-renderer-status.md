# Native renderer: where it stands and what to fix next

Read this after CLAUDE.md. It is the starting point for the next session: **the rendering glitches in the
festival and while driving**. State as of 2026-10-04 (evening).

The native renderer is FH1's own (`fh1/src/native/fh1_*`, run with `--fh1_renderer=native`). It started as
GoatHonks' nfsc-recomp renderer; his later fixes are ported by hand when they work for FH1
(`fh1/src/native/README.md` has the table). The emulated Xbox 360 GPU (default, `run_fh1.bat`) is the reference
for the correct picture.

## What works (checked against the emulated GPU)

- Boot: trademark screen, logo videos, intro, title screen with its video background.
- Menus and loading screens: text, the boxes behind it, artwork, map screen (user checked 2026-10-04: "looks
  fine to me now").
- Brightness of videos, menus and loading screens: same levels as the emulated GPU.
- Festival and driving: drawn at 30 fps, playable, **with the glitches below**.

## Open problems, in the order to take them

The user's words (2026-10-04): "then we fix rendering". Compare `build_logs\reference\test-festX-*-53s.png`
(emulated, correct) with any native festival shot.

1. **Smear over the whole scene** (looks like heavy motion blur with the car standing still) and **speckled
   edges** around the car and structures. Not investigated yet beyond this: the scene depth / stencil is filled
   at 640 pitch with 4x MSAA and used by the 1280x720 1x passes; `--fh1_msaa_4x_as_1x=true` (draw the 4x passes
   into the 1x image at twice the scale) turned the picture pink / black on its first try. Start with
   `--fh1_dump_resolved_at_s=N` (every resolved image of one frame) and the one-frame trace, find the first
   image that is already wrong, and work forward from there. The smear is probably the motion-blur / velocity
   input being wrong rather than the blur itself.
2. **Packed (k_10_11_11) positions**: 14 % of all draws are rejected while driving (cause 316): vegetation,
   billboards and more are missing. With `--fh1_vertices_10_11_11_mask=65535` they are drawn but the festival
   turns black (`build_logs\reference\test-vid-packed-*-95s.png`). The vertex index in r0.x is done (library
   built with it); next: one-frame trace with the mask on, take the first draw with a packed position and compare
   its decoded positions with the emulated GPU's (sign / integer modes of the fetch, `remapInput` in
   shader_common.h), and check whether those shaders also fetch from guest memory (`fh1Fetch` returns 0 while
   `g_GuestBase` is 0). Earlier analysis: docs/history/handoff-english-rename.md, "State of the native renderer".
3. **Festival brighter than the emulated picture**: mid tones 64 against 40 (darkest tones equal, 4). Measure
   again after 1 and 2; candidates: the gamma curve (sRGB is only close to the console's piecewise-linear one),
   exposure / glare inputs.
4. **2x MSAA reflection targets and resolves**, strong glare, and frames of 185-230 ms while driving (time inside
   the ring thread's own draw work; about 25,000 render-target copies rejected in a driving run).
5. **Green car in the evening** (low priority, user: expect it to go with the other fixes). From ~100 s after
   launch the car's upward-facing panels turn green (emulated: stays orange, `build_logs\reference\test-eveXenos-*`).
   Ruled out: texture cache across frames, vertex dedupe, glow sprites n3325/n2933/n3285, the vertex index. Next:
   compare the car paint shader's constants and textures day against evening.
6. Small: two texture formats not supported (causes 422 = k_24_8 not coming from a resolve, 458 = k_DXT3A), 1D
   textures (cause 30), textures with other sign modes than gamma (cause 31).

## How to run and test

- User: `run_fh1.bat --fh1_renderer=native`. Normal play (emulated GPU): `run_fh1.bat`.
- Unattended, to the festival with the user's save (no driving; the user drives manually for driving tests):

      powershell -ExecutionPolicy Bypass -File tools\auto_test.ps1 -Name fest -Seconds 100 -Shots "53,70,95" `
        -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a" `
        -ExtraArgs "--fh1_renderer=native"

  Without `-ExtraArgs` the same run uses the emulated GPU: take both and compare the same seconds. Timeline of
  that run: logo videos 2-26 s, title from ~28 s, menu ~36 s, loading screen ~38-45 s, festival from ~47 s.
  Evening lighting starts ~90 s.
- Build: `tools\build_windows.ps1 -SkipFetch -SkipCodegen` (app only, ~5 min). Run it in the background; never
  wait for it with an open-ended loop.
- Shader library: `fh1_shaders.nfsp` next to fh1.exe (copy of `build_logs\shaders\fh1_shaders.nfsp`, game-derived,
  not in git). After changing the translator (`shaders/XenosRecomp`): `tools\build_shader_tools.ps1`, then
  `python tools\fh1_retranslate_changed.py --apply` (recompiles only the shaders whose HLSL changes), repack and
  copy (steps in that script's header).

## Diagnostics that paid off

- `--fh1_native_diag_frame_s=N`: every draw and copy of one frame in the log (`[trace]` lines: shaders, render
  target, blend, each texture with format / signs / exponent). Shader numbers to files:
  `python tools\fh1_shader_name.py 1212` -> `build_logs\shaders\hlsl\<name>.hlsl` (read only the last ~60 lines).
- `--fh1_native_diag_constants_ps=1212 --fh1_native_diag_constants_ms=0`: the pixel constants a shader gets.
- `--fh1_dump_resolved_at_s=N`, `--fh1_native_diag_resolved=true`: the resolved images of a frame.
- The log's `(cause N)` lines: everything the renderer rejects or replaces, once per kind.
- Same seconds on both renderers + brightness percentiles of the picture area (a 20-line PIL script) settles
  "is it brighter / darker" questions quickly; pixel-level crops enlarged with NEAREST settle edge questions.
- `--fh1_dump_ring_shaders=DIR` + `tools/fh1_synth_containers.py`: shaders the game uploads that the library lacks.

## What was fixed on 2026-10-04, and why (so the same causes are recognised again)

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
