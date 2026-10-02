# FH1 native renderer - plan (started 2026-10-02)

Decision (user, 2026-10-02): stop polishing the emulated Xbox 360 GPU and build a native renderer
the way nfsmw-nx did for Need for Speed Most Wanted (its app is in `reference/nfsmw-app/`, its
design in `docs/native-renderer.md` and `docs/shaders.md`). Fix bugs once it draws.

## What nfsmw-nx built (and what FH1 can reuse)

| part | nfsmw files | size | game-specific? |
| --- | --- | --- | --- |
| own graphics system: presenter, MMIO range, vblank thread, command ring reader, the game's GPU waits | `nfsmw_nativo_sistema.*` | 4,900 lines | 15 NFS addresses |
| draws: PM4 state, vertices/indices, textures, pipelines, constants | `nfsmw_nativo_dibujos.*`, `_texturas_pool`, `_vertices_dedupe` | 13,000 lines | none |
| render targets as Vulkan images, resolves as copies, readbacks | `nfsmw_nativo_destinos.*` | 6,200 lines | 8 NFS addresses |
| shader library lookup (pre-translated SPIR-V by microcode hash) | `nfsmw_nativo_shaders.*` | 600 lines | none |
| which shader each draw uses (D3D shader constructors, Draw* records) | `nfsmw_nativo_ganchos.*` | 1,250 lines | all NFS addresses |
| shader translation before playing (XenosRecomp -> HLSL -> DXC -> SPIR-V library) | `shaders/` | tools | NFS container layout (2005) |

So most of the code is Xenos-level and can be ported; the game-specific parts are the hooks and
the addresses in the system part.

## FH1 facts that matter

- **Shaders**: FH1's `.fxobj` files hold the 2008 container layout (`102A1100` pixel,
  `102A1101` vertex): ~1,400 + ~1,500 in `media/shaders/` alone - the layout XenosRecomp reads
  directly (NFS needed a converter). More `.fxobj` sit in the tracks' `bin.zip` archives, which
  use zip compression method 21 (Xbox LZX); nfsmw's tools already include LZX (`shaders/nfsmw_lzx.cpp`).
- **Frame**: shadow cascades, depth pre-pass, reflection cube map, main scene 1280x720 at 4x MSAA
  (predicated tiling, 3 strips - a native renderer simply draws it once), bloom/luminance chain,
  tone map. Readbacks the CPU needs: auto exposure, car photos.
- **EDRAM tricks the game relies on**: the 4x-at-pitch-P = 1x-at-pitch-2P alias (depth and
  colour), and 16 kinds of data handed between render targets through EDRAM while driving
  (`docs/native-render-targets.md`). Natively these become explicit image-to-image copies or
  shared images.

## Phases

- **N0 - shader library**: extract every container (loose `.fxobj`, LZX archives, default.xex),
  translate with XenosRecomp, compile with DXC, pack `fh1_shaders.nfsp`. Measure how many
  translate. Gate for everything else.
- **N1 - own graphics system**: `fh1_renderer=native` replaces the xenos plugin; presenter, ring
  reader, vblank, the game's GPU waits answered. Milestone: the game runs (screen black) without
  freezing.
- **N2 - shader identity hooks**: FH1's D3D shader constructors and Draw* functions
  (`fh1/src/fh1_d3d_census.cpp` already lists the D3D functions).
- **N3 - draws and textures**: first frames - logos, title, menus. In nfsmw's order:
  - N3a (nfsmw C2, `nfsmw_nativo_destinos.*`): render targets as Vulkan images, clears
    (RB_COLOR_CLEAR), resolves as copies into the destination texture (rectangle and base like
    `draw_util::GetResolveInfo`), and presenting the texture the Swap's fetch constant 0 names,
    through the gamma ramp. Without draws this already shows the game's clear colours per frame;
  - N3b (nfsmw C3-C6, `nfsmw_nativo_dibujos.*`): draws - pipelines from the library's SPIR-V
    (push constants with three buffer addresses, textures in sets 0-2, samplers in set 3, constant
    UBOs in set 4), vertex input from the patched fetches, indices, 2D textures by fetch constant,
    blend/depth/stencil/cull state, viewport and scissor; FH1 adds `g_GuestBase` /
    `g_FetchAddress` for the cars' extra streams (shader_common.h).
- **N4 - render targets and resolves**: main scene drawn once at full size, MSAA, the aliases,
  readbacks. Milestone: the festival looks like D3D12.
- **N5 - races, garage, car photos; then performance** (the nfsmw lessons: constants through a
  uniform buffer, pipeline cache and prewarm, texture fingerprints, no busy waits).

The emulated GPU (D3D12 default, Vulkan) stays as the playable path until the native one
matches it. The experiments in `docs/native-render-targets.md` (transfer skipping, tall main
pass) stay off by default and are paused.

## Progress

- 2026-10-02 N0 first pass: `tools/fh1_extract_shaders.py` cut 2,921 containers (2,918 distinct:
  1,407 pixel, 1,511 vertex) from the loose `media/shaders/**/*.fxobj` files;
  `tools/build_shader_tools.ps1` builds the translator (`shaders/nfsmw_hlsl.cpp` + XenosRecomp,
  clang, no downloads) as `shaders/fh1_hlsl.exe`. Translation to HLSL: **2,463 of 2,918** - every
  pixel shader; 455 vertex shaders rejected with "FETCH de vertices sin elemento declarado" (a
  vertex fetch at an instruction the declaration table does not list - probably FH1's mini
  fetches; next to fix). DXC is in the Windows SDK (Windows Kits 10, bin/10.0.26100.0/x64/dxc.exe).
  Output in build_logs/shaders/ (game data: never in git).
- 2026-10-02 **N0 library done (loose .fxobj)**: all 2,918 shaders translate and compile;
  `tools/build_shader_library.ps1` rebuilds `build_logs/shaders/fh1_shaders.nfsp` (84 MB) from the
  disc in one go. Translator changes (all under `NFSMW_RECOMP`, in `shaders/XenosRecomp/`):
  - vertex fetches the declaration does not list (cars: fetch constants 29-31, computed index
    `r0.w`, mini fetches) read guest memory directly: `fh1Fetch()` in `shader_common.h`, with
    `g_GuestBase` (shared constants +488) and `g_FetchAddress(c)` (+496, constants 24-31, guest
    byte address | endian in bits 0-1). The HLSL lists the constants used ("// FH1_FETCH_CONSTANT");
  - vertex-shader samplers: fetch constants 16-19 = table registers 0-3;
  - `cubeMapData` declared in vertex shaders too; level-0 sampling in vertex shaders (`FH1_SAMPLE`);
  - vertex inputs whose usage has no fixed location get 16+ ("// FH1_INPUT_LOCATION");
  - the shared constants block is 33 float4 now (was 23).
  DXC: the official release (github.com/microsoft/DirectXShaderCompiler, unpacked in
  `FH1-recomp\tools_dxc\`); the Windows SDK's dxc.exe has no SPIR-V. `fh1/src/fh1_shader_library.*`
  is FH1's copy of nfsmw's library reader/writer (2008 containers, 256 MB cap), not yet in fh1.exe.
  Still to add: the `.fxobj` inside the tracks' LZX `bin.zip` archives (and default.xex).
- 2026-10-02 track shaders: 184 `.fxobj` (the world's: trees, flags, terrain blends) exist only in
  the tracks' `bin.zip`, zip method 21. Not deflate, not raw LZX (libmspack lzxd fails at every
  window size, also with swapped bytes; `tools/fh1_lzx_probe.cpp`), no XMemCompress frame headers
  visible. Memory dump route: `--fh1_dump_shaders=SECONDS --fh1_dump_shaders_dir=DIR` scans guest
  memory (fh1/src/fh1_shader_dump.cpp) - at the festival it finds 2,453 distinct containers, 1,947
  not on the disc, but many are damaged or patched in memory (impossible registers, huge sizes that
  exhaust the PC while translating), so merging them blindly (`build_shader_library.ps1 -Merge`)
  is not usable yet. Next: find how the game decodes method 21 (its zip reader in the recompiled
  code) and decode the archives offline. Translation is now one process per container
  (`tools/fh1_translate_shaders.py`) and the packer leaves out containers without SPIR-V.
- 2026-10-02 **N0 complete enough to move on: 3,816 of 3,849 shaders (99%)** in fh1_shaders.nfsp
  (106 MB), from three sources:
  1. loose `media/shaders/**/*.fxobj` (2,918);
  2. the tracks' `bin.zip` archives (+453): FH1's zips have **no local file headers** (an entry's
     data starts at its central-directory offset) and method 21 is **XMemCompress LZX, 128 KB
     window**, framed as `FF <u16 BE uncompressed> <u16 BE block>` (or `<u16 BE block>` for full
     32 KB frames). `tools/fh1_unpack_archives.py` + `shaders/fh1_lzx_decode.exe` (libmspack lzxd)
     unpack them, CRC-checked (187/187 files). This also opens every other FH1 archive (textures,
     models) for later tools;
  3. default.xex (+478 engine shaders: post-processing, UI): it is encrypted and compressed on disc,
     so the build scans the loaded image from a game run with `--fh1_dump_image=build_logs/fh1_image.bin`
     (`build_shader_library.ps1 -Image`, that path by default).
  More translator fixes: NORMAL/TANGENT/BINORMAL/POSITION1 interpolators, pixel shader constants up
  to c255 (NFS: c223), `getWeights2D` with 1/size, 1D textures as one-row 2D (`tfetch1D`), generic
  `s0`-`s15` sampler names for engine shaders without names, repeated vertex usages declared once.
  Left: 13 shaders use integer loop constants missing from their tables (`i0`, `i16`), 20 crash the
  translator (exit 0xC0000409) - all engine shaders from the image; to look at when one is needed.
- 2026-10-02 **N1 done**: `--fh1_renderer=native` (fh1/src/fh1_native_system.cpp, from nfsmw's
  stage C1) replaces the xenos plugin: MMIO registers, ring thread (register writes with their side
  effects, MEM_WRITE / COND_WRITE / REG_TO_MEM / EVENT_WRITE_* / WAIT_REG_MEM / indirect buffers /
  predication by bin select), vblank and PM4 interrupts, test colour per Swap. Unattended run: the
  game boots, takes the autoplay presses and reaches gameplay at its 30 fps cap (~3.6 million
  packets/s, mostly 0x60 / 0x22 / 0x61 / 0x27 / 0x2D / 0x2F), one startup WAIT_REG_MEM timeout,
  no crash. Default (emulated GPU) unchanged. Next: N2 (which shader each draw uses) and N3 (draws).
- 2026-10-02 **N2 first stage** (fh1/src/fh1_native_shaders.*, from nfsmw's nfsmw_nativo_shaders.*
  with a 2008 container reader): the ring's IM_LOAD / IM_LOAD_IMMEDIATE uploads are looked up in
  the library (`--fh1_shader_library=PATH`, default next to fh1.exe). Festival run: **354 of 386
  distinct shaders identified (92%)** - pixel 200/203, vertex 154/183. FH1's D3D patches vertex
  shaders more than NFS's: besides the masked fetch fields it rewrites the destination swizzle of
  every fetch and replaces exports the pixel shader does not read with `C8000000 00000000 02000000`;
  a tolerant second pass accepts both (123 of the vertex shaders). The remaining 29 vertex shaders
  have reordered fetch blocks or remapped export registers: nfsmw's answer is hooks on the game's
  shader constructors and Draw* (exact identity) - FH1's D3D functions still to be located (N2b).
- 2026-10-02 **N3a done** (fh1/src/fh1_native_targets.*, lean version of nfsmw's stage C2): render
  targets as Vulkan images per (EDRAM base, format, pitch) in FH1's formats (8888, 2_10_10_10,
  2_10_10_10_FLOAT, 16/32-bit), copy-mode draws as clears (RB_COLOR_CLEAR converted like the SDK)
  and blits into resolved textures per destination address, and each Swap draws the resolved
  texture named by fetch constant 0 into the presenter with the SDK's output shaders. Festival:
  30 fps, ~87 copies + clears per frame, 16 render targets, ~100 resolved textures, every frame
  presented from the game's resolve chain (black: no draws yet). Depth copies (~8/frame) skipped.
  Lesson: never wait for the GPU while a command buffer is recording (replacing a resolved texture
  first ended the recording and crashed the driver); replaced images are retired per slot.
- 2026-10-02 **Change of base: nfsc-recomp's renderer adopted** (fh1/src/native, see its README).
  The user's friend (GoatHonks) ported the whole nfsmw-nx native renderer to Need for Speed: Carbon
  (nfsc-recomp, same 2008 shader containers as FH1, hookless vertex-shader identification, AMD
  barrier fix, positional interpolants, shaders rebuilt from microcode). With his permission it is
  copied in and built next to our own N1-N3a code; run with `--nfsmw_renderizador=nativo` and the
  library next to fh1.exe as `nfsmw_shaders.nfsp`. FH1 fixes on top: microcode located through the
  2008 shader header (FH1 puts data first in the physical part; without it only 3 of 9 shaders
  matched), our tolerant vertex-shader pass, k_2_10_10_10 resolves, 16/32-bit render-target
  formats, shared constants widened for fh1Fetch. **First native picture**: the FORZA HORIZON logo
  and PRESS START drawn with textures (title video background still white), 30 fps, no crash.
  Missing shaders: `--nfsc_dump_ring_shaders=DIR` writes unknown microcode, `tools/nfsc_synth_containers.py`
  rebuilds containers, our translator now names interpolants by position (as the hardware links
  them) so rebuilt and real shaders link.
