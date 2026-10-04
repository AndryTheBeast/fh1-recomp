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

Bisection on the festival start view (2026-10-02, `tools/bisect_transfers.py`, 15 runs, log
`build_logs/bisect-transfers.txt`): of 104 transfer kinds, **only one is needed**:

    c0.720.16.4>c0.720.16.1

An 8_8_8_8 colour target at EDRAM tile 720 drawn at 4x MSAA with a 640-pixel pitch, then read as a
1x target with a 1280-pixel pitch: the "4x at pitch P = 1x at pitch 2P" alias, this time for
colour (the depth version is what `gpu_msaa_depth_as_1x` handles). Without it, far scenery turns
grey (it feeds the fog / atmosphere). Every other transfer can be skipped there:

    --gpu_skip_all_transfers=true --gpu_keep_transfers=c0.720.16.4>c0.720.16.1

gives the same picture as normal (dome warmth 81 vs 82). Only the festival start was checked:
garage, car photos, races and menus still need checking before this can become a default.
Native render targets therefore need just one alias rule (4x/P = 1x/2P, colour and depth) and no
general EDRAM emulation.

`gpu_msaa_depth_as_1x` (draw 4x depth-only passes straight into their 1x alias at double
resolution) now also works on Vulkan (it was D3D12-only); on by default like on D3D12, festival
picture correct.

## Draw once ("tall main pass") - first prototype

`--gpu_tall_main_pass=true` (Vulkan, off by default; use with the two transfer settings above):

- render targets of the tiled pass (4x MSAA, pitch `gpu_tall_pass_width`=1280) are created
  `gpu_tall_pass_height`=720 rows tall instead of the EDRAM's 512;
- each main-pass draw runs once, at full height (window offset 0, scissor to row 720), in the
  first strip its bin mask names (3 / C / 30); the copies in later strips are skipped;
- each strip's resolve and clear read their own rows of the tall target
  (`g_tall_resolve_row_offset` = minus the strip's window offset; the resolve dump shifts its
  source with the base/first-tile trick described in vulkan/render_target_cache.cpp, the clear
  moves its rectangle; EDRAM ownership stays in EDRAM terms).

Results (festival start, Vulkan, Legion Go, `[fps]` lines):

| mode | draws run per frame | GPU thread time in draws | picture |
| --- | --- | --- | --- |
| normal | ~3,700 | 15.0 ms | correct |
| skip transfers (keep 1) | ~3,800 | 14.3 ms | correct |
| + tall main pass | ~2,300 | 9.4 ms | **incomplete**: big structures missing (dome, stage towers, garage arch) |

The 3 strips join into one correct, seamless picture (resolves and clears from the tall target
work). What is missing: objects the game's CPU side culls per strip. FH1 replays its recorded
command lists once per strip (walker sub_829F5FF0) and leaves out objects outside each strip's
view, so an object visible only in strip 2 appears only in strip 2's commands, often with the
all-strips mask FFFFFFFF - the GPU side cannot tell it from a duplicate.

### Single strip from the game (done 2026-10-02 ~06:10)

`--fh1_single_tile=true --gpu_tall_single_strip=true` (with `--gpu_tall_main_pass=true` and the
two transfer settings; all off by default). The old hook (git c44b37b) is back in
fh1/src/fh1_trace_load.cpp: the game is told there is one strip of 1280x720, so it emits the
main pass once (bin select 80000003 only, 2,231 draws, all run). Then:

- every main-pass draw runs in that one pass (select |= 3C), at full height;
- the copy-outs keep their rows: colour comes as 3 copy-outs (rows 0-256, 256-512, 512-720, all
  to the first destination - the destination follows the rows), depth as ONE copy-out of rows
  0-720, more than the EDRAM holds. Each copy-out is now issued in 256-row pieces (scissor), and
  draw_util::GetResolveInfo turns rows below 0 into `g_tall_resolve_row_offset` (source rows of
  the tall target) while the destination keeps its row. Without the split, depth below row 512
  was garbage and the motion blur smeared the bottom of the picture.

| mode | draws per frame | GPU thread in draws | picture |
| --- | --- | --- | --- |
| normal Vulkan | ~3,700 | 15.0 ms | correct |
| single strip + tall pass | ~2,230 | 9.4 ms | clean, seamless; festival structures missing |

**Open problem**: the dome, stage towers, speaker stacks, garage arch and far tents are missing
in every tall variant (3 strips or 1), but present with the transfer settings alone. So it is
not per-strip culling. Leaving the transfers on with the tall pass breaks the whole picture
(transfers between the overlapping tall targets), so that does not help to find it.
Next: RenderDoc captures of `--gpu_skip_all_transfers ... ` (correct) and of the single-strip
tall pass at the same moment (tools/rdc_find.py / rdc_inputs.py), find the dome's draw in the
correct one and see what happens to it in the tall one (not issued? depth/stencil test? drawn
into another target?). Candidates: the 80000000-mask draws, a pass with all-ones bin select that
uses the main targets, or depth/stencil state the strips' clears provided.

Test shots: build_logs/test-tC3-* (3 strips, tall), test-tS3-* (single strip, tall),
test-tB-* (transfers only, correct).

## User driving tests (2026-10-02 morning)

The parked-festival bisection was not enough: while driving, 15 more transfer kinds appear
(`--gpu_log_transfer_kinds`, run-20261002-084045-kinds-drive.log), and skipping them turns the
picture teal/glowing (fog/light passes reading the wrong data). With these 16 kept the user's
drive (festival -> road, ~100 mph) looked normal:

    c0.720.16.4>c0.720.16.1,c0.0.1.1>d1.0.16.1,c0.0.16.1>c3.0.32.4,c0.0.16.1>c3.0.4.2,c0.0.16.1>d1.128.4.2,c2.0.16.1>d1.0.16.1,c3.0.16.1>d1.0.16.1,d0.720.13.1>d1.0.16.1,d0.720.16.4>c0.720.16.1,d0.720.16.4>d1.0.16.1,d0.720.16.4>d1.0.16.4,d1.0.16.1>c0.0.16.1,d1.0.16.1>c0.720.16.4,d1.0.16.1>d0.720.16.4,d1.4.1.1>d1.0.16.1,d1.720.16.1>d1.0.16.1

(list also in build_logs/keep-driving.txt). Races, garage and menus not checked yet.

Single-strip tall pass + these 16 (run-20261002-084534-tall-keep16.log): parked picture clean,
but after ~10 s of driving the motion blur smears the screen (blocky trails, dark car), and the
festival structures are still missing. Both are bugs of the tall pass itself, not of the skipped
copies. Next: RenderDoc captures while driving, normal vs tall pass, and compare the motion blur
inputs (depth / velocity / previous-frame textures) and the dome's draws.
