# Roadmap

Following the order in `docs/porting-another-game.md`, which worked for nfsmw-nx.

## Stage 1 — Translate the game on PC
- [x] Build the ReXGlue code generator (`rexglue`)
- [x] Extract `default.xex` (NTSC-U, Title ID 4D5309C9, v0.0.0.10) with `tools/extract_xex.bat`
- [x] `rexglue init` the `fh1/` project and run the first codegen (passes: 16 tail-call targets declared in `fh1/overrides.toml`)
- [ ] Fix what the translation misses: missing functions, jump tables, split functions (`tools/huecos.py`, `fh1/overrides.toml`)
  - [x] `0x8241A370`: checked, one genuine 110 KB float-math function (only its C++ file is big)
  - [x] `bdz` switch at `0x82AD80D0` ended one instruction early (`overrides.toml`)
  - [x] `0x830ED900` ran past `KeBugCheck` into the next function (`overrides.toml`)
  - [x] Code gaps: 796 found; 191 are data (`fh1/huecos_excluir.txt`), 115 extra this-adjusting
        thunks split out, 58 truncated functions extended (`tools/fusionar_continuaciones.py`).
        Codegen is clean: no unresolved branches, no `REX_FATAL` stubs. 321 gaps left, mostly data.

## Stage 2 — Boot on Windows with ReXGlue's own graphics
- [ ] Build the Windows runtime (D3D12 backend; GTX 1050 target)
- [ ] Reach the first frame, then menus, then a drive
- [ ] Log kernel/XAM calls the game needs that ReXGlue lacks (Kinect, Xbox Live, content/DLC paths)

## Stage 3 — Move to the Switch system
- [ ] Build with devkitA64 + the Horizon layer in `sdk/`
- [ ] Memory map, threads, audio out

## Stage 4 — Native renderer and pre-translated shaders
- [ ] D3D tracing pass to find FH1's render functions (pattern: `reference/nfsmw-app/src/nfsmw_d3d_trace.cpp`)
- [ ] Port the ring-thread renderer from `reference/nfsmw-app/src/nfsmw_nativo_*`
- [ ] Shader library via XenosRecomp

## Stage 5 — Performance on the console
