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

## Next — native renderer (Stage 3, Vulkan)

The emulated GPU spends most of its time on emulation itself: EDRAM, drawing the main scene 3
times for the console's tiling, transfers, readbacks. The native renderer keeps the parts proven on
FH1 (command processing, SPIR-V shader translation, textures) and replaces the rest:

1. [~] **Native render targets in the Vulkan backend**: one image per render target, no EDRAM
       aliasing or ownership transfers (keep the one aliasing FH1 needs: 4x at pitch P = 1x at 2P),
       resolves as direct copies into textures. 2026-10-02: of 104 kinds of EDRAM copies the
       festival needs exactly one (`c0.720.16.4>c0.720.16.1`, the 4x/1x colour alias); the depth
       alias trick now works on Vulkan too. Details: `docs/native-render-targets.md`.
2. [~] **Draw the main scene once** instead of 3 strips (~2,000 fewer draws per frame, ~-40% CPU on
       the GPU thread) - possible once render targets are not limited to the 10 MB EDRAM.
       Prototype `--gpu_tall_main_pass` (off by default): one seamless full-height pass, GPU
       thread draw time 15.0 -> 9.4 ms, but objects the game culls per strip are missing. Next:
       the game's own single-strip setting (old `--fh1_single_tile` hook) + per-row copy-outs.
3. [ ] Readbacks only where the CPU needs them; compare every change against D3D12 screenshots.
4. [ ] Per-draw cost of the GPU command thread (driver ~30%, emulation bookkeeping the rest).

Milestone: festival and a race correct on Vulkan, faster than D3D12 today.

Small items alongside: F3 frame monitor for the Vulkan presenter; persistent Vulkan pipeline cache
(fewer first-time hitches).

## Later — Nintendo Switch (separate repository)

Started from this repo once PC is done. The Tegra X1 has ~20-50x less GPU and much slower CPU
cores than the Legion Go, so it needs the native renderer, pre-translated shaders (XenosRecomp, as
nfsmw-nx does), no CPU spinning (block on the ring instead of yielding), audio decoding on a
worker, and probably lower handheld settings (shadow/reflection resolution, cheaper anti-aliasing).

## Maybe later

- Source-only pre-release (tag + install guide + known issues; users build from their own disc).
- 60 fps unlock (needs a much faster renderer, and checking the game's timing at 60).
