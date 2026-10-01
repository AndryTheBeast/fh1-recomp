# Roadmap

Forza Horizon (Xbox 360) → PC by static recompilation (ReXGlue), then the Nintendo Switch in a
separate repository. Details, addresses and dead ends of everything below: `docs/roadmap-history.md`.
Where the frame time goes: `docs/performance-review.md`.

## Where we are (2026-10-02)

**Playable on PC from start to finish of a session**: boot, festival, free roam, races, garage,
buying cars, paint shop, saving. Tested on a Lenovo Legion Go (Ryzen Z1 Extreme): **30 fps** (the
game's own cap) in normal play, 26-28 in the busiest spots. Two graphics paths, both correct:

| path | how to run | state |
| --- | --- | --- |
| D3D12 (default) | `run_fh1.bat` | correct picture, 30 fps; F3 frame monitor |
| Vulkan | `run_fh1.bat --gpu_backend=vulkan` | correct picture since 2026-10-01; 28-30 fps, short hitches while shaders are first prepared |

Known small issues: a soft rectangle under the car (probably the motion-blur mask, same on both
paths - compare with console footage); ~6 tessellated draws per frame fail harmlessly.

## Done

**Stage 1 — Translate the game.** `default.xex` + XMediaFacade/SpeechFacade translated; code gaps,
jump tables, tail calls and fibers declared (`fh1/overrides.toml`, gap tools in `tools/`).

**Stage 2 — Run it on Windows with the emulated Xbox 360 GPU.** Fixed along the way: thunk-pool
crash at the intro video, uninitialized stacks (0xBE) crashing save loading, missing `cache:`
device crashing events, cars passing through each other (vmsum3fp128 overflow semantics), paint
shop crash (setjmp/longjmp), garage light bloom and garbage car photos (CPU readback of resolves),
controller merging (DS4Windows). Speed work on the emulation: bulk register writes, shadow buffers
drawn into their 1x alias, clears instead of EDRAM copies (EDRAM GPU time 6.5 → ~3.7 ms/frame).

**Stage 3 groundwork.** Vulkan backend built on Windows and fixed (black world = wrong texture
exponent word in the SPIR-V translator). Tools: frame census of the game's Direct3D, call graph,
RenderDoc dumps (`tools/rdc_*.py`), sampling profiler (`--fh1_profile`), render-target frame log
(`--gpu_log_rt_frame`), automatic test runs with screenshots (`tools/auto_test.ps1`).

**Tried and dropped:** single-pass drawing inside the EDRAM emulation (picture broke - belongs in
the native renderer); 60 fps unlock with vsync off (broke distant rendering; revisit when much
faster); forcing 1x MSAA (garbage on the car).

## Next — performance (from `docs/performance-review.md`)

1. [x] **Read back only what the CPU reads** (2026-10-02, `readback_resolve_skip_steady`): big
       destinations resolved every frame are no longer copied back; resolve GPU time 5.0 -> 4.2 ms.
       To verify by the user: a car photo after a repaint still saves correctly.
2. [x] **Visibility queries checked**: the emulator answers them without GPU work, so the slow
       depth draws in the replay timings were a measurement artifact. Their log lines (8 per
       frame) and other per-frame log spam are now logged a few times only.
3. [ ] **Stop the game's render thread spinning** while it waits for the GPU thread (half a core;
       a whole core on the Switch): hook the ring waits (`sub_829F04A8`, `sub_823E91F0`).
4. [ ] Vulkan: frame monitor (F3) for the Vulkan presenter; persistent pipeline cache to cut hitches.

## Next — better test autoplay (user's request, 2026-10-02)

Tests today hold buttons on a fixed timetable (`--fh1_autoplay`, fh1/src/fh1_autoplay.h).
1. [ ] Scripts: analog sticks/triggers, sequences, waits, named macros (open map, garage, start the
       nearest event, skip cutscene).
2. [ ] Game state from memory (menu open, loading, race running, car speed/position) so scripts
       wait for conditions instead of fixed seconds.
3. [ ] Driving and racing: first try handing the player's car to the game's own AI driver (used by
       opponents and the attract-mode demo); otherwise steer along the GPS route.

## Next — native renderer (Stage 3, Vulkan)

The emulated GPU spends most of its time on emulation itself (EDRAM, drawing the main scene 3
times for the console's tiling, transfers, readbacks). The native renderer keeps the parts proven
on FH1 (command processing, SPIR-V shader translation, textures) and replaces the rest:

1. [ ] **Native render targets in the Vulkan backend**: one image per render target, no EDRAM
       aliasing or ownership transfers (keep the one aliasing FH1 needs: 4x at pitch P = 1x at 2P),
       resolves as direct copies into textures.
2. [ ] **Draw the main scene once** instead of 3 strips (~2,000 fewer draws per frame, ~-40% CPU on
       the GPU thread) - possible once render targets are not limited to the 10 MB EDRAM.
3. [ ] Readbacks only where the CPU needs them; compare every change against D3D12 screenshots.
4. [ ] Per-draw cost of the GPU command thread (driver ~30%, emulation bookkeeping the rest).

Milestone: festival and a race correct on Vulkan, faster than D3D12 today.

## Later — Nintendo Switch (separate repository)

Started from this repo once PC is done. The Tegra X1 has ~20-50x less GPU and much slower CPU
cores than the Legion Go, so it needs the native renderer, pre-translated shaders (XenosRecomp,
as nfsmw-nx does), no CPU spinning, audio decoding on a worker, and probably lower handheld
settings (shadow/reflection resolution, cheaper anti-aliasing).

## Maybe later

- Source-only pre-release (tag + install guide + known issues; users build from their own disc).
- 60 fps unlock (needs a much faster renderer, and checking the game's timing at 60).
