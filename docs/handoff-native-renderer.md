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

## Open problems of the native picture, in the order to take them
(Details and the planned fix for 1 are in docs/handoff-english-rename.md, "State of the native renderer".)
1. Vertex index in r0.x: vegetation / billboard vertex shaders read `r0.x` (the vertex index on the Xbox 360)
   before writing it; the translator starts r0 at 0. With packed positions on the festival turns black
   (`--fh1_vertices_10_11_11_mask=65535` to see it; default 0xFFFE).
2. Speckled edges: depth/stencil fills drawn at 640 pitch 4x MSAA and used by the 1280x720 1x passes
   (`--fh1_msaa_4x_as_1x=true` turned the picture pink/black; needs `--fh1_dump_resolved_at_s=N`).
3. White title / menu background: the frame composite pixel shader outputs white.
4. 2x MSAA reflection targets and resolves; strong glare; frame drops since the wide resolves.
Debug switches: `--fh1_native_diag_frame_s=N`, `--fh1_native_diag_resolved=true`, `--fh1_dump_resolved_at_s=N`,
`--fh1_dump_ring_shaders=DIR` + tools/fh1_synth_containers.py.
