# Roadmap

Forza Horizon (Xbox 360) → PC by static recompilation (ReXGlue), then the Nintendo Switch in a
separate repository. Detailed history, addresses and dead ends: `docs/roadmap-history.md`.
Where the frame time goes: `docs/performance-review.md`.

## Where we are (2026-10-02)

**Playable on PC**: boot, festival, free roam, races, garage, buying and repainting cars (photos
correct), saving. Lenovo Legion Go (Ryzen Z1 Extreme): **30 fps** (the game's own cap) in normal
play, 26-28 in the busiest spots.

| graphics path | how to run | state |
| --- | --- | --- |
| D3D12 (default) | `run_fh1.bat` | correct, 30 fps; F3 frame monitor |
| Vulkan | `run_fh1.bat --gpu_backend=vulkan` | correct, 28-30 fps; short hitches while shaders are first prepared |

Known small issues: a soft rectangle under the car (probably the motion-blur mask, same on both
paths); ~6 tessellated draws per frame fail harmlessly.

## Done

- **Translation (Stage 1)**: `default.xex` + XMediaFacade/SpeechFacade; code gaps, jump tables, tail
  calls and fibers declared (`fh1/overrides.toml`, gap tools in `tools/`).
- **Running on Windows with the emulated Xbox 360 GPU (Stage 2)**: crashes fixed (intro video,
  save loading, events, paint shop); cars no longer pass through each other (vmsum3fp128); garage
  bloom and car photos (CPU readback of resolves, with the readback default finally applied);
  controller merging.
- **Emulation speed**: bulk register writes; shadow buffers drawn into their 1x alias; clears instead
  of EDRAM copies (EDRAM GPU time 6.5 → ~3.7 ms/frame); only CPU-read resolves copied back; game
  render thread yields instead of spinning (99% → 56% busy); per-frame log spam removed.
- **Vulkan**: built on Windows and correct (black world = wrong texture exponent word in the SPIR-V
  translator).
- **Tools**: F3 frame monitor, sampling profiler (`--fh1_profile`), render-target frame log
  (`--gpu_log_rt_frame`), Direct3D census, call graph, RenderDoc scripts (`tools/rdc_*.py`),
  unattended test runs with screenshots (`tools/auto_test.ps1 -Autoplay`, boot with the user's
  save: `33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a`).
  Driving and race tests: the user plays manually.

**Tried and dropped**: single-pass drawing inside the EDRAM emulation (picture broke - belongs in
the native renderer); 60 fps unlock with vsync off (broke distant rendering); forcing 1x MSAA
(garbage on the car); scripted autoplay with a memory scanner, input recorder and route replay
(removed at the user's request; git history ffdb286..e8b043f).

## Next — native renderer (Stage 3), the nfsmw-nx way

Decision 2026-10-02: build a real native renderer like nfsmw-nx did for Most Wanted instead of
polishing the emulated GPU (plan and reuse table: `docs/native-renderer-fh1.md`).

- [x] **N0 shader library** (2026-10-02): `tools/build_shader_library.ps1` builds
      `fh1_shaders.nfsp` with 3,816 of 3,849 shaders (loose files, the tracks' LZX archives -
      now decodable - and the engine's shaders in default.xex).
- [ ] **N1 own graphics system** (`fh1_renderer=native`): presenter, command ring, vblank, GPU
      waits - the game runs without the emulated GPU.
- [ ] **N2 shader identity hooks** (FH1's D3D shader constructors and Draw*).
- [ ] **N3 draws and textures**: logos, title, menus.
- [ ] **N4 render targets and resolves**: main scene drawn once, MSAA, aliases, readbacks -
      festival like D3D12.
- [ ] **N5 races, garage, photos; performance.**

Paused experiments on the emulated GPU (off by default, `docs/native-render-targets.md`): EDRAM
transfer skipping (16 kinds needed while driving) and the tall single-pass main scene (15.0 -> 9.4
ms of draw time, but motion-blur smear and missing structures).

Small items alongside: F3 frame monitor for the Vulkan presenter; persistent Vulkan pipeline cache.

## Later — Nintendo Switch (separate repository)

Started from this repo once PC is done. The Tegra X1 has ~20-50x less GPU and much slower CPU
cores than the Legion Go, so it needs the native renderer, pre-translated shaders (XenosRecomp, as
nfsmw-nx does), no CPU spinning (block on the ring instead of yielding), audio decoding on a
worker, and probably lower handheld settings (shadow/reflection resolution, cheaper anti-aliasing).

## Maybe later

- Source-only pre-release (tag + install guide + known issues; users build from their own disc).
- 60 fps unlock (needs a much faster renderer, and checking the game's timing at 60).
