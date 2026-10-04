# Handoff (2026-10-04): next session = the native renderer, as FH1's own

Read this after CLAUDE.md. The English rename is finished (docs/handoff-english-rename.md, top section).

## The user's decision (2026-10-04)
In the user's words: "We should build our own native renderer applying his fixes that work on ours and not just
straight up use his renderer as is (I want a fh1_renderer=native and not nfsc_renderer=native)".

So: the FH1 native renderer is **ours**. GoatHonks' nfsc-recomp renderer is a source of fixes and ideas to take
when they work for FH1, not the thing we run unchanged.

What exists today (both build into fh1.exe):
- `fh1/src/fh1_native_*.cpp`, `--fh1_renderer=native`: our own first attempt (steps N1-N3a: own graphics system,
  shader identification, render targets, clears, resolves, present). It draws nothing yet (black frame).
- `fh1/src/native/nfsc_*`, `--nfsc_renderer=native`: his renderer with the FH1 changes on top. This is the one
  that draws the title, menus, loading screens and the festival at 30 fps.

Not decided yet - **ask the user first thing** which of these they mean (my reading, not the user's words):
- (a) make the working code ours: rename `fh1/src/native/nfsc_*` to `fh1_native_*`, the option to
  `--fh1_renderer=native`, the settings to `fh1_*`, the library to `fh1_shaders.nfsp`, remove what FH1 does not
  need (Most Wanted / Carbon special cases, Switch-only paths) and retire the old first attempt; from then on his
  fixes are ported by hand. Fastest way to keep the picture we have. Or
- (b) keep growing the old first attempt (`fh1_native_*.cpp`) and port pieces of his renderer into it one by one.
  Much slower: the festival picture would have to be rebuilt.
Either way `diff` against `..\repos\nfsc-recomp-main\carbon\src\native` stops being a plain diff, so note in
`fh1/src/native/README.md` (or its successor) which of his fixes were taken and from which date of his repo.

Also still open from the rename (user has not answered): translate the ~1,100 half-Spanish log messages in the
Most Wanted-only files of `reference/nfsmw-app` by hand, or delete that folder (his English repo is the better
worked example now). `mesa/mesa-switch-nfsmw.patch` was left untouched.

## How to run and test
- User: `run_fh1.bat --nfsc_renderer=native` (until the option is renamed). Normal play: `run_fh1.bat`.
- Shader library next to fh1.exe: `nfsc_shaders.nfsp` (copy of `build_logs\shaders\fh1_shaders.nfsp`).
- Title: `tools\auto_test.ps1 -Name t -Seconds 45 -Shots 43 -ExtraArgs "--nfsc_renderer=native"`
- Festival (boots the user's save, no driving): add
  `-Seconds 100 -Shots 95 -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a"`
- Reference pictures of today's state: `build_logs\test-en2-fest-20261004-145302-95s.png` (native) and
  `build_logs\test-en2-xenos-20261004-145455-72s.png` (the correct picture, emulated GPU).
- Build: `tools\build_windows.ps1 -SkipFetch -SkipCodegen` (app only, ~5 min; without `-SkipCodegen` ~15 min).
  Run it in the background and never wait for it with an open-ended loop.
- After changing shaders/XenosRecomp/shader_common.h: `tools\build_shader_tools.ps1`, regenerate
  `build_logs\shaders\hlsl`, `tools\fh1_compile_shaders.py`, `shaders\fh1_pack_library.exe` (delete the old .nfsp
  first), copy next to fh1.exe as `nfsc_shaders.nfsp`. The translator now writes the English names of
  shader_common.h, so HLSL generated before 2026-10-04 must be regenerated, not patched.

## Open problems of the native picture, in the order to take them
(Details and the planned fix for 1 are in docs/handoff-english-rename.md, "State of the native renderer".)
1. Vertex index in r0.x: vegetation / billboard vertex shaders read `r0.x` (the vertex index on the Xbox 360)
   before writing it; the translator starts r0 at 0. With packed positions on the festival turns black
   (`--fh1_vertices_10_11_11_mask=65535` to see it; default 0xFFFE).
2. Speckled edges: depth/stencil fills drawn at 640 pitch 4x MSAA and used by the 1280x720 1x passes
   (`--fh1_msaa_4x_as_1x=true` turned the picture pink/black; needs `--nfsc_dump_resolved_at_s=N`).
3. White title / menu background: the frame composite pixel shader outputs white.
4. 2x MSAA reflection targets and resolves; strong glare; frame drops since the wide resolves.
Debug switches: `--nfsc_native_diag_frame_s=N`, `--nfsc_native_diag_resolved=true`, `--nfsc_dump_resolved_at_s=N`,
`--nfsc_dump_ring_shaders=DIR` + tools/nfsc_synth_containers.py.
