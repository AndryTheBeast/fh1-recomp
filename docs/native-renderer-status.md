# Native renderer: where it stands and what to fix next

Read this after CLAUDE.md. It is the starting point for the next session. State as of 2026-10-05, fourth session:
the first-person view works (world, mirrors, hands and dashboard), the brightness no longer jumps after a loading
screen or a view switch (confirmed by the user), the festival's brightness equals the emulated picture's (58 s:
5/20/35/92/129 against 5/19/35/92/129) and alpha to mask covers 0 to 4 samples; see the "fourth session" section.
Earlier the same day: soft reflections, the console's gamma curve, smooth edges, the loading-screen freeze.

**The user drove the fourth session's build and confirmed its fixes ("I can confirm all the fixes for this session
are fixed").** What they reported next, to fix first:

- **0. The design creator (paint shop > Design creator > Paint car / Color select) is wrong on the native
  renderer** (the user's pictures: `build_logs\reference\user-20261005-design-creator-a-paint-car-menu.webp`,
  `-b-leaving-dialog.webp`, `-c-color-select.webp`). What they show: the paint booth is flat grey and white with
  no shading (ceiling lamps as plain white bars); the car body is one flat cyan with its lower half black, no
  reflections; **every wheel is a solid magenta box** (and a magenta block under the front bumper); the people
  standing behind the car look right; the "Leaving paint shop" dialog has an empty black body (no text) with a
  dotted pattern. Not looked at yet. Where to start: both renderers in that screen (the unattended test reaches
  the paint shop with `auto_test.ps1 -Autoplay "...;60+0.3=x"`; one more A press at about 70 s opens the design
  creator, which changes nothing in the save until a design is saved), the log's `(cause N)` lines that appear
  there, a trace (`draw VS n-1 ... no registration` = a shader the library lacks: `--fh1_dump_ring_shaders` +
  `tools\fh1_synth_containers.py`, as for the mirrors), then a RenderDoc capture of each renderer and
  `tools\rdc_tex_stats.py` / `rdc_draw_textures.py` on the wheel and body draws. Guesses, to be checked and not
  trusted: a flat magenta box is what a draw with the wrong vertex data or a missing texture looks like here; the
  flat cyan body is the paint without its environment map (the booth probably has its own cube map or a
  different reflection pass); the booth without shading may be a missing light map.
- **0b. The thumbnail bug is back: saving a car with a new modification gives a wrong thumbnail** (the user: "the
  thumbnail bug is here again when I save the car with a new modification"). On the emulated GPU the car photos
  were fixed on 2026-10-01 by `readback_resolve=fast` (the game reads the resolved picture back from guest
  memory to make the photo). The native renderer's resolves do not reach guest memory
  (`--fh1_native_read_resolved_float` copies only the small float pictures and is off): find which resolve the
  photo comes from (a trace while the user saves, `run_native_capture.bat` + "now") and copy that one to guest
  memory in the console's format. Ask the user first whether the wrong thumbnail shows on the native renderer
  only or on the emulated one too, and what it looks like.

**Open list after those two (2026-10-05, end of the fourth session):**

- **A. Races, garage, car photos on the native renderer** (the rest of the user's item 5; the paint shop is done,
  see "Item 5" in the fourth session's section). The user drives there.
- **B. Ask the user** how the crowd's edges look up close (`--fh1_native_alpha_to_mask_samples=false` is the old
  test at one half) and whether the first-person view is right while driving (it was only checked parked).
- **C. Night colors at the festival: still different** (measured after every fix of the fourth session,
  `build_logs\reference\test-ngN-*` / `test-ngX-*`, the car parked in the bumper view). Brightness is equal
  (340 s: 5/25/50/79/147 native, 5/28/54/79/147 emulated) but the mean color is 69 55 49 against 63 60 56: more
  red, less green and blue, at 300 s too (71 58 53 against 65 62 60). Daylight has no such difference (63 54 46 on
  both). Not looked at: capture both renderers at 320 s (`auto_test.ps1 -RenderDoc -Triggers "320"`) and compare
  the composite's inputs with `tools\rdc_tex_stats.py` (the color grading cube changes with the time of day; the
  scene before post-processing matched at dusk in the third session, not checked at night).
- D. Then: frame time while driving, items 6-8 below, the depth pre-pass drawing alpha to mask draws solid.

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
`build_logs\reference\user-native-20261004-night.webp` is the user's own screenshot after the night's fixes.

**Items A (crowd animations) and B (over-sharp picture) were done on 2026-10-04 late night**: see "What was fixed"
below. What is left of B is items 4 and 5 of this list (one sample instead of 4x MSAA).

**Items 0, 0a and 0b were done in the second session of 2026-10-05** (kept below with their old notes, marked
DONE). What follows in this paragraph is the order as it stood before that session.
**Item 2 (the green car at evening) and the glow of item 3 were done on 2026-10-05.** The prompt for the next
session is `docs/next-session-prompt.md`. The order for what is left (the user's, after the night drive of 2026-10-05): the blue outline at night (item
0), the headlights that do not light the road (item 0a), the map screen's missing circle selector (item 0b),
then items 3, 4, 5. The user's order had been (2026-10-05, after looking at
the result: "you fixed a lot of things"): the green car at evening first (item 2), then the other things on
this list. The user also drove at night and
sent two crops: coloured specks (blue, orange) exactly on the car's silhouette, "the sharpening still looks kinda
wrong". See item 0.

How item 2 was started (kept as the method; steps 1 and 2 are now tools, see "Diagnostics that paid off"):

1. Make `--fh1_dump_resolved_at_s` write the float images too (scene 1C4E1000, bloom chain, cube faces 1C879000..):
   today only the 8-bit ones are written, and the evening problem is in pictures that became float. Tone-map for
   the PNG (for example x / (1 + x), then gamma) and say so in the file name.
2. One RenderDoc capture of the emulated GPU at the same second (`auto_test.ps1 -RenderDoc -ExtraArgs
   "--renderdoc_capture_seconds=97"`, then `tools/rdc_dump.py`): the shadow mask (1CE2D000), the cube map faces and
   the scene before post-processing, next to the native dumps of second 97.
3. The car's rear panel is the question: find its draw in a one-frame trace (`--fh1_native_diag_frame_s=97`: the
   draws with the cube map 1C879000 just before the tail lights), log its pixel constants day and evening
   (`--fh1_native_diag_constants_ps=<n>`), and check each of its textures in the dumps. Suspects, most likely
   first: the shadow mask's green channel (1 everywhere but the sky at evening: it may be a second light's mask
   whose stencil-tested passes PS n800 / n56 do not do here what they do on the console), the cube map (its faces
   are 2x MSAA on the console and drawn at one sample here), a texture format still replaced by an empty one
   (cause 422 = k_24_8 not coming from a resolve).

0a. **Night: the headlights light the road: DONE 2026-10-05** (see "What was fixed on 2026-10-05, second
   session"). The notes from before the fix:
   (user, 2026-10-05,
   `build_logs\reference\user-night-20261005-c-headlights-no-ground-light.webp`: the lamps and their glow are
   there, the road in front of the car is as dark as everywhere else). What was known from the
   green-car work: the lighting of headlights and brake lights is deferred. PS n56 writes exp2(-light) of one
   light into green, blue and alpha of the shadow mask (1CE2D000) from the scene depth, a 1D falloff texture
   (132D9000, k_16) and a beam texture (136AC000); every shader that is lit reads those channels when its
   boolean psDeferredHeadlightEnable is set (the car paint: PS n1265, `-log2(mask) * dynamicLights5`). At the
   festival at evening that mask now matches the emulated one for the brake lights (255 124 138 191), so on the
   night road check, in this order: (1) the mask there against a RenderDoc capture of the emulated GPU at the
   same spot (is PS n56 drawn for the headlights, with which lightParams / lightColourTint; the emulated
   capture of the festival had one n56 draw); (2) the road and terrain shaders: do they have the
   psDeferredHeadlightEnable branch and is their boolean set (`--fh1_native_diag_constants_ps=<n>` prints the
   booleans; bool4 bit 30 for the car paint), and what they do with the mask's channels; (3) the raw scene
   (1C4E1000) of both: at the festival the emulated ground and car body were already brighter before
   post-processing, which may be the same missing light as item 3's darker picture.

0b. **Map screen: the circle selector: DONE 2026-10-05** (a k_DXT3A texture; see the second session's section).
   The plan from before the fix: both renderers on the map screen (ask the user how to get there or
   for a screenshot), a one-frame trace there (`--fh1_native_diag_frame_s`), the log's `(cause N)` lines, and a
   RenderDoc capture of the emulated GPU to find the selector's draw (`tools/rdc_draw_state.py`).

0. **Blue outline: DONE 2026-10-05, second session** (it was the clear color of the reflection cube map, none of
   the suspects below; see that session's section). The two earlier fixes (cut at 0, cut at the console's
   ceiling) stay: they are correct for float pictures, they were just not the cause. The notes from before:
   **STILL THERE after both fixes (user's night drive, 2026-10-05).** Screenshots:
   `build_logs\reference\user-night-20261005-b-rear-blue-outline.webp` and `-d-outline-crop.png` (after both
   fixes), `-a-blue-rims.webp` (after the first only). What they show: a blue line one pixel wide exactly on
   the car's silhouette against the road (roof edge, window frame, sides), the rear window's slats and the
   bumper's lower half blue-violet, the body almost black (in the user's emulated night crop of the morning the
   car is orange). The orange dots are not in the new screenshots. So the cut did not remove it, and it is
   probably not a few huge pixels: the line follows every silhouette edge evenly. Suspects, most likely first:
   (1) the scene is drawn with one sample where the console has 4x MSAA: on a silhouette pixel the console
   averages car and road, here the pixel is all car, lit at a grazing angle where the paint and chrome
   shaders' fresnel term goes to full reflection of the (blue, night) sky cube map; and the mask / depth the
   post-processing reads were made at another sample position (the 4x clears drawn at twice the scale);
   (2) the reflection cube map has one level here and nine on the console (1C879000 256x256, then 1C9F9000,
   1CA59000, 1CA71000 ... rendered by the game): chrome and glass sample the sharp level 0 where the console
   takes a blurred, darker one, which would also explain the blue window and bumper; build the cube map with
   its levels from those resolves and compare; (3) the game's FXAA (PS n2283) on the mask or the scene
   brightening an edge. Start by taking the same night spot on both renderers (the user drives; the unattended
   test stops at the festival) and compare the raw scene 1C4E1000 with the capture's: if the blue line is
   already in the raw scene it is 1 or 2, if not it is post-processing. The notes of the second fix:
   **Blue rims and orange dots on the car at night: second fix 2026-10-05.** The
   user's screenshot after the first clamp (night road, native): blue on the chrome trim, the bumper edges and
   the rear window, a few orange dots on the outline. Second fix: `--fh1_native_float_cut` (on by default).
   The console resolves the float scene into 10 bits with exponent bias -2 (cube map -4) and the fetch
   multiplies it back, so a fetch never returns more than 2^(its exponent adjust): 4 for the scene (2 and 1
   for the FXAA's other two taps), 16 for the cube map. The float images here had no ceiling (single pixels
   of 32,208 in the resolved scene at the festival, with 248 negative values and 48 NaN). `fh1Exp` in
   `shader_common.h` now cuts at that ceiling; the renderer packs it into the mantissa of the (negative)
   exponent scale (`ExpScaleFloatPicture` in `fh1_native_draws.cpp`), so the shared constants did not change
   size. Measured at the festival (not at night on the road, where the user saw it): brightness unchanged
   (58 s: 4/12/36/82/115, 95 s: 4/12/31/92/168), mean horizontal gradient 1.39 -> 1.56 at 58 s and 1.31 -> 1.44
   at 95 s (emulated 1.50 and 1.41: the FXAA now sees the same three pictures as on the console), 30 fps.
   What was planned if it did not work (it did not): (1) take the user's spot with a dump (`--fh1_dump_resolved_at_s=-1` and
   a file named dump_now, or ask the user for the second) and read the `[fh1] dump_resolved float` lines for
   the scene and the cube map: where are the huge / negative / NaN values, and which draw writes them (the
   chrome shader PS n259 and the glass are the surfaces that showed blue); (2) the blend inside the float
   target keeps a NaN or an infinite value under every later draw: clamp the shaders' color output for float
   targets (the console's 7e3 format ends at 31.875 and has no negative values); (3) the cube map has one
   level here and nine on the console (the game renders them: 1C9F9000, 1CA59000, ...): chrome samples the
   sharp level where the console samples a blurred one. The first clamp's notes:
   **Coloured specks on silhouettes while driving at night** (user's crops, 2026-10-05). Cause found the same day:
   since the resolved scene is a float image it keeps negative values and NaN (the car paint's grazing-angle terms
   make them on silhouettes), which the console's unsigned formats cannot hold; the post-processing turns them into
   blue or orange pixels. Fetches of such pictures now cut them to 0 (`fh1Exp` in `shader_common.h`: the renderer
   writes a negative exponent scale for a float resolved picture). **Check the result line at the end of "What was
   fixed"**; if specks are still there, the next suspect is blending inside the float target itself (a NaN written
   once stays under every later blend) and the fix is to clamp in the shaders' color output for float targets.

1. **Driving: checked by the user on 2026-10-05 after the second session's fixes: "Everything looks normal"**
   (day, night, a race). One thing left to settle: "the rectangular placeholder under the car that has a
   different texture", which the user is not sure is a fault ("we are yet to confirm if that also happens in
   real xbox gameplay"). The user then checked the emulated GPU: **"it happens on both"**, so it is not a native
   renderer fault. Either the game looks like that or both renderers share the cause; only footage of the
   console can tell. Not looked at (a guess: the car's ground shadow quad). The notes from before that drive:
   **Driving has not been checked since the fixes of 2026-10-04 night.** The user drives by hand: ask for a short
   drive first. To look at: motion blur while moving (the velocity pass now gets real depth and stencil; the car's
   own stencil value, 21, picks its matrix), frame drops (185-230 ms frames were seen before), anything that
   flashes.
2. **Evening look: DONE 2026-10-05** (the rear panel; the red glow is item 3). What was known before the fix:
   (from ~90 s after launch, `test-latenight-*-95s.png` against `test-festX-*-95s.png`): after
   the late-night fixes the sky, the light beams and the crowd match the emulated picture. Left: **the car's rear
   panel is flat bright green** (the emulated one is dark with reflections), and the tail lights have no red glow.
   What is known: the same shaders draw the car day and evening (same PS list with the cube map 1C879000); the 1D
   texture that appears at that moment (132D9000, k_16, 256 wide) is bound now and changed nothing; the shadow mask
   (1CE2D000) at evening is red = sun shadow, green = 1 on everything but the sky, and its passes differ from the
   day ones (PS n800 clouds, PS n56 with color mask 0xE, blend 00080008). Next: a RenderDoc capture of the emulated
   GPU at the same second (`auto_test.ps1 -RenderDoc`) to compare the mask's green channel and the cube map faces.
   The float images (scene, bloom chain, cube faces) are not written by `--fh1_dump_resolved_at_s` yet (8-bit
   only): add that first.
3. **The glow part is DONE 2026-10-05** (see "What was fixed on 2026-10-05": it was not the bloom). Left of this
   item: the picture is a little darker than the emulated one (brightness percentiles 5/25/50/75/95 at 95 s:
   native 4/11/30/90/169, emulated 5/13/40/100/176; at 58 s 4/12/36/82/115 against 5/15/40/92/126). In the raw
   scene before post-processing the emulated ground and car body are already brighter (compare the scene
   resolved to 1C4E1000 with the capture's 1280x720 R10G10B10A2 texture, which is the scene / 4): start there,
   with the car and ground shaders' inputs (the cube map has one level here and nine on the console; cause 422 =
   a k_24_8 texture replaced by an empty one, bound by the cube map pass's draws as t8). The old notes:
   **Brightness and glow in daylight**: the big difference is fixed (exposure). The native picture is now a
   little darker than the emulated one (median 33 against 38 at 58 s) and the tail lights still have no red glow
   (bloom). Candidates: the console cuts the resolved scene at 4.0 (10 bits after exp_bias -2) and the float
   texture does not, so the measured luminance can be higher and the exposure lower; the bloom chain (not dumped
   yet, see item 2); the gamma curve (sRGB is only close to the console's piecewise-linear one).
4. **Stair-stepped edges: DONE 2026-10-05, third session** (`--fh1_native_ssaa`). The old note:
   (car silhouette, shadow edges on the ground): the bright outlines are gone (exponent
   bias), what is left is the scene drawn with one sample where the console uses 4x MSAA. `--fh1_msaa_4x_as_1x`
   style tricks do not help here: the scene needs real multisampled targets (or supersampling) and a resolve.
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

- **A spot only the user can reach** (night road, map screen), since 2026-10-05: the user starts
  `FH1-recomp\run_native_capture.bat` (native: `--fh1_dump_resolved_at_s=-1 --fh1_native_diag_frame_s=-1`) or
  `run_emulated_capture.bat` (emulated GPU through RenderDoc with `--renderdoc_capture_seconds=-1`), drives,
  stops and says "now"; then `powershell -ExecutionPolicy Bypass -File tools\capture_now.ps1 -Name <name>`
  takes a screenshot (`build_logs\spot-<name>-*.png`) and writes the trigger files. Native: dumps in
  `FH1-recomp\dump_resolved` (the second request in `dump_resolved_2`, ...) and a `[trace]` frame in the run
  log each time. Emulated: `build_logs\rdc-spot-<date>_capture*.rdc`. Night falls about 5 minutes after launch.
  `tools/rdc_texture.py` reads one texture of a capture (every cube face: smallest and largest value, PNG;
  `RDC_WHITE=0.0625` for the dark 10-bit pictures).
- **Which shader paints this?** at such a spot: `FH1-recomp\run_native_skip.bat`
  (`--fh1_native_diag_skip_ps=file`), the user parks, then `tools\skip_cycle.ps1 -Name x -List "n,n,n+n"`
  leaves out each entry's draws for 2.5 s and photographs the window (`build_logs\skip-x-<entry>.png`). If no
  entry removes the fault, it is not a draw (a clear, a copy). The list of a pass's shaders comes from a
  one-frame trace (the cube pass: lines with `surf 04010140`). The trace's draw lines now end with the boolean
  registers (`bools <vertex b0-31> <pixel b0-31> <pixel b96-127>`).

- Emulated GPU as the reference, from one RenderDoc capture (`auto_test.ps1 -RenderDoc -ExtraArgs
  "--renderdoc_capture_seconds=97"`, each script's header says how to run it): `tools/rdc_dump.py` (actions,
  render targets; `RDC_SAVE=eid:resource` saves one picture at one event, `RDC_ALPHA=1` keeps its alpha),
  `tools/rdc_constants.py` (the shader constants and textures of a draw, found by index count or event id; the
  n-th float4 of xe_float_cbuffer is the n-th register the shader uses, in rising order),
  `tools/rdc_draw_state.py` (a draw's vertex positions, viewport, depth test, blend). Native side:
  `--fh1_native_diag_constants_ps=<n>` now logs every constant that is not zero.
- The float images (cube map faces and their smaller levels, bloom chain, luminance) are dumped too since
  2026-10-05: `..._float_tm.png` = x / (1 + x) then gamma 2.2, with a log line per image (`[fh1] dump_resolved
  float`: largest and mean value per channel, how many negative values and NaN).
- `--fh1_dump_resolved_at_s=N`: at second N, every resolved image, render target and depth-as-bytes image
  (`dump_resolved\NN_<address>_<size>.png` next to fh1.exe), and **for the following frame every color image right
  after its copy** (`frame_NN_dest<address>_<size>.png`, with `alpha_` twins): the post-processing chain step by
  step. This found the empty velocity image in minutes. The dumps are upside down.
- `--fh1_native_diag_frame_s=N`: every draw and copy of one frame in the log (`[trace]` lines: shaders, render
  target, blend, each texture with format / signs / exponent). Shader numbers to files:
  `python tools\fh1_shader_name.py 1212` -> `build_logs\shaders\hlsl\<name>.hlsl` (read only the last ~60 lines).
- `--fh1_native_diag_constants_ps=2520 --fh1_native_diag_constants_ms=0`: the pixel constants a shader gets
  (c0-c11, c32-c43, c132-c135).
- `--fh1_native_diag_constants_vs=3064`: the same for a vertex shader (c32-c39, c128-c131, c156-c163), its textures
  (`vtN`) and the start of its vertex streams (`vf89`-`vf95` are Direct3D streams 6-0).
- `[fh1] computed index:` log lines: the draws rejected with cause 317 (shader numbers, first indices, each fetch).
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

## What was fixed on 2026-10-05, fourth session: first-person view, brightness jump, the darker picture

Pictures: `build_logs\reference\item1-cockpit-before-after-emulated.png` (cockpit: native before, native after,
emulated) and `item3-brightness-before-after-emulated-58s.png`; screenshots `view-ckN-*` / `view-ckX-*` (cockpit)
and `test-blN-*` / `test-blX-*` (58 and 103 s) in the same folder.

- **First-person view: white world.** In the cockpit view the game draws the driver's hands and the interior first
  (stencil not 0), then seals those pixels at the nearest depth with a full-screen rectangle (VS n2128, depth only,
  depth control 087005F7: front "stencil not equal 0, keep", back "always, replace") and draws the world around
  them. A rectangle list has no front or back on the console; the emulated GPU uses the front state for it. Here
  the back state was applied: the whole screen was sealed and no world draw passed (the white is the scene's clear
  color after exposure). Fix: `control_depth` loses bit 7 (two-sided stencil) for rectangle lists
  (`fh1_native_draws.cpp`). The chase views never showed it: their seal has no back state (00700577).
- **Mirrors without scenery.** The mirror pass (surface 06010190) draws its scenery with a vertex shader the
  library did not have (trace: `draw VS n-1 PS n-1 ... no registration ... IM_LOAD VS n-1`). Eight such vertex
  shaders were dumped with `--fh1_dump_ring_shaders`, made into containers with `tools\fh1_synth_containers.py`
  (`build_logs\shaders\synth3`) and added to the library. **A library built from the disc alone lacks them**: after
  `build_shader_library.ps1` copy `synth*\*.bin` into `containers` again. Shader numbers shifted by eight (the
  composite is now VS n3070 / PS n717, the bright pass VS n501 / PS n1241).
- **Speckled hands and dashboard, and the brightness jump after a loading screen or a view switch (the user's item
  2).** The sign modes of a fetch constant are 1 = signed (two's complement data) and 2 = biased (value * 2 - 1)
  (`xenos::TextureSign`); the code of the third session had them swapped. The normal maps of the hands and the
  dashboard (k_DXN, signs 2A = biased) were decoded as signed blocks (BC5_SNORM): garbage normals. The adapted
  luminance (1FCA6000, k_32_FLOAT, signs 55 = signed, which means nothing for floats) was read as value * 2 - 1,
  about -0.97: the exposure sat at one end of its curve whenever the game restarted its adaptation. After the fix:
  four runs in a row without the jump (mean brightness per second after loading 49 54 53 52 52, emulated 56 57 57
  56 56; before 66 76 75 51 50), and across three view switches native follows emulated second by second. The user
  confirmed ("it's fixed the lighting issue"). `--fh1_native_texture_signs` is on by default (it always was in the
  code; the note "off by default" of the third session was wrong).
- **Picture darker than the emulated one (item 3).** Step by step with a RenderDoc capture of each renderer at
  second 60 (`tools\rdc_tex_stats.py`): scene before post-processing equal within 1 %, adapted luminance 0.0152
  against 0.0145, color grading cube identical, but the bloom the composite adds was 0.021 against 0.047 (a
  transfer curve of the composite's output showed dark tones equal, mid-tones 10 % down, highlights 5 %: what a
  missing glow looks like). Cause: the bright pass (VS n501 / PS n1241) reads the adapted luminance in its vertex
  shader at sampler register 1, and its pixel shader has a texture at register 1 too. The shared block has sixteen
  slots numbered by the pixel shader's registers and a vertex sampler with a number the pixel shader also used was
  left to the pixel shader: the vertex shader got the scene picture (0.125) as its luminance and chose a far lower
  exposure. Fix: the renderer gives each vertex sampler a free slot and writes the slots in shared word 253
  (`g_VsSlots`, four bits per register); translated vertex shaders take their slot from it (`FH1_VS_SLOT` in
  `shader_common.h`, `emitVertexSamplerSlot` in `shader_recompiler.cpp`). Every shader was re-translated.
  - Numbers after it, brightness percentiles 5/25/50/75/95: 58 s native 5/20/35/92/129, emulated 5/19/35/92/129
    (before 5/14/36/82/111), mean color 63 54 46 on both; 103 s native 10/29/48/85/139, emulated 10/29/48/85/128.
    The crowd's mean color is 93 67 32 against 91 65 32 (it was "brighter than the emulated one": the same glow).
- **Crowd cut-out edges (item 4).** Alpha to mask (RB_COLORCONTROL bit 4) was a test at one half. Now the shader
  does what the emulated GPU does (`fh1AlphaToMask` in `shader_common.h`, pipeline alpha function 7): sample k of
  a pixel is kept when alpha + offset / 16 reaches 0.75, 0.25, 0.5, 1.0 (samples 0-3), the two-bit offset coming
  from the top byte of RB_COLORCONTROL for each pixel of a 2x2 block (passed in bits 16-23 of `g_AlphaFunction`).
  In the scene's passes drawn at twice the size each host pixel is one sample; a one-sample pass keeps the pixel
  when two samples would be covered. `--fh1_native_alpha_to_mask_samples=false` is the old test. Festival at 58 s:
  brightness unchanged (5/20/35/92/130), 30 fps, real GPU per Swap 15.9 ms; the crowd strip looks like the
  emulated one at that distance (`test-a2mN-*-58s.png`). Not judged up close: the user drives.
- **Item 5, the part done unattended: the paint shop** (pressing X at the festival's "Paint shop: enter now"
  prompt: `auto_test.ps1 -Autoplay "...;60+0.3=x"`; picture `item5-paintshop-before-after-emulated.png`). The car
  was right, the background was a flat blue-grey haze where the emulated GPU shows the blurred festival. Two
  causes, both in the depth of field (surface 05000140, three render targets, PS n2998 then two blur passes PS
  n1964 / n2126, then the composite variant PS n2063):
  - The blur passes address their pictures in texels: the fetch instruction's tx_coord_denorm bit, with the pixel
    position as the coordinate. The translator ignored the bit, every tap landed on the picture's edge and the
    blur came out as one color (`rdc_tex_stats.py`: output min = max after the second pass). Now such a fetch is
    emitted as `(coordinate) * <sampler>_InvSize` (`shader_recompiler.cpp`); sixteen shaders use it.
  - 1/size of the scene's resolved depth was the host image's (2560x1440 since the smooth edges). Texel offsets
    and texel coordinates count the console's texels: a resolved picture exactly twice its fetch constant's size
    now reports the fetch constant's size (`PrepareTexture`). Before this the fixed blur also blurred the car.
  - After both: 4/16/56/92/146 against 4/15/54/90/146 (72 s, the car turning on its stand), 30 fps.
  - **Not checked: races, garage, car photos** (the user drives there). Menus with a blurred background (pause,
    garage) use the same depth of field and should be looked at first.
- **Tools of this session.** `--fh1_native_renderdoc=true` (the game started through RenderDoc): a capture of the
  native renderer's own frame each time a file `capture_now` appears next to fh1.exe.
  `tools\view_capture.ps1 -Name x [-Native] [-RenderDoc] [-NoCapture]` cycles the camera with RB, recognises the
  cockpit by its dark roof and captures there (the game remembers the camera of the last run, so the number of
  presses is not fixed). `auto_test.ps1 -Triggers "60"` writes the triggers at a second. `rdc_tex_stats.py` (mean /
  min / max of every texture a draw reads and of its output, floats kept), `rdc_pick.py` (a depth buffer or target
  at chosen pixels after chosen events: this showed the seal writing depth 1 and stencil 21 everywhere),
  `rdc_draw_textures.py` (the textures of a draw as PNG with their formats: BC5_SNORM against BC5_UNORM gave the
  sign swap away), `fh1_shot_means.py` (mean brightness of each shot of a run). The trace's draw lines end with
  clip control, cull mode, the viewport's depth range, alpha reference and depth bias.
- **Method that worked**: when a view is wrong only in the native renderer, capture both renderers with RenderDoc
  at that view and compare numbers pass by pass; leaving shaders out one at a time found nothing here (four runs).
- **Seen on the way, not changed**: the scene after FXAA (render target format 2, 10 bits on the console) is kept
  in 8 bits here (it holds square roots of the colors, so the precision is close); alpha to mask draws that write
  no color (the crowd in the depth pre-pass) lose their pixel shader and are drawn solid.

## What was fixed on 2026-10-05, third session, part 2: smooth edges (the scene at four samples per pixel)

Compare `build_logs\reference\item3-edges-before-after-emulated-58s.png` (the car's edge enlarged: native before,
native after, emulated) and `test-ssaaN-*` (58, 100, 300, 340 s) with `test-readN-*` (before) and `test-cubeX-*`.

- **Stair-stepped edges (the user's item 3).** The scene's 4x MSAA passes (pitch above 640: three strips of
  1280x256) are now drawn at twice the width and height into the image of twice the pitch (2560 wide), which is
  the console's four samples per pixel laid out as pixels, and the resolve averages them (`--fh1_native_ssaa`, on
  by default). It reuses what the 4x clears of 640 pitch already had (`pass_msaa_scale_`):
  - `DrawsVulkan::BeginPass`: the 4x passes above 640 pitch take the doubled image too. The tests that tell a
    shadow map or a pass category by the pitch use the guest's pitch (a depth-only scene pass at 2560 would have
    been taken for a shadow map).
  - `TargetsVulkan::Copy`: the color resolve is a linear blit of 2 to 1 (the average of the four samples) into the
    same 1280x720 resolved texture as before, strip by strip. Clears, the useful area and the copy rectangle are
    in the doubled image's pixels.
  - `CopyDepth`: this GPU has no blit for depth formats, so the resolved depth of the scene stays at twice the size
    (2560x1440 at 1DAC5000, the strips at twice their rows; `FindResolvedContainer` takes a scale). Everything
    samples it with coordinates 0..1 and no filtering, which picks one of the four samples, as the scaled shadow
    map already relied on. The 1x depth pre-pass resolves 1280x720 to the same address earlier in the frame: the
    two images alternate there (parked, no wait).
  - Shaders: the pixel position register counts guest pixels (`floor(iPos.xy * g_PosScale)`, shared word 252; 68
    pixel shaders use it, for example to find their place in the shadow mask). The shared block is 256 words now
    (`Fh1BlockShared v[64]`, `kUboBytesShared`): library and fh1.exe must match again.
  - Numbers: 30 fps for the whole parked run (day to night); the GPU's time per frame went from 19.5 to 21.1 ms.
    Busy scenes while driving are not measured yet (the user drives). The ground also shows its fine texture now,
    like the emulated picture (mean horizontal gradient of the night picture 1.22 before, 1.33 now, emulated 1.37).
- **Sign modes of textures** (`--fh1_native_texture_signs`; **this paragraph had modes 1 and 2 swapped, corrected
  in the fourth session: 1 = signed, 2 = biased, and the option was on by default in the code**): the sign byte of the fetch constant, swizzled, rides in
  bits 15-22 of the slot's exponent scale (the gamma mark of part 1 is now mode 3 of it) and `fh1Gamma` applies
  mode 1, biased (value * 2 - 1). Two textures at the festival use other modes than gamma: the adapted luminance
  (1FCA6000, k_32_FLOAT, signs 55, biased) and one k_DXN normal map (signs 2A, signed, now in BC5_SNORM). The
  picture's brightness did not change with it.
- **Occlusion queries measured for real (item 4): prepared, not the default.** `--fh1_native_occlusion=1` now
  counts the console's four samples per pixel (the scene is supersampled; `ScaleOcclusion` returns 1 for it) and
  sums the last count of each strip per query. At the festival it gives 65 samples on average per query (largest
  760) and the tail lights keep their lit lenses but lose most of the red halo around them
  (`build_logs\reference\item4-glow-faked-measured-emulated.png`; rear of the car, mean color 54 43 41 faked,
  46 42 40 measured). A first run with every count four times too high gave the same picture, so the tail
  lights' boxes probably count close to nothing (they sit in the lamp, behind the lens?). Not settled: whether
  the console shows that small halo too, or the boxes are meant to be tested against less than the whole scene.
  The default stays 0 (1000 samples, as the emulated GPU: full glows, also through walls). To judge it, the user
  drives `run_fh1.bat --fh1_renderer=native --fh1_native_occlusion=1` behind a wall and behind a car. Facts for
  the next try: RB_SURFACE_INFO has pitch 0 when the query's end arrives (only the MSAA field is usable), and
  every end seen in the first frame came with the window offset of the third strip.

## What was fixed on 2026-10-05, third session: soft reflections, the console's gamma curve, the loading freeze

Compare `build_logs\reference\item1-car-before-after-emulated-340s.png` (the car at night: native before, native
after, emulated) and the screenshots `test-cubeN-*` (after the reflections), `test-readN-*` (after everything of
this session) and `test-cubeX-*` (emulated), all at 58, 95, 300 and 340 s of the same unattended run.

- **Chrome and paint sharper and whiter than the emulated ones (the user's item 1).** Three causes:
  - The reflection cube map (1C879000, 256x256) had one level. Its fetch constant asks for nine (mip address
    1C9F9000, largest level 8) and the game renders them itself every frame: each face of level n is drawn from
    level n - 1 (VS n2128 / PS n1477) and resolved to its place (1C9F9000, 1CA59000, 1CA71000 ... one face after
    another, 48 small resolves). `PrepareTexture` now gives the cube as many levels as it finds resolved and copies
    them (`--fh1_native_cube_levels`; log line `cube ... with its faces resolved by C2, 9 levels`).
  - The translator dropped the level of detail of every fetch. 524 fetches use the register set by setTexLOD
    (the cars' envSampler and blurredCubemapSampler in 150 shaders, damage and carbon textures) and 828 ask for
    level 0 (no computed level: shadow maps, post-processing). Now: `g_fh1RegLod` and `fh1Lod(mode, lod)` in
    `shader_common.h`, emitted by `shader_recompiler.cpp` before such a fetch (mode 1 = that level, mode 2 =
    computed + bias; no shader of the disc uses a bias).
  - A resolve only says the pitch of its texture (a multiple of 32), not its width, so the cube levels from 16x16
    down to 2x2 were kept in images 32 wide. The draw that makes the next level fetched them with coordinates
    0..1 and read the empty columns too: each level came out darker (level 5 half, level 6 a tenth, 7 and 8
    black). The fetch constant has the real width: the first fetch notes it (`HintWidthResolved`, log line
    `resolved texture at ... is fetched N wide`) and the next resolve creates the image that wide. All eight
    levels now have the mean of level 0 (0.212 0.243 0.266 at dusk). The bloom chain is not affected (its
    textures really are as wide as their pitch; the game draws into a part of them).
  - Numbers (car and the ground around it, window pixels 800-1760 x 800-1460, mean horizontal gradient):
    1.25 before, 0.81 after, emulated 1.18 (the emulated ground is grainier). The picture: the bumper and the
    paint are soft, the rear window shows its slats.
- **Darker picture (item 2), the part that is fixed.** Textures fetched with the gamma sign went through the
  host's sRGB formats. The console's curve is piecewise-linear and brighter in the dark and middle tones (0.5 gives
  0.25, sRGB 0.214; 0.2 gives 0.05, sRGB 0.033), and the emulated GPU applies it in the shader after the fetch.
  Now the same here: the texture keeps its plain format, the renderer marks its slot by multiplying the exponent
  scale by 1.5 (bit 22 of the float) and `fh1Gamma` converts red, green and blue before the swizzle
  (`--fh1_native_gamma_pwl`; `--fh1_native_gamma_pwl_mask` picks the host formats, log lines `gamma texture
  kind`). Only when the three color components are the gamma ones and alpha is not; resolved pictures and gamma
  render targets are unchanged.
  - The raw scene before post-processing now matches the emulated capture of dusk within a few percent (mean of
    the linear values, native / emulated: sky 0.97, ground 1.08, road 1.04, crowd 1.06, car roof 0.93). Before,
    the status document had the emulated ground and car body brighter there.
  - The final picture, brightness percentiles 5/25/50/75/95: 58 s native 5/14/36/82/111, emulated 5/15/38/90/122
    (before 4/12/34/80/113); 340 s (night) native 4/23/44/70/137, emulated 5/27/53/80/147 (before 4/21/39/64/131).
- **Still open of item 2** (see the open list): at dusk the median is 30 against 38, and at night red matches
  everywhere but green and blue are lower over the whole picture (sky 29 29 35 against 30 40 50; ground 84 73 66
  against 83 86 80). Since the raw scene matches, it is in the post-processing: exposure, bloom or the color
  grading (PS n716 reads a 16x16x16 lookup texture, 136FB000, when colorGradingEnable is set).
- **The loading screen freezing for good.** It happened again on the first run after the new shader library, and
  this time the log told what it is: the ring thread was not waiting for anything, it was idle (0 packets, 217
  empty laps a second) because the game had stopped sending commands, right after one frame of 4.2 s. In every
  run that froze the longest frame was 3.3 s or more (4167 and 3334 ms); runs with 3134 and 3050 ms went on. So
  the game (or Direct3D's own code in it) gives up after a frame of about 3.2 s. The long frame was the ring
  compiling ~170 pipelines itself while the prewarm thread, alone, needed 65 s for the 500 of its list and the
  festival loaded in the middle. Now the list is first walked by several threads at once
  (`--fh1_native_prewarm_threads`, default half the logical cores, at most 6): 392 pipelines compiled in 7 s,
  during the logo videos; worst frame of that run 410 ms. What makes the game stop is not found (a frame that
  long for another reason would freeze it again: look for a time-out around 3.2 s in the game's Direct3D code).
- **Not a fault: the car's sky reflection "missing" at 95 s in some runs.** At the festival the reflection on the
  roof switches on between 92 and 95 s of every run (the roof's mean color goes from 32 32 32 to 67 84 91 within
  three seconds; `test-phaseN-*` has a shot every 3 s). Runs differ by a second or two in loading time, so a
  shot at 95 s falls on either side. Compare cars at 100 s or later, or at night.
- Tried and left off: `--fh1_native_read_resolved_float` copies the small float pictures (cube levels, luminance)
  to guest memory as the console's resolve does. The picture did not change, so the game does not seem to read
  them; off by default.
- `tools/fh1_pic_stats.py A.png B.png [--box x0,y0,x1,y1] [--crop out.png]`: the brightness percentiles, mean
  color, gradient and a side-by-side crop used for every comparison above.

## What was fixed on 2026-10-05, second session: headlights on the road, map selector, blue outline

Compare `build_logs\reference\spot-roadX-*.png` (emulated), `spot-roadN-*.png` (native before) and
`spot-roadN2-*.png` (native after): the same street in Carson at night, the car stopped by the user.
`night-car-emulated-before-after-20261005.png` is the car cut out of the three. Maps: `spot-mapX-*`, `spot-mapN-*`,
`spot-mapN2-*`.

- **Headlights did not light the road.** The deferred light mask (1CE2D000) was right: two PS n56 passes at night
  (one per headlamp), each after a stencil clear (VS n1205, depth control 8701) and a light volume (VS n930, 36
  indices, stencil only). The scenery did not use it: its shaders (153 of them, PS n377 is the commonest road
  one) add `-log2(mask) * HeadLightParams2.x` only when the boolean **bEnableDeferredLightContribution** is set,
  and that is pixel-shader boolean **b100**. The shared constants only carried b0-b15 of each stage (inherited
  from the NFS renderer: `1 << (16 + n)` in a 32-bit word), so the test read garbage. The car paint's
  psDeferredHeadlightEnable is b14, which is why the car did react to lights.
- **Fix.** All eight boolean registers (0x4900-0x4907) go into the shared block (words 244-251,
  `kWordsShared` 252, `Fh1BlockShared v[63]`); the translator names a boolean by its number (pixel b<n> = 128 +
  n) and tests it with `FH1_BOOL` (`shaders/XenosRecomp/shader_common.h`; `shaders/shader_common.h` is an older
  copy nothing uses). Every shader changed: the library was rebuilt (`fh1_shaders_before_bools.nfsp` is the one
  before).
- **Numbers** (car and the road around it, window pixels 800-1760 x 790-1460, mean RGB): emulated 72 50 40,
  native before 36 15 18, native after 70 39 31. Whole picture, brightness percentiles 5/25/50/75/95: emulated
  6/14/26/73/127, before 6/13/17/22/34, after 6/11/26/73/129. Festival at dusk unchanged (95 s: 4/11/31/89/167
  against 4/11/29/91/169 before, emulated 5/13/39/102/177), 30 fps.
- **Map selector.** Log cause 458 appeared the moment the map opened: k_DXT3A (format 58), the alpha half of a
  DXT3 block alone, replaced by an empty texture. No host format has it: `PrepareTexture` now widens each
  8-byte block to a BC2 block (same alpha half, empty color half) after the read and the byte swap, and the
  view reads alpha into every channel (`kSwizzleAAAA`). Such textures skip the fingerprint thread.
- **Blue outline, blue rear window, blue stripe under the tail lights, blue bumper** (day and night, only at
  some places). Compare `build_logs\reference\spot-darkN-*.png` and `user-day-20261005-f-*.webp` (before) with
  `spot-afterclear-*.png` (after): pixels on the car with blue 40 above red and green, 19,091 before, 0 after.
  The user confirmed it after driving the fixed build ("there's no blue outline anymore").
  - **Cause.** The game clears each face of the reflection cube map (k_2_10_10_10_FLOAT, base 0, 256 pitch,
    2x) with the resolve's clear value **00701003**. That value is packed like the target: three 10-bit 7e3
    floats (3, 4 and 7: 0.006, 0.008, 0.014, nearly black) and 2 bits of alpha. `Copy` read every clear value
    as four bytes: 0.012 0.063 0.44, a bright blue. The cube's scene is a low-detail one with gaps (the verge
    beside some roads is not drawn in it), the clear color shows there, and glass, chrome and the paint mirror
    it. The emulated cube has (0, 0, 1 step of 10 bits) in the same strip.
  - **Fix** (`fh1_native_targets.cpp`, the clear inside `Copy`): formats 2 and 10 are read as three 10-bit
    fractions, formats 3 and 12 as 7e3 floats (exponent 0: mantissa / 512; else (1 + mantissa / 128) *
    2^(exponent - 3)), alpha as 2 bits. The scene's own clear (C8521485) changes too; the sky covers it.
  - **How it was found.** The user's dark spot: the cube faces in the dump had a flat saturated blue strip. No
    shader explained it, so `--fh1_native_diag_skip_ps=file` + `tools\skip_cycle.ps1` left out each shader of
    the cube pass in turn while the user stayed parked (`run_native_skip.bat`): no shader removed the blue, and
    leaving out the road's (PS n3129) made the whole car blue (`skip-v1-3129.png`). So it was what is under
    the draws: the clear. Ruled out on the way, with numbers: the cube's color order (resolve with
    copy_dest_swap and fetch with ZYXW cancel out, as in the emulated capture), the car shaders' booleans
    (pixel b0-b31 = 00004211 on both renderers for the paint; the trace prints them now), the deferred light
    mask on the car (255 in all four channels), the paint's background map (bound, but psUseBackgroundMap is
    off on both). The festival never shows it: its cube scene has no gap.
- **Seen on the way, not fixed:** parked at the festival at night (`test-parkN-*` / `test-parkX-*-340s.png` in
  build_logs), the native chrome and paint are sharper and whiter than the emulated ones, which are soft: the
  cube map has one level here and nine on the console (item 3's suspect). Brightness there: emulated
  5/29/53/79/147, native 4/21/40/65/131.
- **The first run after a new shader library** froze on the loading screen once (picture stuck, sound running,
  the ring thread silent for 45 s until the test ended); the three runs after it were fine. Probably the
  pipeline cache being rebuilt for 3,849 changed shaders; not confirmed. If it comes back on a later run, it is
  a real hang: look at what the ring thread (the one that logs `[fh1] loop constant`) is waiting for.
- **How it was found.** The user drove to the same street on both renderers and typed "now":
  `run_native_capture.bat` (dump + trace on demand) and `run_emulated_capture.bat` (RenderDoc capture on
  demand), each triggered by `tools\capture_now.ps1`. The native mask dump already had the beam, so the pass
  that makes the light was fine and the fault had to be in a reader of the mask; the commonest shader that
  binds the mask in the trace was read, and its `#define bEnableDeferredLightContribution (1 << 116)` gave it
  away.

## What was fixed on 2026-10-05: the green car at evening

Compare `build_logs\reference\test-fill-20261005-95s.png` (native after) with `test-fillX-20261005-95s.png`
(emulated, same second) and `test-latenight-*-95s.png` (native before).

- **Cause.** The car paint (PS n1265 and the other car shaders) reads the headlight / brake-light lighting from
  the green, blue and alpha channels of the shadow mask (1CE2D000, sampler ShadowMaskSamp) when the boolean
  psDeferredHeadlightEnable is set, which happens at evening. Those channels are built each frame like this
  (render target at EDRAM base 2D0, 1280 pitch): (1) the game points the **depth buffer at base 2D0** and draws
  a depth-only rectangle (VS n1205, mode 5, depth control 8777, Z = 1, stencil FF) on a 4x surface of 640 pitch:
  on the console that leaves FFFFFFFF in the EDRAM = a white color target; (2) PS n800 (a quad whose four
  vertices are zero at evening: it draws nothing); (3) PS n56, a full-screen pass that multiplies
  exp2(-light) into green, blue and alpha (blend 00080008, color mask E); (4) resolve, FXAA of the mask
  (PS n3327), resolve. Here a depth buffer and a color target are separate images, so step 1 never reached the
  mask: it kept the daytime picture (blue = alpha = 0 on everything but the sky, written by PS n800 in daylight)
  and green was multiplied down to 0 frame after frame. The paint took log2 of those zeros: a flat, very
  bright green panel.
- **Fix** (`--fh1_native_depth_fill_color`, on by default). A depth-only rectangle that writes Z without
  testing it is noted (`ContextTargets::NoteFillDepth`); the next pass that draws into the 8-bit color target of
  the same base and pitch first gets the depth buffer's bytes (`TargetsVulkan::ApplyFillDepth`, the pass that
  already writes a resolved depth as bytes). A resolve that clears the color target cancels a pending fill.
  The depth-as-bytes shader gave FF 00 00 for a depth of exactly 1 (16777216 in float arithmetic): now FF FF FF.
- **Numbers** (mask at second 97, centre of the screen inside the brake-light cone, R G B A): emulated
  255 124 138 191; native before 255 0 0 0; native after 255 124 138 191. Ground outside the cone: emulated 255
  in every channel; before 250 255 0 0; after 255 255 255 255 (`python tools\fh1_mask_stats.py` after a run
  with `--fh1_dump_resolved_at_s=97`). Rear panel mean color in the screenshot at 95 s: emulated 98 44 45, native
  before 70 132 62, native after 57 35 30 (darker than the emulated one: the red glow of the tail lights is the
  bloom, item 3). Daylight unchanged (58 s, brightness percentiles 5/25/50/75/95: native 4/12/35/81/116,
  emulated 5/15/40/92/126), 30 fps.
- The same fill is also noted for the color targets at base 2D0 pitch 400 and base 0 pitch 1280 (log lines
  `[fh1] color target base ... is filled through the depth buffer`); boot, videos, menu, loading screen and
  festival were checked after it. If a screen ever shows depth bytes (a mostly red / white picture), this is
  the option to turn off first.
- How it was found (the method, for the next picture problem): the float dumps showed the cube map was fine;
  the pixel and vertex constants of the car shaders were compared with the emulated ones from a RenderDoc
  capture (identical); the difference was in a texture, so the mask was saved from the capture after each of
  its passes and compared channel by channel with the native dump; then each pass's state (vertices, depth
  test, blend) was read from the capture.

### The red glow of the tail lights (same day)

Compare `build_logs\reference\test-glow-20261005-95s.png` (native) with `test-fillX-20261005-95s.png` (emulated).

- It was listed as "bloom", but the bloom chain works (1DE5D000 has the picture). The glow is already in the
  emulated GPU's scene before post-processing. FH1 draws one small box per light inside an occlusion query (VS
  n2396 / PS n455, quad list of 24 vertices, no color, once in each of the three scene strips) and sizes the
  light's glow with the number of samples that passed. The emulated GPU does not measure: it answers 1000. The
  native renderer measured (`--fh1_native_occlusion=1`, inherited from the NFS renderer, made for the sun flare)
  and got 64 samples on average (largest 796): the glows all but vanished.
- Now `--fh1_native_occlusion=0` is the default: 1000 samples, as the reference. Rear panel mean color at 95 s:
  emulated 98 44 45, native before 57 35 30, native now 87 38 33. A glow is never hidden by an object in front
  of the light (same as the emulated GPU).
- To measure for real later: the count must be in the scene's 4x samples and summed over the three strips (each
  strip issues the query's begin and end again and only sees its own rows); the log line `[native] C2 occlusion:
  mode ...` gives the counts every 20 s.
- The user saw the glow in a test window started with the option and took it for an effect of the green fix:
  a clean run of the pushed build (no options) had no glow at 80, 95 and 100 s.

## What was fixed on 2026-10-04, and why (so the same causes are recognised again)

Late night session (crowd animation, over-sharp picture, brightness). Compare `build_logs\reference\test-latenight-*`
(native after) with `test-festX-*` (emulated) and `test-a2m-*` (native before):

- **Crowd not animated (item A)**: the people who move are not the billboards (VS n583, static) but 3D characters
  (VS n2944 / PS n640, and n653 in the depth pass; ~255 draws per frame). Their bones are a vertex stream fetched at
  `bone index + AnimInfo.x` (a register the shader computed), two bones blended. Every such draw was rejected (cause
  317: 185,000 in 20 s). Now the translator turns a declared fetch whose index register is not r0 into a read from
  memory (`fh1FetchRanked` in `shader_common.h`); the renderer uploads the stream once per frame (found again by the
  vertex dedupe) and writes its place, stride, offset and format in the shared constants (words 180-243, numbered by
  the rank of the fetch instruction among the declared fetches). Fetches indexed by r0.y (billboards) are unchanged.
- **Bright outlines on every edge (item B, the main cause)**: the game's FXAA (PS n2283, the Xbox 360 variant of
  FXAA 3.11) reads the scene through three fetch constants that differ only in their **exponent bias** (exp_adjust
  +2, +1, 0: the picture at 1, 1/2 and 1/4) and adds the samples. With the bias ignored, each edge pixel came out
  2-4 times too bright. Fetches are now multiplied by `2^(exp_adjust of the fetch constant + exp_bias of the resolve
  that made the picture)` (`<sampler>_ExpScale`, shared words 164-179; `--fh1_native_exp_bias`).
- **Highlights cut at white (no glow, flat reflections)**: the scene target is a float image, but its resolved
  texture was 8-bit, so everything above 1.0 was lost before the bloom, the FXAA and the composite. The console
  keeps it by resolving with exp_bias -2 (scene) or -4 (cube map) into 10 bits. A float target resolved to a 32-bit
  format now keeps a float resolved texture (`--fh1_native_resolved_hdr`); the cube map takes float faces; the three
  scene strips find their float container (`FindResolvedContainer` takes the format).
- **Picture about a third too bright**: the final composite's vertex shader (VS n3064) reads the adapted luminance
  (1FCA6000, 32x32 k_32_FLOAT) and picks the exposure with it. Only pixel-shader textures were bound, so it read
  black and took the low-light exposure. Vertex sampler N is fetch constant 16 + N and uses the shader's slot N
  (`--fh1_native_vs_textures`).
- 1D textures are bound as one-row 2D textures (the shader samples them at v = 0.5); float color images get a color
  view (they had a depth one).
- Measured on the festival at 58 s (brightness percentiles 5/25/50/75/95 of the picture, and mean horizontal
  gradient = sharpness): emulated 5/16/38/90/123, gradient 1.53; native before 4/20/52/121/167, gradient 2.24; native
  after 4/12/33/80/114, gradient 1.35. So it is now slightly darker and softer than the emulated picture (was much
  brighter and harder).
- 2026-10-05, the clamp for float pictures (item 0): built and run (festival at dusk with three seconds of
  throttle, `build_logseference	est-clamp-20261005-102s.png`): daylight numbers unchanged, 30 fps, no specks on
  the silhouette. **Not proven**: the specks were never reproduced here before the change either (the user saw them
  driving on the road at night); the user has to look again. In the user's night crop the car is orange, not green:
  the green of item 2 belongs to the festival at evening (stage lights nearby), not to night in general.
- After any change to `shaders/XenosRecomp` or `shader_common.h` every shader changes: `fh1_retranslate_changed.py
  --apply` takes ~6 minutes for all 3,850, then repack and copy. **The library and fh1.exe must match** (the shared
  constants block grew: `Fh1BlockShared v[61]` = `kUboBytesShared`).

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
