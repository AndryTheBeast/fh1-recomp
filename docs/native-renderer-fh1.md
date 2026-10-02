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
- **N3 - draws and textures**: first frames - logos, title, menus.
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
