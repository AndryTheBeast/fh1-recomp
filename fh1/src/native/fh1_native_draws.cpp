// fh1 - native renderer, steps C3, C4, C5c and C6 (see fh1_native_draws.h).
//
// What it covers
//   - Shaders: modules of the NFSSPV library with the XenosRecomp interface
//     (shader_common.h and shader_recompiler.cpp): push constants with three
//     buffer addresses (VS, PS and shared constants), 2D, 3D and cubemap
//     textures in sets 0-2 and samplers in set 3, with no size limit;
//     specialization constant 0 with R11G11B10 normals (bit 0) and alpha test (bit 1).
//   - Vertices: the input comes from the fetches of the VS patched by D3D, each
//     element found by its destination register (C5b). Guest data is converted
//     to host words with the fetch constant's byte order, like the emulation
//     (spirv_translator_fetch.cpp), and uploaded to the frame buffer.
//   - Indices: VGT_DMA_BASE/SIZE or automatic, with VGT_INDX_OFFSET. Quad lists
//     become triangles v0 v1 v2 / v0 v2 v3 (primitive_processor.cpp).
//   - 2D textures by fetch constant: linear or tiled, formats 8, 8_8, 8888,
//     2_10_10_10, 16, 16_16, 16F, 32F, DXT1/3/5, DXT5A and DXN, with the fetch
//     constant's swizzle in the view. C2's resolved textures are sampled as they
//     are. The content is compared once per frame. The game's mip levels are
//     included (fh1_native_mipmaps), packed as on the Xbox 360; without them,
//     foliage and asphalt looked grainy in the distance.
//   - State: blending, color mask, depth, stencil, culling, viewport (without the
//     half pixel: the shader adds it, g_HalfPixelOffset) and window scissor.
//   - Clipping disabled (draws in pixels, like the videos): viewport the size of
//     the render target and the transform in the VS with g_NdcScale/g_NdcOffset,
//     which only the library regenerated with the NFSMW XenosRecomp has.
//
// Not covered (rejected or substituted, with the cause logged once)
//   Mips of 3D textures, vertex textures, signed or gamma textures, points,
//   rectangle lists, line loops and vertex formats without a direct Vulkan
//   equivalent.

#include "fh1_native_draws.h"
#include "fh1_hitch_waits.h"

#include "fh1_native_vertex_dedupe.h"
#include "fh1_native_texture_pool.h"

#include "fh1_graphics_settings.h"
#if __has_include("fh1_native_glow_energy_spirv.h") && __has_include("fh1_native_glow_soft_spirv.h")
#include "fh1_native_glow_energy_spirv.h"
#include "fh1_native_glow_soft_spirv.h"
#else
// The two variants of the glow bright pass are derived from a game shader and are not distributed with the
// sources. Without them the glow setting keeps the game's own shader.
#define FH1_WITHOUT_VARIANTS_GLOW 1
static const uint32_t kSpirvGlowEnergy[1] = {0};
static const uint32_t kSpirvGlowSoft[1] = {0};
#endif
#include "fh1_native_shaders.h"
#include "fh1_native_hooks.h"  // fh1_d3d_game_vegetation
#include "fh1_reflection_on_demand.h"  // fh1_reflection_visibility
#include "fh1_shader_library.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <system_error>  // the bind thread
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). The method it picks for GCC (1) reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the write of the data
 * being hashed (strict aliasing). The texture key read keys[4] before writing it, and the same texture
 * was created several times. Same hash values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would "
       "come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "fh1_rect_gs_spirv.h"  // NFSC: RECTANGLE_LIST geometry shader
#include "fh1_flat_ps_spirv.h"  // NFSC: debug flat pixel shader
#include "fh1_view_ps_spirv.h"  // NFSC: debug interpolant-view pixel shaders

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriorityOk (texture bind thread). That header deliberately does not
// include switch.h (same as in fh1_native_system.cpp).
#include "../../sdk/src/core/threading_switch.h"
#endif

/*
 * p_000139, the final composite, is a single full-screen quad with twelve texture samples: 2.8-3.2 real
 * ms, 72-80 % of all post-processing. Seven of those twelve are the radial speed blur. Turning it off
 * removes them and leaves the center tap.
 */
/*
 * Vegetation in the shadow map.
 *
 * The four alpha-tested pixel shaders of the shadow pass (n36, n68, n99, n103) are trees, bushes and wire
 * fences. Measured on PC with an A/B test: they are 55 % of the pass's draws but only 12 % of its
 * triangles.
 *
 * The pass cost follows the triangle count, and removing them is worth -1.2 ms of GPU but -3.1 ms of CPU,
 * which is also at 96 % of a core.
 *
 * The right constant is 0.556 real ms per 10,000 triangles, not the 0.80 first estimated (fit over 17
 * race intervals with a constant open area, r2 = 0.982; see fh1_clip_shadows.cpp). With it, 12 % of
 * the triangles would be -0.44 ms, so about -0.76 ms of the -1.2 ms measured here is not geometry: it is
 * the vegetation's alpha pixel shader, which in the shadow map runs in full only to produce the cutout.
 *
 * What is lost: trees and fences stop casting shadows. Buildings, cars and the road keep theirs. It shows
 * on a forest track.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(fh1_native_fast_untile, true, "FH1",
                    "Native renderer (24/09, build 167): textures are untiled 16 bytes at a time with the row "
                    "computed once. The first 2000 levels are checked against the usual path and, if one differs, "
                    "it turns itself off. false = the usual path, block by block");

/*
 * The FramebufferDe cache forgets destroyed views. It is keyed by handle values (render pass, 5 views
 * and size) and used to release nothing until the destructor. When C2 destroys an image (GetResolved
 * recreates it with another size or format at the same address, and those textures end up as render
 * targets through image swaps), its framebuffers stayed behind and, if Vulkan gave the same handle to a
 * new view, a framebuffer created on the dead view was returned. Destroy now notifies through
 * ForgetView and the framebuffers that use the view are destroyed. false = previous behaviour (nothing
 * is forgotten).
 */
REXCVAR_DEFINE_BOOL(fh1_native_framebuffers_forget_views, true, "FH1",
                    "Native renderer (25/09, build 184): destroying a C2 image destroys the framebuffers that use "
                    "its view, so a reused handle does not return a stale one. false = as before");

REXCVAR_DEFINE_INT32(fh1_native_fingerprint_kb_per_frame, 6144, "FH1",
                     "Native renderer (24/09, build 165): KB of STABLE textures the ring re-checks at most in one "
                     "frame; the ones that do not fit wait 1-2 frames (at most 8 times in a row). Spreads out the "
                     "checks that fell on the same frame. 0 = no limit, as before");

/*
 * Sampled recheck of stable textures. See PrepareTexture and FingerprintSample.
 * Enabled by default: its guard verifies itself and switches off at the first mismatch.
 */
REXCVAR_DEFINE_INT32(fh1_native_fingerprint_sampling, 8, "FH1",
                     "Native renderer (25/09, build 170): STABLE textures are re-checked with the fingerprint of a "
                     "sample (first and last 4 KB block and 1 in 8, base and mips) instead of all their bytes; if "
                     "the sample changes, the full path follows. N = 1 in N re-checks of each texture stays full, "
                     "and the first 3000 compute both fingerprints: a single disagreement turns it off. 0 = always "
                     "the full fingerprint, as before")
    .range(0, 64);

REXCVAR_DEFINE_BOOL(fh1_shadows_without_vegetation, true, "FH1",
                    "Do not draw what uses alpha test into the shadow map: trees, bushes and wire fences. They are "
                    "55 % of the pass's draws and 12 % of its triangles, so it saves little GPU (-1.2 ms) and a "
                    "fair amount of CPU (-3.1 ms). Trees stop casting shadows");

/*
 * Radial blur of the final composite, removed by default (true).
 *
 * The Xbox 360 blurs the screen edges at speed (visible at 200 km/h in a capture of the 360 version),
 * and without it the port looks sharper. Restoring it was measured in a race: with the blur and the
 * 9-sample PCF the GPU went from 25.1 to 27.3 real ms per frame and became the bottleneck again, with
 * twice as many frames of 36 ms or more (4.9 % vs 9.6 %) and no streak above 40 FPS. The difference is
 * hard to notice next to the 360 footage, so it stays removed. false = keep the blur, as on the Xbox 360.
 */
REXCVAR_DEFINE_BOOL(fh1_native_no_blur, true, "FH1",
                    "Remove the radial blur from the final composition. It is 7 of the 12 samples of the only "
                    "full-screen quad of the post-processing (1.5-2.0 real ms). The blur of the edges when "
                    "accelerating and with NOS is lost: the image gets sharper");

REXCVAR_DEFINE_BOOL(fh1_native_skip_shadows, false, "FH1",
                    "Native renderer (FPS test): does not record the shadow-map draws (depth-only target with a "
                    "pitch of 1600 or more); visible cut");
// Enabled by default. On the console (A and B alternating every 30 s), the UBO intervals run 18-23 %
// faster than the neighbouring pointer intervals at the same draw load.
REXCVAR_DEFINE_BOOL(fh1_native_constants_ubo, true, "FH1",
                    "Native renderer: shaders read their constants from dynamic UBOs (constant bank on Maxwell) "
                    "instead of through a 64-bit pointer. Same bytes: the image does not change. Needs the shader "
                    "library with SPEC_CONSTANT_CONSTANTS_UBO; false goes back to the pointer");
REXCVAR_DEFINE_INT32(fh1_native_constants_ubo_toggle_s, 0, "FH1",
                     "Native renderer (test): with N > 0 toggles the constants between pointer (even intervals) "
                     "and UBO (odd intervals) every N seconds and logs each change, to compare captures of a still "
                     "scene in the same run");
/*
 * Descriptor set 4 by differences (the work is done in NVK, see mesa/patch_nvk_set4.py).
 *
 * vkCmdBindDescriptorSets for set 4 costs 1.3-1.4 us per call in a race and happens in 70-78 % of the
 * draws ("C6 substages" report), and it leaves four cbuf rebinds for the Draw. Almost all of it is NVK:
 * four writes to the root table (one per word of the dynamic descriptors, even when only the low part of
 * the address changes) and an 80-slot walk that dirties every cbuf of the set even when only one offset
 * changed. With the patch, NVK sends only the words that change and rebinds only the cbufs whose
 * descriptor changes. It is exact (the GPU ends up with the same bindings), and NVK checks it against
 * what it really bound in each slot: on a DIFFERENCE it fixes that draw, switches off for the session and
 * ControlSet4 reports it in the log as an error.
 * This cvar requests or withdraws it from NVK on each submission; with an unpatched Mesa (or on PC) it
 * does nothing. false = NVK binds the whole set, as usual.
 */
// Enabled by default. Measured: 1 root-table write per bind instead of 4, although the set 4 bind still
// costs 1.31-1.45 us (1.29-1.63 before). At one point the NVK guard seemed to check no cbuf at all; that
// was a bug in its own sampling ("1 in 4,096 draws of the buffer", and no buffer reaches 4,096 draws),
// not a sign that there was nothing to check. In its first ~40 s it checked 386,271 cbufs without a
// single difference, and the Draw rebound 38-45 % fewer cbufs. The patched Mesa (p03 in
// mesa/c186_draw) samples with the process-wide count.
REXCVAR_DEFINE_BOOL(fh1_native_set4_differences, true, "FH1",
                    "Native renderer (25/09, build 184): set 4 (constants by UBO) by differences on NVK: only the "
                    "descriptors that change are sent and rebound. Same result (checks itself). Needs the Mesa "
                    "with patch_nvk_set4; false = as always");
REXCVAR_DEFINE_INT32(fh1_native_set4_differences_toggle_s, 0, "FH1",
                     "Native renderer (test, build 184): with N > 0 toggles set 4 by differences (odd intervals) "
                     "and as always (even intervals) every N seconds and logs each change, to compare C6 substages "
                     "in the same run. 0 = no toggling");
/*
 * The draw path in NVK (Mesa with p01-p04 and p06 from mesa/c186_draw).
 *
 * The ring is the bottleneck, and ~3.4 us of each draw is spent inside NVK: vkCmdDrawIndexed 1.4-1.6 us
 * per call, vkCmdBindPipeline 3-3.7 and set 4 1.3 ("C6 substages" report). Four exact improvements (the
 * GPU receives the same commands with the same data), each with its own guard in NVK that compares
 * against the usual path for the first 20,000 uses and then 1 in 1,024, and switches off on a DIFFERENCE:
 *   - emission: each command written in one go (Draw, cbuf rebinds, root table, BIND_VB), same bytes;
 *   - cbufs: only the slots that can get dirty, and no cbuf flush when nothing is dirty;
 *   - dynamic: only the dynamic state groups with dirty bits;
 *   - prefetch: cache hints (PRFM) for the pipeline and shaders before they are used; the app also hands
 *     the pipeline to NVK as soon as it knows it (after PipelineOf), several us before vkCmdBindPipeline.
 * Estimated for the four together: 0.6-1.5 us per draw (~1.1-2.8 ms of ring time per frame). The
 * per-part measurement (CNTPCT clock, 1 in N calls) gives the real breakdown in "C6 NVK by parts".
 * With an unpatched Mesa (or on PC) none of this does anything.
 */
REXCVAR_DEFINE_INT32(fh1_native_nvk_measure, 64, "FH1",
                     "Native renderer (26/09, build 186): measures inside NVK, by parts, 1 in N calls (power of 2; "
                     "C6 NVK by parts every 10 s). 0 = no measuring")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(fh1_native_nvk_measure_misses, false, "FH1",
                    "Native renderer (test, build 186): reads ahead what the pipeline bind and the shader flush "
                    "will use and measures it separately (how much is cache misses). Does extra work: only for "
                    "measuring");
REXCVAR_DEFINE_BOOL(fh1_native_nvk_emission, true, "FH1",
                    "Native renderer (26/09, build 186): NVK writes each command of the draw at once, with the "
                    "same bytes (checks itself). false = as always");
REXCVAR_DEFINE_BOOL(fh1_native_nvk_cbufs, true, "FH1",
                    "Native renderer (26/09, build 186): NVK only looks at the cbuf slots that can get dirty and "
                    "skips the cbuf flush if nothing is dirty (checks itself). false = as always");
REXCVAR_DEFINE_BOOL(fh1_native_nvk_dynamic, true, "FH1",
                    "Native renderer (26/09, build 186): NVK only emits the dynamic-state groups with dirty bits "
                    "(checks itself). false = as always");
REXCVAR_DEFINE_BOOL(fh1_native_nvk_preload, true, "FH1",
                    "Native renderer (26/09, build 186): cache hints (PRFM) for the pipeline and the shaders "
                    "before using them, and for the pipeline requested after PipelineOf. Changes nothing. false = "
                    "no hints");
REXCVAR_DEFINE_INT32(fh1_native_nvk_toggle_s, 0, "FH1",
                     "Native renderer (test, build 186): with N > 0 toggles the four NVK improvements (odd "
                     "intervals on, even off) every N seconds, to compare in the same run. 0 = no toggling");

#if REX_PLATFORM_SWITCH
/*
 * The state of set 4 by differences lives in NVK (struct nvk_switch_set4 in nvk_cmd_buffer.h,
 * mesa/patch_nvk_set4.py). Same field order and types, with a version: it is a contract. Weak symbol:
 * with an unpatched Mesa the address is null and ControlSet4 reports it once.
 */
extern "C" {
struct NvkSwitchSet4Counts {
  uint64_t bindings_difference;
  uint64_t bindings_complete;
  uint64_t writes_root;
  uint64_t dwords_root;
  uint64_t cbufs_dirty;
  uint64_t cbufs_saved;
  uint64_t draws;
  uint64_t checks;
  uint64_t differences;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t requested;
  int32_t environment;
  int32_t off;
  NvkSwitchSet4Counts total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 has to measure the same as in NVK");
#endif
#if REX_PLATFORM_SWITCH
/*
 * The draw path in NVK (struct nvk_switch_draw in nvk_cmd_buffer.h, p01 in mesa/c186_draw). Same
 * field order and types, with a version: it is a contract. Weak symbols: with an unpatched Mesa the
 * addresses are null and ControlDrawNvk reports it once.
 */
extern "C" {
struct NvkSwPart {
  uint64_t times;
  uint64_t ticks;  // CNTPCT_EL0, a ticks_by_second
};
struct NvkSwImprovement {
  int32_t requested;  // written by the app: 1 yes, 0 no
  int32_t off;  // 1: its guard saw a DIFFERENCE
  uint64_t uses;
  uint64_t validated;
  uint64_t differences;
  uint64_t without_check;
};
struct NvkSwitchDraw {
  int32_t version;
  int32_t measure;
  int32_t measure_misses;
  int32_t environment;
  uint64_t ticks_by_second;
  NvkSwPart parts[16];   // enum nvk_sw_part
  uint64_t counts[13];    // enum nvk_sw_count
  NvkSwImprovement improvements[5];  // emission, cbufs, dynamic, set4_fast (not applied), prefetch
};
extern NvkSwitchDraw nvk_switch_draw __attribute__((weak));
void vk_switch_preload_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDraw: the same size as in NVK");
#endif
/*
 * Sampler caches valid across frames.
 *
 * With three work slots, this cache once turned the race leaderboard (names and distances) into a smear;
 * with two slots it did not show, and disabling the cache with three slots made the text perfect again.
 *
 * The cause: two paths set no validity horizon at all. Resolved textures (a render target read back as a
 * texture) set `valid_until = UINT64_MAX`, "valid forever while the generation does not change", and
 * the race leaderboard is exactly that: a render target the game rewrites every frame. With three work
 * slots the CPU runs two frames ahead and the descriptor slot of an old view was reused, hence the smear.
 * With two slots the distance was not enough for it to show.
 *
 * Fixed by setting `valid_until = frame_` on those two paths (see the comments in PrepareTexture).
 * The other paths were already tied to `texture.next`, the same policy that decides when the content
 * is rechecked: nothing that was there before is relaxed.
 *
 * What it buys: the cold path of the `textures` stage costs 13.5 us and runs 266 times per frame =
 * 3.6 ms. The hot path (2,893 hits) is indistinguishable from zero. With the cache valid across frames,
 * the cold ones drop to those expiring in that frame (~45-70), i.e. 2.6-3.0 ms.
 *
 * What to check, in motion (not paused): the race leaderboard with names and distances, the HUD and the
 * rear-view mirror. If anything looks blurry or delayed, disable this cvar in the toml.
 */
REXCVAR_DEFINE_BOOL(fh1_native_texture_cache_across_frames, true, "FH1",
                    "Native renderer: the sampler caches stay valid across frames (build 128) as long as the "
                    "texture content is not due for a check. false: they expire every frame, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Uploads the game's mip levels. With only the base level of each texture, distant surfaces looked grainy
// compared with the Xbox 360. This also fixes the base level of small textures with packed mips, which
// does not start at the base address.
REXCVAR_DEFINE_BOOL(fh1_native_mipmaps, true, "FH1",
                    "Native renderer: uploads the mip levels the game provides (as on the Xbox 360). false: only "
                    "the base level, as before build 136")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_gamma_pwl, true, "FH1",
                    "Native renderer: textures fetched with the gamma sign are converted in the shader with the "
                    "console's piecewise-linear curve (as the emulated GPU does). false = the host's sRGB formats, "
                    "as before (a darker picture)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_gamma_pwl_mask, 31, "FH1",
                     "Debug: host formats that take the console curve in the shader (1 RGBA8, 2 BC1, 4 BC2, 8 BC3, 16 "
                     "others); the rest keep the host sRGB format");
REXCVAR_DEFINE_BOOL(fh1_native_cube_levels, true, "FH1",
                    "Native renderer: the reflection cube map gets the smaller levels the game renders (as on the "
                    "console). false: the first level only (sharp, white reflections)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A mip level read from the wrong place (packed tail offset, row or slice alignment) causes no Vulkan
// errors, only smudges in the distance. The game's mips are reductions of the base level, so their
// average color has to resemble the base level's.
// The texture cache used to release nothing. On the console (8 laps) it went from 62 MB at the start of
// the race to 392 MB after 12 minutes without levelling off (2,578 textures); with mipmaps it grows 25 %
// faster. Above the limit, the textures unused for the longest are evicted.
// 384 MB was once too much for the console: the cache reached 313 MB after 14 minutes and the GPU ran out
// of memory before getting near the limit: nvMapCreate failed even for 64 KB and the screen went black
// with the audio still playing. The limit was then lowered to 192 MB, to leave room for the render
// targets, guest memory and the rest.
/*
 * 192 -> 384, based on the GPU memory budget report.
 *
 * On the console: "heap 0 (GPU): 478 MB used of 1375 MB budgeted (size 2391 MB); the texture
 * cache holds 150 MB of 192". So more does fit: almost 900 MB of the budget is untouched.
 *
 * And the 192 limit was doing harm: "texture cache near the limit: cold ones are released a few at a time (200
 * in total)" in every report, with 53 MB of texture uploads every 10 s. That is evicting textures only
 * to upload them again right away, and each re-upload is a spike on the ring thread. It matches the
 * stutters: 3.5-5 % of the frames exceed 50 ms.
 *
 * The reason for lowering it to 192 still stands (the GPU ran out of memory after 14 minutes with the
 * cache at 313 MB), but that was with 384 and without the emergency path that exists now (stop the GPU,
 * release half the cache and retry). With 478 of 1375 MB used, 384 leaves a 700 MB margin.
 */
/* 384 -> 512 on the Switch, with fh1_internal_resolution = "auto" (720p handheld and 1080p docked).
 * In races at 1024x576 the cache already reached 272-281 MB, and at 720p and 1080p the resolved render targets
 * are larger, so the resolution and the limit go up together: raising only the resolution once filled the cache,
 * and releasing textures to upload them again caused stutters. GPU memory on the console: 514 MB used of
 * 1,492 MB budgeted. nfsmw.toml has the same value. */
#if REX_PLATFORM_SWITCH
constexpr int32_t kTexturesMbMaxByDefault = 512;
#else
constexpr int32_t kTexturesMbMaxByDefault = 384;
#endif
REXCVAR_DEFINE_INT32(fh1_native_texture_mb_max, kTexturesMbMaxByDefault, "FH1",
                     "Native renderer: MB of textures above which the ones unused the longest (at least 120 "
                     "frames) are released, down to 75 %. If the game asks for them again, they are uploaded "
                     "again. 0 = no limit, as before build 144")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Creating a new texture without stalling the ring thread.
 *
 * With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
 * still makes two ioctls: the plane's VA with its pte_kind and the mapping of the pool chunk
 * (nvk_image.c:1696-1710; all our textures are tiled with pte_kind GENERIC_16BX2,
 * nil/image.rs:439-440). In a race that is ~0.6 ms of wall time per texture, and entering a new zone
 * brings 33-45 at once: 19-27 ms of stalled ring in that frame.
 * With this, the ring does the CPU part and the bind thread does vkBindImageMemory while the ring carries
 * on with the draws; before closing the upload buffer it waits for whatever is missing and records the
 * barrier and the copy in that same buffer. The GPU receives the same thing in the same submission: no
 * placeholder textures and no lower mips.
 */
REXCVAR_DEFINE_BOOL(fh1_native_texture_binding_thread, true, "FH1",
                    "Native renderer (25/09, build 184): the vkBindImageMemory of new textures (address "
                    "reservation and mapping on Horizon, ~0.6 ms each) runs on a separate thread while the ring "
                    "keeps recording; before submitting, whatever is missing is waited for. Same result (checks "
                    "itself). false = on the ring")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_texture_binding_thread_priority, 0x2D, "FH1",
                     "Native renderer (build 184): priority of the texture binding thread. 0x2D, the ring's: above "
                     "the guest (0x3B) so a finished ioctl does not wait for a core, and outside the bands of the "
                     "audio (0x2B) and the presentation (0x2C). It sleeps in the ioctl almost all the time")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Hashing and untiling of new textures, off the ring thread.
 *
 * During zone-change stutters the ring spends 5.3-8.7 ms of the frame on new textures: the XXH3 of guest
 * memory (~37 %), untiling and byte swapping (~63 %, 1.8-3.1 ms per MB), plus the copy to the upload
 * buffer. With this, the ring only copies the bytes the hash covers (the same memory at the same point as
 * the inline path: a snapshot) and a thread does the hash, the untiling, the byte swap and the copy to the
 * upload buffer on that copy. Before submitting, the ring collects whatever is missing and does itself
 * whatever the thread has not started. The barrier and the copy are recorded where they always are: the
 * GPU receives the same bytes in the same submission, with no placeholder textures and no frame of delay.
 * It verifies itself (observing phase, then 1 in 128) and switches off with REXLOG_ERROR on any
 * DIFFERENCE. false = everything on the ring, as before.
 */
// Disabled by default. Its guard tripped at start-up (3 jobs not collected when switching upload buffers)
// and switched it off; also, analysis showed that texture creation causes no race stutter (56 us per
// texture on the ring) and that the zone-change stutter is limited by the game itself.
REXCVAR_DEFINE_BOOL(fh1_native_texture_fingerprint_thread, false, "FH1",
                    "Native renderer (26/09, build 185): the fingerprint (XXH3), untiling and byte order of new "
                    "textures are done by a separate thread on a copy of the guest memory taken on the ring; "
                    "before submitting, whatever is missing is collected. Same result (checks itself). false = on "
                    "the ring, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_texture_fingerprint_thread_priority, 0x2E, "FH1",
                     "Native renderer (build 185): priority of the texture fingerprint thread. 0x2E: below the "
                     "ring (0x2D), which preempts it as soon as it has work, and above the guest (0x3B), so the "
                     "guest does not starve it; outside the audio (0x2B) and the presentation (0x2C)")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_texture_fingerprint_thread_core, -2, "FH1",
                     "Native renderer (build 185): preferred core of the fingerprint thread. -2 = automatic: core "
                     "2, or 1 if the ring runs on 2 (on the busy ring core it would hardly run); -1 = the process "
                     "default; 0-2 = that core. Not exclusive")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_texture_fingerprint_thread_mb, 16, "FH1",
                     "Native renderer (build 185): MB for the guest-memory copies of the new textures of one "
                     "submission. What does not fit is prepared on the ring, as before")
    .range(4, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measurement only: new textures whose content repeats a live one. See NoteContentTexture. It changes
 * no decision of the ring: it only counts, and reports every 10 s in the "C3 reuse by content"
 * line.
 */
REXCVAR_DEFINE_BOOL(fh1_native_diag_reuse, true, "FH1",
                    "Native renderer (26/09, build 186, measurement only): counts how many new textures have the "
                    "same content and shape as another live one in the cache (the game reloads the packs at "
                    "another address) and how many of those others have been unused for more than 120 frames. "
                    "Changes nothing. false = no counting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Pipeline prewarming. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms
 * each on the console (56 of them in the first race, 5.2 s of stutter). It happens the first time after any
 * change to the shader library, the driver or the key, and on a fresh install. See LoopPrewarm.
 */
REXCVAR_DEFINE_INT32(fh1_native_prewarm_threads, -1, "FH1",
                     "Native renderer: threads that walk the pipeline prewarm list at once before the ordered walk "
                     "(-1 = half the logical cores, at most 6; 0 = the single ordered walk, as before)");
REXCVAR_DEFINE_BOOL(fh1_native_pipelines_prewarm, true, "FH1",
                    "Native renderer (26/09, build 186): at startup, a lowest-priority thread re-creates in the "
                    "Vulkan cache the pipelines the ring created in earlier sessions (their list is in "
                    "cache/fh1_native_pipelines.bin), with the same function as the ring, and destroys them: when "
                    "the ring asks for them they are already compiled (no 70-160 ms hitches per pipeline in the "
                    "first race after a change). Changes no pipeline and no draw. false = no prewarming (the list "
                    "is still saved)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Testing only. Pretends the GPU runs out of memory on one in N texture allocations, to check that the
// emergency path (stop the GPU, release half the cache and retry) really works. 0 does nothing, which is the
// normal setting.
REXCVAR_DEFINE_INT32(fh1_native_test_out_of_memory_every, 0, "FH1",
                     "Native renderer (tests only): fakes running out of GPU memory in 1 of every N texture "
                     "allocations, to exercise the recovery. 0 = off")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_diag_mips, false, "FH1",
                    "Native renderer (diagnostic): compares the average color of each mip level with the base "
                    "level in DXT1/3/5 and 8888 textures and logs the ones that do not match (a level read from "
                    "somewhere else). Does not change the image")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_test_depth_clamp, false, "FH1",
                    "Native renderer (tests only, 17/09 build 149): depthClampEnable in every pipeline. Imitates "
                    "what NVK seems to do on the console with what lies behind the far plane (the sun of the "
                    "scene's occlusion queries)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_soft_glow_toggle_s, 0, "FH1",
                     "Native renderer (test, build 150): with N > 0 rotates the sky glow between original, natural "
                     "and soft (fh1_sky_glow) every N seconds and logs each change, to compare captures of the "
                     "same spot");
REXCVAR_DEFINE_BOOL(fh1_native_test_occlusion_always, false, "FH1",
                    "Native renderer (tests only, 17/09 build 150): colorless draws of an occlusion query on a "
                    "target of 640 or more always pass the depth test. Separates a sun that covers no pixels from "
                    "one hidden by depth. Changes what the game sees")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * This test has been answered: there is nothing to gain.
 *
 * A fit over 17 race intervals gives, for the shadow pass, raw ms = 0.0342 x thousands of triangles + 0.023
 * (r2 = 0.982). The intercept is 0.037 real ms: with both 1600x1600 maps open (5.12 Mtexels, i.e. 20.5 MB of
 * loadOp = LOAD per frame) and zero triangles, the pass costs nothing measurable. So on this GPU the
 * loadOp = LOAD of a depth attachment is not paid for: Maxwell does not load the tile up front like a tiled
 * GPU, it reads on demand. The theory of "130-165 MB per frame moved for nothing" further below does not
 * hold for the shadow map depth.
 *
 * On ZCULL: in NVK (nvk_cmd_draw.c, `use_zcull`) a pass with loadOp = CLEAR enables ZCULL even when the
 * image has no plane (ephemeral, without LOAD/STORE between passes, which a map drawn whole every time does
 * not need), while DONT_CARE, which this cvar uses, does not: the condition is
 * `zcull_plane || loadOp == CLEAR`, and DONT_CARE is neither. CLEAR has the same visual risk as DONT_CARE and
 * leaves the map at 1.0 (far) instead of garbage, so it is the better of the two. It still does not pay off:
 * the ZCULL ceiling here is those 0.037 ms, because the pass is pure geometry. For ZCULL to cover the map,
 * the renderArea has to span the full 1600x1600: the ZCULL region comes from render->area, not from the
 * image size.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(fh1_native_shadow_pass_without_load, true, "FH1",
                    "Native renderer (FPS test): opens the shadow-map pass without loading its previous content "
                    "(loadOp = DONT_CARE). Measured in 112: nothing to gain, the depth loadOp costs nothing on "
                    "this GPU (the pass with 0 triangles costs 0.037 ms)");
REXCVAR_DEFINE_INT32(fh1_native_skip_shadows_toggle_s, 0, "FH1",
                     "Native renderer (test): with N > 0 skips the shadows in the odd intervals of N seconds and "
                     "logs each change, to compare captures of the same spot");
REXCVAR_DEFINE_BOOL(fh1_native_diag_repeated_vertices, false, "FH1",
                    "Native renderer (diagnostic): counts the vertex bytes that repeat address, size and content "
                    "in the same frame or against an earlier one");
// On the console, the vertex copy was ~3 of the ring thread's ~9 us per draw ("uploads" stage), and with the
// game's busy-waits removed there are free cores.
REXCVAR_DEFINE_BOOL(fh1_native_uploads_thread, true, "FH1",
                    "Native renderer: vertex copies to the upload buffer are made by a separate thread while the "
                    "ring thread keeps recording; they are waited for before submitting the work to the GPU and "
                    "before returning the read pointer to the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The ring no longer waits for a copy thread that has no core (see WaitUploads).
 *
 * In a race, the ring waited 14.7 ms for the copy thread during an alley stutter (3,965 draws) and 17.9 ms in
 * the next one, and the game spent 21.1 ms without room in the ring. The thread ran at 0x3B, below the two
 * game threads (0x3A), and with the CPU at 278 % out of 300 it sat ready without a core: of those ms, only
 * ~6-7 were copying.
 */
/*
 * Copy thread priority. It used to run at its creation priority (0x3B, below the two game threads at 0x3A)
 * and, with the ring's help path (fh1_native_uploads_help), the ring always reached the copies first: the
 * thread copied 0 MB and the ring 100 % (1.4-1.8 GB, ~1 s every 10 s: ~3 ms per frame on the thread that is
 * the bottleneck). At 0x2E, like the hash thread, it takes the copies as soon as they are queued, on another
 * core; the ring (0x2D) preempts it if they share a core, and the help path only handles what is left when
 * waiting.
 */
REXCVAR_DEFINE_INT32(fh1_native_uploads_thread_priority, 0x2E, "FH1",
                     "Native renderer (26/09, build 187): priority of the vertex copy thread. 0x2E: below the ring "
                     "(0x2D) and above the guest (0x3A-0x3B), so it copies while the ring records. 0x3B = as up to "
                     "build 186")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The copy thread on a different core from the ring. At the Heritage & Omega exit the thread only did 20 % of
 * the copies: the ring, with higher priority (0x2D) and at 94 %, did not leave it its core, and did them
 * itself while waiting (2.5-5 ms per frame). With its preferred core elsewhere, the thread takes CPU from the
 * game threads (0x3A, lower priority), which spend 97 % of their time waiting for the ring. Only the preferred
 * core is set: the affinity mask is untouched and the kernel can still move it (same as the hash thread).
 */
REXCVAR_DEFINE_INT32(fh1_native_uploads_thread_core, -2, "FH1",
                     "Native renderer (26/09, build 191): preferred core of the vertex copy thread. -2 = one "
                     "different from the ring's; -1 = no preference (as up to build 190); 0-2 = that one")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * 16-bit indices with hand-written NEON (IndicesDe16). At one point the compiler stopped vectorizing that
 * loop (0 vector rev16 in Draw, 7 before) and the "indices" substage rose to 2.95 us per draw (the whole
 * stage had been 1.2-1.3 us): ~3 ms of ring time per frame. Same results (the same indices in the same order,
 * the same minimum and maximum). Guard: the first 20,000 draws, then 1 in 4,096, are compared with the plain
 * loop; on a DIFFERENCE the plain loop is kept for the session.
 */
REXCVAR_DEFINE_BOOL(fh1_native_indices_neon, true, "FH1",
                    "Native renderer (26/09, build 187): 16-bit indices are copied and measured (minimum and "
                    "maximum) with NEON, 16 per loop. Same result (checks itself). false = the usual loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_uploads_help, true, "FH1",
                    "Native renderer (26/09, build 185): when the ring thread has to wait for the vertex copies, "
                    "it copies itself the ones the copy thread has not started yet and waits for the one in "
                    "progress with priority inheritance. Same data (checks itself). false = waits as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(fh1_native_diag_empty_gray, false, "FH1",
                    "Native renderer: textures that are not supported yet (cube, 3D, pending formats) sample gray "
                    "instead of zero (tests only)");
REXCVAR_DEFINE_BOOL(fh1_native_inv_tex_size, true, "FH1",
                    "Native renderer (18/09, build 164): shaders take 1/texture size from the constants instead of "
                    "asking the texture on every sample with an offset. It is the same computation with the same "
                    "number: the image does not change. Needs a shader library regenerated with the new helper");
/*
 * Enabled by default: a single texel instead of the 3x3, measured and long enabled in a local toml. Three
 * cvars were only enabled through a local toml, and the toml overrides the default: shipping without that
 * line would lose the improvement without anyone noticing.
 */
/*
 * The cheap PCF was briefly disabled by default because, with a single shadow map sample, walls and garage
 * doors showed diagonal bands (shadow acne) that the Xbox 360 does not have; from a distance they looked
 * blurry. The 9-sample PCF fixed nothing: the grid looked the same with 9 samples as with 1, because it was
 * shadow map acne, which the depth slope bias removes (fh1_native_shadow_bias_slope). The 9 samples
 * only cost GPU time: together with the blur, race GPU time went from 25.1 to 27.3 real ms. So the cheap
 * PCF is enabled by default again.
 */
REXCVAR_DEFINE_BOOL(fh1_native_cheap_pcf, true, "FH1",
                    "Native renderer (20/09): a single shadow-map sample instead of the 3x3 pattern at half a "
                    "texel. The smoke (p_000101) does eleven per pixel in full-screen rectangles and is 21 % of "
                    "the scene. The shadow edge gets a little less soft (25/09, build 183: true by default again; "
                    "the bands on the walls were acne and the depth bias removes them). Needs the regenerated "
                    "shader library");

/*
 * Anisotropic filtering, beyond the Xbox 360 (fh1_native_anisotropic).
 *
 * The game requests trilinear without anisotropy on all its textures ("C4 filters requested": anisotropy 0,
 * bias 0), and the 435 sampling instructions of its 74 pixel shaders do not change that (all of them "use
 * the constant", read from the original binaries). That is why, on the 360 too, surfaces seen at a grazing
 * angle or from a distance look blurry and gain detail as you get closer: the white garage door in the alley,
 * for example. With N > 1, linear samplers with mips get anisotropy N (capped by the device). Single-level
 * ones (resolved targets, shadow map, screen effects) and point samplers stay as they were. It costs GPU time
 * in the scene: measured in "C2: GPU time per Swap ... scene". 0 = trilinear, as on the 360.
 */
// 0 by default. At 8x it did not remove the streaks on the alley doors, and the scene went up ~2 ms of GPU
// time together with the 9 shadow samples.
REXCVAR_DEFINE_INT32(fh1_native_anisotropic, 0, "FH1",
                     "Native renderer (25/09, build 179): anisotropic filtering of the world textures (linear "
                     "samplers with mips). 0 = like the Xbox 360 (trilinear, blurry at an angle and in the "
                     "distance); 2, 4, 8 or 16 = sharper, at a GPU cost")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Alley door diagnostic (fh1_native_diag_min_mip).
 *
 * The garage door slats and the brickwork showed streaks and ripples that the Xbox 360 does not have, and
 * they did not go away at native 720p nor with 8x anisotropy. With N > 0, textures with mips start N levels
 * lower even up close (the sampler raises its minLod). With the car stopped in front of a door: if with 1, 2
 * or 3 it looks blurry but with its slats in place, its mip levels are fine and the streaks come from
 * elsewhere (the shadow); if streaks, breaks or a pattern that is not the door's show up, that level is
 * uploaded wrong. It can be changed at runtime from the settings menu: it is part of the sampler key and,
 * when it changes, the per-texture sampler caches are invalidated. 0 = normal.
 */
/*
 * Our own depth bias in the shadow map (the acne on the alley doors).
 *
 * Garage doors and walls showed a fine grid of dots and diagonal streaks that the Xbox 360 does not have.
 * Diagnosed with captures taken standing in front of a door: without the shadow map the grid disappears;
 * with 9 samples or with 1 it looks the same; and with the minimum mip at 1, 2 or 3 the texture blurs as
 * expected but the grid stays just as sharp. It is shadow acne: the surface shadows itself. The game sets no
 * depth bias in that pass (0 "depth bias" lines in every log). Our own is added only to
 * the shadow map draws, and only if the game does not set one. Too high a value detaches the shadows from the
 * objects casting them. Both values can be changed at runtime from the settings menu. 0 and 0 = previous
 * behaviour.
 */
REXCVAR_DEFINE_INT32(fh1_native_shadow_bias_slope, 20, "FH1",
                     "Native renderer (25/09, build 181): slope-scaled depth bias in the shadow map, in tenths (20 "
                     "= 2.0). Removes the acne (diagonal dot grid on walls and garage doors). Too high detaches "
                     "the shadows from the objects. 0 = as before. Changes at run time")
    .range(0, 100);
REXCVAR_DEFINE_INT32(fh1_native_shadow_bias_constant, 0, "FH1",
                     "Native renderer (25/09, build 181): constant depth bias in the shadow map, in thousands of "
                     "24-bit units (1 = 1000). 0 = no constant. Changes at run time")
    .range(0, 100);

REXCVAR_DEFINE_INT32(fh1_native_diag_min_mip, 0, "FH1",
                     "Diagnostic (25/09, build 180): textures with mips start N levels further down, also up "
                     "close, to see whether their mip levels are right. 0 = normal; 1, 2 or 3 to look. Changes at "
                     "run time")
    .range(0, 4);
/*
 * Splitting the frame into two submissions.
 *
 * Measured on the console: there is a single vkQueueSubmit per frame, at the end, from Present. The GPU
 * runs out of work from the end of one frame until the CPU sends the next, and that is ~4 real ms of idle
 * time per frame ("gap between jobs", constant in a steady race).
 *
 * Submitting as soon as the shadow pass closes lets the GPU start on the shadows while the CPU records the
 * scene. It does not change a single pixel: it is the same work in two pieces.
 */
/*
 * Disabled by default. The idea above is sound but the measurement ruled it out: the gap between
 * submissions went up 3.61 ms when the frame was split in two (two vkQueueSubmit calls = twice the GPU
 * start-up latency, and the second piece cannot start until the CPU finishes recording it). The default
 * follows the measurement: what helps ships enabled and what makes things worse ships disabled. The setting
 * stays so the test can be repeated.
 */
REXCVAR_DEFINE_BOOL(fh1_native_send_after_shadows, false, "FH1",
                    "Native renderer (20/09): submit the work to the GPU as soon as the shadow pass closes, "
                    "instead of all together at the end of the frame. The GPU no longer sits idle waiting for the "
                    "CPU to finish recording");
REXCVAR_DEFINE_INT32(fh1_native_cheap_pcf_toggle_s, 0, "FH1",
                     "Native renderer (20/09): with N > 0 toggles the shadow-map sampling between the 3x3 pattern "
                     "and a single texel every N seconds, and logs it. Used to measure it A/B in the SAME race: "
                     "comparing different races does not work because the stretch of road changes");
REXCVAR_DEFINE_INT32(fh1_native_inv_tex_size_toggle_s, 0, "FH1",
                     "Native renderer (test, build 164): with N > 0 toggles every N seconds between taking 1/size "
                     "from the constants and asking the texture");
REXCVAR_DEFINE_INT32(fh1_native_per_draw_statistics_s, 0, "FH1",
                     "Native renderer (diagnostic, build 159): with N > 0 measures one frame every N seconds with "
                     "one query per draw and splits the fragments by pixel shader");
REXCVAR_DEFINE_INT32(fh1_native_test_scissor, 0, "FH1",
                     "Native renderer (test, build 159): clips the draws of one category to 1x1 (1 shadows, 2 "
                     "scene, 3 cube, 4 post-processing). All the state and all the draws are sent but nothing is "
                     "shaded: the time difference is the per-draw floor. BREAKS THE IMAGE while set: only for "
                     "measuring");
REXCVAR_DEFINE_INT32(fh1_native_test_scissor_toggle_s, 0, "FH1",
                     "Native renderer (test, build 159): with N > 0 toggles the scissor of "
                     "fh1_native_test_scissor every N seconds");
/*
 * Separating the texture unit from the ALU (scene pass analysis).
 *
 * The game requests mipmapMode LINEAR on almost every sampler (mag/min/mip filters 1/1/1). On Maxwell a
 * trilinear sample takes the TMU two cycles and a bilinear one only one, so this halves the texture unit's
 * work without removing a single ALU instruction or shader sample. It is the only clean lever to decide
 * which of the two dominates the 18.94 ms of the scene. The jump between mip levels is visible when moving
 * away: for measurement only.
 */
REXCVAR_DEFINE_BOOL(fh1_native_test_point_mip, false, "FH1",
                    "Native renderer (test, 21/09): the filter BETWEEN mip levels goes from linear to point "
                    "(trilinear -> bilinear). The TMU does half the work and the ALU does not change: if the scene "
                    "gets faster, sampling is the limit; if it does not move, the ALU is. The mip jump is visible "
                    "when moving away: only for measuring");
REXCVAR_DEFINE_INT32(fh1_native_test_point_mip_toggle_s, 0, "FH1",
                     "Native renderer (test, 21/09): with N > 0 toggles fh1_native_test_point_mip every N seconds "
                     "(both samplers live in the cache, no stalls when switching)");
/*
 * Do not upload the same vertices twice in the same frame.
 *
 * Each draw copies the [vmin..vmax] range of each binding to the upload buffer, byte-swapped. There was no
 * cache: the same piece of geometry was copied again in full every time it was drawn. Measured on PC with
 * fh1_native_diag_repeated_vertices:
 *     5954.9 MB copied; 2584.2 MB repeated within the same frame and 3329.2 MB equal to an earlier
 *     frame  ->  99.0-99.6 % of the bytes are byte-for-byte repeats, across eight reports.
 * On the console that is 8.4 MB of vertices per frame at 2518 MB/s into uncached memory = 3.3 ms of
 * writing alone, split between the ring and the copy thread.
 *
 * This only does the safe half: repeats within the same frame (41-49 % of the bytes). And it is exact, not a
 * gamble: on the Xbox 360 the GPU reads the vertices when it executes the draw, so the game cannot rewrite a
 * range that is already referenced without first synchronizing with the GPU. The only point where it can is
 * a guest wait (WAIT_REG_MEM), and there everything recorded is forgotten (g_synchronizations_ring).
 *
 * If geometry ever looks stuck or stretched, this is the first thing to disable.
 */
REXCVAR_DEFINE_BOOL(fh1_native_vs_textures, true, "FH1",
                    "Native renderer: textures fetched by the vertex shader are bound too (the final composite reads "
                    "the adapted luminance there to choose the exposure). false = only the pixel shader's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_exp_bias, true, "FH1",
                    "Native renderer: texture fetches are scaled by the console's exponent bias (the fetch constant's "
                    "exp_adjust plus the exp_bias of the resolve that made the picture). The game's FXAA reads the scene "
                    "at 1, 1/2 and 1/4 this way; without it edges get bright outlines. Needs the shader library built "
                    "after 2026-10-04 night (<sampler>_ExpScale). false = no scale")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_dedupe_vertices, true, "FH1",
                    "Native renderer (21/09): if two draws of the same frame ask for the same vertex range, it is "
                    "uploaded only once. The C6 report logs hits and MB saved");

/*
 * No vkCmdBindVertexBuffers in single-binding draws.
 *
 * Each draw copies its vertices to a new spot in the upload buffer, so its binding changed almost every
 * time: in a race, 0.32-0.46 us per call in 66-98 % of the draws (0.21-0.34 us per draw). In NVK that is
 * three wrapper functions (vk_common_CmdBindVertexBuffers -> 2EXT -> 3KHR) and 5 words with an MME macro per
 * binding.
 *
 * With a single binding, the copy is allocated at a multiple of the stride, the upload buffer stays bound
 * at 0 and the draw is shifted with vertexOffset (indexed) or firstVertex (non-indexed). The GPU reads the
 * same bytes: (offset / stride + i - vmin) * stride = offset + (i - vmin) * stride. The NFSMW shaders do not
 * read SV_VertexID (XenosRecomp only declares it with UNLEASHED_RECOMP and the library is generated with
 * NFSMW_RECOMP), so the base index is not visible anywhere. NVK passes vertexOffset/firstVertex as is to
 * the draw macro on every Draw, with or without this change: recording the draw costs the same.
 *
 * These still bind as usual: draws with two or more bindings, those that reuse (dedupe) a copy that does not
 * start at a multiple of their stride, and the deferred sky, which saves and replays its own binding (and
 * when emitted leaves bindings_recorded_ at 0, so the next draw binds at 0 again).
 *
 * Guard (CheckBaseZero): the first 200,000 draws on this path, then 1 in 4,096, redo the computation
 * backwards in 64 bits and check the limits. On a single difference, that draw binds as usual, DIFFERENCE
 * is written to the log and the path switches off for the session. What the guard cannot see is the GPU:
 * if anything looked wrong, fh1_native_zero_based_vertices = false restores the usual path.
 */
REXCVAR_DEFINE_BOOL(fh1_native_zero_based_vertices, true, "FH1",
                    "Native renderer (25/09, build 179): in draws with a single vertex binding, the upload buffer "
                    "stays bound at 0 and the draw is shifted with vertexOffset/firstVertex, without one "
                    "vkCmdBindVertexBuffers per draw. The GPU reads the same bytes. false = as before");

REXCVAR_DEFINE_BOOL(fh1_native_alpha_only_ps, true, "FH1",
                    "Native renderer (18/09, build 158): in passes without a color target, compiles the pixel "
                    "shader without the color writes. The image does not change (Vulkan discards them) and the "
                    "driver removes as dead code what only fed the color: the alpha test remains");
REXCVAR_DEFINE_INT32(fh1_native_alpha_only_ps_toggle_s, 0, "FH1",
                     "Native renderer (test, build 158): with N > 0 toggles every N seconds between compiling the "
                     "pixel shader with and without the color writes in the colorless passes");

/*
 * Depth test before shading, where it can be done without changing the image.
 *
 * The problem, measured. The scene costs 13.86 raw ms = 22.55 real ms (in a race) and it is pure shading:
 * 6.5 M fragments over 0.92 M pixels, so every screen pixel is shaded 7 times. And our own counter says
 * that 28 % of the color draws prevent early rejection: they have an alpha test or a real kill.
 *
 * Why that costs so much. A pixel shader that can discard forces the hardware to shade first and test
 * depth afterwards (late-Z): otherwise a discarded fragment would already have written its Z. So everything
 * with alpha (smoke, particles, glass, decals) is shaded in full even when it is behind a building. In NVK
 * that is literal: nvk_shader.c only sets SET_API_MANDATED_EARLY_Z when the module declares
 * EarlyFragmentTests, and our modules never declare it.
 *
 * The part that can be fixed, and why it is pixel-identical. The reason for late-Z is the Z write, not the
 * test. If the draw writes neither depth nor stencil, moving the test earlier cannot corrupt anything: there
 * is nothing extra to write. The shader still discards the color the same way. So for every draw with the
 * Z test on, Z write off and stencil off, declaring EarlyFragmentTests is free and exact, and the GPU stops
 * shading what is hidden.
 *
 * Deliberately left out: opaque alpha-tested draws (vegetation) write Z, so they are not touched; and
 * neither are draws inside a game occlusion query, because testing earlier would change the samples it
 * counts, and the game reads them.
 *
 * The "C6 Z early" report says how many draws are fixed and how many cannot be because they write
 * depth: that second figure is the exact size of what only a depth pre-pass would solve.
 */
REXCVAR_DEFINE_BOOL(fh1_native_early_z, true, "FH1",
                    "Native renderer (20/09): in the draws that test depth but do NOT write it (smoke, particles, "
                    "glass, decals), declare EarlyFragmentTests in the pixel shader so the GPU tests depth BEFORE "
                    "shading instead of after. Without this, any shader with alpha test or kill shades all its "
                    "fragments even when they end up hidden. The image is identical: there is no Z write to move "
                    "earlier");
REXCVAR_DEFINE_INT32(fh1_native_early_z_toggle_s, 0, "FH1",
                     "Native renderer (test, 20/09): with N > 0 toggles the early depth test every N seconds, to "
                     "measure it A/B in the SAME race");
/*
 * The sky is drawn first and shades the whole screen for nothing.
 *
 * Measured, not estimated (FRAGMENT_SHADER_INVOCATIONS counter per draw, 20 frames): the sky dome is a
 * single draw per frame, 480 indices = 160 triangles, and yet it invokes 0.83 M fragments: 90 % of the
 * screen. Its pixel shader has 5 texture samples. Cost ~4.8 real ms of the scene's 19.2 ms, 25 %.
 *
 * Why. The game draws it before the world. At that point the Z-buffer is empty, so nothing can discard it:
 * it shades the whole screen and then the world paints over it.
 *
 * Why deferring it does not change a single pixel. The draw is opaque (ONE/ZERO blending: it does not read
 * the target) and already tests depth with LEQUAL without writing it (RB_DEPTHCONTROL 00700732). So without
 * deferral, wherever the world covers it, the sky's result is overwritten; deferred, in the same place, the
 * depth test discards it before shading. The final color is the same in both cases, pixel for pixel, and
 * depth is not touched before or after because the sky does not write it.
 *
 * Where it is emitted. In the same pass, as soon as a draw arrives that it cannot be moved past (one with
 * blending, which does read the background color, or an opaque one that does not write depth, which the
 * sky would cover because the Z-buffer would not have changed), or when the pass closes, whichever comes
 * first. Every copy or resolve of the target goes through FinishPass (fh1_native_targets.cpp), so it
 * is emitted there too before anything reads the color.
 *
 * The only thing that could show, and how to recognize it. If the game drew something at exactly the same
 * depth as the dome (far plane z), the LEQUAL test would let the sky through and cover it, where without
 * deferral it would be the other way round. Nothing should be there (the sky is the farthest thing in the
 * scene), but if the sky is ever seen eating very distant geometry, this is why: disable the cvar.
 */
/*
 * A first version broke the image, and this was why.
 *
 * It produced flickering, badly rendered geometry and odd colors in the menus and on the car. The counter
 * said it unambiguously:
 *
 *     "C6 sky: 7.00 detected per frame, 1.00 actually deferred"
 *
 * The analysis expected 0.9 draws per frame for that pixel shader. In reality there are seven: the sky dome
 * is not the only user of that shader. Deferring one of the seven breaks the order of the other six, and
 * that is where the artifacts came from.
 *
 * What was missing: the fingerprint identifies the shader, not the draw. Two draws with the same shader and
 * similar state are indistinguishable by this criterion. And the counter that would have exposed it
 * (detected per frame) shipped in the same build as the change instead of before it. The dome draw has to
 * be identified as such (by index count, 480 = 160 triangles, by being the first of the pass, and by
 * checking that it really covers the screen), and verified with the counter before anything is deferred.
 */
/*
 * Enabled again, but it no longer decides alone.
 *
 * With this cvar on, earlier versions broke the image because seven draws share the shader's fingerprint.
 * Now the self-checking guard sits above it: even with the cvar on, nothing is deferred until 90
 * consecutive frames have been seen with a single dome, and as soon as two show up it switches off for the
 * rest of the session. See kSkyFramesTest and CloseFrameOfTheGuardOfSky. Enabling it
 * cannot break the image; at worst it does nothing.
 */
REXCVAR_DEFINE_BOOL(fh1_native_deferred_sky, true, "FH1",
                    "Native renderer (22/09): defer the sky draw until after the opaque draws of the same pass, "
                    "instead of drawing it first on an empty Z-buffer. It is a single opaque draw that tests depth "
                    "and does not write it, so the image is identical: what today gets overwritten, the depth test "
                    "discards instead");
/*
 * Draws that paint nothing and are still shaded.
 *
 * Two exact cases, both read from the draw's own registers:
 *
 *  1. Blending is "0 x source + 1 x destination" (ADD) on every written channel: the result is the
 *     destination as is. The draw cannot change a single color pixel.
 *  2. The alpha test function is 0 (NEVER): the recompiled shader always calls clip() (alphaTestValue
 *     returns -1 for case 0), so every fragment dies, and dies before writing depth.
 *
 * In both cases, if the draw also writes neither depth nor stencil, it leaves no trace of any kind and can
 * be skipped entirely. If it writes depth, only its color write is removed (mask 0), which already saves
 * the blending and the write.
 */
REXCVAR_DEFINE_BOOL(fh1_native_skip_invisible, true, "FH1",
                    "Native renderer (20/09): skip the draws that cannot change a single pixel (a blend that "
                    "copies the target, or an alpha test with the function NEVER). The image is identical by "
                    "definition");
/*
 * Small per-draw savings on the ring thread. Each has its own cvar; the first three have a self-checking
 * guard (the first 200,000 cases, then 1 in 4,096, also go through the usual path and are compared; on a
 * difference, DIFFERENCE in the log and the saving is switched off).
 */
REXCVAR_DEFINE_BOOL(fh1_native_framing_cache, true, "FH1",
                    "Native renderer (25/09, build 179): the viewport, the ndc and the scissor of a draw are "
                    "reused as long as the framing registers (their generation) and the pass do not change. Checks "
                    "itself. false = computed for every draw, as before");
REXCVAR_DEFINE_BOOL(fh1_native_useful_height_memo, true, "FH1",
                    "Native renderer (25/09, build 179): the useful height of the target is noted in the map entry "
                    "of the last pitch, without looking it up for every draw. Checks itself. false = as before");
REXCVAR_DEFINE_BOOL(fh1_native_fast_pass_key, true, "FH1",
                    "Native renderer (25/09, build 179): if a draw's targets are the same bytes as those of the "
                    "open pass, their XXH3 is not recomputed. Checks itself. false = XXH3 on every draw");
REXCVAR_DEFINE_BOOL(fh1_native_cvars_per_frame, true, "FH1",
                    "Native renderer (25/09, build 179): fh1_native_alpha_only_ps and "
                    "fh1_native_no_ps_without_color (and their toggles) are read once per frame and not on "
                    "every colorless draw. false = as before");
REXCVAR_DEFINE_INT32(fh1_native_no_ps_without_color_toggle_s, 0, "FH1",
                     "Native renderer (test, build 157): with N > 0 toggles every N seconds between building the "
                     "pipeline with and without a fragment stage in the colorless draws, to compare captures of "
                     "the same spot with the game paused");
REXCVAR_DEFINE_BOOL(fh1_native_no_ps_without_color, true, "FH1",
                    "Native renderer (17/09, build 157): in the draws that write no color, builds the pipeline "
                    "without a fragment stage if the pixel shader cannot discard pixels nor write depth. The image "
                    "does not change and the GPU does not shade (shadow map)");
REXCVAR_DEFINE_STRING(fh1_native_diag_skip_ps, "", "FH1",
                      "Native renderer: comma-separated PS numbers whose draws are not recorded (tests only, to "
                      "locate a draw)");
// A review of other Switch projects found that in NVK for Tegra the memory type the SDK picks for uploads
// (without HOST_CACHED) is an NvMap without CPU caching, and wine-nx measured on the console that writing
// there is slow. All vertex copies go to this buffer, so it can be chosen for an A/B test.
REXCVAR_DEFINE_INT32(fh1_native_upload_memory, 0, "FH1",
                     "Native renderer: memory of the upload buffer. 0 = the one the SDK picks (on the Switch, "
                     "without CPU cache), 1 = with CPU cache (published with vkFlushMappedMemoryRanges before "
                     "submitting), 2 = without CPU cache")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_shared_cache, true, "FH1",
                    "Native renderer (23/09, build 156): the shared constants of each draw (488 bytes) in a small "
                    "buffer WITH CPU cache, separate from the upload buffer. Measured in 155: writing them without "
                    "cache cost 2.0 us per draw and with cache 0.4. Only with the constants by UBO")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Direct-mapped pipeline cache.
 *
 * PipelineOf costs 0.52-0.76 us per draw in a race ("C6 substages"), more than BindVertexBuffers. The
 * one-entry shortcut misses on ~1 in 3 draws (the same ones that then call BindPipeline), and each miss is
 * an XXH3 of 80 bytes, a division by libstdc++'s prime bucket count and 2-3 jumps to nodes scattered across
 * the heap, which with the ring streaming megabytes through the cache almost always miss all the way to
 * memory: ~1.3 us per miss.
 *
 * In front of the map sits a table of 256 contiguous slots (22 KB) indexed by the low bits of the same
 * XXH3, holding the full key: a hit is one memcmp on a single slot. It only stores key -> VkPipeline pairs
 * already in the map, and the map never erases, so it cannot return anything other than what the map
 * would. Guard: the first 200,000 hits, then 1 in 4,096, also look up the map and must get the same
 * VkPipeline; on a difference, DIFFERENCE in the log and it switches off for the session. false = map only,
 * as before.
 */
REXCVAR_DEFINE_BOOL(fh1_native_direct_pipelines, true, "FH1",
                    "Native renderer (25/09, build 179): direct cache of 256 pipelines in front of the PipelineOf "
                    "map. Same result as the map (checks itself). false = only the map, as before");
/*
 * Measurement only, it changes no draw. What changes at each vkCmdBindPipeline of the ring: shaders, vertex
 * input, formats, specialization, or only fixed pipeline state (blending, masks, Z, stencil, cull face,
 * topology, bias, primitive restart). The state-only ones are those Vulkan dynamic state would avoid. Two
 * "C6 pipeline changes" lines every 20 s. false = nothing is counted.
 */
// Disabled by default: it was only a measurement and it has already produced its data.
REXCVAR_DEFINE_BOOL(fh1_native_count_pipeline_changes, false, "FH1",
                    "Native renderer (25/09, build 184): counts what changes on each vkCmdBindPipeline "
                    "(measurement only, changes no draw). C6 pipeline-change lines every 20 s. false = not counted");
/*
 * Dynamic state, phase 0a. A draw's pipeline is looked up with its key in canonical form (Canonicalize):
 * whatever PipelineOf does not read, or reads but Vulkan ignores (the blend equation without blendEnable,
 * the half of the equation whose channels the mask does not write, the Z function without a Z test, stencil
 * operations without stencil, targets not in the pass), is set to a fixed value. The pipeline is the same in
 * everything Vulkan looks at: fewer pipelines and fewer vkCmdBindPipeline calls (the counter's "no effect"
 * category). Guard: the first 200,000 key changes, then 1 in 4,096, compare the fixed state of both keys
 * field by field (FillStateFixed, the relevant part of PipelineOf); on a difference, DIFFERENCE in the
 * log and it switches off for the session. false = the usual key.
 */
// Disabled by default. Measured: PipelineOf got more expensive and no avoidable bind was measured.
REXCVAR_DEFINE_BOOL(fh1_native_canonical_key, false, "FH1",
                    "Native renderer (25/09, build 184): the pipeline is looked up with the key in canonical form "
                    "(only what Vulkan looks at): fewer pipelines and fewer vkCmdBindPipeline. Checks itself. "
                    "false = the usual key");
/*
 * Dynamic state, phase 0b. BeginPass no longer forgets the bound pipeline. Vulkan keeps the binding and
 * the dynamic state across passes of the same command buffer, and NVK only marks state as dirty when a pass
 * begins (nvk_cmd_buffer_dirty_render_pass). The same key carries the same formats and therefore a
 * compatible render pass (compatibility ignores loadOp and storeOp). A new command buffer still starts with
 * nothing bound. It removes the repeated vkCmdBindPipeline of a pass's first draw (the counter's "same key
 * after starting a pass"). false = forgotten at every pass, as before.
 */
REXCVAR_DEFINE_BOOL(fh1_native_pipeline_between_passes, true, "FH1",
                    "Native renderer (25/09, build 184): the bound pipeline is kept when a pass of the same "
                    "command buffer starts (Vulkan keeps it). false = bound again in every pass, as before");
/*
 * Dynamic state, phase 1 (EDS1/EDS2, core in Vulkan 1.3; the console reports API 1.3.354). Cull mode, front
 * face, topology (within its class), Z test, write and function, stencil with its operations, depth bias
 * and primitive restart leave the pipeline: they are set with vkCmdSet* and only when they change. Two draws
 * that only differ in those share a pipeline and need no new vkCmdBindPipeline (3-4.5 us each on the
 * console). Pipelines in this mode carry a flag in the key (fill2) and do not mix with the usual ones;
 * the mode is decided once per command buffer. Guard: the first 200,000 key changes, then 1 in 4,096,
 * compare what was set with what the usual pipeline would carry (FillStateFixed); on a difference,
 * DIFFERENCE in the log and it switches off for the session (pipelines with all state baked in come back,
 * and the draw with the difference already goes out with its own). false = everything in the pipeline, as
 * before.
 */
// Disabled by default. Measured: each BindPipeline went from 2.95 to 4.36 us and PipelineOf from 0.21 to
// 0.47 us per draw, for only 13 % fewer binds: a net loss of ~1 ms of ring time per frame.
REXCVAR_DEFINE_BOOL(fh1_native_dynamic_state, false, "FH1",
                    "Native renderer (25/09, build 184): cull face, front face, topology, Z, stencil, bias and "
                    "restart with vkCmdSet* (EDS1/EDS2 of Vulkan 1.3) instead of in the pipeline: fewer "
                    "vkCmdBindPipeline. Checks itself. false = all in the pipeline, as before");
/*
 * Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3; NVK exposes it on Maxwell and the SDK enables it
 * with ui_vulkan_state_dynamic3.patch). Blending (blendEnable), its equation and each target's color mask
 * are set with vkCmdSet* and stop splitting pipelines: among the first 64 pipelines created, the 12
 * state-only variants differed only in blending. If the SDK or the device does not provide it, it is not
 * used, and one log line says so. Same guard as phase 1. false = blending in the pipeline, as before.
 */
// Disabled by default, for the same measured reason as fh1_native_dynamic_state (it goes with it).
REXCVAR_DEFINE_BOOL(fh1_native_dynamic_state3, false, "FH1",
                    "Native renderer (25/09, build 184): blend, equation and color mask with vkCmdSet* "
                    "(VK_EXT_extended_dynamic_state3) instead of in the pipeline. Checks itself. false = in the "
                    "pipeline, as before");
REXCVAR_DEFINE_BOOL(fh1_native_diag_upload_memory, true, "FH1",
                    "Native renderer: when creating the upload buffer, measures once how many MB/s the CPU writes "
                    "into each visible memory type (8 MB) and how much publishing them costs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in fh1_clip_shadows.cpp, which owns the lever.
namespace fh1::guard30 {
bool WithoutShadows(bool of_user);
}  // namespace fh1::guard30

REXCVAR_DEFINE_INT32(fh1_debug_view_interp, -1, "FH1",
                     "Debug: every pixel shader is replaced by one that shows interpolant TEXCOORD<N> (0-7) as colour")
    .range(-1, 7)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_vertices_10_11_11, true, "FH1",
                    "Native renderer: draw k_10_11_11 vertex data (raw bits through a float input, unpacked in the shader). "
                    "Off: on AMD the festival went black with it (float denormal/NaN handling probably alters the bits)");
REXCVAR_DEFINE_BOOL(fh1_msaa_4x_as_1x, false, "FH1",
                    "Native renderer: 4x MSAA passes draw into the 1x render target of twice the pitch, as they share the "
                    "EDRAM on the Xbox 360 (FH1 draws its scene depth that way). Off: the first try turned the festival pink/black "
                    "(2026-10-03); false = separate one-sample image");
REXCVAR_DEFINE_BOOL(fh1_native_alpha_to_mask, true, "FH1",
                    "Native renderer: draws with alpha to mask and no alpha test discard pixels below half alpha (the "
                    "crowd and foliage cut-outs). false = drawn solid");
REXCVAR_DEFINE_BOOL(fh1_msaa_4x_clears_as_1x, true, "FH1",
                    "Native renderer: 4x MSAA passes of 640 pitch or less (Direct3D's clears of depth and stencil) draw into "
                    "the 1x render target of twice the pitch, which is the one the game uses. false = an image of their "
                    "own (no shadows in the scene)");
REXCVAR_DEFINE_INT32(fh1_vertices_10_11_11_mask, 0xFFFF, "FH1",
                     "Vertex usages (bit = D3DDECLUSAGE: 0 position, 3 normal, 5 texcoord...) whose k_10_11_11 data is drawn. "
                     "Positions were off until 2026-10-04: their draws are billboards fetched with index / 4 "
                     "(EntryVertices::index_computed), and drawn without that they covered the festival in black");
REXCVAR_DEFINE_BOOL(fh1_barriers, true, "FH1",
                    "Full GPU memory barriers between copies, clears and render passes (needed on AMD). false = as the "
                    "Most Wanted port (no barriers: only for comparisons)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_debug_no_cull, false, "FH1", "Debug: face culling is switched off for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_debug_only_ps_pitch, 1280, "FH1", "Debug: surface pitch that fh1_debug_only_ps applies to")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(fh1_debug_only_ps, "", "FH1",
                      "Debug: comma list of pixel shader numbers; draws into 1280-wide surfaces by any other PS are skipped")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_debug_no_depth, false, "FH1",
                    "Debug: the depth test and depth writes are switched off for every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_gamma_textures, true, "FH1",
                    "Textures fetched with the gamma sign are converted to linear when sampled (host sRGB formats)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_native_gamma_targets, true, "FH1",
                    "Draws into k_8_8_8_8_GAMMA render targets are stored gamma-encoded and resolved pictures "
                    "fetched with the gamma sign are decoded (host sRGB views)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_debug_no_dxt, false, "FH1",
                    "Debug: every block-compressed (DXT) texture is replaced by the empty stand-in texture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fh1_debug_flat_ps, false, "FH1",
                    "Debug: every pixel shader is replaced by a flat colour, to see where the geometry lands")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fh1::native {

// Incremented by the ring thread when it handles a WAIT_REG_MEM (fh1_native_system.cpp). That is the
// point where vertex deduplication must forget what it recorded.
std::atomic<uint32_t> g_synchronizations_ring{0};
namespace {

/*
 * A pass's category from its render target, in one place.
 *
 * It used to be decided by width: "1600 or more" meant shadows. With the scene at 1920
 * (fh1_1080p_test) that no longer tells them apart and the scene was counted as shadows, so its
 * category vanished from the report. The shadow map is recognized for what it is: a depth-only target, with
 * no color at all.
 */
inline uint32_t CategoryOfTarget(uint32_t pitch, const uint64_t* keys) {
  const bool color = keys[0] || keys[1] || keys[2] || keys[3];
  if (!color && keys[4] && pitch >= 1600) {
    return kGpuShadows;
  }
  if (pitch >= 1280) {
    return keys[4] ? kGpuScene : kGpuSceneWithoutDepth;
  }
  if (pitch >= 640) {
    return kGpuReflection;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuSmaller;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kRegConstantsVs = 0x4000;
constexpr uint32_t kRegConstantsPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;

// FH1: the exponent scale of a fetch from a resolved picture kept in a float image (fh1Exp in shader_common.h).
// Negative = cut negative values and NaN to 0; its mantissa carries the cut at the top, 2^(the fetch's exponent
// adjust), which is where the console's unsigned resolved format ends (fh1_native_float_cut; INT32_MIN or the
// option off = no cut). `scale` is a power of two.
REXCVAR_DEFINE_BOOL(fh1_native_float_cut, true, "FH1",
                    "Native renderer: fetches of a resolved picture kept in a float image are cut at the value the "
                    "console's 10-bit resolved format ends at (4 for the scene, 16 for the reflection cube map). "
                    "false = no cut, as before (blue rims and orange dots on the car at night, darker picture)");
inline float ExpScaleFloatPicture(float scale, int32_t exp_fetch) {
  if (exp_fetch == INT32_MIN || !REXCVAR_GET(fh1_native_float_cut)) {
    return -scale;
  }
  const int32_t code = std::clamp(exp_fetch, -16, 15) + 17;  // 1..32
  return -scale * (1.0f + float(code) / 64.0f);
}
constexpr uint32_t kRegBooleans = 0x4900;
constexpr uint32_t kRegistersConstants = 0x400;  // 256 constants x 4

constexpr uint32_t kCapacityHeap[4] = {4096, 16, 64, 512};  // 2D, 3D, cube, samplers
// Work slots. The ones actually used are chosen by fh1_native_work_slots; this is the room reserved
// for them, and it has to match the array in fh1_native_targets.cpp.
constexpr size_t kSlotsOfWork = 3;
constexpr VkDeviceSize kSizeUpload = VkDeviceSize(64) << 20;
// Separate buffer for the shared constants, per slot. ~1,400 draws x 512 bytes = 0.7 MB per frame; if it
// fills up, the work is submitted just as with the upload buffer.
constexpr VkDeviceSize kSharedConstantsSize = VkDeviceSize(4) << 20;  // per work slot (there are three)
// The pipeline cache and the prewarm list go in a single file in <NRO folder>/cache/ (nothing loose next to
// the NRO, and a single pipelines .bin). Header "NFPC", version, list bytes and cache bytes (two uint32
// and two uint64), then both parts: the list as is (its "NFPL" header and the records) and the
// vkGetPipelineCacheData data.
constexpr const char* kFolderCache = "cache";
constexpr const char* kFilePipelines = "fh1_native_pipelines.bin";
constexpr uint32_t kMagicFilePipelines = 0x4350464Eu;  // "NFPC" in little-endian
constexpr uint32_t kVersionFilePipelines = 1;
constexpr size_t kHeaderFilePipelines = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
// The two files used by earlier versions, next to the NRO: if the new one does not exist yet they are read
// once (so the existing cache is not lost) and deleted when the new one is written.
constexpr const char* kFileCacheOld = "fh1_native_pipelines.bin";
constexpr const char* kFileListOld = "fh1_native_pipelines_list.bin";
// The pipeline prewarm list. It holds no game data: state keys, formats and fingerprints. A header of four
// uint32 ("NFPL", version, record size and record count) followed by the records (RegisterPipeline).
constexpr uint32_t kMagicListPipelines = 0x4C50464Eu;  // "NFPL" in little-endian
constexpr uint32_t kVersionListPipelines = 1;
constexpr size_t kHeaderList = 4 * sizeof(uint32_t);
constexpr size_t kMaxRegistersList = 4096;
// 296 bytes: NFSMW's shader_common.h (g_NdcScale at +280 and g_NdcOffset at +288).
// Shared constants: texture and sampler indices (0-63), booleans, texcoords, half pixel, alpha threshold
// (68) and function (69), NDC (64-73) and g_InputRemap for the 16 locations (74-89).
// 90 words up to g_InputRemap (bytes 296..359) and 32 more for 1/size of the 16 texture slots (bytes
// 360..487), which avoid querying the texture size on every sample.
constexpr uint32_t kWordsShared = 252;  // FH1: all 256 booleans (244-251); NFSC: + 32 loop constants (words 122-153); FH1: g_GuestBase / g_FetchAddress (154-163), exponent scales (164-179), ranked fetches (180-243)
constexpr uint32_t kWordFetchRankAddress = 180;  // FH1: g_FetchRankAddress, one word per declared fetch 0-31
constexpr uint32_t kWordFetchRankParam = 212;    // FH1: g_FetchRankParam
constexpr uint32_t kWordExpScale = 164;  // FH1: <sampler>_ExpScale, one float per fetch constant 0-15
constexpr uint32_t kWordInvSize = 90;
// Constants through a dynamic UBO (fh1_native_constants_ubo). The bit is SPEC_CONSTANT_CONSTANTS_UBO
// from shader_common.h, and the sizes are the blocks the shaders declare: 256 and 224 float4, and 23 shared
// float4.
constexpr uint32_t kSpecConstantsUbo = uint32_t(1) << 8;
// Internal bit of the pipeline key that no shader reads: the bright pass (PS n137, container
// kFingerprintBrightPass = p_000094) uses the fh1_sky_glow variant: natural
// (p_000094_glow_energy.hlsl) or soft (p_000094_glow_soft.hlsl).
// The shaders take 1/texture size from the shared constants (SPEC_CONSTANT_INV_SIZE_TEX in XenosRecomp)
// and do not query the texture size on every sample.
constexpr uint32_t kSpecInvSizeTex = uint32_t(1) << 9;
constexpr uint32_t kSpecGlowNatural = uint32_t(1) << 12;
constexpr uint32_t kSpecGlowSoft = uint32_t(1) << 13;
// The pixel shader is compiled without its color writes (pass without a color target).
constexpr uint32_t kSpecOnlyAlpha = uint32_t(1) << 14;
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets (nine samples
// per pixel; eleven in p_000101, the smoke, which is full-screen quads and 21 % of the scene). With this bit
// the eight outer offsets are set to zero, the nine samples become identical and the compiler merges them
// into one. Shadow edges lose some smoothness. Needs the library regenerated with
// shaders/fh1_regenerar_library_pcf.sh.
constexpr uint32_t kSpecPcfCheap = uint32_t(1) << 15;
// Bits 16-18 = the alpha test function (0-6). See SPEC_CONSTANT_ALPHA_FUNC_SHIFT in shader_common.h: it
// removes the 7-case switch from every pixel shader.
constexpr uint32_t kSpecFunctionAlphaOffset = 16;
// The radial blur of the final composite (see SPEC_CONSTANT_WITHOUT_BLUR).
constexpr uint32_t kSpecWithoutBlur = uint32_t(1) << 19;
// Internal bit of the pipeline key that no shader reads (shader_common.h goes up to bit 19). It marks that
// the pixel shader module is the copy with OpExecutionMode EarlyFragmentTests.
constexpr uint32_t kSpecZEarly = uint32_t(1) << 20;
// fh1_native_shadow_minimum. SPEC_CONSTANT_SHADOW_MINIMUM from shader_common.h: tfetch2DShadowMin takes the
// minimum of the shadow map and its pair, which goes in the 3D index word of that register.
constexpr uint32_t kSpecShadowMinimum = uint32_t(1) << 23;
constexpr uint64_t kFingerprintComposition = 0x19C0C358044A29BFull;  // p_000139, the race VisualTreatment
// fh1_color_filter. The final composite (kFingerprintComposition, p_000139) tints each channel with a polynomial
// curve (Coeffs0..3 = c6..c9, evaluated at MISCMAP1.w), mixes in a desaturated part with its x component and adds a
// vignette (VisualEffectVignette.x = c1). With a neutral curve (Coeffs0 = (0, 1, 1, 1) and Coeffs1..3 = 0) and the
// vignette at 0 the picture has no filter; the glow (g_fBloomScale, c4) and the brightness of the fades
// (CombinedBrightness, c10) stay the same. soft: halfway between the game's values and the neutral ones (the curve is
// linear in its coefficients). The registers are those of the microcode, the same in every edition.
constexpr uint32_t kBytesTreatment = 11 * 16;  // c0 to c10
void ApplyTreatmentVisual(float* c, int mode) {
  static constexpr float kNeutral[4][4] = {{0, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  const float f = mode == 2 ? 1.0f : 0.5f;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      float& v = c[(6 + k) * 4 + i];
      v += (kNeutral[k][i] - v) * f;
    }
  }
  c[1 * 4 + 0] *= 1.0f - f;
}

/*
 * The render target height comes from the pitch, not from what the game uses.
 *
 * fh1_native_targets.cpp creates every render target with `height = max(720, pitch)`, because the real
 * height is not known when it is created. Measured result:
 *
 *   scene    pitch 1280 -> image 1280x1280, the game uses 1280x720
 *   cubemap  pitch  320 -> image  320x720,  the game uses  320x256
 *   bloom    pitch  320 -> image  320x720,  the game uses  320x180
 *   bloom    pitch  160 -> image  160x720,  the game uses  160x90
 *
 * And since the passes are opened with loadOp = LOAD and closed with storeOp = STORE, that whole area is
 * read and written in each of the ~27 pass openings per frame: 18.81 Mtexels opened when the game uses
 * ~8.5. That is ~130-165 MB per frame moved for nothing, over a 21.3 GB/s bus that the three cores also
 * share.
 *
 * The Xbox 360 paid none of this: its framebuffer lived in 10 MB of on-chip EDRAM at 256 GB/s, and the
 * resolve was a hardware operation. We move it back and forth.
 *
 * It is fixed through the renderArea, not the image size. Vulkan only loads and stores the renderArea;
 * everything outside is preserved. So the height does not have to be guessed when the image is created:
 * opening the pass over the rectangle the game really uses is enough.
 *
 * How that height is known without guessing: from the draws' scissor. The maximum seen per pitch is kept,
 * only grows, and is rounded up to a multiple of 64. Until there is data the whole pass is opened, so the
 * first frame never clips too much.
 */
REXCVAR_DEFINE_BOOL(fh1_native_pass_useful_area, true, "FH1",
                    "Native renderer (20/09): open each pass on the rectangle the game really uses instead of on "
                    "the whole image. The height of the targets is deduced from the pitch, so the scene opens "
                    "1280x1280 to draw 1280x720 and the cube 320x720 to draw 320x256. The image is identical: "
                    "Vulkan only loads and stores the renderArea");
constexpr uint64_t kFingerprintBrightPass = 0xE849A9F6D3323B87ull;
/*
 * The pixel shader of the sky dome (fh1_native_deferred_sky).
 *
 * The fingerprint is the XXH3 of the shader's original container, not of the translated SPIR-V: it
 * identifies the game's shader and does not change when the library is regenerated (checked: the same
 * 28AA3CDAC6C19705 in the 'validated predicates' and 'fusion3' libraries, with SPIR-V of 1,859 and 3,306
 * words). It is the same mechanism as kFingerprintBrightPass and kFingerprintComposition.
 *
 * What it is, beyond doubt: its constant table declares CloudIntensity, SkyAlphaTag and Brightness, and
 * four samplers (DIFFUSEMAP, MISCMAP1, MISCMAP2, MISCMAP3) with 5 samples. It is the only shader in the
 * library (152 containers) that names the sky: it cannot be confused with any other. In the log it shows
 * up as PS n29.
 */
constexpr uint64_t kFingerprintSky = 0x28AA3CDAC6C19705ull;
// The sky dome is 480 indices = 160 triangles, measured in the log (the C6 diag line of the draw with
// PS n29). That is the mark that tells the dome draw apart from the other six that share its pixel
// shader, which were the ones that broke the image in an earlier version.
constexpr uint32_t kIndicesDomeSky = 480;
/*
 * How long the sky guard's test lasts, counting only the frames in which the dome appears (menus and
 * loading screens do not count, see CloseFrameOfTheGuardOfSky). 90 frames are about 3 seconds
 * of racing: enough not to decide on four samples, and short enough for the saving to start almost as
 * the race begins. All 90 must have exactly one dome: as soon as two appear in the same frame (the
 * earlier failure) it switches off for good.
 */
constexpr uint32_t kSkyFramesTest = 90;
constexpr uint32_t kSkyFramesWithOne = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;
constexpr VkDeviceSize kUboBytesPs = 256 * 16;  // NFSC: whole pixel constant bank
constexpr VkDeviceSize kUboBytesShared = 63 * 16;  // FH1: matches Fh1BlockShared (v[63])
constexpr uint32_t kRemapIdentity = 0xFFF;
constexpr uint32_t kMaxVerticesByDraw = uint32_t(1) << 20;
constexpr uint32_t kMemoryPhysical = 0x20000000;
// Times a statement in the timed draws (C6 substages).
#define NFSMW_SUB(k, ...) \
  do { \
    if (time_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)

/*
 * From 8 to 64. Each timed draw reads the clock about 20 times (Stage, NFSMW_SUB, samplers and
 * vertices). At 1 in 8 that averaged 0.15-0.4 us on every draw of the ring, and in the alley there are
 * 2,500-4,000 per frame. "C6 stages", "C6 substages" and "ms copying vertices" divide (or rescale) by
 * the draws that actually carried a stopwatch, so their scale does not change: they just have 8 times
 * fewer samples (~600-1,400 per second in a race).
 */
constexpr uint32_t kStopwatchEvery = 64;  // draws per stage-timed draw (power of 2)
constexpr size_t kCopiesByWarning = 64;   // copies queued between wake-ups of the copy thread (power of 2)
// Copies taken in one batch (fh1_native_uploads_help). In a race they are ~2 KB each: a batch is
// ~30 KB, about 20 us. That is how long the ring may have to wait for the thread, already with its
// priority lent.
constexpr size_t kCopiesByChunk = 16;
// Waits with pending copies in the observing phase (the ring waits as before and checks the marks)
// before it starts helping. In the menus that is a few seconds.
constexpr uint64_t kWaitsCopiesWatching = 512;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (fh1_native_uploads_thread).
struct WorkCopy {
  const uint8_t* source = nullptr;
  uint8_t* target = nullptr;
  uint32_t words = 0;
  xenos::Endian order = xenos::Endian::kNone;
};

void CopyVertices(const WorkCopy& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in a race, against 490 ms with the loop inside Draw).
  const uint8_t* const source = t.source;
  uint8_t* const target = t.target;
  const uint32_t words = t.words;
  const xenos::Endian order = t.order;
  if (order == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, order);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  }
}

// Allocator that leaves whatever resize adds uninitialized: indices_, converted_ and indices16_ are
// fully rewritten right after, and std::vector's zero fill was 1.4 % of the ring thread on PC.
template <class T>
struct WithoutInitialize : std::allocator<T> {
  using value_type = T;
  WithoutInitialize() = default;
  template <class U>
  WithoutInitialize(const WithoutInitialize<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = WithoutInitialize<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

using FnAddressBuffer = VkDeviceAddress(VKAPI_PTR*)(VkDevice, const VkBufferDeviceAddressInfo*);
using FnCopyImage = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);

float Float(uint32_t input_value) {
  return std::bit_cast<float>(input_value);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
uint32_t Log2Ceiling(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

uint32_t Log2Floor(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (fh1_native_fingerprint_sampling). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kSampleEvery are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepareTexture does not use it if it exceeds half. Tested
 * on PC: BytesSample matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
constexpr uint64_t kBlockSample = 4096;
constexpr uint64_t kSampleEvery = 8;

// Bytes FingerprintSample reads in a region of that size.
inline uint64_t BytesSample(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  return (last + kSampleEvery - 1) / kSampleEvery * kBlockSample + (bytes - last * kBlockSample);
}

// Chained XXH3 of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t FingerprintSample(const uint8_t* data, uint64_t bytes, uint64_t seed) {
  if (!bytes) {
    return seed;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  uint64_t fingerprint = seed;
  for (uint64_t b = 0; b < last; b += kSampleEvery) {
    fingerprint = XXH3_64bits_withSeed(data + b * kBlockSample, size_t(kBlockSample), fingerprint);
  }
  return XXH3_64bits_withSeed(data + last * kBlockSample, size_t(bytes - last * kBlockSample), fingerprint);
}

// First level of the packed tail: once the short side is 16 texels or less.
uint32_t LevelPacked(uint32_t width, uint32_t height) {
  const uint32_t l = Log2Ceiling(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
void OffsetPacked(uint32_t width, uint32_t height, uint32_t block, uint32_t level, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_width = Log2Ceiling(width);
  const uint32_t l2_height = Log2Ceiling(height);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
int32_t OffsetTile2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During race stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): ReadLevel called OffsetTile2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as OffsetTile2D (checked block by block on PC).
template <uint32_t kLog2>
void UntileLevel(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t displacement = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kBytes);
        ++column;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void ChangeOrderBytes(uint8_t* data, size_t bytes, uint32_t unit, uint32_t order) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && order != k8in16) || (unit != 2 && unit != 4) || order == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(data + i);
    uint8x16_t r;
    if (unit == 2 || order == k8in16) {
      r = vrev16q_u8(v);
    } else if (order == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(data + i, r);
  }
#endif
  if (unit == 2 || order == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (order == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (order == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// (graphics/pipeline/texture/util.cpp:438-459). pitch and height in blocks.
int32_t OffsetTile3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// USAGE_LOCATIONS of XenosRecomp (shader_recompiler.cpp).
int32_t LocationOfUse(uint8_t use, uint8_t index) {
  switch (use) {
    case 0:  // position
      // NFSC: indices 2..6 share locations with inputs the shaders that use them leave free
      // (same table as USAGE_LOCATIONS in shaders/XenosRecomp/shader_recompiler.cpp).
      switch (index) {
        case 0: return 0;
        case 1: return 15;
        case 2: return 1;
        case 3: return 2;
        case 4: return 3;
        case 5: return 10;
        case 6: return 5;
        default: return -1;
      }
    case 3:  // normal
      return index == 0 ? 1 : -1;
    case 6:  // tangente
      return index == 0 ? 2 : -1;
    case 7:  // binormal
      return index == 0 ? 3 : -1;
    case 5:  // texcoord
      return index < 4 ? 4 + index : (index < 8 ? 12 + (index - 4) : (index == 8 ? 1 : -1));
    case 10:  // color
      return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case 2:  // blend indices
      return index == 0 ? 9 : -1;
    case 1:  // blend weights
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
bool EntryWhole(uint8_t use) {
  // Only BLENDINDICES. Since the fh1_validado_normals library, normals, tangents and binormals are
  // float4: NFSMW stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen in the rear-view mirror).
  return use == 2;
}

uint32_t ComponentsVertex(uint32_t format) {
  switch (format) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}

// Components written by a fetch swizzle (the ones that are not 7), as a 4-bit mask.
uint32_t MaskWritten(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mask |= 1u << i;
    }
  }
  return mask;
}

// g_InputRemap code: the SPIR-V writes r[i] = entry[orig[i]] and the fetch patched by D3D writes
// r[i] = data[patched[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from patched[i]. 7 = the same component.
uint32_t CodeRemap(uint32_t original, uint32_t patched) {
  uint32_t code = kRemapIdentity;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

VkFormat FormatAttribute(uint32_t format, bool entry_whole, bool con_signo, bool whole,
                         bool red_blue, bool& r11g11b10) {
  r11g11b10 = false;
  const auto choose = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (entry_whole) {
      return con_signo ? sint : uint_;
    }
    if (whole) {
      return con_signo ? sscaled : uscaled;
    }
    return con_signo ? snorm : unorm;
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return red_blue ? choose(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : choose(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      if (red_blue) break;
      return choose(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (red_blue) break;
      if (!entry_whole && !REXCVAR_GET(fh1_vertices_10_11_11)) break;
      if (!entry_whole) {
        // FH1: positions and texcoords in this format: the raw bytes go through as exact 0-255 floats and remapInput
        // rebuilds and unpacks the word (remap code bit 12, set by the caller). An R32_SFLOAT input broke the picture
        // on AMD (bit patterns that look like denormals / NaNs do not survive).
        return VK_FORMAT_R8G8B8A8_USCALED;
      }
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return entry_whole || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return entry_whole || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return entry_whole && !red_blue ? (con_signo ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return entry_whole && !red_blue
                 ? (con_signo ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return entry_whole && !red_blue
                 ? (con_signo ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (entry_whole ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:  // k_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (entry_whole ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:  // k_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (entry_whole ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:  // k_32_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (entry_whole ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleAAAA = 3 | (3 << 3) | (3 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct FormatTexture {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint8_t block = 1;       // texels per block side
  uint8_t bytes = 1;        // bytes per block
  uint8_t unit_order = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleRGBA;
};

// Same host formats as the emulation (vulkan/texture_cache.cpp:123-397).
bool FormatTextureOf(uint32_t format, FormatTexture& f) {
  switch (format) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {VK_FORMAT_R8_UNORM, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 58:  // k_DXT3A: the alpha half of a DXT3 block (16 texels of 4 bits). No host format has it alone:
              // PrepareTexture widens each block to a BC2 one (same alpha half, empty color half). FH1's map
              // screen draws its circle selector with one.
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 8, 2, kSwizzleAAAA};
      return true;
    case 24:  // k_16
      f = {VK_FORMAT_R16_UNORM, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

bool IsDepth(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// GuestToHostSwizzle: the fetch constant's swizzle, mapped through the channels the host format has.
VkComponentMapping MappingComponents(uint32_t swizzle_guest, uint16_t swizzle_host) {
  VkComponentMapping mapping{};
  VkComponentSwizzle* output[4] = {&mapping.r, &mapping.g, &mapping.b, &mapping.a};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t input_value = (swizzle_guest >> (i * 3)) & 0x7;
    if (input_value <= 3) {
      const uint32_t host = (swizzle_host >> (input_value * 3)) & 0x7;
      *output[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + host);
    } else if (input_value == 4) {
      *output[i] = VK_COMPONENT_SWIZZLE_ZERO;
    } else {
      *output[i] = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return mapping;
}

struct AttributeVertices {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
};

struct BindingVertices {
  uint32_t slot;   // vertex fetch constant (0-95)
  uint32_t stride;  // bytes
};

struct EntryVertices {
  std::vector<AttributeVertices> attributes;
  std::vector<BindingVertices> bindings;
  std::array<uint32_t, 16> remaps{};  // g_InputRemap by location
  // FH1: a fetch takes its vertex index from a value the shader computes instead of the vertex index itself
  // (billboards: one stored vertex per quad, fetched with index / 4). See the vertex sources in Draw.
  bool index_computed = false;
  // FH1: fetches indexed by a register other than r0 (the animated people: bone index + frame offset). The shader
  // reads them from memory (fh1FetchRanked in shader_common.h); Draw uploads the stream and writes where it is.
  // rank = position of the fetch instruction among the declared fetches, by address (the translator's numbering).
  // stream = true for a full fetch: its stream (vertex fetch slot) is uploaded and its place written at the rank.
  struct FetchMemory {
    uint8_t rank = 0;
    uint8_t slot = 0;
    bool stream = false;
    uint32_t param = 0;  // stride in words | offset in words << 8 | format << 24 | signed << 30 | integer << 31
  };
  std::vector<FetchMemory> fetches_memory;
  uint32_t specialization = 0;
  uint64_t fingerprint = 0;
};

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).
// Returns empty if the module cannot be walked or there was nothing to remove.
std::vector<uint32_t> PruneWritesOfColor(const std::vector<uint32_t>& spirv, uint32_t& removed) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationIndex = 29;
  constexpr uint32_t kStoreOutput = 3;
  removed = 0;
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  std::unordered_set<uint32_t> con_location, discarded;
  // 1) decorations: Location 0..3 marks a color target; BuiltIn or Index != 0 rule it out (gl_FragDepth
  // and the second blend source are not touched).
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return {};
    }
    if (code == kOpDecorate && words >= 3) {
      const uint32_t target_2 = spirv[i + 1];
      const uint32_t decoration = spirv[i + 2];
      if (decoration == kDecorationLocation && words >= 4 && spirv[i + 3] <= 3) {
        con_location.insert(target_2);
      } else if (decoration == kDecorationBuiltIn ||
                 (decoration == kDecorationIndex && words >= 4 && spirv[i + 3] != 0)) {
        discarded.insert(target_2);
      }
    }
    i += words;
  }
  // 2) pointers to those targets: the output variable and whatever is derived from it through
  // OpAccessChain.
  std::unordered_set<uint32_t> pointers;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const uint32_t id = spirv[i + 2];
      if (con_location.count(id) && !discarded.count(id)) {
        pointers.insert(id);
      }
    } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && words >= 4 &&
               pointers.count(spirv[i + 3])) {
      pointers.insert(spirv[i + 2]);
    }
    i += words;
  }
  if (pointers.empty()) {
    return {};
  }
  // 3) the writes to those pointers are dropped. The rest of the module is copied as is: the output
  // variable stays declared and in the entry point's interface, which is valid even if it is never written.
  std::vector<uint32_t> output;
  output.reserve(spirv.size());
  output.insert(output.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    const bool outside = code == kOpStore && words >= 3 && pointers.count(spirv[i + 1]);
    if (outside) {
      ++removed;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  (void)kOpEntryPoint;
  return removed ? output : std::vector<uint32_t>();
}

/*
 * The same module, but declaring OpExecutionMode EarlyFragmentTests.
 *
 * In SPIR-V the execution modes have their own section, right after the OpEntryPoint instructions and
 * before the debug strings and decorations. So it is enough to insert the instruction after the module's
 * last OpExecutionMode (there is always at least one, OriginUpperLeft, which is what DXC emits). No new
 * ids are created, so the header's "bound" does not change and the module stays valid.
 *
 * Returns empty (and then the normal module is used) if the module has no fragment entry point, if it
 * already declared it, or if it writes gl_FragDepth (DepthReplacing): in that last case testing earlier
 * would change the result, because the Z being tested is computed by the shader itself.
 */
std::vector<uint32_t> WithTestsEarly(const std::vector<uint32_t>& spirv, const char*& reason) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kModelFragment = 4;
  constexpr uint32_t kModeEarly = 9, kModeDepthReplacing = 12;
  reason = "";
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    reason = "not SPIR-V";
    return {};
  }
  uint32_t entry = 0;
  size_t where = 0;  // first word after the execution mode section
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      reason = "malformed module";
      return {};
    }
    if (code == kOpEntryPoint && words >= 3) {
      if (spirv[i + 1] == kModelFragment) {
        if (entry) {
          reason = "more than one fragment entry point";
          return {};
        }
        entry = spirv[i + 2];
      }
      if (where < i + words) {
        where = i + words;
      }
    } else if (code == kOpExecutionMode && words >= 3) {
      if (spirv[i + 2] == kModeEarly) {
        reason = "already declared it";
        return {};
      }
      if (spirv[i + 2] == kModeDepthReplacing) {
        reason = "writes gl_FragDepth";
        return {};
      }
      if (where < i + words) {
        where = i + words;
      }
    }
    i += words;
  }
  if (!entry || !where) {
    reason = "no fragment entry point";
    return {};
  }
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 3);
  output.insert(output.end(), spirv.begin(), spirv.begin() + where);
  output.push_back((3u << 16) | kOpExecutionMode);
  output.push_back(entry);
  output.push_back(kModeEarly);
  output.insert(output.end(), spirv.begin() + where, spirv.end());
  return output;
}

struct KeyPipeline {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t entry = 0;
  uint32_t topology = 0;
  uint32_t specialization = 0;
  uint32_t formats[5] = {};
  uint32_t blend[4] = {};
  uint32_t masks = 0;
  uint32_t depth = 0;
  uint32_t rasterization = 0;
  // The key is hashed and compared byte by byte (PipelineOf). With 76 bytes of fields and 8-byte alignment
  // it had 4 bytes of uninitialized implicit padding: stack garbage that made identical keys differ and
  // created duplicate pipelines (125-134, and 193-203 in a later version, with no change in the image).
  uint32_t fill = 0;
  uint32_t fill2 = 0;
};
static_assert(std::has_unique_object_representations_v<KeyPipeline>,
              "KeyPipeline cannot have implicit padding: it is hashed and compared byte by byte");

/*
 * One pipeline of the prewarm list (fh1_native_pipelines_prewarm).
 *
 * What is needed to recreate it in another session exactly as the ring created it: its key as PipelineOf
 * looks it up; the fingerprint of its two shaders in the library (if the library has changed, key.vs
 * and key.ps are no longer the same shaders and the record is skipped); and its vertex input, which the
 * key only carries as a fingerprint. It is stored on disk byte for byte, so it cannot have implicit
 * padding.
 */
struct AttributeRegister {
  uint32_t location = 0;
  uint32_t binding = 0;
  uint32_t format = 0;  // VkFormat
  uint32_t offset = 0;
};

struct RegisterPipeline {
  static constexpr uint32_t kMaxAttributes = 16;
  static constexpr uint32_t kMaxBindings = 16;
  KeyPipeline key;
  uint64_t fingerprint_vs = 0;  // fh1::native::Shader::fingerprint
  uint64_t fingerprint_ps = 0;  // 0 without a fragment stage (key.ps == 0)
  uint32_t n_attributes = 0;
  uint32_t n_bindings = 0;
  AttributeRegister attributes[kMaxAttributes] = {};
  uint32_t strides[kMaxBindings] = {};
};
static_assert(std::has_unique_object_representations_v<RegisterPipeline>,
              "RegisterPipeline is saved to disk byte by byte: no implicit padding");

struct Texture {
  ImageNative image;
  uint32_t layers = 1;  // 6 for cubemaps
  uint32_t background = 0;  // slices of 3D textures; 0 for the rest
  uint64_t fingerprint = 0;
  uint64_t frame = UINT64_MAX;
  uint64_t fingerprint_raw = 0;  // XXH3 of the guest bytes (2D and cubemaps)
  uint64_t next = 0;     // frame of the next check
  uint32_t interval = 1;     // frames between checks: 1 to 32
  uint32_t deferrals = 0;  // consecutive checks deferred by the budget
  // Sample fingerprint (FingerprintSample) of the same content as fingerprint_raw if sample_valid, and how many
  // consecutive rechecks have been accepted on it alone (fh1_native_fingerprint_sampling).
  uint64_t fingerprint_sample = 0;
  bool sample_valid = false;
  uint8_t samples_consecutive = 0;
  bool needs_upload = false;
  std::vector<uint8_t> data;  // levels already laid out for the host: level after level and, in each, layer after layer
  uint32_t levels = 1;        // mip levels of the host image
  std::array<uint32_t, 16> offset_level{};  // data bytes up to each level
  uint64_t bytes = 0;          // what it counts in bytes_textures_ (for eviction)
  // 1 + its index in in_flight_ while the bind thread runs its vkBindImageMemory; 0 = no. While set, the
  // image exists but has no memory: no command may reference it until CollectBindings.
  uint32_t in_flight = 0;
  // Its data and fingerprint_raw are prepared by the hash thread (fh1_native_texture_fingerprint_thread). 0 = no;
  // 1 + its index in fingerprints_planned_ until UploadTexture; kWorkFingerprintPublished until CollectFingerprints
  // sets its fingerprint_raw. While nonzero it is not evicted and its fingerprint_raw is not valid yet.
  uint32_t fingerprint_work = 0;
  // Measurement only (fh1_native_diag_reuse): its shape without the address (when the image is
  // created), its address (only for the log), the content key it is filed under in live_by_content_
  // (0 = none) and whether it is new and its first fingerprint has not been measured yet.
  uint64_t shape_content = 0;
  uint64_t key_content = 0;
  uint32_t address = 0;
  bool content_by_measure = false;
};

struct ViewEntry {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t slot = 0;
  uint32_t heap = 0;  // 0 2D textures, 2 cubemaps
};

class DrawsVulkanImpl final : public DrawsVulkan {
 public:
  DrawsVulkanImpl(const VulkanDevice* vulkan_device, rex::memory::Memory* memory_block,
                    ContextTargets* context_id)
      : vulkan_device_(vulkan_device),
        dfn_(vulkan_device->functions()),
        device_(vulkan_device->device()),
        memory_(memory_block),
        context_(context_id) {}

  ~DrawsVulkanImpl() override {
    StopPrewarm();  // uses the pipeline cache and the layout: first
    StopCopies();
    StopBindings();  // in-flight vkBindImageMemory calls finish before any image is destroyed
    StopFingerprints();  // the hash thread writes to the upload buffer: join it before releasing that
    SaveCachePipelines();
    StopWriterCache();  // writes whatever is still pending
    for (auto& [key, par] : pipelines_) {
      dfn_.vkDestroyPipeline(device_, par.second, nullptr);
    }
    if (cache_pipelines_ != VK_NULL_HANDLE) {
      destroy_cache_(device_, cache_pipelines_, nullptr);
    }
    for (auto& [key, framebuffer] : framebuffers_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkFramebuffer framebuffer : fb_retired_) {  // the ones ForgetView retired
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& [key, pass] : passes_) {
      dfn_.vkDestroyRenderPass(device_, pass, nullptr);
    }
    for (auto& [entry, module_handle] : modules_) {
      dfn_.vkDestroyShaderModule(device_, module_handle, nullptr);
    }
    for (auto& [entry, module_handle] : modules_only_alpha_) {
      if (module_handle != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, module_handle, nullptr);
      }
    }
    for (auto& [entry, module_handle] : modules_z_early_) {
      if (module_handle != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, module_handle, nullptr);
      }
    }
    for (VkShaderModule module_handle : modules_variants_) {
      if (module_handle != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, module_handle, nullptr);
      }
    }
    for (auto& [key, view] : views_) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto& [key, texture] : textures_) {
      DestroyImage(texture.image);
    }
    for (auto& [key, par] : samplers_) {
      dfn_.vkDestroySampler(device_, par.first, nullptr);
    }
    for (ImageNative& empty_2 : empty_) {
      DestroyImage(empty_2);
    }
    // The pool's slabs are released after destroying every image that lives in them. The other way round
    // would free memory that the VkImages still have bound.
    pool_textures_.Finish();
    if (sampler_empty_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_empty_, nullptr);
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pool_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_ubo_, nullptr);
    if (layout_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout_ubo_, nullptr);
    for (VkDescriptorSetLayout layout : layouts_) {
      if (layout != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    for (const BufferUpload& s : shared_bufs_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.memory_block);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.memory_block != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.memory_block, nullptr);
    }
    if (streams_data_) dfn_.vkUnmapMemory(device_, streams_memory_);
    if (streams_buffer_ != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, streams_buffer_, nullptr);
    if (streams_memory_ != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, streams_memory_, nullptr);
    for (const BufferUpload& s : uploads_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.memory_block);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.memory_block != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.memory_block, nullptr);
    }
  }

  bool Initialize() {
    const auto& properties = vulkan_device_->properties();
    const std::pair<bool, const char*> requirements[] = {
        {properties.shaderInt64, "shaderInt64"},
        {properties.bufferDeviceAddress, "bufferDeviceAddress"},
        {properties.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {properties.shaderSampledImageArrayDynamicIndexing,
         "shaderSampledImageArrayDynamicIndexing"},
        {properties.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {properties.descriptorBindingSampledImageUpdateAfterBind,
         "descriptorBindingSampledImageUpdateAfterBind"},
        {properties.descriptorBindingUpdateUnusedWhilePending,
         "descriptorBindingUpdateUnusedWhilePending"},
    };
    for (const auto& [present, name] : requirements) {
      if (!present) {
        REXLOG_ERROR("[native] C6: the Vulkan device does not have {}: not drawing", name);
        return false;
      }
    }
    address_buffer_ = reinterpret_cast<FnAddressBuffer>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkGetBufferDeviceAddress"));
    copy_image_ = reinterpret_cast<FnCopyImage>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage"));
    LoadCachePipelines();
    LoadStateDynamic();  // dynamic state phases 1 and 2
    if (!address_buffer_ || !CreateUpload() || !CreateDescriptores()) {
      return false;
    }
    // The pool is created after CreateDescriptores (where textures_mb_max_ is read) and before CreateEmpty,
    // so the three empty images can already come from it. The slabs are prewarmed here, while the game is
    // still loading: a 32 MB memset during a race would be a 10-15 ms stutter.
    pool_textures_.Start(vulkan_device_, textures_mb_max_);
    return CreateEmpty();
  }

  bool Draw(const RequestDraw& p) override {
    VerdictVegetation(p);  // the ring's versus the game's (fh1_d3d_game_vegetation)
    // Stage stopwatch on 1 in kStopwatchEvery draws: reading the clock 12 times per draw took almost a
    // quarter of the ring thread's busy CPU on PC, and on the Switch each read costs more. The C6 averages
    // come from the sample.
    time_ = (++stopwatch_counter_ & (kStopwatchEvery - 1)) == 0;
    auto mark = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint32_t* r = p.register_values;
    if (!r || !p.vs) {
      return false;
    }
    // --- Primitive type ------------------------------------------------------
    const uint32_t mode_edram = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (mode_edram != uint32_t(xenos::EdramMode::kColorDepth) && mode_edram != 5) {
      return Reject(1, "EDRAM mode without color or depth");
    }
    // Mode 5 (depth only): the Xenos does not run the pixel shader (IsPixelShaderNeededWithRasterization,
    // graphics/util/draw.cpp:125-129). Without a PS there are no textures, pixel constants or alpha test,
    // and the pipeline has no fragment stage. The race shadows work this way, and some arrive with the PS
    // object set to 0.
    const EntryShader* ps =  // can be removed if it writes no color and does not discard
        mode_edram == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && mode_edram == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }
    // Diagnostic: PS whose draws are skipped (fh1_native_diag_skip_ps).
    const std::string& skip = diag_skip_ps_text_;
    if (skip != skip_text_) {
      skip_text_ = skip;
      skip_ps_.clear();
      uint32_t number = 0;
      bool there_is = false;
      for (char c : skip + ",") {
        if (c >= '0' && c <= '9') {
          number = number * 10 + uint32_t(c - '0');
          there_is = true;
        } else if (there_is) {
          skip_ps_.insert(number);
          number = 0;
          there_is = false;
        }
      }
      if (!skip_ps_.empty()) {
        REXLOG_WARN("[native] C6 diag: PS {} are not drawn", skip_text_);
      }
    }
    if (ps && !skip_ps_.empty() && skip_ps_.count(ps->number)) {
      return true;
    }
    {  // NFSC debug: only the listed PS are drawn into the big scene targets (surface pitch 0x500 = 1280 wide)
      static const std::string only_list = REXCVAR_GET(fh1_debug_only_ps);
      static const std::unordered_set<uint32_t> solo_ps = [] {
        std::unordered_set<uint32_t> c;
        uint32_t n = 0;
        bool there_is = false;
        for (char ch : only_list + ",") {
          if (ch >= '0' && ch <= '9') { n = n * 10 + uint32_t(ch - '0'); there_is = true; }
          else if (there_is) { c.insert(n); n = 0; there_is = false; }
        }
        return c;
      }();
      if (!solo_ps.empty() && ps && (r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF) == uint32_t(REXCVAR_GET(fh1_debug_only_ps_pitch)) &&
          !solo_ps.count(ps->number)) {
        return true;
      }
    }
    const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t type = initiator & 0x3F;
    const uint32_t source_2 = (initiator >> 6) & 0x3;
    const bool indices32 = (initiator >> 11) & 0x1;
    const uint32_t count = initiator >> 16;
    if (!count) {
      return true;
    }
    VkPrimitiveTopology topology;
    bool quads = false;
    bool rectangles = false;  // NFSC: RECTANGLE_LIST: 3 vertices per rectangle, the 4th made by a geometry shader
    bool accepts_reset = false;
    switch (type) {
      case 1:  // NFSC: point list (Carbon's light-visibility probes, colour writes off). The translated vertex shaders
               // do not write a point size, so this needs VK_KHR_maintenance5 (size 1.0 then); otherwise they stay rejected.
        if (!vulkan_device_->properties().maintenance5) {
          return Reject(101, "primitive type not supported yet");
        }
        topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        break;
      case 2:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        accepts_reset = true;
        break;
      case 4:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 5:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        accepts_reset = true;
        break;
      case 6:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        accepts_reset = true;
        break;
      case 13:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        quads = true;
        break;
      case 8:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        rectangles = true;
        break;
      default:
        if (type == 1) {  // NFSC diagnostic: what the point-list draws look like
          static uint32_t noted = 0;
          static uint64_t frame_seen = 0;
          static uint32_t in_frame = 0;
          const uint64_t frame_current = frame_;
          if (frame_current != frame_seen) {
            frame_seen = frame_current;
            in_frame = 0;
          }
          if (frame_current >= 240 && frame_current < 243 && in_frame++ < 6 && noted < 40) {
            ++noted;
            const uint32_t ps_size = r[gr::XE_GPU_REG_PA_SU_POINT_SIZE], mm = r[gr::XE_GPU_REG_PA_SU_POINT_MINMAX];
            const uint32_t sq = r[gr::XE_GPU_REG_SQ_PROGRAM_CNTL];
            REXLOG_INFO("[fh1] point list: count {} VS n{} PS n{} point size w={:.2f} h={:.2f} (half, px) minmax "
                        "{:.2f}..{:.2f} SQ_PROGRAM_CNTL {:08X} (param_gen {} vs_export_mode {}) clip {:08X} vte "
                        "{:08X} sc_mode {:08X} source {}",
                        count, p.vs ? p.vs->number : 0u, ps ? ps->number : 0u, float(ps_size >> 16) / 16.0f,
                        float(ps_size & 0xFFFF) / 16.0f, float(mm & 0xFFFF) / 16.0f, float(mm >> 16) / 16.0f, sq,
                        (sq >> 18) & 1, (sq >> 24) & 7, r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL],
                        r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL], source_2);
          }
        }
        return Reject(100 + type, "primitive type not supported yet");
    }
    if (source_2 == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Reject(2, "immediate indices");
    }
    // Clipping disabled on the Xenos: the position may come in pixels (videos). It is drawn with a viewport
    // the size of the render target and the transform in the VS, like the emulation
    // (graphics/util/draw.cpp:341-363).
    const bool without_clip = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t mode_sc = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((mode_sc >> 3) & 0x3) == 2 && ((mode_sc >> 5) & 0x7) != 2) {
      return Reject(4, "polygons drawn as points or lines");
    }

    const EntryVertices* entry = EntryOf(p);
    if (!entry) {
      return false;
    }
    Stage(0, mark);
    std::chrono::steady_clock::time_point t_indices_185 = mark;  // C6 substages 15-18

    // --- Targets ---------------------------------------------------------------
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    const uint32_t mask_register =
        mode_edram == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {
        gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t keys[5] = {};
    uint32_t masks = 0;
    bool there_is_target = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mask = (mask_register >> (i * 4)) & 0xF;
      if (!mask || !ps || !((ps->outputs >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t format = (info >> 16) & 0xF;
      {
        using F = xenos::ColorRenderTargetFormat;  // NFSC: HDR formats aliased to 8-bit for now (see targets.cpp)
        if (format != uint32_t(F::k_8_8_8_8) && format != uint32_t(F::k_8_8_8_8_GAMMA) &&
            format != uint32_t(F::k_2_10_10_10) && format != uint32_t(F::k_2_10_10_10_FLOAT) &&
            format != uint32_t(F::k_2_10_10_10_AS_10_10_10_10) &&
            format != uint32_t(F::k_2_10_10_10_FLOAT_AS_16_16_16_16) &&
            // FH1: wide formats (FormatHostFh1 in fh1_native_targets.cpp).
            format != uint32_t(F::k_16_16) && format != uint32_t(F::k_16_16_16_16) &&
            format != uint32_t(F::k_16_16_FLOAT) && format != uint32_t(F::k_16_16_16_16_FLOAT) &&
            format != uint32_t(F::k_32_FLOAT) && format != uint32_t(F::k_32_32_FLOAT)) {
          return Reject(200 + format, "color target format not supported yet");
        }
      }
      keys[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(format) << 16) |
                  (uint64_t((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 0x3) << 14) | pitch;  // FH1: + MSAA
      masks |= mask << (i * 4);
      there_is_target = true;
    }
    /*
     * Draws that cannot change a single pixel (fh1_native_skip_invisible).
     *
     * Two cases, read from this draw's own registers without any heuristics:
     *
     *  - Blending is "0 x source + 1 x destination" (ADD) on the written channels: the result is the
     *    destination as is. It is checked per channel group because the mask may write only RGB or only
     *    alpha, and then the other group's factors do not matter.
     *  - The alpha test function is 0 = NEVER: alphaTestValue (shader_common.h) returns -1 and clip() kills
     *    every fragment, before depth is written.
     *
     * If there is also no depth or stencil left to write, the whole draw is unnecessary: it is dropped. If
     * it writes depth it is drawn anyway and only counted, because setting the color mask to 0 would send it
     * down the "no color" path (kSpecOnlyAlpha), where fh1_shadows_without_vegetation drops the whole draw and
     * its depth would be lost. Removing the color write saves too little to be worth it.
     * The pruning is skipped while an occlusion query is open: the game reads those samples.
     */
    if (skip_invisible_ && masks) {
      static constexpr uint32_t kBlendOf[4] = {
          gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alpha_never =
          ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;  // function NEVER
      uint32_t masks_useful = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
        if (!mask_i) {
          continue;
        }
        const uint32_t m = r[kBlendOf[i]] & 0x1FFF1FFF;
        const bool copy_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool copy_alpha =
            ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        const bool writes_rgb = (mask_i & 0x7) != 0 && !copy_color;
        const bool writes_alpha = (mask_i & 0x8) != 0 && !copy_alpha;
        if (writes_rgb || writes_alpha) {
          masks_useful |= mask_i << (i * 4);
        }
      }
      if ((!masks_useful || alpha_never) && !occlusion_open_) {
        // With the NEVER function even the depth write dies (clip() runs in the shader, before the merger),
        // so the draw leaves no trace of any kind. With the blending that copies the destination, the draw can
        // only be dropped if it writes neither Z nor stencil either.
        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool leaves_trace =
            !alpha_never && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        ++counts_z_[alpha_never ? kInvisibleAlpha : kInvisibleBlend];
        if (!leaves_trace) {
          return true;
        }
        ++counts_z_[kInvisibleSoloColor];  // drawn anyway: only counts what is left to gain
      }
    }
    // With no color to write, the pixel shader can only have an effect by discarding pixels (alpha test or
    // its own kill) or by writing depth. If it does neither, the pipeline has no fragment stage and the image
    // is identical: the GPU saves shading the whole shadow map.
    // Of the draws that do write color, how many can use early depth rejection and how many force shading
    // before testing. That is what decides whether a depth pre-pass would help at all.
    if (masks && ps && there_is_target) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      const bool discards = test_alpha || ps->discards || (ps->outputs & 0x10);
      if (pitch >= 1600) {
        // shadow map: already counted separately
      } else if (discards) {
        ++scene_with_discard_;
      } else {
        ++scene_without_discard_;
      }
    }
    bool only_alpha = false;
    if (!masks && ps) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      if (!test_alpha && !ps->discards && !(ps->outputs & 0x10)) {
        ++draws_ps_useless_;
        if (cvars_by_frame_ ? without_ps_without_color_frame_ : SinPsSinColor()) {
          ps = nullptr;
        }
      } else {
        ++draws_ps_needed_;
        // Diagnostic: in the shadow map the pixel shader reads constant c1 (g_bShadowMapAlphaEnabled). With
        // that constant at 0 it neither samples nor discards: the stage would be unnecessary.
        if (pitch >= 1600) {
          (Float(r[kRegConstantsPs + 4]) != 0.0f ? ++shadows_alpha_active_ : ++shadows_alpha_off_);
        }
        // Needed for the alpha test or a kill, but its color goes nowhere: it is compiled without those
        // writes.
        if (cvars_by_frame_ ? ps_only_alpha_frame_ : PsOnlyAlpha()) {
          only_alpha = true;
        }
      }
    }
    /*
     * Shadow map vegetation discard, as early as possible.
     *
     * This same discard used to live 640 lines further down, right before the pipeline lookup. So these
     * draws were paid in full and then thrown away: index conversion, vertex sources, PrepareTexture for all
     * their samplers, upload buffer allocation, pass change, vertex copy, index and constant memcpy,
     * viewport, scissor and pipeline lookup.
     *
     * And it is not a handful of draws: 604 per frame are dropped, 31 % of those that come in (the
     * `draws_ps_needed` counter matches the lost ones exactly across twelve race intervals). At
     * ~8.5 us per incoming draw, that is 3-5 ms.
     *
     * Careful when measuring this: the divisor of `C6 stages` is the recorded draws (~1,370), not the
     * incoming ones (~1,970), even though the label says otherwise. Multiplying by the incoming ones inflates
     * the budget by 45 %.
     *
     * The occlusion guard is free and removes the only doubt: with a game occlusion query open the draw
     * counts even if it is not visible, so there it takes the long path and the usual discard drops it.
     */
    const bool vegetation_early = only_alpha && without_vegetation_ && !occlusion_open_;
    // The ring's verdict for the fh1_d3d_game_vegetation guard (VerdictVegetation) must be this same
    // decision. With the settings read on every draw (cvars_by_frame_ false) the alternation can change
    // between the two reads: then no comparison is made.
    if (vegetation_computed_ && cvars_by_frame_ && vegetation_ring_ != vegetation_early) {
      vegetation_detail_.early = vegetation_early;
      NoteVegetationModel(vegetation_flags_, vegetation_detail_);
    }
    if (vegetation_early) {
      ++draws_vegetation_soon_;
      return true;
    }
    const uint32_t control_depth = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (control_depth & 0x3) {  // stencil o z
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      keys[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                  (uint64_t((info >> 16) & 0x1) << 16) |
                  (uint64_t((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 0x3) << 14) | pitch;  // FH1: + MSAA
      there_is_target = true;
      // FH1: a depth-only rectangle that writes Z without testing it is a fill of that EDRAM. The game clears a
      // color target with it (ContextTargets::NoteFillDepth); the pitch is the one BeginPass draws at.
      if (mode_edram == 5 && type == 8 && (control_depth & 0x76) == 0x76) {
        const uint32_t msaa_fill = (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 0x3;
        const bool fill_4x = msaa_fill == uint32_t(xenos::MsaaSamples::k4X) &&
                             (REXCVAR_GET(fh1_msaa_4x_as_1x) ||
                              (pitch <= 640 && REXCVAR_GET(fh1_msaa_4x_clears_as_1x)));
        context_->NoteFillDepth(info & 0xFFF, (info >> 16) & 0x1, fill_4x ? pitch * 2 : pitch);
      }
    }
    if (!there_is_target || !pitch) {
      // Diagnostic: a draw inside an occlusion query that writes nothing counts no samples.
      if (occlusion_open_ && pitch >= 640 && warnings_occlusion_draw_ < 8) {
        ++warnings_occlusion_draw_;
        REXLOG_INFO("[native] C2 occlusion draw {}: NO TARGET (not drawn): VS n{} PS n{} type {} count {} pitch {} "
                    "mask {:08X} depth {:08X} EDRAM mode {}",
                    warnings_occlusion_draw_, p.vs->number, ps ? int(ps->number) : -1, type, count, pitch,
                    mask_register, control_depth, mode_edram);
      }
      return true;  // writes nothing visible
    }
    // FPS test fh1_native_skip_shadows: the shadow map is depth-only and 1600 wide. With
    // fh1_native_skip_shadows_toggle_s = N they are skipped in the odd N-second intervals.
    const bool target_shadows =  // also used for its depth bias
        pitch >= 1600 && keys[4] && !keys[0] && !keys[1] && !keys[2] && !keys[3];
    if (target_shadows) {
      // Step 2 of the 30 FPS guard. Removing the whole pass does not flicker; skipping it on 2 of every
      // 3 frames does, which is why that step no longer exists.
      bool skip = fh1::guard30::WithoutShadows(REXCVAR_GET(fh1_native_skip_shadows));
      const int32_t toggle = REXCVAR_GET(fh1_native_skip_shadows_toggle_s);
      if (!skip && toggle > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_shadows_)
                                  .count();
        skip = (seconds / toggle) % 2 == 1;
        if (skip != shadows_skipped_) {
          shadows_skipped_ = skip;
          REXLOG_INFO("[native] shadow test: {} (frame {})",
                      skip ? "without shadows" : "with shadows", frame_);
        }
      }
      if (skip) {
        return true;
      }
    }

    CutSubstage(15, t_indices_185);  // render targets and discards
    // --- Vertex and index range -------------------------------------------------
    const uint32_t displacement = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool reset = accepts_reset && ((mode_sc >> 21) & 0x1);
    // Depth bias as in the emulation with host render targets (GetPreferredFacePolygonOffset,
    // graphics/util/draw.cpp:92-118).
    float scale_bias = 0.0f, offset_bias = 0.0f;
    if (type >= 4) {  // poligonos
      if (((mode_sc >> 11) & 0x1) && !(mode_sc & 0x1)) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((mode_sc >> 12) & 0x1) && !((mode_sc >> 1) & 0x1) && scale_bias == 0.0f &&
          offset_bias == 0.0f) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((mode_sc >> 13) & 0x1) {
      scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      offset_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    // Constant scaled by the guest format's minimum value (2^24-1 for D24S8, 2^24 for D24FS8) and slope
    // from subpixels to pixels (vulkan/command_processor.cpp:5786-5799).
    float bias[2] = {
        offset_bias * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                    ? float(uint32_t(1) << 24)
                                    : float((uint32_t(1) << 24) - 1)),
        scale_bias * (1.0f / 16.0f)};
    // fh1_native_shadows_bias_*, only in the shadow map and only if the game does not set its own.
    if (target_shadows && type >= 4 && bias[0] == 0.0f && bias[1] == 0.0f &&
        (shadows_bias_constant_ != 0 || shadows_bias_pending_ != 0)) {
      bias[0] = float(shadows_bias_constant_) * 1000.0f;
      bias[1] = float(shadows_bias_pending_) * 0.1f;
    }
    const bool with_bias = bias[0] != 0.0f || bias[1] != 0.0f;
    if (with_bias && warnings_bias_ < 8 &&
        std::memcmp(bias, bias_warned_, sizeof(bias)) != 0) {
      ++warnings_bias_;
      std::memcpy(bias_warned_, bias, sizeof(bias));
      REXLOG_INFO("[native] C6: depth bias: scale {} offset {} (mode {:08X}) -> constant {} slope {}",
                  scale_bias, offset_bias, mode_sc, bias[0], bias[1]);
    }
    CutSubstage(16, t_indices_185);  // depth bias
    uint32_t vmin = UINT32_MAX;
    uint32_t vmax = 0;
    bool con_indices = false;
    bool indices_de_16 = false;  // in indices16_ instead of indices_
    indices_.clear();
    if (source_2 == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto order = static_cast<xenos::Endian>(size >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (count > (size & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(count) * bytes >
                                               kMemoryPhysical) {
        if (warnings_indices_ < 8) {
          ++warnings_indices_;
          REXLOG_WARN("[native] C6 diag: indices: count {} words {} VGT_DMA_SIZE {:08X} VGT_DMA_BASE {:08X} 32 "
                      "bits {} type {} VS n{} PS n{}",
                      count, size & 0xFFFFFF, size, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                      type, p.vs->number, ps ? int(ps->number) : -1);
        }
        return Reject(7, "indices outside their buffer");
      }
      const uint8_t* data = memory_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t index_reset = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (!indices32 && !reset && !displacement && !quads &&
          (order == xenos::Endian::k8in16 || order == xenos::Endian::kNone)) {
        // The normal case in NFSMW: 16 bits without restart or offset. They are uploaded as 16 bits, with a
        // branch-free loop the compiler can vectorize.
        indices16_.resize(count);
        uint16_t* output = indices16_.data();
        uint32_t minimum = 0xFFFF;
        uint32_t maximum = 0;
        // With hand-written NEON and its guard (IndicesDe16, fh1_native_indices_neon).
        IndicesDe16(data, count, output, order == xenos::Endian::k8in16, minimum, maximum);
        vmin = minimum;
        vmax = maximum;
        indices_de_16 = true;
      } else {
        indices_.resize(count);
        uint32_t* output = indices_.data();
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t raw;
            std::memcpy(&raw, data + size_t(i) * 4, 4);
            v = xenos::GpuSwap(raw, order) & 0xFFFFFF;
          } else {
            uint16_t raw;
            std::memcpy(&raw, data + size_t(i) * 2, 2);
            v = xenos::GpuSwap(raw, order);
          }
          if (reset && v == index_reset) {
            output[i] = UINT32_MAX;
            continue;
          }
          v = (v + displacement) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          output[i] = v;
        }
      }
      if (vmin > vmax) {
        return true;  // resets only
      }
      con_indices = true;
    } else {
      vmin = displacement;
      vmax = displacement + count - 1;
      if (quads) {
        indices_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
          indices_[i] = displacement + i;
        }
        con_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesByDraw) {
      return Reject(8, "vertex range too large");
    }
    if (quads) {
      const size_t n = indices_.size() / 4;
      converted_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &converted_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(converted_);
    }
    CutSubstage(17, t_indices_185);  // indices
    // The indices stay as they come: vkCmdDrawIndexed subtracts vmin through vertexOffset.
    const uint32_t vertices = vmax - vmin + 1;

    // Bytes of each vertex binding in guest memory.
    struct Source {
      const uint8_t* data;
      xenos::Endian order;
      uint32_t bytes;
      uint64_t address;  // physical, of the first byte used
      bool expanded = false;  // FH1: data is a copy made here (vertices_expanded_), not guest memory
    };
    std::array<Source, 16> sources{};
    if (entry->bindings.size() > sources.size()) {
      return Reject(9, "too many vertex streams");
    }
    // FH1: billboards (crowd, trees, vegetation) are quad lists with one stored vertex per quad: the shader fetches
    // vertex index / 4 and places the four corners from the index. The host fetches by the index itself, so each
    // stored vertex is repeated four times in the copy. Only that case is known: quads.
    if (entry->index_computed && !quads) {
      {  // FH1 diagnostic: which draws these are (once per VS / type)
        static std::unordered_set<uint64_t> seen;
        if (seen.size() < 24 && seen.insert((uint64_t(p.vs->number) << 8) | type).second) {
          std::string f;
          for (const ElementVertex& e : p.vs->elements) {
            const size_t i = size_t(e.instruction) * 3;
            if (i + 2 >= p.vs_microcode.size()) continue;
            f += fmt::format(" | {}{}@{} {:08X} {:08X} {:08X}", NameUse(e.use), e.index_use, e.instruction,
                             p.vs_microcode[i], p.vs_microcode[i + 1], p.vs_microcode[i + 2]);
          }
          std::string ix;
          for (size_t i = 0; i < indices_.size() && i < 16; ++i) ix += fmt::format(" {}", indices_[i]);
          for (size_t i = 0; i < indices16_.size() && i < 16 && indices_de_16; ++i) ix += fmt::format(" {}", indices16_[i]);
          REXLOG_INFO("[fh1] computed index: VS n{} PS n{} type {} count {} indexed {} vmin {} vmax {} indices:{} fetches:{}",
                      p.vs->number, ps ? int(ps->number) : -1, type, count, con_indices, vmin, vmax, ix, f);
        }
      }
      return Reject(317, "vertex fetch with a computed index outside a quad list");
    }
    if (entry->index_computed) {
      vertices_expanded_.clear();
    }
    VkDeviceSize bytes_vertices = 0;
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const BindingVertices& binding = entry->bindings[b];
      const uint32_t d0 = r[kRegFetch + binding.slot * 2];
      const uint32_t d1 = r[kRegFetch + binding.slot * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Reject(10, "invalid vertex fetch constant");
      }
      const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      if (entry->index_computed) {
        const uint32_t first = vmin / 4, last = vmax / 4;
        const uint64_t start = uint64_t(first) * binding.stride;
        if (start + uint64_t(last - first + 1) * binding.stride > available ||
            address + start + uint64_t(last - first + 1) * binding.stride > kMemoryPhysical) {
          return Reject(12, "vertices outside memory");
        }
        const uint8_t* guest = memory_->TranslatePhysical(uint32_t(address + start));
        std::vector<uint8_t>& copy = vertices_expanded_.emplace_back(size_t(vertices) * binding.stride);
        for (uint32_t j = 0; j < vertices; ++j) {
          std::memcpy(copy.data() + size_t(j) * binding.stride,
                      guest + size_t((vmin + j) / 4 - first) * binding.stride, binding.stride);
        }
        sources[b] = {copy.data(), static_cast<xenos::Endian>(d1 & 0x3), uint32_t(copy.size()), address + start,
                      true};
        bytes_vertices += (copy.size() + 3) & ~VkDeviceSize(3);
        continue;
      }
      const uint64_t start = uint64_t(vmin) * binding.stride;
      uint64_t needed = uint64_t(vertices) * binding.stride;
      if (start + needed > available) {
        Notify(11, "vertices past the end of their buffer: clipped");
        needed = available > start ? (available - start) / binding.stride * binding.stride : 0;
      }
      if (!needed || address + start + needed > kMemoryPhysical) {
        return Reject(12, "vertices outside memory");
      }
      sources[b] = {memory_->TranslatePhysical(uint32_t(address + start)),
                     static_cast<xenos::Endian>(d1 & 0x3), uint32_t(needed),
                     address + start};
      bytes_vertices += (needed + 3) & ~VkDeviceSize(3);
    }

    // FH1: streams the shader reads from memory (EntryVertices::fetches_memory): the whole stream goes to the
    // upload buffer once per frame (the vertex dedupe finds it for the other draws that use it).
    struct SourceMemory {
      const uint8_t* data = nullptr;
      xenos::Endian order = xenos::Endian::kNone;
      uint32_t bytes = 0;
      uint64_t address = 0;
      uint32_t rank = 0;
      uint32_t cached = 0;  // offset in the streams buffer (StreamInCache)
    };
    std::array<SourceMemory, 32> sources_memory{};
    size_t sources_memory_n = 0;
    for (const EntryVertices::FetchMemory& m : entry->fetches_memory) {
      if (!m.stream) {
        continue;
      }
      const uint32_t d0 = r[kRegFetch + uint32_t(m.slot) * 2];
      const uint32_t d1 = r[kRegFetch + uint32_t(m.slot) * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Reject(10, "invalid vertex fetch constant");
      }
      const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      if (!available || available > (uint64_t(16) << 20) || address + available > kMemoryPhysical ||
          sources_memory_n >= sources_memory.size()) {
        return Reject(320, "stream read by the shader: empty, larger than 16 MB or outside memory");
      }
      sources_memory[sources_memory_n++] = {memory_->TranslatePhysical(uint32_t(address)),
                                            static_cast<xenos::Endian>(d1 & 0x3), uint32_t(available), address,
                                            m.rank};
    }
    // Kept across frames in a buffer of their own when they fit (the bones of the festival people are 2.4 MB that
    // never change); otherwise copied to the upload buffer like any vertices.
    bool streams_cached = sources_memory_n != 0;
    for (size_t i = 0; i < sources_memory_n && streams_cached; ++i) {
      SourceMemory& source = sources_memory[i];
      streams_cached = StreamInCache(source.data, source.bytes, source.address, source.order, source.cached);
    }
    if (!streams_cached) {
      for (size_t i = 0; i < sources_memory_n; ++i) {
        bytes_vertices += sources_memory[i].bytes + 16;
      }
    }

    {  // FH1 diagnostic: the first packed k_10_11_11 inputs, their fetch fields and first three vertices
      static uint32_t warnings_packed = 0;
      if (warnings_packed < 12) {
        for (const AttributeVertices& a : entry->attributes) {
          if (a.location != 0 || !(entry->remaps[a.location] & 0x1000u)) continue;  // positions only
          const Source& source = sources[a.binding];
          const uint32_t stride = entry->bindings[a.binding].stride;
          std::string v;
          for (uint32_t k = 0; k < 3 && uint64_t(k) * stride + a.offset + 4 <= source.bytes; ++k) {
            uint32_t word;
            std::memcpy(&word, source.data + uint64_t(k) * stride + a.offset, 4);
            word = xenos::GpuSwap(word, source.order);
            const int32_t x = int32_t(word << 21) >> 21, y = int32_t((word >> 11) << 21) >> 21,
                          z = int32_t(word) >> 22;
            v += fmt::format(" {:08X}=({},{},{})", word, x, y, z);
          }
          REXLOG_INFO("[fh1] packed input VS n{} loc {} code {:08X} stride {} offset {} endian {} vmin {}:{}", p.vs->number,
                      a.location, entry->remaps[a.location], stride, a.offset, int(source.order), vmin, v);
          std::string f;
          for (const ElementVertex& e : p.vs->elements) {
            const size_t i = size_t(e.instruction) * 3;
            if (i + 2 >= p.vs_microcode.size()) continue;
            f += fmt::format(" | use{}.{}@{} orig {:08X} {:08X} {:08X} patched {:08X} {:08X} {:08X}", e.use, e.index_use,
                             e.instruction, p.vs->microcode_shader[i], p.vs->microcode_shader[i + 1],
                             p.vs->microcode_shader[i + 2], p.vs_microcode[i], p.vs_microcode[i + 1],
                             p.vs_microcode[i + 2]);
          }
          REXLOG_INFO("[fh1] packed VS n{} fetches:{}", p.vs->number, f);
          if (++warnings_packed >= 12) break;
        }
      }
    }
    // Diagnostic: one line per combination of VS, PS and render target (at most 32).
    if (diagnostics_ < 32) {
      const uint64_t key_diagnostic = (uint64_t(p.vs->number) << 44) ^
                                         (uint64_t(ps ? ps->number + 1 : 0) << 32) ^ keys[0] ^
                                         (without_clip ? 1 : 0);
      if (diagnosed_.insert(key_diagnostic).second) {
        ++diagnostics_;
        float position[4] = {};
        for (const AttributeVertices& a : entry->attributes) {
          if (a.location != 0) {
            continue;
          }
          const Source& source = sources[a.binding];
          if (source.bytes >= a.offset + 16) {
            for (uint32_t k = 0; k < 4; ++k) {
              uint32_t word;
              std::memcpy(&word, source.data + a.offset + k * 4, 4);
              position[k] = Float(xenos::GpuSwap(word, source.order));
            }
          }
          break;
        }
        if (con_indices) {  // NFSC: first indices of the draw and the vertex range
          std::string first_3;
          const size_t n = indices_de_16 ? indices16_.size() : indices_.size();
          for (size_t i = 0; i < n && i < 18; ++i) {
            first_3 += fmt::format(" {}", indices_de_16 ? uint32_t(indices16_[i]) : indices_[i]);
          }
          REXLOG_INFO("[fh1] indices VS n{} PS n{} type {} count {} range {}..{} (32-bit {}, DMA size {:08X}, base {:08X}):{}",
                      p.vs->number, ps ? int(ps->number) : -1, type, count, vmin, vmax, indices32,
                      r[gr::XE_GPU_REG_VGT_DMA_SIZE], r[gr::XE_GPU_REG_VGT_DMA_BASE], first_3);
        }
        if (type == 8) {  // NFSC: the three corners of a rectangle-list draw (clears are drawn this way)
          for (const AttributeVertices& a : entry->attributes) {
            if (a.location != 0) {
              continue;
            }
            const Source& source = sources[a.binding];
            const uint32_t stride = entry->bindings[a.binding].stride;
            std::string corners;
            for (uint32_t v = 0; v < 3; ++v) {
              if (source.bytes < v * stride + a.offset + 16) {
                break;
              }
              corners += " (";
              for (uint32_t k = 0; k < 4; ++k) {
                uint32_t word;
                std::memcpy(&word, source.data + v * stride + a.offset + k * 4, 4);
                corners += fmt::format("{}{:.1f}", k ? " " : "", Float(xenos::GpuSwap(word, source.order)));
              }
              corners += ")";
            }
            REXLOG_INFO("[fh1] rect-list draw VS n{} PS n{} surface {:08X} corners:{}", p.vs->number,
                        ps ? int(ps->number) : -1, r[gr::XE_GPU_REG_RB_SURFACE_INFO], corners);
            break;
          }
        }
        REXLOG_INFO(
            "[native] C6 diag: VS n{} PS n{} type {} count {} {} VTE {:08X} CLIP {:08X} vp x {}+-{} y {}+-{} z "
            "{}+{} window {:08X} sc {:08X} surface {:08X} color0 {:08X} mask {:08X} depth {:08X} edram {} vtx "
            "{:08X} pos0 ({:.3f} {:.3f} {:.3f} {:.3f}){}",
            p.vs->number, ps ? int(ps->number) : -1, type, count,
            source_2 == 0 ? "indices" : "auto",
            r[gr::XE_GPU_REG_PA_CL_VTE_CNTL], r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL],
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]), r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            mode_sc, r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_COLOR_INFO],
            r[gr::XE_GPU_REG_RB_COLOR_MASK], control_depth, mode_edram,
            r[gr::XE_GPU_REG_PA_SU_VTX_CNTL], position[0], position[1], position[2], position[3],
            without_clip ? " [no clipping]" : "");
      }
    }

    CutSubstage(18, t_indices_185);  // vertices and diagnostics
    Stage(1, mark);
    // --- Textures and samplers ----------------------------------------------------
    uint32_t shared[kWordsShared] = {};
    for (uint32_t i = 0; i < 16; ++i) {
      shared[kWordExpScale + i] = 0x3F800000u;  // 1.0: slots nobody prepares (vertex shader fetches)
    }
    VkDeviceSize bytes_textures = 0;
    DiscardFingerprintsPlanned();  // those of the previous draw that never reached UploadTexture
    textures_a_upload_.clear();
    const auto t_samplers =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    /*
     * fh1_native_lazy_depth. In the final composite without blur, the depth sample (HEIGHTMAP)
     * is dead: C2 is told so that requesting it does not force a copy. The same value decides
     * kSpecWithoutBlur further down, so textures and specialization always agree.
     */
    const bool composition_without_blur =
        ps && ps->shader && ps->shader->fingerprint == kFingerprintComposition && without_blur_frame_;
    if (composition_without_blur) {
      context_->ReadsOfDepthDead(true);
    }
    bool reads_reflection = false;  // fh1_reflection_visibility
    // FH1: the vertex shader's textures too. Its sampler register N (0-3, what its constant table lists and the
    // shader's s<N> slot) is fetch constant 16 + N. The final composite's vertex shader reads the adapted luminance
    // that way and picks the exposure with it; left unbound it read black and the picture came out too bright. A
    // slot the pixel shader also uses stays the pixel shader's.
    std::array<SamplerShader, 32> samplers_draw;
    std::array<uint8_t, 32> samplers_fetch;
    size_t samplers_draw_n = 0;
    if (ps) {
      for (const SamplerShader& sampler : ps->samplers) {
        if (sampler.reg_entry < 16 && samplers_draw_n < samplers_draw.size()) {
          samplers_fetch[samplers_draw_n] = uint8_t(sampler.reg_entry);
          samplers_draw[samplers_draw_n++] = sampler;
        }
      }
    }
    if (vs_textures_) {
      const size_t of_ps = samplers_draw_n;
      for (const SamplerShader& sampler : p.vs->samplers) {
        bool listed = sampler.reg_entry >= 16;
        for (size_t i = 0; i < of_ps && !listed; ++i) {
          listed = samplers_draw[i].reg_entry == sampler.reg_entry;
        }
        if (!listed && samplers_draw_n < samplers_draw.size()) {
          samplers_fetch[samplers_draw_n] = uint8_t(sampler.reg_entry + 16);
          samplers_draw[samplers_draw_n++] = sampler;
        }
      }
    }
    for (size_t sampler_i = 0; sampler_i < samplers_draw_n; ++sampler_i) {
      const SamplerShader& sampler = samplers_draw[sampler_i];
      const uint32_t* fetch = r + kRegFetch + uint32_t(samplers_fetch[sampler_i]) * 6;
      // fh1_reflection_visibility. Here and not in TextureResolved, which, because of the caches, only sees
      // the first draw of each frame that samples it. Without checking the dimension: over-counting only
      // means the reflection gets drawn.
      if (measure_visibility_ && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((fetch[1] & 0xFFFFF000u) & 0x1FFFFFFFu) == fh1::reflection_demand::kAddress) {
        reads_reflection = true;
      }
      ++samplers_prepared_;
      bool sin_cache = false;  // depth requested but not sampled, kept out of the caches
      // Per-register cache: same fetch constant, same frame and no C2 copies in between.
      CacheSampler& cache = cache_samplers_[sampler.reg_entry];
      // Valid until cache.valid_until (with the cross-frame cache disabled, only its own frame)
      if ((cache_between_frames_ ? frame_ <= cache.valid_until : cache.frame == frame_) &&
          cache.generation == generation_textures_ &&
          std::memcmp(cache.fetch.data(), fetch, sizeof(cache.fetch)) == 0) {
        shared[cache.heap * 16 + sampler.reg_entry] = cache.slot;
        shared[48 + sampler.reg_entry] = cache.sampler;
        WriteInvSize(shared, sampler.reg_entry, cache.width, cache.height);
        std::memcpy(shared + kWordExpScale + sampler.reg_entry, &cache.exp_scale, sizeof(float));
        ++samplers_cache_;
        continue;
      }
      uint32_t slot_texture = 0;
      uint32_t heap = 0;
      uint32_t slot_sampler = 0;
      uint32_t width_host = 0, height_host = 0;
      float exp_scale = 1.0f;
      // Second cache, keyed by the whole fetch constant within the same frame and generation: a register
      // changes texture between draws, but textures repeat a lot within a frame, and PrepareTexture already
      // queued their upload the first time.
      CacheSampler& por_fetch =
          cache_fetch_[XXH3_64bits(fetch, sizeof(uint32_t) * 6) & (cache_fetch_.size() - 1)];
      uint64_t valid_until = frame_;
      if ((cache_between_frames_ ? frame_ <= por_fetch.valid_until : por_fetch.frame == frame_) &&
          por_fetch.generation == generation_textures_ &&
          std::memcmp(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch)) == 0) {
        slot_texture = por_fetch.slot;
        heap = por_fetch.heap;
        slot_sampler = por_fetch.sampler;
        valid_until = por_fetch.valid_until;
        width_host = por_fetch.width;
        height_host = por_fetch.height;
        exp_scale = por_fetch.exp_scale;
        ++samplers_cache_fetch_;
      } else {
        // Why the table misses ("C6 cache by fetch" report, every 10 s).
        if (std::memcmp(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch)) != 0) {
          ++(por_fetch.frame == UINT64_MAX ? fetch_misses_empty_ : fetch_misses_collision_);
        } else if (por_fetch.generation != generation_textures_) {
          ++fetch_misses_generation_;
        } else {
          ++fetch_misses_stale_;
        }
        bool sampling_point = false;
        PrepareTexture(fetch, slot_texture, heap, bytes_textures, sampling_point, valid_until,
                        width_host, height_host, exp_scale);
        slot_sampler = SlotSampler(fetch, sampling_point);
        por_fetch.frame = frame_;
        por_fetch.generation = generation_textures_;
        std::memcpy(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch));
        por_fetch.slot = slot_texture;
        por_fetch.heap = heap;
        por_fetch.sampler = slot_sampler;
        por_fetch.valid_until = valid_until;
        por_fetch.width = width_host;
        por_fetch.height = height_host;
        por_fetch.exp_scale = exp_scale;
        // A depth requested without being sampled is not stored in the caches: the next draw that really
        // samples it has to go through TextureResolved, which is where it is recorded if it was deferred.
        if (composition_without_blur && sampling_point) {
          por_fetch.frame = 0;
          por_fetch.valid_until = 0;
          valid_until = 0;
          sin_cache = true;
        }
      }
      shared[heap * 16 + sampler.reg_entry] = slot_texture;
      shared[48 + sampler.reg_entry] = slot_sampler;
      WriteInvSize(shared, sampler.reg_entry, width_host, height_host);
      std::memcpy(shared + kWordExpScale + sampler.reg_entry, &exp_scale, sizeof(float));
      cache.exp_scale = exp_scale;
      cache.frame = frame_;
      cache.generation = generation_textures_;
      std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      cache.slot = slot_texture;
      cache.heap = heap;
      cache.sampler = slot_sampler;
      cache.valid_until = valid_until;
      cache.width = width_host;
      cache.height = height_host;
      if (sin_cache) {
        cache.frame = 0;  // with valid_until at 0 it is not valid across frames either
      }
    }
    if (composition_without_blur) {
      context_->ReadsOfDepthDead(false);
    }
    /*
     * fh1_native_shadow_minimum. If this draw samples the shadow map texture with cars (the game's
     * texture[0], 086AE000) and C2 holds it with only the cars, its pair (the world map) goes in the 3D
     * index word of that register (the shadow map is 2D: that word is unused) and the pipeline carries
     * kSpecShadowMinimum: tfetch2DShadowMin takes the minimum of the two, the same value the copy would have
     * left. While C2 is observing, the pair is the texture itself (the same texel), so the pipelines with
     * the bit already exist when applying starts. Every read is reported to C2, which decides and watches.
     */
    bool shadow_minimum_draw = false;
    if (ps && !ps->samplers.empty()) {
      if (const uint32_t dir_cars = context_->AddressShadowCars()) {
        for (const SamplerShader& s : ps->samplers) {
          if (s.reg_entry >= 16) {
            continue;
          }
          const uint32_t* f = r + kRegFetch + uint32_t(s.reg_entry) * 6;
          if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
              (((f[1] >> 12) << 12) & 0x1FFFFFFF) != dir_cars) {
            continue;
          }
          const bool capable = ps->shadow_minimum && s.map_shadows &&
                             ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked);
          const ImageNative* pair = context_->PartnerShadowCars(capable, ps->number);
          if (!pair) {
            continue;
          }
          const uint32_t slot_pair =
              SlotView(pair->image, pair->format, (f[3] >> 1) & 0xFFF, kSwizzleRRRR);
          if (!slot_pair) {
            context_->PartnerShadowCars(false, ps->number);  // no view, no minimum: C2 records it
            continue;
          }
          shared[16 + s.reg_entry] = slot_pair;
          shadow_minimum_draw = true;
        }
      }
    }

    if (time_) {  // C6 substages: the sampler loop inside the textures stage
      sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_samplers)
                                  .count());
      ++sub_n_[14];
    }
    Stage(2, mark);
    // --- Upload buffer space ----------------------------------------------------------
    const VkDeviceSize bytes_indices =
        indices_de_16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;
    // With a single binding the copy goes to a multiple of the stride: up to stride - 4 bytes of padding.
    const VkDeviceSize gap_base_zero =
        vertices_base_zero_ && entry->bindings.size() == 1 ? VkDeviceSize(entry->bindings[0].stride) : 0;
    const VkDeviceSize needed = gap_base_zero + bytes_vertices + bytes_indices + bytes_textures +
                                    2 * VkDeviceSize(kRegistersConstants) * 4 +
                                    kWordsShared * 4 + 64 * 8 +
                                    // With UBOs the blocks go whole and aligned to alignment_ubo_
                                    (use_ubo_ ? kUboBytesVs + kUboBytesPs + kUboBytesShared +
                                                     3 * alignment_ubo_
                                               : 0);
    if (needed > kSizeUpload) {
      return Reject(13, "draw larger than the upload buffer");
    }
    const bool shared_full =
        shared_separate_ && use_ubo_ &&
        shared_used_ + kUboBytesShared + alignment_ubo_ > kSharedConstantsSize;
    if (upload_used_ + needed > kSizeUpload || shared_full) {
      const auto before_submission = std::chrono::steady_clock::now();
      const bool sent = context_->SendYWait();
      ++submissions_full_;
      ns_submissions_full_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - before_submission)
                                        .count());
      if (!sent) {
        return false;
      }
    }

    // --- Render pass ----------------------------------------------------------------
    /*
     * No XXH3 on every draw (fh1_native_fast_pass_key). If the 40 bytes of render targets are the
     * same ones that produced pass_key_ (BeginPass stores them next to it), their XXH3 is pass_key_.
     * Otherwise it is computed as usual: the pass change decision is the same as before in every case,
     * collisions included.
     */
    uint64_t key_pass;
    if (key_pass_fast_ && pass_keys_valid_ && std::memcmp(keys, pass_keys_, sizeof(keys)) == 0) {
      const uint64_t n = ++keys_pass_fast_;
      key_pass = (n <= kKeysPassACheck || (n & 4095) == 0) ? CheckKeyPass(keys, n) : pass_key_;
    } else {
      key_pass = XXH3_64bits(keys, sizeof(keys));
    }
    if (!pass_active_ || key_pass != pass_key_ ||
        pass_generation_ != context_->GenerationCommands()) {
      const auto before_pass = std::chrono::steady_clock::now();
      if (pass_generation_ != context_->GenerationCommands()) {
        ++passes_by_generation_;
      } else if (!pass_active_ && key_pass == pass_key_) {
        ++passes_resumed_;
      } else {
        ++passes_by_target_;
      }
      FinishPass();
      // The pass change is split. FinishPass closes the previous pass; BeginPass finds or creates the
      // render targets (it may create images, record barriers and clear them), builds the render pass and
      // the framebuffer, and opens the pass.
      const auto after_finish = std::chrono::steady_clock::now();
      if (time_) {
        stages_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            after_finish - before_pass).count());
      }
      const bool started = BeginPass(r, keys, pitch, key_pass);
      const auto after_pass = std::chrono::steady_clock::now();
      ns_passes_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      // The pass change, separate from what the stage costs on each draw.
      if (time_) {
        stages_ns_[7] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      }
      if (!started) {
        return false;
      }
    }
    const VkCommandBuffer cmd = pass_commands_;
    if (recording_generation_ != context_->GenerationCommands()) {
      recording_generation_ = context_->GenerationCommands();
      pipeline_bound_ = VK_NULL_HANDLE;
      eds_valid_ = false;  // phases 1 and 2: nothing is set in the new buffer
      key_bound_valid_ = false;  // New command buffer (CountChangePipeline)
      sets_bound_ = false;
      ubo_bound_ = false;
      state_recorded_ = false;
      bindings_recorded_ = 0;  // different command buffer: nothing is bound
    }

    Stage(3, mark);
    // --- Uploads: textures, vertices, indices and constants ------------------------------
    for (Texture* texture : textures_a_upload_) {
      if (!UploadTexture(*texture)) {
        return false;
      }
    }
    std::array<VkDeviceSize, 16> offsets_vertices{};
    // If the guest has waited for the GPU since the last draw, it may legally have rewritten an already
    // referenced range: what was recorded is no longer valid.
    if (dedupe_active_) {
      const uint32_t sinc = g_synchronizations_ring.load(std::memory_order_relaxed);
      if (sinc != dedupe_sinc_vista_) {
        dedupe_sinc_vista_ = sinc;
        dedupe_.Forget();
      }
    }
    const auto before_vertices =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const Source& source = sources[b];
      if (diag_vertices_repeated_) {
        NoteVerticesRepeated(source.address, source.bytes, uint32_t(source.order), source.data);
      }
      VkDeviceSize offset;
      // If this same range was already copied in this frame, its place in the upload buffer is reused and
      // nothing is copied. See fh1_native_vertex_dedupe.h.
      if (dedupe_active_ && !source.expanded &&
          dedupe_.Find(source.address, source.bytes, uint32_t(source.order), offset)) {
        offsets_vertices[b] = offset;
        continue;
      }
      // With a single binding, at a multiple of its stride (fh1_native_zero_based_vertices).
      if (vertices_base_zero_ && entry->bindings.size() == 1) {
        ReserveMultiple(source.bytes, entry->bindings[0].stride, offset);
      } else {
        Reserve(source.bytes, 4, offset);
      }
      const WorkCopy work{source.data, upload_data_ + offset, source.bytes / 4, source.order};
      // An expanded source lives only during this draw: copied now, never queued nor remembered.
      if (source.expanded || !copies_active_ || !EnqueueCopy(work)) {
        CopyVertices(work);
      }
      bytes_vertices_ += source.bytes;
      offsets_vertices[b] = offset;
      if (dedupe_active_ && !source.expanded) {
        dedupe_.Note(source.address, source.bytes, uint32_t(source.order), offset);
      }
    }
    if (time_) {
      ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - before_vertices)
                                   .count());
    }
    // FH1: streams read by the shader. g_GuestBase (shared words 154-155) is the upload buffer's device address and
    // g_FetchRankAddress(rank) the copy's offset in it; the copy is already in host byte order (endian bits 0). An
    // offset under 4 means "no stream" to the shader, so the first bytes of the buffer are never used.
    for (const EntryVertices::FetchMemory& m : entry->fetches_memory) {
      shared[kWordFetchRankParam + m.rank] = m.param;
    }
    for (size_t i = 0; i < sources_memory_n; ++i) {
      const SourceMemory& source = sources_memory[i];
      if (streams_cached) {
        const uint64_t base = streams_address_;
        std::memcpy(shared + 154, &base, sizeof(base));
        shared[kWordFetchRankAddress + source.rank] = source.cached;
        continue;
      }
      VkDeviceSize offset;
      if (!(dedupe_active_ && dedupe_.Find(source.address, source.bytes, uint32_t(source.order), offset))) {
        if (upload_used_ < 16) {
          upload_used_ = 16;
        }
        Reserve(source.bytes, 4, offset);
        const WorkCopy work{source.data, upload_data_ + offset, source.bytes / 4, source.order};
        CopyVertices(work);
        bytes_vertices_ += source.bytes;
        if (dedupe_active_) {
          dedupe_.Note(source.address, source.bytes, uint32_t(source.order), offset);
        }
      }
      const uint64_t base = upload_address_;
      std::memcpy(shared + 154, &base, sizeof(base));
      shared[kWordFetchRankAddress + source.rank] = uint32_t(offset);
    }
    VkDeviceSize offset_indices = 0;
    if (con_indices) {
      Reserve(bytes_indices, 4, offset_indices);
      bytes_indices_uploaded_ += bytes_indices;
      std::memcpy(upload_data_ + offset_indices,
                  indices_de_16 ? static_cast<const void*>(indices16_.data())
                                : static_cast<const void*>(indices_.data()),
                  size_t(bytes_indices));
    }
    // Only the registers each shader reads (EntryShader::constants_bytes). With the same generation, the
    // copy is reused if it already covers what this shader needs.
    const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constants_bytes, kRegistersConstants * 4);
    if (constants_vs_generation_ != p.generation_constants_vs ||
        constants_vs_epoch_ != epoch_upload_ || bytes_vs > constants_vs_bytes_) {
      // Why they are uploaded again (measurement only; each upload is another set 4 offset).
      ++reuploads_vs_[constants_vs_generation_ != p.generation_constants_vs ? 0
                      : constants_vs_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      // With UBOs the block goes whole (the driver may read the full range) and aligned to the device minimum
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, use_ubo_ ? alignment_ubo_ : 16,
               constants_vs_offset_);
      std::memcpy(upload_data_ + constants_vs_offset_, r + kRegConstantsVs, bytes_vs);
      constants_vs_generation_ = p.generation_constants_vs;
      constants_vs_epoch_ = epoch_upload_;
      constants_vs_bytes_ = bytes_vs;
    }
    const uint32_t bytes_ps =
        ps ? std::min<uint32_t>(ps->constants_bytes, kRegistersConstants * 4) : 0;
    if (ps && (constants_ps_generation_ != p.generation_constants_ps ||
               constants_ps_epoch_ != epoch_upload_ || bytes_ps > constants_ps_bytes_)) {
      ++reuploads_ps_[constants_ps_generation_ != p.generation_constants_ps ? 0
                      : constants_ps_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, use_ubo_ ? alignment_ubo_ : 16,
               constants_ps_offset_);
      std::memcpy(upload_data_ + constants_ps_offset_, r + kRegConstantsPs, bytes_ps);
      constants_ps_generation_ = p.generation_constants_ps;
      constants_ps_epoch_ = epoch_upload_;
      constants_ps_bytes_ = bytes_ps;
    }
    CopyCompositionHandled(ps, r, bytes_ps);  // fh1_color_filter (see the function)

    Stage(4, mark);
    // --- Viewport, scissor and shared constants --------------------------------------------
    /*
     * The framing (viewport, ndc and scissor), cached (fh1_native_framing_cache).
     *
     * The viewport, the ndc and the scissor are a function of the framing registers (PA_CL_VTE_CNTL,
     * PA_CL_VPORT_*, PA_SC_WINDOW_OFFSET/SCISSOR, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL:
     * IsRegisterOfFraming in fh1_native_system.cpp, which bumps generation_framing on every path that
     * writes them) and of the pass size and scale (pass_serie_). In a race the framing really changes
     * 0.07-0.14 times per draw ("C6 generations"). The depth bias is not included: it depends on
     * PA_SU_POLY_OFFSET_*, RB_DEPTH_INFO and the primitive type, which do not bump that generation.
     */
    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    VkViewport viewport{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    VkRect2D scissor{};
    uint32_t framing_empty = 0;
    const bool framing_in_cache = framing_cache_ && framing_valid_ &&
                                   p.generation_framing == framing_generation_ && pass_serie_ == framing_pass_serie_;
    bool check_framing = false;
    if (framing_in_cache) {
      const uint64_t n = ++framing_hits_;
      check_framing = n <= kFramingsACheck || (n & 4095) == 0;
      ++framing_hits_report_;
    }
    if (framing_in_cache && !check_framing) {
      viewport = framing_viewport_;
      std::memcpy(ndc, framing_ndc_, sizeof(ndc));
      scissor = framing_scissor_;
      framing_empty = framing_empty_;
    } else {
      framing_empty = ComputeFraming(r, vte, without_clip, mode_sc, viewport, ndc, scissor);
      ++framing_calculations_report_;
      if (check_framing) {
        CompareFraming(viewport, ndc, scissor, framing_empty);  // with the stored values, before overwriting them
      }
      framing_viewport_ = viewport;
      std::memcpy(framing_ndc_, ndc, sizeof(ndc));
      framing_scissor_ = scissor;
      framing_empty_ = framing_empty;
      framing_generation_ = p.generation_framing;
      framing_pass_serie_ = pass_serie_;
      framing_valid_ = true;
    }
    if (framing_empty != 0) {
      return true;  // empty viewport or scissor: as before
    }

    uint32_t specialization = entry->specialization;
    if (composition_without_blur) {  // the same value that decided the textures
      specialization |= kSpecWithoutBlur;
    }
#ifndef FH1_WITHOUT_VARIANTS_GLOW
    if (ps && ps->shader && ps->shader->fingerprint == kFingerprintBrightPass) {
      const int mode = GlowSky();
      specialization |= mode == 1 ? kSpecGlowNatural : mode == 2 ? kSpecGlowSoft : 0;
    }
#endif
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float threshold_alpha = 0.0f;
    uint32_t function_alpha = 7;  // always
    // The 8 Xenos functions (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) with alphaTestValue
    // (fh1_validado_normals library). Without a PS (mode 5) there is no test.
    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      threshold_alpha = Float(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      function_alpha = control_color & 0x7;
      specialization |= 0x2;
      // The function goes in the pipeline, not in the constants. RB_COLORCONTROL was already part of the key,
      // so this creates no pipelines that did not already exist.
      specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaOffset;
    } else if (ps && ((control_color >> 4) & 0x1) && REXCVAR_GET(fh1_native_alpha_to_mask)) {
      // FH1: alpha to mask (RB_COLORCONTROL bit 4) without an alpha test: the crowd and the foliage are cut out by
      // coverage of the 4x MSAA samples. The scene is drawn with one sample here, so the pixel is kept when half of
      // the samples or more would be covered (alpha >= 0.5). Without this the billboards were solid rectangles.
      threshold_alpha = 0.5f;
      function_alpha = 6;
      specialization |= 0x2;
      specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaOffset;
    }
    shared[64] = (r[kRegBooleans] & 0xFFFF) | ((r[kRegBooleans + 4] & 0xFFFF) << 16);
    shared[65] = 0;  // g_SwappedTexcoords
    for (uint32_t i = 0; i < 8; ++i) {  // FH1: every boolean (FH1_BOOL in shader_common.h)
      shared[244 + i] = r[kRegBooleans + i];
    }
    // NFSC: the live loop constants (SHADER_CONSTANT_LOOP_00-31) for the loops of shaders without definitions.
    std::memcpy(&shared[122], &r[gr::XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 32 * sizeof(uint32_t));
    {  // NFSC diagnostic: loop constants as the shaders see them (logged when one changes, at most 40 lines)
      static uint32_t previous[32] = {};
      static uint32_t noted = 0;
      for (uint32_t i = 0; i < 32 && noted < 40; ++i) {
        const uint32_t v = r[gr::XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i];
        if (v != previous[i]) {
          previous[i] = v;
          ++noted;
          REXLOG_INFO("[fh1] loop constant {} = {:08X} (count {}, start {}, step {}) PS n{}", i, v, v & 0xFF,
                      (v >> 8) & 0xFF, int8_t(v >> 16), ps ? int(ps->number) : -1);
        }
      }
    }
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
      // FH1: half a guest pixel is a whole host pixel in a 4x pass drawn at twice the scale (pass_msaa_scale_).
      const float middle[2] = {pass_msaa_scale_ / viewport.width, -pass_msaa_scale_ / std::abs(viewport.height)};
      std::memcpy(&shared[66], middle, sizeof(middle));
    }
    std::memcpy(&shared[68], &threshold_alpha, sizeof(threshold_alpha));
    shared[69] = function_alpha;  // g_AlphaFunction
    std::memcpy(&shared[70], ndc, sizeof(ndc));
    std::copy(entry->remaps.begin(), entry->remaps.end(), shared + 74);
    VkDeviceSize offset_shared;
    /*
     * How many draws really change the shared constants.
     *
     * This block is 488 bytes that are zeroed, filled, compared and, if they changed, copied twice, once
     * into memory without CPU caching (2654 MB/s). With ~2,400 draws per frame that is ~4.7 MB of traffic
     * per frame on the thread that is already at 96 % of a core.
     *
     * Which fix applies depends on the count: if almost no draw changes the block, the comparison is the
     * waste; if almost all do, the double memcpy is. They are two different fixes, and counting costs one
     * increment.
     */
    ++shared_looked_;
    const auto t_shared =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (shared_epoch_ == epoch_upload_ &&
        std::memcmp(shared, shared_previous_, sizeof(shared)) == 0) {
      offset_shared = shared_offset_;  // the same as the previous draw
    } else {
      ++shared_changed_;
      if (shared_separate_ && use_ubo_) {
        // To the separate CPU-cached buffer; binding 2 of set 4 points to it.
        offset_shared = (shared_used_ + alignment_ubo_ - 1) & ~(alignment_ubo_ - 1);
        shared_used_ = offset_shared + kUboBytesShared;
        std::memcpy(shared_data_ + offset_shared, shared, sizeof(shared));
      } else {
        Reserve(use_ubo_ ? kUboBytesShared : kWordsShared * 4, use_ubo_ ? alignment_ubo_ : 16,
                 offset_shared);
        std::memcpy(upload_data_ + offset_shared, shared, sizeof(shared));
      }
      std::memcpy(shared_previous_, shared, sizeof(shared));
      shared_offset_ = offset_shared;
      shared_epoch_ = epoch_upload_;
    }
    if (time_) {
      sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_shared)
                                  .count());
      ++sub_n_[12];
    }

    // --- Pipeline ---------------------------------------------------------------------------
    KeyPipeline key;
    key.vs = p.vs->number + 1;
    key.ps = ps ? ps->number + 1 : 0;  // 0: mode 5, no fragment stage
    key.entry = entry->fingerprint;
    key.topology = uint32_t(topology);
    if (rectangles) {
      key.fill |= 1;  // NFSC: this pipeline has the rectangle-list geometry stage
    }
    if (use_ubo_) {
      specialization |= kSpecConstantsUbo;  // the shaders read from set 4
    }
    if (inv_size_tex_) {
      specialization |= kSpecInvSizeTex;
    }
    if (pcf_cheap_) {
      specialization |= kSpecPcfCheap;
    }
    if (only_alpha && ps) {
      // If vegetation is excluded from the shadow map, this whole draw is unnecessary.
      if (without_vegetation_) {
        return true;
      }
      specialization |= kSpecOnlyAlpha;
    }
    /*
     * Test depth before shading where it costs nothing.
     *
     * A pixel shader that can discard (alpha test or its own kill) forces the hardware to shade first and
     * test Z afterwards: otherwise a discarded fragment would already have written its depth. But that only
     * matters if the draw writes depth. If it only tests it (smoke, particles, glass, decals, lights),
     * testing earlier is exact: there is no write to move, the shader still discards the color, and the GPU
     * stops shading what is hidden.
     *
     * The counters are kept even with the setting off, so the report says the same in both halves of an A/B
     * test, and the "CANNOT because they write depth" line measures exactly what a depth pre-pass
     * would have to fix.
     */
    if (ps && keys[4] && masks) {
      const bool test_z = ((control_depth >> 1) & 0x1) != 0;
      const bool writes_z = test_z && ((control_depth >> 2) & 0x1) != 0;
      const bool uses_stencil = (control_depth & 0x1) != 0;
      const bool ps_writes_z = (ps->outputs & 0x10) != 0;
      const bool late_2 = (specialization & 0x2) || ps->discards || ps_writes_z;
      if (!test_z) {
        // Without a depth test there is nothing to move earlier.
      } else if (!late_2) {
        ++counts_z_[kZAlreadyEarly];
      } else if (writes_z || ps_writes_z) {
        ++counts_z_[kZWritesZ];
      } else if (uses_stencil) {
        ++counts_z_[kZStencil];
      } else if (occlusion_open_) {
        ++counts_z_[kZOcclusion];  // the game reads those samples: leave it alone
      } else if (ps->shader && ps->shader->fingerprint != kFingerprintBrightPass &&
                 ps->shader->fingerprint != kFingerprintComposition) {
        ++counts_z_[kZSet];
        if (z_early_) {
          specialization |= kSpecZEarly;
        }
      }
    }
    // fh1_native_shadow_minimum. The shadow map pair bit (see above, textures).
    if (shadow_minimum_draw) {
      specialization |= kSpecShadowMinimum;
    }
    /*
     * fh1_native_shadow_minimum. In the car pass of the shadow map, C2 needs to know whether this draw
     * leaves in the depth buffer exactly the minimum of what was there and of its fragments: no stencil, no
     * occlusion query and, if it writes Z, a NEVER, LESS or LEQUAL test. The game's register
     * (RB_DEPTHCONTROL) is checked, not the pipeline key. BeginPass checks that the pass is depth-only.
     */
    if (pass_cars_shadow_) {
      const uint32_t control_z_cars = keys[4] ? control_depth : 0;
      const bool writes_z_cars = ((control_z_cars >> 1) & 0x1) && ((control_z_cars >> 2) & 0x1);
      const uint32_t function_z_cars = (control_z_cars >> 4) & 0x7;
      context_->DrawOfCarsShadow(!(control_z_cars & 0x1) && !occlusion_open_ &&
                                          (!writes_z_cars || function_z_cars == 0 || function_z_cars == 1 ||
                                           function_z_cars == 3),
                                      control_z_cars);
    }
    key.specialization = specialization;
    std::copy(std::begin(pass_formats_), std::end(pass_formats_), std::begin(key.formats));
    static constexpr uint32_t kBlend[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        key.blend[i] = r[kBlend[i]] & 0x1FFF1FFF;
      }
    }
    key.masks = masks;
    key.depth = keys[4] ? control_depth : 0;
    if (key.depth && occlusion_open_ && pitch >= 640 && !masks &&
        REXCVAR_GET(fh1_native_test_occlusion_always)) {
      key.depth |= 0x7 << 4;  // test: Z function always
    }
    // FH1: a rectangle list has no front or back on the console: it is never culled. With the game's culling
    // applied, the rectangles of the motion-blur velocity pass were dropped and the empty velocity texture smeared
    // the whole scene.
    key.rasterization = (mode_sc & (rectangles ? 0x4 : 0x7)) | (reset ? 0x8 : 0) | (with_bias ? 0x10 : 0);
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Looked up with KeyOfLookup (phase 0a: canonical form; phases 1 and 2: without the state set through
    // vkCmdSet*). key stays raw: the deferred sky (opaque_in_all), the counter and the dynamic state read
    // it.
    NFSMW_SUB(11, pipeline = PipelineDe(KeyOfLookup(key), *entry, p));
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
#if REX_PLATFORM_SWITCH
    // The pipeline about to be bound, prefetched into the cache several us before its vkCmdBindPipeline
    // (Mesa patch p06; PRFM only: it reads and writes nothing and cannot fail).
    if (nvk_preload_app_ && pipeline != pipeline_bound_ && vk_switch_preload_pipeline) {
      vk_switch_preload_pipeline(pipeline);
    }
#endif

    Stage(5, mark);
    /*
     * The three dynamic state values that used to be computed inside the recording block are computed here,
     * without recording anything. They are pure computations on the draw's registers; they are needed
     * earlier so they can be saved if this draw is the sky and gets deferred (see below).
     */
    // Scissor test. All the state and the draw are sent as usual; the only change is that there are no
    // pixels to shade. It breaks the image: only for measuring the per-draw floor.
    VkRect2D scissor_final = scissor;
    if (const uint32_t category_scissor = scissor_test_; category_scissor != 0) {
      const uint32_t category = CategoryOfTarget(pitch, keys);
      static constexpr uint32_t kCategoryOf[5] = {0, kGpuShadows, kGpuScene, kGpu320,
                                                   kGpuSceneWithoutDepth};
      if (category_scissor <= 4 && category == kCategoryOf[category_scissor]) {
        scissor_final.extent = {1, 1};
      }
    }
    /*
     * Record how far down the game draws in this render target. It only grows, so the pass is never opened
     * smaller than what has already been seen drawn.
     */
    if (area_util_) {
      const uint32_t until = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));
      const uint32_t rounded = (until + 63u) & ~63u;
      uint32_t& pointed_2 = HeightUsefulOf(pitch);  // without a map lookup on every draw
      if (rounded > pointed_2) {
        pointed_2 = rounded;
      }
    }
    const float blend_constant[4] = {Float(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    const uint32_t stencil_front = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t stencil_back = ((control_depth >> 7) & 0x1)
                                       ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                       : stencil_front;
    /*
     * The sky, deferred until after the opaque draws (fh1_native_deferred_sky).
     *
     * How it is recognized, and why it cannot be mistaken for another draw. Three conditions at once, none of
     * them a magic position or draw count:
     *
     *  1. The pixel shader is the sky's, by the fingerprint of its original container (kFingerprintSky). It is
     *     the only one of the library's 152 shaders whose constant table names CloudIntensity and
     *     SkyAlphaTag. The fingerprint does not depend on the SPIR-V translation or on the library order.
     *  2. The draw writes color and is opaque on all its targets: blending is "1 x source + 0 x destination"
     *     (ADD) on the channels the mask writes, exactly the criterion PipelineOf uses to decide
     *     blendEnable. Opaque = it does not read the existing color, so whatever was painted before it does
     *     not matter.
     *  3. It tests depth and does not write it, and there is no stencil. Without a Z write, deferring it
     *     cannot change what later draws see; with the test on, in its new place the Z test discards it
     *     where the world used to overwrite it.
     *
     * If any of the three fails (another version of the game, another state) the draw takes the usual path
     * and nothing happens.
     *
     * It is not deferred while a game occlusion query is open: the game reads those samples and moving it
     * would change the count (the same guard the early Z uses).
     */
    // A draw with no color to write does not read the target: it does not count as transparent.
    bool opaque_in_all = true;
    for (uint32_t i = 0; i < 4 && opaque_in_all; ++i) {
      const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
      if (!mask_i) {
        continue;
      }
      const uint32_t m = key.blend[i];
      const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alpha_direct =
          ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool blend_on =
          (((mask_i & 0x7) != 0) && !color_direct) || (((mask_i & 0x8) != 0) && !alpha_direct);
      if (blend_on) {
        opaque_in_all = false;
      }
    }
    const bool test_z_draw = ((control_depth >> 1) & 0x1) != 0;
    const bool writes_z_draw = test_z_draw && ((control_depth >> 2) & 0x1) != 0 &&
                                  keys[4] != 0;
    bool is_sky = false;
    if (ps && ps->shader && ps->shader->fingerprint == kFingerprintSky && masks) {
      ++sky_seen_;
      const bool uses_stencil = (control_depth & 0x1) != 0;
      /*
       * Scene pass only. The same dome is also drawn in the car's cubemap and in the reflection, and there it
       * is not touched: the rear-view mirror works and is left alone, and the measured saving (4.8 of the
       * scene's 19.2 ms) is the scene's. This line is what keeps the cubemap out of it.
       */
      const bool in_the_scene = CategoryOfTarget(pitch, keys) == kGpuScene;
      /*
       * The fingerprint is not enough, and that broke an earlier version.
       *
       * The fingerprint identifies the shader, not the draw. The counter said so: "7.00 detected per
       * frame" when the analysis expected 0.9. Seven draws use that pixel shader, and deferring one broke
       * the order of the other six: flickering and odd colors.
       *
       * Now it must also be the dome:
       *   - the index count measured by the analysis of the SPIR-V and the log: 480 = 160 triangles;
       *   - and it must be the first draw of the pass (the dome is painted on the empty Z-buffer, which is
       *     exactly why it shades the whole screen and is then covered).
       *
       * Both are cheap to check and both belong to the dome, not to the shader. If the counter still said
       * more than one per frame, nothing is deferred: there is a guard below.
       */
      const uint32_t indices_draw =
          uint32_t(indices_de_16 ? indices16_.size() : indices_.size());
      const bool geometry_of_dome = indices_draw == kIndicesDomeSky;
      const bool first_of_pass = draws_in_pass_ == 0;
      /* The full criterion, without the cvar or the guard: what is measured during the test. */
      const bool is_the_dome = in_the_scene && opaque_in_all && test_z_draw &&
                              !writes_z_draw && !uses_stencil && !(ps->outputs & 0x10) &&
                              !occlusion_open_ && geometry_of_dome && first_of_pass;
      if (is_the_dome) {
        ++sky_candidates_;  // the ones matching the dome's marks: this must be 1
        /*
         * The guard counts here, whether it defers or not. Two in the same frame is exactly what broke the
         * image before, so as soon as it happens it switches off and stays off for the whole session.
         */
        if (++sky_guard_in_frame_ > 1 && sky_guard_ != kSkyDiscarded) {
          sky_guard_ = kSkyDiscarded;
          REXLOG_WARN("[native] C6 sky: the guard sees {} domes in the SAME frame; nothing is deferred for the "
                      "whole session (the bug of build 137). The image stays intact",
                      sky_guard_in_frame_);
        }
      }
      is_sky = sky_deferred_ && is_the_dome && sky_guard_ == kSkyDeferring;
      if (!is_sky && sky_deferred_) {
        ++sky_no_deferrable_;  // only with the setting on: measures criterion failures, not the cvar
      }
    }
    /*
     * Which draws the sky can be moved past, and why that does not change a single pixel.
     *
     * The sky can only skip past a draw D if both hold:
     *
     *  - D is opaque: it does not read the existing color, so wherever D paints, the final color is its own
     *    whether or not the sky was underneath. A draw with blending does read the background: the sky goes
     *    first.
     *  - D writes depth: after D the Z-buffer holds D's z, which is closer than the dome, so the sky's LEQUAL
     *    test discards it exactly where D painted. If D did not write Z, the Z-buffer would stay as it was
     *    and the sky would be painted over D: that would change the image.
     *
     * So the sky is emitted as soon as the first draw that fails either condition arrives. In practice that
     * is the first transparent draw, because what follows the sky is the opaque world; the counter separates
     * the two reasons so this can be checked in the log.
     */
    if (sky_pending_) {
      if (is_sky) {
        ++sky_two_in_pass_;  // another sky in the same pass: emit the earlier one now, keeping the order
        EmitSkyDeferred(cmd, kSkyByOtherSky);
      } else if (!(opaque_in_all && writes_z_draw)) {
        EmitSkyDeferred(cmd, opaque_in_all ? kSkyByWithoutZ : kSkyByBlend);
      }
    }
    // --- Record ---------------------------------------------------------------------------------
    if (is_sky && reads_reflection) {
      fh1::reflection_demand::NoteVisible(false);  // the deferred sky is not measured
    }
    if (is_sky) {
      // The arguments of the vkCmd* calls this draw would have emitted are saved; nothing is recorded. The
      // upload buffer is an allocator that only moves forward (Reserve) and is not reset until UseSlot,
      // which always comes after BeforeOfSend -> FinishPass: when the sky is emitted, its offsets still
      // point to the same data.
      SkyDeferred& c = sky_;
      c.pipeline = pipeline;
      c.buffer = upload_;
      c.usa_ubo = use_ubo_;
      c.push[0] = upload_address_ + constants_vs_offset_;
      c.push[1] = upload_address_ + constants_ps_offset_;
      c.push[2] = upload_address_ + offset_shared;
      c.offsets_ubo = use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_),
                                                          uint32_t(constants_ps_offset_),
                                                          uint32_t(offset_shared)}
                                : std::array<uint32_t, 3>{0, 0, 0};
      c.slot_ubo = slot_current_;
      c.viewport = viewport;
      c.scissor = scissor_final;
      std::memcpy(c.blend, blend_constant, sizeof(c.blend));
      std::memcpy(c.bias, bias, sizeof(c.bias));
      c.with_stencil = keys[4] != 0;
      c.stencil[0] = stencil_front;
      c.stencil[1] = stencil_back;
      c.n_bindings = uint32_t(entry->bindings.size());
      c.offsets_vertices = offsets_vertices;
      c.con_indices = con_indices;
      c.indices_de_16 = indices_de_16;
      c.indices = uint32_t(indices_de_16 ? indices16_.size() : indices_.size());
      c.first_index = uint32_t(offset_indices / (indices_de_16 ? 2 : 4));
      c.vmin = vmin;
      c.count = count;
      c.ps_mas_uno = ps ? ps->number + 1 : 0;
      c.category = CategoryOfTarget(pitch, keys);
      c.draws_to_defer = draws_in_pass_;
      c.eds_mode = eds_mode_;  // phases 1 and 2: its pipeline lacks this state, so save it all
      if (eds_mode_) {
        StateEdsOf(key, c.eds, eds_mode_);
      }
      sky_pending_ = true;
      ++sky_deferred_count_;
      // Counted as if it had been recorded: it will be recorded before the pass closes.
      ++draws_by_category_[c.category];
      triangles_by_category_[c.category] += (con_indices ? c.indices : count) / 3;
      Stage(6, mark);
      ++drawn_;
      // draws_in_pass_ is not incremented here: this draw is not recorded yet. It is incremented in
      // EmitSkyDeferred, and the difference with draws_to_defer gives the draws that went ahead of
      // it, which is exactly the geometry that now covers it.
      drawn_timed_ += time_ ? 1 : 0;
      return true;
    }
    if (pipeline != pipeline_bound_) {
      if (count_changes_pipeline_) {  // Measurement only (fh1_native_count_pipeline_changes)
        CountChangePipeline(key, pipeline_bound_ == VK_NULL_HANDLE);
      }
      NFSMW_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_bound_ = pipeline;
    }
    // Phases 1 and 2. The state that no longer goes in the pipeline, before the draw. If the guard sees a
    // difference, dynamic state switches off and this same draw is bound with its usual pipeline.
    if (eds_mode_ && !FixStateDynamic(cmd, key)) {
      NFSMW_SUB(11, pipeline = PipelineDe(KeyOfLookup(key), *entry, p));
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      NFSMW_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_bound_ = pipeline;
      key_bound_valid_ = false;  // the counter does not classify this bind
    }
    if (!sets_bound_) {
      NFSMW_SUB(1, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                                sets_.data(), 0, nullptr));
      sets_bound_ = true;
    }
    // Dynamic state and push constants repeat a lot between consecutive draws: they are only recorded if
    // they change (recording: 0.4 us per draw in a race).
    const bool recorded = state_recorded_;
    /*
     * With constants through UBOs, the push constants are dead.
     *
     * Every access in shader_common.h is `NFSMW_UBO ? set 4 : g_PushConstants...`, and NFSMW_UBO is
     * specialization constant 1<<8, fixed when the pipeline is created: DXC folds the ternary and the shader
     * does not even reference g_PushConstants. Recording them meant recording something nobody reads.
     *
     * And it is not free: on Maxwell B, NVK has no hardware root table (nvk_use_hw_root_table requires
     * Turing), so each vkCmdPushConstants is a LOAD_CONSTANT_BUFFER_OFFSET plus the array, emitted on the
     * spot. The UBO is active in 100 % of the measured submissions.
     */
    if (!use_ubo_) {
      const uint64_t push[3] = {upload_address_ + constants_vs_offset_,
                                upload_address_ + constants_ps_offset_,
                                upload_address_ + offset_shared};
      if (!recorded || std::memcmp(push, push_recorded_, sizeof(push)) != 0) {
        dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(push), push);
        std::memcpy(push_recorded_, push, sizeof(push));
      }
    }
    // Set 4 (dynamic UBOs). Always bound, because the new shaders use it statically: with the real offsets
    // if fh1_native_constants_ubo is set, and 0 otherwise. Only recorded when something changes.
    {
      const std::array<uint32_t, 3> offsets_ubo =
          use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_), uint32_t(constants_ps_offset_),
                                              uint32_t(offset_shared)}
                    : std::array<uint32_t, 3>{0, 0, 0};
      ++set4_draws_;  // C6 set 4 report
      if (!ubo_bound_ || slot_ubo_bound_ != slot_current_ || offsets_ubo != offsets_ubo_bound_) {
        // Which offsets change at each bind (measurement only). What NVK saves by differences depends on it:
        // every cbuf whose offset does not change is one rebind less in the Draw.
        if (!ubo_bound_ || slot_ubo_bound_ != slot_current_) {
          ++set4_first_;
        } else {
          ++set4_changes_[(offsets_ubo[0] != offsets_ubo_bound_[0] ? 1u : 0u) |
                          (offsets_ubo[1] != offsets_ubo_bound_[1] ? 2u : 0u) |
                          (offsets_ubo[2] != offsets_ubo_bound_[2] ? 4u : 0u)];
        }
        NFSMW_SUB(2, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                                  &sets_ubo_[slot_current_], 3, offsets_ubo.data()));
        offsets_ubo_bound_ = offsets_ubo;
        slot_ubo_bound_ = slot_current_;
        ubo_bound_ = true;
      }
    }
    if (!recorded || std::memcmp(&viewport, &viewport_recorded_, sizeof(viewport)) != 0) {
      NFSMW_SUB(3, dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport));
      viewport_recorded_ = viewport;
    }
    if (!recorded || std::memcmp(&scissor_final, &scissor_recorded_, sizeof(scissor_final)) != 0) {
      NFSMW_SUB(4, dfn_.vkCmdSetScissor(cmd, 0, 1, &scissor_final));
      scissor_recorded_ = scissor_final;
    }
    if (!recorded ||
        std::memcmp(blend_constant, blend_recorded_, sizeof(blend_constant)) != 0) {
      NFSMW_SUB(5, dfn_.vkCmdSetBlendConstants(cmd, blend_constant));
      std::memcpy(blend_recorded_, blend_constant, sizeof(blend_constant));
    }
    // Also without bias: the state is dynamic in every pipeline and has to be recorded.
    if (!recorded || std::memcmp(bias, bias_recorded_, sizeof(bias)) != 0) {
      NFSMW_SUB(6, dfn_.vkCmdSetDepthBias(cmd, bias[0], 0.0f, bias[1]));
      std::memcpy(bias_recorded_, bias, sizeof(bias));
    }
    if (!recorded) {
      stencil_recorded_valid_ = false;
      type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
    }
    if (keys[4]) {
      const uint32_t front = stencil_front;  // computed before the recording block
      const uint32_t back = stencil_back;
      if (!stencil_recorded_valid_ || front != stencil_recorded_[0] ||
          back != stencil_recorded_[1]) {
        NFSMW_SUB(7, {
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (front >> 8) & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (front >> 16) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
        });
        stencil_recorded_[0] = front;
        stencil_recorded_[1] = back;
        stencil_recorded_valid_ = true;
      }
    }
    state_recorded_ = true;
    /*
     * fh1_native_zero_based_vertices. With a single binding whose copy starts at a multiple of its stride,
     * the upload buffer is bound at 0 (once per command buffer, or after the deferred sky) and the draw is
     * shifted with first_vertex. The others bind as usual.
     */
    bool base_zero = false;
    uint32_t first_vertex = 0;  // offset / stride: where the copy starts, in vertices
    if (vertices_base_zero_ && entry->bindings.size() == 1) {
      const VkDeviceSize stride = entry->bindings[0].stride;
      if (offsets_vertices[0] % stride == 0) {
        base_zero = CheckBaseZero(offsets_vertices[0], stride, sources[0].bytes, vmin, first_vertex);
      } else {
        ++base_zero_misaligned_;
      }
    } else if (entry->bindings.size() > 1) {
      ++base_zero_several_bindings_;
    }
    if (!entry->bindings.empty()) {
      // 16 identical pointers were filled in on every draw to bind one or two. It is filled when the upload
      // buffer changes (once per work slot), not 2,345 times per frame.
      if (buffers_vertices_[0] != upload_) {
        buffers_vertices_.fill(upload_);
        bindings_recorded_ = 0;  // the buffer changed: earlier bindings are stale
      }
      /*
       * And do not bind again if it is exactly what is already bound.
       *
       * In NVK each binding is 5 dwords plus an invocation of the MME macro NVK_MME_BIND_VB
       * (nvk_cmd_draw.c:4658), with no redundancy check inside: ~2,800 MME macros per frame. With vertex
       * deduplication 28-33 % of the bindings reuse the same offset, so two consecutive draws of the same
       * mesh give exactly the same offsets.
       */
      // On the base-zero path what gets bound is the whole buffer, from 0.
      static constexpr VkDeviceSize kBindingInZero = 0;
      const VkDeviceSize* a_bind = base_zero ? &kBindingInZero : offsets_vertices.data();
      const uint32_t n_bindings = uint32_t(entry->bindings.size());
      bool equal = n_bindings == bindings_recorded_;
      for (uint32_t i = 0; equal && i < n_bindings; ++i) {
        equal = a_bind[i] == offsets_recorded_[i];
      }
      if (!equal) {
        NFSMW_SUB(8, dfn_.vkCmdBindVertexBuffers(cmd, 0, n_bindings, buffers_vertices_.data(), a_bind));
        ++base_zero_bindings_recorded_;
        bindings_recorded_ = n_bindings;
        for (uint32_t i = 0; i < n_bindings; ++i) {
          offsets_recorded_[i] = a_bind[i];
        }
      } else {
        ++bindings_saved_;
      }
      ++base_zero_total_;
      base_zero_draws_ += base_zero ? 1 : 0;
    }
    // With a game occlusion query open, this draw counts toward it.
    if (occlusion_open_ && query_occlusion_ == UINT32_MAX) {
      NFSMW_SUB(13, query_occlusion_ = context_->BeginQueryOcclusion());
    }
    // The first 12, then one every 2 s (up to 150), to follow the sun over the course of a race.
    if (occlusion_open_ && pitch >= 640 && warnings_occlusion_draw_ < 16 &&
        (warnings_occlusion_draw_ < 12 ||
         std::chrono::steady_clock::now() - last_warning_occlusion_draw_ >= std::chrono::seconds(2))) {
      ++warnings_occlusion_draw_;
      last_warning_occlusion_draw_ = std::chrono::steady_clock::now();
      REXLOG_INFO("[native] C2 occlusion draw {}: host query {}: VS n{} PS n{} type {} count {} pitch {} masks "
                  "{:08X} depth {:08X} (writes {}, test {}, z function {}) viewport {:.1f},{:.1f} {:.1f}x{:.1f} z "
                  "{:.3f}-{:.3f} scissor {},{} {}x{} with indices {}",
                  warnings_occlusion_draw_, query_occlusion_ == UINT32_MAX ? -1 : int64_t(query_occlusion_),
                  p.vs->number, ps ? int(ps->number) : -1, type, count, pitch, masks, control_depth,
                  (control_depth >> 2) & 0x1, (control_depth >> 1) & 0x1, (control_depth >> 4) & 0x7,
                  viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth,
                  scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height, con_indices);
      // Diagnostic: positions of the first vertices and VS constants c0-c6, to compute the sun's z against
      // the scene depth.
      std::string positions;
      for (const AttributeVertices& a : entry->attributes) {
        if (a.location != 0) {
          continue;
        }
        const Source& source = sources[a.binding];
        const uint32_t stride = entry->bindings[a.binding].stride;
        for (uint32_t v = 0; v < std::min<uint32_t>(count, 4); ++v) {
          const uint64_t since = uint64_t(v) * stride + a.offset;
          if (since + 16 > source.bytes) {
            break;
          }
          positions += " (";
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t word;
            std::memcpy(&word, source.data + since + k * 4, 4);
            positions += fmt::format("{}{:.7g}", k ? "," : "", Float(xenos::GpuSwap(word, source.order)));
          }
          positions += ")";
        }
        positions += fmt::format(" format {} stride {}", int(a.format), stride);
        break;
      }
      std::string constants;
      for (uint32_t k = 0; k < 7; ++k) {
        constants += fmt::format(" c{}=(", k);
        for (uint32_t c = 0; c < 4; ++c) {
          constants += fmt::format("{}{:.7g}", c ? "," : "", Float(r[kRegConstantsVs + k * 4 + c]));
        }
        constants += ")";
      }
      REXLOG_INFO("[native] C2 occlusion draw {} (geometry): VTE {:08X} CLIP {:08X} sc {:08X} RB_DEPTH_INFO {:08X} "
                  "pass {}x{} ndc ({:.5g},{:.5g},{:.5g},{:.5g}) pipeline z function {} vmin {}; positions{}; VS "
                  "constants{}",
                  warnings_occlusion_draw_, vte, r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], mode_sc,
                  r[gr::XE_GPU_REG_RB_DEPTH_INFO], pass_width_, pass_height_, ndc[0], ndc[1], ndc[2], ndc[3],
                  (key.depth >> 4) & 0x7, vmin, positions, constants);
    }
    // Draws and triangles per render target type (same classification as the GPU time).
    {
      const uint32_t category = CategoryOfTarget(pitch, keys);
      const uint32_t vertices_draw = con_indices ? uint32_t(indices_de_16 ? indices16_.size() : indices_.size())
                                                   : count;
      ++draws_by_category_[category];
      triangles_by_category_[category] += vertices_draw / 3;
    }
    // In the diagnostic frame, one query per draw with its pixel shader.
    // The diagnostic window is decided by the context, at the Swap. With the diagnostic off (the normal
    // case) this does not even reach the call.
    const uint32_t query_draw =
        !diag_statistics_draw_
            ? UINT32_MAX
            : context_->BeginStatisticsDraw(
                  ps ? ps->number + 1 : 0,
                  CategoryOfTarget(pitch, keys));
    /*
     * fh1_reflection_visibility. An occlusion query around this draw alone, if it samples the reflection:
     * 0 samples = it wrote nothing to any target. Two of the same type cannot be open at once, so while a
     * game query is open it is not measured and counts as visible. Every kWitnessEvery frames the final
     * composite, which paints the whole screen, is measured the same way: it must produce samples (the
     * guard, in nfsmw-nx's race-cuts code).
     */
    uint32_t query_visibility = UINT32_MAX;
    if (reads_reflection) {
      if (!occlusion_open_ && query_occlusion_ == UINT32_MAX) {
        query_visibility = context_->BeginQueryVisibility(false);
      }
      if (query_visibility == UINT32_MAX) {
        fh1::reflection_demand::NoteVisible(false);
      }
    } else if (witness_pending_ && ps && ps->shader && ps->shader->fingerprint == kFingerprintComposition &&
               !occlusion_open_ && query_occlusion_ == UINT32_MAX) {
      query_visibility = context_->BeginQueryVisibility(true);
      if (query_visibility != UINT32_MAX) {
        witness_pending_ = false;
        // While the first 8 are checked, one every 2 frames: the guard finishes in ~0.5 s of racing.
        witness_next_ =
            frame_ + (fh1::reflection_demand::VisibilityChecked() ? kWitnessEvery : uint64_t(2));
      }
    }
    if (con_indices) {
      // The upload buffer is bound once per index type and each draw uses firstIndex.
      const VkIndexType type_indices = indices_de_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      if (type_indices != type_indices_recorded_) {
        NFSMW_SUB(9, dfn_.vkCmdBindIndexBuffer(cmd, upload_, 0, type_indices));
        type_indices_recorded_ = type_indices;
      }
      // On the base-zero path, vertexOffset also carries where the copy starts (in vertices).
      const int32_t offset_vertices =
          base_zero ? int32_t(int64_t(first_vertex) - int64_t(vmin)) : -int32_t(vmin);
      NFSMW_SUB(10, dfn_.vkCmdDrawIndexed(cmd, uint32_t(indices_de_16 ? indices16_.size() : indices_.size()),
                                          1, uint32_t(offset_indices / (indices_de_16 ? 2 : 4)),
                                          offset_vertices, 0));
    } else {
      NFSMW_SUB(10, dfn_.vkCmdDraw(cmd, count, 1, base_zero ? first_vertex : 0, 0));  // firstVertex
    }
    if (query_visibility != UINT32_MAX) {
      context_->FinishQueryVisibility(query_visibility);
    }
    if (query_draw != UINT32_MAX) {
      context_->FinishStatisticsDraw(query_draw);
    }
    Stage(6, mark);
    if (time_) {
      ++sub_samples_;
      ReportSubstages();
    }
    ++drawn_;
    ++draws_in_pass_;  // to know at which position of the pass the sky ends up emitted
    drawn_timed_ += time_ ? 1 : 0;
    return true;
  }

  /*
   * Emits the deferred sky draw (fh1_native_deferred_sky).
   *
   * It records exactly the same vkCmd* calls it would have recorded in its original place, with the
   * values saved then. It relies on nothing recorded afterwards: it sends all of this draw's dynamic
   * state, and when done it invalidates the tracked state so the next draw records its own again. That
   * way the order cannot slip through an "already set" comparison.
   *
   * The upload buffer offsets are still valid because Reserve only moves forward and the buffer is not
   * reset until UseSlot, which always comes after BeforeOfSend -> FinishPass.
   */
  void EmitSkyDeferred(VkCommandBuffer cmd, uint32_t reason) {
    if (!sky_pending_ || cmd == VK_NULL_HANDLE) {
      return;
    }
    sky_pending_ = false;
    const SkyDeferred& c = sky_;
    ++sky_emitted_[reason < kSkyReasons ? reason : kSkyReasons - 1];
    const uint64_t position = draws_in_pass_ - c.draws_to_defer;
    sky_position_sum_ += position;
    sky_position_max_ = std::max(sky_position_max_, position);
    dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
    pipeline_bound_ = c.pipeline;
    if (c.eds_mode) {  // phases 1 and 2: all the state its pipeline lacks
      EmitStateDynamic(cmd, c.eds, true, c.eds_mode);
    }
    eds_valid_ = false;  // the next draw sets all of its own again
    key_bound_valid_ = false;  // The sky does not keep its key (CountChangePipeline)
    if (!sets_bound_) {
      dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                   sets_.data(), 0, nullptr);
      sets_bound_ = true;
    }
    if (!c.usa_ubo) {
      dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(c.push), c.push);
    }
    dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                 &sets_ubo_[c.slot_ubo], 3, c.offsets_ubo.data());
    dfn_.vkCmdSetViewport(cmd, 0, 1, &c.viewport);
    dfn_.vkCmdSetScissor(cmd, 0, 1, &c.scissor);
    dfn_.vkCmdSetBlendConstants(cmd, c.blend);
    dfn_.vkCmdSetDepthBias(cmd, c.bias[0], 0.0f, c.bias[1]);
    if (c.with_stencil) {
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, c.stencil[0] & 0xFF);
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, c.stencil[1] & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 16) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 16) & 0xFF);
    }
    if (c.n_bindings) {
      std::array<VkBuffer, 16> buffers{};
      buffers.fill(c.buffer);
      dfn_.vkCmdBindVertexBuffers(cmd, 0, c.n_bindings, buffers.data(), c.offsets_vertices.data());
    }
    const uint32_t query = !diag_statistics_draw_
                                  ? UINT32_MAX
                                  : context_->BeginStatisticsDraw(c.ps_mas_uno, c.category);
    if (c.con_indices) {
      const VkIndexType type = c.indices_de_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      dfn_.vkCmdBindIndexBuffer(cmd, c.buffer, 0, type);
      dfn_.vkCmdDrawIndexed(cmd, c.indices, 1, c.first_index, -int32_t(c.vmin), 0);
    } else {
      dfn_.vkCmdDraw(cmd, c.count, 1, 0, 0);
    }
    if (query != UINT32_MAX) {
      context_->FinishStatisticsDraw(query);
    }
    ++draws_in_pass_;
    // Nothing tracked is valid any more: the dynamic state, the bindings and the index type are the sky's.
    // state_recorded_ = false makes the next draw also re-record stencil and indices.
    state_recorded_ = false;
    ubo_bound_ = false;
    bindings_recorded_ = 0;
    stencil_recorded_valid_ = false;
    type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  // Adds the time since the mark to the stage and moves the mark to now.
  /*
   * Measurement only. What takes the time inside the ring's record and pipeline stages (4.1 and 2.5 us per
   * recorded draw): each Vulkan command, PipelineOf and the shared constants block, on the same timed
   * draws (1 in 64). "C6 substages" line every 10 s: us per draw, us per call and calls per timed draw.
   */
  // Its lines, like those of the other ring reports, go to the report thread (FH1_REPORT_RING).
  void ReportSubstages() {
    const auto now = std::chrono::steady_clock::now();
    if (now < sub_next_) {
      return;
    }
    const bool first = sub_next_ == std::chrono::steady_clock::time_point{};
    sub_next_ = now + std::chrono::seconds(10);
    if (first || !sub_samples_) {
      sub_ns_.fill(0);
      sub_n_.fill(0);
      sub_samples_ = 0;
      return;
    }
    static constexpr const char* kNames[19] = {
        "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
        "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineDe", "shared",
        "occlusion", "samplers (texture stage)",
        // The four segments of the "indices" stage (CutSubstage)
        "index stage: targets and discards", "index stage: depth bias",
        "index stage: indices", "index stage: vertices and diagnostic"};
    std::string line;
    double total = 0.0;
    for (size_t k = 0; k < 19; ++k) {
      if (!sub_n_[k]) {
        continue;
      }
      const double by_draw = double(sub_ns_[k]) / 1e3 / double(sub_samples_);
      total += by_draw;
      line += fmt::format(" | {} {:.2f} us/draw ({:.2f} us x {:.2f} per draw)", kNames[k], by_draw,
                           double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                           double(sub_n_[k]) / double(sub_samples_));
    }
    FH1_REPORT_RING("[native] C6 substages ({} draws timed; sum {:.2f} us per draw){}", sub_samples_,
                total, line);
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_samples_ = 0;
  }

  /*
   * The base-zero path of a draw (fh1_native_zero_based_vertices). Returns false if it has to bind as
   * usual. Self-checking guard: the first kBaseZeroACheck draws, then 1 in 4,096, redo the computation
   * backwards in 64 bits (the first vertex the GPU reads falls on the first byte of the copy, with and
   * without indices), and check that vertexOffset fits in a signed 32-bit value and that the copy fits in
   * the buffer. On a single difference: that draw binds as usual, DIFFERENCE in the log and the path is off
   * for the session.
   */
  bool CheckBaseZero(VkDeviceSize offset, VkDeviceSize stride, uint64_t bytes, uint32_t vmin,
                         uint32_t& first_vertex) {
    const VkDeviceSize first_2 = offset / stride;
    if (first_2 > VkDeviceSize(INT32_MAX)) {
      return false;  // does not fit in firstVertex/vertexOffset (cannot happen with a 64 MB buffer)
    }
    const uint64_t n = ++base_zero_checked_;
    if (n <= kBaseZeroACheck || (n & 4095) == 0) {
      const int64_t displacement = int64_t(first_2) - int64_t(vmin);  // the vertexOffset of indexed draws
      const bool ok = first_2 * stride == offset &&
                        (displacement + int64_t(vmin)) * int64_t(stride) == int64_t(offset) &&
                        displacement >= int64_t(INT32_MIN) && displacement <= int64_t(INT32_MAX) &&
                        offset + bytes <= kSizeUpload && offset + bytes <= upload_size_real_;
      if (!ok) {
        vertices_base_zero_off_ = true;
        vertices_base_zero_ = false;
        REXLOG_ERROR("[native] C6 zero-based vertices: DIFFERENCE (offset {} stride {} bytes {} vmin {} first {}): "
                     "this draw and the rest of the session bind as always",
                     offset, stride, bytes, vmin, first_2);
        return false;
      }
      if (n == kBaseZeroACheck) {
        FH1_REPORT_RING("[native] C6 zero-based vertices: {} draws checked, 0 differences; keeps checking 1 in "
                         "4096", n);
      }
    }
    first_vertex = uint32_t(first_2);
    return true;
  }

  /*
   * Set 4 by differences, once per submission (UseSlot). Tells NVK whether it is wanted
   * (fh1_native_set4_differences and the test alternation) and, if the NVK guard has seen a DIFFERENCE,
   * reports it in the log as an error once: NVK has already switched it off for the rest of the session.
   */
  void ControlSet4() {
    bool requested = REXCVAR_GET(fh1_native_set4_differences);
    const int32_t toggle = REXCVAR_GET(fh1_native_set4_differences_toggle_s);
    if (toggle > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                             set4_start_alternation_)
                                .count();
      requested = (seconds / toggle) % 2 == 1;
    }
#if REX_PLATFORM_SWITCH
    NvkSwitchSet4* const nvk = &nvk_switch_set4;
    const bool with_patch = nvk != nullptr && nvk->version == 1;
#else
    const bool with_patch = false;
#endif
    if (requested != set4_requested_ || !set4_requested_noted_) {
      set4_requested_ = requested;
      set4_requested_noted_ = true;
      REXLOG_INFO("[native] C6 set 4 by differences (build 184): {} (frame {}){}",
                  requested ? "requested from NVK" : "NOT requested: NVK binds the whole set", frame_,
                  with_patch ? "" : "; this NVK does not have patch_nvk_set4 (older Mesa or PC): binds as always");
    }
#if REX_PLATFORM_SWITCH
    if (with_patch) {
      __atomic_store_n(&nvk->requested, requested ? 1 : 0, __ATOMIC_RELAXED);
      if (!set4_difference_notified_ && __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0) {
        set4_difference_notified_ = true;
        REXLOG_ERROR("[native] C6 set 4 by differences: DIFFERENCE seen by the NVK guard ({} cbufs bound wrong, "
                     "each one fixed in its draw; details in rex_stderr.log). Off for the rest of the session: NVK "
                     "binds the whole set, as always",
                     __atomic_load_n(&nvk->total.differences, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * The draw path in NVK, once per submission (like ControlSet4). Tells NVK what to measure and which
   * improvements are wanted (they apply from the next command buffer) and, if an improvement's guard has
   * seen a DIFFERENCE, reports it in the log as an error once: NVK has already switched it off for the
   * rest of the session.
   */
  void ControlDrawNvk() {
#if REX_PLATFORM_SWITCH
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      nvk_preload_app_ = false;
      if (!nvk_draw_noted_) {
        nvk_draw_noted_ = true;
        REXLOG_INFO("[native] C6 NVK per draw (build 186): this NVK does not have the nvk_switch_draw contract "
                    "(older Mesa): no measuring and no improvements");
      }
      return;
    }
    int32_t measure = std::max<int32_t>(0, REXCVAR_GET(fh1_native_nvk_measure));
    if (measure > 0) {
      measure = int32_t(std::bit_floor(uint32_t(measure)));  // NVK wants a power of 2
    }
    bool improvements = true;
    const int32_t toggle = REXCVAR_GET(fh1_native_nvk_toggle_s);
    if (toggle > 0) {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - nvk_draw_start_)
              .count();
      improvements = (seconds / toggle) % 2 == 1;
    }
    const int32_t requested_2[5] = {improvements && REXCVAR_GET(fh1_native_nvk_emission) ? 1 : 0,
                                improvements && REXCVAR_GET(fh1_native_nvk_cbufs) ? 1 : 0,
                                improvements && REXCVAR_GET(fh1_native_nvk_dynamic) ? 1 : 0,
                                0,  // the fast set 4 path (p05) is not applied
                                improvements && REXCVAR_GET(fh1_native_nvk_preload) ? 1 : 0};
    __atomic_store_n(&nvk->measure, measure, __ATOMIC_RELAXED);
    __atomic_store_n(&nvk->measure_misses, REXCVAR_GET(fh1_native_nvk_measure_misses) ? 1 : 0, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m) {
      __atomic_store_n(&nvk->improvements[m].requested, requested_2[m], __ATOMIC_RELAXED);
    }
    nvk_preload_app_ = requested_2[4] != 0;
    if (!nvk_draw_noted_ || improvements != nvk_improvements_requested_) {
      nvk_draw_noted_ = true;
      nvk_improvements_requested_ = improvements;
      REXLOG_INFO("[native] C6 NVK per draw (build 186): improvements {} (emission {}, cbufs {}, dynamic {}, "
                  "preload {}); measuring 1 in {} calls{}; environment NVK_SWITCH_DRAW {} (frame {})",
                  improvements ? "requested" : "NOT requested", requested_2[0], requested_2[1], requested_2[2], requested_2[4], measure,
                  REXCVAR_GET(fh1_native_nvk_measure_misses) ? " with the cache misses separate" : "",
                  __atomic_load_n(&nvk->environment, __ATOMIC_RELAXED), frame_);
    }
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "preload"};
    for (size_t m = 0; m < 5; ++m) {
      if (!nvk_off_warned_[m] && __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0) {
        nvk_off_warned_[m] = true;
        REXLOG_ERROR("[native] C6 NVK per draw: DIFFERENCE seen by the guard of '{}' ({} different checks; details "
                     "in rex_stderr.log). Off for the rest of the session: NVK does it as always",
                     kNames[m], __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * Every 10 s, the NVK figures. Per part, us per measured call and us per measured draw; per measured
   * draw, how much work it carries; per pipeline bind, how many state copies changed nothing; and per
   * improvement, uses, checks and differences. Totals are from command buffers that have already finished.
   */
  void ReportDrawNvk() {
#if REX_PLATFORM_SWITCH
    const auto now = std::chrono::steady_clock::now();
    if (now - nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = nvk_report_ == std::chrono::steady_clock::time_point{};
    nvk_report_ = now;
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      return;
    }
    uint64_t times[16], ticks[16], counts[13], improvements[5][4];
    for (size_t i = 0; i < 16; ++i) {
      const uint64_t v = __atomic_load_n(&nvk->parts[i].times, __ATOMIC_RELAXED);
      const uint64_t t = __atomic_load_n(&nvk->parts[i].ticks, __ATOMIC_RELAXED);
      times[i] = v - nvk_parts_previous_[i][0];
      ticks[i] = t - nvk_parts_previous_[i][1];
      nvk_parts_previous_[i][0] = v;
      nvk_parts_previous_[i][1] = t;
    }
    for (size_t i = 0; i < 13; ++i) {
      const uint64_t c = __atomic_load_n(&nvk->counts[i], __ATOMIC_RELAXED);
      counts[i] = c - nvk_counts_previous_[i];
      nvk_counts_previous_[i] = c;
    }
    for (size_t m = 0; m < 5; ++m) {
      const uint64_t c[4] = {__atomic_load_n(&nvk->improvements[m].uses, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].validated, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].without_check, __ATOMIC_RELAXED)};
      for (size_t k = 0; k < 4; ++k) {
        improvements[m][k] = c[k] - nvk_improvements_previous_[m][k];
        nvk_improvements_previous_[m][k] = c[k];
      }
    }
    if (first) {
      return;
    }
    const uint64_t tps = __atomic_load_n(&nvk->ticks_by_second, __ATOMIC_RELAXED);
    const double us_tick = tps ? 1e6 / double(tps) : 0.0;
    const uint64_t draws = counts[0];  // NVK_SW_C_DRAWS_MEASURED
    // us per measured call and, in brackets, us per measured draw.
    const auto part = [&](size_t i) {
      return fmt::format("{:.2f} [{:.2f}]", times[i] ? double(ticks[i]) * us_tick / double(times[i]) : 0.0,
                         draws ? double(ticks[i]) * us_tick / double(draws) : 0.0);
    };
    const auto by_draw = [&](size_t i) { return draws ? double(counts[i]) / double(draws) : 0.0; };
    if (draws) {
      FH1_REPORT_RING(
          "[native] C6 NVK by parts (build 186; us per measured call [us per measured draw]; {} draws measured): "
          "whole draw {} | push desc {} | dynamic {} | shader touch {} | shaders {} | cbufs {} | emit {} | "
          "BindPipeline: shaders {} touch {} state copy {} | sets {} (root {} dirty {}) | root table {} | new "
          "chunk {} | BindVertexBuffers {}",
          draws, part(0), part(1), part(2), part(3), part(4), part(5), part(6), part(7), part(8),
          part(9), part(10), part(11), part(12), part(13), part(14), part(15));
      FH1_REPORT_RING(
          "[native] C6 NVK per measured draw: {:.1f} dwords; {:.2f} with dirty dynamic state ({:.1f} bits); {:.2f} "
          "with dirty shaders; {:.2f} cbufs rebound | pipelines: {} binds measured, {:.1f} % copies that changed "
          "nothing, {:.2f} new bits per bind | {} uploads to the root table ({:.1f} words each), {} new chunks",
          by_draw(1), by_draw(2), counts[2] ? double(counts[3]) / double(counts[2]) : 0.0, by_draw(4),
          by_draw(5), counts[6], counts[6] ? 100.0 * double(counts[7]) / double(counts[6]) : 0.0,
          counts[6] ? double(counts[8]) / double(counts[6]) : 0.0, counts[10],
          counts[10] ? double(counts[11]) / double(counts[10]) : 0.0, counts[12]);
    }
    static constexpr size_t kShown[4] = {0, 1, 2, 4};  // without the fast set 4 (p05), which is not applied
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "preload"};
    std::string text;
    for (const size_t m : kShown) {
      const char* state = __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0 ? "OFF by the guard"
                           : __atomic_load_n(&nvk->improvements[m].requested, __ATOMIC_RELAXED) == 0 ? "not "
                                                                                                       "requested"
                                                                                              : "on";
      text += fmt::format(" | {} ({}): {} uses, {} checked equal, {} different, {} not checked", kNames[m],
                           state, improvements[m][0], improvements[m][1], improvements[m][2], improvements[m][3]);
    }
    FH1_REPORT_RING("[native] C6 NVK improvements (build 186, last 10 s){}", text);
#endif
  }

  /*
   * Every 10 s, set 4. What changes at each bind (measured here, with or without the NVK patch) and, if
   * NVK has it, what is saved: root table writes per bind (4 without it), cbufs not rebound and the state
   * of its guard.
   */
  void ReportSet4() {
    const auto now = std::chrono::steady_clock::now();
    if (now - set4_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = set4_report_ == std::chrono::steady_clock::time_point{};
    set4_report_ = now;
    uint64_t bindings = set4_first_;
    for (const uint64_t n : set4_changes_) {
      bindings += n;
    }
    if (!first && set4_draws_ && bindings) {
      const auto pct = [bindings](uint64_t n) { return 100.0 * double(n) / double(bindings); };
      FH1_REPORT_RING(
          "[native] C6 set 4: {:.2f} binds per draw ({} in {} draws); only VS changes {:.1f} %, only PS {:.1f} %, "
          "only shared {:.1f} %, VS+PS {:.1f} %, VS+shared {:.1f} %, PS+shared {:.1f} %, all three {:.1f} %, after "
          "buffer, slot or sky {:.1f} % | constants uploaded again: VS {} by generation, {} by epoch and {} "
          "because the shader reads more; PS {}, {} and {}",
          double(bindings) / double(set4_draws_), bindings, set4_draws_, pct(set4_changes_[1]),
          pct(set4_changes_[2]), pct(set4_changes_[4]), pct(set4_changes_[3]), pct(set4_changes_[5]),
          pct(set4_changes_[6]), pct(set4_changes_[7]), pct(set4_first_), reuploads_vs_[0], reuploads_vs_[1],
          reuploads_vs_[2], reuploads_ps_[0], reuploads_ps_[1], reuploads_ps_[2]);
#if REX_PLATFORM_SWITCH
      if (NvkSwitchSet4* const nvk = &nvk_switch_set4; nvk != nullptr && nvk->version == 1) {
        const uint64_t counts[9] = {__atomic_load_n(&nvk->total.bindings_difference, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.bindings_complete, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.writes_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dwords_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_dirty, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_saved, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.draws, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.checks, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.differences, __ATOMIC_RELAXED)};
        uint64_t d[9];
        for (size_t i = 0; i < 9; ++i) {
          d[i] = counts[i] - set4_nvk_previous_[i];
          set4_nvk_previous_[i] = counts[i];
        }
        const char* const state = __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0 ? "OFF by the guard"
                                   : __atomic_load_n(&nvk->environment, __ATOMIC_RELAXED) == 0
                                       ? "off by NVK_SWITCH_DYN_UBO_DELTA"
                                   : !set4_requested_ ? "off by the cvar"
                                                   : "on";
        FH1_REPORT_RING(
            "[native] C6 set 4 by differences (NVK, {}): {} binds by differences and {} whole; {:.2f} writes to "
            "the root table per bind (4 before) with {:.2f} words; cbufs dirtied {} and not dirtied {} ({:.1f} % "
            "fewer); guard: {} cbufs checked ({} draws on the new path), {} differences",
            state, d[0], d[1], d[0] ? double(d[2]) / double(d[0]) : 0.0, d[0] ? double(d[3]) / double(d[0]) : 0.0,
            d[4], d[5], d[4] + d[5] ? 100.0 * double(d[5]) / double(d[4] + d[5]) : 0.0, d[7], d[6], d[8]);
      }
#endif
    }
    set4_changes_.fill(0);
    set4_first_ = 0;
    set4_draws_ = 0;
    reuploads_vs_.fill(0);
    reuploads_ps_.fill(0);
  }

  // Every 10 s, how many draws go without binding their own offset (base zero).
  void ReportBaseZero() {
    const auto now = std::chrono::steady_clock::now();
    if (now - base_zero_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = base_zero_report_ == std::chrono::steady_clock::time_point{};
    base_zero_report_ = now;
    if (!first && base_zero_total_) {
      FH1_REPORT_RING("[native] C6 zero-based vertices ({}): {} of {} draws with vertices without binding their "
                       "own offset ({:.1f} %); {} with several bindings and {} with the dedupe copy off a multiple "
                       "of the stride; {:.2f} vkCmdBindVertexBuffers per draw",
                  vertices_base_zero_off_ ? "OFF by the guard"
                  : vertices_base_zero_       ? "on"
                                              : "off",
                  base_zero_draws_, base_zero_total_, 100.0 * double(base_zero_draws_) / double(base_zero_total_),
                  base_zero_several_bindings_, base_zero_misaligned_,
                  double(base_zero_bindings_recorded_) / double(base_zero_total_));
    }
    base_zero_draws_ = 0;
    base_zero_total_ = 0;
    base_zero_several_bindings_ = 0;
    base_zero_misaligned_ = 0;
    base_zero_bindings_recorded_ = 0;
  }

  /*
   * The framing of a draw, moved out of Draw unchanged (fh1_native_framing_cache). Returns 0 if it
   * has to draw, 1 if the viewport is empty and 2 if the scissor is empty (in both cases Draw returns
   * without drawing, as before; the scissor is left at zero). Same operations in the same order. The only
   * multiplications GCC could fuse with an add or subtract (width * 0.5 and height * 0.5) are exact, so
   * fused or not they give the same bits.
   */
  uint32_t ComputeFraming(const uint32_t* r, uint32_t vte, bool without_clip, uint32_t mode_sc, VkViewport& viewport,
                            float ndc[4], VkRect2D& scissor) {
    float scale_x = (vte & 0x1) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float center_x = (vte & 0x2) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    float scale_y = (vte & 0x4) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float center_y = (vte & 0x8) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float scale_z = (vte & 0x10) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float center_z = (vte & 0x20) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t window_x = int32_t((window & 0x7FFF) << 17) >> 17;
    const int32_t window_y = int32_t(((window >> 16) & 0x7FFF) << 17) >> 17;
    if ((mode_sc >> 16) & 0x1) {
      center_x += float(window_x);
      center_y += float(window_y);
    }
    // With the shadow map drawn smaller (fh1_native_shadow_scale), the guest still speaks in
    // 1600-pixel units. The viewport and the scissor are multiplied by the pass scale, and with that the
    // geometry lands where it should; the rest (ndc, half pixel) already comes from the pass size.
    if (pass_scale_ != 1.0f) {
      scale_x *= pass_scale_;
      center_x *= pass_scale_;
      scale_y *= pass_scale_;
      center_y *= pass_scale_;
    }
    if (pass_msaa_scale_ != 1.0f) {  // FH1: 4x MSAA pass drawn into the 1x image of twice the size
      scale_x *= pass_msaa_scale_;
      center_x *= pass_msaa_scale_;
      scale_y *= pass_msaa_scale_;
      center_y *= pass_msaa_scale_;
    }
    viewport = VkViewport{};
    viewport.x = center_x - std::abs(scale_x);
    viewport.width = 2.0f * std::abs(scale_x);
    // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
    viewport.y = center_y + scale_y;
    viewport.height = -2.0f * scale_y;
    if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
      Notify(14, "OpenGL clip space (dx_clip_space_def = 0): Z not adjusted");
    }
    viewport.minDepth = std::clamp(center_z, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(center_z + scale_z, 0.0f, 1.0f);
    // g_NdcScale (x, y) and g_NdcOffset (x, y): identity when clipping is on.
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (without_clip) {
      const float width = float(pass_width_);
      const float height = float(pass_height_);
      ndc[0] = scale_x * 2.0f / width;
      ndc[2] = (center_x - width * 0.5f) * 2.0f / width;
      // DXC flips Y at the end of the VS (-fvk-invert-y): the D3D Y goes here.
      ndc[1] = -scale_y * 2.0f / height;
      ndc[3] = -(center_y - height * 0.5f) * 2.0f / height;
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = width;
      viewport.height = height;
    }
    scissor = VkRect2D{};
    if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
      static uint32_t noted_vp = 0;
      if (noted_vp < 20 && ((frame_ >= 240 && frame_ < 244) || (frame_ >= 3500 && frame_ < 3503))) {
        ++noted_vp;
        REXLOG_INFO("[fh1] draw skipped: EMPTY VIEWPORT ({:.0f},{:.0f}) {:.0f}x{:.0f} scale ({:.1f},{:.1f}) centre ({:.1f},{:.1f}) pass {}x{} frame {}",
                    viewport.x, viewport.y, viewport.width, viewport.height, scale_x, scale_y, center_x, center_y, pass_width_,
                    pass_height_, frame_);
      }
      return 1;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += window_x;
      y0 += window_y;
      x1 += window_x;
      y1 += window_y;
    }
    if (pass_msaa_scale_ != 1.0f) {  // FH1
      const int32_t m = int32_t(pass_msaa_scale_);
      x0 *= m;
      y0 *= m;
      x1 *= m;
      y1 *= m;
    }
    if (pass_scale_ != 1.0f) {  // also in guest pixels
      x0 = int32_t(std::floor(float(x0) * pass_scale_));
      y0 = int32_t(std::floor(float(y0) * pass_scale_));
      x1 = int32_t(std::ceil(float(x1) * pass_scale_));
      y1 = int32_t(std::ceil(float(y1) * pass_scale_));
    }
    x0 = std::clamp(x0, 0, int32_t(pass_width_));
    x1 = std::clamp(x1, 0, int32_t(pass_width_));
    y0 = std::clamp(y0, 0, int32_t(pass_height_));
    y1 = std::clamp(y1, 0, int32_t(pass_height_));
    if (x1 <= x0 || y1 <= y0) {
      static uint32_t noted_scissor = 0;
      if (noted_scissor < 20 && ((frame_ >= 240 && frame_ < 244) || (frame_ >= 3500 && frame_ < 3503))) {
        ++noted_scissor;
        REXLOG_INFO("[fh1] draw skipped: EMPTY SCISSOR tl={:08X} br={:08X} window_offset=({}, {}) pass {}x{} viewport ({:.0f},{:.0f}) {:.0f}x{:.0f} frame {}",
                    tl, br, window_x, window_y, pass_width_, pass_height_, viewport.x, viewport.y, viewport.width, viewport.height,
                    frame_);
      }
      return 2;
    }
    scissor = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    return 0;
  }

  // The C1 guard. The current computation against the saved one, byte by byte.
  void CompareFraming(const VkViewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t empty) {
    ++framing_checked_;
    const bool equal = empty == framing_empty_ &&
                       std::memcmp(&viewport, &framing_viewport_, sizeof(viewport)) == 0 &&
                       std::memcmp(ndc, framing_ndc_, sizeof(framing_ndc_)) == 0 &&
                       std::memcmp(&scissor, &framing_scissor_, sizeof(scissor)) == 0;
    if (!equal) {
      framing_cache_off_ = true;
      framing_cache_ = false;
      REXLOG_ERROR("[native] C6 cached framing: DIFFERENCE in check {} (empty {} / {}; viewport {},{} {}x{} / "
                   "{},{} {}x{}; scissor {},{} {}x{} / {},{} {}x{}). Off for the rest of the session: this draw "
                   "uses the recomputed one",
                   framing_checked_, empty, framing_empty_, viewport.x, viewport.y, viewport.width,
                   viewport.height, framing_viewport_.x, framing_viewport_.y, framing_viewport_.width,
                   framing_viewport_.height, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                   scissor.extent.height, framing_scissor_.offset.x, framing_scissor_.offset.y,
                   framing_scissor_.extent.width, framing_scissor_.extent.height);
    } else if (framing_checked_ == kFramingsACheck) {
      FH1_REPORT_RING("[native] C6 cached framing: {} hits checked against the computation, 0 differences; keeps "
                       "checking 1 in 4096", framing_checked_);
    }
  }

  /*
   * The height_useful_ element for that pitch (fh1_native_useful_height_memo). The pitch is the pass's and only
   * changes with the pass, so the last one is remembered. References to unordered_map elements are not
   * invalidated by insertion (only by erasure, and height_useful_ never erases). Guard: the first
   * kHeightUsefulACheck reuses, then 1 in 4,096, also look it up in the map and must get the same element.
   */
  uint32_t& HeightUsefulOf(uint32_t pitch) {
    if (!height_useful_memo_active_) {
      return height_useful_[pitch];
    }
    if (height_useful_memo_ == nullptr || pitch != height_useful_memo_pitch_) {
      height_useful_memo_ = &height_useful_[pitch];
      height_useful_memo_pitch_ = pitch;
      return *height_useful_memo_;
    }
    const uint64_t n = ++height_useful_memo_hits_;
    if (n <= kHeightUsefulACheck || (n & 4095) == 0) {
      uint32_t* const of_map = &height_useful_[pitch];
      if (of_map != height_useful_memo_) {
        height_useful_memo_off_ = true;
        height_useful_memo_active_ = false;
        height_useful_memo_ = nullptr;
        REXLOG_ERROR("[native] C6 remembered useful height: DIFFERENCE (pitch {}, check {}): it is not the map "
                     "entry. Off for the rest of the session", pitch, n);
        return *of_map;
      }
      if (n == kHeightUsefulACheck) {
        FH1_REPORT_RING("[native] C6 remembered useful height: {} checks against the map, 0 differences; keeps "
                         "checking 1 in 4096", n);
      }
    }
    return *height_useful_memo_;
  }

  /*
   * The C3 guard. With the same render target bytes, the XXH3 must be pass_key_. Returns the computed
   * one, which is the one that decides for this draw.
   */
  uint64_t CheckKeyPass(const uint64_t keys[5], uint64_t n) {
    const uint64_t computed = XXH3_64bits(keys, sizeof(uint64_t) * 5);
    if (computed != pass_key_) {
      key_pass_fast_off_ = true;
      key_pass_fast_ = false;
      REXLOG_ERROR("[native] C6 pass key: DIFFERENCE (check {}: {:016X} computed, {:016X} saved). Off for the rest "
                   "of the session", n, computed, pass_key_);
    } else if (n == kKeysPassACheck) {
      FH1_REPORT_RING("[native] C6 pass key: {} reused ones checked with XXH3, 0 differences; keeps checking 1 in "
                       "4096", n);
    }
    return computed;
  }

  // Every 10 s, how much the small per-draw savings save.
  void ReportDetails() {
    const auto now = std::chrono::steady_clock::now();
    if (now - details_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = details_report_ == std::chrono::steady_clock::time_point{};
    details_report_ = now;
    const uint64_t framings = framing_hits_report_ + framing_calculations_report_;
    if (!first && framings) {
      FH1_REPORT_RING("[native] C6 details (build 179): cached framing in {} of {} draws ({:.1f} %; {}{}); pass "
                       "key without XXH3 {} times ({}); remembered useful height {} times ({}); colorless-draw "
                       "settings {}",
                  framing_hits_report_, framings,
                  100.0 * double(framing_hits_report_) / double(framings),
                  framing_cache_off_ ? "OFF by the guard" : framing_cache_ ? "on" : "off",
                  framing_checked_ >= kFramingsACheck ? ", guard passed" : ", checking",
                  keys_pass_fast_ - keys_pass_fast_previous_,
                  key_pass_fast_off_ ? "OFF by the guard" : key_pass_fast_ ? "on" : "off",
                  height_useful_memo_hits_ - height_useful_memo_hits_previous_,
                  height_useful_memo_off_ ? "OFF by the guard" : height_useful_memo_active_ ? "on" : "off",
                  cvars_by_frame_ ? "once per frame" : "on every draw");
    }
    framing_hits_report_ = 0;
    framing_calculations_report_ = 0;
    keys_pass_fast_previous_ = keys_pass_fast_;
    height_useful_memo_hits_previous_ = height_useful_memo_hits_;
  }

  // The guard of the direct-mapped pipeline cache. The same key in the map (the usual path) must give the
  // same VkPipeline as the slot. Otherwise it switches off and returns false.
  bool CheckSlotPipeline(uint64_t fingerprint, const KeyPipeline& key, VkPipeline in_slot, uint64_t n) {
    ++pipelines_direct_checked_;
    VkPipeline of_map = VK_NULL_HANDLE;
    if (const auto it = pipelines_.find(fingerprint);
        it != pipelines_.end() && std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
      of_map = it->second.second;
    }
    if (of_map != in_slot) {
      pipelines_direct_off_ = true;
      pipelines_direct_ = false;
      REXLOG_ERROR("[native] C6 pipelines: DIFFERENCE between the direct cache and the map (check {}, VS {} PS {} "
                   "specialization {:08X}). Off for the rest of the session: the map decides",
                   n, key.vs, key.ps, key.specialization);
      return false;
    }
    if (n == kPipelinesACheck) {
      FH1_REPORT_RING("[native] C6 pipelines: {} direct-cache hits checked against the map, 0 differences; keeps "
                       "checking 1 in 4096", n);
    }
    return true;
  }

  /*
   * Measurement only (fh1_native_count_pipeline_changes). What changes at each vkCmdBindPipeline.
   *
   * BindPipeline costs 3.3-4.5 us per call in a race (0.3-0.4 per draw), and on each one NVK copies all
   * the pipeline's fixed state (vk_dynamic_graphics_state_copy, ~80 groups). If the change is state only,
   * with dynamic state (vkCmdSet*) no new pipeline would be needed. Before touching anything, the counts
   * and kinds have to be known:
   *  - cull mode, topology within the same class, Z test/write/function, stencil, bias and restart are
   *    EDS1/EDS2, core in Vulkan 1.3: the console (API 1.3.354) already provides them without touching
   *    the SDK;
   *  - blending, color masks and topology of another class need VK_EXT_extended_dynamic_state3, which NVK
   *    exposes on Maxwell and the SDK only enables with ui_vulkan_state_dynamic3.patch.
   * The new key is compared with the last one bound in the same command buffer, field by field and
   * without XXH3. Blending, masks and depth are compared in canonical form (StateCanonical: only what
   * PipelineOf really reads). If only bits PipelineOf does not use differ, the pipeline is effectively the
   * same: no effect. Cost: an 80-byte memcmp and about 60 operations per bind (0.3-0.4 binds per draw). It
   * changes nothing.
   */
  static uint32_t ClassTopology(uint32_t topology) {
    switch (topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        return 0;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        return 1;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return 3;
      default:
        return 2;  // triangles: list, strip and fan
    }
  }

  // What PipelineOf really reads from a key's blending, masks and depth, with its same criterion
  // (blendEnable per target; depthWriteEnable = test and write; the back face copies the front).
  static void StateCanonical(const KeyPipeline& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth) {
    masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      blend[i] = 0;
      if (!c.formats[i]) {
        continue;  // PipelineOf skips color targets not in the pass
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alpha_direct = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      if (((mask & 0x7) && !color_direct) || ((mask & 0x8) && !alpha_direct)) {
        blend[i] = m;  // blendEnable: factors count (without blending Vulkan ignores them)
      }
    }
    uint32_t d = c.formats[4] ? c.depth : 0;  // without a depth target PipelineOf ignores it
    d &= ~0x8u;                                      // PipelineOf does not read bit 3
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // without stencil its functions and operations do not count
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    depth = d;
  }

  // Classifies a vkCmdBindPipeline from Draw against the last key bound in this buffer.
  // after_begin_pass: BeginPass had forgotten the bound pipeline (pipeline_bound_ set to
  // VK_NULL_HANDLE).
  void CountChangePipeline(const KeyPipeline& new_entry, bool after_begin_pass) {
    uint64_t* const n = changes_pipeline_.data();
    ++n[kChangeBindings];
    n[kChangeAfterPass] += after_begin_pass ? 1 : 0;
    const KeyPipeline& a = key_bound_;
    constexpr uint32_t kBitsTestAlpha = 0x2u | (0x7u << kSpecFunctionAlphaOffset);
    if (!key_bound_valid_) {
      ++n[kChangeFirst];
    } else if (std::memcmp(&a, &new_entry, sizeof(new_entry)) == 0) {
      ++n[kChangeIdentical];  // only after BeginPass: Vulkan kept the bound pipeline across passes
    } else if (a.vs != new_entry.vs || a.ps != new_entry.ps) {
      ++n[kChangeShaders];
    } else if (a.entry != new_entry.entry) {
      ++n[kChangeEntry];
    } else if (std::memcmp(a.formats, new_entry.formats, sizeof(a.formats)) != 0) {
      ++n[kChangeFormats];
    } else if (const uint32_t spec = a.specialization ^ new_entry.specialization; spec != 0) {
      ++n[(spec & ~kBitsTestAlpha) == 0                      ? kChangeSpecAlpha
          : (spec & ~(kBitsTestAlpha | kSpecZEarly)) == 0 ? kChangeSpecZEarly
                                                              : kChangeSpecOther];
    } else {
      // Same shaders, input, formats and specialization: only fixed pipeline state changes.
      uint32_t blend_a[4], blend_n[4], masks_a, masks_n, zs_a, zs_n;
      StateCanonical(a, blend_a, masks_a, zs_a);
      StateCanonical(new_entry, blend_n, masks_n, zs_n);
      const bool topology = a.topology != new_entry.topology;
      const bool kind = ClassTopology(a.topology) != ClassTopology(new_entry.topology);
      const bool blend = std::memcmp(blend_a, blend_n, sizeof(blend_a)) != 0;
      const bool masks = masks_a != masks_n;
      const uint32_t zs = zs_a ^ zs_n;
      const uint32_t rast = a.rasterization ^ new_entry.rasterization;
      if (!topology && !blend && !masks && !zs && !rast) {
        ++n[kChangeWithoutEffect];  // only bits PipelineOf does not read: effectively the same pipeline
        n[kChangeWithoutEffectAfterPass] += after_begin_pass ? 1 : 0;
      } else {
        const bool eds12 = !blend && !masks && !kind;
        ++n[kChangeOnlyState];
        n[kChangeStateAfterPass] += after_begin_pass ? 1 : 0;
        n[kChangeEds12] += eds12 ? 1 : 0;
        n[kChangeEds12AfterPass] += (eds12 && after_begin_pass) ? 1 : 0;
        n[kFieldTopology] += topology ? 1 : 0;
        n[kFieldClassTopology] += kind ? 1 : 0;
        n[kFieldBlend] += blend ? 1 : 0;
        n[kFieldMasks] += masks ? 1 : 0;
        n[kFieldZ] += (zs & 0x76u) ? 1 : 0;
        n[kFieldStencil] += (zs & ~0x76u) ? 1 : 0;
        n[kFieldFace] += (rast & 0x7u) ? 1 : 0;
        n[kFieldReset] += (rast & 0x8u) ? 1 : 0;
        n[kFieldBias] += (rast & 0x10u) ? 1 : 0;
      }
    }
    key_bound_ = new_entry;
    key_bound_valid_ = true;
  }

  // Every 20 s, what changes at each vkCmdBindPipeline of the ring (measurement only). Reads its cvar on
  // every call: ReportPipelinesDirect calls it, once per frame.
  void ReportChangesPipeline(std::chrono::steady_clock::time_point now) {
    const bool count_2 = REXCVAR_GET(fh1_native_count_pipeline_changes);
    if (count_2 != count_changes_pipeline_) {
      count_changes_pipeline_ = count_2;
      key_bound_valid_ = false;  // what was tracked while not counting cannot be compared
    }
    if (now - changes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = changes_report_ == std::chrono::steady_clock::time_point{};
    const double seconds = std::chrono::duration<double>(now - changes_report_).count();
    changes_report_ = now;
    const uint64_t draws = drawn_ - changes_draws_previous_;
    const uint64_t frames = frame_ - changes_frames_previous_;
    changes_draws_previous_ = drawn_;
    changes_frames_previous_ = frame_;
    const std::array<uint64_t, kChangesN> n = changes_pipeline_;
    changes_pipeline_.fill(0);
    if (first || !count_changes_pipeline_ || !n[kChangeBindings] || !draws || !frames) {
      return;
    }
    const double bindings = double(n[kChangeBindings]);
    const double f = double(frames);
    const auto by_draw = [&](uint64_t x) { return double(x) / double(draws); };
    const auto pct = [&](uint64_t x) { return 100.0 * double(x) / bindings; };
    const uint64_t spec = n[kChangeSpecAlpha] + n[kChangeSpecZEarly] + n[kChangeSpecOther];
    FH1_REPORT_RING(
        "[native] C6 pipeline changes (build 184, measurement only): {} vkCmdBindPipeline in {:.0f} s ({:.3f} per "
        "draw, {:.1f} per frame; {} are the first of a pass) | no previous key {} | the SAME key after starting a "
        "pass {} ({:.1f} %) | shaders change {} ({:.1f} %) | other input {} | other formats {} | specialization {} "
        "({:.1f} %: alpha test {}, with early Z {}, other {}) | no effect (bits PipelineOf does not read) {} "
        "({:.1f} %) | STATE ONLY {} ({:.1f} %, {:.3f} per draw): EDS1/EDS2 without touching the SDK {}, the rest "
        "needs EDS3",
        n[kChangeBindings], seconds, by_draw(n[kChangeBindings]), bindings / f, n[kChangeAfterPass],
        n[kChangeFirst], n[kChangeIdentical], pct(n[kChangeIdentical]), n[kChangeShaders], pct(n[kChangeShaders]),
        n[kChangeEntry], n[kChangeFormats], spec, pct(spec), n[kChangeSpecAlpha], n[kChangeSpecZEarly],
        n[kChangeSpecOther], n[kChangeWithoutEffect], pct(n[kChangeWithoutEffect]), n[kChangeOnlyState],
        pct(n[kChangeOnlyState]), by_draw(n[kChangeOnlyState]), n[kChangeEds12]);
    // Avoidable binds per frame with each fix, each one separately (those of the first draw of a pass are
    // not avoided by dynamic state or by the canonical key while BeginPass forgets the bound pipeline).
    constexpr double kUsByBinding = 3.0;  // BindPipeline: 3.3-4.5 us per call in a race
    const uint64_t canonical = n[kChangeWithoutEffect] - n[kChangeWithoutEffectAfterPass];
    const uint64_t eds12 = n[kChangeEds12] - n[kChangeEds12AfterPass];
    const uint64_t eds123 = n[kChangeOnlyState] - n[kChangeStateAfterPass];
    FH1_REPORT_RING(
        "[native] C6 pipeline changes, state-only fields: topology {} (of class {}), blend {}, masks {}, Z {}, "
        "stencil {}, cull face {}, restart {}, bias {}; {} of them the first of a pass | avoidable binds per frame "
        "(~3 us each): {:.1f} with the canonical key ({:.2f} ms), {:.1f} without forgetting the pipeline when a "
        "pass starts ({:.2f} ms), {:.1f} with EDS1/EDS2 ({:.2f} ms), {:.1f} with EDS1/EDS2/EDS3 ({:.2f} ms)",
        n[kFieldTopology], n[kFieldClassTopology], n[kFieldBlend], n[kFieldMasks], n[kFieldZ],
        n[kFieldStencil], n[kFieldFace], n[kFieldReset], n[kFieldBias], n[kChangeStateAfterPass],
        double(canonical) / f, double(canonical) / f * kUsByBinding / 1000.0, double(n[kChangeIdentical]) / f,
        double(n[kChangeIdentical]) / f * kUsByBinding / 1000.0, double(eds12) / f,
        double(eds12) / f * kUsByBinding / 1000.0, double(eds123) / f, double(eds123) / f * kUsByBinding / 1000.0);
  }

  // Every 10 s, the hit rate of the direct-mapped pipeline cache.
  void ReportPipelinesDirect() {
    const auto now = std::chrono::steady_clock::now();
    ReportChangesPipeline(now);  // reads its cvar every frame and writes every 20 s
    TryPrewarm();       // starts the thread as soon as the library is loaded
    ReportPrewarm(now);  // every 10 s, if there is anything new, and its guard
    if (now - pipelines_direct_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = pipelines_direct_report_ == std::chrono::steady_clock::time_point{};
    pipelines_direct_report_ = now;
    const uint64_t hits = pipelines_direct_hits_ - pipelines_direct_hits_previous_;
    const uint64_t misses = pipelines_direct_misses_ - pipelines_direct_misses_previous_;
    pipelines_direct_hits_previous_ = pipelines_direct_hits_;
    pipelines_direct_misses_previous_ = pipelines_direct_misses_;
    if (!first && (hits || misses)) {
      FH1_REPORT_RING("[native] C6 pipelines (build 179): direct cache {} hits and {} misses ({:.1f} % of the "
                       "lookups the one-entry shortcut does not solve); {} checked against the map ({})",
                  hits, misses, 100.0 * double(hits) / double(hits + misses),
                  pipelines_direct_checked_,
                  pipelines_direct_off_ ? "OFF by the guard"
                  : pipelines_direct_checked_ >= kPipelinesACheck ? "guard passed"
                                                                           : "checking");
    }
  }

  /*
   * The "indices" stage once rose from ~1.4 to ~2.6 us per draw without any change to its code. Four
   * cuts inside it (C6 substages 15-18), only on the timed draws (1 in 64): they cost four clock reads on
   * those draws, so the "indices" stage reads ~0.2-0.3 us higher than without them.
   */
  void CutSubstage(size_t k, std::chrono::steady_clock::time_point& t) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
    ++sub_n_[k];
    t = now;
  }

  void Stage(size_t stage, std::chrono::steady_clock::time_point& mark) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    stages_ns_[stage] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
    mark = now;
  }

  // Diagnostic fh1_native_diag_repeated_vertices: vertex bytes that repeat address, size, byte order
  // and content within the same frame or relative to an earlier frame.
  void NoteVerticesRepeated(uint64_t address, uint32_t bytes, uint32_t order,
                               const uint8_t* data) {
    const auto before = std::chrono::steady_clock::now();
    const uint64_t fingerprint = XXH3_64bits(data, bytes);
    const uint64_t key =
        XXH3_64bits_withSeed(&address, sizeof(address), (uint64_t(bytes) << 2) | order);
    if (vertices_seen_.size() > 200000) {
      vertices_seen_.clear();  // memory cap of the diagnostic
    }
    VerticesSeen& seen = vertices_seen_[key];
    if (seen.fingerprint == fingerprint && seen.frame == frame_) {
      bytes_repeated_frame_ += bytes;
    } else if (seen.fingerprint == fingerprint && seen.frame != UINT64_MAX &&
               seen.frame < frame_) {
      bytes_equal_previous_ += bytes;
    }
    seen.frame = frame_;
    seen.fingerprint = fingerprint;
    ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - before)
                                      .count());
  }

  void FinishPass() override {
    if (pass_active_) {
      /*
       * Fallback path of the deferred sky. If nothing has emitted it yet (no transparent draw arrived, or the
       * pass closes because of a copy, a resolve or a submission), it is emitted here, inside the pass and
       * before vkCmdEndRenderPass. Losing the sky would be a visible and serious bug, so this is the last
       * place it can be, and it is always reached: the command buffer is only closed through SendWork
       * -> BeforeOfSend -> FinishPass.
       */
      if (sky_pending_) {
        EmitSkyDeferred(pass_commands_, kSkyByEndOfPass);
      }
      // A host occlusion query cannot stay open outside its pass.
      if (query_occlusion_ != UINT32_MAX) {
        context_->FinishQueryOcclusion(query_occlusion_);
        query_occlusion_ = UINT32_MAX;
      }
      const auto before_end = std::chrono::steady_clock::now();
      dfn_.vkCmdEndRenderPass(pass_commands_);
      BarrierGlobal(pass_commands_);
      if (statistics_pass_ != UINT32_MAX) {
        context_->FinishStatistics(statistics_pass_);
        statistics_pass_ = UINT32_MAX;
      }
      ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_end).count());
      const uint32_t category_closed = category_pass_;
      pass_active_ = false;
      // Just in case. A pending sky with the pass already closed cannot be recorded; it is counted so it
      // shows up in the report instead of disappearing silently.
      if (sky_pending_) {
        sky_pending_ = false;
        ++sky_lost_;
      }
      // As soon as the shadow pass closes, submit to the GPU. Once per frame: the flag keeps reopened passes
      // from submitting again. SendYWait submits and keeps recording in the next slot; it waits for
      // nothing (the name is misleading).
      if (category_closed == kGpuShadows && !sent_after_shadows_ && context_ &&
          REXCVAR_GET(fh1_native_send_after_shadows)) {
        sent_after_shadows_ = true;
        ++submissions_after_shadows_;
        context_->SendYWait();
      }
    }
  }

  void BeforeOfSend() override {
    WaitUploads();  // deferred vertex copies, before flushing the mapping and submitting
    FinishPass();
    // Whatever is still on the texture bind thread, with its views, descriptors, barriers and copies, goes
    // into this upload buffer before it is closed (fh1_native_texture_binding_thread). It is the last thing
    // recorded: after the pass closes (which may emit the deferred sky), and meanwhile the thread has kept
    // binding.
    CollectBindings(true);
    // The data of the new textures prepared by the hash thread, into this upload buffer before the mapping
    // is flushed and it is submitted (fh1_native_texture_fingerprint_thread). Their barrier and copy are
    // already recorded.
    CollectFingerprints();
    if (upload_used_ && !upload_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, upload_memory_, upload_type_, 0,
                                                    upload_size_real_, upload_used_);
    }
    if (shared_separate_ && shared_used_ && !shared_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, shared_memory_, shared_type_, 0,
                                                    shared_size_real_, shared_used_);
      shared_bytes_published_ += shared_used_;
    }
  }

  void UseSlot(uint32_t slot) override {
    WaitUploads();  // already empty after BeforeOfSend; just in case, before switching buffers
    ReportCopies();  // every 10 s, who made the vertex copies and how long the ring waited
    ReportCacheFetch();  // every 10 s, the per-fetch sampler table
    // A texture may arrive here with its bind in flight (it was prepared before this submission's first
    // Record) and that is normal; what it cannot have is its data already in the upload buffer just
    // submitted.
    CheckBindingsToChangeOfBuffer();
    CheckFingerprintsToChangeOfBuffer();  // before resetting the upload buffer
    const BufferUpload& s = uploads_[slot % uploads_.size()];
    upload_ = s.buffer;
    upload_memory_ = s.memory_block;
    upload_size_real_ = s.size_real;
    upload_data_ = s.data;
    upload_address_ = s.address;
    slot_current_ = uint32_t(slot % uploads_.size());  // this slot's UBO set
    upload_used_ = 0;
    if (shared_separate_) {
      const BufferUpload& c = shared_bufs_[slot_current_];
      shared_data_ = c.data;
      shared_memory_ = c.memory_block;
      shared_size_real_ = c.size_real;
      shared_used_ = 0;
    }
    ++epoch_upload_;
    ++frame_;
    CloseFrameOfTheGuardOfSky();
    // fh1_reflection_visibility, once per submission (the guard may switch it off mid-session).
    measure_visibility_ = fh1::reflection_demand::MeasureVisibility();
    if (measure_visibility_ && frame_ >= witness_next_) {
      witness_pending_ = true;
    }
    // The upload buffer is reset here, so the recorded offsets are no longer valid. This also covers
    // SendYWait, which goes through here.
    dedupe_.NewFrame(frame_);
    sent_after_shadows_ = false;  // the post-shadow submission is once per frame
    /*
     * Diagnostic cvars are read once per frame, not per draw.
     *
     * REXCVAR_GET is not a variable read: it is FLAGS_##name##_storage_(), an out-of-line function call
     * (cvar.h:343). Four of them were being made per draw (one of them per vertex binding, not per draw),
     * and one returned a std::string that was then compared. With 2,345 draws per frame that shows, and it
     * serves no purpose: none of them changes within a frame.
     */
    diag_skip_ps_text_ = REXCVAR_GET(fh1_native_diag_skip_ps);
    if (diag_skip_ps_text_ == "file") {
      // FH1: "file" = the list is the first line of skip_ps.txt in the working folder, read again every 15 frames
      // (tools/skip_cycle.ps1 changes it while the user stays parked at a spot: which shader paints this?).
      static std::string list_file;
      static uint32_t frames = 0;
      if (frames++ % 15 == 0) {
        list_file.clear();
        if (std::FILE* f = std::fopen("skip_ps.txt", "r")) {
          char line[256] = {};
          if (std::fgets(line, sizeof(line), f)) {
            list_file = line;
          }
          std::fclose(f);
        }
      }
      diag_skip_ps_text_ = list_file;
    }
    diag_vertices_repeated_ = REXCVAR_GET(fh1_native_diag_repeated_vertices);
    dedupe_active_ = REXCVAR_GET(fh1_native_dedupe_vertices);
    exp_bias_ = REXCVAR_GET(fh1_native_exp_bias);
    vs_textures_ = REXCVAR_GET(fh1_native_vs_textures);
    vertices_base_zero_ = REXCVAR_GET(fh1_native_zero_based_vertices) && !vertices_base_zero_off_;
    diag_statistics_draw_ = REXCVAR_GET(fh1_native_per_draw_statistics_s) > 0;
    area_util_ = REXCVAR_GET(fh1_native_pass_useful_area);
    // The small per-draw savings.
    framing_cache_ = REXCVAR_GET(fh1_native_framing_cache) && !framing_cache_off_;
    height_useful_memo_active_ = REXCVAR_GET(fh1_native_useful_height_memo) && !height_useful_memo_off_;
    key_pass_fast_ = REXCVAR_GET(fh1_native_fast_pass_key) && !key_pass_fast_off_;
    cvars_by_frame_ = REXCVAR_GET(fh1_native_cvars_per_frame);
    ps_only_alpha_frame_ = PsOnlyAlpha();
    without_ps_without_color_frame_ = SinPsSinColor();
    without_vegetation_ = REXCVAR_GET(fh1_shadows_without_vegetation);
    pipelines_direct_ = REXCVAR_GET(fh1_native_direct_pipelines) && !pipelines_direct_off_;
    key_canonical_ = REXCVAR_GET(fh1_native_canonical_key) && !key_canonical_off_;  // phase 0a
    {
      const bool new_value = REXCVAR_GET(fh1_native_pipeline_between_passes);  // phase 0b
      if (new_value != pipeline_between_passes_ || !pipeline_between_passes_noted_) {
        pipeline_between_passes_ = new_value;
        pipeline_between_passes_noted_ = true;
        REXLOG_INFO("[native] C6 pipeline at pass start: {} (frame {})",
                    new_value ? "the bound one is KEPT (phase 0b of the dynamic state)" : "bound again, as before",
                    frame_);
      }
    }
    // Dynamic state phases 1 and 2, once per command buffer: the whole buffer uses one mode, because a
    // pipeline with a fixed state invalidates the dynamic value of that state.
    {
      uint32_t mode = 0;
      if (eds12_available_ && !eds_off_ && REXCVAR_GET(fh1_native_dynamic_state)) {
        mode |= kEds12;
      }
      if (eds3_available_ && !eds_off_ && REXCVAR_GET(fh1_native_dynamic_state3)) {
        mode |= kEds3;
      }
      if (mode != eds_mode_ || !eds_mode_noted_) {
        eds_mode_noted_ = true;
        REXLOG_INFO("[native] C6 dynamic state: {} (frame {})", NameModeEds(mode), frame_);
      }
      eds_mode_ = mode;
    }
    scissor_test_ = ScissorOfTest();
    without_blur_frame_ = REXCVAR_GET(fh1_native_no_blur);  // once per frame
    // The cheap PCF bit also changes the pipeline: once per frame.
    {
      bool new_value = REXCVAR_GET(fh1_native_cheap_pcf);
      const int32_t toggle_pcf = REXCVAR_GET(fh1_native_cheap_pcf_toggle_s);
      if (toggle_pcf > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / toggle_pcf) % 2 == 1;
      }
      if (new_value != pcf_cheap_) {
        pcf_cheap_ = new_value;
        REXLOG_INFO("[native] C2 shadow-map sampling: {}",
                    new_value ? "a single texel (cheap PCF)" : "3x3 pattern at half a texel");
      }
    }
    // The filter between mip levels is part of the sampler key, so it is also decided once per frame. Off
    // (the normal case), this is reading one cvar and comparing a bool.
    {
      bool new_value = REXCVAR_GET(fh1_native_test_point_mip);
      const int32_t toggle_mip = REXCVAR_GET(fh1_native_test_point_mip_toggle_s);
      if (toggle_mip > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / toggle_mip) % 2 == 1;
      }
      if (new_value != mip_point_test_) {
        mip_point_test_ = new_value;
        REXLOG_INFO("[native] C4 filter between mip levels: {}",
                    new_value ? "POINT (test: trilinear -> bilinear, the TMU does half)"
                          : "linear (the one the game asks for)");
      }
    }
    // fh1_native_diag_min_mip. Part of the sampler key; when it changes the per-texture sampler caches
    // are invalidated (generation_textures_, as when an image is retired) so it shows in the next frame.
    {
      const uint32_t new_value = uint32_t(std::clamp(int32_t(REXCVAR_GET(fh1_native_diag_min_mip)), 0, 4));
      if (new_value != diag_mip_minimum_) {
        diag_mip_minimum_ = new_value;
        ++generation_textures_;
        REXLOG_INFO("[native] C4 mip diagnostic: textures with mips start {} levels further down{}", new_value,
                    new_value ? " (DIAGNOSTIC: everything will look blurry)" : " (normal)");
      }
    }
    // The shadow map's own depth bias, once per frame.
    {
      const int32_t constant = std::clamp(int32_t(REXCVAR_GET(fh1_native_shadow_bias_constant)), 0, 100);
      const int32_t pending = std::clamp(int32_t(REXCVAR_GET(fh1_native_shadow_bias_slope)), 0, 100);
      if (constant != shadows_bias_constant_ || pending != shadows_bias_pending_ || !shadows_bias_noted_) {
        shadows_bias_constant_ = constant;
        shadows_bias_pending_ = pending;
        shadows_bias_noted_ = true;
        REXLOG_INFO("[native] C4 shadow-map depth bias: slope {:.1f}, constant {} (x1000 24-bit units){}",
                    double(pending) * 0.1, constant, (constant || pending) ? "" : " (off, as before)");
      }
    }
    // The 1/size bit changes the pipeline, so it is decided once per frame.
    {
      const int32_t toggle = REXCVAR_GET(fh1_native_inv_tex_size_toggle_s);
      bool new_value = REXCVAR_GET(fh1_native_inv_tex_size);
      if (toggle > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_ps_)
                                  .count();
        new_value = (seconds / toggle) % 2 == 1;
      }
      if (new_value != inv_size_tex_) {
        inv_size_tex_ = new_value;
        REXLOG_INFO("[native] C2 texture size: {}",
                    new_value ? "by constant" : "asked from the texture");
      }
    }
    /*
     * The early depth test also changes the pipeline (a different module), so it is decided once per
     * frame, like the PCF and the 1/size.
     */
    {
      const int32_t toggle = REXCVAR_GET(fh1_native_early_z_toggle_s);
      bool new_value = REXCVAR_GET(fh1_native_early_z);
      if (toggle > 0) {
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - start_alternation_z_)
                                  .count();
        new_value = (seconds / toggle) % 2 == 1;
      }
      if (new_value != z_early_ || !alternation_z_noted_) {
        z_early_ = new_value;
        alternation_z_noted_ = true;
        REXLOG_INFO("[native] C6 depth test: {} (frame {})",
                    new_value ? "BEFORE shading where Z is not written (early Z)"
                          : "after shading, as before",
                    frame_);
      }
    }
    skip_invisible_ = REXCVAR_GET(fh1_native_skip_invisible);
    // The deferred sky, also once per frame. If it is switched off mid-frame with one already deferred, the
    // pending one is still emitted through its usual path.
    {
      const bool new_value = REXCVAR_GET(fh1_native_deferred_sky);
      if (new_value != sky_deferred_ || !sky_noted_) {
        sky_deferred_ = new_value;
        sky_noted_ = true;
        REXLOG_INFO("[native] C6 sky: {} (frame {})",
                    new_value ? "DEFERRED until after the opaque draws of the pass"
                          : "in its original place (first, on the empty Z)",
                    frame_);
      }
    }
    ReportZEarly();
    ReportPipelinesDirect();
    ReportDetails();
    ReportBaseZero();
    ControlSet4();  // set 4 by differences in NVK (cvar, alternation and guard)
    ReportSet4();
    ControlDrawNvk();  // the draw path in NVK (measurement, improvements and guards)
    ReportDrawNvk();
    ReportKeyCanonical();  // Dynamic state, phase 0a
    ReportPipelineBetweenPasses();  // Dynamic state, phase 0b
    ReportStateDynamic();  // dynamic state phases 1 and 2
    ReportBindings();  // texture bind thread, every 10 s
    ReportFingerprints();  // texture hash thread, every 10 s
    ReportReuse();  // measurement only: new textures with a live one's content, every 10 s
    EvictTexturesSiDoesMissing();
    pool_textures_.ByFrame(frame_);  // grows with headroom, never exactly on demand
    // Test fh1_native_constants_ubo_toggle_s. Switched here because the new epoch forces all constants
    // to be allocated again: nothing allocated in one mode is reused in the other.
    if (toggle_ubo_s_ > 0) {
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_ubo_).count();
      const bool ubo = (seconds / toggle_ubo_s_) % 2 == 1;
      if (ubo != use_ubo_) {
        use_ubo_ = ubo;
        // With UBOs no push constants are recorded, so when going back to pointers the tracked state is
        // stale.
        std::memset(push_recorded_, 0, sizeof(push_recorded_));
        REXLOG_INFO("[native] constants test: {} (frame {})", ubo ? "by UBO" : "by pointer", frame_);
      }
    }
    ++submissions_;  // "C6 constants by UBO" report
    if (use_ubo_) {
      ++submissions_ubo_;
    }
    // What the prewarm thread actually compiles also goes into the cache and has to be saved.
    if (const uint32_t compiled = prewarm_compiled_.load(std::memory_order_relaxed);
        compiled != prewarm_counted_) {
      pipelines_without_save_ += compiled - prewarm_counted_;
      prewarm_counted_ = compiled;
    }
    // The pipeline cache is saved after 64 new pipelines, or after a minute with any new one: on the
    // Switch, exiting does not always reach the destructor.
    if (pipelines_without_save_ &&
        (pipelines_without_save_ >= 64 ||
         std::chrono::steady_clock::now() - cache_saved_ >= std::chrono::seconds(60))) {
      SaveCachePipelines();
    }
  }

  void InvalidateTextures() override { ++generation_textures_; }

  // Only the cache entries that use a view of those images. The views stay valid (they go with their
  // image); what has to be redone is which slot each fetch constant gets.
  void InvalidateImages(VkImage a, VkImage b) override {
    std::array<std::pair<uint32_t, uint32_t>, 64> slots{};
    size_t n = 0;
    for (VkImage image : {a, b}) {
      if (image == VK_NULL_HANDLE || (image == b && a == b)) {
        continue;
      }
      const auto it = views_by_image_.find(image);
      if (it == views_by_image_.end()) {
        continue;
      }
      for (uint64_t key : it->second) {
        const auto v = views_.find(key);
        if (v != views_.end() && v->second.image == image && n < slots.size()) {
          slots[n++] = {v->second.heap, v->second.slot};
        }
      }
    }
    if (!n) {
      return;
    }
    const auto affected = [&](const CacheSampler& c) {
      for (size_t i = 0; i < n; ++i) {
        if (slots[i].first == c.heap && slots[i].second == c.slot) {
          return true;
        }
      }
      return false;
    };
    for (CacheSampler& c : cache_samplers_) {
      if (affected(c)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
    for (CacheSampler& c : cache_fetch_) {
      if (affected(c)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
  }

  void OcclusionOpen(bool open) override {
    if (!open && query_occlusion_ != UINT32_MAX) {
      // Still inside the pass of the last counted draw: FinishPass would have closed it already.
      context_->FinishQueryOcclusion(query_occlusion_);
      query_occlusion_ = UINT32_MAX;
    }
    occlusion_open_ = open;
  }

  uint64_t Drawn() const override { return drawn_; }

  // fh1_native_texture_mb_max: with the texture cache above the limit, evict the textures unused for
  // the longest until it drops to 75 %, at most once every 60 frames.
  // It is safe without waiting for the GPU: a texture not prepared for 120 frames cannot be referenced by
  // any pending submission. The slot caches are valid for at most 32 frames after preparing, and only the
  // previous submission can still be on the GPU (Record waits for the slot before reusing it). The heap
  // slots are updated with UPDATE_AFTER_BIND, and the new generation invalidates the caches.
  static constexpr uint64_t kFramesWithoutUseForRelease = 120;
  /*
   * How much GPU memory there is and how much is left.
   *
   * The cache limit was picked by eye twice (384 MB and then 192) because this data was not available.
   * With VK_EXT_memory_budget the driver reports the budget and the usage; without it, at least the heap
   * sizes are visible. It goes with the cache report, every 256 new textures.
   */
  void NoteMemoryOfTheGpu() {
    const auto* instance = vulkan_device_->vulkan_instance();
    if (!instance) {
      return;
    }
    const auto& ifn = instance->functions();
    const VkPhysicalDevice physical = vulkan_device_->physical_device();
    const bool there_is_budget = vulkan_device_->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties2.pNext = there_is_budget ? &budget : nullptr;
    if (there_is_budget) {
      ifn.vkGetPhysicalDeviceMemoryProperties2(physical, &properties2);
    } else {
      ifn.vkGetPhysicalDeviceMemoryProperties(physical, &properties2.memoryProperties);
    }
    const auto& mp = properties2.memoryProperties;
    std::string text;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (!text.empty()) {
        text += " | ";
      }
      const bool local = (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (there_is_budget) {
        text += fmt::format("heap {}{}: {} MB used of {} MB budgeted (size {} MB)", i,
                             local ? " (GPU)" : "", budget.heapUsage[i] >> 20,
                             budget.heapBudget[i] >> 20, mp.memoryHeaps[i].size >> 20);
      } else {
        text += fmt::format("heap {}{}: {} MB", i, local ? " (GPU)" : "",
                             mp.memoryHeaps[i].size >> 20);
      }
    }
    REXLOG_INFO("[native] C3 GPU memory: {}{}; the texture cache holds {} MB of {} MB", text,
                there_is_budget ? "" : " (without VK_EXT_memory_budget: sizes only)", bytes_textures_ >> 20,
                textures_mb_max_);
    // Without this line there is no way to know whether the pool is working or fragmenting. A vegetation
    // counter once existed and was never printed.
    if (pool_textures_.Active()) {
      REXLOG_INFO("[native] C3 {}", pool_textures_.Summary());
    }
  }

  /*
   * Without stutters.
   *
   * The first version waited until the limit was exceeded and evicted 25 % at once. In a race that is a
   * stutter, and a stutter every few minutes is no better than running out of memory every fourteen.
   *
   * Now a few are evicted per frame as soon as the cache nears the limit, and only cold textures: those
   * unused for 120 frames, i.e. two to four seconds. They are not in the working set, so they do not have
   * to be uploaded again right away and there is no thrashing. If there are not enough cold ones, nothing
   * is forced: the cache is allowed to grow, since going a little over is better than uploading and
   * evicting the same thing.
   */
  static constexpr uint32_t kReleaseByFrame = 4;

  void EvictTexturesSiDoesMissing() {
    /*
     * Descriptor slots (2D, 3D, cube). Eviction used to look only at megabytes, so thousands of small
     * textures could fill the 4,096 2D slots while the cache was still under its MB limit. From then on a
     * new texture got slot 0, the empty texture: with alpha test the whole surface disappears (suspected
     * cause of the faces missing in a story cinematic after a long session, 2026-10-03; not proven).
     * Now cold textures are also released little by little once a heap is 75 % full, and the log says
     * when a heap gets there and how many slots could not be given out.
     */
    bool slots_tight = false;
    for (uint32_t h = 0; h < 3; ++h) {
      const auto& m = heaps_[h];
      if (m.capacity == 0) {
        continue;  // heaps not created yet
      }
      const uint32_t used = m.next - uint32_t(m.free.size());
      if (used > m.capacity / 4 * 3) {
        slots_tight = true;
        if (!slots_warned_[h]) {
          slots_warned_[h] = true;
          REXLOG_INFO("[native] C3: texture heap {} is {} of {} slots full: cold textures are now also released "
                      "for slots ({} textures, {} MB)",
                      h, used, m.capacity, textures_.size(), bytes_textures_ >> 20);
        }
      }
    }
    if (heap_full_ != heap_full_logged_ && frame_ >= warning_heap_full_ + 600) {
      warning_heap_full_ = frame_;
      heap_full_logged_ = heap_full_;
      REXLOG_WARN("[native] C3: texture heap full {} times so far: those textures were drawn with the empty one "
                  "({} textures, {} MB)",
                  heap_full_, textures_.size(), bytes_textures_ >> 20);
    }
    const uint64_t limit = uint64_t(std::max(textures_mb_max_, 0)) << 20;
    if (slots_tight) {
      ReleaseSomeFewCold();
    }
    if (!limit) {
      return;
    }
    // Little by little, from 75 % of the limit. That is what avoids the stutter.
    if (!slots_tight && bytes_textures_ > limit / 4 * 3) {
      ReleaseSomeFewCold();
    }
    // Safety net: if it still goes over the limit (because almost everything is hot), one batch every 60
    // frames. With the above working, this should almost never trigger.
    if (bytes_textures_ > limit && frame_ >= attempt_eviction_ + 60) {
      attempt_eviction_ = frame_;
      ReleaseTextures(limit / 4 * 3, kFramesWithoutUseForRelease, "above the limit");
    }
  }

  // The kReleaseByFrame oldest cold ones. Without sorting the whole cache: they are picked on the fly.
  void ReleaseSomeFewCold() {
    std::array<std::pair<uint64_t, uint64_t>, kReleaseByFrame> chosen{};  // (frame, key)
    uint32_t how_many = 0;
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight, nor those owned by the hash thread.
      if (texture.image.image == VK_NULL_HANDLE || texture.needs_upload || texture.in_flight || texture.fingerprint_work ||
          texture.frame == UINT64_MAX ||
          texture.frame + kFramesWithoutUseForRelease >= frame_) {
        continue;
      }
      if (how_many < kReleaseByFrame) {
        chosen[how_many++] = {texture.frame, key};
        continue;
      }
      // Replaces the most recent of the chosen ones, if this one is older.
      uint32_t worst = 0;
      for (uint32_t i = 1; i < how_many; ++i) {
        if (chosen[i].first > chosen[worst].first) {
          worst = i;
        }
      }
      if (texture.frame < chosen[worst].first) {
        chosen[worst] = {texture.frame, key};
      }
    }
    if (how_many == 0) {
      return;  // all hot: let it grow rather than thrash
    }
    std::unordered_set<VkImage> images;
    for (uint32_t i = 0; i < how_many; ++i) {
      auto it = textures_.find(chosen[i].second);
      if (it == textures_.end()) {
        continue;
      }
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    ReleaseImages(images);
    textures_released_ += images.size();
    released_little_a_little_ += images.size();
    if (frame_ >= warning_trickle_ + 600) {  // one warning every 600 frames, not one per texture
      warning_trickle_ = frame_;
      REXLOG_INFO("[native] C3: texture cache near the limit: cold ones are released a few at a time ({} in "
                  "total); {} textures and {} MB of {} MB remain",
                  released_little_a_little_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
    }
  }

  /*
   * The last resort, when the GPU has no more memory to give.
   *
   * Once nvMapCreate started failing even for 64 KB there was no way back: the driver ended up
   * quarantining ranges and the screen went black with the audio still playing. Before it gets to that,
   * half the texture cache is released.
   *
   * Normal eviction only touches what has been unused for 120 frames, because images are destroyed at
   * once and the GPU could still be reading them. Here that margin is not needed: it waits for the GPU to
   * finish everything in flight, and then none is in use. It costs a one-frame stutter; crashing costs
   * the game.
   */
  bool ReleaseTexturesByMissingOfMemory() override {
    if (textures_.empty() || bytes_textures_ == 0 || releasing_by_missing_of_memory_) {
      return false;  // the guard keeps an allocation inside the cleanup itself from re-entering here
    }
    releasing_by_missing_of_memory_ = true;
    bool algo = false;
    if (context_ && context_->WaitGpuOfAll()) {
      algo = ReleaseTextures(bytes_textures_ / 2, 0, "OUT OF GPU MEMORY") > 0;
    }
    releasing_by_missing_of_memory_ = false;
    return algo;
  }

  // Actually retires a group of images. First the views and their slots (pointed at the empty texture),
  // then the images. Used by both the gradual and the batch eviction.
  void ReleaseImages(const std::unordered_set<VkImage>& images) {
    ++generation_textures_;
    for (auto it = views_.begin(); it != views_.end();) {
      if (!images.count(it->second.image)) {
        ++it;
        continue;
      }
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_by_image_.erase(it->second.image);  // all views of that image go
      it = views_.erase(it);
    }
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (!images.count(it->second.image.image)) {
        ++it;
        continue;
      }
      RemoveContentTexture(it->second, it->first);  // measurement only: fh1_native_diag_reuse
      DestroyImage(it->second.image);
      it = textures_.erase(it);
    }
  }

  // Evicts unused textures until below `target`. Returns the bytes freed.
  uint64_t ReleaseTextures(uint64_t target_2, uint64_t age_minimum, const char* reason) {
    std::vector<std::pair<uint64_t, uint64_t>> candidates_2;  // (last frame it was prepared, key)
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight (their image has no memory yet), nor those owned by the hash thread.
      if (texture.image.image != VK_NULL_HANDLE && !texture.needs_upload && !texture.in_flight && !texture.fingerprint_work &&
          texture.frame != UINT64_MAX &&
          texture.frame + age_minimum < frame_) {
        candidates_2.emplace_back(texture.frame, key);
      }
    }
    std::sort(candidates_2.begin(), candidates_2.end());
    const uint64_t before = bytes_textures_;
    std::unordered_set<VkImage> images;
    for (const auto& [last, key] : candidates_2) {
      if (bytes_textures_ <= target_2) {
        break;
      }
      auto it = textures_.find(key);
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    if (images.empty()) {
      return 0;
    }
    ReleaseImages(images);
    textures_released_ += images.size();
    REXLOG_INFO("[native] C3: texture cache {} ({} MB): released {} unused for more than {} frames ({} MB); {} "
                "textures and {} MB remain ({} released in total)",
                reason, before >> 20, images.size(), age_minimum,
                (before - bytes_textures_) >> 20, textures_.size(), bytes_textures_ >> 20, textures_released_);
    return before - bytes_textures_;
  }

  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR. Needed for images created without
  // TRANSFER_DST (the only ones the driver gives a ZCULL plane), because vkCmdClearDepthStencilImage
  // requires that usage (VUID-vkCmdClearDepthStencilImage-pRanges-02660). It costs no extra GPU time: it
  // is exactly how NVK implements that command internally (nvk_cmd_clear.c, clear_image opens a pass with
  // loadOp = CLEAR).
  bool ClearDepthInPass(VkCommandBuffer commands, const ImageNative& image,
                               float depth, uint32_t stencil) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !image.width ||
        !image.height) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {0, 0, 0, 0, uint32_t(image.format)};
    const VkRenderPass pass = PassOf(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[4] = image.view;
    const VkFramebuffer framebuffer = FramebufferDe(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.depthStencil = {depth, stencil};
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea.extent = {image.width, image.height};
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    BarrierGlobal(commands);
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    BarrierGlobal(commands);
    ++clears_depth_in_pass_;
    return true;
  }

  // fh1_native_clear_useful_area. Clears only the `area` rectangle of a color image with a
  // loadOp = CLEAR pass: what NVK does internally for vkCmdClearColorImage (nvk_cmd_clear.c), over that
  // rectangle. What lies outside is not touched (Vulkan only loads and stores the renderArea).
  void ForgetStateBound() override {
    // The same as on a new command buffer (see the draw), plus what the deferred sky forgets.
    pipeline_bound_ = VK_NULL_HANDLE;
    eds_valid_ = false;
    key_bound_valid_ = false;
    sets_bound_ = false;
    ubo_bound_ = false;
    state_recorded_ = false;
    bindings_recorded_ = 0;
    stencil_recorded_valid_ = false;
    type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  bool ClearColorInPass(VkCommandBuffer commands, const ImageNative& image, const VkClearColorValue& color,
                         const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !area.extent.width || !area.extent.height ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > image.width ||
        uint32_t(area.offset.y) + area.extent.height > image.height) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {uint32_t(image.format), 0, 0, 0, 0};
    const VkRenderPass pass = PassOf(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[0] = image.view;
    const VkFramebuffer framebuffer = FramebufferDe(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.color = color;
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    BarrierGlobal(commands);
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    BarrierGlobal(commands);
    return true;
  }

  // fh1_native_framebuffers_forget_views. C2 is about to destroy this view (Destroy, with the GPU
  // idle for that image): the FramebufferDe framebuffers that use it are destroyed and removed from the
  // cache. It walks the created framebuffers (dozens); never during a draw.
  // Self-checking guard: the cache and its view list must stay in step (only FramebufferDe inserts) and
  // no pass may be open (whoever destroys the view has already submitted the work). Otherwise: DIFFERENCE
  // in the log and, for the rest of the session, nothing is destroyed at runtime: the affected
  // framebuffers (or the whole cache, if they are out of step) are retired and destroyed at shutdown.
  // Retiring is never worse than the previous behaviour.
  void ForgetView(VkImageView view) override {
    if (view == VK_NULL_HANDLE || !REXCVAR_GET(fh1_native_framebuffers_forget_views)) {
      return;
    }
    const bool a_la_par = framebuffers_views_.size() == framebuffers_.size();
    if (!fb_retire_ && (pass_active_ || !a_la_par)) {
      fb_retire_ = true;
      REXLOG_ERROR("[native] C6 framebuffers (build 184): DIFFERENCE when destroying view {:016X}: {}. For the "
                   "rest of the session the affected framebuffers are retired without destroying them (destroyed "
                   "at shutdown)",
                   uint64_t(reinterpret_cast<uintptr_t>(view)),
                   pass_active_ ? "a pass is open (whoever destroys the view has not submitted the work)"
                                : "the cache and its list of views do not match (someone creates framebuffers "
                                  "another way)");
    }
    uint32_t outside = 0;
    if (!a_la_par) {
      // Unknown which ones use the view: drop them all (they are rebuilt on request).
      for (auto& [key, framebuffer] : framebuffers_) {
        fb_retired_.push_back(framebuffer);
      }
      outside = uint32_t(framebuffers_.size());
      framebuffers_.clear();
      framebuffers_views_.clear();
    } else {
      for (auto it = framebuffers_views_.begin(); it != framebuffers_views_.end();) {
        if (std::find(it->second.begin(), it->second.end(), view) == it->second.end()) {
          ++it;
          continue;
        }
        const auto fb = framebuffers_.find(it->first);
        if (fb != framebuffers_.end()) {
          if (fb_retire_) {
            fb_retired_.push_back(fb->second);
          } else {
            dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
          }
          framebuffers_.erase(fb);
          ++outside;
        }
        it = framebuffers_views_.erase(it);
      }
    }
    ++fb_views_forgotten_;
    fb_forgotten_ += outside;
    if (fb_views_forgotten_ <= 16 || (fb_views_forgotten_ & 63) == 0) {
      FH1_REPORT_RING("[native] C6 framebuffers (build 184): view {:016X} destroyed: {} framebuffers {} with it; "
                       "{} remain in the cache ({} views and {} framebuffers since startup; {} retired)",
                           uint64_t(reinterpret_cast<uintptr_t>(view)), outside,
                           fb_retire_ ? "retired (not destroyed)" : "destroyed", framebuffers_.size(),
                           fb_views_forgotten_, fb_forgotten_, fb_retired_.size());
    }
  }

  void ForgetImage(VkImage image) override {
    ++generation_textures_;  // the sampler cache could point to a retired view
    const auto index = views_by_image_.find(image);  // without walking views_
    if (index == views_by_image_.end()) {
      return;
    }
    for (uint64_t key : index->second) {
      const auto it = views_.find(key);
      if (it == views_.end() || it->second.image != image) {
        continue;
      }
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_.erase(it);
    }
    views_by_image_.erase(index);
  }

  StatisticsDraws Statistics() const override {
    StatisticsDraws e;
    e.drawn = drawn_;
    e.rejected = rejected_;
    e.pipelines = pipelines_.size();
    e.textures = textures_.size();
    e.uploads_texture = uploads_texture_;
    e.megabytes_uploaded = megabytes_bytes_ >> 20;
    e.megabytes_textures = bytes_textures_ >> 20;
    e.ms_pipelines = ns_pipelines_ / 1000000;
    e.passes = passes_started_;
    e.submissions_full = submissions_full_;
    e.ns_submissions_full = ns_submissions_full_;
    e.bytes_vertices = bytes_vertices_;
    e.dedupe_hits = dedupe_.hits();
    e.dedupe_bytes = dedupe_.bytes_saved();
    e.dedupe_collisions = dedupe_.collisions();
    e.bytes_indices = bytes_indices_uploaded_;
    e.samplers = samplers_prepared_;
    e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
    e.ns_passes = ns_passes_;
    e.ns_vertices = ns_vertices_;
    e.entries_computed = entries_computed_;
    e.ns_entries = ns_entries_;
    e.entries_reused = entries_reused_ + entries_cache_hits_;
    e.passes_by_generation = passes_by_generation_;
    e.passes_by_target = passes_by_target_;
    e.passes_resumed = passes_resumed_;
    e.ns_render_pass = ns_render_pass_;
    e.texels_passes = texels_passes_;
    e.texels_by_category = texels_by_category_;
    e.draws_by_category = draws_by_category_;
    e.draws_ps_useless = draws_ps_useless_;
    e.draws_ps_needed = draws_ps_needed_;
    e.shadows_alpha_active = shadows_alpha_active_;
    e.shadows_alpha_off = shadows_alpha_off_;
    e.triangles_by_category = triangles_by_category_;
    e.passes_by_category = passes_by_category_;
    e.submissions = submissions_;
    e.submissions_ubo = submissions_ubo_;
    e.shared_looked = shared_looked_;
    e.shared_changed = shared_changed_;
    e.bytes_repeated_frame = bytes_repeated_frame_;
    e.bytes_equal_previous = bytes_equal_previous_;
    e.ns_hash_vertices = ns_hash_vertices_;
    e.causes.assign(causes_.begin(), causes_.end());
    e.stages_ns = stages_ns_;
    e.scene_with_discard = scene_with_discard_;
    e.scene_without_discard = scene_without_discard_;
    e.drawn_timed = drawn_timed_;
    e.vegetation_soon = draws_vegetation_soon_;
    std::sort(e.causes.begin(), e.causes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejected_;
    ++causes_[cause];
    last_cause_ = cause;
    Notify(cause, text);
    return false;
  }

  void Notify(uint32_t cause, const char* text) {
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C6: {} (cause {})", text, cause);
    }
  }

  // --- Texture binds on a separate thread (fh1_native_texture_binding_thread) ----------------------------------
  /*
   * Why. With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
   * still makes two ioctls: reserving the plane's VA with its pte_kind and mapping the pool chunk into it
   * (nvk_image_plane_bind, nvk_image.c:1696-1710; vkCreateImage reserves no VA except for sparse images,
   * :1337-1350). All our textures are tiled with pte_kind GENERIC_16BX2 (nil/image.rs:439-440 and 876-918),
   * so both are always paid. In a race: 336 VA reservations in 95 ms and 336 mappings in 111 ms, ~0.6 ms of
   * wall time per texture with the ring asleep in the ioctl. Entering a new zone brings 33-45 textures in
   * one frame: 19-27 ms of stalled ring that did not show up in "textures X ms".
   *
   * How. The ring does the CPU part (vkCreateImage, requirements, pool chunk, descriptor slot) and only
   * queues the vkBindImageMemory. The draw is recorded with its slot and UploadTexture leaves the data in the
   * upload buffer as usual. In BeforeOfSend (CollectBindings) it waits for whatever is missing, creates the
   * views, writes the descriptors (UPDATE_AFTER_BIND: legal until submission) and records the barrier and
   * the copy in that same upload buffer, which goes in the same vkQueueSubmit, ahead of the work. The GPU
   * receives exactly the same thing in the same submission.
   *
   * The driver. vkBindImageMemory from another thread is safe: the VA and the mapping are under
   * device->va_mutex (horizon/nouveau_horizon_vm.c:173 and 377), the mapping refcount under
   * memory_identity_mutex (nouveau_horizon_memory.c:1065), and everything else belongs to that image. The
   * pool is not touched from the thread: it stays single-threaded (fh1_native_texture_pool.h).
   *
   * Threads (see nfsc-nx docs/nfsmw-nx/platform-notes.md, Threads). Shared state is under bindings_mutex_, and every decision
   * to sleep or wake is taken with the lock held; the thread only reads the image, memory and offset of its
   * request and stores the result under the lock. The ring's wait has a timeout and logs every 2 s.
   * Persistent thread (never detached), joined in the destructor before any image is destroyed. Priority
   * 0x2D: a host service, above the guest.
   *
   * Self-checking guard. Observing phase: the first kBindingsACheck wait for their bind right after
   * queuing it (same order as before: only the thread doing the ioctl changes). If all succeed, applying
   * phase. In any phase, a failed bind, a wait of more than 2 s or a deferred copy that can no longer go in
   * its upload buffer: REXLOG_ERROR with the DIFFERENCE and off for the session (the usual path returns;
   * whatever was in flight finishes through the fallback path). And if for 3 reports in a row the ring
   * waits for the thread longer than the thread takes to bind, it does not pay off: it switches off too.
   */
  void DecideBindings() {
    const bool requested = REXCVAR_GET(fh1_native_texture_binding_thread);
    bindings_priority_ = std::clamp<int32_t>(REXCVAR_GET(fh1_native_texture_binding_thread_priority), 0x2C, 0x3B);
    bindings_phase_ = requested ? kBindingsWatching : kBindingsOff;
    REXLOG_INFO("[native] C3: binding of new textures on a separate thread (fh1_native_texture_binding_thread) = "
                "{}",
                requested ? fmt::format("YES, priority {:#x}; WATCHING phase: the first {} wait for their binding "
                                        "right away (pool textures only, and the pool is {})",
                                     bindings_priority_, kBindingsACheck,
                                     pool_textures_.Active() ? "on" : "OFF: it will not be used")
                       : std::string("no, on the ring thread as always"));
  }

  void TurnOffBindings(const std::string& reason) {
    if (bindings_phase_ == kBindingsOff) {
      return;
    }
    bindings_phase_ = kBindingsOff;
    REXLOG_ERROR("[native] C3: texture binding thread OFF for the rest of the session: {}. New textures are "
                 "created whole on the ring thread again ({} bound on the thread, {} failed, {} views and {} "
                 "copies deferred)",
                 reason, bindings_thread_total_, bindings_failed_, views_deferred_, copies_deferred_);
  }

  // Queues the vkBindImageMemory of an image. false = not queued (queue full or no thread): the usual path.
  bool EnqueueBinding(VkImage image, VkDeviceMemory memory_block, VkDeviceSize offset, uint64_t& ticket) {
    if (!bindings_thread_.joinable()) {
      try {
        bindings_thread_ = std::thread([this] { LoopBindings(); });
      } catch (const std::system_error& error) {
        TurnOffBindings(fmt::format("DIFFERENCE: the thread could not be created ({})", error.what()));
        return false;
      }
    }
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      if (bindings_requested_ - bindings_collected_ >= kQueueBindings) {
        ++bindings_queue_full_;
        return false;
      }
      RequestBinding& request = queue_bindings_[bindings_requested_ & (kQueueBindings - 1)];
      request.image = image;
      request.memory_block = memory_block;
      request.offset = offset;
      request.result = VK_NOT_READY;
      request.ns = 0;
      ticket = bindings_requested_++;
      notify = bindings_sleeping_;  // decided under the lock: if it sleeps, it is inside its wait with the predicate
    }
    if (notify) {
      bindings_cv_.notify_one();
    }
    return true;
  }

  // The thread: the requests' vkBindImageMemory calls, in order. It sleeps on a decision taken under the
  // lock; it never spins. It does not write to the log (the ring records the figures in ReportBindings).
  void LoopBindings() {
    rex::thread::set_current_thread_name("FH1 texture binding");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(bindings_priority_));
#else
    const bool priority_ok = true;
#endif
    std::unique_lock<std::mutex> lock(bindings_mutex_);
    bindings_priority_ok_ = priority_ok;
    for (;;) {
      if (bindings_done_ == bindings_requested_) {
        if (bindings_stop_) {
          return;  // stop, nothing pending
        }
        if (bindings_waiting_) {
          bindings_done_cv_.notify_one();
        }
        bindings_sleeping_ = true;
        bindings_cv_.wait(lock, [this] { return bindings_stop_ || bindings_done_ != bindings_requested_; });
        bindings_sleeping_ = false;
        continue;
      }
      // The slot is not reused until the ring collects it (bindings_collected_): it stays ours without the lock.
      RequestBinding& request = queue_bindings_[bindings_done_ & (kQueueBindings - 1)];
      const VkImage image = request.image;
      const VkDeviceMemory memory_block = request.memory_block;
      const VkDeviceSize offset = request.offset;
      lock.unlock();
      const auto before = std::chrono::steady_clock::now();
      const VkResult result = dfn_.vkBindImageMemory(device_, image, memory_block, offset);
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - before)
                                       .count());
      lock.lock();
      request.result = result;
      request.ns = ns;
      ns_bindings_thread_ += ns;
      ns_binding_worst_ = std::max(ns_binding_worst_, ns);
      ++bindings_done_;
      if (bindings_waiting_) {
        bindings_done_cv_.notify_one();
      }
    }
  }

  void StopBindings() {
    if (!bindings_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      bindings_stop_ = true;
    }
    bindings_cv_.notify_one();
    bindings_thread_.join();  // first finishes every queued request
    REXLOG_INFO("[native] C3: texture binding thread stopped ({} bound on the thread, {} failed, {} views and {} "
                "copies deferred, {} slots lost)",
                bindings_thread_total_, bindings_failed_, views_deferred_, copies_deferred_, slots_lost_);
  }

  // The ring waits until the thread has done the requests before `target`. With a timeout and a log line
  // every 2 s.
  void WaitBindings(uint64_t target_2) {
    std::unique_lock<std::mutex> lock(bindings_mutex_);
    if (bindings_done_ >= target_2) {
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    bindings_waiting_ = true;
    if (bindings_sleeping_) {
      bindings_cv_.notify_one();  // should not be needed (it has work); in case a wake-up was lost
    }
    int seconds = 0;
    while (!bindings_done_cv_.wait_for(lock, std::chrono::seconds(2),
                                        [this, target_2] { return bindings_done_ >= target_2; })) {
      seconds += 2;
      bindings_stuck_ = true;
      REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for texture bindings: target {}, done {}, "
                   "requested {}; the thread is {}",
                   seconds, target_2, bindings_done_, bindings_requested_,
                   bindings_sleeping_ ? "ASLEEP (lost notification)" : "awake (an ioctl that does not return)");
    }
    bindings_waiting_ = false;
    lock.unlock();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - before)
                                     .count());
    ns_wait_bindings_total_ += ns;
    ns_wait_bindings_report_ += ns;
    ns_wait_bindings_worst_ = std::max(ns_wait_bindings_worst_, ns);
    ++waits_bindings_report_;
    fh1::waits::g_ns_waiting_bindings.fetch_add(ns, std::memory_order_relaxed);
  }

  /*
   * The CPU part of CreateTexture (image, requirements, pool chunk) here, and its vkBindImageMemory to the
   * thread. false = nothing was touched and the caller continues through CreateTexture: thread off, pool off
   * or full, queue full, or (in the observing phase) a failed bind whose fallback path also failed.
   */
  bool CreateTextureInThread(Texture& texture, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                          uint32_t background, uint32_t levels) {
    if (bindings_phase_ == kBindingsWithoutDecide) {
      DecideBindings();
    }
    // With the out-of-memory test enabled, everything goes through CreateTexture: that test lives there.
    if (bindings_phase_ == kBindingsOff || !pool_textures_.Active() || test_without_memory_every_ > 0) {
      return false;
    }
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, background, levels);
    VkImage image = VK_NULL_HANDLE;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    dfn_.vkGetImageMemoryRequirements(device_, image, &req);
    VkDeviceMemory block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t id_block = 0xFFFFFFFFu;
    if (!pool_textures_.Reserve(req, block, offset, id_block)) {
      dfn_.vkDestroyImage(device_, image, nullptr);
      return false;  // no room in the pool: CreateTexture, which can take the dedicated path
    }
    uint64_t ticket = 0;
    if (!EnqueueBinding(image, block, offset, ticket)) {
      dfn_.vkDestroyImage(device_, image, nullptr);  // before the chunk, as the pool requires (not bound yet)
      pool_textures_.Free(id_block);
      return false;
    }
    texture.image.image = image;
    texture.image.memory_block = VK_NULL_HANDLE;  // from the pool: not freed on its own
    texture.image.pool_block = id_block;
    texture.image.width = width;
    texture.image.height = height;
    texture.image.format = format;
    texture.image.prepared = false;
    TextureInFlight flight;
    flight.texture = &texture;
    flight.ticket = ticket;
    flight.image = image;
    flight.format = format;
    flight.width = width;
    flight.height = height;
    flight.layers = layers;
    flight.background = background;
    flight.levels = levels;
    in_flight_.push_back(flight);
    texture.in_flight = uint32_t(in_flight_.size());
    images_in_flight_.insert(image);
    if (bindings_phase_ == kBindingsWatching) {
      // Observing phase: wait now, before SlotView and UploadTexture: everything stays as without the thread.
      CollectBindings(true);
      if (texture.image.image == VK_NULL_HANDLE) {
        return false;  // neither the thread nor the fallback (already logged): CreateTexture, with its emergency path
      }
      if (++bindings_checked_ >= kBindingsACheck && bindings_phase_ == kBindingsWatching) {
        bindings_phase_ = kBindingsApplying;
        uint64_t ns_thread = 0;
        bool priority_ok = true;
        {
          std::lock_guard<std::mutex> lock(bindings_mutex_);
          ns_thread = ns_bindings_thread_;
          priority_ok = bindings_priority_ok_;
        }
        REXLOG_INFO("[native] C3: texture binding thread: {} bindings with an immediate wait and none failed "
                    "({:.0f} us on average inside vkBindImageMemory; the ring waited {:.0f} us on average per "
                    "texture). Moving to the APPLYING phase: the wait goes before each submission{}",
                    bindings_checked_, double(ns_thread) / 1e3 / double(bindings_checked_),
                    double(ns_wait_bindings_total_) / 1e3 / double(bindings_checked_),
                    priority_ok ? "" : " (the kernel did NOT accept the thread priority)");
      }
    }
    return true;
  }

  // SlotView for an image with its bind in flight: the slot now, the view in CollectBindings.
  uint32_t SlotViewInFlight(uint64_t key, VkImage image, VkFormat format, uint32_t swizzle,
                              uint16_t swizzle_host, uint32_t heap) {
    ViewEntry view;
    view.image = image;
    view.heap = heap;
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      ++heap_full_;
      Notify(35, "texture heap full");
      return 0;
    }
    views_.emplace(key, view);  // view.view stays VK_NULL_HANDLE until CollectBindings
    views_by_image_[image].push_back(key);
    ViewInFlight deferred;
    deferred.key = key;
    deferred.image = image;
    deferred.format = format;
    deferred.swizzle = swizzle;
    deferred.swizzle_host = swizzle_host;
    deferred.heap = heap;
    deferred.slot = view.slot;
    views_in_flight_.push_back(deferred);
    ++views_deferred_;
    ++views_deferred_report_;
    return view.slot;
  }

  // UploadTexture for a texture with its bind in flight: the data is already in the upload buffer; record
  // where.
  bool DeferCopy(Texture& texture, VkDeviceSize offset, VkCommandBuffer upload) {
    const size_t index = size_t(texture.in_flight) - 1;
    if (index >= in_flight_.size() || in_flight_[index].texture != &texture || in_flight_[index].copy) {
      TurnOffBindings(fmt::format("DIFFERENCE: incoherent in-flight texture index ({} of {})", index,
                                in_flight_.size()));
      return false;
    }
    TextureInFlight& flight = in_flight_[index];
    flight.copy = true;
    flight.copy_offset = offset;
    flight.copy_epoch = epoch_upload_;
    flight.copy_commands = upload;
    ++copies_deferred_;
    ++copies_deferred_report_;
    return true;
  }

  // The deferred views of an image that had to be recreated: new key (it goes with the image) and its list.
  void MoveViewsInFlight(VkImage old, VkImage new_entry) {
    if (old == new_entry) {
      return;
    }
    std::vector<uint64_t> keys;
    for (ViewInFlight& deferred : views_in_flight_) {
      if (deferred.image != old) {
        continue;
      }
      const auto it = views_.find(deferred.key);
      if (it == views_.end()) {
        continue;
      }
      ViewEntry view = it->second;
      views_.erase(it);
      view.image = new_entry;
      deferred.image = new_entry;
      if (new_entry != VK_NULL_HANDLE) {
        deferred.key = KeyView(new_entry, deferred.swizzle, deferred.swizzle_host, deferred.heap);
      }
      if (!views_.emplace(deferred.key, view).second) {
        deferred.key = 0;  // key collision: CollectBindings puts the empty one in its slot
        continue;
      }
      keys.push_back(deferred.key);
    }
    views_by_image_.erase(old);  // an image in flight only has deferred views
    if (new_entry != VK_NULL_HANDLE && !keys.empty()) {
      auto& list = views_by_image_[new_entry];
      list.insert(list.end(), keys.begin(), keys.end());
    }
  }

  /*
   * Finishes everything the bind thread has. Ring thread only. `with_copies` = false outside BeforeOfSend
   * (the upload buffer would no longer be the one holding that data): those textures are uploaded again at
   * their next check.
   */
  void CollectBindings(bool with_copies) {
    // No re-entry: if something in here ended up starting new work (UseSlot), that call does nothing and
    // the outer one sees the upload buffer changed (DIFFERENCE and a full upload at the next check).
    if (in_flight_.empty() || collecting_bindings_) {
      return;
    }
    collecting_bindings_ = true;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t wait_before = ns_wait_bindings_total_;
    const uint64_t target_2 = in_flight_.back().ticket + 1;
    WaitBindings(target_2);
    if (bindings_stuck_) {
      bindings_stuck_ = false;
      TurnOffBindings("DIFFERENCE: a wait of more than 2 s for the thread (see the lines above)");
    }
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      for (TextureInFlight& flight : in_flight_) {
        flight.result = queue_bindings_[flight.ticket & (kQueueBindings - 1)].result;
      }
      bindings_collected_ = target_2;  // from here on its slots can be reused
    }
    // 1. Results, in order. If a bind failed, that image is dropped (no command references it yet) and the
    //    texture is created again through CreateTexture without the emergency path: releasing half the cache
    //    submits the work, and we are before that.
    for (TextureInFlight& flight : in_flight_) {
      Texture& texture = *flight.texture;
      images_in_flight_.erase(flight.image);
      texture.in_flight = 0;
      if (flight.result == VK_SUCCESS) {
        ++bindings_thread_total_;
        ++bindings_thread_report_;
        fh1::waits::g_textures_bound_thread.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++bindings_failed_;
      TurnOffBindings(fmt::format("DIFFERENCE: vkBindImageMemory returned {} on the thread for a {}x{} texture ({} "
                                  "levels, {} layers)",
                                int32_t(flight.result), flight.width, flight.height, flight.levels, flight.layers));
      dfn_.vkDestroyImage(device_, flight.image, nullptr);
      pool_textures_.Free(texture.image.pool_block);
      texture.image.image = VK_NULL_HANDLE;
      texture.image.pool_block = 0xFFFFFFFFu;
      if (!CreateTexture(texture.image, flight.format, flight.width, flight.height, flight.layers, flight.background,
                        flight.levels, false)) {
        // Not even then. The texture is left without an image: its draws in this submission see the empty one
        // (the same as when it cannot be created) and the next time it is used it is created in full, with the
        // emergency path.
        texture.image = ImageNative{};
        bytes_textures_ -= std::min(bytes_textures_, texture.bytes);
        texture.bytes = 0;
      }
      MoveViewsInFlight(flight.image, texture.image.image);
    }
    // 2. Deferred views: created and written to their slot. If that is not possible, the slot keeps the
    //    empty one (what a draw sees when SlotView returns 0) and is not reused: this submission
    //    references it (it is counted).
    for (const ViewInFlight& deferred : views_in_flight_) {
      const auto it = deferred.key ? views_.find(deferred.key) : views_.end();
      if (it != views_.end() && it->second.image != VK_NULL_HANDLE &&
          CreateViewTexture(it->second.image, deferred.format, deferred.swizzle, deferred.swizzle_host,
                            deferred.heap, it->second.view) == VK_SUCCESS) {
        WriteImage(deferred.heap, deferred.slot, it->second.view);
        continue;
      }
      WriteImage(deferred.heap, deferred.slot, empty_[deferred.heap].view);
      if (it != views_.end()) {
        views_.erase(it);  // the image has no views_by_image_ left (MoveViewsInFlight) or is rebuilt when used
      }
      ++slots_lost_;
      Notify(34, "could not create the view of a texture");
    }
    views_in_flight_.clear();
    // 3. Deferred barriers and copies, in the upload buffer where UploadTexture left their data.
    VkCommandBuffer upload = VK_NULL_HANDLE;
    for (const TextureInFlight& flight : in_flight_) {
      if (!flight.copy) {
        continue;
      }
      Texture& texture = *flight.texture;
      if (texture.image.image == VK_NULL_HANDLE) {
        continue;  // no image: created and fully uploaded next time it is used
      }
      if (with_copies && upload == VK_NULL_HANDLE) {
        upload = context_->CommandsUpload();
      }
      if (!with_copies || upload == VK_NULL_HANDLE || upload != flight.copy_commands ||
          flight.copy_epoch != epoch_upload_) {
        TurnOffBindings(fmt::format("DIFFERENCE: a deferred copy can no longer go in its upload buffer (epoch {} "
                                    "against {}, {})",
                                  flight.copy_epoch, epoch_upload_,
                                  with_copies ? "another upload buffer" : "collected when switching buffers"));
        texture.image.prepared = false;  // the next check uploads it again in full, with its barrier
        texture.next = 0;
        continue;
      }
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, flight.copy_offset);
    }
    in_flight_.clear();
    collecting_bindings_ = false;
    if (!measuring_creation_) {  // inside PrepareTexture its stopwatch already counts it
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
      const uint64_t wait = ns_wait_bindings_total_ - wait_before;
      const uint64_t own = ns > wait ? ns - wait : 0;
      fh1::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
      ns_create_report_ += own;
    }
  }

  // UseSlot: a texture with its bind in flight may reach new work (it was prepared before that work's
  // first Record), but not with its data already in the previous upload buffer: that should never happen.
  void CheckBindingsToChangeOfBuffer() {
    for (const TextureInFlight& flight : in_flight_) {
      if (flight.copy) {
        CollectBindings(false);  // logs the DIFFERENCE, switches off and leaves those textures to be uploaded again
        return;
      }
    }
  }

  // Every 10 s, if there were new textures: what the thread did and how long the ring waited. This is also
  // where it is decided whether it pays off: if for 3 reports in a row the ring waits for the thread longer
  // than the thread takes to bind, the ring would do better doing it itself, and it switches off.
  void ReportBindings() {
    if (bindings_phase_ == kBindingsWithoutDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_bindings_ < std::chrono::seconds(10)) {
      return;
    }
    report_bindings_ = now;
    uint64_t ns_thread = 0;
    uint64_t worst_thread = 0;
    uint64_t queue_full = 0;
    bool priority_ok = true;
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      ns_thread = ns_bindings_thread_ - ns_bindings_thread_previous_;
      ns_bindings_thread_previous_ = ns_bindings_thread_;
      worst_thread = ns_binding_worst_;
      ns_binding_worst_ = 0;
      queue_full = bindings_queue_full_ - queue_full_previous_;
      queue_full_previous_ = bindings_queue_full_;
      priority_ok = bindings_priority_ok_;
    }
    const uint64_t created = textures_created_ - created_report_previous_;
    created_report_previous_ = textures_created_;
    if (created || bindings_thread_report_ || waits_bindings_report_) {
      static constexpr const char* kPhases[] = {"off", "WATCHING", "APPLYING"};
      FH1_REPORT_RING(
          "[native] C3 new textures (build 184), last 10 s: {} created, {} with the binding on the thread; the "
          "ring spent {:.1f} ms creating and waited {:.1f} ms for the thread {} times (worst {:.2f} ms); the "
          "thread, {:.1f} ms inside vkBindImageMemory (average {:.0f} us, worst {:.2f} ms); {} views and {} copies "
          "deferred; {} by the usual path with the queue full; phase {}{}",
          created, bindings_thread_report_, double(ns_create_report_) / 1e6, double(ns_wait_bindings_report_) / 1e6,
          waits_bindings_report_, double(ns_wait_bindings_worst_) / 1e6, double(ns_thread) / 1e6,
          bindings_thread_report_ ? double(ns_thread) / 1e3 / double(bindings_thread_report_) : 0.0,
          double(worst_thread) / 1e6, views_deferred_report_, copies_deferred_report_, queue_full,
          kPhases[std::clamp<int32_t>(bindings_phase_, 0, 2)], priority_ok ? "" : " (the kernel did NOT accept the "
                                                                                  "priority)");
    }
    if (bindings_phase_ == kBindingsApplying && bindings_thread_report_ >= 16) {
      reports_without_compensate_ = ns_wait_bindings_report_ > ns_thread ? reports_without_compensate_ + 1 : 0;
      if (reports_without_compensate_ >= 3) {
        TurnOffBindings(fmt::format("not worth it: in 3 reports in a row the ring waited for the thread longer "
                                    "than the thread took to bind (the last one, {:.1f} ms of waiting for {:.1f} "
                                    "ms of binding)",
                                  double(ns_wait_bindings_report_) / 1e6, double(ns_thread) / 1e6));
      }
    }
    bindings_thread_report_ = 0;
    waits_bindings_report_ = 0;
    ns_wait_bindings_report_ = 0;
    ns_wait_bindings_worst_ = 0;
    ns_create_report_ = 0;
    views_deferred_report_ = 0;
    copies_deferred_report_ = 0;
  }

  // --- Hashing of new textures on a separate thread (fh1_native_texture_fingerprint_thread) ------------------------
  /*
   * Why. During zone-change stutters the ring spends 5.3-8.7 ms of the frame in "textures": the XXH3 of
   * guest memory (2.3-3.1 ms, with some rechecks included) and the untiling and byte swapping of the new
   * ones (3.0-5.6 ms, 1.8-3.1 ms per MB); and outside that figure, the copy to the upload buffer (~0.4 ms
   * per MB). The ring is the bottleneck (the GPU waits 5-11 ms per frame).
   *
   * Exact. The hash and the untiling read guest memory, which the game may reuse as soon as the ring
   * returns the read pointer or writes a fence. So the thread is never given guest addresses: the ring
   * copies, right there (where the inline path computes the hash), the bytes the hash covers (base and
   * mips, the same ranges) into a snapshot, and the thread works on that copy. They are the same bytes read
   * at the same point, and the hash, the untiling and the byte swap are functions of those bytes: the
   * results are identical. Every read of the plan falls inside the copy (PlanReadFingerprint; the bound was
   * tested against both untiling paths on 145,500 rectangles). The thread writes the result into the space
   * UploadTexture reserves in the upload buffer (same size, same order), and the barrier and the copy are
   * recorded where they always are: the GPU does not read that buffer until vkQueueSubmit, and
   * BeforeOfSend (CollectFingerprints) waits before that. Same submission, same bytes: no placeholder textures
   * and no frame of delay. The texture's fingerprint_raw is set when collected, before anything looks at it.
   *
   * The ring never waits for more than one job. When collecting, it takes the ones the thread has not
   * started (from the end of the queue) and does them itself; it only waits for the one the thread is in
   * the middle of. If the thread has no core, the ring ends up doing the usual work at submission, plus the
   * copy, and the "not worth it" guard switches it off.
   *
   * Threads (see nfsc-nx docs/nfsmw-nx/platform-notes.md, Threads). All shared state is under fingerprints_mutex_, and every
   * decision to sleep or wake is taken with the lock held: no "store mine and read yours" with atomics.
   * The thread only reads the snapshot and the plan of its job (written before publishing it) and only
   * writes its buffers, its space in the upload buffer and its job's results; it does not write to the log.
   * Persistent thread (never detached), joined in the destructor before the upload buffer is released.
   * Priority 0x2E: the ring (0x2D) preempts it as soon as it has work (equal priorities do not time-slice)
   * and the guest (0x3B) does not starve it of a core; it also prefers a core other than the ring's, which
   * is busy.
   *
   * Self-checking guard. Observing phase: the first kFingerprintsACheck new textures go through the usual
   * path (that is what gets uploaded) and also through the thread, which returns its raw hash and the XXH3
   * of its data; they are compared when collected. All equal: applying phase, and 1 in
   * kCheckFingerprintOneOfEvery keeps being compared. A DIFFERENCE, a wait of more than 2 s, a job that does
   * not add up, or no payoff for 3 reports in a row: REXLOG_ERROR and off for the session. After a
   * DIFFERENCE, the thread's remaining textures are uploaded again in full. The worst case is the usual
   * path.
   */
  struct ReadFingerprint {  // one ReadLevel call, reading from the snapshot
    uint64_t source = 0;  // offset in snapshots_
    uint64_t row_bytes = 0;
    size_t target = 0;  // offset in the texture data
    uint32_t pitch_blocks = 0;
    uint32_t ox = 0;
    uint32_t oy = 0;
    uint32_t bx = 0;
    uint32_t by = 0;
    uint32_t bx_host = 0;
    bool tile = false;
    bool should_check = false;  // the fast untiling guard repeats this level on the usual path
  };
  struct WorkFingerprint {
    // Written by the ring before publishing; whoever does the job (the thread or the ring) only reads it.
    Texture* texture = nullptr;  // used by the ring only; in fingerprints_planned_, nullptr = already published or discarded
    uint64_t key = 0;
    uint64_t sequence = 0;
    uint64_t inst_base = 0;  // the copy of the base (all layers) in snapshots_
    uint64_t bytes_base = 0;
    uint64_t inst_mips = 0;  // the copy of the mips
    uint64_t bytes_mips = 0;
    size_t bytes_data = 0;
    uint8_t* target = nullptr;  // its space in the upload buffer; nullptr for comparison jobs
    uint64_t epoch = 0;          // epoch_upload_ of that buffer
    uint32_t read_start = 0;  // su plan: reads_fingerprint_[read_start, read_start + reads)
    uint32_t reads = 0;
    uint32_t bytes_block = 1;
    uint32_t log2_block = 0;
    uint32_t unit_order = 0;
    uint32_t order = 0;
    uint32_t address = 0;  // for the log
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool fast = false;           // tile_fast_ when planning
    bool check_order = false;  // the fast byte swap guard checks this texture
    bool compare = false;         // observing phase (or 1 in kCheckFingerprintOneOfEvery): only returns its hashes
    bool warning_levels = false;    // collected without differences: the "levels checked ... all equal" line
    bool warning_commands = false;
    // Written by whoever does the job; the ring reads it under fingerprints_mutex_ once it is done.
    int32_t state = 0;
    bool by_ring = false;
    bool order_different = false;
    uint32_t levels_different = 0;
    std::array<uint32_t, 5> different{};  // first differing level: blocks in x and y, pitch and origin (x, y)
    uint64_t fingerprint_raw = 0;
    uint64_t fingerprint_data = 0;  // comparison jobs only
    uint64_t ns_fingerprint = 0;
    uint64_t ns_rest = 0;
  };
  struct ComparisonFingerprint {  // ring only: what the usual path produced for a comparison texture
    uint64_t raw_value = 0;
    uint64_t data = 0;
    bool is_noted = false;
  };
  static constexpr size_t kReadsByTexture = 6 * 16;  // layers x mip levels
  static constexpr uint32_t kReadsFingerprint = 8192;       // reads planned between two collections
  static constexpr uint64_t kQueueFingerprints = 256;           // power of 2
  static constexpr uint32_t kWorkFingerprintPublished = 0xFFFFFFFFu;  // in Texture::fingerprint_work
  static constexpr int32_t kWorkPending = 1;
  static constexpr int32_t kWorkInThread = 2;
  static constexpr int32_t kWorkInRing = 3;
  static constexpr int32_t kWorkDone = 4;
  static constexpr int32_t kFingerprintsWithoutDecide = -1;
  static constexpr int32_t kFingerprintsOff = 0;
  static constexpr int32_t kFingerprintsWatching = 1;
  static constexpr int32_t kFingerprintsApplying = 2;
  static constexpr uint64_t kFingerprintsACheck = 64;
  static constexpr uint64_t kCheckFingerprintOneOfEvery = 128;

  void DecideFingerprints() {
    const bool requested = REXCVAR_GET(fh1_native_texture_fingerprint_thread);
    fingerprints_priority_ = std::clamp<int32_t>(REXCVAR_GET(fh1_native_texture_fingerprint_thread_priority), 0x2C, 0x3B);
    fingerprints_core_requested_ = std::clamp<int32_t>(REXCVAR_GET(fh1_native_texture_fingerprint_thread_core), -2, 2);
    snapshots_bytes_ = uint64_t(std::clamp<int32_t>(REXCVAR_GET(fh1_native_texture_fingerprint_thread_mb), 4, 64))
                          << 20;
    fingerprints_phase_ = requested ? kFingerprintsWatching : kFingerprintsOff;
    REXLOG_INFO("[native] C3: fingerprint and untiling of new textures on a separate thread "
                "(fh1_native_texture_fingerprint_thread) = {}",
                requested ? fmt::format("YES, priority {:#x}, {} MB of snapshots; WATCHING phase: the first {} "
                                        "also go through the usual path and are compared",
                                     fingerprints_priority_, snapshots_bytes_ >> 20, kFingerprintsACheck)
                       : std::string("no, on the ring thread as always"));
  }

  void TurnOffFingerprints(const std::string& reason) {
    if (fingerprints_phase_ == kFingerprintsOff) {
      return;
    }
    fingerprints_phase_ = kFingerprintsOff;
    REXLOG_ERROR("[native] C3: texture fingerprint thread OFF for the rest of the session: {}. New textures are "
                 "prepared whole on the ring thread again ({} done by the thread, {} by the ring, {} compared)",
                 reason, fingerprints_thread_total_, fingerprints_ring_total_, fingerprints_compared_);
  }

  /*
   * One ReadLevel call turned into a read from the snapshot. false = the texture goes through the usual
   * path: ReadLevel would go outside memory (its own check) or would read outside what the hash covers.
   * `range_*` is the hash region (base or mips) and `in_copy` is where its copy starts in the snapshot.
   * The bound on what is read is the hash's (GetTiledAddressUpperBound2D): the corner of the last
   * 32x32-block tile plus the size of one tile; it was checked against what UntileLevel and
   * ReadLevelTileOfAlways read.
   */
  static bool PlanReadFingerprint(std::array<ReadFingerprint, kReadsByTexture>& reads, uint32_t& n,
                                   uint64_t address, bool tile, uint32_t pitch_blocks, uint64_t row_bytes,
                                   const FormatTexture& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                                   uint32_t bx_host, size_t target, uint64_t range_start, uint64_t range_bytes,
                                   uint64_t in_copy) {
    if (!bx || !by) {
      return true;  // ReadLevel reads nothing
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t loose =
        tile ? uint64_t(std::max<int64_t>(OffsetTile2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u), pitch_blocks,
                                                                     log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + loose > kMemoryPhysical) {
      return false;  // ReadLevel would return false: let the usual path report it
    }
    const uint64_t read =
        tile ? uint64_t(std::max<int64_t>(OffsetTile2D(int32_t((ox + bx - 1) & ~31u),
                                                                     int32_t((oy + by - 1) & ~31u), pitch_blocks,
                                                                     log2),
                                             0)) +
                      (log2 == 0 ? 0xA00u : log2 == 1 ? 0xC00u : (0x400u << log2))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (n >= reads.size() || address < range_start || address + read > range_start + range_bytes) {
      return false;
    }
    ReadFingerprint& l = reads[n++];
    l.source = in_copy + (address - range_start);
    l.row_bytes = row_bytes;
    l.target = target;
    l.pitch_blocks = pitch_blocks;
    l.ox = ox;
    l.oy = oy;
    l.bx = bx;
    l.by = by;
    l.bx_host = bx_host;
    l.tile = tile;
    l.should_check = false;
    return true;
  }

  /*
   * The rest of a new texture for the thread: space, snapshot, plan and job. true = applying phase: the
   * thread prepares it and the tail of PrepareTexture is done here (interval, upload and counters); the
   * caller returns. false = the usual path continues: it was not possible, or it is a comparison texture
   * (observing phase, and 1 in kCheckFingerprintOneOfEvery), whose job is already published and is compared
   * when collected.
   */
  bool PlanFingerprint(Texture& texture, uint64_t key, uint32_t n_reads, const uint8_t* raw_base,
                     uint64_t extension, const uint8_t* raw_mips, uint64_t extension_mips, const FormatTexture& tf,
                     uint32_t order, size_t bytes_data, const std::array<size_t, 16>& offsets,
                     uint32_t address, uint32_t width, uint32_t height, uint32_t format, VkDeviceSize& bytes_upload) {
    comparison_texture_ = nullptr;
    if (fingerprints_phase_ == kFingerprintsWithoutDecide) {
      DecideFingerprints();
    }
    if (fingerprints_phase_ == kFingerprintsOff || texture.fingerprint_work != 0 ||
        (fingerprints_phase_ == kFingerprintsWatching &&
         fingerprints_comparisons_planned_ >= kFingerprintsACheck + fingerprints_without_compare_)) {
      return false;  // while observing, with comparisons already running, the rest only take the usual path
    }
    if (!snapshots_) {
      snapshots_.reset(new (std::nothrow) uint8_t[size_t(snapshots_bytes_)]);
      reads_fingerprint_.reset(new (std::nothrow) ReadFingerprint[kReadsFingerprint]);
      if (!snapshots_ || !reads_fingerprint_) {
        snapshots_.reset();
        reads_fingerprint_.reset();
        TurnOffFingerprints(fmt::format("DIFFERENCE: no memory for {} MB of snapshots", snapshots_bytes_ >> 20));
        return false;
      }
    }
    const uint64_t inst_base = (snapshots_used_ + 63) & ~uint64_t(63);
    const uint64_t inst_mips = inst_base + ((extension + 63) & ~uint64_t(63));  // the same space the plan assumes
    const uint64_t in_queue = fingerprints_published_ - fingerprints_collected_ + fingerprints_planned_pending_;
    if (in_queue >= kQueueFingerprints || reads_used_ + n_reads > kReadsFingerprint ||
        inst_mips + extension_mips > snapshots_bytes_) {
      ++fingerprints_without_room_report_;
      return false;
    }
    const bool compare =
        fingerprints_phase_ == kFingerprintsWatching || (++fingerprints_new_applying_ % kCheckFingerprintOneOfEvery) == 0;
    // The snapshot: the same bytes the usual path's hash covers, at the same point.
    const auto before_copy = std::chrono::steady_clock::now();
    std::memcpy(snapshots_.get() + inst_base, raw_base, size_t(extension));
    if (extension_mips) {
      std::memcpy(snapshots_.get() + inst_mips, raw_mips, size_t(extension_mips));
    }
    const uint64_t ns_copy = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before_copy)
                                           .count());
    snapshots_used_ = inst_mips + extension_mips;
    ns_snapshots_report_ += ns_copy;
    bytes_snapshots_report_ += extension + extension_mips;
    fh1::waits::g_ns_fingerprint_snapshot.fetch_add(ns_copy, std::memory_order_relaxed);
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    WorkFingerprint t;
    t.texture = &texture;
    t.key = key;
    t.inst_base = inst_base;
    t.bytes_base = extension;
    t.inst_mips = inst_mips;
    t.bytes_mips = extension_mips;
    t.bytes_data = bytes_data;
    t.read_start = reads_used_;
    t.reads = n_reads;
    t.bytes_block = tf.bytes;
    t.log2_block = log2;
    t.unit_order = tf.unit_order;
    t.order = order;
    t.address = address;
    t.width = width;
    t.height = height;
    t.format = format;
    t.fast = tile_fast_ > 0;
    t.compare = compare;
    // The plan, with the decisions of the fast untiling and byte swap guards taken in the same order as the
    // usual path (levels_seen_ and commands_seen_). Not for a comparison texture: the usual path also
    // processes it, and that is what keeps those counts.
    for (uint32_t i = 0; i < n_reads; ++i) {
      ReadFingerprint l = reads_plan_[i];
      l.source += inst_base;
      if (!compare && l.tile && t.fast && tf.bytes == (1u << log2)) {
        ++levels_seen_;
        l.should_check = levels_seen_ <= kLevelsACheck || levels_seen_ % kCheckOneOfEvery == 0;
        if (l.should_check && ++levels_checked_ == kLevelsACheck) {
          t.warning_levels = true;
        }
      }
      reads_fingerprint_[reads_used_ + i] = l;
    }
    reads_used_ += n_reads;
    if (!compare && t.fast && order != 0 && (tf.unit_order == 2 || tf.unit_order == 4)) {
      ++commands_seen_;
      t.check_order = commands_seen_ <= kLevelsACheck || commands_seen_ % kCheckOneOfEvery == 0;
      if (t.check_order && ++commands_checked_ == kLevelsACheck) {
        t.warning_commands = true;
      }
    }
    ++fingerprints_planned_report_;
    if (compare) {
      // Published now, with no space in the upload buffer: the thread only returns its two hashes. What gets
      // uploaded is the usual path's result, which continues on return (NoteComparisonFingerprint stores its
      // hashes).
      ++fingerprints_comparisons_planned_;
      ++fingerprints_comparison_report_;
      const uint64_t sequence = PublishWorkFingerprint(t, nullptr);
      comparisons_fingerprint_[sequence & (kQueueFingerprints - 1)] = ComparisonFingerprint{};
      comparison_texture_ = &texture;
      comparison_sequence_ = sequence;
      return false;
    }
    // Applying phase: the tail of PrepareTexture for a new texture, the same as there.
    fingerprints_planned_.push_back(t);
    ++fingerprints_planned_pending_;
    texture.fingerprint_work = uint32_t(fingerprints_planned_.size());
    texture.sample_valid = false;
    texture.samples_consecutive = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      texture.offset_level[n] = uint32_t(offsets[n]);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.fingerprint = 0;  // a new texture's data hash decides nothing: 0, as on the usual path
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    fh1::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    fh1::waits::g_bytes_uploaded.fetch_add(bytes_data, std::memory_order_relaxed);
    bytes_upload += (bytes_data + 3) & ~size_t(3);
    return true;
  }

  // Queues a job for the thread (under the lock), waking it if it sleeps. Returns its sequence number. If
  // the thread cannot be created, the job stays queued anyway and the ring does it when collecting (and the
  // feature switches off).
  uint64_t PublishWorkFingerprint(const WorkFingerprint& work, uint8_t* target) {
    if (!fingerprints_thread_.joinable() && !fingerprints_without_thread_) {
#if REX_PLATFORM_SWITCH
      const int ring = RexSwitchCurrentCore();  // the ring thread asks for itself
#else
      const int ring = -1;
#endif
      fingerprints_core_ = fingerprints_core_requested_ != -2 ? fingerprints_core_requested_ : ring == 2 ? 1 : 2;
      try {
        fingerprints_thread_ = std::thread([this] { LoopFingerprints(); });
        REXLOG_INFO("[native] C3: texture fingerprint thread created: preferred core {} (the ring runs on {}), "
                    "priority {:#x}",
                    fingerprints_core_, ring, fingerprints_priority_);
      } catch (const std::system_error& error) {
        fingerprints_without_thread_ = true;
        TurnOffFingerprints(fmt::format("DIFFERENCE: the thread could not be created ({})", error.what()));
      }
    }
    bool notify = false;
    uint64_t sequence = 0;
    {
      std::lock_guard<std::mutex> lock(fingerprints_mutex_);
      sequence = fingerprints_published_;
      WorkFingerprint& slot_2 = queue_fingerprints_[sequence & (kQueueFingerprints - 1)];
      slot_2 = work;
      slot_2.sequence = sequence;
      slot_2.target = target;
      slot_2.epoch = epoch_upload_;
      slot_2.state = kWorkPending;
      ++fingerprints_published_;
      notify = fingerprints_sleeping_;  // decided under the lock: if it sleeps, it is in its wait with the predicate
    }
    if (notify) {
      fingerprints_cv_.notify_one();
    }
    return sequence;
  }

  // The data bytes of a planned texture (UploadTexture reserves that space). If its index does not match
  // (should not happen), the texture's own, which come from the same per-level computation.
  size_t BytesFingerprintPlanned(const Texture& texture) const {
    const size_t index = size_t(texture.fingerprint_work) - 1;
    return index < fingerprints_planned_.size() && fingerprints_planned_[index].texture == &texture
               ? fingerprints_planned_[index].bytes_data
               : size_t(texture.bytes);
  }

  // UploadTexture for a planned texture: its job, with the space just reserved for it, goes to the thread's
  // queue.
  void PublishFingerprint(Texture& texture, uint8_t* target, size_t bytes) {
    const size_t index = size_t(texture.fingerprint_work) - 1;
    if (index >= fingerprints_planned_.size() || fingerprints_planned_[index].texture != &texture) {
      // Should never happen. Without a job its data would never arrive: zeros in its space, and it is uploaded
      // in full next time.
      std::memset(target, 0, bytes);
      texture.fingerprint_work = 0;
      texture.fingerprint_raw = 0;
      texture.next = 0;
      TurnOffFingerprints(fmt::format("DIFFERENCE: incoherent planned job ({} of {})", index,
                                fingerprints_planned_.size()));
      return;
    }
    PublishWorkFingerprint(fingerprints_planned_[index], target);
    fingerprints_planned_[index].texture = nullptr;  // published: no longer counts as planned
    --fingerprints_planned_pending_;
    texture.fingerprint_work = kWorkFingerprintPublished;
  }

  // At the start of each draw: the previous draw's planned textures that never reached UploadTexture (the
  // draw was rejected before). As on the usual path, they stay un-uploaded and are prepared again at their
  // next check.
  void DiscardFingerprintsPlanned() {
    if (!fingerprints_planned_pending_) {
      return;
    }
    for (WorkFingerprint& t : fingerprints_planned_) {
      if (t.texture) {
        t.texture->fingerprint_work = 0;
        t.texture = nullptr;
        ++fingerprints_discarded_;
      }
    }
    fingerprints_planned_pending_ = 0;
  }

  // Observing phase (and 1 in kCheckFingerprintOneOfEvery): the usual path's hashes of the texture whose
  // comparison job was just published. CollectFingerprints compares them.
  void NoteComparisonFingerprint(const Texture& texture, const std::vector<uint8_t>& data) {
    comparison_texture_ = nullptr;
    if (comparison_sequence_ < fingerprints_collected_) {
      return;  // already collected (should not happen): left uncompared and counted
    }
    ComparisonFingerprint& c = comparisons_fingerprint_[comparison_sequence_ & (kQueueFingerprints - 1)];
    c.raw_value = texture.fingerprint_raw;
    c.data = XXH3_64bits(data.data(), data.size());
    c.is_noted = true;
  }

  /*
   * One job: done by the thread (or by the ring, if it takes it when collecting). The same as
   * PrepareTexture's usual path and in the same order (raw hash, levels through ReadLevel, and byte swap
   * with its guard), but reading from the snapshot. It only reads this job's snapshot and plan, and only
   * writes `data`, `check`, its space in the upload buffer and the results in `t`: nothing of the
   * ring's state. `fast_off` belongs to whoever does the job: after a difference in its guard, the
   * rest takes the usual path, as on the ring (which also turns off tile_fast_ when collecting it).
   */
  void ExecuteFingerprint(WorkFingerprint& t, std::vector<uint8_t>& data, std::vector<uint8_t>& check,
                      bool& fast_off) {
    const auto start = std::chrono::steady_clock::now();
    const uint8_t* const snapshot = snapshots_.get();
    uint64_t fingerprint = XXH3_64bits(snapshot + t.inst_base, size_t(t.bytes_base));
    if (t.bytes_mips) {
      fingerprint = XXH3_64bits_withSeed(snapshot + t.inst_mips, size_t(t.bytes_mips), fingerprint);
    }
    t.fingerprint_raw = fingerprint;
    const auto after_fingerprint = std::chrono::steady_clock::now();
    data.assign(t.bytes_data, 0);
    const uint32_t b = t.bytes_block;
    for (uint32_t i = 0; i < t.reads; ++i) {
      const ReadFingerprint& l = reads_fingerprint_[t.read_start + i];
      const uint8_t* const source = snapshot + l.source;
      uint8_t* const target = data.data() + l.target;
      if (!l.tile) {
        for (uint32_t y = 0; y < l.by; ++y) {
          std::memcpy(target + size_t(y) * l.bx_host * b,
                      source + uint64_t(l.oy + y) * l.row_bytes + uint64_t(l.ox) * b, size_t(l.bx) * b);
        }
        continue;
      }
      if (t.fast && !fast_off && b == (1u << t.log2_block)) {
        switch (t.log2_block) {
          case 0: UntileLevel<0>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 1: UntileLevel<1>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 2: UntileLevel<2>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 3: UntileLevel<3>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          default: UntileLevel<4>(source, l.pitch_blocks, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
        }
        if (l.should_check) {
          // The ReadLevel guard: repeated on the usual path (which is the result kept) and compared.
          check.assign(size_t(l.by) * l.bx_host * b, 0);
          for (uint32_t y = 0; y < l.by; ++y) {
            std::memcpy(check.data() + size_t(y) * l.bx_host * b, target + size_t(y) * l.bx_host * b,
                        size_t(l.bx) * b);
          }
          ReadLevelTileOfAlways(source, l.pitch_blocks, t.log2_block, b, l.ox, l.oy, l.bx, l.by, l.bx_host,
                                    target);
          bool equal_2 = true;
          for (uint32_t y = 0; y < l.by && equal_2; ++y) {
            equal_2 = std::memcmp(check.data() + size_t(y) * l.bx_host * b,
                                  target + size_t(y) * l.bx_host * b, size_t(l.bx) * b) == 0;
          }
          if (!equal_2) {
            if (!t.levels_different++) {
              t.different = {l.bx, l.by, l.pitch_blocks, l.ox, l.oy};
            }
            fast_off = true;
          }
        }
        continue;
      }
      ReadLevelTileOfAlways(source, l.pitch_blocks, t.log2_block, b, l.ox, l.oy, l.bx, l.by, l.bx_host, target);
    }
    const auto order = static_cast<xenos::Endian>(t.order);
    if (t.fast && !fast_off && order != xenos::Endian::kNone && (t.unit_order == 2 || t.unit_order == 4)) {
      if (t.check_order) {
        check = data;
      }
      ChangeOrderBytes(data.data(), data.size(), t.unit_order, t.order);
      if (t.check_order) {
        OrderOfAlways(check, t.unit_order, order);
        if (check != data) {
          t.order_different = true;
          data.swap(check);  // the usual one is kept, as on the ring
          fast_off = true;
        }
      }
    } else {
      OrderOfAlways(data, t.unit_order, order);
    }
    if (t.compare) {
      t.fingerprint_data = XXH3_64bits(data.data(), data.size());
    }
    if (t.target) {
      std::memcpy(t.target, data.data(), data.size());
    }
    const auto fin = std::chrono::steady_clock::now();
    t.ns_fingerprint = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_fingerprint - start).count());
    t.ns_rest = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(fin - after_fingerprint).count());
  }

  // The thread: the queued jobs, in order, skipping those the ring has taken. It sleeps on a decision taken
  // under the lock; it never spins. It does not write to the log (the ring records the figures when
  // collecting).
  void LoopFingerprints() {
    rex::thread::set_current_thread_name("FH1 texture fingerprints");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(fingerprints_priority_));
    const bool core_ok = RexSwitchSetCurrentThreadCore(int(fingerprints_core_));
    const int core = RexSwitchCurrentCore();
#else
    const bool priority_ok = true;
    const bool core_ok = true;
    const int core = -1;
#endif
    std::vector<uint8_t> data;         // owned by this thread only
    std::vector<uint8_t> check;  // owned by this thread only
    bool fast_off = false;        // owned by this thread only: its fast-path guard saw a difference
    std::unique_lock<std::mutex> lock(fingerprints_mutex_);
    fingerprints_priority_ok_ = priority_ok;
    fingerprints_core_ok_ = core_ok;
    fingerprints_core_real_ = core;
    for (;;) {
      while (fingerprints_taken_ != fingerprints_published_ &&
             queue_fingerprints_[fingerprints_taken_ & (kQueueFingerprints - 1)].state != kWorkPending) {
        ++fingerprints_taken_;  // taken by the ring
      }
      if (fingerprints_taken_ == fingerprints_published_) {
        if (fingerprints_stop_) {
          return;  // stop, nothing pending
        }
        if (fingerprints_waiting_) {
          fingerprints_done_cv_.notify_one();
        }
        fingerprints_sleeping_ = true;
        fingerprints_cv_.wait(lock, [this] { return fingerprints_stop_ || fingerprints_taken_ != fingerprints_published_; });
        fingerprints_sleeping_ = false;
        continue;
      }
      // The slot belongs to this thread while it is InThread: the ring neither touches nor reuses it until it is
      // collected done.
      WorkFingerprint& t = queue_fingerprints_[fingerprints_taken_ & (kQueueFingerprints - 1)];
      t.state = kWorkInThread;
      ++fingerprints_taken_;
      lock.unlock();
      ExecuteFingerprint(t, data, check, fast_off);
      lock.lock();
      t.state = kWorkDone;
      ++fingerprints_done_;
      if (fingerprints_waiting_) {
        fingerprints_done_cv_.notify_one();
      }
    }
  }

  /*
   * Waits for and collects all published jobs. Ring thread only: in BeforeOfSend (before vkQueueSubmit)
   * and, if needed, before rechecking a texture. It takes the ones the thread has not started (from the
   * end) and does them itself; it only waits for the one the thread is in the middle of, with a timeout and
   * a log line every 2 s. Then it sets each texture's raw hash and compares the comparison jobs. If nothing
   * planned is left, the snapshots start over.
   */
  void CollectFingerprints() {
    if (collecting_fingerprints_) {
      return;
    }
    if (fingerprints_published_ == fingerprints_collected_) {
      if (!fingerprints_planned_pending_ && (snapshots_used_ || reads_used_)) {
        snapshots_used_ = 0;  // nothing published uncollected or planned: everything starts over
        reads_used_ = 0;
        fingerprints_planned_.clear();
      }
      return;
    }
    collecting_fingerprints_ = true;
    uint64_t ns_wait = 0;
    uint64_t ns_ring = 0;
    uint64_t done_ring = 0;
    {
      std::unique_lock<std::mutex> lock(fingerprints_mutex_);
      int seconds = 0;
      while (fingerprints_done_ != fingerprints_published_) {
        // 1. One the thread has not started, from the end of the queue (the thread goes from the start).
        WorkFingerprint* mine = nullptr;
        for (uint64_t i = fingerprints_published_; i > fingerprints_taken_; --i) {
          WorkFingerprint& t = queue_fingerprints_[(i - 1) & (kQueueFingerprints - 1)];
          if (t.state == kWorkPending) {
            t.state = kWorkInRing;
            mine = &t;
            break;
          }
        }
        if (mine) {
          lock.unlock();
          const auto before = std::chrono::steady_clock::now();
          ExecuteFingerprint(*mine, fingerprints_data_ring_, fingerprints_check_ring_, fingerprints_fast_off_ring_);
          ns_ring += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
          ++done_ring;
          lock.lock();
          mine->by_ring = true;
          mine->state = kWorkDone;
          ++fingerprints_done_;
          continue;
        }
        // 2. What is left, the thread has half done: wait for it, deciding under the lock.
        const auto before = std::chrono::steady_clock::now();
        fingerprints_waiting_ = true;
        if (fingerprints_sleeping_) {
          fingerprints_cv_.notify_one();  // should not be needed (it has one in progress); in case a wake-up was lost
        }
        const bool ready = fingerprints_done_cv_.wait_for(lock, std::chrono::seconds(2),
                                                         [this] { return fingerprints_done_ == fingerprints_published_; });
        fingerprints_waiting_ = false;
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before)
                                         .count());
        ns_wait += ns;
        ns_wait_fingerprints_worst_ = std::max(ns_wait_fingerprints_worst_, ns);
        ++waits_fingerprints_report_;
        if (!ready) {
          seconds += 2;
          fingerprints_stuck_ = true;
          REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for the texture fingerprint thread: "
                       "published {}, taken {}, done {}; the thread is {}",
                       seconds, fingerprints_published_, fingerprints_taken_, fingerprints_done_,
                       fingerprints_sleeping_ ? "ASLEEP (lost notification)" : "awake (a job that does not finish)");
        }
      }
      // The results were written by whoever did each job; the lock makes them visible here.
      fingerprints_collected_list_.clear();
      for (uint64_t s = fingerprints_collected_; s < fingerprints_published_; ++s) {
        fingerprints_collected_list_.push_back(queue_fingerprints_[s & (kQueueFingerprints - 1)]);
      }
      fingerprints_collected_ = fingerprints_published_;  // from here on its slots can be reused
    }
    comparison_texture_ = nullptr;
    bool difference = false;
    std::string detail;
    uint64_t thread_value = 0;
    uint64_t ns_thread = 0;
    for (WorkFingerprint& t : fingerprints_collected_list_) {
      if (t.by_ring) {
        ++fingerprints_ring_total_;
      } else {
        ++fingerprints_thread_total_;
        if (!t.compare) {  // what the thread actually took off the ring
          ++thread_value;
          ns_thread += t.ns_fingerprint + t.ns_rest;
          ns_fingerprint_raw_thread_report_ += t.ns_fingerprint;
        }
      }
      // The fast-path guards, with the same lines as the usual path (ReadLevel and the byte swap).
      if (t.levels_different) {
        REXLOG_ERROR("[native] C3: the fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, from "
                     "{},{}). It is turned OFF and the usual one is used.",
                     t.different[0], t.different[1], t.bytes_block, t.different[2], t.different[3], t.different[4]);
        tile_fast_ = 0;
      } else if (t.warning_levels) {
        REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the usual one, all equal. Staying with "
                    "the fast one.",
                    kLevelsACheck);
      }
      if (t.order_different) {
        REXLOG_ERROR("[native] C3: the fast byte swap does NOT match (unit {}, order {}, {} bytes). The fast byte "
                     "swap and untiling are turned OFF.",
                     t.unit_order, t.order, t.bytes_data);
        tile_fast_ = 0;
      } else if (t.warning_commands) {
        REXLOG_INFO("[native] C3: fast byte swap: {} textures checked against the usual one, all equal.",
                    kLevelsACheck);
      }
      if (t.compare) {
        const ComparisonFingerprint& c = comparisons_fingerprint_[t.sequence & (kQueueFingerprints - 1)];
        if (!c.is_noted) {
          ++fingerprints_without_compare_;
          continue;
        }
        ++fingerprints_compared_;
        if (c.raw_value != t.fingerprint_raw || c.data != t.fingerprint_data) {
          difference = true;
          detail = fmt::format("DIFFERENCE in texture {:08X} {}x{} format {}: raw fingerprint {:016X} on the ring "
                               "and {:016X} on the thread; data {:016X} and {:016X}",
                                t.address, t.width, t.height, t.format, c.raw_value, t.fingerprint_raw, c.data,
                                t.fingerprint_data);
        }
        continue;
      }
      // A texture done by the thread: its raw hash is that of the same bytes that were uploaded. It must still
      // be in the cache as it was left, and its data must have gone to the current upload buffer (the one about
      // to be submitted).
      const auto it = textures_.find(t.key);
      if (it == textures_.end() || &it->second != t.texture || t.texture->fingerprint_work != kWorkFingerprintPublished) {
        difference = true;
        detail = fmt::format("DIFFERENCE: texture {:08X} of a collected job is no longer as it was left",
                              t.address);
        t.texture = nullptr;
        continue;
      }
      if (t.epoch != epoch_upload_) {
        difference = true;  // uploaded whole again (fingerprints_suspicious_)
        detail = fmt::format("DIFFERENCE: the data of texture {:08X} went to another upload buffer (epoch {} "
                             "against {})",
                              t.address, t.epoch, epoch_upload_);
      }
      t.texture->fingerprint_raw = t.fingerprint_raw;
      t.texture->fingerprint_work = 0;
      NoteContentTexture(*t.texture, t.key);  // measurement only: fh1_native_diag_reuse
    }
    if (difference) {
      fingerprints_suspicious_ = true;
      TurnOffFingerprints(detail);
    }
    if (fingerprints_stuck_) {
      fingerprints_stuck_ = false;
      TurnOffFingerprints("DIFFERENCE: a wait of more than 2 s for the fingerprint thread (see the lines above)");
    }
    if (fingerprints_suspicious_) {
      // The thread's textures planned before the DIFFERENCE may carry the same fault: they are uploaded again
      // in full, on the usual path (already switched off), at their next check.
      for (const WorkFingerprint& t : fingerprints_collected_list_) {
        if (!t.compare && t.texture) {
          t.texture->image.prepared = false;
          t.texture->next = 0;
          t.texture->fingerprint_work = 0;
          ++fingerprints_reuploads_;
        }
      }
    } else if (fingerprints_phase_ == kFingerprintsWatching && fingerprints_compared_ >= kFingerprintsACheck) {
      fingerprints_phase_ = kFingerprintsApplying;
      bool priority_ok = true;
      bool core_ok = true;
      int core = -1;
      {
        std::lock_guard<std::mutex> lock(fingerprints_mutex_);
        priority_ok = fingerprints_priority_ok_;
        core_ok = fingerprints_core_ok_;
        core = fingerprints_core_real_;
      }
      REXLOG_INFO("[native] C3: texture fingerprint thread: {} new textures done by both paths, all equal (raw "
                  "fingerprint and data; {} could not be compared). Moving to the APPLYING phase: the thread "
                  "prepares them and 1 in {} is still compared. The thread runs on core {}{}{}",
                  fingerprints_compared_, fingerprints_without_compare_, kCheckFingerprintOneOfEvery, core,
                  core_ok ? "" : " (the kernel did NOT accept the preferred core)",
                  priority_ok ? "" : " (the kernel did NOT accept the priority)");
    }
    fingerprints_thread_report_ += thread_value;
    fingerprints_ring_report_ += done_ring;
    ns_fingerprints_thread_report_ += ns_thread;
    ns_fingerprints_ring_report_ += ns_ring;
    ns_wait_fingerprints_report_ += ns_wait;
    // A few times per frame (on each submission with new textures): the fetch_add calls do not show.
    fh1::waits::g_textures_fingerprint_thread.fetch_add(thread_value, std::memory_order_relaxed);
    fh1::waits::g_ns_fingerprint_thread.fetch_add(ns_thread, std::memory_order_relaxed);
    fh1::waits::g_textures_fingerprint_ring.fetch_add(done_ring, std::memory_order_relaxed);
    fh1::waits::g_ns_fingerprint_ring.fetch_add(ns_ring, std::memory_order_relaxed);
    fh1::waits::g_ns_waiting_fingerprints.fetch_add(ns_wait, std::memory_order_relaxed);
    if (!fingerprints_planned_pending_) {
      snapshots_used_ = 0;
      reads_used_ = 0;
      fingerprints_planned_.clear();
    }
    collecting_fingerprints_ = false;
  }

  // UseSlot: everything published is collected in BeforeOfSend. If anything reaches here (it should
  // not), its data was going to the buffer just closed without waiting for it: it is collected now (before
  // this one is reset) and those textures are uploaded again.
  void CheckFingerprintsToChangeOfBuffer() {
    if (fingerprints_published_ == fingerprints_collected_) {
      return;
    }
    const uint64_t without_collect = fingerprints_published_ - fingerprints_collected_;
    fingerprints_suspicious_ = true;  // whatever is collected now is uploaded again in full
    CollectFingerprints();
    TurnOffFingerprints(fmt::format("DIFFERENCE: {} fingerprint-thread jobs not collected when switching upload "
                                    "buffers",
                              without_collect));
  }

  void StopFingerprints() {
    if (!fingerprints_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(fingerprints_mutex_);
      fingerprints_stop_ = true;
    }
    fingerprints_cv_.notify_one();
    fingerprints_thread_.join();  // first finishes the published jobs
    REXLOG_INFO("[native] C3: texture fingerprint thread stopped ({} textures done by the thread, {} by the ring, "
                "{} compared, {} discarded, {} uploaded again)",
                fingerprints_thread_total_, fingerprints_ring_total_, fingerprints_compared_, fingerprints_discarded_,
                fingerprints_reuploads_);
  }

  // Every 10 s, if there were new textures: what the thread and the ring did. This is also where it is
  // decided whether it pays off: if for 3 reports in a row (with 16 or more jobs) what the thread took off
  // the ring is less than what the ring spent copying snapshots and waiting for it, the ring would do better
  // alone, and it switches off.
  void ReportFingerprints() {
    if (fingerprints_phase_ == kFingerprintsWithoutDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_fingerprints_ < std::chrono::seconds(10)) {
      return;
    }
    report_fingerprints_ = now;
    bool priority_ok = true;
    int core = -1;
    {
      std::lock_guard<std::mutex> lock(fingerprints_mutex_);
      priority_ok = fingerprints_priority_ok_;
      core = fingerprints_core_real_;
    }
    if (fingerprints_planned_report_ || fingerprints_thread_report_ || fingerprints_ring_report_ || fingerprints_without_room_report_) {
      static constexpr const char* kPhases[] = {"off", "WATCHING", "APPLYING"};
      FH1_REPORT_RING(
          "[native] C3 new-texture fingerprints (build 185), last 10 s: {} planned ({} for comparison); the thread "
          "prepared {} in {:.1f} ms (fingerprint {:.1f} ms) and the ring {} at submission in {:.1f} ms; the ring "
          "copied {:.1f} MB of snapshots in {:.1f} ms and waited {:.1f} ms for the thread {} times (worst {:.2f} "
          "ms); {} by the usual path without room; {} compared since the start ({} could not be); thread on core "
          "{}, priority {:#x}{}; phase {}",
          fingerprints_planned_report_, fingerprints_comparison_report_, fingerprints_thread_report_,
          double(ns_fingerprints_thread_report_) / 1e6, double(ns_fingerprint_raw_thread_report_) / 1e6,
          fingerprints_ring_report_, double(ns_fingerprints_ring_report_) / 1e6,
          double(bytes_snapshots_report_) / 1048576.0, double(ns_snapshots_report_) / 1e6,
          double(ns_wait_fingerprints_report_) / 1e6, waits_fingerprints_report_, double(ns_wait_fingerprints_worst_) / 1e6,
          fingerprints_without_room_report_, fingerprints_compared_, fingerprints_without_compare_, core, fingerprints_priority_,
          priority_ok ? "" : " (the kernel did NOT accept the priority)", kPhases[std::clamp<int32_t>(fingerprints_phase_, 0, 2)]);
    }
    if (fingerprints_phase_ == kFingerprintsApplying && fingerprints_thread_report_ + fingerprints_ring_report_ >= 16) {
      const bool compensates = ns_fingerprints_thread_report_ > ns_snapshots_report_ + ns_wait_fingerprints_report_;
      reports_without_compensate_fingerprints_ = compensates ? 0 : reports_without_compensate_fingerprints_ + 1;
      if (reports_without_compensate_fingerprints_ >= 3) {
        TurnOffFingerprints(fmt::format("not worth it: in 3 reports in a row the thread took less off the ring "
                                        "than the ring spent copying and waiting for it (the last one: thread "
                                        "{:.1f} ms, copies {:.1f} ms, wait {:.1f} ms)",
                                  double(ns_fingerprints_thread_report_) / 1e6, double(ns_snapshots_report_) / 1e6,
                                  double(ns_wait_fingerprints_report_) / 1e6));
      }
    }
    fingerprints_planned_report_ = 0;
    fingerprints_comparison_report_ = 0;
    fingerprints_thread_report_ = 0;
    fingerprints_ring_report_ = 0;
    ns_fingerprints_thread_report_ = 0;
    ns_fingerprint_raw_thread_report_ = 0;
    ns_fingerprints_ring_report_ = 0;
    ns_snapshots_report_ = 0;
    bytes_snapshots_report_ = 0;
    ns_wait_fingerprints_report_ = 0;
    ns_wait_fingerprints_worst_ = 0;
    waits_fingerprints_report_ = 0;
    fingerprints_without_room_report_ = 0;
  }

  // --- Vertex copies on a separate thread (fh1_native_uploads_thread) ------------------------------------------
  // One producer (the ring thread, in Draw) and one consumer (copies_thread_). The producer writes the job
  // into copies_written_ and publishes it; the thread copies up to there and publishes copies_done_.
  // With the queue full the copy is done on the ring, as before. The destinations are distinct ranges of
  // the upload buffer, so order does not matter. The copies must be waited for before submitting the work
  // (BeforeOfSend) and before returning the read pointer to the game (WaitUploads from
  // fh1_native_system.cpp): after that the game may reuse its vertex buffers.
  //
  // Sleeping and waking are decided under the lock. Two earlier versions hung: the copy thread asleep with
  // 1,123 copies queued and the ring waiting for them. Each side used to decide without the lock whether to
  // wake the other, Dekker-style: the ring published copies_written_ and read copies_done_, and the
  // thread the other way round. With store(..., memory_order_release) MSVC's STL emits a plain mov, and on
  // x86 (TSO) a store may not yet be visible from another core: both sides missed the wake-up. ARM gives no
  // guarantee either. Now copies_sleeping_ and copies_waiting_ are only touched under the lock, and
  // nothing is decided without it: the wake-up arrives at the next point that takes the lock (every
  // kCopiesByWarning copies, when the ring waits, and when the thread runs out of work). There is no atomic
  // hint flag any more; the thread always signals from its no-work path, under the lock, which it goes
  // through as soon as it finishes.
  // Only the queuing thread may wait (single producer). Otherwise it wakes the thread once and waits
  // without helping.
  //
  /*
   * The ring helps instead of waiting (fh1_native_uploads_help).
   *
   * Why. In a race the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws, 5 waits)
   * and 17.9 ms in the next (3,890 draws, 3 waits; the game spent 21.1 ms without room in the ring). Copying
   * what those frames needed is ~9-11 MB, ~6-7 ms at the rate the profile gives the thread (1.5 GB/s): the
   * rest was the thread ready and without a core. It ran at 0x3B, below the two game threads (0x3A), and
   * Horizon only moves a ready thread to another core when that core runs out of work: with the CPU at
   * 278 % out of 300 (the profile block for that stutter), almost never. And the ring (0x2D) waited for it
   * on a condition variable, which does not lend priority: a textbook priority inversion.
   *
   * How. On every wait with pending copies (by the queuing thread, in the applying phase):
   *   1. the ring takes and copies, in batches of kCopiesByChunk, whatever the thread has not taken yet.
   *      If the thread is running on another core they share what is left; if it has no core, the ring
   *      does it all, without waiting for it.
   *   2. whatever is missing can only be the batch the thread is working on. The ring waits by taking
   *      copies_chunk_mutex_, which the thread holds while copying: a libnx mutex waits in the kernel
   *      (svcArbitrateLock) and Horizon lends the owner the waiter's priority, so the thread rises to 0x2D
   *      until it releases it. It is one batch.
   * The thread's own priority is not changed: it takes no core from anyone while the ring is not waiting
   * for it.
   *
   * Counters. copies_written_ (ring only), copies_taken_ (the thread or the ring, with CAS, in order)
   * and copies_done_ (how many are done, whoever did them): done <= taken <= written. Outside
   * WaitUploads only the thread takes, so it finishes in order and "done = h" still means the first h
   * are done: EnqueueCopy's space accounting still holds. Inside, the ring does not queue, and it leaves
   * with everything done.
   *
   * Same data. Each copy is done exactly once by CopyVertices with the same slot (source, destination,
   * words and byte order), and all of them before returning from WaitUploads, as before: before
   * submitting and before returning the read pointer. Only who does it changes, and sometimes it is done
   * earlier.
   *
   * Self-checking guard. Each copy leaves its index + 1 in copies_marks_. On each wait the ring checks the
   * marks of what was queued since the previous one and that the counters add up; a copy without a mark is
   * done right there, before submitting and before returning the read pointer, and it is a DIFFERENCE
   * (REXLOG_ERROR). Phases: observing (the first kWaitsCopiesWatching waits with pending copies: the
   * thread already takes batches, the ring waits as before and checks), applying (help) and, after a
   * DIFFERENCE with help on, no help (the plain wait). A DIFFERENCE without help, or with the counters out
   * of step, turns the thread off for the session and the copies go back to the ring (the original
   * behaviour). The plain wait no longer waits forever: after 2 s it takes the batch mutex and finishes the
   * job. The batch mutex has no timeout because inside it the thread only takes and copies.
   */
  enum PhaseCopies : int32_t { kCopiesWithoutHelp = 0, kCopiesWatching = 1, kCopiesApplying = 2 };

  void NotifyOtherThreadCopies(const char* where) {
    if (!copies_other_thread_warned_) {
      copies_other_thread_warned_ = true;
      REXLOG_ERROR("[native] C6: {} from a thread that is not the one that queues the vertex copies", where);
    }
  }

  bool EnqueueCopy(const WorkCopy& work) {
    if (copies_without_thread_) {
      // Thread switched off by a DIFFERENCE (or it could not be created): the copy goes on the ring.
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(work.words) * 4;
      return false;
    }
    if (!copies_thread_.joinable()) {
      copies_producer_ = std::this_thread::get_id();
      copies_phase_ = copies_help_requested_ ? kCopiesWatching : kCopiesWithoutHelp;
#if REX_PLATFORM_SWITCH
      const int ring_core = RexSwitchCurrentCore();  // asked by the ring, which creates the thread
#else
      const int ring_core = -1;
#endif
      try {
        copies_thread_ = std::thread([this, ring_core] {
          rex::thread::set_current_thread_name("FH1 vertex copies");
#if REX_PLATFORM_SWITCH
          // fh1_native_uploads_thread_priority (see the cvar).
          const int32_t priority = REXCVAR_GET(fh1_native_uploads_thread_priority);
          const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
          // fh1_native_uploads_thread_core (see the cvar).
          const int32_t requested = REXCVAR_GET(fh1_native_uploads_thread_core);
          const int core = requested == -2 ? (ring_core == 2 ? 1 : 2) : int(requested);
          const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
          REXLOG_INFO("[native] C6: vertex copy thread at priority {:#x}{}; preferred core {}{} (the ring runs on "
                      "{}), now on {}",
                      priority, priority_ok ? "" : " (NOT accepted: it keeps the one it was created with)", core,
                      core_ok ? "" : " (NOT accepted)", ring_core, RexSwitchCurrentCore());
#endif
          LoopCopies();
        });
      } catch (const std::system_error& error) {
        copies_without_thread_ = true;
        REXLOG_ERROR("[native] C6: could not create the vertex copy thread ({}): copies go on the ring",
                     error.what());
        return false;
      }
      REXLOG_INFO("[native] C6: vertex copies on a separate thread (fh1_native_uploads_thread); the ring copies "
                  "them itself while waiting for them (fh1_native_uploads_help) = {}",
                  copies_help_requested_
                      ? fmt::format("YES, in chunks of {}; WATCHING phase during the first {} waits with pending "
                                    "copies",
                                    kCopiesByChunk, kWaitsCopiesWatching)
                      : std::string("no, waits for the thread as before"));
    } else if (std::this_thread::get_id() != copies_producer_) {
      NotifyOtherThreadCopies("EnqueueCopy");
    }
    const size_t written = copies_written_.load(std::memory_order_relaxed);
    if (written - copies_done_.load(std::memory_order_acquire) >= copies_.size()) {
      ++copies_in_line_;
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(work.words) * 4;
      return false;
    }
    copies_[written & (copies_.size() - 1)] = work;
    copies_written_.store(written + 1);
    ++copies_inf_.queued;
    copies_inf_.bytes_queued += uint64_t(work.words) * 4;
    // Wake-up every kCopiesByWarning copies: waking the thread as soon as it slept woke it on almost every
    // copy, and every wake-up goes through the kernel (1.4 % of the ring and almost all of the copy thread's
    // CPU). Whatever is left without a wake-up is collected by WaitUploads.
    if (((written + 1) & (kCopiesByWarning - 1)) == 0) {
      WakeCopies();
    }
    return true;
  }

  void WakeCopies() {
    bool notify;
    {
      std::lock_guard<std::mutex> lock(copies_mutex_);
      notify = copies_sleeping_;
    }
    if (notify) {
      copies_cv_.notify_one();
    }
  }

  // Fence measurement only (fh1_native_system.cpp, NoteFenceCopies).
  size_t CopiesPending() const override {
    if (!copies_thread_.joinable()) {
      return 0;
    }
    const size_t written = copies_written_.load(std::memory_order_relaxed);
    const size_t done_2 = copies_done_.load(std::memory_order_relaxed);
    return written > done_2 ? written - done_2 : 0;
  }

  void WaitUploads() override {
    if (!copies_thread_.joinable()) {
      return;
    }
    const bool producer = std::this_thread::get_id() == copies_producer_;
    if (!producer) {
      NotifyOtherThreadCopies("WaitUploads");
    }
    const size_t target_2 = copies_written_.load(std::memory_order_relaxed);
    if (copies_done_.load(std::memory_order_acquire) == target_2) {
      if (producer) {
        CheckCopies(target_2);  // what the thread did since the last wait
      }
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    // What the thread had not taken yet on arrival (while observing, what the ring would have copied).
    copies_inf_.without_take += target_2 - std::min(target_2, copies_taken_.load(std::memory_order_acquire));
    uint64_t ns_helping = 0;
    uint64_t helped = 0;
    if (producer && copies_phase_ == kCopiesApplying) {
      // 1. What the thread has not taken yet, the ring copies in batches: if the thread is running, they
      //    share it.
      uint64_t bytes = 0;
      while (TakeYCopy(true, helped, bytes)) {
      }
      const auto after_help = std::chrono::steady_clock::now();
      ns_helping = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_help - before).count());
      copies_helped_ += helped;
      copies_bytes_helped_ += bytes;
      copies_ns_helping_ += ns_helping;
      copies_inf_.helped += helped;
      copies_inf_.bytes_helped += bytes;
      copies_inf_.ns_helping += ns_helping;
      if (helped) {
        ++copies_inf_.waits_with_help;
      }
      // 2. What is missing is the batch the thread is working on: wait on its mutex, which lends the thread
      //    the ring's priority until it releases it.
      if (copies_done_.load(std::memory_order_acquire) != target_2) {
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          if (copies_done_.load(std::memory_order_acquire) != target_2) {
            RepairCopies(target_2, "with the chunk lock held the thread has nothing in hand and copies are still "
                                   "missing");
          }
        }
        const uint64_t ns_chunk = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - after_help)
                                               .count());
        ++copies_inf_.waits_chunk;
        copies_inf_.ns_chunk += ns_chunk;
        copies_inf_.ns_chunk_worst = std::max(copies_inf_.ns_chunk_worst, ns_chunk);
      }
    } else {
      WaitCopiesAsAlways(target_2);
    }
    if (producer) {
      CheckCopies(target_2);
      if (copies_phase_ == kCopiesWatching && ++copies_waits_watching_ >= kWaitsCopiesWatching) {
        copies_phase_ = kCopiesApplying;
        REXLOG_INFO("[native] C6: vertex copies: {} waits with pending copies in the WATCHING phase and {} copies "
                    "checked, all with their mark and the counters balanced. Moving to the APPLYING phase: the "
                    "ring copies what the thread has not taken and waits for the chunk in progress with its lock",
                    copies_waits_watching_, copies_checked_);
      }
    }
    ++waits_copies_;
    const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before)
                                           .count());
    // The waiting time now counts only the wait; what the ring copies is counted separately (as helping).
    const uint64_t ns_wait = ns_total > ns_helping ? ns_total - ns_helping : 0;
    ns_waiting_copies_ += ns_wait;
    ++copies_inf_.waits;
    copies_inf_.ns_wait += ns_wait;
    copies_inf_.ns_wait_worst = std::max(copies_inf_.ns_wait_worst, ns_wait);
    // And to the "[hitch] ring" line (fh1_hitch_waits.h). This is only reached when the ring really
    // waits, a few times per frame: the fetch_add calls do not show.
    fh1::waits::g_ns_waiting_copies.fetch_add(ns_wait, std::memory_order_relaxed);
    fh1::waits::g_waits_copies.fetch_add(1, std::memory_order_relaxed);
    fh1::waits::g_ns_helping_copies.fetch_add(ns_helping, std::memory_order_relaxed);
    fh1::waits::g_copies_helped.fetch_add(helped, std::memory_order_relaxed);
  }

  // The plain wait (observing and no-help phases, or when another thread waits): the ring sleeps until the
  // thread finishes. After 2 s it no longer keeps waiting forever (the earlier hang): it takes the batch
  // mutex, which lends its priority to the thread if it is copying, and if copies are still missing with
  // the thread out of its loop, the ring does them and switches the thread off.
  void WaitCopiesAsAlways(size_t target_2) {
    bool a_time = false;
    {
      std::unique_lock<std::mutex> lock(copies_mutex_);
      if (copies_sleeping_) {
        copies_cv_.notify_one();  // a wake-up EnqueueCopy skipped (it only wakes every kCopiesByWarning copies)
      }
      copies_waiting_ = true;
      a_time = copies_done_cv_.wait_for(lock, std::chrono::seconds(2), [this, target_2] {
        return copies_done_.load(std::memory_order_acquire) == target_2;
      });
      copies_waiting_ = false;
    }
    ++copies_inf_.waits_whole;
    if (a_time) {
      return;
    }
    // Should never happen: diagnostic for a hang once seen in the menus.
    REXLOG_ERROR("[native] C6: the ring thread has been waiting 2 s for the vertex copies: target {}, done {}, "
                 "taken {}, written {}, copied by the thread {}",
                 target_2, copies_done_.load(std::memory_order_acquire),
                 copies_taken_.load(std::memory_order_acquire), copies_written_.load(std::memory_order_acquire),
                 copies_progress_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
    if (copies_done_.load(std::memory_order_acquire) != target_2) {
      RepairCopies(target_2, "2 s waiting for the thread and, with the thread out of its loop, copies are still "
                             "missing");
    }
  }

  // Takes the next untaken batch (up to kCopiesByChunk copies, in order) and copies it. Used by the
  // thread, holding copies_chunk_mutex_, and by the ring when helping. false = nothing was left to take.
  bool TakeYCopy(bool ring, uint64_t& copies, uint64_t& bytes) {
    const size_t written = copies_written_.load(std::memory_order_acquire);
    size_t taken = copies_taken_.load(std::memory_order_acquire);
    size_t n = 0;
    do {
      if (taken >= written) {
        return false;
      }
      n = std::min<size_t>(written - taken, kCopiesByChunk);
    } while (!copies_taken_.compare_exchange_weak(taken, taken + n, std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
    const size_t mask = copies_.size() - 1;
    for (size_t i = taken; i < taken + n; ++i) {
      const WorkCopy& work = copies_[i & mask];
      CopyVertices(work);
      bytes += uint64_t(work.words) * 4;
      copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
      if (!ring) {
        copies_progress_.store(i + 1, std::memory_order_relaxed);  // for the WaitUploads diagnostic
      }
    }
    copies += n;
    copies_done_.fetch_add(n, std::memory_order_acq_rel);  // publishes the copies and their marks
    return true;
  }

  bool ThereIsCopiesWithoutTake() const {
    return copies_taken_.load(std::memory_order_acquire) != copies_written_.load(std::memory_order_acquire);
  }

  // The guard, on each wait and with everything finished (the ring does not queue while waiting and the
  // thread has nothing half taken): what was queued since the previous one has its mark and the counters
  // add up. Ring only.
  void CheckCopies(size_t target_2) {
    if (target_2 == copies_verified_ || copies_without_thread_) {
      return;
    }
    const size_t mask = copies_.size() - 1;
    const size_t since =
        std::max(copies_verified_, target_2 > copies_.size() ? target_2 - copies_.size() : size_t(0));
    size_t without_mark = 0;
    size_t first = 0;
    for (size_t i = since; i < target_2; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        if (!without_mark) {
          first = i;
        }
        ++without_mark;
      }
    }
    copies_checked_ += target_2 - since;
    copies_inf_.checked += target_2 - since;
    const size_t taken = copies_taken_.load(std::memory_order_acquire);
    const size_t done_2 = copies_done_.load(std::memory_order_acquire);
    if (!without_mark && taken == target_2 && done_2 == target_2) {
      copies_verified_ = target_2;
      return;
    }
    const std::string reason = without_mark ? fmt::format("{} of {} copies without their mark (the first one, "
                                                          "number {})", without_mark,
                                                       target_2 - since, first)
                                         : std::string("all with their mark, but the counters do not balance");
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);  // the thread, out of its copy loop
    if (copies_phase_ == kCopiesApplying && taken == target_2 && done_2 == target_2) {
      // With the counters right, the suspect is the help path: redo whatever is missing and go back to the
      // plain wait.
      const size_t redone = RedoWithoutMark(target_2);
      copies_verified_ = target_2;
      ++copies_differences_;
      copies_phase_ = kCopiesWithoutHelp;
      REXLOG_ERROR("[native] C6: vertex copies: DIFFERENCE: {} ({} queued, counters balanced; {} redone on the "
                   "ring before submitting). Ring help OFF for the rest of the session: waits for the thread as "
                   "before",
                   reason, target_2, redone);
      return;
    }
    RepairCopies(target_2, reason);
  }

  // Copies on the ring whatever was queued up to `target` (since the last check, and at most one lap of
  // the queue) that lacks its mark, and marks it. With copies_chunk_mutex_ held. Returns how many.
  size_t RedoWithoutMark(size_t target_2) {
    const size_t mask = copies_.size() - 1;
    const size_t since =
        std::max(copies_verified_, target_2 > copies_.size() ? target_2 - copies_.size() : size_t(0));
    size_t redone = 0;
    for (size_t i = since; i < target_2; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        CopyVertices(copies_[i & mask]);
        copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
        ++redone;
      }
    }
    return redone;
  }

  // Should never happen. With copies_chunk_mutex_ held, i.e. with the thread out of its copy loop and
  // nothing half taken: the ring copies whatever is missing up to `target` (before submitting and before
  // returning the read pointer), the counters are left consistent and the thread is switched off for the
  // session.
  void RepairCopies(size_t target_2, const std::string& reason) {
    const size_t taken = copies_taken_.load(std::memory_order_acquire);
    const size_t done_2 = copies_done_.load(std::memory_order_acquire);
    const size_t redone = RedoWithoutMark(target_2);
    copies_taken_.store(target_2, std::memory_order_release);
    copies_done_.store(target_2, std::memory_order_release);
    copies_verified_ = target_2;
    ++copies_differences_;
    copies_without_thread_ = true;
    REXLOG_ERROR("[native] C6: vertex copies: DIFFERENCE: {} (queued {}, taken {}, done {}; {} redone on the ring "
                 "before submitting). Copy thread OFF for the rest of the session: copies go on the ring, as "
                 "before build 90",
                 reason, target_2, taken, done_2, redone);
  }

  const char* NamePhaseCopies() const {
    if (copies_without_thread_) {
      return "NO THREAD (copies on the ring)";
    }
    return copies_phase_ == kCopiesApplying ? "APPLYING" : copies_phase_ == kCopiesWatching ? "WATCHING" : "NO HELP";
  }

  // Every 10 s, if there were copies: who did them and how long the ring really waited. Ring only.
  void ReportCopies() {
    if (!copies_thread_.joinable() && !copies_without_thread_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - copies_report_ < std::chrono::seconds(10)) {
      return;
    }
    copies_report_ = now;
    const uint64_t thread_n = copies_thread_n_.load(std::memory_order_relaxed);
    const uint64_t thread_bytes = copies_thread_bytes_.load(std::memory_order_relaxed);
    const uint64_t d_thread_n = thread_n - copies_thread_n_previous_;
    const uint64_t d_thread_bytes = thread_bytes - copies_thread_bytes_previous_;
    copies_thread_n_previous_ = thread_n;
    copies_thread_bytes_previous_ = thread_bytes;
    const ReportCopiesFigures& c = copies_inf_;
    if (c.queued || c.in_line || c.waits) {
      FH1_REPORT_RING(
          "[native] C6 vertex copies (build 185), last 10 s: {} queued ({:.1f} MB) and {} on the ring without "
          "going through the queue ({:.1f} MB); the thread copied {} ({:.1f} MB) and the ring helped with {} "
          "({:.1f} MB, {:.1f} ms) in {} of {} waits with pending copies ({} not taken on arrival); waited for the "
          "thread's chunk {} times ({:.2f} ms, worst {:.2f} ms) and for all of it {} times; pure wait {:.1f} ms "
          "(worst {:.2f} ms); {} copies checked, {} differences; phase {}",
          c.queued, double(c.bytes_queued) / 1048576.0, c.in_line, double(c.bytes_in_line) / 1048576.0,
          d_thread_n, double(d_thread_bytes) / 1048576.0, c.helped, double(c.bytes_helped) / 1048576.0,
          double(c.ns_helping) / 1e6, c.waits_with_help, c.waits, c.without_take, c.waits_chunk,
          double(c.ns_chunk) / 1e6, double(c.ns_chunk_worst) / 1e6, c.waits_whole, double(c.ns_wait) / 1e6,
          double(c.ns_wait_worst) / 1e6, c.checked, copies_differences_, NamePhaseCopies());
    }
    copies_inf_ = ReportCopiesFigures{};
  }

  // The per-fetch-constant sampler table, every 10 s: hits, and misses by cause.
  void ReportCacheFetch() {
    const auto now = std::chrono::steady_clock::now();
    if (now - fetch_report_ < std::chrono::seconds(10)) {
      return;
    }
    fetch_report_ = now;
    const std::array<uint64_t, 6> now_figures{samplers_cache_fetch_, fetch_misses_collision_, fetch_misses_empty_,
                                               fetch_misses_generation_, fetch_misses_stale_, samplers_cache_};
    std::array<uint64_t, 6> d{};
    for (size_t i = 0; i < d.size(); ++i) {
      d[i] = now_figures[i] - fetch_report_previous_[i];
    }
    fetch_report_previous_ = now_figures;
    const uint64_t misses = d[1] + d[2] + d[3] + d[4];
    if (d[0] + misses == 0) {
      return;
    }
    FH1_REPORT_RING(
        "[native] C6 sampler cache per fetch (build 192, {} slots), last 10 s: {} register hits, {} table lookups "
        "with {} hits and {} misses ({:.1f} %): {} collisions with another fetch in its slot, {} in an empty slot, "
        "{} by generation and {} stale (the texture is due for a check)",
        cache_fetch_.size(), d[5], d[0] + misses, d[0], misses,
        100.0 * double(misses) / double(std::max<uint64_t>(d[0] + misses, 1)), d[1], d[2], d[3], d[4]);
  }

  void StopCopies() {
    if (!copies_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(copies_mutex_);
      copies_stop_ = true;
    }
    copies_cv_.notify_one();
    copies_thread_.join();
    REXLOG_INFO("[native] C6: vertex copy thread stopped ({} copies on the ring because the queue was full, {} "
                "ring waits, {:.1f} ms waiting; 26/09 (build 185): the ring copied {} while waiting ({:.1f} MB, "
                "{:.1f} ms), {} checked, {} differences, phase {})",
                copies_in_line_, waits_copies_, double(ns_waiting_copies_) / 1e6, copies_helped_,
                double(copies_bytes_helped_) / 1048576.0, double(copies_ns_helping_) / 1e6, copies_checked_,
                copies_differences_, NamePhaseCopies());
  }

  void LoopCopies() {
    for (;;) {
      if (ThereIsCopiesWithoutTake()) {
        // Holding copies_chunk_mutex_ from the first batch until there is nothing left to take, and never
        // waiting on anything inside. If the ring waits for the current batch, it waits on this mutex and the
        // thread runs at the ring's priority until it releases it.
        uint64_t copies = 0;
        uint64_t bytes = 0;
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          while (TakeYCopy(false, copies, bytes)) {
          }
        }
        copies_thread_n_.fetch_add(copies, std::memory_order_relaxed);
        copies_thread_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        continue;  // look again: if nothing is left, sleep through the path below
      }
      // Nothing to take (the ring may have taken the last ones). Under the lock: wake the ring if it waits,
      // and sleep until there is more. If the read of copies_written_ was stale, the ring signals at its next
      // point that takes the lock.
      std::unique_lock<std::mutex> lock(copies_mutex_);
      if (copies_waiting_) {
        copies_done_cv_.notify_one();
      }
      copies_sleeping_ = true;
      copies_cv_.wait(lock, [this] { return copies_stop_ || ThereIsCopiesWithoutTake(); });
      copies_sleeping_ = false;
      /*
       * The thread used to die on its own. It exited with "if (!ThereIsCopiesWithoutTake()) return;", on the
       * assumption that it was only woken without work in order to stop. But the ring takes untaken copies
       * when it waits for them (TakeYCopy, without copies_mutex_): if the thread woke because there were
       * copies and the ring took them before this check, the thread ended for good. It was seen at the
       * Heritage & Omega exit with the ring at 94 %: from then on "the thread copied 0" in every report, the
       * menu included, and the ring did all the copies (3-4 ms more per frame for the rest of the session).
       * Now it only exits when asked to stop; otherwise it looks again.
       */
      if (copies_stop_ && !ThereIsCopiesWithoutTake()) {
        return;  // stop, nothing pending
      }
    }
  }

  // fh1_color_filter. If the draw is the final composite and the filter is not the original one, uploads its
  // own copy of the constants with the color curve changed, and leaves the cache empty so the next draw uploads the
  // game's. If there is no room in the upload buffer (Draw does not count this copy), the frame keeps the original
  // filter.
  // It is out of line and Draw always calls it: that way Draw gains no branch and its control flow is still the
  // one in the PGO profile. With a branch inside, GCC would drop the whole profile of the ring's hottest function.
  [[gnu::noinline]] void CopyCompositionHandled(const EntryShader* ps, const uint32_t* r,
                                                 uint32_t bytes_ps) noexcept {
    if (!ps || !ps->shader || ps->shader->fingerprint != kFingerprintComposition) {
      return;
    }
    const int mode = fh1::settings::TreatmentVisual();
    // One line per mode change, on the first draw of the composite: the values the game sets.
    static std::atomic<int> mode_noted{-1};
    if (mode_noted.exchange(mode, std::memory_order_relaxed) != mode) {
      const float* k = reinterpret_cast<const float*>(r + kRegConstantsPs);
      REXLOG_INFO("[native] color filter {}: composition with {} bytes of constants. The game sets Coeffs0 "
                  "({:.3f}, {:.3f}, {:.3f}, {:.3f}), Coeffs1 ({:.3f}, {:.3f}, {:.3f}, {:.3f}), Desaturation {:.3f} "
                  "and vignette {:.3f}",
                  mode, bytes_ps, k[24], k[25], k[26], k[27], k[28], k[29], k[30], k[31], k[8], k[4]);
    }
    if (bytes_ps < kBytesTreatment) {
      return;
    }
    VkDeviceSize offset = 0;
    if (mode == 0 || !Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps,
                               use_ubo_ ? alignment_ubo_ : 16, offset)) {
      return;
    }
    std::memcpy(upload_data_ + offset, r + kRegConstantsPs, bytes_ps);
    ApplyTreatmentVisual(reinterpret_cast<float*>(upload_data_ + offset), mode);
    constants_ps_offset_ = offset;
    constants_ps_generation_ = UINT64_MAX;
  }

  bool Reserve(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& offset) {
    const VkDeviceSize start = (upload_used_ + alignment - 1) & ~(alignment - 1);
    if (start + bytes > kSizeUpload) {
      offset = 0;
      return false;  // cannot happen: Draw checks the space first
    }
    offset = start;
    upload_used_ = start + bytes;
    megabytes_bytes_ += bytes;
    return true;
  }

  // Like Reserve, but the offset is a multiple of `multiple` (the stride: a multiple of 4, not always a
  // power of 2). Draw has already asked for space including that extra padding (gap_base_zero).
  bool ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset) {
    const VkDeviceSize start = (upload_used_ + multiple - 1) / multiple * multiple;
    if (start + bytes > kSizeUpload) {
      offset = 0;
      return false;  // cannot happen: Draw checks the space first
    }
    offset = start;
    upload_used_ = start + bytes;
    megabytes_bytes_ += bytes;
    return true;
  }

  // The pipelines file (cache/fh1_native_pipelines.bin): the Vulkan cache and the prewarm list.
  static std::filesystem::path PathFilePipelines() {
    return rex::filesystem::GetExecutableFolder() / kFolderCache / kFilePipelines;
  }

  static void ReadFileWhole(const std::filesystem::path& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = file ? std::streamoff(file.tellg()) : 0;
    if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
      data.resize(size_t(bytes));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(bytes))) {
        data.clear();
      }
    }
  }

  // Reads the pipelines file and splits it into the cache and the list. If it does not exist yet, the two
  // older files (next to the NRO), which are deleted when the new one is written.
  void ReadFilePipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& list) {
    std::vector<uint8_t> file_data;
    ReadFileWhole(PathFilePipelines(), file_data);
    if (!file_data.empty()) {
      uint32_t magic = 0;
      uint32_t version = 0;
      uint64_t bytes_list = 0;
      uint64_t bytes_cache = 0;
      if (file_data.size() >= kHeaderFilePipelines) {
        std::memcpy(&magic, file_data.data(), 4);
        std::memcpy(&version, file_data.data() + 4, 4);
        std::memcpy(&bytes_list, file_data.data() + 8, 8);
        std::memcpy(&bytes_cache, file_data.data() + 16, 8);
      }
      if (magic != kMagicFilePipelines || version != kVersionFilePipelines ||
          bytes_list > file_data.size() || bytes_cache > file_data.size() ||
          kHeaderFilePipelines + bytes_list + bytes_cache != file_data.size()) {
        REXLOG_WARN("[native] C6: pipeline file of another version or damaged: starting from scratch");
        return;
      }
      const auto start = file_data.begin() + kHeaderFilePipelines;
      list.assign(start, start + std::ptrdiff_t(bytes_list));
      cache.assign(start + std::ptrdiff_t(bytes_list), file_data.end());
      return;
    }
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
    ReadFileWhole(folder / kFileCacheOld, cache);
    ReadFileWhole(folder / kFileListOld, list);
    files_old_ = !cache.empty() || !list.empty();
  }

  // On-disk pipeline cache (derived from the game's shaders: not distributed). If the driver rejects it
  // (another driver or corrupt data), it starts from scratch.
  void LoadCachePipelines() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto create = reinterpret_cast<FnCreateCachePipelines>(
        ifn.vkGetDeviceProcAddr(device_, "vkCreatePipelineCache"));
    data_cache_ = reinterpret_cast<FnDataCachePipelines>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetPipelineCacheData"));
    destroy_cache_ = reinterpret_cast<FnDestroyCachePipelines>(
        ifn.vkGetDeviceProcAddr(device_, "vkDestroyPipelineCache"));
    if (!create || !data_cache_ || !destroy_cache_) {
      REXLOG_WARN("[native] C6: no pipeline cache (the driver does not provide its functions)");
      return;
    }
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data;
    ReadFilePipelines(data, list_read_);
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      data.clear();
      if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
        cache_pipelines_ = VK_NULL_HANDLE;
      }
    }
    bytes_cache_saved_ = data.size();
    cache_saved_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C6: pipeline cache {} ({} KB read from {}{})",
                cache_pipelines_ != VK_NULL_HANDLE ? "active" : "not available",
                data.size() >> 10, path.string(), files_old_ ? ", from the two old files" : "");
    // The writer thread always saves both parts; it starts from what is on disk.
    written_cache_ = std::move(data);
    if (files_old_) {
      // they came from the two older files: moved to the new one on the first save even if nothing changed
      bytes_cache_saved_ = 0;
      pipelines_without_save_ = 1;
      list_without_save_ = 1;
    }
    LoadListPipelines();  // the prewarm list
  }

  void SaveCachePipelines() {
    cache_saved_ = std::chrono::steady_clock::now();
    // The prewarm list is saved even if the cache does not grow (a new pipeline whose shaders were already
    // in it does not make it grow). The cache, as usual: only if it has new entries.
    std::vector<uint8_t> list;
    if (list_without_save_) {
      list_without_save_ = 0;
      list = SerializeListPipelines();
    }
    std::vector<uint8_t> data;
    if (pipelines_without_save_) {
      pipelines_without_save_ = 0;
      size_t bytes = 0;
      // without new entries it is not rewritten (the SD card is slow on the Switch)
      if (cache_pipelines_ != VK_NULL_HANDLE &&
          data_cache_(device_, cache_pipelines_, &bytes, nullptr) == VK_SUCCESS && bytes &&
          bytes != bytes_cache_saved_) {
        data.resize(bytes);
        const VkResult result = data_cache_(device_, cache_pipelines_, &bytes, data.data());
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !bytes) {
          data.clear();
        } else {
          data.resize(bytes);
          bytes_cache_saved_ = bytes;
        }
      }
    }
    if (data.empty() && list.empty()) {
      return;
    }
    // The file is written on its own thread: on the Switch, writing about 2 MB to the SD card from the ring
    // thread stalled it on every save. If the previous one has not been written yet, this one replaces it.
    std::lock_guard<std::mutex> lock(writer_mutex_);
    if (!writer_cache_.joinable()) {
      writer_cache_ = std::thread([this] {
        rex::thread::set_current_thread_name("FH1 pipeline cache");
        WriterCacheMain();
      });
    }
    if (!data.empty()) {  // only the list, if the cache did not grow
      writer_data_ = std::move(data);
    }
    if (!list.empty()) {
      writer_list_ = std::move(list);
    }
    writer_pending_ = true;
    writer_warning_.notify_one();
  }

  // Pipeline cache writer thread. Sleeping or continuing is decided with the lock held.
  void WriterCacheMain() {
    std::vector<uint8_t> data;
    std::vector<uint8_t> list;  // the prewarm one
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(writer_mutex_);
        writer_warning_.wait(lock, [this] { return writer_pending_ || writer_stop_; });
        if (!writer_pending_) {
          return;  // stop, nothing pending
        }
        data = std::move(writer_data_);
        writer_data_ = {};
        list = std::move(writer_list_);
        writer_list_ = {};
        writer_pending_ = false;
      }
      // A single file with both parts. Only one may arrive (the cache did not grow, or the list did not
      // change): the other is the last one written.
      if (!data.empty()) {
        written_cache_ = std::move(data);
      }
      if (!list.empty()) {
        written_list_ = std::move(list);
      }
      WriteFilePipelines();
    }
  }

  // cache/fh1_native_pipelines.bin with the list and the cache, written to a temporary file that is then
  // renamed. Writer thread only. The first time it is written, the two older files are deleted if present.
  void WriteFilePipelines() {
    const auto before = std::chrono::steady_clock::now();
    const std::filesystem::path path = PathFilePipelines();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporal = path;
    temporal += ".tmp";
    {
      const uint32_t magic = kMagicFilePipelines;
      const uint32_t version = kVersionFilePipelines;
      const uint64_t bytes_list = written_list_.size();
      const uint64_t bytes_cache = written_cache_.size();
      std::ofstream file(temporal, std::ios::binary | std::ios::trunc);
      if (!file || !file.write(reinterpret_cast<const char*>(&magic), 4) ||
          !file.write(reinterpret_cast<const char*>(&version), 4) ||
          !file.write(reinterpret_cast<const char*>(&bytes_list), 8) ||
          !file.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
          !file.write(reinterpret_cast<const char*>(written_list_.data()), std::streamsize(bytes_list)) ||
          !file.write(reinterpret_cast<const char*>(written_cache_.data()), std::streamsize(bytes_cache))) {
        return;
      }
    }
    error.clear();
    std::filesystem::rename(temporal, path, error);
    if (error) {
      std::filesystem::remove(path, error);
      std::filesystem::rename(temporal, path, error);
    }
    if (!error && files_old_) {
      const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
      std::error_code without_importance;
      std::filesystem::remove(folder / kFileCacheOld, without_importance);
      std::filesystem::remove(folder / kFileListOld, without_importance);
      files_old_ = false;
    }
    if (saved_cache_++ < 8) {
      REXLOG_INFO("[native] C6: pipelines saved ({} KB of cache and {} in the prewarm list, in {} ms, from its "
                  "thread{})",
                  written_cache_.size() >> 10,
                  written_list_.size() >= kHeaderList
                      ? (written_list_.size() - kHeaderList) / sizeof(RegisterPipeline)
                      : 0,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                      .count(),
                  error ? ", could not rename" : "");
    }
  }

  void StopWriterCache() {
    {
      std::lock_guard<std::mutex> lock(writer_mutex_);
      writer_stop_ = true;
      writer_warning_.notify_one();
    }
    if (writer_cache_.joinable()) {
      writer_cache_.join();
    }
  }

  // One upload buffer per work slot of the render target code: the previous frame's may still be in use
  // on the GPU while the other is filled.
  bool CreateUpload() {
    for (BufferUpload& s : uploads_) {
      const bool created_2 = CreateBufferUpload();
      s = {upload_, upload_memory_, upload_size_real_, upload_data_, upload_address_};
      upload_ = VK_NULL_HANDLE;
      upload_memory_ = VK_NULL_HANDLE;
      upload_data_ = nullptr;
      if (!created_2) {
        return false;
      }
    }
    const auto& types = vulkan_device_->memory_types();
    REXLOG_INFO("[native] C6: upload buffer in memory type {} ({}, {}; fh1_native_upload_memory = {})",
                upload_type_, ((types.host_cached >> upload_type_) & 1) ? "with CPU cache" : "without CPU cache",
                upload_coherent_ ? "coherent, not published" : "published before submitting",
                REXCVAR_GET(fh1_native_upload_memory));
    MeasureMemoryUpload();
    CreateSharedSeparate();
    UseSlot(0);
    return true;
  }

  // A small CPU-cached buffer per slot, only for the shared constants. If anything fails, it continues as
  // before (in the upload buffer) and says so in the log.
  void CreateSharedSeparate() {
    shared_separate_ = false;
    if (!REXCVAR_GET(fh1_native_shared_cache)) {
      REXLOG_INFO("[native] C6: shared constants in the upload buffer (fh1_native_shared_cache = false)");
      return;
    }
    const auto& types = vulkan_device_->memory_types();
    for (BufferUpload& c : shared_bufs_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = kSharedConstantsSize;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &c.buffer) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not create the shared-constants buffer; they go in the upload buffer");
        return;
      }
      VkMemoryRequirements requirements;
      dfn_.vkGetBufferMemoryRequirements(device_, c.buffer, &requirements);
      uint32_t type = 0;
      if (!rex::bit_scan_forward(requirements.memoryTypeBits & types.host_visible & types.host_cached, &type)) {
        REXLOG_WARN("[native] C6: no visible memory with CPU cache; the shared constants go in the upload buffer");
        return;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = requirements.size;
      reserve.memoryTypeIndex = type;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &c.memory_block) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, c.buffer, c.memory_block, 0) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not allocate the shared-constants buffer; they go in the upload buffer");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, c.memory_block, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not map the shared-constants buffer; they go in the upload buffer");
        return;
      }
      c.data = static_cast<uint8_t*>(mapped);
      c.size_real = requirements.size;
      shared_type_ = type;
    }
    shared_coherent_ = (types.host_coherent >> shared_type_) & 0x1;
    shared_separate_ = true;
    REXLOG_INFO("[native] C6: shared constants separate, in memory type {} (with CPU cache, {}), {} MB per slot",
                shared_type_, shared_coherent_ ? "coherent" : "published before submitting",
                kSharedConstantsSize >> 20);
  }

  // FH1: vertex streams the shaders read from memory (EntryVertices::fetches_memory), kept across frames: copied
  // once, in host byte order, and copied again (to a new place) only when the guest bytes change. offset = where
  // the stream is in the buffer whose device address is streams_address_. false = no room or no buffer: the caller
  // uses the upload buffer.
  bool StreamInCache(const uint8_t* data, uint32_t bytes, uint64_t address, xenos::Endian order, uint32_t& offset) {
    // Small streams are the game's per-frame ones (a new address or new bytes every frame: 1,800 of them in a
    // minute of festival): they would only fill the buffer, and copying them is cheap.
    if (streams_failed_ || bytes < 65536) {
      return false;
    }
    if (streams_buffer_ == VK_NULL_HANDLE && !CreateBufferStreams()) {
      streams_failed_ = true;
      REXLOG_WARN("[native] C6: no buffer for the streams read by shaders: they go in the upload buffer");
      return false;
    }
    StreamCached& e = streams_[(address << 26) ^ (uint64_t(bytes) << 2) ^ uint64_t(order)];
    if (e.offset && e.frame == frame_) {
      offset = e.offset;
      return true;
    }
    const uint64_t fingerprint = XXH3_64bits(data, bytes);
    if (e.offset && e.fingerprint == fingerprint) {
      e.frame = frame_;
      offset = e.offset;
      return true;
    }
    const VkDeviceSize start = (streams_used_ + 15) & ~VkDeviceSize(15);
    if (start + bytes > kSizeStreams) {
      return false;
    }
    CopyVertices(WorkCopy{data, streams_data_ + start, bytes / 4, order});
    streams_used_ = start + bytes;
    if (!streams_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, streams_memory_, streams_type_, 0,
                                                    streams_size_real_, streams_used_);
    }
    REXLOG_INFO("[fh1] stream read by shaders {:08X} ({} bytes) kept at {} of the streams buffer ({} KB used)",
                address, bytes, start, streams_used_ >> 10);
    e.fingerprint = fingerprint;
    e.offset = uint32_t(start);
    e.frame = frame_;
    offset = e.offset;
    return true;
  }

  bool CreateBufferStreams() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kSizeStreams;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &streams_buffer_) != VK_SUCCESS) {
      streams_buffer_ = VK_NULL_HANDLE;
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, streams_buffer_, &requirements);
    streams_type_ = rex::ui::vulkan::util::ChooseMemoryType(vulkan_device_->memory_types(), requirements.memoryTypeBits,
                                                            rex::ui::vulkan::util::MemoryPurpose::kUpload);
    if (streams_type_ == UINT32_MAX) {
      return false;
    }
    streams_coherent_ = (vulkan_device_->memory_types().host_coherent >> streams_type_) & 0x1;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.pNext = &flags;
    reserve.allocationSize = requirements.size;
    reserve.memoryTypeIndex = streams_type_;
    void* mapped = nullptr;
    if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &streams_memory_) != VK_SUCCESS ||
        dfn_.vkBindBufferMemory(device_, streams_buffer_, streams_memory_, 0) != VK_SUCCESS ||
        dfn_.vkMapMemory(device_, streams_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    streams_size_real_ = requirements.size;
    streams_data_ = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = streams_buffer_;
    streams_address_ = address_buffer_(device_, &address);
    return streams_address_ != 0;
  }

  bool CreateBufferUpload() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kSizeUpload;
    // UNIFORM_BUFFER because it is also bound as a dynamic UBO (constants through UBOs, set 4).
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &upload_) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, upload_, &requirements);
    // fh1_native_upload_memory: with 1 or 2, a host-visible type with or without CPU caching is looked
    // for; if there is none, the SDK's.
    const auto& types = vulkan_device_->memory_types();
    upload_type_ = UINT32_MAX;
    const int32_t preference = REXCVAR_GET(fh1_native_upload_memory);
    if (preference == 1 || preference == 2) {
      const uint32_t visibles = requirements.memoryTypeBits & types.host_visible;
      uint32_t type = 0;
      if (rex::bit_scan_forward(visibles & (preference == 1 ? types.host_cached : ~types.host_cached), &type)) {
        upload_type_ = type;
      }
    }
    if (upload_type_ == UINT32_MAX) {
      upload_type_ = rex::ui::vulkan::util::ChooseMemoryType(types, requirements.memoryTypeBits,
                                                             rex::ui::vulkan::util::MemoryPurpose::kUpload);
    }
    if (upload_type_ == UINT32_MAX) {
      return false;
    }
    upload_coherent_ = (vulkan_device_->memory_types().host_coherent >> upload_type_) & 0x1;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.pNext = &flags;
    reserve.allocationSize = requirements.size;
    reserve.memoryTypeIndex = upload_type_;
    if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &upload_memory_) != VK_SUCCESS) {
      return false;
    }
    upload_size_real_ = requirements.size;
    if (dfn_.vkBindBufferMemory(device_, upload_, upload_memory_, 0) != VK_SUCCESS) {
      return false;
    }
    void* mapped = nullptr;
    if (dfn_.vkMapMemory(device_, upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    upload_data_ = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = upload_;
    upload_address_ = address_buffer_(device_, &address);
    return upload_address_ != 0;
  }

  // For the fh1_native_upload_memory A/B test: CPU write MB/s into each host-visible memory type and the
  // cost of publishing those 8 MB (vkFlushMappedMemoryRanges; in NVK for Tegra, armDCacheClean). Once.
  void MeasureMemoryUpload() {
    if (!REXCVAR_GET(fh1_native_diag_upload_memory)) {
      return;
    }
    constexpr VkDeviceSize kBytes = VkDeviceSize(8) << 20;
    std::vector<uint8_t> source(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < source.size(); ++i) {
      source[i] = uint8_t((i * 131) ^ (i >> 11));
    }
    const auto& types = vulkan_device_->memory_types();
    std::string report;
    for (uint32_t type = 0; type < 32; ++type) {
      if (!((types.host_visible >> type) & 1)) {
        continue;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = kBytes;
      reserve.memoryTypeIndex = type;
      VkDeviceMemory memory_block = VK_NULL_HANDLE;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &memory_block) != VK_SUCCESS) {
        continue;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, memory_block, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        using Clock = std::chrono::steady_clock;
        std::memcpy(mapped, source.data(), size_t(kBytes));  // first pass: pages and caches
        const auto before = Clock::now();
        std::memcpy(mapped, source.data(), size_t(kBytes));
        const auto copied = Clock::now();
        const bool coherent = (types.host_coherent >> type) & 1;
        if (!coherent) {
          VkMappedMemoryRange range{};
          range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range.memory = memory_block;
          range.size = VK_WHOLE_SIZE;
          dfn_.vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        const auto published = Clock::now();
        const double seconds = std::chrono::duration<double>(copied - before).count();
        report += fmt::format(
            "{}type {} ({}{}): {:.0f} MB/s{}", report.empty() ? "" : "; ", type,
            ((types.host_cached >> type) & 1) ? "with CPU cache" : "without CPU cache", coherent ? ", coherent" : "",
            seconds > 0 ? 8.0 / seconds : 0.0,
            coherent ? std::string()
                      : fmt::format(", publish {:.2f} ms",
                                    std::chrono::duration<double, std::milli>(published - copied).count()));
        dfn_.vkUnmapMemory(device_, memory_block);
      }
      dfn_.vkFreeMemory(device_, memory_block, nullptr);
    }
    REXLOG_INFO("[native] C6: CPU write speed by memory type (8 MB): {}",
                report.empty() ? std::string("no measurable types") : report);
  }

  bool CreateDescriptores() {
    static constexpr VkDescriptorType kTypes[4] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    const VkDescriptorBindingFlags flags_binding =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = kTypes[i];
      binding.descriptorCount = kCapacityHeap[i];
      binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
      flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      flags.bindingCount = 1;
      flags.pBindingFlags = &flags_binding;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &flags;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &binding;
      if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]) != VK_SUCCESS) {
        return false;
      }
      heaps_[i].capacity = kCapacityHeap[i];
    }
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         kCapacityHeap[0] + kCapacityHeap[1] + kCapacityHeap[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kCapacityHeap[3]}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 4;
    info_pool.poolSizeCount = 2;
    info_pool.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &pool_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorPool = pool_;
    reserve.descriptorSetCount = 4;
    reserve.pSetLayouts = layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve, sets_.data()) != VK_SUCCESS) {
      return false;
    }
    // --- Set 4, the constants through dynamic UBOs ------------------------------------------------------
    // Always created: the shaders of the current library use it statically even with the bit off (it is
    // then bound with offsets 0). With an older library it is unnecessary and harmless. No
    // UPDATE_AFTER_BIND: dynamic descriptors do not support it. CreateUpload runs first, so the buffers
    // already exist.
    use_ubo_ = REXCVAR_GET(fh1_native_constants_ubo);
    cache_between_frames_ = REXCVAR_GET(fh1_native_texture_cache_across_frames);
    REXLOG_INFO("[native] C6: texture caches across frames (fh1_native_texture_cache_across_frames) = {}",
                cache_between_frames_ ? "SI" : "no");
    mipmaps_ = REXCVAR_GET(fh1_native_mipmaps);
    cube_levels_ = REXCVAR_GET(fh1_native_cube_levels);
    gamma_pwl_ = REXCVAR_GET(fh1_native_gamma_pwl);
    gamma_textures_ = REXCVAR_GET(fh1_native_gamma_textures);
    gamma_targets_ = REXCVAR_GET(fh1_native_gamma_targets);
    REXLOG_INFO("[native] C3: gamma textures read as sRGB (fh1_native_gamma_textures) = {}",
                gamma_textures_ ? "yes" : "no");
    REXLOG_INFO("[native] C3: texture mip levels (fh1_native_mipmaps) = {}", mipmaps_ ? "SI" : "no");
    textures_mb_max_ = REXCVAR_GET(fh1_native_texture_mb_max);
    test_without_memory_every_ = REXCVAR_GET(fh1_native_test_out_of_memory_every);
    if (test_without_memory_every_ > 0) {
      REXLOG_WARN("[native] C3: TEST active: an out-of-memory failure will be faked every {} texture allocations",
                  test_without_memory_every_);
    }
    REXLOG_INFO("[native] C3: texture cache limit (fh1_native_texture_mb_max) = {} MB{}", textures_mb_max_,
                textures_mb_max_ > 0 ? "" : " (no limit)");
    diag_mips_ = REXCVAR_GET(fh1_native_diag_mips);
    if (diag_mips_) {
      REXLOG_INFO("[native] C3: mip diagnostic (fh1_native_diag_mips) = YES");
    }
    alignment_ubo_ = std::max<VkDeviceSize>(16, vulkan_device_->properties().minUniformBufferOffsetAlignment);
    toggle_ubo_s_ = REXCVAR_GET(fh1_native_constants_ubo_toggle_s);
    REXLOG_INFO("[native] C6: constants by dynamic UBO (fh1_native_constants_ubo) = {}; toggle every {} s; "
                "alignment {} bytes",
                use_ubo_ ? "SI" : "no", toggle_ubo_s_, alignment_ubo_);
    std::array<VkDescriptorSetLayoutBinding, 3> bindings_ubo{};
    for (uint32_t b = 0; b < 3; ++b) {
      bindings_ubo[b].binding = b;
      bindings_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings_ubo[b].descriptorCount = 1;
      bindings_ubo[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_ubo{};
    info_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_ubo.bindingCount = 3;
    info_ubo.pBindings = bindings_ubo.data();
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_ubo, nullptr, &layout_ubo_) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize size_ubo{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * uint32_t(sets_ubo_.size())};
    VkDescriptorPoolCreateInfo info_pool_ubo{};
    info_pool_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_ubo.maxSets = uint32_t(sets_ubo_.size());
    info_pool_ubo.poolSizeCount = 1;
    info_pool_ubo.pPoolSizes = &size_ubo;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_ubo, nullptr, &pool_ubo_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, kSlotsOfWork> layouts_ubo;
    layouts_ubo.fill(layout_ubo_);
    VkDescriptorSetAllocateInfo reserve_ubo{};
    reserve_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_ubo.descriptorPool = pool_ubo_;
    reserve_ubo.descriptorSetCount = uint32_t(sets_ubo_.size());
    reserve_ubo.pSetLayouts = layouts_ubo.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve_ubo, sets_ubo_.data()) != VK_SUCCESS) {
      return false;
    }
    for (size_t slot = 0; slot < sets_ubo_.size(); ++slot) {
      const VkDescriptorBufferInfo blocks[3] = {{uploads_[slot].buffer, 0, kUboBytesVs},
                                                 {uploads_[slot].buffer, 0, kUboBytesPs},
                                                 {shared_separate_ ? shared_bufs_[slot].buffer
                                                                      : uploads_[slot].buffer,
                                                  0, kUboBytesShared}};
      std::array<VkWriteDescriptorSet, 3> writes_ubo{};
      for (uint32_t b = 0; b < 3; ++b) {
        writes_ubo[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes_ubo[b].dstSet = sets_ubo_[slot];
        writes_ubo[b].dstBinding = b;
        writes_ubo[b].descriptorCount = 1;
        writes_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes_ubo[b].pBufferInfo = &blocks[b];
      }
      dfn_.vkUpdateDescriptorSets(device_, 3, writes_ubo.data(), 0, nullptr);
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    24};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const std::array<VkDescriptorSetLayout, 5> layouts_pipeline = {layouts_[0], layouts_[1], layouts_[2],
                                                                    layouts_[3], layout_ubo_};
    info_layout.setLayoutCount = 5;
    info_layout.pSetLayouts = layouts_pipeline.data();
    info_layout.pushConstantRangeCount = 1;
    info_layout.pPushConstantRanges = &range;
    return dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &layout_pipeline_) ==
           VK_SUCCESS;
  }

  // Slot 0 of each heap: a transparent black texture and a basic sampler, as the emulation does for an
  // invalid fetch constant.
  bool CreateEmpty() {
    const std::array<std::pair<VkImageViewType, uint32_t>, 3> types = {
        {{VK_IMAGE_VIEW_TYPE_2D, 1}, {VK_IMAGE_VIEW_TYPE_3D, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 6}}};
    for (uint32_t i = 0; i < 3; ++i) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = types[i].second;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
              empty_[i].image, empty_[i].memory_block)) {
        return false;
      }
      VkImageViewCreateInfo view{};
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = empty_[i].image;
      view.viewType = types[i].first;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, types[i].second};
      if (dfn_.vkCreateImageView(device_, &view, nullptr, &empty_[i].view) != VK_SUCCESS) {
        return false;
      }
      empty_[i].width = empty_[i].height = 1;
      empty_[i].format = VK_FORMAT_R8G8B8A8_UNORM;
      WriteImage(i, 0, empty_[i].view);
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_empty_) != VK_SUCCESS) {
      return false;
    }
    WriteSampler(0, sampler_empty_);
    return true;
  }

  // The empty ones are initialized on the first submission: GENERAL and cleared to zero.
  bool PrepareEmpty() {
    if (empty_prepared_) {
      return true;
    }
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload) {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t layers = i == 2 ? 6 : 1;
      Barrier(upload, empty_[i].image, layers);
      VkClearColorValue zero{};
      if (REXCVAR_GET(fh1_native_diag_empty_gray)) {
        zero.float32[0] = zero.float32[1] = zero.float32[2] = 0.5f;
        zero.float32[3] = 1.0f;
      }
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      dfn_.vkCmdClearColorImage(upload, empty_[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                                &range);
      empty_[i].prepared = true;
    }
    empty_prepared_ = true;
    return true;
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers};  // and mips
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
  }

  void WriteImage(uint32_t heap, uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[heap];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  void WriteSampler(uint32_t slot, VkSampler sampler) {
    VkDescriptorImageInfo image{};
    image.sampler = sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[3];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  uint32_t ReserveSlot(uint32_t heap) {
    auto& m = heaps_[heap];
    if (!m.free.empty()) {
      const uint32_t slot = m.free.back();
      m.free.pop_back();
      return slot;
    }
    return m.next < m.capacity ? m.next++ : 0;
  }

  // The draw's vertex input, recomputed only when the patched VS changes.
  const EntryVertices* EntryOf(const RequestDraw& p) {
    if (entry_generation_ == p.generation_vs && entry_vs_ == p.vs) {
      if (entry_valid_) {
        ++entries_reused_;
        return entry_current_;
      }
      // The same input that already failed: no recomputation and no warning.
      ++rejected_;
      ++causes_[entry_cause_];
      return nullptr;
    }
    entry_generation_ = p.generation_vs;
    entry_vs_ = p.vs;
    entry_valid_ = false;
    // Cache: the same VS with the same patched fetches gives the same input.
    const uint64_t key_entry = KeyEntry(p);
    if (key_entry) {
      if (const auto it = entries_cache_.find(key_entry); it != entries_cache_.end()) {
        entry_current_ = &it->second;
        entry_valid_ = true;
        entry_cause_ = 0;
        ++entries_cache_hits_;
        return entry_current_;
      }
    }
    entry_ = EntryVertices{};
    const uint64_t rejected_before = rejected_;
    const auto before_entry = std::chrono::steady_clock::now();
    const EntryVertices* entry = ComputeEntry(p);
    ns_entries_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - before_entry)
                                 .count());
    ++entries_computed_;
    entry_cause_ = rejected_ != rejected_before ? last_cause_ : 0;
    entry_current_ = &entry_;
    if (entry && key_entry && entries_cache_.size() < 4096) {
      entries_cache_.emplace(key_entry, entry_);
    }
    return entry;
  }

  // Key of the input cache: everything ComputeEntry reads from the elements and from both microcodes
  // (instruction, usage and index; original and patched words of each fetch), seeded with the VS. 0 = do
  // not cache.
  uint64_t KeyEntry(const RequestDraw& p) const {
    const EntryShader& vs = *p.vs;
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size() || vs.elements.size() > 32) {
      return 0;
    }
    std::array<uint32_t, 32 * 6> words;
    size_t n = 0;
    for (const ElementVertex& element : vs.elements) {
      const size_t i = size_t(element.instruction) * 3;
      if (i + 2 >= patched.size()) {
        return 0;
      }
      words[n++] = (uint32_t(element.instruction) << 16) | (uint32_t(element.use) << 8) |
                      element.index_use;
      words[n++] = vs.microcode[i];
      words[n++] = vs.microcode[i + 1];
      words[n++] = patched[i];
      words[n++] = patched[i + 1];
      words[n++] = patched[i + 2];
    }
    const uint64_t key =
        XXH3_64bits_withSeed(words.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
    return key ? key : 1;
  }

  const EntryVertices* ComputeEntry(const RequestDraw& p) {
    const EntryShader& vs = *p.vs;
    entry_.remaps.fill(kRemapIdentity);
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size()) {
      Reject(20, "the ring's VS does not have the container's length");
      return nullptr;
    }
    uint32_t locations_used = 0;
    for (const ElementVertex& element : vs.elements) {
      const uint32_t reg_entry = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      const uint32_t original = vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF;
      size_t q = SIZE_MAX;
      for (const ElementVertex& other_value : vs.elements) {
        const size_t i = size_t(other_value.instruction) * 3;
        if (((patched[i] >> 12) & 0x3F) != reg_entry || (patched[i] & 0x1F) != 0) {
          continue;
        }
        if (q == SIZE_MAX) {
          q = i;
        }
        if (MaskWritten(patched[i + 1] & 0xFFF) == MaskWritten(original)) {
          q = i;  // the one that writes the same components
          break;
        }
      }
      if (q == SIZE_MAX) {
        if (warned_vs_.size() < 16 && warned_vs_.insert(vs.number).second) {
          std::string detail;
          for (const ElementVertex& other_value : vs.elements) {
            const size_t i = size_t(other_value.instruction) * 3;
            detail += fmt::format(" {}{}@{}: original r{} op{}, patched r{} op{} {:08X} {:08X} {:08X};",
                                   NameUse(other_value.use), other_value.index_use, other_value.instruction,
                                   (vs.microcode[i] >> 12) & 0x3F, vs.microcode[i] & 0x1F,
                                   (patched[i] >> 12) & 0x3F, patched[i] & 0x1F, patched[i],
                                   patched[i + 1], patched[i + 2]);
          }
          REXLOG_WARN("[native] C6 diag: VS n{} without a fetch for {}{} (register r{}):{}", vs.number,
                      NameUse(element.use), element.index_use, reg_entry, detail);
        }
        Reject(21, "vertex fetch not found in the patched VS");
        return nullptr;
      }
      const uint32_t d0 = patched[q], d1 = patched[q + 1], d2 = patched[q + 2];
      // FH1: source register and component of the fetch index (word 0 bits 5-10 and 30-31). Anything but r0.x is an
      // index the shader computed; a mini fetch (word 1 bit 30) reuses the vertex of the fetch before it.
      uint32_t d0_full = d0;
      if ((d1 >> 30) & 0x1) {
        // The full fetch a mini fetch belongs to: the nearest fetch instruction before it that is not one.
        size_t best = SIZE_MAX;
        for (const ElementVertex& other_value : vs.elements) {
          const size_t i = size_t(other_value.instruction) * 3;
          if (i < q && !((patched[i + 1] >> 30) & 0x1) && (best == SIZE_MAX || i > best)) {
            best = i;
          }
        }
        if (best != SIZE_MAX) {
          d0_full = patched[best];
        }
      }
      if (((d0_full >> 5) & 0x3F) != 0) {
        // Indexed by a computed register: read from memory by the shader, not a vertex attribute.
        uint32_t rank = 0;
        for (const ElementVertex& other_value : vs.elements) {
          if (size_t(other_value.instruction) * 3 < q) {
            ++rank;
          }
        }
        const int32_t offset_words = int32_t(d2 << 1) >> 9;
        if (rank >= 32 || offset_words < 0 || offset_words > 0xFFFF) {
          Reject(318, "vertex fetch with a computed index: beyond the 32 declared fetches or a negative offset");
          return nullptr;
        }
        bool known = false;
        for (const EntryVertices::FetchMemory& m : entry_.fetches_memory) {
          known |= m.rank == rank;
        }
        if (!known) {
          entry_.fetches_memory.push_back(
              {uint8_t(rank), uint8_t(((d0_full >> 20) & 0x1F) * 3 + ((d0_full >> 25) & 0x3)), !((d1 >> 30) & 0x1),
               (d2 & 0xFF) | (uint32_t(offset_words) << 8) | (((d1 >> 16) & 0x3F) << 24) |
                   (((d1 >> 12) & 0x1) << 30) | (((d1 >> 13) & 0x1) << 31)});
        }
        continue;
      }
      if (!((d1 >> 30) & 0x1) && ((d0 >> 30) & 0x3) != 0) {
        entry_.index_computed = true;
      }
      const uint32_t slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
      const uint32_t format = (d1 >> 16) & 0x3F;
      const uint32_t stride = (d2 & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t location = LocationOfUse(element.use, element.index_use);
      if (location < 0 || ((locations_used >> location) & 0x1)) {
        Notify(22, "vertex element without a free location in the shader: skipped");
        continue;
      }
      uint32_t code = CodeRemap(original, d1 & 0xFFF);
      if (format == 16 && !EntryWhole(element.use) && (REXCVAR_GET(fh1_vertices_10_11_11_mask) >> std::min<uint32_t>(element.use, 15) & 1) == 0) {
        Reject(316, "vertex format not supported yet (fh1_vertices_10_11_11_mask)");
        return nullptr;
      }
      if (format == 16 && !EntryWhole(element.use)) {
        // FH1: packed k_10_11_11 (see FormatAttribute): unpacked in the shader, sign and integer modes from the fetch.
        if (location >= 16) {
          Reject(26, "k_10_11_11 vertex in a location without a remap");
          return nullptr;
        }
        code |= 0x1000u | (((d1 >> 12) & 0x1) ? 0x2000u : 0u) | (((d1 >> 13) & 0x1) ? 0x4000u : 0u) |
                (((d1 >> 14) & 0x1) ? 0x8000u : 0u);
      }
      // FH1: the fetch's exp_adjust (word 1 bits 24-29, signed), a power-of-two scale on the format's components that
      // the Vulkan formats do not apply; remapInput does (bits 16-21, component count - 1 in bits 22-23).
      if (const uint32_t exp_adjust = (d1 >> 24) & 0x3F; exp_adjust && !EntryWhole(element.use)) {
        if (location >= 16) {
          Notify(27, "vertex with exp_adjust in a location without a remap: not scaled");
        } else {
          code |= (exp_adjust << 16) | ((ComponentsVertex(format) - 1) << 22);
        }
      }
      if (code != kRemapIdentity && warnings_swizzle_ < 24) {
        ++warnings_swizzle_;
        REXLOG_INFO("[native] C6: VS n{} {}{} (format {}): original swizzle {:03X}, patched {:03X}, remap {:03X}",
                    vs.number, NameUse(element.use), element.index_use, format, original,
                    d1 & 0xFFF, code);
      }
      bool r11g11b10 = false;
      const VkFormat vk = FormatAttribute(format, EntryWhole(element.use), (d1 >> 12) & 0x1,
                                          (d1 >> 13) & 0x1, false, r11g11b10);
      if (vk == VK_FORMAT_UNDEFINED) {
        Reject(300 + format, "vertex format not supported yet");
        return nullptr;
      }
      if (r11g11b10) {
        entry_.specialization |= 0x1;
      }
      uint32_t binding = 0;
      while (binding < entry_.bindings.size() && entry_.bindings[binding].slot != slot) {
        ++binding;
      }
      if (binding == entry_.bindings.size()) {
        entry_.bindings.push_back({slot, stride});
      } else if (!entry_.bindings[binding].stride) {
        entry_.bindings[binding].stride = stride;
      }
      if (offset < 0) {
        Reject(24, "negative vertex offset");
        return nullptr;
      }
      entry_.attributes.push_back({uint32_t(location), binding, vk, uint32_t(offset)});
      entry_.remaps[location] = code;
      locations_used |= uint32_t(1) << location;
    }
    for (const BindingVertices& binding : entry_.bindings) {
      if (!binding.stride) {
        Reject(25, "vertex stream without a stride");
        return nullptr;
      }
    }
    uint64_t fingerprint = XXH3_64bits(entry_.attributes.data(),
                                  entry_.attributes.size() * sizeof(AttributeVertices));
    fingerprint = XXH3_64bits_withSeed(entry_.bindings.data(),
                                  entry_.bindings.size() * sizeof(BindingVertices), fingerprint);
    entry_.fingerprint = fingerprint ^ entry_.specialization;
    entry_valid_ = true;
    return &entry_;
  }

  // Texture of a fetch constant: heap slot and, if it has to be uploaded, it is left in textures_a_upload_
  // with its data already prepared. Base level of 2D textures and cubemaps.
  void PrepareTexture(const uint32_t* f, uint32_t& slot, uint32_t& heap,
                       VkDeviceSize& bytes_upload, bool& sampling_point, uint64_t& valid_until,
                       uint32_t& width_host_out, uint32_t& height_host_out, float& exp_scale_out) {
    // FH1: exponent bias. The console multiplies what a fetch returns by 2^exp_adjust (fetch constant word 3, bits
    // 13-18, signed), and a resolve stores the picture multiplied by 2^exp_bias. Resolved images here hold the
    // picture as drawn, so both are applied when it is read.
    const int32_t exp_fetch = int32_t(f[3] << 13) >> 26;
    exp_scale_out = exp_bias_ ? std::ldexp(1.0f, exp_fetch) : 1.0f;
    slot = 0;
    heap = 0;
    width_host_out = 0;  // 0 = unknown (1/size is not written)
    height_host_out = 0;
    sampling_point = false;
    valid_until = frame_;  // by default, this frame only
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const bool cube = dimension == uint32_t(xenos::DataDimension::kCube);
    const bool volume = dimension == uint32_t(xenos::DataDimension::k3D);
    // FH1: a 1D texture is one row; the shader samples it as a 2D texture at v = 0.5 (tfetch1D).
    const bool one_d = dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cube && !volume;
    if (one_d && warnings_1d_ < 8) {
      ++warnings_1d_;
      REXLOG_INFO("[fh1] 1D texture {:08X} format {} width {} (fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  (f[1] >> 12) << 12, f[1] & 0x3F, (f[2] & 0xFFFFFF) + 1, f[0], f[1], f[2], f[3], f[4], f[5]);
    }
    heap = cube ? 2 : volume ? 1 : 0;
    const uint32_t layers = cube ? 6 : 1;
    if (const uint32_t signs = (f[0] >> 2) & 0xFF; signs && !(gamma_textures_ && gamma_targets_ && signs == 0x3F)) {
      Notify(31, "signed or gamma textures: read as unsigned");
    }
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = (f[1] >> 12) << 12;
    if (!cube && !volume) {
      if (const ImageNative* resolved = context_->TextureResolved(base & 0x1FFFFFFF)) {
        // Without this the shadows flicker.
        // With fh1_native_inv_tex_size the shader takes 1/size from the shared constants, and this path
        // used to return without reporting the size: 0 was written and the tfetch offsets (the taps of the
        // shadow map's PCF filter) all landed on the same texel. The filtering disappeared and the edge
        // shimmered as the camera moved. Resolved textures report their size like any other.
        width_host_out = resolved->width;
        height_host_out = resolved->height;
        // FH1: fetched narrower than the image, which took the resolve's pitch (see HintWidthResolved).
        if (const uint32_t width_fetch = (f[2] & 0x1FFF) + 1;
            !one_d && width_fetch < resolved->width && ((width_fetch + 31) & ~31u) == resolved->width) {
          context_->HintWidthResolved(base & 0x1FFFFFFF, width_fetch);
        }
        if (exp_bias_) {
          exp_scale_out = std::ldexp(1.0f, exp_fetch + resolved->exp_bias);
        }
        // A picture kept in a float image where the console has an unsigned format: negative = "cut negative
        // values and NaN to 0" (fh1Exp in shader_common.h).
        if (resolved->format == VK_FORMAT_R16G16B16A16_SFLOAT && resolved->exp_bias != 0) {
          exp_scale_out = ExpScaleFloatPicture(exp_scale_out, exp_bias_ ? exp_fetch : INT32_MIN);
        }
        // One trace per size, to check in the log that the 1/size constant of resolved textures is no longer
        // 0 (that was the cause of the shadow flicker).
        if (warnings_invsize_resolved_.insert(uint64_t(resolved->width) << 32 | resolved->height).second) {
          REXLOG_INFO("[native] C3: resolved texture {}x{}: 1/size = {:.6f}, {:.6f}",
                      resolved->width, resolved->height, 1.0f / float(resolved->width),
                      1.0f / float(resolved->height));
        }
        if (IsDepth(resolved->format)) {
          // FH1: fetched as a color format (k_8_8_8_8 by the motion-blur velocity and depth-of-field shaders), the
          // shader wants the bytes of the console's depth word, not the depth value.
          if (const uint32_t format_fetch = f[1] & 0x3F; format_fetch != 22 && format_fetch != 23) {
            if (const ImageNative* bytes = context_->TextureResolvedBytes(base & 0x1FFFFFFF)) {
              slot = SlotView(bytes->image, bytes->format, swizzle, kSwizzleRGBA);
              sampling_point = true;
              valid_until = frame_;
              return;
            }
          }
          // Depth copy (k_24_8): depth comes out in R and the fetch constant's swizzle distributes it. No
          // filtering: it is read as is.
          slot = SlotView(resolved->image, resolved->format, swizzle, kSwizzleRRRR);
          sampling_point = true;
          // This used to be UINT64_MAX, and that is what forced the cross-frame cache off.
          // A resolved texture is a render target the game rewrites every frame. Saying its descriptor slot is
          // valid "until the generation changes" meant that, with the CPU two frames ahead (3 work slots), an
          // old view was reused: that was the smeared race leaderboard that kept
          // fh1_native_texture_cache_across_frames at false for a while. The other paths are already tied
          // to texture.next and relax nothing; these two were the only ones without a horizon.
          valid_until = frame_;
          return;
        }
        // The emulation writes the copy to memory with copy_dest_swap and loads it as a texture: the fetch
        // constant's swizzle (ZYXW for 8888) undoes that swap. Here the copy is image to image and does not
        // swap channels, so the swap goes into the host channels (without this, Mia came out blue).
        // FH1: fetched with the gamma sign, a resolved 8-bit picture is decoded like any gamma texture.
        const bool srgb = gamma_targets_ && ((f[0] >> 2) & 0x3F) == 0x3F &&
                          resolved->format == VK_FORMAT_R8G8B8A8_UNORM && resolved->view_srgb != VK_NULL_HANDLE;
        slot = SlotView(resolved->image, srgb ? VK_FORMAT_R8G8B8A8_SRGB : resolved->format, swizzle,
                             resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA, 0, srgb);
        valid_until = frame_;  // see the long comment on the depth copy
        return;
      }
    }
    const uint32_t format = f[1] & 0x3F;
    FormatTexture tf;
    if (!FormatTextureOf(format, tf)) {
      Notify(400 + format, "texture format not supported yet: an empty one is used");
      return;
    }
    // FH1: a texture whose red, green and blue are fetched with the gamma sign (3) holds gamma-encoded color and the
    // console converts it to linear when sampling. Read raw, every such texture (most of the world and the HUD) came
    // out too bright and the whole picture looked washed out. The host's sRGB formats do that conversion (the
    // console's curve is piecewise-linear, close to sRGB). The signs are part of the texture key, so the same
    // memory fetched without them keeps its own plain image.
    // FH1 (fh1_native_gamma_pwl): the console's curve is not sRGB (it is brighter in the dark and middle tones),
    // so the texture stays in its plain format and the shader converts what it fetched (fh1Gamma in
    // shader_common.h), as the emulated GPU does. The mark is the exponent scale times 1.5. Only when red, green
    // and blue of the fetched value are the gamma components (or constants) and alpha is not.
    bool gamma_in_shader = false;
    if (gamma_textures_ && gamma_pwl_ && ((f[0] >> 2) & 0x3F) == 0x3F && exp_scale_out > 0.0f) {
      const uint32_t signs_all = (f[0] >> 2) & 0xFF;
      gamma_in_shader = true;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t source = (swizzle >> (3 * i)) & 0x7;
        if (source >= 4) {
          continue;  // constant 0 or 1: the curve leaves both as they are
        }
        const bool gamma_component = ((signs_all >> (2 * source)) & 0x3) == 3;
        if (gamma_component != (i < 3)) {
          gamma_in_shader = false;
        }
      }
      {
        const int32_t mask = REXCVAR_GET(fh1_native_gamma_pwl_mask);
        const uint32_t bit = tf.format == VK_FORMAT_R8G8B8A8_UNORM ? 1 : tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 2
                             : tf.format == VK_FORMAT_BC2_UNORM_BLOCK ? 4 : tf.format == VK_FORMAT_BC3_UNORM_BLOCK ? 8 : 16;
        if (!(uint32_t(mask) & bit)) {
          gamma_in_shader = false;
        }
        const uint64_t kind = (uint64_t(format) << 32) | (uint64_t(swizzle) << 12) | (signs_all << 1) | (gamma_in_shader ? 1 : 0);
        if (kinds_gamma_.insert(kind).second) {
          REXLOG_INFO("[fh1] gamma texture kind: format {} swizzle {:03X} signs {:02X} host format {} -> {} (first {:08X} "
                      "{}x{})", format, swizzle, signs_all, uint32_t(tf.format),
                      gamma_in_shader ? "console curve in the shader" : "host sRGB format", base, (f[2] & 0x1FFF) + 1,
                      ((f[2] >> 13) & 0x1FFF) + 1);
        }
      }
      if (gamma_in_shader) {
        exp_scale_out *= 1.5f;
      }
    }
    if (gamma_textures_ && !gamma_in_shader && ((f[0] >> 2) & 0x3F) == 0x3F) {
      switch (tf.format) {
        case VK_FORMAT_R8G8B8A8_UNORM: tf.format = VK_FORMAT_R8G8B8A8_SRGB; break;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: tf.format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; break;
        case VK_FORMAT_BC2_UNORM_BLOCK: tf.format = VK_FORMAT_BC2_SRGB_BLOCK; break;
        case VK_FORMAT_BC3_UNORM_BLOCK: tf.format = VK_FORMAT_BC3_SRGB_BLOCK; break;
        default: Notify(41, "gamma texture in a format without an sRGB twin: read raw"); break;
      }
    }
    if (REXCVAR_GET(fh1_debug_no_dxt) && tf.block > 1) {
      return;  // NFSC debug: compressed textures are replaced by the empty stand-in
    }
    // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h:1222-1233).
    const uint32_t width = volume ? (f[2] & 0x7FF) + 1 : one_d ? (f[2] & 0xFFFFFF) + 1 : (f[2] & 0x1FFF) + 1;
    const uint32_t height = volume ? ((f[2] >> 11) & 0x7FF) + 1 : one_d ? 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    if (one_d && width > 8192) {
      Notify(30, "1D texture wider than 8192: an empty one is used");
      return;
    }
    const uint32_t background = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
    if (volume && tf.block > 1) {
      Notify(38, "compressed 3D texture: an empty one is used");
      return;
    }
    const uint32_t blocks_x = (width + tf.block - 1) / tf.block;
    const uint32_t blocks_y = (height + tf.block - 1) / tf.block;
    const uint32_t pitch_blocks = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);
    // The faces of a cubemap are consecutive: each takes its base level with rows and columns aligned to
    // 32 blocks and the total to 4 KB (GetGuestTextureLayout, pipeline/texture/util.cpp:311-337).
    const uint64_t stride_face =
        (uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes *
             ((blocks_y + 31) & ~uint32_t(31)) +
         4095) &
        ~uint64_t(4095);
    const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t key = XXH3_64bits(keys, sizeof(keys));

    // Mip levels with the rules of GetSubresourcesFromFetchConstant (pipeline/texture/util.cpp:67-97): no
    // mip address means no mips; the maximum is clamped to the size; if the base is missing or the minimum
    // is above 0, the base is not read. 3D textures still use only the base.
    const bool tile_texture = (f[0] >> 31) & 0x1;
    const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
    const uint32_t level_packed =
        mipmaps_ && !volume && !one_d && ((f[5] >> 11) & 0x1) ? LevelPacked(width, height) : UINT32_MAX;
    uint32_t level_max = 0;
    bool read_base = true;
    if (mipmaps_ && !volume && !one_d && dir_mips != 0) {
      const uint32_t tam_max = Log2Floor(std::max(width, height));
      uint32_t level_min = std::min((f[4] >> 2) & 0xFu, tam_max);
      level_max = std::max(std::min((f[4] >> 6) & 0xFu, tam_max), level_min);
      if (level_max != 0) {
        if (dir_base == 0) {
          level_min = std::max(level_min, 1u);
        }
        read_base = level_min == 0;
      }
    }
    // Mip regions from dir_mips (GetGuestTextureLayout): each level stored with rows of 32 blocks computed
    // from the size rounded up to a power of 2 (and aligned to 256 bytes if the texture is linear) and
    // layers aligned to 4 KB; the levels of the packed tail share the region of the first of them.
    struct RegionMip {
      uint64_t displacement = 0;
      uint64_t row_bytes = 0;
      uint64_t stride = 0;
      uint32_t pitch_blocks = 0;
    };
    std::array<RegionMip, 16> regions{};
    uint64_t extension_mips = 0;
    if (level_max != 0) {
      const uint32_t last_2 = level_packed == 0 ? 0 : std::min(level_max, level_packed);
      for (uint32_t s = level_packed == 0 ? 0 : 1; s <= last_2; ++s) {
        RegionMip& region = regions[s];
        const uint32_t row_texels = std::max(std::bit_ceil(width) >> s, 1u);
        const uint32_t rows_texels = std::max(std::bit_ceil(height) >> s, 1u);
        region.pitch_blocks = ((row_texels + tf.block - 1) / tf.block + 31) & ~31u;
        region.row_bytes = uint64_t(region.pitch_blocks) * tf.bytes;
        if (!tile_texture) {
          region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
        }
        const uint64_t rows_blocks = uint64_t(((rows_texels + tf.block - 1) / tf.block + 31) & ~31u);
        region.stride = (region.row_bytes * rows_blocks + 4095) & ~uint64_t(4095);
        region.displacement = extension_mips;
        extension_mips += region.stride * layers;
      }
      if (dir_mips + extension_mips > kMemoryPhysical) {
        level_max = 0;  // the mips would go past memory: base only
        extension_mips = 0;
        read_base = true;
      }
    }
    // With the packed tail starting at level 0 (short side of 16 texels or less), the base does not start
    // at its address either (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl applies the same
    // offset to level 0).
    uint32_t base_ox = 0;
    uint32_t base_oy = 0;
    if (level_packed == 0) {
      OffsetPacked(width, height, tf.block, 0, base_ox, base_oy);
    }

    if (cube && copy_image_) {
      // The game's dynamic cubemap: C2 resolves its faces one by one to those same addresses. They are
      // copied to the cubemap's layers in the upload command buffer, which runs before the work one, so they
      // lag one frame behind.
      std::array<const ImageNative*, 6> faces{};
      bool resolved_2 = true;
      for (uint32_t c = 0; c < 6 && resolved_2; ++c) {
        faces[c] = context_->TextureResolved(
            uint32_t((uint64_t(base) + c * stride_face) & 0x1FFFFFFF));
        // FH1: the faces are float images when they come from the HDR target (fh1_native_resolved_hdr).
        resolved_2 = faces[c] && (faces[c]->format == VK_FORMAT_R8G8B8A8_UNORM ||
                                  faces[c]->format == VK_FORMAT_R16G16B16A16_SFLOAT) &&
                    faces[c]->format == faces[0]->format &&
                    faces[c]->width >= width && faces[c]->height >= height;
      }
      if (resolved_2) {
        const VkFormat format_faces = faces[0]->format;
        // FH1: the game also renders the cube map's smaller levels (each face of level n drawn from level n - 1
        // and resolved to its place after the mip address: 1C9F9000, 1CA59000, 1CA71000 ...). Chrome and paint
        // sample those blurred levels; with the first level alone the reflections were sharp and white.
        std::array<std::array<const ImageNative*, 6>, 16> faces_mip{};
        uint32_t levels_cube = 1;
        if (cube_levels_) {
          for (uint32_t s = 1; s <= level_max && s < 16; ++s) {
            bool whole = true;
            for (uint32_t c = 0; c < 6 && whole; ++c) {
              const ImageNative* face = context_->TextureResolved(
                  uint32_t((dir_mips + regions[s].displacement + c * regions[s].stride) & 0x1FFFFFFF));
              faces_mip[s][c] = face;
              whole = face && face->format == format_faces && face->width >= std::max(width >> s, 1u) &&
                      face->height >= std::max(height >> s, 1u);
            }
            if (!whole) {
              break;
            }
            levels_cube = s + 1;
          }
        }
        const uint64_t key_resolved =
            key ^ 0x9E3779B97F4A7C15ull ^ (uint64_t(format_faces) << 40) ^ (uint64_t(levels_cube) << 52);
        if (exp_bias_) {
          exp_scale_out = std::ldexp(1.0f, exp_fetch + faces[0]->exp_bias);
        }
        if (format_faces == VK_FORMAT_R16G16B16A16_SFLOAT && faces[0]->exp_bias != 0) {
          exp_scale_out = ExpScaleFloatPicture(exp_scale_out, exp_bias_ ? exp_fetch : INT32_MIN);
        }
        Texture& texture = textures_[key_resolved];
        if (texture.image.image == VK_NULL_HANDLE) {
          if (!CreateTexture(texture.image, format_faces, width, height, layers, 0, levels_cube)) {
            textures_.erase(key_resolved);
            Notify(32, "could not create a texture");
            return;
          }
          texture.layers = layers;
          texture.bytes = uint64_t(width) * height * (format_faces == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 8) * layers;
          bytes_textures_ += texture.bytes;
          REXLOG_INFO("[native] C3: cube {:08X} {}x{} with its faces resolved by C2, {} levels (fetch {:08X} {:08X} "
                      "{:08X} {:08X} {:08X} {:08X})",
                      base, width, height, levels_cube, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        if (texture.frame != frame_) {
          const VkCommandBuffer upload = context_->CommandsUpload();
          if (!upload) {
            return;
          }
          texture.frame = frame_;
          if (!texture.image.prepared) {
            Barrier(upload, texture.image.image, layers);
            texture.image.prepared = true;
          }
          for (uint32_t c = 0; c < 6; ++c) {
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c, 1};
            copy.extent = {width, height, 1};
            copy_image_(upload, faces[c]->image, VK_IMAGE_LAYOUT_GENERAL, texture.image.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            for (uint32_t s = 1; s < levels_cube; ++s) {
              copy.dstSubresource.mipLevel = s;
              copy.extent = {std::max(width >> s, 1u), std::max(height >> s, 1u), 1};
              copy_image_(upload, faces_mip[s][c]->image, VK_IMAGE_LAYOUT_GENERAL, texture.image.image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            }
          }
          // Rear-view mirror diagnostic: how many times the faces are copied to the cubemap per submission.
          ++cubes_refreshed_;
          const auto now = std::chrono::steady_clock::now();
          if (now - report_cubes_ >= std::chrono::seconds(10)) {
            // It used to be the last periodic line the ring wrote itself; it now goes to the report thread.
            FH1_REPORT_RING("[native] C3 cubes with C2 faces: {} refreshes in {} jobs",
                                 cubes_refreshed_ - cubes_refreshed_previous_,
                                 frame_ - frame_report_cubes_);
            report_cubes_ = now;
            cubes_refreshed_previous_ = cubes_refreshed_;
            frame_report_cubes_ = frame_;
          }
        }
        slot = SlotView(texture.image.image, format_faces, swizzle,
                             faces[0]->swap_rb ? kSwizzleBGRA : kSwizzleRGBA, heap);
        // The cubemap with the resolved faces must also report its size, or its 1/size stays at 0 and the
        // tfetch offsets are lost (see the branch above).
        width_host_out = texture.image.width;
        height_host_out = texture.image.height;
        return;
      }
    }

    Texture& texture = textures_[key];
    if (texture.image.image == VK_NULL_HANDLE) {
      // BC in Vulkan: the size is in whole blocks.
      const uint32_t width_host = tf.block > 1 ? (width + 3) & ~uint32_t(3) : width;
      const uint32_t height_host = tf.block > 1 ? (height + 3) & ~uint32_t(3) : height;
      // With its mip levels (those that fit in the host size).
      const uint32_t levels = std::min(level_max + 1, Log2Floor(std::max(width_host, height_host)) + 1);
      /*
       * The vkBindImageMemory goes to the bind thread if possible (fh1_native_texture_binding_thread) and,
       * otherwise, CreateTexture as usual. What the ring spends creating is timed, without its wait for the
       * thread (that is counted separately), for the [hitch] ring line and the 10 s report.
       */
      const auto before_create = std::chrono::steady_clock::now();
      const uint64_t wait_before_create = ns_wait_bindings_total_;
      measuring_creation_ = true;
      const bool created_3 = CreateTextureInThread(texture, tf.format, width_host, height_host, layers, background, levels) ||
                          CreateTexture(texture.image, tf.format, width_host, height_host, layers, background, levels);
      measuring_creation_ = false;
      {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before_create)
                                         .count());
        const uint64_t wait = ns_wait_bindings_total_ - wait_before_create;
        const uint64_t own = ns > wait ? ns - wait : 0;
        fh1::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
        ns_create_report_ += own;
      }
      if (!created_3) {
        textures_.erase(key);
        Notify(32, "could not create a texture");
        return;
      }
      texture.layers = layers;
      texture.background = background;
      texture.levels = levels;
      levels_mip_uploaded_ += levels - 1;
      // Measurement only: the shape without the address, to count, once its first hash is known, whether it
      // repeats the content of a live one (NoteContentTexture). If this image is being recreated, the
      // previous one stops counting. The words are the key's without the base and mip addresses and without
      // bit 11 of f[1], which the sampler sets: format, byte order, tiling, pitch, size, mip limits, dimension
      // and packed tail.
      if (DiagReuse()) {
        RemoveContentTexture(texture, key);
        const uint32_t shape[5] = {keys[0], f[1] & 0x7FF, keys[2], keys[3], (f[5] >> 9) & 0x7};
        texture.shape_content = XXH3_64bits(shape, sizeof(shape));
        texture.address = base;
        texture.content_by_measure = true;
      }
      for (uint32_t n = 0; n < levels; ++n) {
        texture.bytes += uint64_t((std::max(width_host >> n, 1u) + tf.block - 1) / tf.block) *
                         ((std::max(height_host >> n, 1u) + tf.block - 1) / tf.block) * tf.bytes * layers *
                         (background ? background : 1);
      }
      bytes_textures_ += texture.bytes;
      // Diagnostic: why the cache grows on the console. A new texture at an address that already had another
      // points to world zones reloaded at the same place; with the same address, format and size but a
      // different key, to key fields that change (mip limits, byte order...).
      {
        ++textures_created_;
        fh1::waits::g_textures_created.fetch_add(1, std::memory_order_relaxed);
        const uint64_t shape = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(format) << 40) ^
                                                                              (uint64_t(width) << 20) ^ height);
        if (created_by_address_[base]++ > 0) {
          ++created_in_address_view_;
        }
        const auto [it_shape, new_shape] = key_by_shape_.try_emplace(shape, key);
        if (!new_shape && it_shape->second != key) {
          ++created_same_shape_other_key_;
          it_shape->second = key;
          /*
           * In the main menu, ~100 textures per frame once came back with the same address, format and size and
           * a different key, and were created again every frame. This shows which key words change (the first
           * 40 times).
           */
          const auto [it_words, new_ones] = words_by_shape_.try_emplace(shape);
          /*
           * Key guard. The same five words cannot produce a different key: if one does, the key was not computed
           * from what its words say (as when XXH3 read keys[4] before it was written; see docs/nfsmw-nx/toolchain.md).
           */
          if (!new_ones && std::equal(std::begin(keys), std::end(keys), it_words->second.begin())) {
            const uint64_t times = ++keys_incoherent_;
            if (times == 1 || times == 10 || times == 100 || times == 1000 || times % 10000 == 0) {
              REXLOG_ERROR("[native] C3 INCOHERENT texture key ({} times): {:08X} {}x{} format {} with the same "
                           "five words as last time and a different key; the key does not come from its words",
                           times, base, width, height, format);
            }
          }
          if (!new_ones && warnings_other_key_ < 40) {
            ++warnings_other_key_;
            const auto& before = it_words->second;
            REXLOG_INFO("[native] C3 same texture with another key: {:08X} {}x{} format {} | before {:08X} {:08X} "
                        "{:08X} {:02X} {:06X} | now {:08X} {:08X} {:08X} {:02X} {:06X} | changed {:08X} {:08X} "
                        "{:08X} {:02X} {:06X}",
                        base, width, height, format, before[0], before[1], before[2], before[3], before[4], keys[0],
                        keys[1], keys[2], keys[3], keys[4], before[0] ^ keys[0], before[1] ^ keys[1],
                        before[2] ^ keys[2], before[3] ^ keys[3], before[4] ^ keys[4]);
          }
          std::copy(std::begin(keys), std::end(keys), it_words->second.begin());
        } else if (new_shape) {
          auto& words = words_by_shape_[shape];
          std::copy(std::begin(keys), std::end(keys), words.begin());
        }
        // From 256 to 1024. This line and the GPU memory line after it were the two longest stutters of the
        // "diagnostic dump" group: 147.6 ms and 107.5 ms of frame time on their own. With 1024 they
        // still appear once or twice per session, which is enough to see whether the duplicate key counter
        // spikes.
        if (textures_created_ % 1024 == 0) {
          REXLOG_INFO("[native] C3 cache diag: {} textures created ({} in the cache, {} MB): {} at an address that "
                      "already had another texture; {} with the same address, format and size as another but a "
                      "different key",
                      textures_created_, textures_.size(), bytes_textures_ >> 20, created_in_address_view_,
                      created_same_shape_other_key_);
          NoteMemoryOfTheGpu();
        }
      }
      if (textures_.size() <= 48) {
        REXLOG_INFO("[native] C3: texture {:08X} {}x{} format {} {} order {} pitch {} swizzle {:03X} signs "
                    "{:02X}{}; levels {} (mips at {:08X}, packed from {})",
                    base, width, height, format, ((f[0] >> 31) & 0x1) ? "tiled" : "linear",
                    (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                    cube ? " (cube)" : volume ? " (3D)" : "", levels, dir_mips,
                    level_packed == UINT32_MAX ? std::string("none") : std::to_string(level_packed));
      }
    }
    slot = SlotView(texture.image.image, tf.format, swizzle, tf.swizzle_host, heap);
    width_host_out = texture.image.width;  // the host's, which is what the shader sees
    height_host_out = texture.image.height;
    if (texture.frame == frame_) {
      // Valid until the frame before the next check (if it changed now, only this frame)
      valid_until = std::max<uint64_t>(frame_, texture.next ? texture.next - 1 : 0);
      return;  // already checked this frame
    }
    // Textures that do not change are checked less and less often (down to every 32 frames); those that
    // change (videos) go back to being checked every frame.
    if (texture.image.prepared && frame_ < texture.next) {
      valid_until = texture.next - 1;
      return;
    }
    texture.frame = frame_;
    // If its hash thread job is still uncollected (a buffer-full submission happened in the middle of its
    // draw), its fingerprint_raw is not valid yet: it is collected before checking, as if the ring had done
    // it.
    if (texture.fingerprint_work == kWorkFingerprintPublished) {
      CollectFingerprints();
    }
    // What it costs from here to the end (check, untile, prepare the upload).
    struct StopwatchTexture {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~StopwatchTexture() {
        fh1::waits::g_ns_textures.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                         .count()),
            std::memory_order_relaxed);
      }
    } stopwatch_texture;
    // 2D and cubemaps: before untiling, XXH3 of the guest bytes. If they have not changed there is nothing
    // to do (untiling and comparing every texture every frame took 62 of the 66 us of each menu draw).
    if (!volume) {
      const uint32_t log2_block = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                                : tf.bytes >= 2 ? 1 : 0;
      // Extent of a layer: tiled, GetTiledAddressUpperBound2D (pipeline/texture/util.cpp:461-486); linear,
      // up to the last block. Includes the packed base offset (base_ox, base_oy; 0 for the rest).
      const uint32_t blocks_x_read = blocks_x + base_ox;
      const uint32_t blocks_y_read = blocks_y + base_oy;
      const uint64_t extension_layer =
          ((f[0] >> 31) & 0x1)
              ? uint64_t(std::max<int64_t>(
                    OffsetTile2D(int32_t((blocks_x_read - 1) & ~31u),
                                            int32_t((blocks_y_read - 1) & ~31u), pitch_blocks,
                                            log2_block),
                    0)) +
                    (log2_block == 0   ? 0xA00u
                     : log2_block == 1 ? 0xC00u
                                        : (0x400u << log2_block))
              : uint64_t(std::max(pitch_blocks, blocks_x_read)) * tf.bytes * (blocks_y_read - 1) +
                    uint64_t(blocks_x_read) * tf.bytes;
      const uint64_t start_raw = uint64_t(base) & 0x1FFFFFFF;
      const uint64_t extension = (layers - 1) * stride_face + extension_layer;
      if (start_raw + extension <= kMemoryPhysical) {
        /*
         * Per-frame check budget.
         * During race stutters both game threads wait for room in the ring (20-50 ms) while the ring neither
         * stops nor waits for the GPU: the ring thread itself is stuck. During the stutters, that thread was in
         * PrepareTexture and in XXH3. Textures that arrive together (one zone) double their interval at the
         * same time (1, 2, 4... 32), so all of them are rechecked in the same frame. Here a stable texture
         * (interval 8 or more) whose check does not fit in the frame's budget is deferred a few frames, at
         * most kDeferralMax in a row. New textures, changing ones (videos) and just-uploaded ones are
         * never deferred.
         */
        /*
         * Sampled recheck of stable textures (fh1_native_fingerprint_sampling).
         * A stable texture used to go through XXH3 in full every 32-39 frames only to conclude, almost always,
         * that it had not changed: in the alley that was 0.2-5.3 ms per frame of the ring thread. Now every
         * full check from interval 4 on also computes the hash of a sample of the same memory (FingerprintSample:
         * first and last 4 KB block and 1 in 8, of the base and the mips) and, if the full hash matches, stores
         * it. Rechecks of stable textures compute only the sample: if it matches, the texture is taken as
         * unchanged; otherwise the usual full path follows. Textures where the sample would read more than half
         * the bytes (those with few blocks) work as before.
         * Self-checking guard: the first kSamplesACheck rechecks of stable textures compute both hashes
         * and count disagreements (same sample, different full hash). After that, 1 in N rechecks of each
         * texture is still full (N = the cvar), so a change the sample misses is caught at most N rechecks
         * later. A single disagreement turns sampling off for the rest of the session, with a warning.
         */
        if (sampling_fingerprints_ < 0) {
          const int32_t every = REXCVAR_GET(fh1_native_fingerprint_sampling);
          sampling_fingerprints_ = every >= 2 ? std::min<int32_t>(every, 64) : 0;
          REXLOG_INFO("[native] C3: sampled re-check of stable textures (fh1_native_fingerprint_sampling) = {}",
                      sampling_fingerprints_ ? fmt::format("YES, 1 in {} full; the first {} with both fingerprints",
                                                      sampling_fingerprints_, kSamplesACheck)
                                        : std::string("no, always the full fingerprint"));
        }
        const uint64_t bytes_complete = extension + extension_mips;
        const uint64_t bytes_of_sample = BytesSample(extension) + BytesSample(extension_mips);
        const bool with_sample = sampling_fingerprints_ > 0 && texture.image.prepared && texture.interval >= 4 &&
                                 bytes_of_sample * 2 <= bytes_complete;
        const bool sample_useful = with_sample && texture.interval >= 8 && texture.sample_valid;
        const bool only_sample = sample_useful && samples_checked_ >= kSamplesACheck &&
                                  texture.samples_consecutive + 1u < uint32_t(sampling_fingerprints_);
        const uint64_t bytes_sample = with_sample ? bytes_of_sample : 0;
        // What will actually be read, which is what counts against the budget.
        const uint64_t bytes_fingerprint = only_sample ? bytes_sample : bytes_complete + bytes_sample;
        if (frame_fingerprints_ != frame_) {
          frame_fingerprints_ = frame_;
          bytes_fingerprint_frame_ = 0;
        }
        if (budget_fingerprints_ < 0) {
          budget_fingerprints_ = std::max(REXCVAR_GET(fh1_native_fingerprint_kb_per_frame), 0);
          REXLOG_INFO("[native] C3: texture check budget = {} KB per frame{}",
                      budget_fingerprints_, budget_fingerprints_ ? "" : " (no limit)");
        }
        if (budget_fingerprints_ && texture.image.prepared && texture.interval >= 8 &&
            texture.deferrals < kDeferralMax && bytes_fingerprint_frame_ > 0 &&
            bytes_fingerprint_frame_ + bytes_fingerprint > uint64_t(budget_fingerprints_) * 1024) {
          ++texture.deferrals;
          texture.next = frame_ + 1 + (key & 1);
          valid_until = texture.next - 1;
          fh1::waits::g_fingerprints_deferred.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        texture.deferrals = 0;
        bytes_fingerprint_frame_ += bytes_fingerprint;
        fh1::waits::g_bytes_fingerprint.fetch_add(bytes_fingerprint, std::memory_order_relaxed);
        const auto start_fingerprint_raw = std::chrono::steady_clock::now();
        const uint8_t* const raw_base = memory_->TranslatePhysical(uint32_t(start_raw));
        const uint8_t* const raw_mips = extension_mips ? memory_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
        // The sample goes before the full hash. If the full one matches, the sample is of that same content,
        // and it is the one stored.
        uint64_t sample = 0;
        if (with_sample) {
          sample = FingerprintSample(raw_base, extension, 0);
          if (extension_mips) {
            sample = FingerprintSample(raw_mips, extension_mips, sample);
          }
          bytes_sample_ += bytes_sample;
          ReportSampling(start_fingerprint_raw);
        }
        const bool sample_equal = sample_useful && sample == texture.fingerprint_sample;
        if (only_sample && sample_equal) {
          fh1::waits::g_ns_fingerprint_raw.fetch_add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            start_fingerprint_raw)
                           .count()),
              std::memory_order_relaxed);
          ++texture.samples_consecutive;
          ++samples_hits_;
          bytes_saved_sample_ += bytes_complete - bytes_sample;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          return;
        }
        if (only_sample) {
          // The sample changed: now the full hash, which was not in this frame's budget.
          ++samples_different_;
          bytes_fingerprint_frame_ += bytes_complete;
          fh1::waits::g_bytes_fingerprint.fetch_add(bytes_complete, std::memory_order_relaxed);
        }
        /*
         * New texture -> hash thread (fh1_native_texture_fingerprint_thread; see PlanFingerprint).
         * The plan is the same ReadLevel calls the usual path makes further down, with the same arguments, but
         * reading from a copy of the bytes this hash covers. If any read falls outside them, or ReadLevel
         * would go past memory, the texture continues on the usual path.
         */
        // FH1: not k_DXT3A (58), whose data is widened after the read (below); the thread does not do that.
        if (!texture.image.prepared && fingerprints_phase_ != kFingerprintsOff && !diag_mips_ && tile_fast_ >= 0 &&
            format != 58) {
          std::array<size_t, 16> offsets{};
          std::array<size_t, 16> layer_level{};
          size_t bytes_plan = 0;
          for (uint32_t n = 0; n < texture.levels; ++n) {
            const uint32_t bx_n = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_n = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
            offsets[n] = bytes_plan;
            layer_level[n] = size_t(bx_n) * by_n * tf.bytes;
            bytes_plan += layer_level[n] * layers;
          }
          const uint32_t bx_base = (texture.image.width + tf.block - 1) / tf.block;
          const uint64_t mips_in_copy = (extension + 63) & ~uint64_t(63);  // where the copy of the mips starts
          uint32_t n_reads = 0;
          bool plan_ok = true;
          for (uint32_t c = 0; plan_ok && c < (read_base ? layers : 0u); ++c) {
            plan_ok = PlanReadFingerprint(reads_plan_, n_reads, dir_base + c * stride_face, tile_texture,
                                           pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                                           base_ox, base_oy, blocks_x, blocks_y, bx_base, layer_level[0] * c,
                                           start_raw, extension, 0);
          }
          for (uint32_t n = 1; plan_ok && n < texture.levels; ++n) {
            const uint32_t s = level_packed == 0 ? 0 : std::min(n, level_packed);
            uint32_t ox = 0;
            uint32_t oy = 0;
            if (n >= level_packed) {
              OffsetPacked(width, height, tf.block, n, ox, oy);
            }
            const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
            for (uint32_t c = 0; plan_ok && c < layers; ++c) {
              plan_ok = PlanReadFingerprint(reads_plan_, n_reads,
                                             dir_mips + regions[s].displacement + c * regions[s].stride,
                                             tile_texture, regions[s].pitch_blocks, regions[s].row_bytes, tf,
                                             ox, oy, std::min(bx_guest, bx_host), std::min(by_guest, by_host),
                                             bx_host, offsets[n] + layer_level[n] * c, dir_mips, extension_mips,
                                             mips_in_copy);
            }
          }
          if (plan_ok && PlanFingerprint(texture, key, n_reads, raw_base, extension, raw_mips, extension_mips,
                                       tf, (f[1] >> 6) & 0x3, bytes_plan, offsets, base, width, height,
                                       format, bytes_upload)) {
            return;  // applying phase: the thread prepares it; the tail of this function is already done
          }
        }
        uint64_t fingerprint_raw = XXH3_64bits(raw_base, size_t(extension));
        if (extension_mips) {  // and the bytes of all the mips
          fingerprint_raw = XXH3_64bits_withSeed(raw_mips, size_t(extension_mips), fingerprint_raw);
        }
        fh1::waits::g_ns_fingerprint_raw.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          start_fingerprint_raw)
                         .count()),
            std::memory_order_relaxed);
        const bool complete_equal = texture.image.prepared && fingerprint_raw == texture.fingerprint_raw;
        if (sample_useful && !only_sample) {
          // The guard. This stable recheck computed both hashes.
          ++samples_checked_;
          ++samples_with_the_two_;
          if (sample_equal && !complete_equal) {
            REXLOG_ERROR("[native] C3: the SAMPLE of texture {:08X} {}x{} format {} ({} KB of base and {} KB of "
                         "mips) did not see a change that the full fingerprint did. Fingerprint sampling is turned "
                         "OFF (after {} re-checks with both).",
                         uint32_t(start_raw), width, height, format, extension >> 10, extension_mips >> 10,
                         samples_checked_);
            sampling_fingerprints_ = 0;
          } else if (samples_checked_ == kSamplesACheck) {
            REXLOG_INFO("[native] C3: fingerprint sampling: {} re-checks with both fingerprints, no disagreement. "
                        "Sampling continues, with 1 in {} full.",
                        samples_checked_, sampling_fingerprints_);
          }
        }
        if (complete_equal) {
          // The sample of this content, for the stable rechecks.
          if (with_sample && sampling_fingerprints_ > 0) {
            texture.fingerprint_sample = sample;
            texture.sample_valid = true;
          }
          texture.samples_consecutive = 0;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          return;
        }
        texture.sample_valid = false;  // the content changed; the sample is no longer valid
        texture.samples_consecutive = 0;
        texture.fingerprint_raw = fingerprint_raw;
        NoteContentTexture(texture, key);  // measurement only: fh1_native_diag_reuse
      }
    }
    // Base level of each layer, untiled and in host byte order.
    const uint32_t blocks_x_host = (texture.image.width + tf.block - 1) / tf.block;
    const uint32_t blocks_y_host = (texture.image.height + tf.block - 1) / tf.block;
    const size_t bytes_layer = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    std::vector<uint8_t>& data = temporal_;
    // All levels back to back, each one layer after layer (UploadTexture copies them one by one).
    std::array<size_t, 16> bytes_layer_level{};
    size_t bytes_data = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      texture.offset_level[n] = uint32_t(bytes_data);
      bytes_layer_level[n] = size_t(bx_host) * by_host * tf.bytes;
      bytes_data += bytes_layer_level[n] * layers * (background ? background : 1);
    }
    data.assign(bytes_data, 0);
    const bool tile = (f[0] >> 31) & 0x1;
    if (volume && !ReadVolume(f, tf, blocks_x, blocks_y, background, blocks_x_host,
                                blocks_y_host, pitch_blocks, tile, data)) {
      Notify(33, "texture outside memory");
      if (!texture.image.prepared) {
        slot = 0;  // the image remains uninitialized
      }
      return;
    }
    // Base level: not read if not needed (minimum level 1; it stays zeroed and the sampler never goes below
    // 1).
    for (uint32_t c = 0; c < (volume || !read_base ? 0u : layers); ++c) {
      const uint64_t address = dir_base + c * stride_face;
      if (!ReadLevel(address, tile, pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                     base_ox, base_oy, blocks_x, blocks_y, blocks_x_host, data.data() + bytes_layer * c)) {
        Notify(33, "texture outside memory");
        if (!texture.image.prepared) {
          slot = 0;  // the image remains uninitialized
        }
        return;
      }
    }
    // Mip levels. Those in the packed tail share a region and are located with OffsetPacked;
    // the rest, each in its own.
    for (uint32_t n = 1; n < texture.levels; ++n) {
      const uint32_t s = level_packed == 0 ? 0 : std::min(n, level_packed);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= level_packed) {
        OffsetPacked(width, height, tf.block, n, ox, oy);
      }
      const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      for (uint32_t c = 0; c < layers; ++c) {
        const uint64_t address = dir_mips + regions[s].displacement + c * regions[s].stride;
        if (!ReadLevel(address, tile, regions[s].pitch_blocks, regions[s].row_bytes, tf, ox, oy,
                       std::min(bx_guest, bx_host), std::min(by_guest, by_host), bx_host,
                       data.data() + texture.offset_level[n] + bytes_layer_level[n] * c)) {
          Notify(39, "mip level outside memory: left at zero");
          break;
        }
      }
    }
    const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    // In one go (ChangeOrderBytes), with the same guard as the fast untiling.
    if (tile_fast_ > 0 && order != xenos::Endian::kNone && (tf.unit_order == 2 || tf.unit_order == 4)) {
      // The first 200, then 1 in 64. Checking every texture, this guard never got to switch itself off (there
      // were only 1,024 textures) and cost 2.1 ms per MB on every new texture.
      ++commands_seen_;
      const bool should_check = commands_seen_ <= kLevelsACheck || commands_seen_ % kCheckOneOfEvery == 0;
      if (should_check) {
        check_tile_ = data;
      }
      ChangeOrderBytes(data.data(), data.size(), tf.unit_order, uint32_t(order));
      if (should_check) {
        ++commands_checked_;
        OrderOfAlways(check_tile_, tf.unit_order, order);
        if (check_tile_ != data) {
          REXLOG_ERROR("[native] C3: the fast byte swap does NOT match (unit {}, order {}, {} bytes). The fast "
                       "byte swap and untiling are turned OFF.",
                       tf.unit_order, uint32_t(order), data.size());
          data.swap(check_tile_);
          tile_fast_ = 0;
        } else if (commands_checked_ == kLevelsACheck) {
          REXLOG_INFO("[native] C3: fast byte swap: {} textures checked against the usual one, all equal.",
                      commands_checked_);
        }
      }
    } else {
      OrderOfAlways(data, tf.unit_order, order);
    }
    if (format == 58) {  // FH1: k_DXT3A blocks (8 bytes) become BC2 blocks (16): alpha half, then an empty color half
      std::vector<uint8_t> wide(data.size() * 2, 0);
      for (size_t i = 0; i + 8 <= data.size(); i += 8) {
        std::memcpy(wide.data() + i * 2, data.data() + i, 8);
      }
      data.swap(wide);
      for (uint32_t n = 0; n < texture.levels; ++n) {
        texture.offset_level[n] *= 2;
      }
    } else if (diag_mips_ && texture.levels > 1 && read_base && !volume) {
      ReviewMips(base, format, width, height, tf, texture, bytes_layer_level, data);
    }
    /*
     * For a new (unprepared) texture this hash decides nothing: it is uploaded regardless. 0 is stored, and
     * the first time its memory changes it will be computed and uploaded (at most, one extra upload).
     */
    const auto start_fingerprint_data = std::chrono::steady_clock::now();
    const uint64_t fingerprint = texture.image.prepared ? XXH3_64bits(data.data(), data.size()) : 0;
    fh1::waits::g_ns_fingerprint_data.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                      start_fingerprint_data)
                     .count()),
        std::memory_order_relaxed);
    if (texture.image.prepared && fingerprint == texture.fingerprint) {
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      return;
    }
    // Observing phase of the hash thread: what the usual path produces, to compare when collecting.
    if (comparison_texture_ == &texture) {
      NoteComparisonFingerprint(texture, data);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.fingerprint = fingerprint;
    texture.data.swap(data);
    texture.needs_upload = true;
    textures_a_upload_.push_back(&texture);
    fh1::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    fh1::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
    bytes_upload += (texture.data.size() + 3) & ~size_t(3);
  }

  // --- Measurement only: new textures with the content of another (fh1_native_diag_reuse) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFramesWithoutUseForRelease frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. live_by_content_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepareTexture, and CollectFingerprints (hash thread);
   *   - when the texture is released: ReleaseImages, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReuseLookMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image and no pending hash thread job), so a
   * stale entry cannot count.
   */
  static constexpr uint32_t kReuseLookMax = 8;  // candidates checked per new texture
  static constexpr uint32_t kReuseDetails = 8;  // lines detailing the first matches

  bool DiagReuse() {
    if (diag_reuse_ < 0) {
      diag_reuse_ = REXCVAR_GET(fh1_native_diag_reuse) ? 1 : 0;
      REXLOG_INFO("[native] C3: measuring new textures with the content of another live one "
                  "(fh1_native_diag_reuse) = {}",
                  diag_reuse_ ? "YES (counting only: changes nothing)" : "no");
    }
    return diag_reuse_ > 0;
  }

  // The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
  static uint64_t KeyContent(const Texture& texture) {
    const uint64_t shape[4] = {texture.shape_content, (uint64_t(texture.image.format) << 32) | texture.levels,
                               (uint64_t(texture.image.width) << 32) | texture.image.height, texture.layers};
    return XXH3_64bits_withSeed(shape, sizeof(shape), texture.fingerprint_raw) | 1;
  }

  // Stops counting a texture (when it is released, when its image is recreated, or before recording it
  // with another hash).
  void RemoveContentTexture(Texture& texture, uint64_t key) {
    if (!texture.key_content) {
      return;
    }
    const auto [since, until] = live_by_content_.equal_range(texture.key_content);
    for (auto it = since; it != until; ++it) {
      if (it->second == key) {
        live_by_content_.erase(it);
        break;
      }
    }
    if (live_by_content_.find(texture.key_content) == live_by_content_.end() && contents_different_) {
      --contents_different_;
    }
    texture.key_content = 0;
  }

  // With the raw hash just set. If the texture is new, counts whether another live one has the same
  // content and shape, and whether that other one is cold. Then records it under its content key. Ring
  // thread only.
  void NoteContentTexture(Texture& texture, uint64_t key) {
    if (!DiagReuse()) {
      return;
    }
    RemoveContentTexture(texture, key);
    const uint64_t content = KeyContent(texture);
    if (texture.content_by_measure) {
      texture.content_by_measure = false;
      ++reuse_new_;
      reuse_bytes_new_ += texture.bytes;
      const Texture* equal = nullptr;
      const Texture* cold = nullptr;
      uint32_t looked = 0;
      const auto [since, until] = live_by_content_.equal_range(content);
      for (auto it = since; it != until; ++it) {
        if (looked++ >= kReuseLookMax) {
          ++reuse_without_look_;
          break;
        }
        const auto other_3 = textures_.find(it->second);
        if (other_3 == textures_.end() || &other_3->second == &texture) {
          continue;
        }
        const Texture& t = other_3->second;
        if (t.key_content != content || t.image.image == VK_NULL_HANDLE || !t.image.prepared ||
            t.fingerprint_work) {
          continue;
        }
        equal = &t;
        if (t.frame != UINT64_MAX && t.frame + kFramesWithoutUseForRelease < frame_) {
          cold = &t;
          break;  // one cold one is enough
        }
      }
      if (equal) {
        ++reuse_match_;
        reuse_bytes_match_ += texture.bytes;
      }
      if (cold) {
        ++reuse_cold_;
        reuse_bytes_cold_ += texture.bytes;
      }
      if (equal && reuse_details_ < kReuseDetails) {
        ++reuse_details_;
        const Texture& t = cold ? *cold : *equal;
        FH1_REPORT_RING("[native] C3 reuse by content (build 186, measurement only): the new texture {:08X} "
                         "({}x{}, VkFormat {}, {} levels, {} layers, {} KB) has the same content and shape as "
                         "{:08X}, {}",
                             texture.address, texture.image.width, texture.image.height,
                             uint32_t(texture.image.format), texture.levels, texture.layers, texture.bytes >> 10,
                             t.address,
                             cold ? fmt::format("not checked for {} frames", frame_ - t.frame)
                                  : std::string("which is still in use"));
      }
    }
    if (live_by_content_.find(content) == live_by_content_.end()) {
      ++contents_different_;
    }
    live_by_content_.emplace(content, key);
    texture.key_content = content;
  }

  // Every 10 s, if there were new textures with a hash. The cache snapshot (recorded, distinct contents)
  // is O(1).
  void ReportReuse() {
    if (diag_reuse_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_reuse_ < std::chrono::seconds(10)) {
      return;
    }
    report_reuse_ = now;
    if (reuse_new_) {
      const uint64_t pointed = uint64_t(live_by_content_.size());
      FH1_REPORT_RING(
          "[native] C3 reuse by content (build 186, measurement only), last 10 s: {} new textures with a "
          "fingerprint ({:.1f} MB); {} with the same content and shape as another live one ({:.1f} MB), {} of them "
          "with one not checked for more than {} frames ({:.1f} MB); {} with more than {} equal ones without "
          "looking at them whole | in the cache: {} textures with a fingerprint in {} different contents ({} "
          "repeated)",
          reuse_new_, double(reuse_bytes_new_) / 1048576.0, reuse_match_,
          double(reuse_bytes_match_) / 1048576.0, reuse_cold_, kFramesWithoutUseForRelease,
          double(reuse_bytes_cold_) / 1048576.0, reuse_without_look_, kReuseLookMax, pointed,
          contents_different_, pointed - std::min(pointed, contents_different_));
    }
    reuse_new_ = 0;
    reuse_match_ = 0;
    reuse_cold_ = 0;
    reuse_without_look_ = 0;
    reuse_bytes_new_ = 0;
    reuse_bytes_match_ = 0;
    reuse_bytes_cold_ = 0;
  }

  // Every 30 s, one line with what hash sampling has done (see PrepareTexture). The ring thread writes
  // it, but it is a single short line every 30 s.
  void ReportSampling(std::chrono::steady_clock::time_point now) {
    if (now - report_sampling_ < std::chrono::seconds(30)) {
      return;
    }
    if (report_sampling_ != std::chrono::steady_clock::time_point{}) {
      FH1_REPORT_RING("[native] C3 fingerprint sampling: {} re-checks solved with the sample ({:.1f} MB read and "
                       "{:.1f} MB not read), {} with the sample changed and {} with both fingerprints since the "
                       "previous line; {} with both since the start",
                  samples_hits_, double(bytes_sample_) / 1048576.0,
                  double(bytes_saved_sample_) / 1048576.0, samples_different_, samples_with_the_two_,
                  samples_checked_);
    }
    report_sampling_ = now;
    samples_hits_ = 0;
    samples_different_ = 0;
    samples_with_the_two_ = 0;
    bytes_sample_ = 0;
    bytes_saved_sample_ = 0;
  }

  // fh1_native_diag_mips: average color of each level of the first layer against the base's, for
  // DXT1/3/5 (average of the two colors of each block, in RGB565) and 8888. Only levels of 2x2 blocks or
  // more: the small ones are too little data for an average. Counts the textures with any level more than
  // 32/255 away from the base in any channel and logs the first 12 with their averages.
  void ReviewMips(uint32_t base, uint32_t format, uint32_t width, uint32_t height, const FormatTexture& tf,
                   const Texture& texture, const std::array<size_t, 16>& bytes_layer_level,
                   const std::vector<uint8_t>& data) {
    const bool dxt = tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || tf.format == VK_FORMAT_BC2_UNORM_BLOCK ||
                     tf.format == VK_FORMAT_BC3_UNORM_BLOCK;
    if (!dxt && tf.format != VK_FORMAT_R8G8B8A8_UNORM) {
      return;
    }
    const auto average = [&](uint32_t n, std::array<double, 3>& rgb) {
      const size_t start = texture.offset_level[n];
      const size_t bytes = bytes_layer_level[n];
      if (start + bytes > data.size() || bytes < 4u * tf.bytes) {
        return false;
      }
      rgb = {0.0, 0.0, 0.0};
      size_t count = 0;
      for (size_t i = 0; i + tf.bytes <= bytes; i += tf.bytes) {
        const uint8_t* p = data.data() + start + i;
        if (dxt) {
          const uint8_t* color = tf.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? p : p + 8;
          for (uint32_t k = 0; k < 2; ++k) {
            const uint16_t v = uint16_t(color[2 * k] | (color[2 * k + 1] << 8));
            rgb[0] += ((v >> 11) & 31) * (255.0 / 31.0);
            rgb[1] += ((v >> 5) & 63) * (255.0 / 63.0);
            rgb[2] += (v & 31) * (255.0 / 31.0);
          }
          count += 2;
        } else {
          rgb[0] += p[0];
          rgb[1] += p[1];
          rgb[2] += p[2];
          count += 1;
        }
      }
      for (double& c : rgb) {
        c /= double(count);
      }
      return true;
    };
    std::array<double, 3> base_rgb{};
    if (!average(0, base_rgb)) {
      return;
    }
    ++mips_reviewed_;
    std::string levels = fmt::format("[0]={:.0f}/{:.0f}/{:.0f}", base_rgb[0], base_rgb[1], base_rgb[2]);
    double worst = 0.0;
    for (uint32_t n = 1; n < texture.levels; ++n) {
      std::array<double, 3> rgb{};
      if (!average(n, rgb)) {
        break;
      }
      for (uint32_t c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(rgb[c] - base_rgb[c]));
      }
      levels += fmt::format(" [{}]={:.0f}/{:.0f}/{:.0f}", n, rgb[0], rgb[1], rgb[2]);
    }
    if (worst > 32.0) {
      if (++mips_rare_ <= 12) {
        REXLOG_WARN("[native] C3 mips diag: texture {:08X} {}x{} format {} with {} levels: one level is {:.0f} "
                    "away from the base; RGB averages {}",
                    base, width, height, format, texture.levels, worst, levels);
      }
    }
    if (mips_reviewed_ % 200 == 0) {
      REXLOG_INFO("[native] C3 mips diag: {} textures reviewed, {} with some level more than 32/255 from the base",
                  mips_reviewed_, mips_rare_);
    }
  }

  // The blocks of one level (one layer) of a 2D texture or cubemap, from block (ox, oy) of its guest
  // region (the packed tail or the base of a small texture do not start at 0), to the host destination
  // bx_host blocks wide. Linear: row_bytes per row; tiled: pitch in blocks (GetTiledOffset2D). false if
  // it goes past memory.
  bool ReadLevel(uint64_t address, bool tile, uint32_t pitch_blocks, uint64_t row_bytes,
                 const FormatTexture& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* target) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t end =
        tile ? uint64_t(std::max<int64_t>(OffsetTile2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u),
                                                                     pitch_blocks, log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + end > kMemoryPhysical) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    if (!tile) {
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(target + size_t(y) * bx_host * tf.bytes,
                    source + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      }
      return true;
    }
    // The fast untiling (UntileLevel) when the block is 1, 2, 4, 8 or 16 bytes.
    if (tile_fast_ < 0) {
      tile_fast_ = REXCVAR_GET(fh1_native_fast_untile) ? 1 : 0;
      REXLOG_INFO("[native] C3: fast untiling (fh1_native_fast_untile) = {}",
                  tile_fast_ ? "YES, checking the first levels against the usual one" : "no");
    }
    if (tile_fast_ && tf.bytes == (1u << log2)) {
      switch (log2) {
        case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      }
      /*
       * Guard: the first kLevelsACheck levels are repeated on the usual path and compared byte by byte.
       * If one differs, the usual result is kept, a warning is logged and the fast path is off for the rest
       * of the session.
       */
      // The first 200, then 1 in 64 (checking every level, the guard cost 4.2 ms per MB).
      ++levels_seen_;
      if (levels_seen_ <= kLevelsACheck || levels_seen_ % kCheckOneOfEvery == 0) {
        ++levels_checked_;
        check_tile_.assign(size_t(by) * bx_host * tf.bytes, 0);
        for (uint32_t y = 0; y < by; ++y) {
          std::memcpy(check_tile_.data() + size_t(y) * bx_host * tf.bytes,
                      target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
        }
        ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
        bool equal_2 = true;
        for (uint32_t y = 0; y < by && equal_2; ++y) {
          equal_2 = std::memcmp(check_tile_.data() + size_t(y) * bx_host * tf.bytes,
                                target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
        }
        if (!equal_2) {
          REXLOG_ERROR("[native] C3: the fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, from "
                       "{},{}). It is turned OFF and the usual one is used.",
                       bx, by, tf.bytes, pitch_blocks, ox, oy);
          tile_fast_ = 0;
        } else if (levels_checked_ == kLevelsACheck) {
          REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the usual one, all equal. Staying "
                      "with the fast one.",
                      levels_checked_);
        }
      }
      return true;
    }
    ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
    return true;
  }

  // The original byte swap, word by word.
  static void OrderOfAlways(std::vector<uint8_t>& data, uint32_t unit, xenos::Endian order) {
    if (unit == 2 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 1 < data.size(); i += 2) {
        uint16_t v;
        std::memcpy(&v, data.data() + i, 2);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 2);
      }
    } else if (unit == 4 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 3 < data.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, data.data() + i, 4);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 4);
      }
    }
  }

  // The original untiling, block by block.
  static void ReadLevelTileOfAlways(const uint8_t* source, uint32_t pitch_blocks, uint32_t log2, uint32_t bytes,
                                        uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                        uint8_t* target) {
    for (uint32_t y = 0; y < by; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t displacement =
            OffsetTile2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2);
        std::memcpy(target + (size_t(y) * bx_host + x) * bytes, source + displacement, bytes);
      }
    }
  }

  // Base level of a 3D texture, slice by slice (z, then y, then x), in the order vkCmdCopyBufferToImage
  // uploads it. Tiled: GetTiledOffset3D; linear: consecutive slices with rows and columns aligned to 32
  // blocks (GetGuestTextureLayout).
  bool ReadVolume(const uint32_t* f, const FormatTexture& tf, uint32_t blocks_x,
                   uint32_t blocks_y, uint32_t background, uint32_t blocks_x_host,
                   uint32_t blocks_y_host, uint32_t pitch_blocks, bool tile,
                   std::vector<uint8_t>& data) {
    const uint64_t address = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                             : tf.bytes >= 2 ? 1 : 0;
    const uint64_t row = uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t cut = row * ((blocks_y + 31) & ~uint32_t(31));
    const uint64_t end =
        tile ? uint64_t(std::max<int64_t>(
                      OffsetTile3D(int32_t((blocks_x + 31) & ~31u),
                                              int32_t((blocks_y + 31) & ~31u),
                                              int32_t((background + 3) & ~3u), pitch_blocks,
                                              blocks_y, log2),
                      0))
                : cut * background;
    if (address + end > kMemoryPhysical) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    const size_t bytes_cut = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    for (uint32_t z = 0; z < background; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint64_t displacement =
              tile ? uint64_t(OffsetTile3D(int32_t(x), int32_t(y), int32_t(z),
                                                         pitch_blocks, blocks_y, log2))
                      : z * cut + y * row + uint64_t(x) * tf.bytes;
          std::memcpy(data.data() + bytes_cut * z + (size_t(y) * blocks_x_host + x) * tf.bytes,
                      source + displacement, tf.bytes);
        }
      }
    }
    return true;
  }

  // A texture's VkImageCreateInfo, factored out so CreateTexture and CreateTextureInThread create exactly the
  // same image.
  static VkImageCreateInfo InfoImageTexture(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                             uint32_t background, uint32_t levels) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = background ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = format;
    info.extent = {width, height, background ? background : 1};
    info.mipLevels = std::max(levels, 1u);
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
  }

  // `emergency` = false only from CollectBindings. Half the cache cannot be released there: that waits
  // for the GPU by submitting the work, and we are inside BeforeOfSend. Out of memory: false, and the
  // texture is recreated later.
  bool CreateTexture(ImageNative& image, VkFormat format, uint32_t width, uint32_t height,
                    uint32_t layers = 1, uint32_t background = 0, uint32_t levels = 1, bool emergency = true) {
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, background, levels);
    /*
     * The pool first, and the usual path only if it does not fit.
     *
     * A dedicated allocation per texture costs 1.9 ms of CPU on Horizon, measured inside the ioctls:
     * nvMapCreate 422 us, plus two GPU address reservations (127 us each) and two mappings (626 us each).
     * There are two of each because the dedicated allocation makes its own address and mapping, and then
     * nvk_image_plane_bind makes others for the plane. And the only thing the dedicated allocation buys is
     * compression, which our textures never use (nvk_image.c:843-846 returns false with
     * SAMPLED|TRANSFER_DST). So we paid double and got nothing.
     *
     * Sub-allocating from large slabs removes the allocation's address and mapping: 1.9 -> 0.75 ms. For a
     * burst of 15 textures in one frame, from 28.5 to 11.3 ms. That is what shows when entering a new zone
     * of the map.
     *
     * It does not change a single pixel: same tiling, same format, same pte_kind. Binding at an offset
     * keeps the block-linear layout (nvk_image.c:1700-1710) and is the normal path of any sub-allocator in
     * NVK.
     */
    bool reserved = false;
    if (pool_textures_.Active()) {
      VkImage image_pool = VK_NULL_HANDLE;
      if (dfn_.vkCreateImage(device_, &info, nullptr, &image_pool) == VK_SUCCESS) {
        VkMemoryRequirements req{};
        dfn_.vkGetImageMemoryRequirements(device_, image_pool, &req);
        VkDeviceMemory block = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint32_t id_block = 0xFFFFFFFFu;
        if (pool_textures_.Reserve(req, block, offset, id_block) &&
            dfn_.vkBindImageMemory(device_, image_pool, block, offset) == VK_SUCCESS) {
          image.image = image_pool;
          image.memory_block = VK_NULL_HANDLE;  // from the pool: not freed on its own
          image.pool_block = id_block;
          reserved = true;
        } else {
          if (id_block != 0xFFFFFFFFu) {
            pool_textures_.Free(id_block);
          }
          dfn_.vkDestroyImage(device_, image_pool, nullptr);
          pool_textures_.NoteDedicated();
        }
      }
    }
    if (!reserved) {
      image.pool_block = 0xFFFFFFFFu;
      reserved = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
          image.memory_block);
    }
    if (reserved && test_without_memory_every_ > 0 &&
        ++reserves_of_texture_ % uint64_t(test_without_memory_every_) == 0) {
      // Test: undo the successful allocation and continue as if it had failed.
      DestroyImage(image);
      reserved = false;
      REXLOG_INFO("[native] C3: test, faking out of memory on allocation {}", reserves_of_texture_);
    }
    if (!reserved) {
      // Out of GPU memory. Instead of giving up (which ends in a black screen), half the cache is released
      // with the GPU idle and the allocation is retried once.
      // The retry always takes the dedicated path, on purpose. Getting here means memory is short, and the
      // dedicated path is the one that can ask the system for more; the pool only hands out what it already
      // has. pool_block is left invalid so DestroyImage does not touch the pool.
      image.pool_block = 0xFFFFFFFFu;
      if (!emergency || !ReleaseTexturesByMissingOfMemory() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
              image.memory_block)) {
        REXLOG_ERROR("[native] C3: out of GPU memory for a {}x{} texture ({} levels, {} layers) and {}; {} "
                     "textures and {} MB remain",
                     width, height, std::max(levels, 1u), layers,
                     emergency ? "releasing the cache was not enough"
                                : "without releasing the cache (from CollectBindings; recreated at its next check)",
                     textures_.size(), bytes_textures_ >> 20);
        return false;
      }
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  bool UploadTexture(Texture& texture) {
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload || !texture.needs_upload) {
      return upload != VK_NULL_HANDLE;
    }
    VkDeviceSize offset;
    /*
     * If the hash thread prepares it, its data does not exist yet: its space is reserved (same size, same
     * point) and the thread writes it there; CollectFingerprints waits for it before submitting. The barrier
     * and the copy are recorded below as usual: the GPU does not read the upload buffer until
     * vkQueueSubmit.
     */
    if (texture.fingerprint_work && texture.fingerprint_work != kWorkFingerprintPublished) {
      const size_t bytes = BytesFingerprintPlanned(texture);
      Reserve(bytes, 16, offset);  // BC: offset multiple of the block
      PublishFingerprint(texture, upload_data_ + offset, bytes);
    } else {
      Reserve(texture.data.size(), 16, offset);  // BC: offset multiple of the block
      std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
    }
    /*
     * If its vkBindImageMemory is still on the bind thread, neither the barrier nor the copy can be
     * recorded for the image (it has no memory). The data is already in the upload buffer; the barrier and
     * the copy are recorded in CollectBindings, in this same upload buffer before it is closed
     * (BeforeOfSend).
     */
    if (texture.in_flight && !DeferCopy(texture, offset, upload)) {
      CollectBindings(true);  // inconsistent index (DeferCopy already logged it and switched off): collect everything here
    }
    if (!texture.in_flight && texture.image.image != VK_NULL_HANDLE) {
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, offset);
    }
    texture.needs_upload = false;
    /*
     * The copy of the pixels is no longer needed. They are already in the upload buffer, and to know
     * whether the texture changes its hashes are kept, not the bytes. Keeping the copy made the texture
     * cache take the same space again in CPU RAM (150-680 MB). That once ended in std::bad_alloc in the
     * main menu, with the cache at 682 MB.
     */
    // The larger buffer is kept as temporal_ (so the next new texture does not ask the system for memory
    // again or touch fresh pages); the other is released.
    if (texture.data.capacity() > temporal_.capacity()) {
      temporal_.swap(texture.data);
    }
    std::vector<uint8_t>().swap(texture.data);
    ++uploads_texture_;
    return true;
  }

  // One range per mip level (with all its layers). Factored out for UploadTexture and for the deferred
  // copies of CollectBindings.
  void RecordCopyTexture(VkCommandBuffer upload, const Texture& texture, VkDeviceSize offset) {
    std::array<VkBufferImageCopy, 16> copies{};
    for (uint32_t n = 0; n < texture.levels; ++n) {
      VkBufferImageCopy& copy = copies[n];
      copy.bufferOffset = offset + texture.offset_level[n];
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, n, 0, texture.layers};
      copy.imageExtent = {std::max(texture.image.width >> n, 1u), std::max(texture.image.height >> n, 1u),
                           texture.background ? texture.background : 1};
    }
    dfn_.vkCmdCopyBufferToImage(upload, upload_, texture.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                texture.levels, copies.data());
  }

  // A texture's view, the same for SlotView and for the deferred views of CollectBindings.
  VkResult CreateViewTexture(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap,
                             VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = heap == 2   ? VK_IMAGE_VIEW_TYPE_CUBE
                    : heap == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.components = MappingComponents(swizzle, swizzle_host);
    info.subresourceRange = {IsDepth(format)
                                 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, VK_REMAINING_MIP_LEVELS, 0, heap == 2 ? 6u : 1u};  // with its mips
    return dfn_.vkCreateImageView(device_, &info, nullptr, &view);
  }

  // The views_ key (image, heap and both swizzles), factored out for CollectBindings.
  static uint64_t KeyView(VkImage image, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap) {
    return XXH3_64bits_withSeed(&image, sizeof(image),
                                (uint64_t(heap) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
  }

  // Heap slot (0 for 2D textures, 2 for cubemaps) for a view of the image with that swizzle.
  uint32_t SlotView(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                       uint32_t heap = 0, bool srgb_of_resolved = false) {
    // The same key; the sRGB view of a resolved picture is a second view of the same image.
    const uint64_t key = KeyView(image, swizzle, swizzle_host, heap) ^ (srgb_of_resolved ? 0x5A17C0DE5A17C0DEull : 0);
    if (const auto it = views_.find(key); it != views_.end()) {
      return it->second.slot;
    }
    // An image whose vkBindImageMemory is still on the bind thread. The view carries the image's GPU
    // address, which comes from the bind: the slot is reserved now, and the view is created and written in
    // CollectBindings, before submission (UPDATE_AFTER_BIND descriptors).
    if (!images_in_flight_.empty() && images_in_flight_.count(image)) {
      return SlotViewInFlight(key, image, format, swizzle, swizzle_host, heap);
    }
    ViewEntry view;
    view.image = image;
    view.heap = heap;
    if (CreateViewTexture(image, format, swizzle, swizzle_host, heap, view.view) != VK_SUCCESS) {
      Notify(34, "could not create the view of a texture");
      return 0;
    }
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
      ++heap_full_;
      Notify(35, "texture heap full");
      return 0;
    }
    WriteImage(heap, view.slot, view.view);
    views_.emplace(key, view);
    views_by_image_[image].push_back(key);
    return view.slot;
  }

  // point: no filtering (depth textures).
  uint32_t SlotSampler(const uint32_t* f, bool point = false) {
    // Diagnostic: filters the game requests that this renderer does not apply (anisotropy, mip bias and
    // maximum mip level; the bias is not even part of the key). Each new value is logged once.
    {
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      const int32_t bias = int32_t(f[4] << 10) >> 22;  // lod_bias: 10 signed bits, 5 fractional
      const uint32_t mip_max = (f[4] >> 6) & 0xF;
      const uint32_t index_bias = uint32_t(bias + 512);
      if (!((aniso_seen_ >> aniso) & 1u) || !biases_seen_[index_bias] || !((mip_max_seen_ >> mip_max) & 1u)) {
        aniso_seen_ |= 1u << aniso;
        biases_seen_[index_bias] = true;
        mip_max_seen_ |= 1u << mip_max;
        REXLOG_INFO("[native] C4 filters requested by the game (new value): anisotropic {}, mip bias {:.3f}, "
                    "maximum mip level {}, mag/min/mip filters {}/{}/{}{}",
                    aniso, float(bias) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                    (f[3] >> 23) & 0x3, point ? " (point sampling)" : "");
      }
    }
    /*
     * Trilinear -> bilinear, for measurement only (scene pass analysis).
     *
     * The game requests mag/min/mip filters 1/1/1, i.e. mipmapMode LINEAR. On Maxwell (GM20B) a trilinear
     * sample takes the texture unit two cycles (two bilinear ones) and a bilinear sample only one: with the
     * filter between levels at NEAREST the TMU's work is halved without touching a single ALU instruction.
     * It is the only lever that separates the two:
     *   - if the scene drops by ~half of the TMU's share, sampling dominates;
     *   - if it does not move, the ALU dominates and the TMU has headroom.
     * A mip level jump is visible on receding surfaces (the PS2 "shimmer"). For measurement only. Off by
     * default: at 0 this is two bool comparisons per new sampler (samplers are cached) and not one more
     * instruction on the draw path.
     *
     * The modified mip field is part of the sampler key, so toggling at runtime creates two different
     * samplers and nothing has to be invalidated: the alternation works.
     */
    uint32_t field_filters = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
    if (mip_point_test_ && ((field_filters >> 4) & 0x3) == 1) {
      field_filters &= ~(0x3u << 4);  // LINEAR -> NEAREST between mip levels
    }
    // With fh1_native_diag_min_mip the minimum level rises (without passing the maximum) and goes into
    // the key that way: each value has its own samplers, and going back to 0 reuses the usual ones.
    uint32_t field_mips = (f[4] >> 2) & 0xFF;  // minimum in 0-3, maximum in 4-7
    if (diag_mip_minimum_ != 0) {
      const uint32_t minimum = field_mips & 0xF;
      const uint32_t maximum = (field_mips >> 4) & 0xF;
      if (maximum > minimum) {
        field_mips = (field_mips & 0xF0) | std::min(maximum, minimum + diag_mip_minimum_);
      }
    }
    const uint32_t key = ((f[0] >> 10) & 0x1FF) | (field_filters << 9) |
                           (field_mips << 21) | ((f[5] & 0x3) << 29) |
                           (point ? 0x80000000u : 0u);
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
      return it->second.second;
    }
    static constexpr VkSamplerAddressMode kModes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const bool mirror = vulkan_device_->properties().samplerMirrorClampToEdge;
    const auto mode = [&](uint32_t input_value) {
      const VkSamplerAddressMode m = kModes[input_value & 0x7];
      return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !mirror
                 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : m;
    };
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter =
        ((f[3] >> 19) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter =
        ((f[3] >> 21) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (field_filters >> 4) & 0x3;  // already includes the point mip test
    info.mipmapMode =
        mip == 1 && !point ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = mode(f[0] >> 10);
    info.addressModeV = mode(f[0] >> 13);
    info.addressModeW = mode(f[0] >> 16);
    info.minLod = float(field_mips & 0xF);  // with the mip diagnostic if it is enabled
    // With real mips, the fetch constant's maximum level also limits (like the emulation's sampler);
    // single-level textures stay as before.
    info.maxLod = mip == 2 ? info.minLod + 0.25f : std::max(info.minLod, float((f[4] >> 6) & 0xF));
    info.borderColor = (f[5] & 0x3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // fh1_native_anisotropic. Only linear samplers with mips (maxLod above minLod); the value is read at
    // start-up, so it does not need to be in the key: every sampler is created with the same one.
    {
      static const uint32_t aniso_requested = uint32_t(std::max(0, int32_t(REXCVAR_GET(fh1_native_anisotropic))));
      const auto& properties = vulkan_device_->properties();
      if (aniso_requested > 1 && properties.samplerAnisotropy && info.minFilter == VK_FILTER_LINEAR &&
          info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::max(1.0f, std::min(float(aniso_requested), properties.maxSamplerAnisotropy));
        if (!aniso_noted_) {
          aniso_noted_ = true;
          REXLOG_INFO("[native] C4 anisotropic filtering {} on the linear samplers with mips (requested {}, device "
                      "cap {}); single-level and point ones unchanged",
                      info.maxAnisotropy, aniso_requested, properties.maxSamplerAnisotropy);
        }
      }
    }
    VkSampler sampler;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
      Notify(36, "could not create a sampler");
      return 0;
    }
    const uint32_t slot = ReserveSlot(3);
    if (!slot) {
      dfn_.vkDestroySampler(device_, sampler, nullptr);
      Notify(37, "sampler heap full");
      return 0;
    }
    WriteSampler(slot, sampler);
    samplers_.emplace(key, std::make_pair(sampler, slot));
    return slot;
  }

  // Category clipped to 1x1 (0 = none), with its alternating test.
  uint32_t ScissorOfTest() {
    const int32_t category = REXCVAR_GET(fh1_native_test_scissor);
    if (category <= 0) {
      return 0;
    }
    const int32_t toggle = REXCVAR_GET(fh1_native_test_scissor_toggle_s);
    if (toggle <= 0) {
      return uint32_t(category);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool with_scissor = (seconds / toggle) % 2 == 1;
    if (with_scissor != alternation_scissor_noted_) {
      alternation_scissor_noted_ = with_scissor;
      REXLOG_INFO("[native] C2 test scissor: {}", with_scissor ? "clipping to 1x1" : "normal");
    }
    return with_scissor ? uint32_t(category) : 0;
  }

  /*
   * The ring's verdict for vegetation filtered in the game (fh1_d3d_game_vegetation). If the draw
   * carries the game's verdict (the kVeg* flags of its record), the ring's own is computed from this
   * draw's registers and PS: the criterion of Draw's early discard, minus the earlier exits that have
   * no side effects (primitive type, vertex input, diagnostics) and do not draw either. It goes to the
   * guard (NoteVegetationRing) and, if Draw reaches its early discard, it must have decided the
   * same (NoteVegetationModel otherwise).
   */
  void VerdictVegetation(const RequestDraw& p) {
    vegetation_computed_ = false;
    if (!(p.vegetation_game & kVegThereIs)) {
      return;
    }
    const uint32_t* r = p.register_values;
    DetailVegetation a;
    if (r) {
      a.mode = r[gr::XE_GPU_REG_RB_MODECONTROL];
      a.mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
      a.control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    }
    a.vs = p.vs ? int32_t(p.vs->number) : -1;
    a.ps = p.ps ? int32_t(p.ps->number) : -1;
    if (p.ps) {
      a.outputs = p.ps->outputs;
      a.discards = p.ps->discards;
    }
    if (r && p.vs && p.ps && (a.mode & 0x7) == uint32_t(xenos::EdramMode::kColorDepth)) {
      bool color = false;
      for (uint32_t i = 0; i < 4; ++i) {
        color = color || (((a.mask >> (i * 4)) & 0xF) && ((p.ps->outputs >> i) & 0x1));
      }
      const bool test_alpha = ((a.control >> 3) & 0x1) && (a.control & 0x7) != 7;
      a.structure = !color && (test_alpha || p.ps->discards || (p.ps->outputs & 0x10));
    }
    a.settings = (cvars_by_frame_ ? ps_only_alpha_frame_ : PsOnlyAlpha()) && without_vegetation_;
    a.occlusion = occlusion_open_;
    vegetation_ring_ = a.structure && a.settings && !a.occlusion;
    vegetation_computed_ = true;
    vegetation_flags_ = p.vegetation_game;
    vegetation_detail_ = a;
    NoteVegetationRing(p.vegetation_game, a);
  }

  // The setting, or its alternating test.
  bool PsOnlyAlpha() {
    const int32_t toggle = REXCVAR_GET(fh1_native_alpha_only_ps_toggle_s);
    if (toggle <= 0) {
      return REXCVAR_GET(fh1_native_alpha_only_ps);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool only_alpha = (seconds / toggle) % 2 == 1;
    if (only_alpha != alternation_only_alpha_noted_) {
      alternation_only_alpha_noted_ = only_alpha;
      REXLOG_INFO("[native] C2 color writes: {}",
                  only_alpha ? "removed in the colorless passes" : "all");
    }
    return only_alpha;
  }

  // The setting, or its alternating test.
  bool SinPsSinColor() {
    const int32_t toggle = REXCVAR_GET(fh1_native_no_ps_without_color_toggle_s);
    if (toggle <= 0) {
      return REXCVAR_GET(fh1_native_no_ps_without_color);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_ps_)
                              .count();
    const bool sin_ps = (seconds / toggle) % 2 == 1;
    if (sin_ps != alternation_ps_noted_) {
      alternation_ps_noted_ = sin_ps;
      REXLOG_INFO("[native] C2 fragment stage: {}",
                  sin_ps ? "removed in the colorless draws" : "always built");
    }
    return sin_ps;
  }

  // 1/size of a slot's host image, in the shared constants. With size 0 (a texture that could not be
  // prepared) 0 is written: the shader does not use it because it does not sample either.
  static void WriteInvSize(uint32_t* shared, uint32_t reg_entry, uint32_t width, uint32_t height) {
    const float inv[2] = {width ? 1.0f / float(width) : 0.0f, height ? 1.0f / float(height) : 0.0f};
    std::memcpy(shared + kWordInvSize + reg_entry * 2, inv, sizeof(inv));
  }

  bool BeginPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch,
                   uint64_t key_pass) {
    start_pass_ = time_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    if (!PrepareEmpty()) {
      return false;
    }
    // FH1: a 4x MSAA render target occupies the EDRAM of the 1x one of twice the pitch and height (its 2x2
    // samples are that target's pixels), and FH1 relies on it: it draws scene depth at 640 pitch 4x and then
    // uses it as the 1280x720 depth. 4x passes draw into that 1x image at twice the scale (ComputeFraming).
    // 2x MSAA stays at one sample (its resolves are not scaled yet). fh1_msaa_4x_as_1x = false: as before.
    const uint32_t msaa = (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 0x3;
    // FH1 (fh1_msaa_4x_clears_as_1x): only the 4x passes of 640 pitch or less. Those are Direct3D's clears: a
    // rectangle on a 4x surface of half the pitch covers the same EDRAM as the 1x (or 2x) target the game really
    // uses, four samples per pixel drawn. Kept in an image of their own they cleared nothing: the stencil the shadow
    // passes test was never reset and the scene had no shadows. The scene itself (4x at 1280 pitch) stays as it was.
    const bool msaa_4x = msaa == uint32_t(xenos::MsaaSamples::k4X) &&
                         (REXCVAR_GET(fh1_msaa_4x_as_1x) || (pitch <= 640 && REXCVAR_GET(fh1_msaa_4x_clears_as_1x)));
    pass_msaa_scale_ = msaa_4x ? 2.0f : 1.0f;
    const uint32_t pitch_guest = pitch;
    if (msaa_4x) pitch *= 2;
    std::array<ImageNative*, 5> images{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        images[i] = context_->TargetColor(uint32_t(keys[i] >> 24) & 0xFFF,
                                              uint32_t(keys[i] >> 16) & 0xF, pitch);
        if (!images[i]) {
          return Reject(40, "no color target");
        }
      }
    }
    if (keys[4]) {
      images[4] = context_->TargetDepth(uint32_t(keys[4] >> 24) & 0xFFF,
                                                  uint32_t(keys[4] >> 16) & 0x1, pitch);
      if (!images[4]) {
        return Reject(41, "no depth target");
      }
    }
    (void)pitch_guest;
    // The render targets are already resolved above; that segment is closed.
    const auto after_targets = time_ ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    if (time_) {
      stages_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          after_targets - start_pass_).count());
    }
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    std::array<VkImageView, 5> views{};
    uint32_t formats[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      if (images[i]) {
        width = std::min(width, images[i]->width);
        height = std::min(height, images[i]->height);
        views[i] = images[i]->view;
        formats[i] = uint32_t(images[i]->format);
        // FH1: a k_8_8_8_8_GAMMA color target stores what the shader writes gamma-encoded (see view_srgb).
        if (gamma_targets_ && i < 4 && (uint32_t(keys[i] >> 16) & 0xF) == 1 &&
            images[i]->view_srgb != VK_NULL_HANDLE) {
          views[i] = images[i]->view_srgb;
          formats[i] = uint32_t(VK_FORMAT_R8G8B8A8_SRGB);
        }
      }
    }
    // Shadow map: a depth-only target with a pitch of 1600 or more (same as skip_shadows).
    const bool is_shadows = pitch >= 1600 && formats[4] && !formats[0] && !formats[1] &&
                            !formats[2] && !formats[3];
    // fh1_native_shadow_minimum. If C2 says this is the car pass of the shadow map, each draw is validated
    // for it (and the pass must be depth-only).
    pass_cars_shadow_ = images[4] != nullptr && context_->PassOfCarsShadow(images[4], is_shadows);
    // GPU time per pass type (C2 report), by render target width. Shadows are recognized by being
    // depth-only, not by measuring 1600 or more. With the scene at 1920 (fh1_1080p_test) the width no
    // longer tells them apart: the scene fell into the shadow bucket and vanished from its own category.
    category_pass_ = CategoryOfTarget(pitch, keys);
    context_->MarkGpu(category_pass_);
    const bool shadows_without_load =
        is_shadows && REXCVAR_GET(fh1_native_shadow_pass_without_load);
    const VkRenderPass pass = PassOf(formats, shadows_without_load ? kLoadIgnore : kLoadRead);
    if (pass == VK_NULL_HANDLE) {
      return Reject(42, "could not create the render pass");
    }
    const VkFramebuffer framebuffer = FramebufferDe(pass, views, width, height);
    if (framebuffer == VK_NULL_HANDLE) {
      return Reject(43, "could not create the framebuffer");
    }
    if (time_) {
      stages_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - after_targets).count());
    }
    const auto before_open = time_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const VkCommandBuffer cmd = context_->CommandsWork();
    if (!cmd) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    // Only the rectangle the game uses (see fh1_native_pass_useful_area).
    uint32_t height_pass = height;
    if (area_util_) {
      const auto it = height_useful_.find(pitch);
      if (it != height_useful_.end() && it->second < height) {
        height_pass = it->second;
      }
    }
    start.renderArea.extent = {width, height_pass};
    // fh1_native_diag_clears. What this pass loads and stores of each render target, to compare with
    // what is cleared of it (C2 clears per target). Before opening the pass.
    for (const ImageNative* image : images) {
      if (image) {
        context_->NoteAreaOfPass(image, width, height_pass);
      }
    }
    // The statistics cover the whole pass, from here to after EndRenderPass.
    statistics_pass_ = context_->BeginStatistics(category_pass_);
    const auto before_start = std::chrono::steady_clock::now();
    BarrierGlobal(cmd);
    dfn_.vkCmdBeginRenderPass(cmd, &start, VK_SUBPASS_CONTENTS_INLINE);
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_start).count());
    if (time_) {
      stages_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_open).count());
    }
    pass_active_ = true;
    ++passes_started_;
    texels_passes_ += uint64_t(width) * height_pass;  // how much tile is loaded by loadOp = LOAD
    {
      // Same criterion as the per-category GPU time: the render target's pitch decides.
      const uint32_t category = CategoryOfTarget(pitch, keys);
      texels_by_category_[category] += uint64_t(width) * height_pass;
      ++passes_by_category_[category];
    }
    pass_commands_ = cmd;
    draws_in_pass_ = 0;  // position within the pass (deferred sky report)
    pass_key_ = key_pass;
    std::memcpy(pass_keys_, keys, sizeof(pass_keys_));  // the bytes that produce pass_key_
    pass_keys_valid_ = true;
    ++pass_serie_;  // the cached framing depends on the pass size and scale
    pass_generation_ = context_->GenerationCommands();
    pass_width_ = width;
    pass_height_ = height;
    // Only the shadow map is drawn at a size other than the one the guest thinks.
    pass_scale_ = (is_shadows && pitch && width && width != pitch)
                       ? float(width) / float(pitch)
                       : 1.0f;
    std::copy(std::begin(formats), std::end(formats), std::begin(pass_formats_));
    pass_rp_ = pass;
    // Phase 0b (fh1_native_pipeline_between_passes). Vulkan keeps the bound pipeline across passes of the
    // same buffer: the pass's first draw only binds again if its key differs. A new buffer still starts
    // with nothing bound (Draw, recording_generation_).
    if (!pipeline_between_passes_) {
      pipeline_bound_ = VK_NULL_HANDLE;
    } else if (pipeline_bound_ != VK_NULL_HANDLE) {
      ++passes_with_pipeline_;
    }
    state_recorded_ = false;
    if (pipeline_bound_ == VK_NULL_HANDLE) {
      eds_valid_ = false;  // pass that forgets the pipeline (without 0b): set everything again
    }
    return true;
  }

  // ZCULL: three load modes. kLoadClear is needed to clear the depth of images created without
  // TRANSFER_DST, the only ones eligible for a ZCULL plane.
  enum : uint32_t { kLoadRead = 0, kLoadIgnore = 1, kLoadClear = 2 };

  VkRenderPass PassOf(const uint32_t formats[5], uint32_t mode_load = kLoadRead) {
    // The mode is part of the key: two render passes with the same formats but a different loadOp are
    // different.
    const uint64_t key =
        XXH3_64bits_withSeed(formats, sizeof(uint32_t) * 5, mode_load);
    if (const auto it = passes_.find(key); it != passes_.end()) {
      return it->second;
    }
    const VkRenderPass pass = CreatePass(formats, mode_load);  // the usual code, factored out
    if (pass == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
    passes_.emplace(key, pass);
    return pass;
  }

  // The plain loop for 16-bit indices (byte-swapped or not), untouched.
  static void IndicesOf16Scale(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& minimum,
                                 uint32_t& maximum) {
    minimum = 0xFFFF;
    maximum = 0;
    if (rotate) {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t raw;
        std::memcpy(&raw, data + size_t(i) * 2, 2);
        const uint16_t v = std::byteswap(raw);
        output[i] = v;
        minimum = std::min<uint32_t>(minimum, v);
        maximum = std::max<uint32_t>(maximum, v);
      }
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t v;
        std::memcpy(&v, data + size_t(i) * 2, 2);
        output[i] = v;
        minimum = std::min<uint32_t>(minimum, v);
        maximum = std::max<uint32_t>(maximum, v);
      }
    }
  }

  // The same loop with NEON, 16 indices per iteration (vrev16 swaps the bytes of each 16-bit index, the
  // same as std::byteswap), and the remainder with the plain loop. NEON loads and stores need no
  // alignment on AArch64.
  static void IndicesDe16Neon(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& minimum,
                              uint32_t& maximum) {
#if defined(__aarch64__)
    uint32_t i = 0;
    uint32_t mn = 0xFFFF;
    uint32_t mx = 0;
    if (count >= 16) {
      uint16x8_t min_a = vdupq_n_u16(0xFFFF);
      uint16x8_t min_b = min_a;
      uint16x8_t max_a = vdupq_n_u16(0);
      uint16x8_t max_b = max_a;
      // Two loops (swapping is fixed per draw): no decision inside the iteration.
      if (rotate) {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2)));
          const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2 + 16)));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      } else {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2));
          const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2 + 16));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      }
      mn = vminvq_u16(vminq_u16(min_a, min_b));
      mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
    }
    for (; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, data + size_t(i) * 2, 2);
      if (rotate) {
        v = std::byteswap(v);
      }
      output[i] = v;
      mn = std::min<uint32_t>(mn, v);
      mx = std::max<uint32_t>(mx, v);
    }
    minimum = mn;
    maximum = mx;
#else
    IndicesOf16Scale(data, count, output, rotate, minimum, maximum);
#endif
  }

  // The 16-bit indices of a draw (see fh1_native_indices_neon). Ring only.
  void IndicesDe16(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& minimum,
                   uint32_t& maximum) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(fh1_native_indices_neon) ? 1 : 0;
      REXLOG_INFO("[native] C6 16-bit indices (build 187): {}",
                  indices_neon_ ? "with NEON; the first 20,000 draws are checked and then 1 in 4,096"
                                : "with the usual loop (fh1_native_indices_neon = false)");
    }
    if (indices_neon_ == 0) {
      IndicesOf16Scale(data, count, output, rotate, minimum, maximum);
      return;
    }
    IndicesDe16Neon(data, count, output, rotate, minimum, maximum);
    const uint64_t n = ++indices_neon_draws_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    indices_neon_test_.resize(count);
    uint32_t mn = 0;
    uint32_t mx = 0;
    IndicesOf16Scale(data, count, indices_neon_test_.data(), rotate, mn, mx);
    ++indices_neon_checked_;
    if (mn != minimum || mx != maximum ||
        (count && std::memcmp(indices_neon_test_.data(), output, size_t(count) * 2) != 0)) {
      // The plain path stays for this draw and for the rest of the session.
      if (count) {
        std::memcpy(output, indices_neon_test_.data(), size_t(count) * 2);
      }
      minimum = mn;
      maximum = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[native] C6 16-bit indices: DIFFERENCE between NEON and the usual loop ({} indices, {}; "
                   "minimum {} against {}, maximum {} against {}). Off for the rest of the session: the usual loop",
                   count, rotate ? "swapped" : "not swapped", minimum, mn, maximum, mx);
      return;
    }
    if (indices_neon_checked_ == 20000) {
      REXLOG_INFO("[native] C6 16-bit indices (build 187): 20,000 draws checked against the usual loop, 0 "
                  "differences; keeps checking 1 in 4,096");
    }
  }

  // The PassOf render pass without its cache, with nothing changed. The pipeline prewarm also uses it
  // from its thread (a compatible one: the same formats). It only reads its arguments.
  VkRenderPass CreatePass(const uint32_t formats[5], uint32_t mode_load) const {
    std::array<VkAttachmentDescription, 5> attachments{};
    std::array<VkAttachmentReference, 4> colors{};
    VkAttachmentReference depth{};
    uint32_t n = 0, n_colors = 0;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!formats[i]) {
        continue;
      }
      VkAttachmentDescription& a = attachments[n];
      a.format = VkFormat(formats[i]);
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      // With the test active, the shadow map does not load its previous content: the game clears it before
      // drawing it, so fetching the 1600x1600 tile only to throw it away is wasted work.
      a.loadOp = mode_load == kLoadIgnore ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                 : mode_load == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = i != 4                       ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                        : mode_load == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.stencilStoreOp = i == 4 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (i < 4) {
        colors[n_colors++] = {n, VK_IMAGE_LAYOUT_GENERAL};
      } else {
        depth = {n, VK_IMAGE_LAYOUT_GENERAL};
      }
      ++n;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = n_colors;
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = formats[4] ? &depth : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass;
    if (dfn_.vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pass;
  }

  VkFramebuffer FramebufferDe(VkRenderPass pass, const std::array<VkImageView, 5>& views,
                              uint32_t width, uint32_t height) {
    struct Key {
      VkRenderPass pass;
      std::array<VkImageView, 5> views;
      uint32_t width, height;
    } key{pass, views, width, height};
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    if (const auto it = framebuffers_.find(fingerprint); it != framebuffers_.end()) {
      return it->second;
    }
    std::array<VkImageView, 5> attachments{};
    uint32_t n = 0;
    for (VkImageView view : views) {
      if (view != VK_NULL_HANDLE) {
        attachments[n++] = view;
      }
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(fingerprint, framebuffer);
    framebuffers_views_[fingerprint] = views;  // For ForgetView
    return framebuffer;
  }

  // The bright pass variants of fh1_sky_glow (1 natural, 2 soft), compiled with the same DXC
  // options as the library.
  VkShaderModule ModuleVariant(size_t index, const uint32_t* spirv, size_t bytes) {
    if (!modules_variants_created_[index]) {
      modules_variants_created_[index] = true;
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &modules_variants_[index]) != VK_SUCCESS) {
        modules_variants_[index] = VK_NULL_HANDLE;
      }
    }
    return modules_variants_[index];
  }

  // fh1_sky_glow (F4 menu, Graphics: 0 original, 1 natural, 2 soft), or its test rotation; logs
  // every change.
  int GlowSky() {
    int mode = fh1::settings::GlowSky();
    const int32_t toggle = REXCVAR_GET(fh1_native_soft_glow_toggle_s);
    if (toggle > 0) {
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - start_shadows_)
                                .count();
      mode = int((seconds / toggle) % 3);
    }
    mode = std::clamp(mode, 0, 2);
    if (mode != glow_noted_) {
      glow_noted_ = mode;
      static constexpr const char* kNames[3] = {"original", "natural", "soft"};
      REXLOG_INFO("[native] sky glow: {} (frame {})", kNames[mode], frame_);
    }
    return mode;
  }

  /*
   * The early Z and invisible draws report, every 10 s like the others.
   *
   * What matters is not what is saved but the second figure: "CANNOT because they write depth"
   * is, draw by draw, the exact size of what only a depth pre-pass could address, and that is expensive
   * to implement. A small figure means a pre-pass is not worth the risk.
   */
  void ReportZEarly() {
    ++frames_z_;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_z_).count() < 10) {
      return;
    }
    last_report_z_ = now;
    const uint64_t seen_2 = frames_z_ - frames_z_previous_;
    const double frames = double(seen_2 ? seen_2 : 1);
    std::array<uint64_t, kZCounts> d{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kZCounts; ++i) {
      d[i] = counts_z_[i] - counts_z_previous_[i];
      counts_z_previous_[i] = counts_z_[i];
      total += d[i];
    }
    frames_z_previous_ = frames_z_;
    ReportSky(frames);  // goes before the !total cutoff, which has nothing to do with the sky
    if (!total) {
      return;
    }
    const uint64_t late = d[kZSet] + d[kZWritesZ] + d[kZStencil] + d[kZOcclusion];
    FH1_REPORT_RING(
        "[native] C6 early Z per frame: {:.0f} draws use it, {:.0f} CANNOT because they write depth (those are the "
        "size of the C88 pre-pass), {:.0f} because of stencil, {:.0f} inside an occlusion query; {:.0f} already "
        "tested before. Of the ones that forced shading first, {:.0f} % are fixed{}",
        double(d[kZSet]) / frames, double(d[kZWritesZ]) / frames,
        double(d[kZStencil]) / frames, double(d[kZOcclusion]) / frames,
        double(d[kZAlreadyEarly]) / frames,
        late ? 100.0 * double(d[kZSet]) / double(late) : 0.0,
        z_early_without_module_ ? fmt::format(" ({} shaders without a patched module)", z_early_without_module_)
                               : std::string());
    if (d[kInvisibleBlend] || d[kInvisibleAlpha] || d[kInvisibleSoloColor]) {
      FH1_REPORT_RING(
          "[native] C6 invisible draws per frame: {:.1f} with the blend that copies the target, {:.1f} with the "
          "alpha test at NEVER; {:.1f} keep only the depth",
          double(d[kInvisibleBlend]) / frames, double(d[kInvisibleAlpha]) / frames,
          double(d[kInvisibleSoloColor]) / frames);
    }
  }

  /*
   * The deferred sky report. Without it there is no way to measure whether it works.
   *
   * What to check: "deferred" must be ~0.9 per frame in a race (the sky is a single draw and is
   * sometimes absent). "lost_count" must be 0: if not, some path closes the pass without going through
   * FinishPass and the sky is being lost. And "position" says how many draws of the pass were recorded
   * between the old place and the new one: exactly the amount of geometry that now covers the sky before
   * it is shaded. If it is small, so is the saving.
   */
  void ReportSky(double frames) {
    const uint64_t deferred_2 = sky_deferred_count_ - sky_deferred_count_previous_;
    const uint64_t seen_2 = sky_seen_ - sky_seen_previous_;
    if (!seen_2) {
      return;
    }
    std::array<uint64_t, kSkyReasons> by_reason{};
    uint64_t emitted = 0;
    for (uint32_t i = 0; i < kSkyReasons; ++i) {
      by_reason[i] = sky_emitted_[i] - sky_emitted_previous_[i];
      sky_emitted_previous_[i] = sky_emitted_[i];
      emitted += by_reason[i];
    }
    const uint64_t position_sum = sky_position_sum_ - sky_position_sum_previous_;
    const uint64_t candidates = sky_candidates_ - sky_candidates_previous_;
    sky_candidates_previous_ = sky_candidates_;
    sky_seen_previous_ = sky_seen_;
    sky_deferred_count_previous_ = sky_deferred_count_;
    sky_position_sum_previous_ = sky_position_sum_;
    /*
     * The line that decides whether the criterion works. "with the dome geometry" is the number that
     * matters: it must be 1.00 per frame. The "detected" are by fingerprint only and once gave 7.00,
     * which is what broke the image.
     */
    FH1_REPORT_RING("[native] C6 sky: {:.2f} detected by fingerprint and {:.2f} with the dome geometry (480 "
                     "indices and first in the pass) per frame. The second must be 1.00. GUARD: {} ({} frames with "
                     "a dome looked at, {} with exactly one, maximum {} in one frame)",
                double(seen_2) / frames, double(candidates) / frames,
                sky_guard_ == kSkyDeferring  ? "PASSED, deferring"
                : sky_guard_ == kSkyDiscarded ? "REJECTED, nothing is deferred"
                                                     : "still watching",
                sky_guard_frames_, sky_guard_with_one_, sky_guard_max_);
    FH1_REPORT_RING(
        "[native] C6 deferred sky ({}): {:.2f} detected per frame, {:.2f} really deferred ({} not deferrable in "
        "total, {} with another sky already pending); emitted {} when a blended draw arrives, {} when an opaque "
        "draw that does not write Z arrives, {} when the pass closes, {} because of another sky; LOST {}; average "
        "position {:.0f} draws ahead of it (maximum {})",
        sky_deferred_ ? "on" : "OFF", double(seen_2) / frames,
        double(deferred_2) / frames, sky_no_deferrable_, sky_two_in_pass_,
        by_reason[kSkyByBlend], by_reason[kSkyByWithoutZ], by_reason[kSkyByEndOfPass],
        by_reason[kSkyByOtherSky], sky_lost_,
        emitted ? double(position_sum) / double(emitted) : 0.0, sky_position_max_);
  }

  /*
   * Closes the frame for the sky guard. See the block of fields.
   *
   * Runs once per frame and does nothing once the guard has decided, so its cost is one comparison. The
   * decision is taken only once per session and logged.
   */
  void CloseFrameOfTheGuardOfSky() {
    const uint32_t in_this_frame = sky_guard_in_frame_;
    sky_guard_in_frame_ = 0;
    if (sky_guard_ != kSkyWatching) {
      return;
    }
    /*
     * A frame without a sky says nothing about the criterion: menus, the logo and loading screens, which
     * come before any race. If they counted, the test would run out in the menu and the guard would switch
     * off for good without ever seeing the dome. Only frames in which the dome appears count.
     */
    if (in_this_frame == 0) {
      return;
    }
    sky_guard_max_ = std::max(sky_guard_max_, in_this_frame);
    if (in_this_frame == 1) {
      ++sky_guard_with_one_;
    }
    if (++sky_guard_frames_ < kSkyFramesTest) {
      return;
    }
    /*
     * The test is over. It only switches on if it came out clean: never more than one, and enough frames
     * with exactly one. In any other case it stays off and the image is identical to the non-deferred one,
     * which is known to work.
     */
    if (sky_guard_max_ == 1 && sky_guard_with_one_ >= kSkyFramesWithOne) {
      sky_guard_ = kSkyDeferring;
      REXLOG_INFO("[native] C6 sky: guard PASSED ({} of {} frames with exactly one dome, never two). The sky is "
                  "deferred from now on: 3.4-4.3 ms of GPU without changing a pixel",
                  sky_guard_with_one_, sky_guard_frames_);
    } else {
      sky_guard_ = kSkyDiscarded;
      REXLOG_WARN("[native] C6 sky: guard NOT passed (maximum {} domes in one frame, {} of {} frames with exactly "
                  "one; {} were needed). Nothing is deferred: the image stays exactly as it was",
                  sky_guard_max_, sky_guard_with_one_, sky_guard_frames_,
                  kSkyFramesWithOne);
    }
  }

  VkShaderModule ModuleOf(const EntryShader& entry) {
    if (const auto it = modules_.find(&entry); it != modules_.end()) {
      return it->second;
    }
    const auto& spirv = entry.shader->spirv;
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module_handle = VK_NULL_HANDLE;
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_handle) != VK_SUCCESS) {
      module_handle = VK_NULL_HANDLE;
    }
    modules_.emplace(&entry, module_handle);
    return module_handle;
  }

  // Pixel shader module without the color writes, one per shader. If it cannot be pruned, the normal one
  // is returned: the image is the same, only the saving is lost.
  VkShaderModule ModuleOnlyAlpha(const EntryShader& entry) {
    if (const auto it = modules_only_alpha_.find(&entry); it != modules_only_alpha_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(entry);
    }
    uint32_t removed = 0;
    const std::vector<uint32_t> pruned = PruneWritesOfColor(entry.shader->spirv, removed);
    VkShaderModule module_handle = VK_NULL_HANDLE;
    if (!pruned.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = pruned.size() * sizeof(uint32_t);
      info.pCode = pruned.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_handle) != VK_SUCCESS) {
        module_handle = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C5a: PS n{} without color writes: {} removed, {} words of {} ({})",
                entry.number, removed, pruned.size(), entry.shader->spirv.size(),
                module_handle != VK_NULL_HANDLE ? "module created" : "using the normal one");
    modules_only_alpha_.emplace(&entry, module_handle);
    return module_handle != VK_NULL_HANDLE ? module_handle : ModuleOf(entry);
  }

  /*
   * Pixel shader module with EarlyFragmentTests declared, one per shader. If that is not possible (it
   * already had it, it writes gl_FragDepth...), the normal one is returned: the image is the same and only
   * the saving is lost. It is logged once per shader so the log shows which ones qualified.
   */
  VkShaderModule ModuleZEarly(const EntryShader& entry) {
    if (const auto it = modules_z_early_.find(&entry); it != modules_z_early_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(entry);
    }
    const char* reason = "";
    const std::vector<uint32_t> patched = WithTestsEarly(entry.shader->spirv, reason);
    VkShaderModule module_handle = VK_NULL_HANDLE;
    if (!patched.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = patched.size() * sizeof(uint32_t);
      info.pCode = patched.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_handle) != VK_SUCCESS) {
        module_handle = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C6 early Z: PS n{} {} ({} kills, {} words)", entry.number,
                module_handle != VK_NULL_HANDLE
                    ? std::string("tests depth before shading")
                    : fmt::format("stays as it was: {}", reason),
                entry.kills, entry.shader->spirv.size());
    modules_z_early_.emplace(&entry, module_handle);
    if (module_handle == VK_NULL_HANDLE) {
      ++z_early_without_module_;
    }
    return module_handle != VK_NULL_HANDLE ? module_handle : ModuleOf(entry);
  }

  /*
   * The fixed state of a pipeline, exactly as PipelineOf sets it. It is its usual code, moved here without
   * changing any computation: only its eight declarations become references into StateFixedPipeline. Used
   * by PipelineOf and by the guards of the canonical key (phase 0a) and of dynamic state (phases 1 and 2),
   * which thus compare against what the pipeline really carries and not against another copy of the same
   * computations. blend.pAttachments points to fixed.blends: the structure is not copied.
   */
  struct StateFixedPipeline {
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineViewportStateCreateInfo view{};
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo sampling{};
    VkPipelineDepthStencilStateCreateInfo depth{};
    std::array<VkPipelineColorBlendAttachmentState, 4> blends{};
    uint32_t n_colors = 0;
    VkPipelineColorBlendStateCreateInfo blend{};
    StateFixedPipeline() = default;
    StateFixedPipeline(const StateFixedPipeline&) = delete;
    StateFixedPipeline& operator=(const StateFixedPipeline&) = delete;
  };

  // warn = false from the prewarm thread (Warn belongs to the ring only).
  void FillStateFixed(const KeyPipeline& key, StateFixedPipeline& fixed, bool notify = true) {
    VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VkPrimitiveTopology(key.topology);
    assembly.primitiveRestartEnable = (key.rasterization & 0x8) ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo& view = fixed.view;
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = ((key.rasterization & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                           ((key.rasterization & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);
    // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (not confirmed on screen).
    rasterization.frontFace =
        (key.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (REXCVAR_GET(fh1_debug_no_cull)) {  // NFSC debug
      rasterization.cullMode = VK_CULL_MODE_NONE;
    }
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = (key.rasterization & 0x10) ? VK_TRUE : VK_FALSE;
    rasterization.depthClampEnable = REXCVAR_GET(fh1_native_test_depth_clamp) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const uint32_t d = key.depth;
    VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (key.formats[4]) {
      depth.depthTestEnable = (d >> 1) & 0x1;
      depth.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      depth.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
      if (REXCVAR_GET(fh1_debug_no_depth)) {  // NFSC debug
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
      }
      depth.stencilTestEnable = d & 0x1;
      depth.front.failOp = VkStencilOp((d >> 11) & 0x7);
      depth.front.passOp = VkStencilOp((d >> 14) & 0x7);
      depth.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
      depth.front.compareOp = VkCompareOp((d >> 8) & 0x7);
      if ((d >> 7) & 0x1) {
        depth.back.compareOp = VkCompareOp((d >> 20) & 0x7);
        depth.back.failOp = VkStencilOp((d >> 23) & 0x7);
        depth.back.passOp = VkStencilOp((d >> 26) & 0x7);
        depth.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
      } else {
        depth.back = depth.front;
      }
    }

    static constexpr VkBlendFactor kFactors[32] = {
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    const auto operation = [&](uint32_t op) {
      switch (op) {
        case 1:
          return VK_BLEND_OP_SUBTRACT;
        case 2:
          return VK_BLEND_OP_MIN;
        case 3:
          return VK_BLEND_OP_MAX;
        case 4:
          if (notify) {
            Notify(52, "blend with reverse subtract: subtract is used");
          }
          return VK_BLEND_OP_SUBTRACT;
        default:
          return VK_BLEND_OP_ADD;
      }
    };
    std::array<VkPipelineColorBlendAttachmentState, 4>& blends = fixed.blends;
    uint32_t& n_colors = fixed.n_colors;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!key.formats[i]) {
        continue;
      }
      const uint32_t m = key.blend[i];
      VkPipelineColorBlendAttachmentState& s = blends[n_colors++];
      s.colorWriteMask = (key.masks >> (i * 4)) & 0xF;
      const uint32_t source_2 = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
      const uint32_t source_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                     target_a = (m >> 24) & 0x1F;
      /*
       * Blending is only enabled if it is needed on the channels that are written.
       *
       * It used to require all six fields to be "1 x source + 0 x destination, ADD". But if the mask does
       * not write RGB, the three color factors do not matter, and the same goes for alpha. With blendEnable
       * false the ROP does not have to read the destination or go through the blend unit. The result is the
       * same.
       */
      const bool writes_rgb = (s.colorWriteMask & 0x7) != 0;
      const bool writes_alpha = (s.colorWriteMask & 0x8) != 0;
      const bool color_direct = source_2 == 1 && target == 0 && op == 0;
      const bool alpha_direct = source_a == 1 && target_a == 0 && op_a == 0;
      s.blendEnable = (writes_rgb && !color_direct) || (writes_alpha && !alpha_direct);
      s.srcColorBlendFactor = kFactors[source_2];
      s.dstColorBlendFactor = kFactors[target];
      s.colorBlendOp = operation(op);
      s.srcAlphaBlendFactor = kFactors[source_a];
      s.dstAlphaBlendFactor = kFactors[target_a];
      s.alphaBlendOp = operation(op_a);
    }
    VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = n_colors;
    blend.pAttachments = blends.data();
  }

  // One stencil face, in what Vulkan looks at in the pipeline (masks and reference are dynamic).
  static bool SameFaceStencil(const VkStencilOpState& a, const VkStencilOpState& b) {
    return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
  }

  /*
   * Two fixed states, field by field, in what Vulkan looks at: the blend equation only with blendEnable,
   * each half only if the mask writes its channels, the Z function only with a Z test, stencil operations
   * only with stencil. Returns the first differing field, or nullptr if there is none.
   */
  static const char* DifferenceEffective(const StateFixedPipeline& a, const StateFixedPipeline& b) {
    if (a.assembly.topology != b.assembly.topology) {
      return "topology";
    }
    if (a.assembly.primitiveRestartEnable != b.assembly.primitiveRestartEnable) {
      return "primitiveRestartEnable";
    }
    if (a.view.viewportCount != b.view.viewportCount || a.view.scissorCount != b.view.scissorCount) {
      return "viewportCount o scissorCount";
    }
    const VkPipelineRasterizationStateCreateInfo& ra = a.rasterization;
    const VkPipelineRasterizationStateCreateInfo& rb = b.rasterization;
    if (ra.polygonMode != rb.polygonMode || ra.lineWidth != rb.lineWidth ||
        ra.rasterizerDiscardEnable != rb.rasterizerDiscardEnable || ra.depthClampEnable != rb.depthClampEnable) {
      return "polygonMode, lineWidth, rasterizerDiscardEnable or depthClampEnable";
    }
    if (ra.cullMode != rb.cullMode) {
      return "cullMode";
    }
    if (ra.frontFace != rb.frontFace) {
      return "frontFace";
    }
    if (ra.depthBiasEnable != rb.depthBiasEnable) {
      return "depthBiasEnable";
    }
    if (a.sampling.rasterizationSamples != b.sampling.rasterizationSamples) {
      return "rasterizationSamples";
    }
    const VkPipelineDepthStencilStateCreateInfo& pa = a.depth;
    const VkPipelineDepthStencilStateCreateInfo& pb = b.depth;
    if (pa.depthTestEnable != pb.depthTestEnable) {
      return "depthTestEnable";
    }
    if (pa.depthWriteEnable != pb.depthWriteEnable) {
      return "depthWriteEnable";
    }
    if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
      return "depthCompareOp";
    }
    if (pa.depthBoundsTestEnable != pb.depthBoundsTestEnable) {
      return "depthBoundsTestEnable";
    }
    if (pa.stencilTestEnable != pb.stencilTestEnable) {
      return "stencilTestEnable";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.front, pb.front)) {
      return "front (stencil)";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.back, pb.back)) {
      return "back (stencil)";
    }
    if (a.n_colors != b.n_colors || a.blend.attachmentCount != b.blend.attachmentCount ||
        a.blend.logicOpEnable != b.blend.logicOpEnable) {
      return "attachmentCount o logicOpEnable";
    }
    for (uint32_t i = 0; i < a.n_colors && i < 4; ++i) {
      const VkPipelineColorBlendAttachmentState& x = a.blends[i];
      const VkPipelineColorBlendAttachmentState& y = b.blends[i];
      if (x.colorWriteMask != y.colorWriteMask) {
        return "colorWriteMask";
      }
      if (x.blendEnable != y.blendEnable) {
        return "blendEnable";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x7) &&
          (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
           x.colorBlendOp != y.colorBlendOp)) {
        return "color equation";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x8) &&
          (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
           x.alphaBlendOp != y.alphaBlendOp)) {
        return "alpha equation";
      }
    }
    return nullptr;
  }

  /*
   * Phase 0a (fh1_native_canonical_key). A draw's key in canonical form. Same criterion as
   * StateCanonical (the counter) with two differences: disabled blending becomes 1 x source + 0 x
   * destination, ADD (0x00010001; with 0, FillStateFixed would enable it with zero factors), and with
   * blending enabled the half of the equation (color or alpha) whose channels the mask does not write also
   * becomes that.
   */
  static void Canonicalize(KeyPipeline& c) {
    constexpr uint32_t kDirect = 0x00010001u;  // 1 x source + 0 x destination, ADD, for color and alpha
    uint32_t masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formats[i]) {
        c.blend[i] = 0;  // FillStateFixed skips targets not in the pass (Draw leaves them at 0)
        continue;
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alpha_direct = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool writes_rgb = (mask & 0x7) != 0;
      const bool writes_alpha = (mask & 0x8) != 0;
      if (!((writes_rgb && !color_direct) || (writes_alpha && !alpha_direct))) {
        c.blend[i] = kDirect;  // without blendEnable Vulkan ignores the equation
        continue;
      }
      uint32_t canonical = m;
      if (!writes_rgb) {
        canonical = (canonical & 0xFFFF0000u) | 0x00000001u;  // the color equation reaches no channel
      }
      if (!writes_alpha) {
        canonical = (canonical & 0x0000FFFFu) | 0x00010000u;  // neither does the alpha one
      }
      c.blend[i] = canonical;
    }
    c.masks = masks;
    uint32_t d = c.formats[4] ? c.depth : 0;  // ignored without a depth target
    d &= ~0x8u;                                      // bit 3 is not read
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // without stencil its functions and operations do not count
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    c.depth = d;
  }

  /*
   * The phase 0a guard. Canonicalize only touches blending, masks and depth: the rest of the key must come
   * out the same, and the fixed state of both keys must match in everything Vulkan looks at
   * (DifferenceEffective on FillStateFixed). On a difference the phase switches off for the session and
   * false is returned.
   */
  bool CheckKeyCanonical(const KeyPipeline& raw_value, const KeyPipeline& canonical, uint64_t n) {
    ++canonical_checked_;
    const char* field = nullptr;
    if (raw_value.vs != canonical.vs || raw_value.ps != canonical.ps || raw_value.entry != canonical.entry ||
        raw_value.topology != canonical.topology || raw_value.specialization != canonical.specialization ||
        raw_value.rasterization != canonical.rasterization || raw_value.fill != canonical.fill ||
        raw_value.fill2 != canonical.fill2 ||
        std::memcmp(raw_value.formats, canonical.formats, sizeof(raw_value.formats)) != 0) {
      field = "shaders, input, topology, specialization, rasterization or formats";
    } else {
      StateFixedPipeline a;
      StateFixedPipeline b;
      FillStateFixed(raw_value, a);
      FillStateFixed(canonical, b);
      field = DifferenceEffective(a, b);
    }
    if (field) {
      key_canonical_off_ = true;
      key_canonical_ = false;
      canonical_valid_ = false;
      REXLOG_ERROR("[native] C6 canonical key: DIFFERENCE in {} (check {}: VS {} PS {}, blend {:08X} -> {:08X}, "
                   "masks {:04X} -> {:04X}, depth {:08X} -> {:08X}). Off for the rest of the session: the usual "
                   "key is used",
                   field, n, raw_value.vs, raw_value.ps, raw_value.blend[0], canonical.blend[0], raw_value.masks,
                   canonical.masks, raw_value.depth, canonical.depth);
      return false;
    }
    if (n == kCanonicalACheck) {
      FH1_REPORT_RING("[native] C6 canonical key: {} changed keys checked field by field against the usual one, 0 "
                       "differences; keeps checking 1 in 4096",
                           n);
    }
    return true;
  }

  /*
   * The key used to look up a draw's pipeline. The raw one (key) is still used for everything else: the
   * deferred sky reads its blending (opaque_in_all) and the counter compares it. Phase 0a: canonical
   * form, remembering the previous draw's (the same raw key gives the same canonical one).
   */
  KeyPipeline KeyOfLookup(const KeyPipeline& key) {
    KeyPipeline c = key;
    if (key_canonical_) {
      if (canonical_valid_ && std::memcmp(&key, &canonical_raw_, sizeof(key)) == 0) {
        c = canonical_result_;
      } else {
        Canonicalize(c);
        if (std::memcmp(&c, &key, sizeof(c)) != 0) {
          const uint64_t n = ++canonical_changed_;
          if ((n <= kCanonicalACheck || (n & 4095) == 0) && !CheckKeyCanonical(key, c, n)) {
            c = key;  // the guard saw a difference: this draw, and the rest of the session, use the usual one
          }
        }
        canonical_raw_ = key;
        canonical_result_ = c;
        canonical_valid_ = key_canonical_;
      }
    }
    // Phases 1 and 2. What goes through vkCmdSet* leaves the key, and the flag (fill2) keeps these
    // pipelines apart from the usual ones in the map, in the direct-mapped cache and in PipelineOf's
    // shortcut.
    const uint32_t mode_eds = eds_mode_;
    if (mode_eds & kEds12) {
      c.depth = 0;
      c.rasterization = 0;
      c.topology = RepresentativeTopology(c.topology);
    }
    if (mode_eds & kEds3) {  // phase 2: blending and masks go through vkCmdSet*; targets stay in formats
      std::fill(std::begin(c.blend), std::end(c.blend), 0u);
      c.masks = 0;
    }
    c.fill2 = mode_eds;
    return c;
  }

  // Every 20 s, phase 0a (fh1_native_canonical_key).
  void ReportKeyCanonical() {
    const auto now = std::chrono::steady_clock::now();
    if (now - canonical_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = canonical_report_ == std::chrono::steady_clock::time_point{};
    canonical_report_ = now;
    const uint64_t changed = canonical_changed_ - canonical_changed_previous_;
    canonical_changed_previous_ = canonical_changed_;
    const uint64_t pipelines = pipelines_.size();
    const uint64_t new_items = pipelines - std::min<uint64_t>(pipelines, canonical_pipelines_previous_);
    canonical_pipelines_previous_ = pipelines;
    if (first || (!changed && !new_items)) {
      return;
    }
    FH1_REPORT_RING("[native] C6 canonical key (build 184): {}; {} keys changed in 20 s ({} checked since the "
                     "start, {}); {} pipelines in the map, {} new in 20 s",
                         key_canonical_off_ ? "OFF by the guard"
                         : key_canonical_       ? "on"
                                                 : "off",
                         changed, canonical_checked_,
                         canonical_checked_ >= kCanonicalACheck ? "guard passed" : "checking", pipelines,
                         new_items);
  }

  // Every 20 s, phase 0b (fh1_native_pipeline_between_passes).
  void ReportPipelineBetweenPasses() {
    const auto now = std::chrono::steady_clock::now();
    if (now - between_passes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = between_passes_report_ == std::chrono::steady_clock::time_point{};
    between_passes_report_ = now;
    const uint64_t passes = passes_started_ - between_passes_passes_previous_;
    const uint64_t kept = passes_with_pipeline_ - between_passes_kept_previous_;
    between_passes_passes_previous_ = passes_started_;
    between_passes_kept_previous_ = passes_with_pipeline_;
    if (first || !passes) {
      return;
    }
    FH1_REPORT_RING("[native] C6 pipeline between passes (build 184): {}; {} passes started in 20 s, {} with a "
                     "bound pipeline that is kept (its first draw does not bind again if the key is the same)",
                         pipeline_between_passes_ ? "kept" : "forgotten, as before", passes, kept);
  }

  /*
   * Dynamic state phases 1 and 2. The state that leaves the pipeline, with the Vulkan values the usual
   * pipeline would carry (the criterion of FillStateFixed, which is PipelineOf's).
   */
  struct StateEds {
    uint32_t face = 0;         // VkCullModeFlags
    uint32_t front = 0;       // VkFrontFace
    uint32_t topology = 0;    // VkPrimitiveTopology
    uint32_t reset = 0;     // primitiveRestartEnable
    uint32_t bias = 0;        // depthBiasEnable
    uint32_t test_z = 0;     // depthTestEnable
    uint32_t writes_z = 0;    // depthWriteEnable
    uint32_t function_z = 0;    // VkCompareOp
    uint32_t uses_stencil = 0;     // stencilTestEnable
    uint32_t ahead[4] = {};  // failOp, passOp, depthFailOp (VkStencilOp) y compareOp (VkCompareOp)
    uint32_t behind[4] = {};   // the same for the back face
    uint32_t n_colors = 0;                    // phase 2: the pass's color targets, compacted as in PipelineOf
    VkBool32 blend_active[4] = {};            // blendEnable
    VkColorBlendEquationEXT equation[4] = {};  // color and alpha factors and operations
    VkColorComponentFlags mask[4] = {};     // colorWriteMask
  };

  // Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
  // one per class. Classes the ring does not draw stay as they are.
  static uint32_t RepresentativeTopology(uint32_t topology) {
    switch (ClassTopology(topology)) {
      case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 2:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      default:
        return topology;
    }
  }

  static const char* NameModeEds(uint32_t mode) {
    switch (mode & (kEds12 | kEds3)) {
      case kEds12:
        return "EDS1/EDS2 (phase 1)";
      case kEds3:
        return "EDS3 (phase 2)";
      case kEds12 | kEds3:
        return "EDS1/EDS2 and EDS3 (phases 1 and 2)";
      default:
        return "off: all the state in the pipeline, as before";
    }
  }

  // The dynamic state of a raw key in the given mode (kEds12 | kEds3).
  void StateEdsOf(const KeyPipeline& c, StateEds& e, uint32_t mode) {
    e = StateEds{};
    if (mode & kEds12) {
      e.face = ((c.rasterization & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
               ((c.rasterization & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      e.front = (c.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      e.reset = (c.rasterization & 0x8) ? 1u : 0u;
      e.bias = (c.rasterization & 0x10) ? 1u : 0u;
      e.topology = c.topology;
      if (c.formats[4]) {  // without a depth target everything stays zero, as in FillStateFixed
        const uint32_t d = c.depth;
        e.test_z = (d >> 1) & 0x1;
        e.writes_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
        e.function_z = (d >> 4) & 0x7;
        e.uses_stencil = d & 0x1;
        e.ahead[0] = (d >> 11) & 0x7;
        e.ahead[1] = (d >> 14) & 0x7;
        e.ahead[2] = (d >> 17) & 0x7;
        e.ahead[3] = (d >> 8) & 0x7;
        if ((d >> 7) & 0x1) {
          e.behind[0] = (d >> 23) & 0x7;
          e.behind[1] = (d >> 26) & 0x7;
          e.behind[2] = (d >> 29) & 0x7;
          e.behind[3] = (d >> 20) & 0x7;
        } else {
          std::memcpy(e.behind, e.ahead, sizeof(e.behind));
        }
      }
    }
    if (mode & kEds3) {  // phase 2: like FillStateFixed, target by target and compacted
      for (uint32_t i = 0; i < 4; ++i) {
        if (!c.formats[i]) {
          continue;
        }
        const uint32_t a = e.n_colors++;
        const uint32_t m = c.blend[i];
        const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
        const uint32_t source_2 = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
        const uint32_t source_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, target_a = (m >> 24) & 0x1F;
        const bool color_direct = source_2 == 1 && target == 0 && op == 0;
        const bool alpha_direct = source_a == 1 && target_a == 0 && op_a == 0;
        e.mask[a] = mask;
        e.blend_active[a] =
            (((mask & 0x7) != 0 && !color_direct) || ((mask & 0x8) != 0 && !alpha_direct)) ? VK_TRUE : VK_FALSE;
        VkColorBlendEquationEXT& q = e.equation[a];
        q.srcColorBlendFactor = kFactorsBlend[source_2];
        q.dstColorBlendFactor = kFactorsBlend[target];
        q.colorBlendOp = OperationBlend(op);
        q.srcAlphaBlendFactor = kFactorsBlend[source_a];
        q.dstAlphaBlendFactor = kFactorsBlend[target_a];
        q.alphaBlendOp = OperationBlend(op_a);
      }
    }
  }

  /*
   * Records the vkCmdSet* calls for what changed since the last state set in the buffer (eds_recorded_),
   * or for everything with all_state = true, and tracks it. What Vulkan ignores in this draw (the Z function
   * without a Z test, stencil operations without stencil, the blend equation without blending) is not
   * re-recorded if only that changed; with all_state = true it is recorded anyway, so everything is set at least
   * once in the buffer.
   */
  void EmitStateDynamic(VkCommandBuffer cmd, const StateEds& d, bool all_state, uint32_t mode) {
    StateEds& g = eds_recorded_;
    eds_calls_[kEdsAllState] += all_state ? 1 : 0;
    if (mode & kEds12) {
      if (all_state || d.face != g.face) {
        set_face_(cmd, VkCullModeFlags(d.face));
        g.face = d.face;
        ++eds_calls_[kEdsFace];
      }
      if (all_state || d.front != g.front) {
        set_front_(cmd, VkFrontFace(d.front));
        g.front = d.front;
        ++eds_calls_[kEdsFront];
      }
      if (all_state || d.topology != g.topology) {
        set_topology_(cmd, VkPrimitiveTopology(d.topology));
        g.topology = d.topology;
        ++eds_calls_[kEdsTopology];
      }
      if (all_state || d.reset != g.reset) {
        set_reset_(cmd, VkBool32(d.reset));
        g.reset = d.reset;
        ++eds_calls_[kEdsReset];
      }
      if (all_state || d.bias != g.bias) {
        set_bias_(cmd, VkBool32(d.bias));
        g.bias = d.bias;
        ++eds_calls_[kEdsBias];
      }
      if (all_state || d.test_z != g.test_z) {
        set_test_z_(cmd, VkBool32(d.test_z));
        g.test_z = d.test_z;
        ++eds_calls_[kEdsTestZ];
      }
      if (all_state || d.writes_z != g.writes_z) {
        set_writes_z_(cmd, VkBool32(d.writes_z));
        g.writes_z = d.writes_z;
        ++eds_calls_[kEdsWritesZ];
      }
      if (all_state || (d.test_z && d.function_z != g.function_z)) {
        set_function_z_(cmd, VkCompareOp(d.function_z));
        g.function_z = d.function_z;
        ++eds_calls_[kEdsFunctionZ];
      }
      if (all_state || d.uses_stencil != g.uses_stencil) {
        set_stencil_(cmd, VkBool32(d.uses_stencil));
        g.uses_stencil = d.uses_stencil;
        ++eds_calls_[kEdsStencil];
      }
      const bool ahead = all_state || (d.uses_stencil && std::memcmp(d.ahead, g.ahead, sizeof(d.ahead)) != 0);
      const bool behind = all_state || (d.uses_stencil && std::memcmp(d.behind, g.behind, sizeof(d.behind)) != 0);
      if (ahead && behind && std::memcmp(d.ahead, d.behind, sizeof(d.ahead)) == 0) {
        set_stencil_ops_(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                          VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
        ++eds_calls_[kEdsStencilOps];
      } else {
        if (ahead) {
          set_stencil_ops_(cmd, VK_STENCIL_FACE_FRONT_BIT, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                            VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
          ++eds_calls_[kEdsStencilOps];
        }
        if (behind) {
          set_stencil_ops_(cmd, VK_STENCIL_FACE_BACK_BIT, VkStencilOp(d.behind[0]), VkStencilOp(d.behind[1]),
                            VkStencilOp(d.behind[2]), VkCompareOp(d.behind[3]));
          ++eds_calls_[kEdsStencilOps];
        }
      }
      if (ahead) {
        std::memcpy(g.ahead, d.ahead, sizeof(g.ahead));
      }
      if (behind) {
        std::memcpy(g.behind, d.behind, sizeof(g.behind));
      }
    }
    if (mode & kEds3) {  // phase 2
      const uint32_t n = d.n_colors;
      if (!n) {
        if (all_state) {
          g.n_colors = 0;  // nothing set in this buffer: the first draw with color will set it all
        }
      } else {
        const bool other_n = all_state || n != g.n_colors;
        bool active = other_n;
        bool equation = other_n;
        bool mask = other_n;
        for (uint32_t a = 0; a < n && a < 4; ++a) {
          active = active || d.blend_active[a] != g.blend_active[a];
          mask = mask || d.mask[a] != g.mask[a];
          if (!equation && d.blend_active[a]) {  // without blending the equation does not count; each half, with its channels
            const VkColorBlendEquationEXT& x = d.equation[a];
            const VkColorBlendEquationEXT& y = g.equation[a];
            equation = ((d.mask[a] & 0x7) != 0 &&
                        (x.srcColorBlendFactor != y.srcColorBlendFactor ||
                         x.dstColorBlendFactor != y.dstColorBlendFactor || x.colorBlendOp != y.colorBlendOp)) ||
                       ((d.mask[a] & 0x8) != 0 &&
                        (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor ||
                         x.dstAlphaBlendFactor != y.dstAlphaBlendFactor || x.alphaBlendOp != y.alphaBlendOp));
          }
        }
        if (active) {
          set_blend_active_(cmd, 0, n, d.blend_active);
          std::memcpy(g.blend_active, d.blend_active, sizeof(g.blend_active));
          ++eds_calls_[kEdsBlendActive];
        }
        if (equation) {
          set_equation_(cmd, 0, n, d.equation);
          std::memcpy(g.equation, d.equation, sizeof(g.equation));
          ++eds_calls_[kEdsEquation];
        }
        if (mask) {
          set_mask_(cmd, 0, n, d.mask);
          std::memcpy(g.mask, d.mask, sizeof(g.mask));
          ++eds_calls_[kEdsMask];
        }
        g.n_colors = n;
      }
    }
  }

  /*
   * The guard of phases 1 and 2. What the ring believes is set in the buffer (eds_recorded_) must be what
   * this draw's usual pipeline would carry. That state comes from FillStateFixed with the raw key, the
   * same code PipelineOf uses to create the usual pipelines (not another copy of StateEdsOf), and it is
   * compared field by field in what Vulkan looks at. On a difference dynamic state switches off for the
   * session (both phases) and false is returned: the draw has to use its usual pipeline. Limit: it cannot
   * see the GPU, only that the computations and the record of what was recorded agree.
   */
  bool CheckStateDynamic(const KeyPipeline& key, uint64_t n) {
    ++eds_checked_;
    StateFixedPipeline fixed;
    FillStateFixed(key, fixed);
    const StateEds& g = eds_recorded_;
    const char* field = nullptr;
    uint32_t fixed_2 = 0;
    uint32_t expected = 0;
    const auto look = [&](const char* name, uint32_t a, uint32_t b) {
      if (!field && a != b) {
        field = name;
        fixed_2 = a;
        expected = b;
      }
    };
    if (eds_mode_ & kEds12) {
      const VkPipelineRasterizationStateCreateInfo& r = fixed.rasterization;
      const VkPipelineDepthStencilStateCreateInfo& z = fixed.depth;
      look("cullMode", g.face, r.cullMode);
      look("frontFace", g.front, r.frontFace);
      look("topology", g.topology, fixed.assembly.topology);
      look("primitiveRestartEnable", g.reset, fixed.assembly.primitiveRestartEnable);
      look("depthBiasEnable", g.bias, r.depthBiasEnable);
      look("depthTestEnable", g.test_z, z.depthTestEnable);
      look("depthWriteEnable", g.writes_z, z.depthWriteEnable);
      if (z.depthTestEnable) {
        look("depthCompareOp", g.function_z, z.depthCompareOp);
      }
      look("stencilTestEnable", g.uses_stencil, z.stencilTestEnable);
      if (z.stencilTestEnable) {
        look("front.failOp", g.ahead[0], z.front.failOp);
        look("front.passOp", g.ahead[1], z.front.passOp);
        look("front.depthFailOp", g.ahead[2], z.front.depthFailOp);
        look("front.compareOp", g.ahead[3], z.front.compareOp);
        look("back.failOp", g.behind[0], z.back.failOp);
        look("back.passOp", g.behind[1], z.back.passOp);
        look("back.depthFailOp", g.behind[2], z.back.depthFailOp);
        look("back.compareOp", g.behind[3], z.back.compareOp);
      }
    }
    if ((eds_mode_ & kEds3) && fixed.n_colors) {  // phase 2 (without color targets there is nothing to check)
      look("attachmentCount", g.n_colors, fixed.n_colors);
      for (uint32_t a = 0; a < fixed.n_colors && a < 4; ++a) {
        const VkPipelineColorBlendAttachmentState& s = fixed.blends[a];
        const VkColorBlendEquationEXT& q = g.equation[a];
        look("colorWriteMask", g.mask[a], s.colorWriteMask);
        look("blendEnable", g.blend_active[a], s.blendEnable);
        if (s.blendEnable && (s.colorWriteMask & 0x7)) {
          look("srcColorBlendFactor", q.srcColorBlendFactor, s.srcColorBlendFactor);
          look("dstColorBlendFactor", q.dstColorBlendFactor, s.dstColorBlendFactor);
          look("colorBlendOp", q.colorBlendOp, s.colorBlendOp);
        }
        if (s.blendEnable && (s.colorWriteMask & 0x8)) {
          look("srcAlphaBlendFactor", q.srcAlphaBlendFactor, s.srcAlphaBlendFactor);
          look("dstAlphaBlendFactor", q.dstAlphaBlendFactor, s.dstAlphaBlendFactor);
          look("alphaBlendOp", q.alphaBlendOp, s.alphaBlendOp);
        }
      }
    }
    if (field) {
      eds_off_ = true;
      eds_mode_ = 0;
      eds_valid_ = false;
      pipeline_bound_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[native] C6 dynamic state: DIFFERENCE in {} (check {}: set {} and the usual pipeline would "
                   "have {}; VS {} PS {}, topology {}, depth {:08X}, rasterization {:02X}, masks {:04X}). Off for "
                   "the rest of the session: the pipelines with all the state fixed come back",
                   field, n, fixed_2, expected, key.vs, key.ps, key.topology, key.depth,
                   key.rasterization, key.masks);
      return false;
    }
    if (n == kEdsACheck) {
      FH1_REPORT_RING("[native] C6 dynamic state: {} key changes checked against the state of their usual "
                       "pipeline, 0 differences; keeps checking 1 in 4096",
                           n);
    }
    return true;
  }

  /*
   * Phases 1 and 2, before every draw with dynamic state. With the same raw key as the last state set in
   * this buffer there is nothing to do; otherwise what changed is set and the guard checks it for the
   * first 200,000 changes and then 1 in 4,096. Returns false if the guard has switched dynamic state off.
   */
  bool FixStateDynamic(VkCommandBuffer cmd, const KeyPipeline& key) {
    ++eds_draws_;
    if (eds_valid_ && std::memcmp(&key, &eds_key_, sizeof(key)) == 0) {
      ++eds_repeated_;
      return true;
    }
    StateEds d;
    StateEdsOf(key, d, eds_mode_);
    EmitStateDynamic(cmd, d, !eds_valid_, eds_mode_);
    eds_key_ = key;
    eds_valid_ = true;
    const uint64_t n = ++eds_checkable_;
    if (n <= kEdsACheck || (n & 4095) == 0) {
      return CheckStateDynamic(key, n);
    }
    return true;
  }

  /*
   * Dynamic state phases 1 and 2. The EDS1/EDS2 vkCmdSet* functions are core in Vulkan 1.3 and the SDK's
   * table does not load them: they are requested from the driver, like vkCmdCopyImage. With an API below
   * 1.3 or with any of them missing, phase 1 is not used for the whole session and one log line says so.
   */
  void LoadStateDynamic() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto request_2 = [&](const char* name) { return ifn.vkGetDeviceProcAddr(device_, name); };
    set_face_ = reinterpret_cast<PFN_vkCmdSetCullMode>(request_2("vkCmdSetCullMode"));
    set_front_ = reinterpret_cast<PFN_vkCmdSetFrontFace>(request_2("vkCmdSetFrontFace"));
    set_topology_ = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(request_2("vkCmdSetPrimitiveTopology"));
    set_reset_ = reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(request_2("vkCmdSetPrimitiveRestartEnable"));
    set_bias_ = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(request_2("vkCmdSetDepthBiasEnable"));
    set_test_z_ = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(request_2("vkCmdSetDepthTestEnable"));
    set_writes_z_ = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(request_2("vkCmdSetDepthWriteEnable"));
    set_function_z_ = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(request_2("vkCmdSetDepthCompareOp"));
    set_stencil_ = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(request_2("vkCmdSetStencilTestEnable"));
    set_stencil_ops_ = reinterpret_cast<PFN_vkCmdSetStencilOp>(request_2("vkCmdSetStencilOp"));
    const uint32_t api = vulkan_device_->properties().apiVersion;
    eds12_available_ = api >= VK_MAKE_API_VERSION(0, 1, 3, 0) && set_face_ && set_front_ && set_topology_ &&
                        set_reset_ && set_bias_ && set_test_z_ && set_writes_z_ && set_function_z_ &&
                        set_stencil_ && set_stencil_ops_;
    REXLOG_INFO("[native] C6 dynamic state (build 184): EDS1/EDS2 {} (device API {}.{}.{})",
                eds12_available_ ? "available (core 1.3)" : "NOT available: phase 1 is not used",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
#if defined(REX_UI_VULKAN_STATE_DYNAMIC3)
    // Phase 2. The driver only provides these three if the SDK enabled the extension; the SDK records the
    // three features in its properties (ui_vulkan_state_dynamic3.patch).
    set_blend_active_ = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(request_2("vkCmdSetColorBlendEnableEXT"));
    set_equation_ = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(request_2("vkCmdSetColorBlendEquationEXT"));
    set_mask_ = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(request_2("vkCmdSetColorWriteMaskEXT"));
    const auto& p3 = vulkan_device_->properties();
    eds3_available_ = vulkan_device_->extensions().ext_EXT_extended_dynamic_state3 &&
                       p3.extendedDynamicState3ColorBlendEnable && p3.extendedDynamicState3ColorBlendEquation &&
                       p3.extendedDynamicState3ColorWriteMask && set_blend_active_ && set_equation_ && set_mask_;
    REXLOG_INFO("[native] C6 dynamic state (build 184): EDS3 (blend, equation and color mask) {}",
                eds3_available_ ? "available"
                                 : "NOT available (the device does not offer it or it is not enabled): phase 2 is "
                                   "not used");
#else
    REXLOG_INFO("[native] C6 dynamic state (build 184): EDS3 NOT available: this SDK does not enable "
                "VK_EXT_extended_dynamic_state3 (ui_vulkan_state_dynamic3.patch is missing); phase 2 is not used");
#endif
  }

  // Every 20 s, dynamic state phases 1 and 2.
  void ReportStateDynamic() {
    const auto now = std::chrono::steady_clock::now();
    if (now - eds_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = eds_report_ == std::chrono::steady_clock::time_point{};
    eds_report_ = now;
    const std::array<uint64_t, kEdsN> n = eds_calls_;
    eds_calls_.fill(0);
    const uint64_t draws = eds_draws_ - eds_draws_previous_;
    const uint64_t repeated = eds_repeated_ - eds_repeated_previous_;
    const uint64_t frames = frame_ - eds_frames_previous_;
    eds_draws_previous_ = eds_draws_;
    eds_repeated_previous_ = eds_repeated_;
    eds_frames_previous_ = frame_;
    if (first || !draws) {
      return;
    }
    uint64_t calls = 0;
    for (size_t k = 0; k < kEdsN; ++k) {
      calls += k == kEdsAllState ? 0 : n[k];
    }
    FH1_REPORT_RING(
        "[native] C6 dynamic state (build 184): {}; {} draws in 20 s ({:.0f} per frame; {} repeat the previous "
        "key) and {} vkCmdSet* ({:.3f} per draw; {} times everything: new buffer, sky or pass that forgets the "
        "pipeline) | cull face {}, front face {}, topology {}, restart {}, bias {}, Z test {}, Z write {}, Z "
        "function {}, stencil {}, stencil ops {} | blend {}, equation {}, masks {} | pipelines created: {} usual, "
        "{} EDS1/EDS2, {} EDS3, {} with both | guard: {} checked ({})",
        eds_off_ ? "OFF by the guard" : NameModeEds(eds_mode_), draws,
        frames ? double(draws) / double(frames) : 0.0, repeated, calls,
        double(calls) / double(draws), n[kEdsAllState], n[kEdsFace], n[kEdsFront], n[kEdsTopology],
        n[kEdsReset], n[kEdsBias], n[kEdsTestZ], n[kEdsWritesZ], n[kEdsFunctionZ], n[kEdsStencil],
        n[kEdsStencilOps], n[kEdsBlendActive], n[kEdsEquation], n[kEdsMask], pipelines_by_mode_[0],
        pipelines_by_mode_[kEds12], pipelines_by_mode_[kEds3], pipelines_by_mode_[kEds12 | kEds3],
        eds_checked_, eds_checked_ >= kEdsACheck ? "guard passed" : "checking");
  }

  // Phase 2. The blend operation of a register field, like the lambda in FillStateFixed.
  VkBlendOp OperationBlend(uint32_t op) {
    switch (op) {
      case 1:
        return VK_BLEND_OP_SUBTRACT;
      case 2:
        return VK_BLEND_OP_MIN;
      case 3:
        return VK_BLEND_OP_MAX;
      case 4:
        Notify(52, "blend with reverse subtract: subtract is used");
        return VK_BLEND_OP_SUBTRACT;
      default:
        return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline PipelineDe(const KeyPipeline& key, const EntryVertices& entry,
                        const RequestDraw& p) {
    /*
     * One-entry shortcut.
     *
     * There are 105 pipelines in a whole race and 2,345 draws per frame, so consecutive draws almost always
     * repeat the key. The full lookup is an XXH3 of 80 bytes + an integer division by libstdc++'s prime
     * bucket count + two pointer hops (bucket and node, ~96 B) = 2-3 cache misses, all to end up comparing
     * the same 80 bytes this compares. Here there is just one memcmp on memory that is already hot.
     */
    if (last_key_valid_ && std::memcmp(&last_key_, &key, sizeof(key)) == 0) {
      return last_pipeline_;
    }
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    // The direct-mapped cache (fh1_native_direct_pipelines) before the map.
    SlotPipeline& slot_2 = slots_pipeline_[fingerprint & (kSlotsPipeline - 1)];
    if (pipelines_direct_ && slot_2.pipeline != VK_NULL_HANDLE &&
        std::memcmp(&slot_2.key, &key, sizeof(key)) == 0) {
      const uint64_t n = ++pipelines_direct_hits_;
      if (!(n <= kPipelinesACheck || (n & 4095) == 0) ||
          CheckSlotPipeline(fingerprint, key, slot_2.pipeline, n)) {
        last_key_ = key;
        last_pipeline_ = slot_2.pipeline;
        last_key_valid_ = true;
        return slot_2.pipeline;
      }
      // the guard saw a difference: this draw goes through the map, as before
    }
    if (pipelines_direct_) {
      ++pipelines_direct_misses_;
    }
    if (const auto it = pipelines_.find(fingerprint); it != pipelines_.end()) {
      if (std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
        last_key_ = key;
        last_pipeline_ = it->second.second;
        last_key_valid_ = true;
        if (pipelines_direct_) {  // the slot keeps the map's pair
          slot_2.key = key;
          slot_2.pipeline = it->second.second;
        }
        return it->second.second;
      }
      Notify(50, "pipeline fingerprint collision");
      return VK_NULL_HANDLE;
    }
    const VkShaderModule vs = ModuleOf(*p.vs);
    VkShaderModule ps = VK_NULL_HANDLE;
    if (key.ps && (key.specialization & kSpecGlowNatural)) {
      ps = ModuleVariant(1, kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
    } else if (key.ps && (key.specialization & kSpecGlowSoft)) {
      ps = ModuleVariant(2, kSpirvGlowSoft, sizeof(kSpirvGlowSoft));
    } else if (key.ps && (key.specialization & kSpecOnlyAlpha)) {
      ps = ModuleOnlyAlpha(*p.ps);
    } else if (key.ps && (key.specialization & kSpecZEarly)) {
      ps = ModuleZEarly(*p.ps);
    } else if (key.ps) {
      ps = ModuleOf(*p.ps);
    }
    if (vs == VK_NULL_HANDLE || (key.ps && ps == VK_NULL_HANDLE)) {
      Reject(51, "could not create a shader module");
      return VK_NULL_HANDLE;
    }
    // The VkGraphicsPipelineCreateInfo lives in CreatePipelineVulkan, the same function the prewarm uses from
    // its thread; what surrounds it stays here, as it was.
    uint32_t n_colors = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto start_creation = std::chrono::steady_clock::now();
    if (CreatePipelineVulkan(key, entry, vs, ps, pass_rp_, true, pipeline, n_colors) != VK_SUCCESS) {
      Reject(53, "could not create a pipeline");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++pipelines_without_save_;
    }
    const uint64_t ns_creation = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start_creation)
                                              .count());
    ns_pipelines_ += ns_creation;
    NotePipelineCreated(key, entry, p, pipeline, ns_creation);  // List and measurement
    pipelines_.emplace(fingerprint, std::make_pair(key, pipeline));
    ++pipelines_by_mode_[key.fill2 & 3];  // Dynamic state report
    if (pipelines_direct_ && pipeline != VK_NULL_HANDLE) {  // the same pair as the map
      slot_2.key = key;
      slot_2.pipeline = pipeline;
    }
    if (pipelines_.size() <= 64) {
      REXLOG_INFO("[native] C6: pipeline {} (VS n{} PS n{}, topology {}, {} colors, depth {:08X}, blend {:08X}, "
                  "specialization {})",
                  pipelines_.size(), p.vs->number, key.ps ? int(p.ps->number) : -1,
                  key.topology, n_colors,
                  key.depth, key.blend[0], key.specialization);
    }
    return pipeline;
  }

  // NFSC: the RECTANGLE_LIST geometry shader (fh1_rect_gs_spirv.h), created once; it is also called from the prewarm thread.
  VkShaderModule ModuleGeometryRectangles() {
    std::lock_guard<std::mutex> lock(mutex_gs_rect_);
    if (module_gs_rect_ == VK_NULL_HANDLE) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = sizeof(fh1::kRectGsSpirv);
      info.pCode = fh1::kRectGsSpirv;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_gs_rect_) != VK_SUCCESS) {
        module_gs_rect_ = VK_NULL_HANDLE;
      }
    }
    return module_gs_rect_;
  }
  // NFSC: a full memory barrier between GPU work items (copies, clears, render passes). The renderer came from a
  // GPU/driver that serialized these by itself; on AMD (and per the Vulkan spec) a copy could read a render target
  // while it was still being drawn or already being cleared: black scene, 8x8 compressed-tile garbage.
  void BarrierGlobal(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE || !REXCVAR_GET(fh1_barriers)) {
      return;
    }
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                              &barrier, 0, nullptr, 0, nullptr);
  }
  VkShaderModule ModulePixelFlat() {
    std::lock_guard<std::mutex> lock(mutex_gs_rect_);
    if (module_ps_flat_ == VK_NULL_HANDLE) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = sizeof(fh1::kFlatPsSpirv);
      info.pCode = fh1::kFlatPsSpirv;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_ps_flat_) != VK_SUCCESS) {
        module_ps_flat_ = VK_NULL_HANDLE;
      }
    }
    return module_ps_flat_;
  }
  VkShaderModule module_ps_flat_ = VK_NULL_HANDLE;
  VkShaderModule ModulePixelView() {
    std::lock_guard<std::mutex> lock(mutex_gs_rect_);
    const int32_t k = REXCVAR_GET(fh1_debug_view_interp);
    if (module_ps_view_ == VK_NULL_HANDLE) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = fh1::kViewPsBytes[k];
      info.pCode = fh1::kViewPsSpirv[k];
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_ps_view_) != VK_SUCCESS) {
        module_ps_view_ = VK_NULL_HANDLE;
      }
    }
    return module_ps_view_;
  }
  VkShaderModule module_ps_view_ = VK_NULL_HANDLE;
  std::mutex mutex_gs_rect_;
  VkShaderModule module_gs_rect_ = VK_NULL_HANDLE;

  /*
   * The Vulkan pipeline for a key. It is PipelineOf's usual code, moved here without changes except for
   * the render pass (now an argument) and notify, so the prewarm can create from its thread exactly the
   * same VkGraphicsPipelineCreateInfo as the ring. It only reads its arguments, layout_pipeline_ and the
   * pipeline cache, which Vulkan synchronizes internally; notify = false outside the ring.
   */
  VkResult CreatePipelineVulkan(const KeyPipeline& key, const EntryVertices& entry, VkShaderModule vs,
                               VkShaderModule ps, VkRenderPass pass, bool notify, VkPipeline& pipeline,
                               uint32_t& n_colors_output) {
    const VkSpecializationMapEntry map{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo specialization{1, &map, sizeof(uint32_t), &key.specialization};
    VkPipelineShaderStageCreateInfo stages[3]{};
    for (auto& stage : stages) {
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage.pName = "main";
      stage.pSpecializationInfo = &specialization;
    }
    uint32_t n_stages = 0;
    stages[n_stages].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[n_stages++].module = vs;
    if (key.fill & 1) {  // NFSC: RECTANGLE_LIST
      stages[n_stages].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
      stages[n_stages].pSpecializationInfo = nullptr;
      stages[n_stages++].module = ModuleGeometryRectangles();
    }
    if (key.ps) {
      stages[n_stages].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[n_stages++].module = REXCVAR_GET(fh1_debug_flat_ps)        ? ModulePixelFlat()
                                  : REXCVAR_GET(fh1_debug_view_interp) >= 0 ? ModulePixelView()
                                                                           : ps;
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < entry.bindings.size(); ++i) {
      bindings.push_back({i, entry.bindings[i].stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const AttributeVertices& a : entry.attributes) {
      attributes.push_back({a.location, a.binding, a.format, a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertices.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertices.pVertexBindingDescriptions = bindings.data();
    vertices.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertices.pVertexAttributeDescriptions = attributes.data();

    // The fixed state is filled in by FillStateFixed: it is the usual code, moved there without
    // changing any computation, so the guards of the canonical key and of dynamic state compare with what
    // really goes into the VkGraphicsPipelineCreateInfo.
    StateFixedPipeline fixed;
    FillStateFixed(key, fixed, notify);  // notify
    const VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    const VkPipelineViewportStateCreateInfo& view = fixed.view;
    const VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    const VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    const VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    const VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    const uint32_t n_colors = fixed.n_colors;

    // Dynamic state phases 1 and 2. Their pipelines (flagged in fill2) also declare as dynamic what is
    // now set with vkCmdSet*; their lookup key already has it zeroed (KeyOfLookup).
    VkDynamicState dynamic_2[24] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    uint32_t n_dynamic = 7;
    if (key.fill2 & kEds12) {
      for (const VkDynamicState state : kDynamicEds12) {
        dynamic_2[n_dynamic++] = state;
      }
    }
    if (key.fill2 & kEds3) {
      for (const VkDynamicState state : kDynamicEds3) {
        dynamic_2[n_dynamic++] = state;
      }
    }
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = n_dynamic;
    dynamic.pDynamicStates = dynamic_2;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = n_stages;  // mode 5: VS only (no fragment stage); NFSC: plus the geometry stage for rectangle lists
    info.pStages = stages;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &sampling;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_pipeline_;
    info.renderPass = pass;  // the pass's (ring) or a compatible one (prewarm)
    info.basePipelineIndex = -1;
    n_colors_output = n_colors;
    pipeline = VK_NULL_HANDLE;
    return dfn_.vkCreateGraphicsPipelines(device_, cache_pipelines_, 1, &info, nullptr, &pipeline);
  }

  // --- Pipeline prewarming (fh1_native_pipelines_prewarm) ---------------------------
  /*
   * Why. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms each on
   * the console. In a first race after the library and the key had changed there were 56 (5.2 s of
   * stutter at the start). It happens the first time after any change to the library, the driver or the
   * key, and on a fresh install.
   *
   * How. The ring records every pipeline it creates (NotePipelineCreated) and the list is saved with the
   * cache (SaveCachePipelines). In the next session, as soon as the library is loaded
   * (TryPrewarm), a lowest-priority thread (LoopPrewarm) walks the list in creation order
   * (the menu first) and recreates each pipeline with CreatePipelineVulkan, the same function the ring
   * uses, with modules from the same SPIR-V (the same variants and fallbacks as PipelineOf) and a
   * compatible render pass (CreatePass, the same formats); then destroys it. The Vulkan cache keeps the
   * compiled shaders, so when the ring asks for it, it comes from the cache (0-2 ms, like the 8 of the
   * menu). The ring changes nothing of what it draws: it only reads two counters of the thread for the
   * report and the guard.
   *
   * Guard. The thread's pipelines are destroyed unused: they cannot change the image. What can fail is
   * that they are not the ones the ring asks for. The report measures how long the ring takes to create
   * the listed pipelines the thread already prewarmed: if at least half of 8 or more take as long as a
   * compiled one (20 ms or more), it writes DIFFERENCE and stops the thread. Records whose shaders are no
   * longer in the library, or from another dynamic state mode, are skipped.
   */
  enum : uint8_t { kListPending = 0, kListPrewarm = 1, kListWithoutShader = 2, kListFailed = 3,
                   kListOtherMode = 4 };
  static constexpr uint64_t kNsCompiled = 5000000;  // more than this: really compiled (a cache hit is 0-2 ms)
  static constexpr uint64_t kNsSlow = 20000000;     // the ring creating one already prewarmed: mismatch

  // At start-up (Initialize): the previous session's list, before the thread exists. It is not a
  // separate file: it is the part of the pipelines file that LoadCachePipelines read.
  void LoadListPipelines() {
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data = std::move(list_read_);
    list_read_ = {};
    if (data.size() < kHeaderList ||
        data.size() > kHeaderList + kMaxRegistersList * sizeof(RegisterPipeline)) {
      data.clear();
    }
    const char* reason = nullptr;
    uint32_t header[4] = {};
    if (data.empty()) {
      reason = "no list: starting in this session";
    } else {
      std::memcpy(header, data.data(), sizeof(header));
      if (header[0] != kMagicListPipelines || header[1] != kVersionListPipelines ||
          header[2] != sizeof(RegisterPipeline) ||
          data.size() != kHeaderList + size_t(header[3]) * sizeof(RegisterPipeline)) {
        reason = "list of another version or damaged: starting from scratch";
      }
    }
    if (!reason) {
      list_file_.reserve(header[3]);
      for (uint32_t i = 0; i < header[3]; ++i) {
        RegisterPipeline r;
        std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(RegisterPipeline), sizeof(r));
        if (r.n_attributes > RegisterPipeline::kMaxAttributes || r.n_bindings > RegisterPipeline::kMaxBindings) {
          continue;
        }
        if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), list_file_.size()).second) {
          list_file_.push_back(r);
        }
      }
      written_list_ = std::move(data);  // the writer thread rewrites it unchanged if only the cache changes
    }
    state_list_.assign(list_file_.size(), kListPending);
    REXLOG_INFO("[native] C6 prewarm (build 186): {} pipelines in the list of {}{}{}", list_file_.size(),
                path.string(), reason ? ": " : "", reason ? reason : "");
  }

  // Ring only (SaveCachePipelines): the file's list without the records the thread found missing their
  // shaders, followed by this session's new ones. Above kMaxRegistersList the oldest are dropped.
  std::vector<uint8_t> SerializeListPipelines() const {
    const size_t until = prewarm_until_.load(std::memory_order_acquire);
    std::vector<const RegisterPipeline*> register_values;
    register_values.reserve(list_file_.size() + list_session_.size());
    for (size_t i = 0; i < list_file_.size(); ++i) {
      if (i < until && state_list_[i] == kListWithoutShader) {
        continue;
      }
      register_values.push_back(&list_file_[i]);
    }
    for (const RegisterPipeline& r : list_session_) {
      register_values.push_back(&r);
    }
    if (register_values.size() > kMaxRegistersList) {
      register_values.erase(register_values.begin(), register_values.begin() + std::ptrdiff_t(register_values.size() - kMaxRegistersList));
    }
    std::vector<uint8_t> data(kHeaderList + register_values.size() * sizeof(RegisterPipeline));
    const uint32_t header[4] = {kMagicListPipelines, kVersionListPipelines, uint32_t(sizeof(RegisterPipeline)),
                                  uint32_t(register_values.size())};
    std::memcpy(data.data(), header, sizeof(header));
    for (size_t i = 0; i < register_values.size(); ++i) {
      std::memcpy(data.data() + kHeaderList + i * sizeof(RegisterPipeline), register_values[i], sizeof(RegisterPipeline));
    }
    return data;
  }

  // Ring only (PipelineOf), with every pipeline it creates: the measurement for the report and the guard,
  // and new ones go to the list.
  void NotePipelineCreated(const KeyPipeline& key, const EntryVertices& entry, const RequestDraw& p,
                            VkPipeline pipeline, uint64_t ns) {
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    if (const auto it = index_list_.find(fingerprint); it != index_list_.end()) {
      const size_t j = it->second;
      if (j < list_file_.size() && j < prewarm_until_.load(std::memory_order_acquire) &&
          state_list_[j] == kListPrewarm) {
        ++ring_prewarmed_;
        ns_ring_prewarmed_ += ns;
        if (ns >= kNsSlow) {
          ++ring_prewarmed_slow_;
        }
      } else {
        ++ring_of_list_;
        ns_ring_of_list_ += ns;
      }
      return;
    }
    ++ring_new_;
    ns_ring_new_ += ns;
    if (pipeline == VK_NULL_HANDLE || !p.vs || !p.vs->shader || (key.ps && (!p.ps || !p.ps->shader)) ||
        entry.attributes.size() > RegisterPipeline::kMaxAttributes ||
        entry.bindings.size() > RegisterPipeline::kMaxBindings ||
        list_file_.size() + list_session_.size() >= kMaxRegistersList) {
      return;
    }
    RegisterPipeline r;
    r.key = key;
    r.fingerprint_vs = p.vs->shader->fingerprint;
    r.fingerprint_ps = key.ps ? p.ps->shader->fingerprint : 0;
    r.n_attributes = uint32_t(entry.attributes.size());
    for (uint32_t k = 0; k < r.n_attributes; ++k) {
      const AttributeVertices& a = entry.attributes[k];
      r.attributes[k] = {a.location, a.binding, uint32_t(a.format), a.offset};
    }
    r.n_bindings = uint32_t(entry.bindings.size());
    for (uint32_t k = 0; k < r.n_bindings; ++k) {
      r.strides[k] = entry.bindings[k].stride;
    }
    index_list_.emplace(fingerprint, SIZE_MAX);  // from this session: not recorded again
    list_session_.push_back(r);
    ++list_without_save_;
    ++ring_a_list_;
  }

  // Ring only, on each submission: starts the thread once, as soon as the library is loaded.
  void TryPrewarm() {
    if (prewarm_decided_) {
      return;
    }
    if (list_file_.empty() || cache_pipelines_ == VK_NULL_HANDLE ||
        !REXCVAR_GET(fh1_native_pipelines_prewarm)) {
      prewarm_decided_ = true;
      if (!list_file_.empty()) {
        REXLOG_INFO("[native] C6 prewarm (build 186): not prewarming ({})",
                    cache_pipelines_ == VK_NULL_HANDLE ? "without a pipeline cache"
                                                       : "fh1_native_pipelines_prewarm = false");
      }
      return;
    }
    const ShadersNative* library = LibraryActive();
    if (!library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE) {
      return;  // not yet: check again on the next submission
    }
    prewarm_decided_ = true;
    library_prewarm_ = library;
    prewarm_eds_ = eds_mode_;  // the ring would not request those of another dynamic state mode
    prewarm_start_ = std::chrono::steady_clock::now();
    try {
      prewarm_thread_ = std::thread([this] { LoopPrewarm(); });
      REXLOG_INFO("[native] C6 prewarm (build 186): thread created for {} pipelines of the list",
                  list_file_.size());
    } catch (const std::system_error& error) {
      REXLOG_WARN("[native] C6 prewarm (build 186): could not create the thread ({}); no prewarming", error.what());
    }
  }

  // The thread. It only reads list_file_, the library, the layout and the cache; it writes
  // state_list_[i] before publishing prewarm_until_ = i + 1, and its atomic counters. Its modules
  // and render pass are its own and it destroys them when done.
  void LoopPrewarm() {
    rex::thread::set_current_thread_name("FH1 pipeline prewarm");
    int32_t priority = -1;
#if REX_PLATFORM_SWITCH
    // The lowest priority the system accepts, and never above the guest's (0x3B): compiling at the priority
    // threads are born with would take the core from the game and the ring. If none is accepted, nothing is
    // compiled.
    for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
      if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
        priority = candidate;
        break;
      }
    }
    if (priority < 0) {
      REXLOG_WARN("[native] C6 prewarm (build 186): the system accepts no priority from 0x3B to 0x3F: no prewarming");
      prewarm_finished_.store(true, std::memory_order_release);
      return;
    }
#endif
    prewarm_priority_.store(priority, std::memory_order_relaxed);
    const ShadersNative& library = *library_prewarm_;
    // FH1: the list is walked twice. First by several threads at once, only to fill the pipeline cache (each
    // with its own modules and passes, results thrown away); then in order by this thread, which now finds
    // everything in the cache. With one thread a new shader library took 65 s to prewarm, the festival loaded
    // in the middle of it, the ring compiled ~170 pipelines itself in one frame of 4 s, and the game stopped
    // sending commands for good (loading screen frozen, sound running; seen with frames of 3.3 and 4.2 s, never
    // with 3.1 s or less).
    std::atomic<size_t> next_parallel{0};
    std::atomic<uint32_t> compiled_parallel{0};
    std::atomic<uint64_t> ns_compiled_parallel{0};
    const auto walk = [&](const bool parallel) {
    std::unordered_map<uint64_t, VkShaderModule> modules;  // (variant << 32) | number
    std::unordered_map<uint64_t, VkRenderPass> passes;       // by formats
    const auto create = [&](const uint32_t* spirv, size_t bytes) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      VkShaderModule module_handle = VK_NULL_HANDLE;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module_handle) != VK_SUCCESS) {
        module_handle = VK_NULL_HANDLE;
      }
      return module_handle;
    };
    // ModuleOf: the library's SPIR-V as is.
    const auto normal = [&](const EntryShader& e) {
      auto it = modules.find(e.number);
      if (it == modules.end()) {
        it = modules.emplace(e.number, create(e.shader->spirv.data(), e.shader->spirv.size() * sizeof(uint32_t))).first;
      }
      return it->second;
    };
    // The pixel shader PipelineOf would choose, in the same order: the bright pass variants
    // (ModuleVariant), without color writes (ModuleOnlyAlpha) and with early tests (ModuleZEarly); the
    // latter two with the same fallback: if it cannot be pruned or patched, the normal one.
    const auto pixel = [&](const KeyPipeline& key, const EntryShader& e) {
      const uint32_t spec = key.specialization;
      const uint32_t variant = (spec & kSpecGlowNatural) ? 1
                                : (spec & kSpecGlowSoft) ? 2
                                : (spec & kSpecOnlyAlpha)        ? 3
                                : (spec & kSpecZEarly)       ? 4
                                                                : 0;
      if (variant == 0) {
        return normal(e);
      }
      const uint64_t key_module = (uint64_t(variant) << 32) | (variant <= 2 ? 0xFFFFFFFFull : uint64_t(e.number));
      auto it = modules.find(key_module);
      if (it == modules.end()) {
        VkShaderModule module_handle = VK_NULL_HANDLE;
        if (variant == 1) {
          module_handle = create(kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
        } else if (variant == 2) {
          module_handle = create(kSpirvGlowSoft, sizeof(kSpirvGlowSoft));
        } else if (variant == 3) {
          uint32_t removed = 0;
          const std::vector<uint32_t> pruned = PruneWritesOfColor(e.shader->spirv, removed);
          if (!pruned.empty()) {
            module_handle = create(pruned.data(), pruned.size() * sizeof(uint32_t));
          }
        } else {
          const char* reason = "";
          const std::vector<uint32_t> patched = WithTestsEarly(e.shader->spirv, reason);
          if (!patched.empty()) {
            module_handle = create(patched.data(), patched.size() * sizeof(uint32_t));
          }
        }
        it = modules.emplace(key_module, module_handle).first;
      }
      if (it->second != VK_NULL_HANDLE || variant <= 2) {
        return it->second;
      }
      return normal(e);
    };
    uint32_t done = 0, compiled = 0, sin_shader = 0, other_mode = 0, failed = 0;
    uint64_t ns_compiled = 0;
    const size_t n = list_file_.size();
    for (size_t k_walk = 0; k_walk < n; ++k_walk) {
      if (prewarm_stop_.load(std::memory_order_relaxed)) {
        break;
      }
      const size_t i = parallel ? next_parallel.fetch_add(1, std::memory_order_relaxed) : k_walk;
      if (i >= n) {
        break;
      }
      const RegisterPipeline& r = list_file_[i];
      uint8_t state = kListFailed;
      const EntryShader* vs = r.key.vs ? library.ByNumber(r.key.vs - 1) : nullptr;
      const EntryShader* ps = r.key.ps ? library.ByNumber(r.key.ps - 1) : nullptr;
      if (!vs || !vs->vertices || !vs->shader || vs->shader->fingerprint != r.fingerprint_vs ||
          (r.key.ps && (!ps || ps->vertices || !ps->shader || ps->shader->fingerprint != r.fingerprint_ps))) {
        state = kListWithoutShader;
        ++sin_shader;
      } else if (r.key.fill2 != prewarm_eds_) {
        state = kListOtherMode;
        ++other_mode;
      } else {
        const VkShaderModule module_vs = normal(*vs);
        const VkShaderModule module_ps = r.key.ps ? pixel(r.key, *ps) : VK_NULL_HANDLE;
        const uint64_t key_pass = XXH3_64bits(r.key.formats, sizeof(r.key.formats));
        auto it_pass = passes.find(key_pass);
        if (it_pass == passes.end()) {
          it_pass = passes.emplace(key_pass, CreatePass(r.key.formats, kLoadRead)).first;
        }
        EntryVertices entry;
        for (uint32_t k = 0; k < r.n_attributes; ++k) {
          const AttributeRegister& a = r.attributes[k];
          entry.attributes.push_back({a.location, a.binding, VkFormat(a.format), a.offset});
        }
        for (uint32_t k = 0; k < r.n_bindings; ++k) {
          entry.bindings.push_back({0, r.strides[k]});
        }
        if (module_vs != VK_NULL_HANDLE && (!r.key.ps || module_ps != VK_NULL_HANDLE) &&
            it_pass->second != VK_NULL_HANDLE) {
          VkPipeline pipeline = VK_NULL_HANDLE;
          uint32_t n_colors = 0;
          const auto t0 = std::chrono::steady_clock::now();
          const VkResult result =
              CreatePipelineVulkan(r.key, entry, module_vs, module_ps, it_pass->second, false, pipeline, n_colors);
          const uint64_t ns = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
          if (result == VK_SUCCESS && pipeline != VK_NULL_HANDLE) {
            dfn_.vkDestroyPipeline(device_, pipeline, nullptr);
            state = kListPrewarm;
            ++done;
            if (ns >= kNsCompiled) {
              ++compiled;
              ns_compiled += ns;
            }
          } else {
            ++failed;
          }
        } else {
          ++failed;
        }
      }
      if (parallel) {
        continue;
      }
      state_list_[i] = state;
      prewarm_done_.store(done, std::memory_order_relaxed);
      prewarm_compiled_.store(compiled + compiled_parallel.load(std::memory_order_relaxed), std::memory_order_relaxed);
      prewarm_ns_compiled_.store(ns_compiled + ns_compiled_parallel.load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
      prewarm_without_shader_.store(sin_shader, std::memory_order_relaxed);
      prewarm_other_mode_.store(other_mode, std::memory_order_relaxed);
      prewarm_failed_.store(failed, std::memory_order_relaxed);
      prewarm_until_.store(i + 1, std::memory_order_release);
    }
    for (const auto& [key, module_handle] : modules) {
      if (module_handle != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, module_handle, nullptr);
      }
    }
    for (const auto& [key, pass] : passes) {
      if (pass != VK_NULL_HANDLE) {
        dfn_.vkDestroyRenderPass(device_, pass, nullptr);
      }
    }
    if (parallel) {
      compiled_parallel.fetch_add(compiled, std::memory_order_relaxed);
      ns_compiled_parallel.fetch_add(ns_compiled, std::memory_order_relaxed);
      return std::array<uint32_t, 5>{};
    }
    return std::array<uint32_t, 5>{done, compiled, sin_shader, other_mode, failed};
    };
    uint32_t threads_parallel = 0;
#if !REX_PLATFORM_SWITCH
    {
      const int32_t requested = REXCVAR_GET(fh1_native_prewarm_threads);
      threads_parallel = requested >= 0 ? uint32_t(requested)
                                        : std::min<uint32_t>(6, std::max<uint32_t>(std::thread::hardware_concurrency(), 2) / 2);
      std::vector<std::thread> helpers;
      for (uint32_t t = 0; t < threads_parallel; ++t) {
        helpers.emplace_back([&walk] { walk(true); });
      }
      for (std::thread& helper : helpers) {
        helper.join();
      }
    }
#endif
    const std::array<uint32_t, 5> totals = walk(false);
    const uint32_t done = totals[0], compiled = totals[1] + compiled_parallel.load(), sin_shader = totals[2],
                   other_mode = totals[3], failed = totals[4];
    const uint64_t ns_compiled = ns_compiled_parallel.load();
    const size_t n = list_file_.size();
    REXLOG_INFO("[native] C6 prewarm: {} threads walked the list first", threads_parallel);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarm_start_).count();
    REXLOG_INFO("[native] C6 prewarm (build 186): {} in {:.1f} s, priority {:#x}: {} of {} pipelines prewarmed, {} "
                "really compiled ({:.0f} ms, {:.1f} ms each; the rest was already in the cache), {} skipped "
                "because the library no longer has their shaders, {} of another dynamic-state mode and {} failed",
                prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority,
                done, n, compiled, double(ns_compiled) / 1e6,
                compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0, sin_shader, other_mode, failed);
    prewarm_finished_.store(true, std::memory_order_release);
  }

  // Ring only, every 10 s if anything changed: the thread's progress, what the ring had to create, and
  // the guard (see above).
  void ReportPrewarm(std::chrono::steady_clock::time_point now) {
    if (now - prewarm_report_ < std::chrono::seconds(10)) {
      return;
    }
    prewarm_report_ = now;
    if (!prewarm_difference_ && ring_prewarmed_ >= 8 &&
        ring_prewarmed_slow_ * 2 >= ring_prewarmed_) {
      prewarm_difference_ = true;
      prewarm_stop_.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[native] C6 prewarm (build 186): DIFFERENCE: of {} pipelines the ring asked for already "
                   "prewarmed, {} took as long as a compiled one (20 ms or more): what the thread prewarms is not "
                   "what the ring asks for. The thread is stopped; nothing else changes",
                   ring_prewarmed_, ring_prewarmed_slow_);
    }
    const size_t until = prewarm_until_.load(std::memory_order_acquire);
    const bool finished = prewarm_finished_.load(std::memory_order_acquire);
    const uint64_t changes = uint64_t(until) + ring_prewarmed_ + ring_of_list_ + ring_new_ +
                             (finished ? 1 : 0) + (prewarm_decided_ ? 1 : 0);
    if (changes == prewarm_changes_previous_) {
      return;
    }
    prewarm_changes_previous_ = changes;
    const auto average = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
    const uint32_t compiled = prewarm_compiled_.load(std::memory_order_relaxed);
    FH1_REPORT_RING(
        "[native] C6 pipeline prewarm (build 186): list of {}; thread {} (priority {:#x}): {} walked, {} "
        "prewarmed, {} really compiled ({:.0f} ms), {} without their shaders, {} of another mode, {} failed | the "
        "ring created {} from the list already prewarmed ({:.1f} ms on average, {} slow), {} from the list not "
        "prewarmed ({:.1f} ms on average) and {} new ({:.1f} ms on average; {} added to the list)",
        list_file_.size(),
        !prewarm_thread_.joinable() ? (prewarm_decided_ ? "not started" : "waiting for the library")
        : finished                    ? (prewarm_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished")
                                       : "running",
        uint32_t(prewarm_priority_.load(std::memory_order_relaxed)), until,
        prewarm_done_.load(std::memory_order_relaxed), compiled,
        double(prewarm_ns_compiled_.load(std::memory_order_relaxed)) / 1e6,
        prewarm_without_shader_.load(std::memory_order_relaxed), prewarm_other_mode_.load(std::memory_order_relaxed),
        prewarm_failed_.load(std::memory_order_relaxed), ring_prewarmed_,
        average(ns_ring_prewarmed_, ring_prewarmed_), ring_prewarmed_slow_, ring_of_list_,
        average(ns_ring_of_list_, ring_of_list_), ring_new_, average(ns_ring_new_, ring_new_),
        ring_a_list_);
  }

  void StopPrewarm() {
    prewarm_stop_.store(true, std::memory_order_relaxed);
    if (prewarm_thread_.joinable()) {
      prewarm_thread_.join();  // at most, as long as the pipeline being compiled takes
    }
  }

  void DestroyImage(ImageNative& image) {
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);
    // The pool chunk is returned after destroying the image that used it, and in that case `memory` is
    // NULL on purpose: the memory belongs to a shared slab and is not freed on its own.
    if (image.pool_block != 0xFFFFFFFFu) pool_textures_.Free(image.pool_block);
    if (image.memory_block != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.memory_block, nullptr);
    image = ImageNative{};
  }

  struct Heap {
    uint32_t capacity = 0;
    uint32_t next = 1;
    std::vector<uint32_t> free;
  };

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  ContextTargets* context_;
  FnAddressBuffer address_buffer_ = nullptr;
  FnCopyImage copy_image_ = nullptr;  // resolved faces of the dynamic cubemaps

  VkBuffer upload_ = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory_ = VK_NULL_HANDLE;
  uint32_t upload_type_ = UINT32_MAX;
  VkDeviceSize upload_size_real_ = 0;
  uint8_t* upload_data_ = nullptr;
  VkDeviceAddress upload_address_ = 0;
  VkDeviceSize upload_used_ = 0;
  bool upload_coherent_ = false;
  uint64_t epoch_upload_ = 0;
  uint64_t frame_ = 0;
  // Fast untiling and its guard (see ReadLevel).
  static constexpr uint32_t kLevelsACheck = 200;  // it used to be 2000
  static constexpr uint32_t kCheckOneOfEvery = 64;   // after the first ones, by sampling
  uint64_t levels_seen_ = 0;
  uint64_t commands_seen_ = 0;
  int tile_fast_ = -1;
  uint32_t levels_checked_ = 0;
  uint32_t commands_checked_ = 0;
  std::vector<uint8_t> check_tile_;
  // Texture check budget (see PrepareTexture).
  static constexpr uint32_t kDeferralMax = 8;
  int32_t budget_fingerprints_ = -1;
  uint64_t frame_fingerprints_ = UINT64_MAX;
  uint64_t bytes_fingerprint_frame_ = 0;
  // Sampled recheck of stable textures (see PrepareTexture and FingerprintSample).
  static constexpr uint64_t kSamplesACheck = 3000;  // the first ones, with both hashes: the guard
  int32_t sampling_fingerprints_ = -1;      // -1 cvar not read; 0 off; N: 1 in N rechecks is full
  uint64_t samples_checked_ = 0;  // stable rechecks with both hashes, since start-up
  uint64_t samples_with_the_two_ = 0;  // the same, since the last report line
  uint64_t samples_hits_ = 0;     // decided on the sample alone
  uint64_t samples_different_ = 0;    // the sample changed and the full path followed
  uint64_t bytes_sample_ = 0;
  uint64_t bytes_saved_sample_ = 0;
  std::chrono::steady_clock::time_point report_sampling_{};
  // Vertex copies on a separate thread (EnqueueCopy). Power-of-2 capacity.
  const bool copies_active_ = REXCVAR_GET(fh1_native_uploads_thread);
  std::array<WorkCopy, 8192> copies_{};
  std::atomic<size_t> copies_written_{0};
  // How many copies are done, whether by the thread or the ring (see WaitUploads). Outside
  // WaitUploads they finish in order, and then it is also the index of the first one not done.
  std::atomic<size_t> copies_done_{0};
  bool copies_sleeping_ = false;   // under copies_mutex_: the copy thread sleeps or is about to
  bool copies_waiting_ = false;   // under copies_mutex_: the ring waits in WaitUploads
  std::mutex copies_mutex_;
  std::condition_variable copies_cv_;
  std::condition_variable copies_done_cv_;
  bool copies_stop_ = false;  // under copies_mutex_
  std::thread copies_thread_;
  uint64_t copies_in_line_ = 0;
  uint64_t waits_copies_ = 0;
  uint64_t ns_waiting_copies_ = 0;
  std::atomic<size_t> copies_progress_{0};  // copies done by the thread, one by one (diagnostic)
  std::thread::id copies_producer_;
  bool copies_other_thread_warned_ = false;
  // The ring's help (fh1_native_uploads_help) and its guard. See WaitUploads.
  const bool copies_help_requested_ = REXCVAR_GET(fh1_native_uploads_help);
  int32_t copies_phase_ = 0;         // PhaseCopies; ring only
  bool copies_without_thread_ = false;    // off after a DIFFERENCE: copies go on the ring; ring only
  std::atomic<size_t> copies_taken_{0};  // how many were taken, in order, by the thread or the ring (with CAS)
  std::mutex copies_chunk_mutex_;          // held by the thread while it copies what it took (lends priority)
  std::array<std::atomic<uint32_t>, 8192> copies_marks_{};  // index + 1 of the last copy done in each slot
  std::atomic<uint64_t> copies_thread_n_{0};  // copies done by the thread, for the report
  std::atomic<uint64_t> copies_thread_bytes_{0};
  size_t copies_verified_ = 0;  // marks checked up to here; ring only
  uint64_t copies_checked_ = 0;
  uint64_t copies_waits_watching_ = 0;
  uint64_t copies_helped_ = 0;
  uint64_t copies_bytes_helped_ = 0;
  uint64_t copies_ns_helping_ = 0;
  uint64_t copies_differences_ = 0;
  struct ReportCopiesFigures {  // the last 10 s (ReportCopies); ring only
    uint64_t queued = 0, bytes_queued = 0, in_line = 0, bytes_in_line = 0;
    uint64_t helped = 0, bytes_helped = 0, ns_helping = 0, waits_with_help = 0;
    uint64_t waits = 0, without_take = 0, waits_chunk = 0, ns_chunk = 0, ns_chunk_worst = 0;
    uint64_t waits_whole = 0, ns_wait = 0, ns_wait_worst = 0, checked = 0;
  };
  ReportCopiesFigures copies_inf_{};
  std::chrono::steady_clock::time_point copies_report_{};
  uint64_t copies_thread_n_previous_ = 0;
  uint64_t copies_thread_bytes_previous_ = 0;
  struct BufferUpload {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory_block = VK_NULL_HANDLE;
    VkDeviceSize size_real = 0;
    uint8_t* data = nullptr;
    VkDeviceAddress address = 0;
  };
  // One per work slot (there are 3). With fewer than there are slots, two slots would share a buffer and
  // the CPU would write over what the GPU is still reading.
  std::array<BufferUpload, kSlotsOfWork> uploads_{};  // upload_* are the current slot's
  // Separate shared constants, CPU-cached (fh1_native_shared_cache).
  std::array<BufferUpload, kSlotsOfWork> shared_bufs_{};
  bool shared_separate_ = false;
  bool shared_coherent_ = true;
  uint32_t shared_type_ = 0;
  uint8_t* shared_data_ = nullptr;
  VkDeviceMemory shared_memory_ = VK_NULL_HANDLE;
  VkDeviceSize shared_size_real_ = 0;
  VkDeviceSize shared_used_ = 0;
  uint64_t shared_bytes_published_ = 0;

  std::array<VkDescriptorSetLayout, 4> layouts_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  // Set 4, the constants through dynamic UBOs. One set per upload slot (each one is a VkBuffer).
  bool use_ubo_ = false;
  VkDeviceSize alignment_ubo_ = 256;
  VkDescriptorSetLayout layout_ubo_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ubo_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOfWork> sets_ubo_{};  // one per work slot
  uint32_t slot_current_ = 0;
  bool ubo_bound_ = false;
  uint32_t slot_ubo_bound_ = UINT32_MAX;
  std::array<uint32_t, 3> offsets_ubo_bound_{};
  // Set 4 by differences (ControlSet4, ReportSet4).
  std::array<uint64_t, 8> set4_changes_{};  // binds by what changes: bit 0 VS, bit 1 PS, bit 2 shared
  uint64_t set4_first_ = 0;              // binds after a new command buffer, a new slot or the sky
  uint64_t set4_draws_ = 0;
  std::array<uint64_t, 3> reuploads_vs_{};  // constants uploaded again: by generation, by epoch, because they grow
  std::array<uint64_t, 3> reuploads_ps_{};
  std::chrono::steady_clock::time_point set4_report_{};
  std::chrono::steady_clock::time_point set4_start_alternation_ = std::chrono::steady_clock::now();
  bool set4_requested_ = true;
  bool set4_requested_noted_ = false;
#if REX_PLATFORM_SWITCH
  bool set4_difference_notified_ = false;
  uint64_t set4_nvk_previous_[9] = {};  // NVK counts at the previous report
#endif
  // The draw path in NVK (ControlDrawNvk and ReportDrawNvk). Ring only.
  bool nvk_preload_app_ = false;  // hand the pipeline to NVK after PipelineOf
  bool nvk_draw_noted_ = false;
  bool nvk_improvements_requested_ = true;
  bool nvk_off_warned_[5] = {};
  std::chrono::steady_clock::time_point nvk_draw_start_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point nvk_report_{};
  uint64_t nvk_parts_previous_[16][2] = {};
  uint64_t nvk_counts_previous_[13] = {};
  uint64_t nvk_improvements_previous_[5][4] = {};
  int32_t toggle_ubo_s_ = 0;
  uint64_t submissions_ = 0;
  uint64_t submissions_ubo_ = 0;
  std::chrono::steady_clock::time_point start_ubo_ = std::chrono::steady_clock::now();
  std::array<Heap, 4> heaps_{};
  std::array<ImageNative, 3> empty_{};
  VkSampler sampler_empty_ = VK_NULL_HANDLE;
  bool empty_prepared_ = false;

  std::unordered_map<const EntryShader*, VkShaderModule> modules_;
  std::array<VkShaderModule, 3> modules_variants_{};  // 1 natural, 2 soft
  std::array<bool, 3> modules_variants_created_{};
  int glow_noted_ = 0;
  std::unordered_map<uint64_t, std::pair<KeyPipeline, VkPipeline>> pipelines_;
  // One-entry shortcut for PipelineOf (see the comment there).
  KeyPipeline last_key_{};
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  bool last_key_valid_ = false;
  // direct cache in front of pipelines_ (fh1_native_direct_pipelines).
  struct SlotPipeline {
    KeyPipeline key{};
    VkPipeline pipeline = VK_NULL_HANDLE;  // VK_NULL_HANDLE = empty slot
  };
  static constexpr size_t kSlotsPipeline = 256;  // power of 2; 256 x 88 bytes = 22 KB contiguous
  std::array<SlotPipeline, kSlotsPipeline> slots_pipeline_{};
  bool pipelines_direct_ = true;           // the cvar, once per frame
  bool pipelines_direct_off_ = false;  // the guard saw a difference
  uint64_t pipelines_direct_hits_ = 0;
  uint64_t pipelines_direct_misses_ = 0;
  uint64_t pipelines_direct_checked_ = 0;
  uint64_t pipelines_direct_hits_previous_ = 0;
  uint64_t pipelines_direct_misses_previous_ = 0;
  std::chrono::steady_clock::time_point pipelines_direct_report_{};
  static constexpr uint64_t kPipelinesACheck = 200000;
  // fh1_native_count_pipeline_changes (CountChangePipeline and ReportChangesPipeline).
  enum : uint32_t {
    kChangeBindings = 0,        // vkCmdBindPipeline calls counted in Draw
    kChangeAfterPass,           // of those, the first of a pass (BeginPass forgets the bound pipeline)
    kChangeFirst,            // no previous key in the buffer (new buffer or after the deferred sky)
    kChangeIdentical,           // the same key: only after BeginPass
    kChangeShaders,            // another VS or PS
    kChangeEntry,            // same shaders, different vertex input
    kChangeFormats,           // same shaders and input, different formats (another pass)
    kChangeSpecAlpha,           // all the above equal; specialization only changes the alpha test (bits 1, 16-18)
    kChangeSpecZEarly,      // ... the alpha test and the early Z (bit 20)
    kChangeSpecOther,           // ... other specialization bits
    kChangeWithoutEffect,          // state only, in bits PipelineOf does not read: effectively the same pipeline
    kChangeWithoutEffectAfterPass,  // ... and the first of a pass
    kChangeOnlyState,         // real state-only changes: what dynamic state would avoid
    kChangeStateAfterPass,     // ... and the first of a pass
    kChangeEds12,              // ... without blending, masks or another topology class (EDS1/EDS2, core 1.3)
    kChangeEds12AfterPass,      // ... and the first of a pass
    kFieldTopology,           // per field, on the canonical state (one bind may change several)
    kFieldClassTopology,
    kFieldBlend,
    kFieldMasks,
    kFieldZ,
    kFieldStencil,
    kFieldFace,
    kFieldReset,
    kFieldBias,
    kChangesN
  };
  std::array<uint64_t, kChangesN> changes_pipeline_{};
  KeyPipeline key_bound_{};       // the last key bound in this command buffer
  bool key_bound_valid_ = false;   // false: new buffer, after the deferred sky, or not counting
  bool count_changes_pipeline_ = true;  // the cvar, once per frame (ReportChangesPipeline)
  uint64_t changes_draws_previous_ = 0;
  uint64_t changes_frames_previous_ = 0;
  std::chrono::steady_clock::time_point changes_report_{};
  // Dynamic state, phase 0a (fh1_native_canonical_key, KeyOfLookup).
  static constexpr uint64_t kCanonicalACheck = 200000;
  bool key_canonical_ = true;           // the cvar, once per command buffer (UseSlot)
  bool key_canonical_off_ = false;  // the guard saw a difference
  bool canonical_valid_ = false;         // canonical_raw_ -> canonical_result_ is the previous draw's
  KeyPipeline canonical_raw_{};
  KeyPipeline canonical_result_{};
  uint64_t canonical_changed_ = 0;      // keys Canonicalize changes (not counting consecutive repeats)
  uint64_t canonical_checked_ = 0;
  uint64_t canonical_changed_previous_ = 0;
  uint64_t canonical_pipelines_previous_ = 0;
  std::chrono::steady_clock::time_point canonical_report_{};
  // Dynamic state, phase 0b (fh1_native_pipeline_between_passes).
  bool pipeline_between_passes_ = true;  // the cvar, once per command buffer (UseSlot)
  bool pipeline_between_passes_noted_ = false;
  uint64_t passes_with_pipeline_ = 0;   // passes started with a bound pipeline that is kept
  uint64_t between_passes_passes_previous_ = 0;
  uint64_t between_passes_kept_previous_ = 0;
  std::chrono::steady_clock::time_point between_passes_report_{};
  // Dynamic state phases 1 and 2 (FixStateDynamic, its guard and its report).
  enum : uint32_t { kEds12 = 1, kEds3 = 2 };  // bits of eds_mode_ and of KeyPipeline::fill2
  static constexpr VkDynamicState kDynamicEds12[10] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr uint64_t kEdsACheck = 200000;
  enum : uint32_t {
    kEdsFace = 0,
    kEdsFront,
    kEdsTopology,
    kEdsReset,
    kEdsBias,
    kEdsTestZ,
    kEdsWritesZ,
    kEdsFunctionZ,
    kEdsStencil,
    kEdsStencilOps,
    kEdsBlendActive,  // phase 2
    kEdsEquation,      // phase 2
    kEdsMask,       // phase 2
    kEdsAllState,          // times everything is set (not a call)
    kEdsN
  };
  PFN_vkCmdSetCullMode set_face_ = nullptr;  // Vulkan 1.3 core (LoadStateDynamic)
  PFN_vkCmdSetFrontFace set_front_ = nullptr;
  PFN_vkCmdSetPrimitiveTopology set_topology_ = nullptr;
  PFN_vkCmdSetPrimitiveRestartEnable set_reset_ = nullptr;
  PFN_vkCmdSetDepthBiasEnable set_bias_ = nullptr;
  PFN_vkCmdSetDepthTestEnable set_test_z_ = nullptr;
  PFN_vkCmdSetDepthWriteEnable set_writes_z_ = nullptr;
  PFN_vkCmdSetDepthCompareOp set_function_z_ = nullptr;
  PFN_vkCmdSetStencilTestEnable set_stencil_ = nullptr;
  PFN_vkCmdSetStencilOp set_stencil_ops_ = nullptr;
  bool eds12_available_ = false;  // API 1.3 and the ten functions
  bool eds_off_ = false;       // the guard saw a difference: for the rest of the session, everything in the pipeline
  bool eds_mode_noted_ = false;
  uint32_t eds_mode_ = 0;          // kEds12 | kEds3 of this command buffer (UseSlot; the guard sets it to 0)
  bool eds_valid_ = false;        // eds_recorded_ is what is set in this buffer (false: new buffer, sky or pass)
  StateEds eds_recorded_{};
  KeyPipeline eds_key_{};      // raw key of the last state set
  uint64_t eds_draws_ = 0;       // draws with dynamic state
  uint64_t eds_repeated_ = 0;     // ... with the same raw key as the previous one: nothing to set or check
  uint64_t eds_checkable_ = 0;  // ... with something to set (what the guard counts)
  uint64_t eds_checked_ = 0;
  uint64_t eds_draws_previous_ = 0;
  uint64_t eds_repeated_previous_ = 0;
  uint64_t eds_frames_previous_ = 0;
  std::array<uint64_t, kEdsN> eds_calls_{};    // vkCmdSet* recorded per state since the last report
  std::array<uint64_t, 4> pipelines_by_mode_{};  // pipelines created per mode (fill2) since start-up
  std::chrono::steady_clock::time_point eds_report_{};
  // Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3).
  static constexpr VkDynamicState kDynamicEds3[3] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
  // The same mapping as kFactors in FillStateFixed (fields 2, 3 and 17-31 give ZERO).
  static constexpr VkBlendFactor kFactorsBlend[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  PFN_vkCmdSetColorBlendEnableEXT set_blend_active_ = nullptr;
  PFN_vkCmdSetColorBlendEquationEXT set_equation_ = nullptr;
  PFN_vkCmdSetColorWriteMaskEXT set_mask_ = nullptr;
  bool eds3_available_ = false;  // the SDK enables the extension and its three features; the driver provides the functions
  std::unordered_map<uint64_t, VkRenderPass> passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
  // fh1_native_framebuffers_forget_views. The views of each cached framebuffer (by the same key), the
  // ones retired by the guard, and the counts for the warning.
  std::unordered_map<uint64_t, std::array<VkImageView, 5>> framebuffers_views_;
  std::vector<VkFramebuffer> fb_retired_;
  bool fb_retire_ = false;
  uint64_t fb_views_forgotten_ = 0;
  uint64_t fb_forgotten_ = 0;
  std::unordered_map<uint64_t, Texture> textures_;
  // Measurement only (fh1_native_diag_reuse): textures with a hash, by content key (content key ->
  // texture key; see NoteContentTexture), and the counts for the 10 s report.
  std::unordered_multimap<uint64_t, uint64_t> live_by_content_;
  uint64_t contents_different_ = 0;
  int32_t diag_reuse_ = -1;  // -1 = cvar not read
  uint64_t reuse_new_ = 0;
  uint64_t reuse_match_ = 0;
  uint64_t reuse_cold_ = 0;
  uint64_t reuse_without_look_ = 0;
  uint64_t reuse_bytes_new_ = 0;
  uint64_t reuse_bytes_match_ = 0;
  uint64_t reuse_bytes_cold_ = 0;
  uint32_t reuse_details_ = 0;
  std::chrono::steady_clock::time_point report_reuse_{};
  std::unordered_map<uint64_t, ViewEntry> views_;
  // The views_ keys of each image. InvalidateImages (every resolve and every copy) used to walk all of
  // views_: 2.7 % of the ring thread in a race. Maintained together with views_ in SlotView
  // (insertion) and in RemoveView (every removal).
  std::unordered_map<VkImage, std::vector<uint64_t>> views_by_image_;
  std::unordered_map<uint32_t, std::pair<VkSampler, uint32_t>> samplers_;
  // Diagnostic: filter values already logged (SlotSampler).
  uint32_t aniso_seen_ = 0;
  bool aniso_noted_ = false;  // fh1_native_anisotropic, one line when the first one is created
  uint32_t diag_mip_minimum_ = 0;  // fh1_native_diag_min_mip, read once per frame
  // fh1_native_shadows_bias_*, read once per frame.
  int32_t shadows_bias_constant_ = 0;
  int32_t shadows_bias_pending_ = 0;
  bool shadows_bias_noted_ = false;
  uint32_t mip_max_seen_ = 0;
  std::array<bool, 1024> biases_seen_{};
  std::vector<Texture*> textures_a_upload_;
  std::vector<uint8_t> temporal_;
  std::vector<uint32_t, WithoutInitialize<uint32_t>> indices_;
  std::vector<uint32_t, WithoutInitialize<uint32_t>> converted_;
  std::vector<uint16_t, WithoutInitialize<uint16_t>> indices16_;  // fast path: 16 bits without converting
  // IndicesDe16 (fh1_native_indices_neon). Ring only.
  int32_t indices_neon_ = -1;  // -1 cvar not read, 0 plain loop, 1 NEON
  uint64_t indices_neon_draws_ = 0;
  uint64_t indices_neon_checked_ = 0;
  std::vector<uint16_t, WithoutInitialize<uint16_t>> indices_neon_test_;
  // Dynamic state already recorded in the current command buffer and pass.
  bool state_recorded_ = false;
  uint64_t push_recorded_[3] = {};
  VkViewport viewport_recorded_{};
  VkRect2D scissor_recorded_{};
  float blend_recorded_[4] = {};
  float bias_recorded_[2] = {};  // depth bias: constant and slope
  float bias_warned_[2] = {};
  uint32_t warnings_bias_ = 0;
  std::string skip_text_;  // fh1_native_diag_skip_ps already read
  std::unordered_set<uint32_t> skip_ps_;
  uint32_t stencil_recorded_[2] = {};  // RB_STENCILREFMASK for front and back
  bool stencil_recorded_valid_ = false;
  VkIndexType type_indices_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  // Shared constants of the previous draw and where they were uploaded.
  uint32_t shared_previous_[kWordsShared] = {};
  uint64_t shared_looked_ = 0;    // C6 report, to decide how to make the block cheaper
  uint64_t shared_changed_ = 0;
  VkDeviceSize shared_offset_ = 0;
  uint64_t shared_epoch_ = UINT64_MAX;

  EntryVertices entry_;
  const EntryVertices* entry_current_ = &entry_;  // entry_ or a cache element
  std::unordered_map<uint64_t, EntryVertices> entries_cache_;
  uint64_t entries_cache_hits_ = 0;
  const EntryShader* entry_vs_ = nullptr;
  uint64_t entry_generation_ = UINT64_MAX;
  bool entry_valid_ = false;

  uint64_t constants_vs_generation_ = UINT64_MAX;
  uint64_t constants_vs_epoch_ = UINT64_MAX;
  VkDeviceSize constants_vs_offset_ = 0;
  uint64_t constants_ps_generation_ = UINT64_MAX;
  uint64_t constants_ps_epoch_ = UINT64_MAX;
  VkDeviceSize constants_ps_offset_ = 0;
  uint32_t constants_vs_bytes_ = 0;
  uint32_t constants_ps_bytes_ = 0;

  bool pass_active_ = false;
  // The game's open occlusion query and the host query counting this pass's span.
  bool occlusion_open_ = false;
  // fh1_reflection_visibility (see fh1_reflection_on_demand.h). The witness, every ~1 s at 30 FPS.
  static constexpr uint64_t kWitnessEvery = 32;
  bool measure_visibility_ = false;
  bool witness_pending_ = false;
  uint64_t witness_next_ = 0;
  uint32_t query_occlusion_ = UINT32_MAX;
  uint32_t category_pass_ = kGpuOthers;
  bool sent_after_shadows_ = false;        // once per frame
  // Per-frame copies of the diagnostic cvars (see the comment at the Swap).
  std::string diag_skip_ps_text_;
  bool diag_vertices_repeated_ = false;
  bool diag_statistics_draw_ = false;
  uint32_t scissor_test_ = 0;
  std::array<VkBuffer, 16> buffers_vertices_{};
  // Height actually used by each render target, deduced from the scissor. It only grows.
  std::unordered_map<uint32_t, uint32_t> height_useful_;
  bool area_util_ = true;
  // Small per-draw savings. See their cvars.
  bool framing_cache_ = true;             // C1: the cvar, once per frame
  bool framing_cache_off_ = false;    // C1: the guard saw a difference
  bool framing_valid_ = false;
  uint64_t framing_generation_ = 0;
  uint64_t framing_pass_serie_ = 0;
  uint64_t pass_serie_ = 0;                // incremented on every BeginPass
  VkViewport framing_viewport_{};
  VkRect2D framing_scissor_{};
  float framing_ndc_[4] = {};
  uint32_t framing_empty_ = 0;
  uint64_t framing_hits_ = 0;         // since start-up (the guard)
  uint64_t framing_checked_ = 0;
  uint64_t framing_hits_report_ = 0;
  uint64_t framing_calculations_report_ = 0;
  static constexpr uint64_t kFramingsACheck = 200000;
  bool height_useful_memo_active_ = true;      // C2
  bool height_useful_memo_off_ = false;
  uint32_t* height_useful_memo_ = nullptr;     // height_useful_ element of the last pitch (the map never erases)
  uint32_t height_useful_memo_pitch_ = 0;
  uint64_t height_useful_memo_hits_ = 0;
  uint64_t height_useful_memo_hits_previous_ = 0;
  static constexpr uint64_t kHeightUsefulACheck = 200000;
  bool key_pass_fast_ = true;          // C3
  bool key_pass_fast_off_ = false;
  uint64_t pass_keys_[5] = {};           // the render target bytes that produced pass_key_
  bool pass_keys_valid_ = false;
  uint64_t keys_pass_fast_ = 0;
  uint64_t keys_pass_fast_previous_ = 0;
  static constexpr uint64_t kKeysPassACheck = 200000;
  bool cvars_by_frame_ = true;        // C5
  bool ps_only_alpha_frame_ = true;
  bool without_ps_without_color_frame_ = true;
  std::chrono::steady_clock::time_point details_report_{};
  bool without_vegetation_ = false;
  // Shadow map vegetation draws discarded early (see the early discard in Draw).
  uint64_t draws_vegetation_soon_ = 0;
  // VerdictVegetation (fh1_d3d_game_vegetation).
  bool vegetation_computed_ = false;  // this draw carries the game's verdict and the ring's has been computed
  bool vegetation_ring_ = false;     // what the early discard has to decide
  uint16_t vegetation_flags_ = 0;
  DetailVegetation vegetation_detail_;
  /*
   * The sky draw, saved in full to replay later (fh1_native_deferred_sky).
   *
   * This holds everything the vkCmd* calls of that draw need, by copy: handles (which do not change
   * within the pass) and values (which depend on no guest register once copied). The only thing kept "by
   * reference" are the offsets within the upload buffer, and that buffer only moves forward until
   * UseSlot resets it, always after the pass is closed.
   */
  enum : uint32_t {
    kSkyByBlend = 0,  // a blended draw arrived: that one does read the background color
    kSkyByWithoutZ,        // an opaque draw without Z write arrived: it could not cover the sky
    kSkyByEndOfPass,   // the pass closes (fallback path)
    kSkyByOtherSky,   // a second sky in the same pass: the first one is emitted now
    kSkyReasons
  };
  struct SkyDeferred {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;   // the upload one: vertices and indices
    bool usa_ubo = false;
    uint64_t push[3] = {};             // only with constants through pointers
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t slot_ubo = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    bool with_stencil = false;
    uint32_t stencil[2] = {};          // RB_STENCILREFMASK for front and back
    uint32_t n_bindings = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool con_indices = false;
    bool indices_de_16 = false;
    uint32_t indices = 0;
    uint32_t first_index = 0;
    uint32_t vmin = 0;
    uint32_t count = 0;               // no indices
    uint32_t ps_mas_uno = 0;           // for the diagnostic per-draw query
    uint32_t category = 0;
    uint64_t draws_to_defer = 0;   // position in the pass when it was deferred
    uint32_t eds_mode = 0;  // phases 1 and 2 its pipeline was looked up with (eds_mode_)
    StateEds eds{};        // and the state that pipeline lacks
  };
  SkyDeferred sky_{};
  bool sky_pending_ = false;
  bool sky_deferred_ = true;  // the cvar, read once per frame
  bool sky_noted_ = false;
  uint64_t draws_in_pass_ = 0;
  uint64_t sky_seen_ = 0;
  uint64_t sky_seen_previous_ = 0;
  // The ones that pass both marks (480 indices and first of the pass). This is the number to watch: if it
  // is not 1.00 per frame, the criterion still does not isolate the dome and nothing should be deferred.
  // The fingerprint-only "detected" once gave 7.00.
  uint64_t sky_candidates_ = 0;
  uint64_t sky_candidates_previous_ = 0;
  /*
   * The self-checking guard.
   *
   * Two earlier versions broke the image by enabling this blindly. The check that was missing ("the
   * criterion detects one draw per frame, not seven") no longer needs a diagnostic build: the game itself
   * runs it, before anything is deferred.
   *
   * Phase 0, observing: for the first kSkyFramesTest race frames nothing is deferred (the image
   *   is exactly the non-deferred one) and the candidates in each frame are counted.
   * Phase 1, deferring: only if those frames never had more than one and at least kSkyFramesWithOne
   *   had exactly one. Then yes, and that is 3.4-4.3 ms of GPU time.
   * Phase 2, off for good: as soon as two are seen in the same frame, or if the test does not come out
   *   clean. It is not retried for the whole session.
   *
   * So the worst case of this change is that it does nothing. Breaking the image is not among the
   * possible outcomes.
   */
  enum : uint32_t { kSkyWatching = 0, kSkyDeferring = 1, kSkyDiscarded = 2 };
  uint32_t sky_guard_ = kSkyWatching;
  uint32_t sky_guard_frames_ = 0;
  uint32_t sky_guard_with_one_ = 0;
  uint32_t sky_guard_in_frame_ = 0;
  uint32_t sky_guard_max_ = 0;
  uint64_t sky_deferred_count_ = 0;
  uint64_t sky_deferred_count_previous_ = 0;
  uint64_t sky_no_deferrable_ = 0;
  uint64_t sky_two_in_pass_ = 0;
  uint64_t sky_lost_ = 0;
  std::array<uint64_t, kSkyReasons> sky_emitted_{};
  std::array<uint64_t, kSkyReasons> sky_emitted_previous_{};
  uint64_t sky_position_sum_ = 0;
  uint64_t sky_position_sum_previous_ = 0;
  uint64_t sky_position_max_ = 0;
  // The last binding made with vkCmdBindVertexBuffers, so it is not repeated.
  std::array<VkDeviceSize, 16> offsets_recorded_{};
  uint32_t bindings_recorded_ = 0;
  uint64_t bindings_saved_ = 0;
  // fh1_native_zero_based_vertices (see the cvar) and its guard.
  bool vertices_base_zero_ = true;           // the cvar, read once per frame (UseSlot)
  bool vertices_base_zero_off_ = false;  // the guard saw a difference: the rest of the session, as before
  uint64_t base_zero_checked_ = 0;       // draws on the base-zero path since start-up (the guard)
  uint64_t base_zero_draws_ = 0;           // since the last report: without binding their own offset
  uint64_t base_zero_total_ = 0;             // since the last report: draws recorded with vertices
  uint64_t base_zero_several_bindings_ = 0;
  uint64_t base_zero_misaligned_ = 0;      // one binding, but the dedupe gave a copy off a multiple
  uint64_t base_zero_bindings_recorded_ = 0;  // vkCmdBindVertexBuffers recorded since the last report
  std::chrono::steady_clock::time_point base_zero_report_{};
  static constexpr uint64_t kBaseZeroACheck = 200000;
  std::array<uint64_t, 19> sub_ns_{};  // C6 substages, measurement only
  std::array<uint64_t, 19> sub_n_{};
  uint64_t sub_samples_ = 0;
  std::chrono::steady_clock::time_point sub_next_{};
  uint64_t submissions_after_shadows_ = 0;         // for the report
  uint32_t statistics_pass_ = UINT32_MAX;
  uint32_t warnings_occlusion_draw_ = 0;  // diagnostic of the query draws
  std::chrono::steady_clock::time_point last_warning_occlusion_draw_{};
  VkCommandBuffer pass_commands_ = VK_NULL_HANDLE;
  uint64_t pass_key_ = 0;
  uint64_t pass_generation_ = UINT64_MAX;
  uint32_t pass_width_ = 0;
  uint32_t pass_height_ = 0;
  float pass_scale_ = 1.0f;  // 1 except in the scaled shadow map
  float pass_msaa_scale_ = 1.0f;  // FH1: 2 in 4x MSAA passes (BeginPass)
  std::deque<std::vector<uint8_t>> vertices_expanded_;  // FH1: the current draw's repeated vertices (index_computed)
  uint64_t clears_depth_in_pass_ = 0;  // ZCULL
  std::chrono::steady_clock::time_point start_pass_{};  // pass change breakdown
  uint32_t pass_formats_[5] = {};
  VkRenderPass pass_rp_ = VK_NULL_HANDLE;
  uint64_t recording_generation_ = UINT64_MAX;
  VkPipeline pipeline_bound_ = VK_NULL_HANDLE;
  bool sets_bound_ = false;

  uint32_t diagnostics_ = 0;
  std::unordered_set<uint32_t> warned_vs_;
  uint32_t warnings_indices_ = 0;
  uint32_t warnings_swizzle_ = 0;
  std::unordered_set<uint64_t> diagnosed_;
  uint64_t drawn_ = 0;
  uint64_t drawn_timed_ = 0;  // of those, with the stage stopwatch
  uint32_t stopwatch_counter_ = 0;
  bool time_ = false;  // the current draw carries the stage stopwatch
  // Rear-view mirror diagnostic: cubemap refreshes from their resolved faces (report every 10 s).
  uint64_t cubes_refreshed_ = 0;
  uint64_t cubes_refreshed_previous_ = 0;
  uint64_t frame_report_cubes_ = 0;
  std::chrono::steady_clock::time_point report_cubes_{};
  uint64_t rejected_ = 0;
  // 8 stages. Stage 7 is the pass change, split from the pass stage to tell how much of the 3.9 us per
  // draw is the actual change (15 per frame at 162 us) and how much is what is done on every draw.
  std::array<uint64_t, kStagesDraw> stages_ns_{};
  std::unordered_map<uint32_t, uint64_t> causes_;
  uint32_t last_cause_ = 0;
  uint32_t entry_cause_ = 0;  // cause of the failed vertex input
  uint64_t uploads_texture_ = 0;
  uint64_t megabytes_bytes_ = 0;
  uint64_t bytes_textures_ = 0;  // texture images created (C6 report)
  int32_t textures_mb_max_ = 0;   // fh1_native_texture_mb_max
  // Large slabs from which textures take chunks, instead of a dedicated allocation per texture (1.9 ms of
  // CPU each on Horizon). Finish() goes in the destructor after the DestroyImage loops, never before.
  PoolTextures pool_textures_;
  // Texture bind thread (fh1_native_texture_binding_thread). See the LoopBindings block.
  struct RequestBinding {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory_block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkResult result = VK_NOT_READY;  // written by the thread, under bindings_mutex_
    uint64_t ns = 0;                    // wall time inside vkBindImageMemory, on the thread
  };
  struct TextureInFlight {  // ring thread only
    Texture* texture = nullptr;
    uint64_t ticket = 0;
    VkImage image = VK_NULL_HANDLE;  // the queued one (to remove it from images_in_flight_ and move its views)
    VkResult result = VK_NOT_READY;
    VkFormat format = VK_FORMAT_UNDEFINED;  // the fields below, to recreate it through CreateTexture if the bind fails
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    uint32_t background = 0;
    uint32_t levels = 1;
    bool copy = false;  // UploadTexture already left its data in the upload buffer
    VkDeviceSize copy_offset = 0;
    uint64_t copy_epoch = 0;
    VkCommandBuffer copy_commands = VK_NULL_HANDLE;
  };
  struct ViewInFlight {  // ring thread only
    uint64_t key = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    uint16_t swizzle_host = 0;
    uint32_t heap = 0;
    uint32_t slot = 0;
  };
  static constexpr uint64_t kQueueBindings = 256;  // power of 2
  static constexpr uint64_t kBindingsACheck = 64;
  static constexpr int32_t kBindingsWithoutDecide = -1;
  static constexpr int32_t kBindingsOff = 0;
  static constexpr int32_t kBindingsWatching = 1;
  static constexpr int32_t kBindingsApplying = 2;
  std::array<RequestBinding, kQueueBindings> queue_bindings_{};
  std::mutex bindings_mutex_;
  std::condition_variable bindings_cv_;         // there are requests or a stop request (the thread waits on it)
  std::condition_variable bindings_done_cv_;  // the thread finished one (the ring waits on it)
  uint64_t bindings_requested_ = 0;       // under bindings_mutex_
  uint64_t bindings_done_ = 0;        // under bindings_mutex_
  uint64_t bindings_collected_ = 0;     // under bindings_mutex_
  bool bindings_sleeping_ = false;     // under bindings_mutex_
  bool bindings_waiting_ = false;     // under bindings_mutex_
  bool bindings_stop_ = false;         // under bindings_mutex_
  bool bindings_priority_ok_ = true;   // under bindings_mutex_
  uint64_t ns_bindings_thread_ = 0;       // under bindings_mutex_
  uint64_t ns_binding_worst_ = 0;        // under bindings_mutex_
  uint64_t bindings_queue_full_ = 0;    // under bindings_mutex_
  std::thread bindings_thread_;
  int32_t bindings_priority_ = 0x2D;  // written by the ring before creating the thread
  // Ring thread only:
  int32_t bindings_phase_ = kBindingsWithoutDecide;
  bool bindings_stuck_ = false;
  bool measuring_creation_ = false;
  bool collecting_bindings_ = false;
  std::vector<TextureInFlight> in_flight_;
  std::vector<ViewInFlight> views_in_flight_;
  std::unordered_set<VkImage> images_in_flight_;
  uint64_t bindings_checked_ = 0;
  uint64_t bindings_thread_total_ = 0;
  uint64_t bindings_failed_ = 0;
  uint64_t views_deferred_ = 0;
  uint64_t copies_deferred_ = 0;
  uint64_t slots_lost_ = 0;
  uint64_t ns_wait_bindings_total_ = 0;
  // The 10 s report (ReportBindings):
  std::chrono::steady_clock::time_point report_bindings_{};
  uint64_t bindings_thread_report_ = 0;
  uint64_t waits_bindings_report_ = 0;
  uint64_t ns_wait_bindings_report_ = 0;
  uint64_t ns_wait_bindings_worst_ = 0;
  uint64_t ns_create_report_ = 0;
  uint64_t created_report_previous_ = 0;
  uint64_t views_deferred_report_ = 0;
  uint64_t copies_deferred_report_ = 0;
  uint64_t ns_bindings_thread_previous_ = 0;
  uint64_t queue_full_previous_ = 0;
  uint32_t reports_without_compensate_ = 0;
  // Texture hash thread (fh1_native_texture_fingerprint_thread). See the PlanFingerprint block.
  std::array<WorkFingerprint, kQueueFingerprints> queue_fingerprints_{};  // a slot belongs to the thread while it is InThread
  std::mutex fingerprints_mutex_;
  std::condition_variable fingerprints_cv_;         // there are jobs or a stop request (the thread waits on it)
  std::condition_variable fingerprints_done_cv_;  // the thread finished one (the ring waits on it)
  uint64_t fingerprints_published_ = 0;   // under fingerprints_mutex_ (only the ring writes it)
  uint64_t fingerprints_taken_ = 0;      // under fingerprints_mutex_: the thread has looked up to here
  uint64_t fingerprints_done_ = 0;       // under fingerprints_mutex_
  bool fingerprints_sleeping_ = false;    // under fingerprints_mutex_
  bool fingerprints_waiting_ = false;    // under fingerprints_mutex_
  bool fingerprints_stop_ = false;        // under fingerprints_mutex_
  bool fingerprints_priority_ok_ = true;  // under fingerprints_mutex_
  bool fingerprints_core_ok_ = true;     // under fingerprints_mutex_
  int fingerprints_core_real_ = -1;      // under fingerprints_mutex_: where the thread ran when starting
  std::thread fingerprints_thread_;
  int32_t fingerprints_priority_ = 0x2E;  // written by the ring before creating the thread
  int32_t fingerprints_core_requested_ = -2;
  int32_t fingerprints_core_ = -1;       // written by the ring before creating the thread
  // The snapshots and the plans: the ring writes them before publishing each job; the thread only reads
  // them.
  std::unique_ptr<uint8_t[]> snapshots_;
  std::unique_ptr<ReadFingerprint[]> reads_fingerprint_;
  uint64_t snapshots_bytes_ = 0;
  // Ring thread only:
  int32_t fingerprints_phase_ = kFingerprintsWithoutDecide;
  uint64_t fingerprints_collected_ = 0;
  uint64_t snapshots_used_ = 0;
  uint32_t reads_used_ = 0;
  std::array<ReadFingerprint, kReadsByTexture> reads_plan_{};
  std::vector<WorkFingerprint> fingerprints_planned_;
  uint64_t fingerprints_planned_pending_ = 0;
  std::vector<WorkFingerprint> fingerprints_collected_list_;
  std::array<ComparisonFingerprint, kQueueFingerprints> comparisons_fingerprint_{};
  const Texture* comparison_texture_ = nullptr;
  uint64_t comparison_sequence_ = 0;
  std::vector<uint8_t> fingerprints_data_ring_;
  std::vector<uint8_t> fingerprints_check_ring_;
  bool fingerprints_fast_off_ring_ = false;
  bool fingerprints_without_thread_ = false;
  bool fingerprints_stuck_ = false;
  bool fingerprints_suspicious_ = false;
  bool collecting_fingerprints_ = false;
  uint64_t fingerprints_comparisons_planned_ = 0;
  uint64_t fingerprints_new_applying_ = 0;
  uint64_t fingerprints_thread_total_ = 0;
  uint64_t fingerprints_ring_total_ = 0;
  uint64_t fingerprints_compared_ = 0;
  uint64_t fingerprints_without_compare_ = 0;
  uint64_t fingerprints_discarded_ = 0;
  uint64_t fingerprints_reuploads_ = 0;
  // The 10 s report (ReportFingerprints):
  std::chrono::steady_clock::time_point report_fingerprints_{};
  uint64_t fingerprints_planned_report_ = 0;
  uint64_t fingerprints_comparison_report_ = 0;
  uint64_t fingerprints_thread_report_ = 0;
  uint64_t fingerprints_ring_report_ = 0;
  uint64_t ns_fingerprints_thread_report_ = 0;
  uint64_t ns_fingerprint_raw_thread_report_ = 0;
  uint64_t ns_fingerprints_ring_report_ = 0;
  uint64_t ns_snapshots_report_ = 0;
  uint64_t bytes_snapshots_report_ = 0;
  uint64_t ns_wait_fingerprints_report_ = 0;
  uint64_t ns_wait_fingerprints_worst_ = 0;
  uint64_t waits_fingerprints_report_ = 0;
  uint64_t fingerprints_without_room_report_ = 0;
  uint32_t reports_without_compensate_fingerprints_ = 0;
  int32_t test_without_memory_every_ = 0;  // fh1_native_test_out_of_memory_every
  uint64_t reserves_of_texture_ = 0;
  bool releasing_by_missing_of_memory_ = false;  // guard against reentry
  uint64_t released_little_a_little_ = 0;
  uint64_t warning_trickle_ = 0;
  uint64_t attempt_eviction_ = 0;
  uint64_t textures_released_ = 0;
  // Diagnostic of why the cache grows: creations per address and last key per shape (address, format and
  // size). Only touched when a texture is created.
  uint64_t textures_created_ = 0;
  uint64_t created_in_address_view_ = 0;
  uint64_t created_same_shape_other_key_ = 0;
  std::unordered_map<uint32_t, uint32_t> created_by_address_;
  std::unordered_map<uint64_t, uint64_t> key_by_shape_;
  std::unordered_map<uint64_t, std::array<uint32_t, 5>> words_by_shape_;
  uint64_t keys_incoherent_ = 0;  // texture key guard (PrepareTexture)
  uint32_t warnings_other_key_ = 0;
  // "C6 counters".
  uint64_t passes_started_ = 0;
  uint64_t submissions_full_ = 0;
  uint64_t ns_submissions_full_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_uploaded_ = 0;
  uint64_t samplers_prepared_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t ns_passes_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t entries_computed_ = 0;
  uint64_t ns_entries_ = 0;
  uint64_t entries_reused_ = 0;
  uint64_t passes_by_generation_ = 0;
  uint64_t passes_by_target_ = 0;
  uint64_t passes_resumed_ = 0;
  uint64_t texels_passes_ = 0;
  std::array<uint64_t, kGpuCategories> texels_by_category_{};
  std::array<uint64_t, kGpuCategories> draws_by_category_{};
  std::chrono::steady_clock::time_point start_alternation_ps_ = std::chrono::steady_clock::now();
  bool alternation_ps_noted_ = false;
  bool alternation_only_alpha_noted_ = false;
  bool inv_size_tex_ = false;
  bool pcf_cheap_ = false;                    // a single shadow map sample
  bool pass_cars_shadow_ = false;            // fh1_native_shadow_minimum, car pass
  bool without_blur_frame_ = true;       // fh1_native_no_blur, per frame
  bool mip_point_test_ = false;            // Test: trilinear -> bilinear (measurement only)
  // Deduplication of vertex uploads within the frame.
  DedupeVertices dedupe_;
  bool dedupe_active_ = true;
  uint32_t dedupe_sinc_vista_ = 0;  // last value seen of g_synchronizations_ring
  std::set<uint64_t> warnings_invsize_resolved_;  // one trace per size
  bool alternation_scissor_noted_ = false;
  std::unordered_map<const EntryShader*, VkShaderModule> modules_only_alpha_;
  // The same module with OpExecutionMode EarlyFragmentTests (fh1_native_early_z).
  std::unordered_map<const EntryShader*, VkShaderModule> modules_z_early_;
  uint64_t draws_ps_useless_ = 0;     // no color and a PS that does not discard
  uint64_t draws_ps_needed_ = 0;  // no color, but the PS is needed
  uint64_t shadows_alpha_active_ = 0;
  uint64_t scene_with_discard_ = 0;
  uint64_t scene_without_discard_ = 0;   // shadow map draws with the alpha test enabled
  /*
   * fh1_native_early_z and fh1_native_skip_invisible. The first four split C6's "prevent it"
   * in two: those fixed by testing earlier and those that cannot be fixed because they write depth or
   * stencil (the latter are the exact size of a depth pre-pass).
   */
  enum : uint32_t {
    kZSet = 0,       // its depth test is moved earlier
    kZWritesZ,         // not possible: the draw writes depth
    kZStencil,         // not possible: stencil is involved
    kZOcclusion,         // left alone: a game occlusion query is open
    kZAlreadyEarly,       // no alpha test or kill: the GPU already tested early
    kInvisibleBlend,   // blending copies the destination: the draw paints nothing
    kInvisibleAlpha,     // alpha test with the NEVER function
    kInvisibleSoloColor,  // invisible in color but writes Z: only its color is removed
    kZCounts
  };
  std::array<uint64_t, kZCounts> counts_z_{};
  std::array<uint64_t, kZCounts> counts_z_previous_{};
  uint64_t z_early_without_module_ = 0;
  uint64_t frames_z_ = 0;
  uint64_t frames_z_previous_ = 0;
  std::chrono::steady_clock::time_point last_report_z_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point start_alternation_z_ = std::chrono::steady_clock::now();
  bool z_early_ = true;
  bool alternation_z_noted_ = false;
  bool skip_invisible_ = true;
  // Texture descriptor slots (EvictTexturesSiDoesMissing).
  std::array<bool, 4> slots_warned_{};
  uint64_t heap_full_ = 0;
  uint64_t heap_full_logged_ = 0;
  uint64_t warning_heap_full_ = 0;
  uint64_t shadows_alpha_off_ = 0;  // ... and without it (the stage would be unnecessary)
  std::array<uint64_t, kGpuCategories> triangles_by_category_{};
  std::array<uint64_t, kGpuCategories> passes_by_category_{};
  uint64_t ns_render_pass_ = 0;
  // Test fh1_native_skip_shadows_toggle_s.
  std::chrono::steady_clock::time_point start_shadows_ = std::chrono::steady_clock::now();
  bool shadows_skipped_ = false;
  // Diagnostic fh1_native_diag_repeated_vertices.
  struct VerticesSeen {
    uint64_t frame = UINT64_MAX;
    uint64_t fingerprint = 0;
  };
  std::unordered_map<uint64_t, VerticesSeen> vertices_seen_;
  uint64_t bytes_repeated_frame_ = 0;
  uint64_t bytes_equal_previous_ = 0;
  uint64_t ns_hash_vertices_ = 0;
  // Last texture and sampler result of each PS sampler register.
  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t generation = 0;
    uint32_t slot = 0;
    uint32_t heap = 0;
    uint32_t sampler = 0;
    uint64_t valid_until = 0;  // last frame it is valid without going back to PrepareTexture
    uint32_t width = 0;  // host image size, for 1/size
    uint32_t height = 0;
    float exp_scale = 1.0f;  // FH1: 2^(the fetch constant's exp_adjust + the resolve's exp_bias)
  };
  uint32_t warnings_1d_ = 0;
  // FH1: StreamInCache.
  static constexpr VkDeviceSize kSizeStreams = VkDeviceSize(32) << 20;
  struct StreamCached {
    uint64_t fingerprint = 0;
    uint32_t offset = 0;  // 0 = not in the buffer
    uint64_t frame = UINT64_MAX;  // last frame_ its guest bytes were compared
  };
  std::unordered_map<uint64_t, StreamCached> streams_;
  VkBuffer streams_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory streams_memory_ = VK_NULL_HANDLE;
  VkDeviceSize streams_size_real_ = 0;
  VkDeviceSize streams_used_ = 16;  // offsets under 4 mean "no stream" to the shader
  uint8_t* streams_data_ = nullptr;
  VkDeviceAddress streams_address_ = 0;
  uint32_t streams_type_ = UINT32_MAX;
  bool streams_coherent_ = true;
  bool streams_failed_ = false;
  bool exp_bias_ = true;              // fh1_native_exp_bias
  bool vs_textures_ = true;           // fh1_native_vs_textures
  bool cache_between_frames_ = true;  // fh1_native_texture_cache_across_frames
  bool mipmaps_ = true;                 // fh1_native_mipmaps
  bool cube_levels_ = true;             // fh1_native_cube_levels
  bool gamma_pwl_ = true;               // fh1_native_gamma_pwl
  std::unordered_set<uint64_t> kinds_gamma_;  // logged once each
  bool gamma_textures_ = true;          // fh1_native_gamma_textures
  bool gamma_targets_ = true;           // fh1_native_gamma_targets
  bool diag_mips_ = false;              // fh1_native_diag_mips
  uint64_t mips_reviewed_ = 0;
  uint64_t mips_rare_ = 0;
  uint64_t levels_mip_uploaded_ = 0;    // report: mip levels (without the base) in created textures
  std::array<CacheSampler, 16> cache_samplers_{};
  // Second cache, by the whole fetch constant: a direct-mapped table (the hash picks the slot). It went
  // from 256 to 1024 slots because in a race there are ~340 distinct textures per frame and 256 slots
  // collided (64 KB: fits in L2).
  /*
   * From 1024 to 4096 (288 KB, still fits in the 2 MB L2). At the Heritage & Omega exit there are ~3,000
   * draws per frame and ~205 misses of this table per frame (C6 counters: 97,709 in 20 s), at ~15 us
   * each through PrepareTexture (the texture stage measures 1.2-1.5 us per draw with 3.4 % misses). With
   * N distinct fetches in M slots, the fraction sharing a slot with another is 1 - e^(-N/M): with ~600
   * that is 44 % at 1024 and 14 % at 4096. The hit conditions do not change: there is just more room. The
   * misses-by-cause report (every 10 s) says how many were collisions.
   */
  std::array<CacheSampler, 4096> cache_fetch_{};
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t fetch_misses_collision_ = 0;      // The slot held another fetch constant
  uint64_t fetch_misses_empty_ = 0;       // slot not used yet
  uint64_t fetch_misses_generation_ = 0;  // same fetch, but generation_textures_ changed
  uint64_t fetch_misses_stale_ = 0;    // same fetch, but the texture is due for a check (valid_until)
  std::array<uint64_t, 6> fetch_report_previous_{};
  std::chrono::steady_clock::time_point fetch_report_{};
  uint64_t generation_textures_ = 0;  // changes with every C2 copy and every retired image
  // On-disk pipeline cache (LoadCachePipelines and SaveCachePipelines).
  using FnCreateCachePipelines = VkResult(VKAPI_PTR*)(VkDevice, const VkPipelineCacheCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkPipelineCache*);
  using FnDataCachePipelines = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, size_t*, void*);
  using FnDestroyCachePipelines = void(VKAPI_PTR*)(VkDevice, VkPipelineCache,
                                                    const VkAllocationCallbacks*);
  FnDataCachePipelines data_cache_ = nullptr;
  FnDestroyCachePipelines destroy_cache_ = nullptr;
  VkPipelineCache cache_pipelines_ = VK_NULL_HANDLE;
  uint32_t pipelines_without_save_ = 0;
  uint32_t saved_cache_ = 0;
  size_t bytes_cache_saved_ = 0;  // size of the last file read or saved
  std::chrono::steady_clock::time_point cache_saved_{};
  // Thread that writes the pipeline cache to disk (WriterCacheMain). It is persistent, because on
  // Horizon detach() closes the game, and wake-ups are decided with the lock held.
  std::thread writer_cache_;
  std::mutex writer_mutex_;
  std::condition_variable writer_warning_;
  std::vector<uint8_t> writer_data_;
  bool writer_pending_ = false;
  bool writer_stop_ = false;
  // The single pipelines file. The last written version of each part (or the one read at start-up): owned
  // by the writer thread once it is running, and by Initialize before that. list_read_ goes from
  // LoadCachePipelines to LoadListPipelines.
  std::vector<uint8_t> written_cache_;
  std::vector<uint8_t> written_list_;
  std::vector<uint8_t> list_read_;
  bool files_old_ = false;  // the two older files were read: deleted when the new one is written
  uint64_t ns_pipelines_ = 0;  // creating pipelines (report C6)
  std::unordered_set<uint32_t> warned_;
  // Pipeline prewarm (LoopPrewarm). list_file_ and state_list_ are sized before the thread
  // starts and never change size; the thread writes state_list_[i] before publishing
  // prewarm_until_ > i. Anything not atomic and not marked otherwise belongs to the ring only.
  std::vector<RegisterPipeline> list_file_;         // the list read at start-up (walked by the thread)
  std::vector<uint8_t> state_list_;                   // kList* of each record of list_file_
  std::unordered_map<uint64_t, size_t> index_list_;   // XXH3 of the key -> index (SIZE_MAX: from this session)
  std::vector<RegisterPipeline> list_session_;          // this session's new ones
  uint32_t list_without_save_ = 0;
  std::vector<uint8_t> writer_list_;                 // under writer_mutex_
  std::thread prewarm_thread_;
  const ShadersNative* library_prewarm_ = nullptr;
  uint32_t prewarm_eds_ = 0;
  bool prewarm_decided_ = false;
  bool prewarm_difference_ = false;
  std::chrono::steady_clock::time_point prewarm_start_{};
  std::chrono::steady_clock::time_point prewarm_report_{};
  uint64_t prewarm_changes_previous_ = 0;
  std::atomic<bool> prewarm_stop_{false};
  std::atomic<bool> prewarm_finished_{false};
  std::atomic<size_t> prewarm_until_{0};
  std::atomic<uint32_t> prewarm_done_{0};
  std::atomic<uint32_t> prewarm_compiled_{0};
  std::atomic<uint64_t> prewarm_ns_compiled_{0};
  std::atomic<uint32_t> prewarm_without_shader_{0};
  std::atomic<uint32_t> prewarm_other_mode_{0};
  std::atomic<uint32_t> prewarm_failed_{0};
  std::atomic<int32_t> prewarm_priority_{-1};
  uint32_t prewarm_counted_ = 0;  // of prewarm_compiled_, already added to pipelines_without_save_
  uint64_t ring_prewarmed_ = 0, ring_prewarmed_slow_ = 0, ns_ring_prewarmed_ = 0;
  uint64_t ring_of_list_ = 0, ns_ring_of_list_ = 0;
  uint64_t ring_new_ = 0, ns_ring_new_ = 0, ring_a_list_ = 0;
};

}  // namespace

std::unique_ptr<DrawsVulkan> DrawsVulkan::Create(const VulkanDevice* vulkan_device,
                                                    rex::memory::Memory* memory_block,
                                                    ContextTargets* context_id) {
  if (!vulkan_device || !memory_block || !context_id) {
    return nullptr;
  }
  auto draws = std::make_unique<DrawsVulkanImpl>(vulkan_device, memory_block, context_id);
  if (!draws->Initialize()) {
    REXLOG_ERROR("[native] C6: the native draws could not be prepared");
    return nullptr;
  }
  REXLOG_INFO("[native] C6: native draws prepared");
  return draws;
}

}  // namespace fh1::native
