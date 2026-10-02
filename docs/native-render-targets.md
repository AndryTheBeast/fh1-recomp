# Native render targets (Stage 3, step 1-2) - findings and plan

Working notes for the native renderer's first steps in the Vulkan backend. Plain summary first,
details below. Started 2026-10-02.

## In short

- Today every render target lives inside an imitation of the Xbox 360's 10 MB EDRAM. When the game
  reuses a piece of EDRAM with another format or size, the emulator **copies** ("ownership
  transfer") the old content into the new render target, in case the game reads it.
- **Experiment (2026-10-02): skipping every one of those copies still gives an almost correct
  picture.** Only far scenery (mountains, the festival dome) turns grey and foggy. So FH1 relies on
  very few of the ~100 kinds of copies; the rest are wasted work.
- Skipping them saves about 2 ms per frame on the GPU command thread (draws 15.3 / 14.7 / 15.5 ms
  -> 12.9 / 12.6 / 13.8 ms per frame in the festival, Vulkan, Legion Go). The game stays at its
  30 fps cap either way, so the gain shows up as free time (more `WAIT_REG_MEM` idle).
- A native render-target cache (one Vulkan image per render target, no EDRAM) only has to reproduce
  the few copies the game really needs - see "Needed transfers" below.
- The 10 MB limit is also what forces the 3 strips: at 4x MSAA and 1280 pixels wide, a render
  target can be at most 512 rows tall in the EDRAM model (the 11-bit EDRAM addressing period,
  `RenderTargetCache::GetRenderTargetHeight`), and the frame is 720 rows. Native images have no
  such limit, which is what makes "draw the scene once" possible.

## Tools added

| setting / tool | what it does |
| --- | --- |
| `--gpu_skip_all_transfers=true` | skip every EDRAM ownership transfer (render targets start with whatever they had) |
| `--gpu_keep_transfers=SIG,SIG,...` | with the above: still perform these kinds of transfers |
| `--gpu_log_transfer_kinds=true` | log each new kind of transfer once, with its frame number |
| `--gpu_log_rt_frame=N` | (existing) every render-target setup, transfer and resolve of frame N; transfers now carry `sig` |
| `tools/img_diff.py` | fog check on the 80-88 s festival shot: dome "warmth" (red minus blue), ~55-80 correct, below 0 foggy |
| `tools/bisect_transfers.py LOG` | runs the game repeatedly, dropping halves of the kept transfer kinds, until only the needed ones remain |

A transfer signature reads `SOURCE>DEST`, each `{c|d}FORMAT.BASE.PITCH.MSAA`: colour or depth,
the guest format number, the EDRAM base tile, the pitch in 32bpp tiles (80 samples wide), and the
MSAA sample count. Example: `d1.720.16.1>c3.0.32.4` = 1x depth (D24FS8) at tile 720, pitch 1280
pixels, copied into a 4x colour target (2_10_10_10_FLOAT) at tile 0, pitch 32 tiles (1280 pixels
at 4x).

## What a festival frame does (frame 2000, Vulkan)

Main scene, 3 strips of 1280x256/256/208 at 4x MSAA: colour `c3.0.32.4` (2_10_10_10_FLOAT), depth
`d1.1024.32.4` (D24FS8). Each strip ends with a depth resolve and a colour resolve to its own
buffer (e.g. 0x1DAC5000, 0x1DC05000, 0x1DD45000 for depth: 1280x256 each).

Then post-processing at 1x: a 640-wide 4x depth target `d0.0.16.4` sharing the bytes of the
1280-wide 1x colour `c3.0.16.1` (the "4x at pitch P = 1x at pitch 2P" alias), a downsample chain
(320x192, 160x96, 80x48, 64, 32 ...), a 32x32 `k_32_FLOAT` target (`c14.0.1.1`, luminance for the
auto exposure), three 320x180 targets (2x RGBA16F + RGBA8, MRT), and the final 1280x720 targets
(`c2.0.16.1`, `c1.0.16.1` = 8_8_8_8).

Predicated tiling: the game's Direct3D does not send the scene 3 times. It records it once and
the command stream is replayed per strip: the bin select is set to 80000003, then C, then 30 (5
bin-select sets per frame), and each draw carries a bin mask of the strips its screen extent
touches (3, C, 30, or FFFFFFFF for draws not tiled). ~3,700 predicated draws per frame, ~3,100 of
them run. Drawing once = run the first replay with every strip selected into a full 1280x720
target, skip the draws of the other two replays, and serve each strip's resolve from the matching
rows of the full target.

## Needed transfers

(Filled in from the bisection run below.)

## Plan for the native render-target cache (Vulkan)

1. **A cvar to pick it**: `render_target_path_vulkan=native` (default stays the current path until
   the native one matches the D3D12 screenshots).
2. **Images keyed by what the game draws, not by EDRAM**: one image per (base, pitch, MSAA,
   format) as today, but sized by what is drawn (up to 8192), not by the EDRAM period. Keep the
   one alias FH1 needs: 4x at pitch P and 1x at pitch 2P at the same base are the same image
   (already done for depth-only passes by `gpu_msaa_depth_as_1x`).
3. **No ownership transfers** except the kinds listed under "Needed transfers", done as direct
   image-to-image draws (they already exist as transfer shaders; only the decision changes).
4. **Resolves straight from the image**: today a resolve first dumps the render target into the
   EDRAM buffer, then a compute shader writes the guest texture. The native path reads the image
   in the resolve shader directly (saves the dump pass per resolve).
5. **Draw once** (roadmap step 2): with images taller than 512 rows, run the strip replays as one
   pass (see above). Resolves keep their per-strip destinations.
6. Check every step against D3D12 screenshots (festival start, a race) and the `[fps]` lines.
