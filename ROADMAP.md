# Roadmap

Forza Horizon (Xbox 360) → PC by static recompilation (ReXGlue), then the Nintendo Switch in a
separate repository. Detailed history, addresses and dead ends: `docs/history/roadmap-history.md`.

## Where we are (2026-10-04)

**Playable on PC**: boot, festival, free roam, races, garage, buying and repainting cars (photos
correct), saving. Lenovo Legion Go (Ryzen Z1 Extreme): **30 fps** (the game's own cap) in normal
play, 26-28 in the busiest spots.

| graphics path | how to run | state |
| --- | --- | --- |
| Emulated Xbox 360 GPU, D3D12 (default) | `run_fh1.bat` | correct, 30 fps; F3 frame monitor |
| Emulated Xbox 360 GPU, Vulkan | `run_fh1.bat --gpu_backend=vulkan` | correct, 28-30 fps |
| **Native renderer** (Vulkan, in progress) | `run_fh1.bat --fh1_renderer=native` | boot, videos, menus, loading screens correct; festival and driving run at 30 fps with picture glitches |

## Now — native renderer (Stage 3)

The renderer that draws the game directly with Vulkan, without emulating the Xbox 360 GPU. It is
what the Switch needs. State, open problems and how to test: **`docs/native-renderer-status.md`**.

- [x] **N0 shader library**: 3,849 shaders pre-translated (`fh1_shaders.nfsp`).
- [x] **N1 own graphics system**: the game runs without the emulated GPU.
- [x] **N2 shader identity**: the shaders the game uploads are matched to the library (missing ones
      are added from ring dumps).
- [x] **N3 draws and textures**: boot, videos, title, menus, loading screens, map - same picture as
      the emulated GPU (2026-10-04).
- [ ] **N4 the 3D scene** — next, in this order:
  1. smear over the scene and speckled edges (festival, driving)
  2. packed vertex positions (vegetation, billboards: 14 % of draws missing)
  3. brightness against the emulated picture, glare
  4. 2x MSAA reflections; frame drops while driving
  5. green car in the evening
- [ ] **N5 races, garage, photos; performance.**

## Done before

- **Translation (Stage 1)**: `default.xex` + XMediaFacade/SpeechFacade; code gaps, jump tables, tail
  calls and fibers declared (`fh1/overrides.toml`, gap tools in `tools/`).
- **Running on Windows with the emulated Xbox 360 GPU (Stage 2)**: crashes fixed (intro video,
  save loading, events, paint shop); cars no longer pass through each other (vmsum3fp128); garage
  bloom and car photos; controller merging.
- **Emulation speed**: bulk register writes; shadow buffers drawn into their 1x alias; clears instead
  of EDRAM copies; only CPU-read resolves copied back; game render thread yields instead of spinning.
- **Vulkan backend of the emulated GPU**: built on Windows and correct.
- **Tools**: F3 frame monitor, sampling profiler (`--fh1_profile`), render-target frame log,
  call graph, RenderDoc scripts, unattended test runs with screenshots (`tools/auto_test.ps1`).

**Tried and dropped**: single-pass drawing inside the EDRAM emulation; 60 fps unlock with vsync off
(broke distant rendering); forcing 1x MSAA; scripted autoplay with route replay (driving tests are
done by hand). Paused experiments on the emulated GPU: `docs/history/native-render-targets.md`.

## Later — Nintendo Switch (separate repository)

Started from this repo once PC is done. The Tegra X1 has ~20-50x less GPU and much slower CPU
cores than the Legion Go, so it needs the native renderer, the pre-translated shaders, no CPU
spinning, audio decoding on a worker, and probably lower handheld settings. What costs time on PC
today: `docs/performance-review.md`. The Switch platform itself: `docs/nfsmw-nx/`.

## Maybe later

- Source-only pre-release (tag + install guide + known issues; users build from their own disc).
- 60 fps unlock (needs a much faster renderer, and checking the game's timing at 60).
