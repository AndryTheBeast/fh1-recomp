# Roadmap

Following the order in `docs/porting-another-game.md`, which worked for nfsmw-nx.

## Stage 1 — Translate the game on PC
- [x] Build the ReXGlue code generator (`rexglue`)
- [ ] Extract `default.xex` (NTSC-U, with title update if available) into `fh1/assets/game_root`
- [ ] `rexglue init` the `fh1/` project and run the first codegen
- [ ] Fix what the translation misses: missing functions, jump tables, split functions (`tools/huecos.py`, `fh1/overrides.toml`)

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
