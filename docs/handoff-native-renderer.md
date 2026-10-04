# Handoff (2026-10-04): the native renderer, FH1's own

Read this after CLAUDE.md. The English rename is finished (docs/handoff-english-rename.md, top section).

## The user's decision (2026-10-04)
In the user's words: "We should build our own native renderer applying his fixes that work on ours and not just
straight up use his renderer as is (I want a fh1_renderer=native and not nfsc_renderer=native)".

So: the FH1 native renderer is **ours**. GoatHonks' nfsc-recomp renderer is a source of fixes and ideas to take
when they work for FH1, not the thing we run unchanged.

## Done 2026-10-04: the working renderer is ours
The user chose "adopt the working code" (and "delete `reference/nfsmw-app`"). So:
- `fh1/src/native/fh1_*` became `fh1/src/native/fh1_*`: namespace `fh1::native`, every setting `fh1_*`, macros
  `FH1_*`, option `--fh1_renderer=native`, library `fh1_shaders.nfsp`, pipeline cache `fh1_native_pipelines.bin`.
  The same rename was applied to the `fh1_*` names in sdk/, shaders/, tools/ and mesa/README.md (for example
  `--fh1_io_cache_mb`, `tools/fh1_synth_containers.py`, `Fh1BlockShared` in shader_common.h - a name only, the
  shader library did not need rebuilding).
- The old black-screen attempt (`fh1/src/fh1_native_*.cpp`, `fh1/src/fh1_shader_library.*`) was removed; the library
  packer (`tools/build_shader_tools.ps1`) now builds with `fh1/src/native/fh1_shader_library.cpp`.
- Most Wanted's address-based hooks at the end of `fh1_native_hooks.cpp` (already compiled out) were removed, and
  with them the `NFSC_NATIVE_RENDERER` build switch.
- `reference/nfsmw-app` was deleted (git history keeps it).
- `fh1/src/native/README.md` records the base date of his repo (2026-10-04) and has the table to fill in when one
  of his later fixes is ported.

Not removed, on purpose (my judgement, tell the user if they ask): the `REX_PLATFORM_SWITCH` / `__aarch64__` blocks
(small, compiled out on Windows, and the Switch port starts from this code) and the Carbon / Most Wanted special
cases inside the 13,000-line draw code (shadow, reflection, glow and vegetation shortcuts keyed to those games).
Those are best removed one at a time with a festival screenshot after each, not in one sweep.

## How to run and test
- User: `run_fh1.bat --fh1_renderer=native`. Normal play: `run_fh1.bat`.
- Shader library next to fh1.exe: `fh1_shaders.nfsp` (copy of `build_logs\shaders\fh1_shaders.nfsp`).
- Title: `tools\auto_test.ps1 -Name t -Seconds 45 -Shots 43 -ExtraArgs "--fh1_renderer=native"`
- Festival (boots the user's save, no driving): add
  `-Seconds 100 -Shots 95 -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a"`
- Reference pictures of today's state: `build_logs\test-en2-fest-20261004-145302-95s.png` (native) and
  `build_logs\test-en2-xenos-20261004-145455-72s.png` (the correct picture, emulated GPU).
- Build: `tools\build_windows.ps1 -SkipFetch -SkipCodegen` (app only, ~5 min; without `-SkipCodegen` ~15 min).
  Run it in the background and never wait for it with an open-ended loop.
- After changing shaders/XenosRecomp/shader_common.h: `tools\build_shader_tools.ps1`, regenerate
  `build_logs\shaders\hlsl`, `tools\fh1_compile_shaders.py`, `shaders\fh1_pack_library.exe` (delete the old .nfsp
  first), copy next to fh1.exe as `fh1_shaders.nfsp`. The translator now writes the English names of
  shader_common.h, so HLSL generated before 2026-10-04 must be regenerated, not patched.

## The user's order of work (2026-10-04 evening)
In the user's words: "Let's Fix the teal screen in the bigining, then the white video into, the white video
backgrounds the loading screens etc... and then we fix rendering". And: "Don't give to much importance to the
green thing now" (the user saw on the emulated GPU that a reflection is added to the car when evening starts,
and expects the green to go once the rendering glitches are fixed).

Done the same evening (commits 06ccc15 and the one after):
- Teal screen at boot = the trademark / legal text screen. The game resolves it once into a texture (1DAC5000)
  and then presents front buffers it never resolved into. Present now shows the last screen-sized resolved
  texture in that case. The pulsing teal test color is behind `--fh1_native_swap_test_color` (default: black).
- White videos (boot logos, intro, title background). FH1 draws videos into a k_8_8_8_8_GAMMA render target and
  resolves with the color info set to plain k_8_8_8_8; the renderer kept one image per format, so the resolve
  read an image nobody drew. Formats 1 and 0 now share one image (FormatTargetCanonical).
- Grey tint on everything (user report). FH1 loads the piecewise-linear gamma ramp (DC_LUT_PWL_DATA, 128
  segments), not the 256-entry table; the renderer ignored it and showed the picture with an identity ramp, so
  dark tones were lifted in videos, menus and the festival. NoteRampGamma now reads it and samples it into
  the 256 entries the output uses. Checked: Turn 10 logo background 14 on both renderers (was 28 native).
- Diagnostics: one-frame trace up to 20,000 lines; trace lines show each texture's sign bits and exp adjust;
  `--fh1_native_diag_constants_ps` also logs the boolean registers.

Grey tint, second round (2026-10-04 late, user: "only the Dolby intro has deep blacks"):
- The intro videos are not a renderer problem. Their files hold those levels (read with PyAV, `pip install av`):
  Dolby background Y=16 (video black), Playground Y=19-20, Turn 10 Y=29. After the game's range expansion that is
  0, 4 and 14-15 on screen, and native and the emulated GPU both show exactly that (tools: auto_test shots at the
  same seconds on both renderers + percentiles of the picture area).
- The real tint was in menus, loading screens and the festival: darkest tones 20 (emulated: 4), mid tones 112
  (emulated: 40). Cause: textures fetched with the gamma sign (signs 3F: most of the world and the HUD) were read
  raw. They now get the host's sRGB formats (`--fh1_native_gamma_textures`, default on; fh1_native_draws.cpp,
  PrepareTexture). After: darkest tones 4, mid tones 64, loading screens show their picture.
- Videos looked "low bitrate" on native (user report): coarse color steps in dark areas. Cause: the game draws
  videos into a k_8_8_8_8_GAMMA target and fetches the resolved picture with the gamma sign; stored raw, the dark
  tones had a quarter of the precision. Now 8-bit color images carry a second sRGB view (ImageNative::view_srgb):
  passes whose guest format is 1 draw through it, and resolved pictures fetched with signs 3F are sampled through
  it (`--fh1_native_gamma_targets`, default on). Checked with a crop of the Playground background brightened x8:
  smooth, same as the emulated GPU; levels unchanged.
- Still open on this subject: (a) the console's curve is piecewise-linear, sRGB is only close (Turn 10 background
  15 instead of 14); (b) the festival is still brighter than the emulated picture (mid tones 64 vs 40), mixed
  with the smear / speckle glitches below, so re-measure after those; (c) loading screens: the dark / white boxes
  behind the text are missing on native.

Known but not done:
- Loading screens and menus: text on black now (not white). The emulated GPU may show a video or picture behind
  them: compare a loading screen and the pause/main menus on both renderers before calling this item finished.
- Textures sampled with the gamma sign (log: "signed or gamma textures: read as unsigned (cause 31)") and writes
  to the gamma render target format are both passed through raw. For the videos the two cancel; a texture that
  is gamma-signed but was not written through a gamma target would come out too bright.

What the user's driving run showed (build_logs/run-20261004-161606.log): 14 % of all draws rejected, all cause
316 (packed positions); about 25,000 render-target copies rejected; two unsupported texture formats (causes 422
and 458); frames of 185-230 ms while driving with the time inside the ring's own draw work.

## Rendering problems, in the order to take them (after the items above)
(Details and the planned fix for 1 are in docs/handoff-english-rename.md, "State of the native renderer".)
1. Packed (k_10_11_11) POSITIONS: draws that use them are still rejected by default (mask 0xFFFE), so vegetation,
   billboards and whatever else uses them are missing. With `--fh1_vertices_10_11_11_mask=65535` the festival still
   turns black (test 2026-10-04 15:50, `build_logs\test-vid-packed-*-95s.png`).
   Done 2026-10-04: the vertex index. Vertex shaders now start with `r0.x = SV_VertexID` as on the console
   (translator, NFSMW_RECOMP), vertex shaders are compiled with `-fvk-support-nonzero-base-vertex` (index of the
   draw, not index + vertexOffset) and the device enables `shaderDrawParameters` (sdk vulkan_device.cpp,
   Vulkan11Features). Library rebuilt (3,849 shaders; the previous set is kept in build_logs\shaders as
   `*_before_vertexid`). Non-indexed draws get 0..count-1 (VGT_INDX_OFFSET is not added).
   So the vertex index was not the whole cause. Next: `--fh1_native_diag_frame_s=N` with the mask on, find the
   first draw with a packed position and compare its decoded positions with the emulated GPU's (sign / integer
   modes of the fetch, `remapInput`), and check whether those shaders also fetch from guest memory (`fh1Fetch`
   returns 0 while `g_GuestBase` is 0).
1b. Green car in the evening (user report 2026-10-04). The user's observation: it starts when the game's evening
   lighting comes up. Measured: car orange until ~80 s after launch, green from ~100 s (test save, festival).
   Reference with the emulated GPU at 130 s (`build_logs\test-eveXenos-*-130s.png`): ground lit grey-white by the
   floodlights (the native picture does that too, so the ground is right), car stays orange. Native: the car's
   upward-facing panels turn green, its sides stay brown - so the car paint shader gets a wrong colour for the
   evening floodlight term (a light colour, constant or small texture read wrongly), not a wrong reflection: the
   reflection cube faces look plausible (`fh1\out\win-release\dump_day` and `dump_eve`, every image of one frame).
   Ruled out by tests: texture cache across frames, vertex dedupe, the glow-sprite pixel shaders n3325/n2933/n3285,
   the vertex index. Note: `--fh1_native_diag_frame_s` stops at 4,000 lines, less than one FH1 frame, so two traces
   do not cover the same draws; raise the cap (TraceDraw in fh1_native_system.cpp) before comparing frames.
   Next: with `--fh1_native_diag_vertices_ps` / `--fh1_native_diag_constants_ps` on the car body's pixel shader,
   compare its constants and sampler formats day vs evening.
2. Speckled edges: depth/stencil fills drawn at 640 pitch 4x MSAA and used by the 1280x720 1x passes
   (`--fh1_msaa_4x_as_1x=true` turned the picture pink/black; needs `--fh1_dump_resolved_at_s=N`).
3. (done 2026-10-04: white title / menu background, see above)
4. 2x MSAA reflection targets and resolves; strong glare; frame drops since the wide resolves.
Debug switches: `--fh1_native_diag_frame_s=N`, `--fh1_native_diag_resolved=true`, `--fh1_dump_resolved_at_s=N`,
`--fh1_dump_ring_shaders=DIR` + tools/fh1_synth_containers.py.
