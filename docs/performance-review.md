# Performance review (2026-10-01) — where FH1's frame time goes, and what to cut for the Switch

Measured on the Legion Go (Ryzen Z1 Extreme: 8 Zen 4 cores, RDNA 3 iGPU ~8.6 TFLOPS), D3D12 RTV,
festival, car parked in a busy spot, nobody touching the pad. Sources: the `[fps]` lines (GPU-thread
breakdown and host GPU timestamps), `--fh1_profile=30` (sampling profiler of every thread,
test-review-20261001-232*), and a Vulkan RenderDoc capture timed per draw with
`tools/rdc_timing.py` (replay timings are inflated ~4x because every draw is measured on its own;
use them as proportions).

## 1. The frame today

| | per frame | notes |
| --- | --- | --- |
| frame rate | 27.6 fps (30 cap) | 28-30 in quieter spots |
| draws issued | ~3,900 | ~3,700 of them predicated: the main scene is replayed for 3 strips |
| GPU command thread (CPU) | 100% busy: 18.7 ms in draws, 4.2 ms waiting for vblank/memory, 3.0 ms presenting | the limiter on the CPU side |
| host GPU | 29.6 ms | draws 13.4, resolves 4.7, primitives 3.4, EDRAM transfers 2.8, textures 2.6, uploads 2.6 |

CPU threads (profiler, % of wall time):

| thread | busy | what |
| --- | --- | --- |
| game render thread | 98% | **about half of it spinning** in `sub_829F04A8` / `sub_823E91F0` = waiting for the GPU command thread (ring space, read pointer) |
| GPU command thread | 88% | ~30% inside the D3D12 driver (per-draw overhead), the rest emulation: packet parsing, `UpdateBindings`, texture/sampler lookups, render-target cache, endian-swapping uploads (`copy_and_swap_32`, 3.5%) |
| audio thread | ~20% real | XMA decoding (FFmpeg `imdct`) + the game's audio mixer (`sub_82FB...`, `sub_8300...`) |
| game logic thread | ~33% | physics, AI (spread out, no single hot spot) |

GPU work by pass (Vulkan replay, proportions of the frame):

| share | pass |
| --- | --- |
| ~40% | main scene, 1280x720 4x MSAA, drawn **3 times** (one per strip) |
| ~16% | early depth-only pass at 1280x720 with the game's visibility queries: 37 tiny draws (7-315 indices) each timed ~0.5 ms - suspicious, see 2.4 |
| ~10% | shadow cascades (1024², 520² depth) |
| ~8% | reflection cube map (6 faces 256² 2x MSAA + mips) |
| rest | post-processing (bloom chain, exposure, tone map), HUD, transfers, resolves |

## 2. What to cut, ranked by gain per effort

### 2.1 Draw the main scene once instead of three times — biggest win (big job)
The predicated tiling replays ~1,100 draws three times. One pass would remove roughly 2,000 draws
per frame: about **-40% CPU time on the GPU thread and -25..35% host GPU**. The quick version
(collapsing tiling inside the EDRAM emulation, `--fh1_single_tile`) was tried and removed - the 4x
720p frame does not fit the 10 MB EDRAM and the workarounds broke the picture. It belongs in the
native renderer, whose render targets have no EDRAM size limit (ROADMAP Stage 3).

### 2.2 Stop the game render thread spinning (small job, important on Switch)
Half of a core is burned in a busy-wait loop for the GPU thread. On the PC that is free; on the
Switch (3 usable 1 GHz A57 cores) it steals a whole core from the GPU command thread or the
game. Hook `sub_829F04A8` / `sub_823E91F0` to block on an event signalled when the ring read
pointer moves (nfsmw-nx did the same for its ring waits).

### 2.3 Only read back what the CPU reads (small job, ~1-3 ms GPU + CPU memcpy)
`readback_resolve=fast` copies **every** resolve back to CPU memory: main scene (3.7 MB), depth
strips, shadow maps (2 MB each)... The CPU only needs the auto-exposure value (4 KB) and the car
photos (884 KB). Skip readback for big per-frame destinations (or keep a per-destination "CPU
reads it" flag learned from page-fault watches). Measured cost of fast vs none today: ~1-2 fps in a
busy spot, ~3 ms of resolve GPU time.

### 2.4 Check the visibility-query depth pass (small job to investigate)
37 depth-only draws of a few triangles each, interleaved with the game's occlusion ("viz")
queries (`Begin viz query ID 3C..3F` in the Vulkan log, every frame), cost a sixth of the frame in
the replay timings. If each query forces a pipeline flush or a host query round trip, batching or
answering the queries asynchronously (the game only needs them a frame later) could save several
ms. Verify with real timestamps first.

### 2.5 Per-draw CPU cost in the GPU command thread (medium job)
~30% of the thread is the D3D12 driver; the rest is per-draw emulation work. Gains: skip redundant
root-parameter/descriptor updates, cache texture/sampler lookups per draw state, larger
command-list batches, avoid re-hashing unchanged shader/pipeline state. Each is a few %, together
maybe -20% thread time. On the Switch the Vulkan/NVN driver overhead differs; the emulation part
carries over.

### 2.6 Remaining EDRAM transfers (2.8-4 ms GPU) — native renderer
Already cut from ~7 to ~3 ms today (shadow buffer alias, clear instead of transfer). What is left
carries real data (4x/1x aliasing, depth read as colour) and disappears with native render targets.

### 2.7 Audio (small job)
XMA decoding runs on the calling thread (`[xma] split (mode 0)`). Fine on 8 Zen 4 cores; on the
Switch move it to a worker and keep the mixer thread light.

### 2.8 Texture uploads
Endian swapping on the CPU (`copy_and_swap_32`, 3.5% of the GPU thread) - do it in the GPU load
shaders, which already exist for most formats.

## 3. What the Switch means

The Tegra X1 has ~0.4 TFLOPS docked and ~0.16 handheld, against ~8.6 on the Z1 Extreme: **20-50x
less GPU**, and 3-4 A57 cores at 1-1.8 GHz against 8 Zen 4 cores at up to 5 GHz (~5-8x less per
core). Today the frame needs ~25-30 ms of a GPU that is 35x faster than the Xbox 360's (the Xbox ran
this at 30 fps on 0.24 TFLOPS), so almost all of today's GPU time is **emulation overhead**: EDRAM
emulation, 3x tiling, generic translated shaders, transfers, readbacks.

So the Switch port is only realistic with:
1. the native renderer (no EDRAM, one pass, resolves as direct copies),
2. pre-translated and optimized shaders (XenosRecomp, as nfsmw-nx does on the Switch),
3. no CPU spinning and a light GPU command thread (2.2, 2.5),
4. probably reduced settings in handheld mode: shadow map size, reflection cube map size/rate,
   MSAA 4x -> 2x or a cheaper AA, and maybe a lower internal resolution (the game's own 1280x720
   is already modest).

## 4. Suggested order

1. 2.3 readback filter and 2.4 viz-query check (days) - immediate PC gains, low risk.
2. 2.2 ring-wait hook (days) - CPU relief, prerequisite for the Switch.
3. Native render target cache in the Vulkan backend (weeks) - enables 2.1 and 2.6.
4. 2.5 per-draw cost, 2.7 audio worker, 2.8 GPU swaps (as the profiler points).
