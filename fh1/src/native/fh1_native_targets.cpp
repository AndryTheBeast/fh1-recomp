// fh1 - native renderer, part C2 (see fh1_native_targets.h).
//
// WHAT IT COVERS
//   - k_8_8_8_8 and k_8_8_8_8_GAMMA color render targets without MSAA, as
//     R8G8B8A8 images of pitch x max(720, pitch) (capped at 2048).
//   - Copy of a rectangle of the render target to the resolved texture at
//     RB_COPY_DEST_BASE (destination format k_8_8_8_8), with the rectangle and
//     the base computed like GetResolveInfo (graphics/util/draw.cpp:765-1010).
//   - Render targets with MSAA: used with 1 sample (MSAA hangs NVK on the Switch).
//   - Color clear with RB_COLOR_CLEAR, converted like the emulation does
//     (vulkan/render_target_cache.cpp:5350-5355).
//   - Presentation: the resolved texture requested by fetch constant 0 of the
//     Swap is drawn into the presenter output with the SDK's own shaders. By
//     default, with the gamma ramp the game loads, like the Xbox 360 display
//     (shaders/fh1_output_gamma_ramp.frag).
//
// DRAWS (parts C3-C6, fh1_native_draws.cpp)
//   This class lends them the render targets (color and depth), the frame's
//   command buffer and an upload one that is submitted right before it.
//
// WHAT IT DOES NOT COVER
//   Copies from depth, 16- and 32-bit formats, 3D textures as destination,
//   clearing only the rectangle (the whole image is cleared) and the normal
//   draws (parts C3-C6, in fh1_native_draws.cpp).
//
// Everything is used only by the native system's PM4 ring thread: no locks.

#include "fh1_native_targets.h"
#include "fh1_native_capture.h"
#include "fh1_hitch_waits.h"
#include "fh1_fence_wait.h"
#include "fh1_native_shaders.h"  // Samplers of the PS (fh1_native_diag_readers_s)
#include "fh1_reflection_on_demand.h"  // road reflection only when it is read

#include "fh1_graphics_settings.h"

#include <fstream>
#include <rex/cvar.h>
#include <rex/frame_stats.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#if REX_PLATFORM_SWITCH
#include <rex/watchdog.h>
#endif
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#define FH1_DUMP_LOG(folder, name, rgba, w, h)   fh1::capture::SavePngRgba((folder / (name)).string().c_str(), rgba.data(), w, h)
#include <algorithm>
#include <array>
#include <chrono>

#if REX_PLATFORM_SWITCH
// Counter 0 of the console profiler (game FPS), the same one IssueSwap of the emulated path uses.
extern "C" void RexSwitchPerfCount(unsigned id);
// Interval of a long frame for stack sampling ("during the hitches" section).
extern "C" void RexSwitchPerfHitch(uint64_t start, uint64_t fin);
#endif
#include <cmath>
#include <filesystem>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_INT32(fh1_native_resolve_without_copy_toggle_s, 0, "FH1",
                     "Native renderer (test, build 154): with N > 0 toggles between copying and swapping every N "
                     "seconds, to compare captures of the same spot with the game paused");
REXCVAR_DEFINE_INT32(fh1_native_shadow_scale, 100, "FH1",
                     "Native renderer (19/09, build 201): draws the game's two 1600x1600 shadow maps at this "
                     "percentage and scales them up when resolving them. 100 = like the Xbox 360 (1600, which is "
                     "2000 of its 2048 EDRAM tiles); 64 = like the PC version of this same game (1024). The value "
                     "is taken when the first map is created and does not change at run time")
    .range(50, 100);
REXCVAR_DEFINE_BOOL(fh1_native_resolve_without_copy, true, "FH1",
                    "Native renderer (17/09, build 154; default since 161, measured on the console: -2.36 ms of "
                    "the 7.41 of copies): when a whole target is resolved to a texture of the same size and "
                    "format, the images are swapped instead of copying the pixels. The image does not change; if "
                    "the game draws into that target again without clearing it first, the copy is restored");
/*
 * Swap also when the command does not clear the render target.
 *
 * The game resolves the shadow map twice per frame, once per cascade. The first command clears the
 * render target and therefore swaps (free); the second does not clear, and therefore copied the whole
 * 1600x1600: 2.56 Mpixels, 2.4 ms of real GPU time measured, 44 % of all copies in the frame.
 *
 * Requiring the command to clear was a precaution, not a necessity: if the game draws into the render
 * target again without clearing it, RestoreContent already brings the image back from the resolved
 * texture. So the worst case is paying the same copy later (net zero), not a regression. And the game
 * starts the next frame by clearing the shadow map, so there should not be a single restore.
 *
 * How to check it in the log: "C2 resolves without copying" must go from ~0.93 to ~1.9 per frame and
 * "restores" must stay at 0. If the restores go up, this does not help and should be turned off:
 * we would be paying for the copy anyway, just somewhere else.
 */
/*
 * Swap the color too, not only the depth.
 *
 * Image swapping (resolving without copying) works for depth and is worth 2.36 ms measured. For color it
 * never could, not because of the mechanism but because of a silly size detail: the color render target is
 * created with `height = max(720, pitch)`, so with pitch 1280 it is 1280x1280, while the resolved texture is
 * 1280x720. Since SwapWithResolved requires the same size, it always said no.
 *
 * The idea: the game never draws below row 720 in a color render target (the viewport and the scissor are
 * 1280x720 in every draw measured), so the pitch-1280 color render target was created 720 high and both
 * sizes matched.
 *
 * That is 4 copies of 1280x720 per frame = 3.69 Mpixels, 78 % of all copy traffic.
 *
 * Result (tested): it does not work, for two separate reasons.
 *   1. Without requiring a clear: 16,807 color swaps and 17,350 restores, almost one for one. The game
 *      draws on top again without clearing, so the copy is paid anyway, later and with two operations
 *      instead of one.
 *   2. Requiring a clear and creating the render target 720 high: the scene breaks (the screen fills with
 *      a yellow smear). The color render target cannot be shrunk to 720.
 * It stays off. The code is kept because the mechanism is correct; what fails is that this game reuses the
 * color render target without clearing it.
 *
 * The safety net is mandatory and did not exist for color: TargetColor did not call RestoreContent
 * (TargetDepth did). Without it, the first frame in which the game draws on top without clearing
 * would see the previous frame's content. It is added here.
 */
REXCVAR_DEFINE_BOOL(fh1_native_swap_color, false, "FH1",
                    "Native renderer (20/09): when the whole color target is resolved to a texture of the same "
                    "size, swap the images instead of copying 1280x720 pixels. That is 4 copies per frame, 78 % of "
                    "the copy traffic. The image does not change; if the game draws on top without clearing, it is "
                    "restored");

REXCVAR_DEFINE_BOOL(fh1_native_swap_without_clear, true, "FH1",
                    "Native renderer (20/09): swap the image also when the resolve command does not clear the "
                    "target. This removes the 1600x1600 copy of the second shadow map (2.4 real ms). If the game "
                    "draws on top without clearing, it is restored: worst case, the same as now. Watch 'restores' "
                    "in the log: it must stay at 0");
/*
 * The flickering shadows of the main menu. This was the cause.
 *
 * A resolve without a clear that is done by swapping leaves the content in the texture and the render target
 * with that texture's old image (content_invalid). It was only brought back when drawing on top
 * (TargetDepth) and never before another resolve, which read the old image as it was.
 *
 * In a race it does not happen: between the two resolves of the shadow map the cars are drawn and that
 * brings it back. In the menu it does, every frame. The garage draws the car into the 1600x1600 map and
 * resolves twice in a row, without drawing in between: without a clear to 07CEA000 (read by the car body) and
 * with a clear to 086AE000 (read by the scenery). On the console, in the menu: 398 swaps without a clear + 398
 * with a clear and 0 restores in 10 s. With images A, B and C:
 *     draw into A -> resolve to 07CEA000: the texture keeps A (correct), the render target gets B (the old one)
 *                 -> resolve to 086AE000: the texture keeps B: the previous frame's map
 * The scenery always sampled the map from one frame earlier with the current frame's light matrix. While the
 * shadow framing does not change, it is not noticeable. As soon as it jumps (the menu camera comes in
 * rotating) the old map lands shifted and the wall to the right of the car goes dark for one frame: a
 * console capture shows it every ~4 game frames between seconds 8 and 9.
 *
 * Now, if the render target about to be resolved has its content in another texture, it is first brought
 * back by copying (without lending or the minimum trick: this is a resolve, not the car pass), and the
 * resolve continues as usual. It is the exact path: on the Xbox 360 both resolves read the same EDRAM. It
 * costs one 1600x1600 copy per frame in the menu (~1.5 ms of GPU); in a race nothing is touched because the
 * render target already arrives valid. Report line: "C2 restores per frame", the "to
 * resolve" figure. false = previous behavior.
 */
REXCVAR_DEFINE_BOOL(fh1_native_resolve_valid_content, true, "FH1",
                    "Native renderer (26/09, build 193): before resolving a depth target whose content went away "
                    "in a swap, bring it back. Fixes the flicker of the menu shadows (the scenery sampled the "
                    "previous frame's map). false = as in 192");
/*
 * The verdict on swap_without_clear, with numbers measured on the console.
 *
 * Console log, three consecutive 10 s intervals in a race:
 *     swaps without clear       4679 -> 4885 -> 5145 -> 5369   (+206, +260, +224)
 *     restores                  3584 -> 3790 -> 4050 -> 4274   (+206, +260, +224)
 * One restore per swap, exactly, in all three intervals. So the pixel saving is exactly zero: the
 * 1600x1600 copy is not avoided, it is paid later and with two operations (swap + restore) instead of one.
 *
 * And it is the missing piece to make the "copies" breakdown of the C2 report add up:
 *     recorded inventory  2.67 Mpixels x 0.78 ms/Mpixel = 2.08 ms
 *     restores            0.83/frame x 2.56 Mpixels x 0.78 = 1.66 ms  <-- not recorded
 *     scaling blit 1280x720 -> 1024x576 (internal resolution)         = 0.49 ms
 *                                                            total    4.23 ms
 *     measured: 2.69 raw x 1.627 = 4.38 ms real.
 * Restores were 38 % of the frame's copies and did not show up in any bucket. They are recorded from here
 * on (NoteCopy in RestoreContent), so that the inventory adds up by itself.
 *
 * The setting is not turned off: turning it off moves the copy back to the resolve and costs the same. What
 * has to go is the restore, which is what the two settings below are for.
 */
REXCVAR_DEFINE_BOOL(fh1_native_restore_useful_area, true, "FH1",
                    "Native renderer (20/09 night): when bringing back the content of a swapped target, copy only "
                    "the rows the game really resolves instead of the whole image. Targets are created with height "
                    "= max(720, pitch) (the scene is 1280x1280 to draw 1280x720), and what lies below the useful "
                    "area is never drawn nor read");
/*
 * Restore without copying a single pixel.
 *
 * The render target lost its content in a swap: it is in the resolved texture R. Normally R -> target is
 * copied (10.24 MB for the shadow map, 1.66 ms real per frame). But the content does not have to be
 * duplicated, it only has to be in the render target: swapping the two images again is enough. Cost: zero
 * bytes, only handles.
 *
 * What it costs: R keeps the render target's old image until the game resolves to that address again. For
 * the shadow map that happens in the same frame and before anybody reads it:
 *     [cascade 1 pass]  <- restored here, R is lent out
 *     resolve cascade 1 -> another address
 *     [cascade 2 pass]
 *     resolve cascade 2 -> R          <- R gets good content back
 *     reflection, cubemap, SCENE      <- the scene is what samples R
 * So in steady state it is safe. In transitions (menu, loading, pause) it may not be.
 *
 * That is why it is off and watched: 'borrowed reads' in the C2 report counts the times someone
 * requested a resolved texture while it was lent out. If the log says 0, this is worth 1.66 ms real and
 * gets turned on. If it says anything else, the image could show the previous frame's shadows and it stays
 * off.
 */
/*
 * It stayed on, but the watchdog reading flipped. Read this.
 *
 * The mix-up: it was turned on citing "borrowed reads 0, clean" from an earlier run. That zero proved
 * nothing: the cvar was false in the toml, not a single image was lent, and the watchdog counted zeros
 * because it had nothing to count. The first session in which this path actually ran says the opposite:
 *     borrowed reads 8226   -> *** THE LOAN IS NOT SAFE ***
 * One read per frame, exactly, during the five minutes of racing. The premise written above ("in steady
 * state it is safe, the risk is the transitions") is backwards: the transitions gave 9 isolated reads and
 * the steady state gives one every time.
 *
 * And even so it stayed on, because what it buys is measured and was the best variance improvement so far,
 * and variance is what the player notices (same load, without vs. with):
 *     copies         4.29 -> 1.93 ms real        frame       38.63 -> 36.11 ms
 *     deviation      8.19 -> 7.06 ms             > 50 ms     6.14 % -> 3.66 %
 *     frame median   43.0 -> 33.3 ms
 * Turning it off brings the median back to 43 ms, for a risk that in six minutes of play did not produce a
 * single visible fault. The "C2 borrowed reads, by address" line reports what is read, with address
 * and size: if it is the 1600x1600 shadow map, it has to be fixed (by returning the render target, which is
 * where the good content is); anything else may be harmless.
 */
/*
 * Off. This is what left the car without a shadow (bridges, trees).
 *
 * The race shadow pass resolves the same render target twice (sub_82443B18):
 *   1. after the world, without a clear  -> texture[1] ([0x82A15374], 07CEA000)   sub_824427F8
 *   2. after the cars, with a clear      -> texture[0] ([0x82A15370], 086AE000)   sub_82442908
 * The car body (effect type 7, sub_824511E8) samples 1 (the map without cars, so as not to shadow itself)
 * and the world samples 0. Between the two resolves the game draws the cars on top without clearing, so
 * here the render target was restored by swapping: texture[1] got its old image back and stayed lent out
 * until the next frame's resolve. The scene reads it before that: in a console run, the increase of
 * "borrowed reads" is exactly the number of reads of 07CEA000 in each race report (369/369, 385/385,
 * 336/336...). And since it always gets the same image back, the car does not read the previous frame's
 * map: it reads one frozen since the start of the race. The ground darkens under the bridge and the car
 * does not.
 *
 * It costs the 1600x1600 copy this used to save (2.56 Mpixels, ~2.00 ms real per frame in a race): that
 * saving was fake. With false, "borrowed reads" and "by swap" must stay at 0.
 */
REXCVAR_DEFINE_BOOL(fh1_native_restore_by_swap, false, "FH1",
                    "Native renderer (20/09 night; turned OFF on 25/09): bring back the content of a swapped "
                    "target by swapping the images again instead of copying them. DO NOT TURN ON: it leaves "
                    "texture[1] of the shadow map lent, the one the car samples, and the car stops receiving "
                    "shadows. Watch 'lent reads' in the log: it must be 0");
/*
 * Depth that nobody samples is not copied (fh1_native_lazy_depth).
 *
 * Every race frame the game resolves the scene depth to a 1024x576 texture (091F0000 in the logs: 0.59
 * Mpixels and one copy per frame, "C2 resolved faces") and the only reader is the final composition
 * p_000139 (PS n19): it is its HEIGHTMAP, which only feeds the radial blur factor. With
 * fh1_native_no_blur (true by default) that sampling is dead (the specialization constant cuts
 * the blend and the compiler removes it), but the copy was still being paid: ~0.35 ms real per frame
 * (0.60 ms per Mpixel, which is what the 1600x1600 copy cost when it came back: copies went from 1.93 to
 * 3.47 ms).
 *
 * Now that copy is deferred, and only if in the last kLazyFrames frames that address was requested
 * by the composition without blur and no draw really sampled it. It is kept pending (source, destination,
 * rectangle):
 *   - if a draw really samples it (another shader, p_000140 with depth of field, or the composition with the
 *     blur on), it is copied right before that draw. The source has not changed: every write to it first
 *     goes through BeforeOfWriteDepth (pass, clear, restore, swap and destruction);
 *   - if the source is about to be written in another frame and the composition already requested it,
 *     nobody really read it: it is dropped without copying (the normal case in a race: the next frame's
 *     scene clear);
 *   - if the source is about to be written in the same frame, or without the composition having requested
 *     it, it is copied (exact);
 *   - if another resolve arrives at the same address while the copy is still deferred, it is copied first
 *     (exact).
 * GUARD: if a draw really samples an address whose copy was dropped (a late read: a previous frame's depth
 * after the next frame has started drawing), DIFFERENCE in the log and it turns off for the session.
 * Report line: "C2 lazy depth". false = always copy, as before.
 */
// The first version saved nothing (the copy was recorded if the source was rewritten in the same frame, which
// is always the case); with the current BeforeOfWriteDepth rule it is dropped. On again.
REXCVAR_DEFINE_BOOL(fh1_native_lazy_depth, true, "FH1",
                    "Native renderer (25/09, build 184): the depth the game resolves for the final composition is "
                    "only copied if a draw really samples it (without the blur, none does). The image does not "
                    "change; ~0.35 ms of GPU per frame. Checks itself. false = always copy, as before");
/*
 * The front buffer is drawn from its render target, without copying it (fh1_native_lazy_front).
 *
 * At the end of each frame the game resolves the 1024x576 output (the pitch-1040 render target) to one of
 * its two front buffers (09430000 and 09670000) and the Swap draws it with PaintOutput. That is 0.59
 * Mpixels of copying per frame (~0.35 ms real, at 0.60 ms per Mpixel) and across the measured sessions no
 * draw has ever sampled a front buffer: 0 reads in every session ("C2 resolved faces"). Only the Swap
 * reads them.
 *
 * If the address is a front buffer (a Swap drew it in the last kFrontFrames and no draw sampled it)
 * and the copy is 1 to 1, of the whole texture and from the corner of the render target, it is deferred:
 *   - in the Swap it is drawn from the image that holds the content with the exact variant of the output
 *     (texelFetch of the pixel, output of the front buffer's size and without FXAA, which is the usual
 *     one): texel (x, y) of that image is the same byte the copy would have left at (x, y) of the texture.
 *     With FXAA, without the ramp or with another output, the copy is recorded before submitting the work
 *     and the texture is drawn, as always;
 *   - if the game clears the whole render target (which it does when the next frame starts), the render
 *     target gets a spare image of the same size and the one with the content is kept for the front
 *     buffer: the clear does not need what was there, so nothing is copied. There are at most
 *     kFrontImagesMax spares (~4 MB each);
 *   - if the render target is written some other way first (a pass, a restore, a swap), if a draw samples
 *     the front buffer or if another resolve arrives that does not cover it entirely, the copy is recorded
 *     first (exact);
 *   - if another resolve arrives that covers it entirely, the deferred copy is unnecessary and dropped.
 * The content is never lost: the image is the same by construction. GUARD: if something requests a front
 * buffer whose copy can no longer be made, DIFFERENCE in the log and it turns off for the session. Report
 * line: "C2 lazy front buffer". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(fh1_native_lazy_front, true, "FH1",
                    "Native renderer (25/09, build 184): the Swap paints the front buffer from its render target "
                    "(or a retained image) instead of first copying it to the texture: 0.59 Mpixels less per "
                    "frame. Same image; with FXAA it is copied as always. Checks itself. false = always copy, as "
                    "before");
/*
 * The shadow map without the 1600x1600 copy (fh1_native_shadow_minimum).
 *
 * The race shadow pass (sub_82443B18) draws the world into the 1600x1600 render target, resolves it without
 * a clear to texture[1] (07CEA000: the map without cars, sampled by the car body), draws the cars on top and
 * resolves it with a clear to texture[0] (086AE000: the map with cars, sampled by the world). Since we
 * resolve by swapping images, after the first resolve the render target no longer holds the world and it
 * has to be brought back before the cars: that is the 1600x1600 copy (2.56 Mpixels, ~1.5 ms real per frame,
 * the most expensive copy in the frame).
 *
 * With this setting the render target is cleared to 1.0 instead of copied and the cars are drawn alone.
 * texture[0] ends up with the cars only, and the draws that sample it through their SHADOWMAP_SAMPLER
 * (tfetch2DShadowMin in the library) get min(texture[0], texture[1]): the 3D index word of that register
 * carries texture[1] and the pipeline carries the SPEC_CONSTANT_SHADOW_MINIMUM bit (23). This is exact, not
 * an approximation: with Z writes and a LESS or LEQUAL test, what a draw leaves on a buffer is the minimum
 * of what was there and of its fragments, and min(world, min(1, cars)) = min(world, cars) because no texel
 * exceeds 1.0. The map is point-sampled and both views carry the same swizzle: the result is the same value
 * the copy would have left. The depth bias is the same on both paths (same pipelines). With the cheap PCF
 * there is one extra read per fragment that samples the world shadow; with the 9-sample PCF it would be nine
 * and it does not pay off: without fh1_native_cheap_pcf it copies as always.
 *
 * It costs a clear of 2.56 Mpixels (~0.19 ms at 0.075 ms per Mpixel) and that read; it saves the copy
 * (~1.54 ms).
 *
 * GUARD, self-checking:
 *   - WATCHING: it copies as always and watches the whole cycle (resolve without a clear by swapping, car pass
 *     on the same render target, resolve with a clear by swapping), that every draw of the car pass is exact
 *     (pass without color, without stencil, without an occlusion query and, if it writes Z, a NEVER, LESS or
 *     LEQUAL test) and that every read of texture[0] comes from a shader with tfetch2DShadowMin in that
 *     register and outside the window between the two resolves. Those reads already use the pipeline with
 *     the bit (minimum with itself: the same texel), so switching to applying does not create pipelines in
 *     the middle of a race. After kShadowMinimumCycles clean cycles in a row it moves to
 *   - APPLYING: clears instead of copying. If anything watched fails, DIFFERENCE in the log (REXLOG_ERROR) and
 *   - OFF for the session: it copies again. A library without tfetch2DShadowMin never leaves watching.
 * Report line: "C2 shadow by minimum". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(fh1_native_shadow_minimum, true, "FH1",
                    "Native renderer (25/09, build 184): the world's shadow map is no longer restored by copying "
                    "1600x1600; the cars are drawn on the cleared target and the world samples the minimum of the "
                    "two textures. Same image. Needs the library with tfetch2DShadowMin. Checks itself. false = "
                    "copy as before");
REXCVAR_DEFINE_INT32(fh1_native_shadow_minimum_toggle_s, 0, "FH1",
                     "Native renderer (25/09, build 184, test): with N > 0 and the guard already applying, toggles "
                     "every N seconds between the minimum (even intervals) and the usual copy (odd ones), to "
                     "measure the net gain on the console with 'C2: GPU per Swap'. 0 = no toggling");
/*
 * Repeated clears.
 *
 * Clears are 0.41 raw = 0.67 ms real per frame (9 per frame: 4 color and 5 depth).
 * vkCmdClearColorImage and vkCmdClearDepthStencilImage clear the whole image, and the game requests some
 * clears on a render target that is already cleared to that same value and has not been drawn to since.
 * That clear does not change a single bit.
 *
 * The condition is deliberately conservative: it is skipped only if (a) the last clear of that render
 * target used the same value, (b) the global draw counter has not gone up since then (meaning nothing has
 * been drawn anywhere) and (c) the render target has not changed image through a swap nor received a
 * restore. With that it is impossible to skip a clear that is needed.
 */
REXCVAR_DEFINE_BOOL(fh1_native_skip_repeated_clears, true, "FH1",
                    "Native renderer (20/09 night): skip a clear when the target is already cleared to that same "
                    "value and nothing has been drawn since. Not a single pixel changes; 'clears skipped' in the "
                    "C2 report says how many are saved");
/*
 * How much of each clear is used (fh1_native_diag_clears).
 *
 * Clears are ~0.70 ms real per frame in a race (8 color and 9 depth) and they all clear the whole image:
 * 1280x1280 for a 1280x720 scene, 1040x1040 for a 1024x576 output, 320x720 for a 256x256 cubemap face. To
 * clear less, it must first be known, per render target, which part is really used until the next clear:
 * what the passes load and store (their renderArea, the useful area), the rectangles that are resolved,
 * what is restored and what is swapped (whole). Measurement only: it does not change the image. One line
 * every 20 s: "C2 clears per target". false = not measured.
 */
// false by default. Measured: the clip to the useful area that needs it only saved 0.09 ms of GPU, and the
// tracking runs on the PM4 ring thread, which is now the bottleneck.
REXCVAR_DEFINE_BOOL(fh1_native_diag_clears, false, "FH1",
                    "Native renderer (25/09, build 184, diagnostic): per target, the cleared area against the area "
                    "used until the next clear (passes, resolves, restores and swaps). One line every 20 s ('C2 "
                    "clears per target'). Does not change the image");
/*
 * Color clears only over the area in use (fh1_native_clear_useful_area).
 *
 * vkCmdClearColorImage clears the whole image: 1280x1280 for a 1280x720 scene, 1040x1040 for the 1024x576
 * output, 320x720 for a 256x256 cubemap face. With the measurement of fh1_native_diag_clears (the
 * largest height used by passes, resolves, restores and swaps of that render target, plus one row of margin,
 * rounded up to the next multiple of 64), once a render target has kAreaUsefulCycles measured clears only that
 * top band is cleared, with a loadOp = CLEAR pass (the same thing NVK does internally, over fewer pixels).
 * The bottom band is recorded with its color and cleared before any use that reaches it: the result is the
 * same as clearing it all. Draws do not touch it: NVK clips each pass to its renderArea (SET_SURFACE_CLIP)
 * and the pass that reaches the band completes it before it opens. If a render target needs that 3 times,
 * it stops being clipped (its useful area is not stable). Color only: depth without TRANSFER_DST is cleared
 * per pass and that pass clears the whole ZCULL region (NVK), so it is not clipped. GUARD: if a band cannot
 * be completed, DIFFERENCE in the log and it turns off for the session. false = clear the whole image, as
 * before.
 */
// false by default. Measured: 0.09 ms of GPU per frame (1.18 Mpixels not cleared): not worth it with the
// PM4 ring as the bottleneck.
REXCVAR_DEFINE_BOOL(fh1_native_clear_useful_area, false, "FH1",
                    "Native renderer (25/09, build 184): color clears clear only the rows that are really used; "
                    "the rest is cleared before something uses it. Same image. Needs fh1_native_diag_clears. "
                    "false = the whole image, as before");
// Defined in fh1_native_draws.cpp; here it is only read so as not to open two queries of the same type
// at once.
REXCVAR_DECLARE(int32_t, fh1_native_per_draw_statistics_s);
REXCVAR_DECLARE(bool, fh1_barriers);
REXCVAR_DECLARE(bool, fh1_native_ssaa);
// fh1_native_shadow_minimum only pays off with the single-sample PCF (defined in fh1_native_draws.cpp).
REXCVAR_DECLARE(bool, fh1_native_cheap_pcf);
REXCVAR_DEFINE_BOOL(fh1_native_pipeline_statistics, false, "FH1",
                    "Native renderer (17/09, build 156): counts shaded fragments, vertex invocations and clipped "
                    "primitives per pass type (C2 report). The image does not change, but the queries cost GPU "
                    "time: only for measuring");
REXCVAR_DEFINE_BOOL(fh1_native_diag_clear, false, "FH1",
                    "Native renderer: clear each render target with its own color instead of the game's color "
                    "(tests only: checks copy, clear and presentation)");
REXCVAR_DEFINE_BOOL(fh1_native_diag_resolved, false, "FH1",
                    "Native renderer: present the resolved textures of each frame as a mosaic, in the order of "
                    "their copies (tests only)");
/*
 * Who reads each resolved texture (fh1_native_diag_readers_s, measurement only).
 *
 * "C2 resolved faces" counts copies and reads per address, but a read is a call to TextureResolved and
 * only happens when the sampler caches of DrawsVulkan miss (per frame and per fetch constant): it does not
 * say which shader reads nor after which copy. With 098B0000 (the 1024x576 scene) that leaves the question
 * open: it is written twice per frame (the blit of the 1280x720 scene and, after the composition, the
 * composited scene 1 to 1) and it has two reads per frame. If both come from the first write, nobody reads
 * the second one before the next frame covers it entirely, and it is unnecessary (0.59 Mpixels, ~0.35 ms
 * real per frame).
 *
 * Every N seconds two whole frames are examined: every logical write to a resolved texture (the color or
 * depth resolve, even if the real copy is deferred or is a swap), every draw that samples it according to
 * the fetch constants of its pixel shader, bypassing the caches, and the Swap that draws the front buffer.
 * At the end, one line per address: for each write, who reads it until the next one (PS nN, sampler, draws
 * and the render target being drawn into) and SOBRA if nobody reads it and the next one covers it entirely.
 * What is read before the first write of the window goes to the last one before it ("[before]"). The
 * per-address lines are only printed if their pattern changes (the first three windows, all of them); the
 * summary line always is.
 * A write that comes out as SOBRA stays watched in every frame (menus, pause, rain, cutscenes...),
 * separating the reads before its source is written again (a deferred copy covers them: it would be recorded
 * right before that draw; this is the case of the raindrops on the screen with 098B0000) from the ones after
 * (with a single one, dropping the copy when the source is written would not be exact). The first of each
 * kind is reported; the counts, in every summary. It is the evidence needed before deferring or dropping a
 * copy.
 * Cost: outside the window, one if per draw and per copy (and, with something watched, comparing each
 * sampler's address with 1-4 addresses); in the two frames of the window, one lookup in the resolved map
 * per sampler (~0.2-0.3 ms per frame). 0 = off.
 */
// 0 by default. Measured: its watching only served the lazy composite copy, which saves nothing.
REXCVAR_DEFINE_INT32(fh1_native_diag_readers_s, 0, "FH1",
                     "Native renderer (25/09, build 184): every this many seconds two whole frames are watched "
                     "and, per resolved texture, it writes which pixel shaders read each write before the next one "
                     "(C2 readers lines); what comes out as UNUSED is then watched in every frame. Measurement "
                     "only. 0 = off")
    .range(0, 3600);
/*
 * The composited scene is only copied if someone reads it (fh1_native_lazy_composite).
 * The scene texture (098B0000 at 1024x576) is written twice per frame: the blit of the scene and, after the
 * composition, the composited scene 1 to 1 from the output (sub_82442478: VT, resolve and drops). The
 * second one is only read by the raindrops on the screen (sub_82448168, PS n145), which come right after and
 * before the HUD. It is deferred: it is recorded right before the first draw that samples it without its
 * source having been written again (exact), dropped if another write covers it entirely (exact) and dropped
 * when its source (the HUD) is written if nobody has read it: 0.59 Mpixels and ~0.35 ms real of GPU per
 * frame without drops. It only applies to the (address, source) pair that the watching of
 * fh1_native_diag_readers_s has seen kCompositeALook times with no read after the source was written,
 * and it turns itself off if a draw samples the texture after its copy was dropped (DIFFERENCE in the log).
 * Without the diagnostic (fh1_native_diag_readers_s = 0) it never applies. false = always copy, as
 * before.
 */
// false by default. Measured: 19,895 of 21,041 copies were recorded anyway because a draw reads it almost
// every frame: 0 ms saved.
REXCVAR_DEFINE_BOOL(fh1_native_lazy_composite, false, "FH1",
                    "Native renderer (25/09, build 184): the 1-to-1 copy of the composed scene to its texture "
                    "(098B0000, before the HUD) is deferred: it is recorded if a draw samples it before its source "
                    "is written again (the raindrops) and dropped otherwise. Activates after the watching of "
                    "fh1_native_diag_readers_s and turns itself off at the first disagreement. false = always "
                    "copied");
REXCVAR_DEFINE_BOOL(fh1_native_resolved_hdr, true, "FH1",
                    "Native renderer: a float (HDR) render target resolved to a 32-bit texture format keeps a float "
                    "resolved texture, so values above 1.0 reach the bloom, the reflections and the final composite as "
                    "on the Xbox 360 (which stores them scaled by exp_bias). false = 8-bit resolved textures (cut at 1.0)");
REXCVAR_DEFINE_BOOL(fh1_hdr_float, true, "FH1",
                    "Carbon's HDR scene targets (k_2_10_10_10_FLOAT and its 16_16_16_16 alias) are real 16-bit float images, as "
                    "on the Xbox 360 (values above 1.0 and fine dark steps survive blending). false = aliased to 8-bit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_hitch_ms, 60, "FH1",
                     "Frames slower than this many ms get the three [hitch] lines in the log (who waited for whom, "
                     "GPU time, new textures). 25 catches the small dips while driving");
REXCVAR_DEFINE_BOOL(fh1_native_depth_bytes, true, "FH1",
                    "Native renderer: a resolved depth fetched as a color texture gives the console's bytes (24-bit "
                    "depth and stencil), as FH1's motion blur and depth of field expect. false = the depth value "
                    "in every channel, as before (smeared scene)");
REXCVAR_DEFINE_BOOL(fh1_native_depth_fill_color, true, "FH1",
                    "Native renderer: a depth-only rectangle that fills a depth buffer also reaches the 8-bit color "
                    "target on the same EDRAM (its bytes are written there before the next draw into it). FH1 clears "
                    "the shadow / headlight mask to white that way at evening. false = as before (the mask keeps "
                    "the daytime picture and the car turns green)");
REXCVAR_DEFINE_INT32(fh1_dump_resolved_at_s, 0, "FH1",
                     "Debug: after this many seconds, once, save every resolved colour image of the next frame as PNG "
                     "files in dump_resolved/ next to the executable (0 = never)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_read_resolved_texels, 4096, "FH1",
                     "Native renderer: resolved textures of up to this many texels are also copied to guest "
                     "memory, which the game reads for its exposure (0 = none; 4096 = 64x64, the ones the exposure "
                     "uses; 57600 = 320x180)");
REXCVAR_DEFINE_INT32(fh1_native_read_resolved_half_texels, 32768, "FH1",
                     "Native renderer: a resolve to a 16-bit float texture (k_16_16_16_16_FLOAT) of up to this many "
                     "texels is also written to guest memory, at once. FH1 computes a scene's ambient light on the "
                     "CPU from a 256x128 sphere map it has just resolved (paint booth, garage). 0 = never");
REXCVAR_DEFINE_INT32(fh1_native_read_one_off, 4, "FH1",
                     "Native renderer: a resolve to a destination that was not resolved in the last 60 frames is "
                     "written to guest memory at once, whatever its size, for this many resolves in a row (the "
                     "emulated GPU's readback_resolve_sync_one_off). FH1 reads the photo of a car back on the CPU "
                     "when a paint job is saved or a car is bought; without it the photo was whatever an older "
                     "picture had left at that address. 0 = only the small ones, as before");
REXCVAR_DEFINE_INT32(fh1_native_read_one_off_wait_texels, 210000, "FH1",
                     "Native renderer: a one-off resolve (fh1_native_read_one_off) into a texture of at least this "
                     "many texels (its pitch times its height) is in guest memory before the next command is read; "
                     "into a smaller texture it arrives when the GPU has finished the submission (a frame later). "
                     "The photos of a car (a 768x288 picture, 221184 texels) are read by the game at once. A "
                     "measured limit, the cause is not known: with 100000 the paint booth lost its ambient light "
                     "and the photos their car (the pictures between the two limits are 928x219 and 1824x114 ones "
                     "of the design creator's livery pass); 210000, 250000, 900000 and 1000000 kept the booth right");
REXCVAR_DEFINE_BOOL(fh1_native_diag_photo, false, "FH1",
                    "Debug: when the game resolves a piece of a car's photo (a one-off resolve into a 768x288 "
                    "texture), ask for a frame trace (with --fh1_native_diag_frame_s=-1) and a dump of every "
                    "resolved picture (with --fh1_dump_resolved_at_s=-1) at the next Swap");
REXCVAR_DEFINE_BOOL(fh1_native_read_resolved_float, false, "FH1",
                    "Native renderer: the small float pictures (levels of the reflection cube map, luminance) are "
                    "copied to guest memory too, converted to the format the game resolved them in. false = only the "
                    "8-bit ones (the default: tried on 2026-10-05, the picture did not change)");
REXCVAR_DEFINE_BOOL(fh1_native_invalidate_textures_every_copy, false, "FH1",
                    "Native renderer: drop the texture caches on every copy (the behavior before build 127). Since "
                    "127 they are only dropped when a resolved texture is created, remade, prepared or changes its "
                    "channel order");
/*
 * On by default. It was written earlier and left off without being measured.
 *
 * The baseline without overclock shows the ring waiting 17.24 ms per frame for the previous presentation
 * to finish: almost exactly one 60 Hz vsync. The frame's work is ~38 ms, i.e. 2.3 vsyncs, and it should land
 * on 3 (50 ms, 20 FPS); with that wait on top it goes to almost 4 (62 ms, 16 FPS).
 *
 * And it fits the SDK, which already has a mailbox of three output images made for this:
 *     static constexpr uint32_t kGuestOutputMailboxSize = 3;
 *     // mailbox for presenting ... without long interlocking between guest output refreshing and painting
 * The SDK hands out a different image on each refresh, so painting the next one while the previous one is
 * presented is the intended use, not a shortcut. It does not change the image.
 *
 * To compare without a new NRO: set it to false in the toml and repeat the race.
 */
/*
 * How many work slots are really used (there is room for 3).
 *
 * With 2 the CPU can only be one frame ahead of the GPU, and on the console that meant 4,826 ms out of
 * every 10 seconds stopped inside Record() waiting for the GPU to finish the previous work. With 3 it can
 * be two ahead. Each slot costs a 64 MB upload buffer.
 *
 * It is left selectable to compare without a new NRO: with 2, exactly the earlier behavior.
 */
/*
 * Four. Breaking a console run down by regime showed that the bottleneck changes with the stretch of track,
 * and that the third slot falls short exactly where the GPU is the limit:
 *
 *   few draws (straight):    GPU 31.37 ms | gap 2.05 | fence wait inside Record() 4.9-9.8 ms
 *   many draws (alley):      GPU 30.30 ms | gap 5.50 | fence wait ~0
 *
 * That is: on the straight the CPU arrives first and waits for the GPU to release the slot; in the alley it
 * is the other way round. With four slots the CPU can be three frames ahead and stops stalling in the first
 * case. It fits easily: the log says "heap 0 (GPU): 482 MB used of 1382 MB budgeted", and each
 * slot costs a 64 MB upload buffer.
 */
/*
 * It stays at three. The fourth one breaks the image, and the reason is known.
 *
 * A build with only this change (sky deferral off) showed the same artifacts seen before: flickering and
 * odd colors. So the fourth slot shared the blame, it was not innocent.
 *
 * The cause: the work slots and the output slots are two different things and only one is configurable.
 *
 *     work slots   (fh1_native_work_slots):  3 -> 4
 *     output slots (kSlotsOutput, :486):          3   FIXED
 *
 * `kSlotsOutput` governs the quad that draws the game image on screen: its pools, its descriptors, its
 * fences (`fences_output_`) and `outputs_pending_`, and the rotation at :3305 is `% kSlotsOutput`.
 * Letting the CPU get three frames ahead in the work reuses an output slot whose image is still being
 * displayed. Hence the artifacts.
 *
 * To raise it, kSlotsOutput must be raised at the same time and the four places above reviewed.
 * Changing only this number is not enough.
 *
 * -------- what was believed before, kept so that it is not repeated --------
 * Four, and this time on its own.
 *
 * It first went in together with the sky deferral and the lazy sealing. That build broke the image and all
 * three were reverted without knowing which one it was; then the sky counter showed the culprit was the
 * sky ("7.00 detected per frame" when 0.9 was expected). So this one comes back on its own, which
 * is how it should have been added from the start.
 *
 * What it buys, measured per regime:
 *   few draws (straight):    GPU 31.37 ms | gap 2.05 | waiting for the fence in Record() 4.9-9.8 ms
 *   many draws (alley):      GPU 30.30 ms | gap 5.50 | waiting for the fence ~0
 *
 * On the straight the CPU arrives first and stalls waiting for the GPU to release the slot. With four it is
 * three frames ahead. It costs 64 MB of upload buffer, out of the ~900 MB free in the heap.
 */
REXCVAR_DEFINE_INT32(fh1_native_work_slots, 3, "FH1",
                     "Native renderer: work slots (2 to 4). With more, the CPU runs more frames ahead of the GPU "
                     "and stalls less; each one costs 64 MB. 2 = as build 80")
    .range(2, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ZCULL (hierarchical depth culling), switchable.
//
// It is enabled by removing TRANSFER_DST from the game's depth buffers, which is what makes them eligible
// for a ZCULL plane in the driver. It measured -2.12 ms in the scene, but right after that the car body was
// observed darkening at times as if it were in shadow, and the scene and the reflection cubemap are exactly
// what ends up under ZCULL (the shadow map does not: it has TRANSFER_DST). It stayed off until a race ruled
// that out.
/*
 * On by default. It had been turned off on suspicion of darkening the car body, and that suspicion was
 * ruled out: the change of tone was the environment cubemap with faces_max=1 refreshing one face every six
 * frames, so the car's lighting lagged and then jumped. ZCULL is worth -2.12 ms measured and was already
 * enabled by hand in the test toml; the default value was misleading.
 */
REXCVAR_DEFINE_BOOL(fh1_native_zcull, true, "FH1",
                    "Hierarchical depth culling (ZCULL): removes TRANSFER_DST from the game's depth targets so the "
                    "driver gives them a ZCULL plane");

REXCVAR_DEFINE_BOOL(fh1_native_output_without_wait, true, "FH1",
                    "Native renderer: paints the output of each Swap rotating 3 slots instead of one, so it does "
                    "not wait for the GPU to finish the previous output (on the console the ring waited there 17 "
                    "ms per Swap, almost a vsync). Does not change the image; false goes back to the previous "
                    "behavior");
// In NVK, TOP_OF_PIPE is PIPELINE_LOCATION_NONE: the timestamp is released when the GPU reads the command,
// not when the previous work finishes, and the per-category breakdown is approximate (a copy gets charged
// with the draw of the previous pass). BOTTOM_OF_PIPE is PIPELINE_LOCATION_ALL: it is released when all
// previous work finishes.
/*
 * On by default. It was only enabled through the toml, and the toml overrides the default: if the toml
 * ever ships without that line, the improvement silently disappears. Three cvars like this were found, one
 * of them worth 2 ms and dead for several builds.
 */
REXCVAR_DEFINE_BOOL(fh1_native_precise_marks, true, "FH1",
                    "Native renderer (measurement): GPU timestamps between categories with BOTTOM_OF_PIPE (written "
                    "when the previous work finishes) instead of TOP_OF_PIPE (when the command is read): exact "
                    "split by category. Does not change the image");
REXCVAR_DEFINE_INT32(fh1_native_precise_marks_toggle_s, 0, "FH1",
                     "Native renderer (measurement): with N > 0 toggles normal (even intervals) and precise (odd "
                     "intervals) timestamps every N seconds and logs each change, to compare in the same run");
REXCVAR_DEFINE_INT32(fh1_native_reads_every, 1, "FH1",
                     "Native renderer: for each target, 1 in N small copies is read and written into guest memory "
                     "(1 = all); the game uses them for its exposure");
// The Xbox 360 passes the image through the gamma ramp the game loads, and NFSMW does not load the identity
// (measured: [64] = 273 and [128] = 539 in 10 bits, instead of 256 and 513).
REXCVAR_DEFINE_BOOL(fh1_native_gamma_ramp, true, "FH1",
                    "Native renderer: applies to the output the gamma ramp the game loads, like the Xbox 360's "
                    "screen (without it, midtones and shadows come out darker). false: the image as is, as before "
                    "build 137")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard (fh1_guard_30) lives in fh1_clip_shadows.cpp, which owns the lever it pulls.
namespace fh1::guard30 {
void Beat(double ms);
void Report();
}  // namespace fh1::guard30

#include "fh1_depth_pack_spirv.h"  // FH1: resolved depth fetched as bytes (TextureResolvedBytes)

namespace fh1::native {
namespace shaders {
// The same SPIR-V the SDK presenter uses to draw the game image
// (vulkan_presenter.cpp:128-141).
#include "vulkan_spirv/guest_output_bilinear_ps.h"
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
// The same sampling as guest_output_bilinear_ps with the game's gamma ramp
// (shaders/fh1_output_gamma_ramp.frag).
#include "shaders/fh1_output_gamma_ramp_ps.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

constexpr VkFormat kFormatColor = VK_FORMAT_R8G8B8A8_UNORM;

// NFSC: Carbon renders its scene into the Xbox 360 HDR formats (k_2_10_10_10_FLOAT and its 16_16_16_16 alias).
// First step: they are ALIASED to the 8-bit target (values above 1.0 clip), to bring up the whole pipeline; true
// floating-point targets come next (docs/carbon/native-renderer-plan.md).
inline bool FormatColorAccepted(uint32_t f) {
  using F = xenos::ColorRenderTargetFormat;
  return f == uint32_t(F::k_8_8_8_8) || f == uint32_t(F::k_8_8_8_8_GAMMA) || f == uint32_t(F::k_2_10_10_10) ||
         f == uint32_t(F::k_2_10_10_10_FLOAT) || f == uint32_t(F::k_2_10_10_10_AS_10_10_10_10) ||
         f == uint32_t(F::k_2_10_10_10_FLOAT_AS_16_16_16_16) ||
         // FH1: its scene and post-processing also draw into 16- and 32-bit-per-channel targets (FormatHostFh1).
         f == uint32_t(F::k_16_16) || f == uint32_t(F::k_16_16_16_16) || f == uint32_t(F::k_16_16_FLOAT) ||
         f == uint32_t(F::k_16_16_16_16_FLOAT) || f == uint32_t(F::k_32_FLOAT) || f == uint32_t(F::k_32_32_FLOAT);
}

// FH1: host format of a resolved texture for the wide copy-destination formats (VK_FORMAT_UNDEFINED = 8-bit).
inline VkFormat FormatCopyFh1(uint32_t format_target) {
  using C = xenos::ColorFormat;
  switch (C(format_target)) {
    case C::k_16_16:
    case C::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case C::k_16_16_16_16:
    case C::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case C::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case C::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}
inline uint32_t BytesTexelLog2Fh1(VkFormat f) {
  return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32_SFLOAT ? 3 : 2;
}

// FH1: host image format of the wide render-target formats (VK_FORMAT_UNDEFINED = the usual 8-bit or HDR choice).
// The 16-bit fixed formats (range -32..32 on the Xbox 360) are kept as half floats for now.
inline VkFormat FormatHostFh1(uint32_t f) {
  using F = xenos::ColorRenderTargetFormat;
  switch (xenos::ColorRenderTargetFormat(f)) {
    case F::k_16_16:
    case F::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}
// FH1 debug (fh1_dump_resolved_at_s): the float images are dumped too. Bytes per pixel of a format the dump can
// read (0 = not dumped), and its conversion to 8 bits: x / (1 + x), then gamma 2.2 ("_tm" in the file name).
// Negative values and NaN show as 0; the log line gives their count and the largest value per channel.
inline uint32_t DumpBytesTexel(VkFormat f) {
  switch (f) {
    case kFormatColor:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    default: return 0;
  }
}
inline float DumpHalfToFloat(uint16_t h) {
  const uint32_t exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v;
  if (exponent == 0) {
    v = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    v = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
  } else {
    v = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
  }
  return (h & 0x8000) ? -v : v;
}
struct DumpFloatStats {
  float maximum[4] = {0, 0, 0, 0};
  double sum[4] = {0, 0, 0, 0};
  uint64_t negative = 0, nan = 0, pixels = 0;
};
// rgba and alpha get width * height * 4 bytes each (alpha = the fourth channel as grey).
inline void DumpFloatToRgba(VkFormat f, const uint8_t* data, uint32_t width, uint32_t height,
                            std::vector<uint8_t>& rgba, std::vector<uint8_t>& alpha, DumpFloatStats& stats) {
  const uint32_t channels = f == VK_FORMAT_R16G16B16A16_SFLOAT ? 4 : f == VK_FORMAT_R32_SFLOAT ? 1 : 2;
  const bool half = f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R16G16_SFLOAT;
  const size_t pixels = size_t(width) * height;
  rgba.assign(pixels * 4, 0);
  alpha.assign(pixels * 4, 255);
  stats.pixels = pixels;
  for (size_t i = 0; i < pixels; ++i) {
    float v[4] = {0, 0, 0, 1};
    for (uint32_t c = 0; c < channels; ++c) {
      if (half) {
        uint16_t h;
        std::memcpy(&h, data + (i * channels + c) * 2, 2);
        v[c] = DumpHalfToFloat(h);
      } else {
        std::memcpy(&v[c], data + (i * channels + c) * 4, 4);
      }
      if (std::isnan(v[c])) {
        ++stats.nan;
        v[c] = 0;
      } else if (v[c] < 0) {
        ++stats.negative;
        v[c] = 0;
      } else if (std::isinf(v[c])) {
        v[c] = 65504.0f;
      }
      stats.maximum[c] = std::max(stats.maximum[c], v[c]);
      stats.sum[c] += v[c];
    }
    if (channels == 1) {
      v[1] = v[2] = v[0];
    }
    for (uint32_t c = 0; c < 4; ++c) {
      const float mapped = std::pow(v[c] / (1.0f + v[c]), 1.0f / 2.2f);
      const uint8_t byte = uint8_t(std::min(255.0f, mapped * 255.0f + 0.5f));
      if (c < 3) {
        rgba[i * 4 + c] = byte;
      } else {
        alpha[i * 4] = alpha[i * 4 + 1] = alpha[i * 4 + 2] = byte;
      }
    }
    rgba[i * 4 + 3] = 255;
  }
}
constexpr uint32_t kHeightMaximumTarget = 2048;
constexpr VkImageSubresourceRange kRangeColor = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange kRangeDepth = {
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
using FnCopyImage = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);
using FnClearDepth = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout,
                                             const VkClearDepthStencilValue*, uint32_t,
                                             const VkImageSubresourceRange*);
using FnBlit = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                uint32_t, const VkImageBlit*, VkFilter);

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fixed16p8(float input_value) {
  if (!(std::abs(input_value) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double scaled = std::clamp(double(input_value) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(scaled));
}

// Address of a texel in a 32x32-tiled texture (pipeline/texture/util.cpp:424-436).
int32_t OffsetTile2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// With 4-byte texels, OffsetTile2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (WriteReads).
const std::array<uint16_t, 1024>& TableTile2DTexel4() {
  static const std::array<uint16_t, 1024> table = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(OffsetTile2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return table;
}

int32_t ExtendSign15(uint32_t input_value) {
  return int32_t(input_value << 17) >> 17;
}

// Fixed color per render target with fh1_native_diag_clear: the
// capture shows which render target reached the screen.
VkClearColorValue ColorDiagnostic(uint32_t base, uint32_t format, uint32_t pitch) {
  uint32_t h = base * 2654435761u ^ pitch * 2246822519u ^ format * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  VkClearColorValue color{};
  for (uint32_t j = 0; j < 3; ++j) {
    color.float32[j] = float((h >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
  }
  color.float32[3] = 1.0f;
  return color;
}

using Image = ImageNative;  // prepared = already in GENERAL and cleared

struct Resolved {
  Image image;
  uint32_t format_guest = 0;
  bool swap_rb = false;
  // Times the draws have requested this address since the last report. It lives here and not in the report
  // map because TextureResolved is on the hot path (one call per texture and draw, ~8,000 per frame) and the
  // lookup of the resolved texture is done anyway.
  uint64_t reads = 0;
  // FH1: fingerprint of the guest memory at this address when the game last resolved here, and whether the game
  // has written other data there since (see TargetsVulkan::StampMemory).
  uint64_t stamp_memory = 0;
  uint64_t stamp_checked = UINT64_MAX;  // presentation count of the last check
  bool overwritten = false;
};

// C2 report of readbacks per render target (base and size) and cadence of fh1_native_reads_every.
struct TargetRead {
  uint32_t base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;    // small copies seen in the interval
  uint64_t skipped = 0;  // of those, the ones not read because of fh1_native_reads_every
};

// Rear-view mirror diagnostic: copies to a square resolved texture (NoteCopy).
// And of all the others too, with their size and how many times each one is read. That answers whether a
// copy is needed: a resolved texture that is copied every frame and never requested is wasted bandwidth.
struct CopyTarget {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;
  uint64_t with_draws = 0;  // copies with some draw since the previous copy
  uint64_t draws = 0;
  uint64_t pixels = 0;
};

// What is known about a render target between game commands. It serves two purposes:
//  - skipping a clear that changes nothing (fh1_native_skip_repeated_clears);
//  - restoring only the rows the game really uses (fh1_native_restore_useful_area).
// It lives in a separate map and not in ImageNative because that structure belongs to another file.
struct StateTarget {
  bool clear_clean = false;    // the content is exactly the last clear, with nothing on top
  uint64_t value_clear = 0;     // packed color, or depth+stencil
  uint64_t draws_to_clear = 0; // global draw counter at that moment
  uint32_t height_used = 0;        // the largest y1 the game has resolved from this render target
};

// Readback of a small resolved texture (fh1_native_read_resolved_texels):
// host-visible buffer it is copied to and what is needed to write it to the guest.
struct ReadAccess {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory_block = VK_NULL_HANDLE;
  uint8_t* data = nullptr;
  VkDeviceSize bytes = 0;
  bool coherent = true;
};
struct ReadPending {
  ReadAccess* read;
  uint32_t base;          // RB_COPY_DEST_BASE
  int32_t x0;             // texel of the texture where the rectangle starts
  int32_t y0;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;         // RB_COPY_DEST_PITCH
  uint32_t height_target;
  uint32_t info;          // RB_COPY_DEST_INFO
  VkFormat format_host = VK_FORMAT_R8G8B8A8_UNORM;  // FH1: of the resolved image (float pictures are converted)
};

// Output slots with fh1_native_output_without_wait (without it, only slot 0 is used).
constexpr uint32_t kSlotsOutput = 3;
// GPU timestamps per slot (the last one is kept for the final timestamp).
constexpr uint32_t kMarksBySlot = 128;
constexpr uint8_t kGpuFin = 0xFF;
// Host occlusion queries per work unit (draw spans of the game's queries).
constexpr uint32_t kOcclusionsBySlot = 32;
// fh1_reflection_visibility. Own occlusion queries per work unit: the draws that sample the reflection and
// the witness (the final composition). The ones that do not fit are assumed visible.
constexpr uint32_t kVisibilityBySlot = 16;
constexpr uint8_t kVisibilityWater = 0;
constexpr uint8_t kVisibilityWitness = 1;
// Passes measured per work unit. In a race there are ~16 per frame including the resumed ones.
constexpr uint32_t kStatisticsBySlot = 64;
constexpr uint32_t kCountersStatistic = 3;  // vertices, clipped primitives and fragments
// Draws measured in a diagnostic frame (the scene has ~1200).
constexpr uint32_t kStatisticsDrawBySlot = 2048;
constexpr uint32_t kLabelsShader = 512;
// Buckets of the copy breakdown by size (pixels of the copy).
constexpr uint32_t kBucketsCopy = 4;
constexpr uint32_t kPixelsBucket[kBucketsCopy] = {64 * 64, 320 * 320, 1024 * 1024, 0xFFFFFFFFu};

// One of the work slots: while a frame is being recorded, the previous ones can still be
// on the GPU with the other one.
struct SlotWork {
  VkCommandPool pool_work = VK_NULL_HANDLE;
  VkCommandPool pool_upload = VK_NULL_HANDLE;
  VkCommandBuffer work = VK_NULL_HANDLE;
  VkCommandBuffer upload = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  uint64_t order = 0;                        // submission number
  // Wall-clock time of the work, from vkQueueSubmit to the signaled fence. The GPU timestamps give
  // 30.5 ms per frame while the hardware's own counter says 99.7 % load on a 55 ms frame: either there is
  // work we do not mark or the timestamp scale is wrong. This bounds the truth from above (the CPU sees
  // the fence a little late) and the timestamps from below.
  std::chrono::steady_clock::time_point sent{};
  std::vector<ReadPending> reads;  // readbacks submitted with the work
  std::vector<uint8_t> categories;  // GPU category of each timestamp written
  bool marks_precise = false;      // intermediate marks with BOTTOM_OF_PIPE
  // Occlusion queries recorded in the work, in order from the slot's first one, with the number of the game
  // query they add to.
  std::vector<std::pair<uint32_t, uint64_t>> occlusions;
  // Statistics queries recorded, with the category of each one's pass.
  std::vector<std::pair<uint32_t, uint8_t>> statistics;
  // Per-draw queries, with the pixel shader number of each one.
  std::vector<std::pair<uint32_t, uint16_t>> statistics_draw;
  // Reflection visibility queries, with their type (kVisibilityWater or kVisibilityWitness).
  std::vector<std::pair<uint32_t, uint8_t>> visibility;
};

// A game occlusion query (from its Issue(BEGIN) to its Issue(END)) while its spans are being counted on the
// GPU.
struct QueryOcclusionGame {
  uint32_t base = 0;              // D3D counter structure
  uint64_t sample_total = 0;          // sum of the spans read
  uint32_t ranges_pending = 0;  // recorded and not yet read
  bool finished = false;         // its Issue(END) already arrived
  bool failed = false;           // some span without room or not read: not published
};

class TargetsVulkan final : public TargetsNative, public ContextTargets {
 public:
  TargetsVulkan(const VulkanDevice* vulkan_device, rex::memory::Memory* memory_block)
      : vulkan_device_(vulkan_device),
        dfn_(vulkan_device->functions()),
        device_(vulkan_device->device()),
        memory_(memory_block),
        family_(vulkan_device->queue_family_graphics_compute()) {}

  ~TargetsVulkan() override {
    WaitGpu();
    draws_.reset();  // their framebuffers and views point to these images
    DestroyDepthBytes();
    for (auto& [key, image] : depths_) {
      Destroy(image);
    }
    for (auto& [key, image] : targets_) {
      Destroy(image);
    }
    for (auto& [key, resolved] : resolved_) {
      Destroy(resolved.image);
    }
    for (auto& [key, resolved] : parked_) {  // FH1
      Destroy(resolved.image);
    }
    Destroy(tile_);
    for (ImageFront& spare : front_images_) {  // fh1_native_lazy_front
      Destroy(spare.image);
    }
    reads_pending_.clear();
    for (SlotWork& slot : slots_) {
      slot.reads.clear();
    }
    for (auto& [key, read] : reads_) {
      DestroyRead(read);
    }
    for (auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
      }
    }
    if (pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, pipeline_, nullptr);
    for (VkPipeline p : pipelines_ramp_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    if (fs_ramp_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_ramp_, nullptr);
    for (RampOutput& ramp : ramps_output_) {
      if (ramp.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buffer, nullptr);
      if (ramp.memory_block != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.memory_block, nullptr);
    }
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_descriptores_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorPool(device_, pool_descriptores_, nullptr);
    if (layout_descriptores_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorSetLayout(device_, layout_descriptores_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_, nullptr);
    if (vs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_, nullptr);
    if (render_pass_output_ != VK_NULL_HANDLE)
      dfn_.vkDestroyRenderPass(device_, render_pass_output_, nullptr);
    for (SlotWork& slot : slots_) {
      if (slot.fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, slot.fence, nullptr);
      if (slot.pool_work != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, slot.pool_work, nullptr);
      if (slot.pool_upload != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, slot.pool_upload, nullptr);
    }
    if (queries_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, queries_, nullptr);
    if (occlusions_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, occlusions_, nullptr);
    if (visibility_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, visibility_, nullptr);
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (fences_output_[i] != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fences_output_[i], nullptr);
      if (pools_output_[i] != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pools_output_[i], nullptr);
    }
  }

  bool Initialize() {
    // vkCmdCopyImage is not in the SDK's function table: it is requested from the driver.
    copy_image_ = reinterpret_cast<FnCopyImage>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdCopyImage"));
    if (!copy_image_) {
      REXLOG_ERROR("[native] C2: the driver does not provide vkCmdCopyImage");
      return false;
    }
    clear_depth_ = reinterpret_cast<FnClearDepth>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(
            device_, "vkCmdClearDepthStencilImage"));
    blit_ = reinterpret_cast<FnBlit>(
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdBlitImage"));
    // Attachment, copy and clear source and destination, and sampling of the resolved ones (shadows).
    const VkFormatFeatureFlags kUsesDepth =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    // To draw the shadow map smaller it has to be scaled up when resolving it, and that is a vkCmdBlitImage on
    // depth. If the driver does not provide it, nothing is scaled.
    const VkFormatFeatureFlags kUsesScaled =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatFeatureFlags uses_chosen = 0;
    for (const VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
      VkFormatProperties properties{};
      vulkan_device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
          vulkan_device_->physical_device(), candidate, &properties);
      const VkFormatFeatureFlags uses = properties.optimalTilingFeatures & kUsesDepth;
      if (!(uses & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        continue;
      }
      if (format_depth_ == VK_FORMAT_UNDEFINED || uses == kUsesDepth) {
        format_depth_ = candidate;
        uses_chosen = uses;
        depth_scalable_ =
            (properties.optimalTilingFeatures & kUsesScaled) == kUsesScaled;
      }
      if (uses == kUsesDepth) {
        break;
      }
    }
    if (format_depth_ != VK_FORMAT_UNDEFINED) {
      const VkFormatFeatureFlags missing = kUsesDepth & ~uses_chosen;
      REXLOG_INFO("[native] C2: depth format {}{}{}{}",
                  format_depth_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
                  (missing & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? ", no sampling" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? ", not a copy source" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? ", not a copy destination" : "");
      if (!depth_scalable_) {
        REXLOG_INFO("[native] C2: the driver does not scale depth: the shadow map stays at its size");
      }
    }
    // One pool per command buffer: the SDK table does not include
    // vkResetCommandBuffer either, so the whole pool is reset.
    VkCommandPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info_pool.queueFamilyIndex = family_;
    VkCommandBufferAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    reserve.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    reserve.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{};
    info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (SlotWork& slot : slots_) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &slot.pool_work) !=
              VK_SUCCESS ||
          dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &slot.pool_upload) !=
              VK_SUCCESS ||
          dfn_.vkCreateFence(device_, &info_fence, nullptr, &slot.fence) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = slot.pool_work;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &slot.work) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = slot.pool_upload;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &slot.upload) != VK_SUCCESS) {
        return false;
      }
    }
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &pools_output_[i]) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = pools_output_[i];
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &commands_output_[i]) != VK_SUCCESS) {
        return false;
      }
      if (dfn_.vkCreateFence(device_, &info_fence, nullptr, &fences_output_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    slots_used_ = uint32_t(std::clamp(REXCVAR_GET(fh1_native_work_slots), 2,
                                          int32_t(slots_.size())));
    REXLOG_INFO("[native] C2: work slots (fh1_native_work_slots) = {} of {}", slots_used_,
                slots_.size());
    output_without_wait_ = REXCVAR_GET(fh1_native_output_without_wait);
    invalidate_every_copy_ = REXCVAR_GET(fh1_native_invalidate_textures_every_copy);
    REXLOG_INFO("[native] C2: texture caches dropped on every copy (fh1_native_invalidate_textures_every_copy) = "
                "{}",
                invalidate_every_copy_ ? "SI" : "no");
    REXLOG_INFO("[native] C2: output without waiting for the previous one (fh1_native_output_without_wait) = {}",
                output_without_wait_ ? "SI" : "no");
    marks_precise_ = REXCVAR_GET(fh1_native_precise_marks);
    toggle_marks_s_ = REXCVAR_GET(fh1_native_precise_marks_toggle_s);
    start_marks_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C2: precise GPU timestamps (fh1_native_precise_marks) = {}; toggle every {} s",
                marks_precise_ ? "SI" : "no", toggle_marks_s_);
    // GPU time per work unit: two timestamps per slot. Without timestamp bits on the queue, it is not measured.
    {
      const auto& ifn = vulkan_device_->vulkan_instance()->functions();
      uint32_t families = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families,
                                                   nullptr);
      std::vector<VkQueueFamilyProperties> queues(families);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families,
                                                   queues.data());
      VkPhysicalDeviceProperties physical_2{};
      ifn.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(), &physical_2);
      write_mark_ = reinterpret_cast<FnWriteMark>(
          ifn.vkGetDeviceProcAddr(device_, "vkCmdWriteTimestamp"));
      read_queries_ = reinterpret_cast<FnReadQueries>(
          ifn.vkGetDeviceProcAddr(device_, "vkGetQueryPoolResults"));
      if (family_ < families && queues[family_].timestampValidBits && write_mark_ &&
          read_queries_ && physical_2.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo info_queries{};
        info_queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info_queries.queryCount = uint32_t(slots_.size() * kMarksBySlot);
        if (dfn_.vkCreateQueryPool(device_, &info_queries, nullptr, &queries_) != VK_SUCCESS) {
          queries_ = VK_NULL_HANDLE;
        }
        period_mark_ns_ = physical_2.limits.timestampPeriod;
      }
      REXLOG_INFO("[native] C2: GPU time per Swap {}",
                  queries_ != VK_NULL_HANDLE ? "measured with timestamps"
                                               : "not available (the queue has no timestamps)");
      // The game's occlusion queries (the sun flare), counted on the GPU.
      VkQueryPoolCreateInfo info_occlusions{};
      info_occlusions.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      info_occlusions.queryType = VK_QUERY_TYPE_OCCLUSION;
      info_occlusions.queryCount = uint32_t(slots_.size() * kOcclusionsBySlot);
      if (!read_queries_ ||
          dfn_.vkCreateQueryPool(device_, &info_occlusions, nullptr, &occlusions_) != VK_SUCCESS) {
        occlusions_ = VK_NULL_HANDLE;
      }
      occlusion_precise_ = vulkan_device_->properties().occlusionQueryPrecise;
      REXLOG_INFO("[native] C2: host occlusion queries {} ({})",
                  occlusions_ != VK_NULL_HANDLE ? "available" : "not available: faked count",
                  occlusion_precise_ ? "precise" : "not precise: they count whether there was any sample");
      // fh1_reflection_visibility, in a separate pool so as not to touch the count of the game's queries.
      if (fh1::reflection_demand::MeasureVisibility() && read_queries_) {
        VkQueryPoolCreateInfo info_visibility{};
        info_visibility.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_visibility.queryType = VK_QUERY_TYPE_OCCLUSION;
        info_visibility.queryCount = uint32_t(slots_.size() * kVisibilityBySlot);
        if (dfn_.vkCreateQueryPool(device_, &info_visibility, nullptr, &visibility_) != VK_SUCCESS) {
          visibility_ = VK_NULL_HANDLE;
        }
        REXLOG_INFO("[native] C2: reflection visibility (build 192): {}",
                    visibility_ != VK_NULL_HANDLE ? "queries available"
                                                   : "NO queries: the reflection is decided by reads (as in 191)");
      }
      // Pipeline statistics per pass. The order of the counters is the order of the bits,
      // not the order of this list: vertices, clipped primitives and fragments.
      if (vulkan_device_->properties().pipelineStatisticsQuery && read_queries_) {
        VkQueryPoolCreateInfo info_statistics{};
        info_statistics.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_statistics.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_statistics.queryCount = uint32_t(slots_.size() * kStatisticsBySlot);
        info_statistics.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_statistics, nullptr, &statistics_) != VK_SUCCESS) {
          statistics_ = VK_NULL_HANDLE;
        }
      }
      if (statistics_ != VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info_draw{};
        info_draw.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_draw.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_draw.queryCount = uint32_t(slots_.size() * kStatisticsDrawBySlot);
        info_draw.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_draw, nullptr, &statistics_draw_) != VK_SUCCESS) {
          statistics_draw_ = VK_NULL_HANDLE;
        }
        fragments_by_shader_.assign(size_t(kLabelsShader) * kGpuCategories, 0);
        draws_by_shader_.assign(size_t(kLabelsShader) * kGpuCategories, 0);
      }
      REXLOG_INFO("[native] C2: pipeline statistics per pass {}",
                  statistics_ != VK_NULL_HANDLE ? "available (fh1_native_pipeline_statistics)"
                                                  : "not available");
    }

    VkSamplerCreateInfo info_sampler{};
    info_sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info_sampler.magFilter = VK_FILTER_LINEAR;
    info_sampler.minFilter = VK_FILTER_LINEAR;
    info_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn_.vkCreateSampler(device_, &info_sampler, nullptr, &sampler_) != VK_SUCCESS) {
      return false;
    }

    // Same layout as the presenter: image at 0 and sampler at 1, and the gamma ramp at 2 (the pipeline without
    // the ramp does not use it).
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = &sampler_;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_layout.bindingCount = 3;
    info_layout.pBindings = bindings;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_layout, nullptr, &layout_descriptores_) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange ranges[2]{};
    ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ranges[0].offset = 0;
    ranges[0].size = 16;  // GuestOutputPaintRectangleConstants
    ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ranges[1].offset = 16;
    ranges[1].size = 16;  // Presenter::BilinearConstants
    VkPipelineLayoutCreateInfo info_pipeline_layout{};
    info_pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info_pipeline_layout.setLayoutCount = 1;
    info_pipeline_layout.pSetLayouts = &layout_descriptores_;
    info_pipeline_layout.pushConstantRangeCount = 2;
    info_pipeline_layout.pPushConstantRanges = ranges;
    if (dfn_.vkCreatePipelineLayout(device_, &info_pipeline_layout, nullptr, &layout_pipeline_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlotsOutput},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, kSlotsOutput},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlotsOutput}};
    VkDescriptorPoolCreateInfo info_pool_desc{};
    info_pool_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_desc.maxSets = kSlotsOutput;
    info_pool_desc.poolSizeCount = 3;
    info_pool_desc.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_desc, nullptr, &pool_descriptores_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve_desc{};
    reserve_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_desc.descriptorPool = pool_descriptores_;
    reserve_desc.descriptorSetCount = 1;
    reserve_desc.pSetLayouts = &layout_descriptores_;
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (dfn_.vkAllocateDescriptorSets(device_, &reserve_desc, &descriptores_output_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    // One ramp buffer per output slot, so it is only changed in a slot the GPU is no longer using. Without
    // them, the output has no ramp (as before).
    ramp_gamma_ = REXCVAR_GET(fh1_native_gamma_ramp);
    for (uint32_t i = 0; i < kSlotsOutput && ramp_gamma_; ++i) {
      RampOutput& ramp = ramps_output_[i];
      uint32_t type = 0;
      void* mapped = nullptr;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, sizeof(ramp_values_), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp.buffer, ramp.memory_block, &type) ||
          dfn_.vkMapMemory(device_, ramp.memory_block, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C2: could not create the gamma ramp buffer: the output goes without it");
        ramp_gamma_ = false;
        break;
      }
      ramp.data = static_cast<uint8_t*>(mapped);
      ramp.type = type;
      VkDescriptorBufferInfo info_buffer{ramp.buffer, 0, sizeof(ramp_values_)};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptores_output_[i];
      write.dstBinding = 2;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write.pBufferInfo = &info_buffer;
      dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    if (!ramp_gamma_) {
      // Without a ramp, binding 2 stays unwritten: the pipeline without the ramp does not read it.
      for (RampOutput& ramp : ramps_output_) {
        if (ramp.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buffer, nullptr);
        if (ramp.memory_block != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.memory_block, nullptr);
        ramp = RampOutput{};
      }
    }

    VkShaderModuleCreateInfo info_module{};
    info_module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info_module.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
    info_module.pCode = shaders::guest_output_triangle_strip_rect_vs;
    if (dfn_.vkCreateShaderModule(device_, &info_module, nullptr, &vs_) != VK_SUCCESS) {
      return false;
    }
    info_module.codeSize = sizeof(shaders::guest_output_bilinear_ps);
    info_module.pCode = shaders::guest_output_bilinear_ps;
    if (dfn_.vkCreateShaderModule(device_, &info_module, nullptr, &fs_) != VK_SUCCESS) {
      return false;
    }
    if (ramp_gamma_) {
      info_module.codeSize = sizeof(shaders::fh1_output_gamma_ramp_ps);
      info_module.pCode = shaders::fh1_output_gamma_ramp_ps;
      if (dfn_.vkCreateShaderModule(device_, &info_module, nullptr, &fs_ramp_) != VK_SUCCESS) {
        return false;
      }
    }

    VkAttachmentDescription attachment{};
    attachment.format = VulkanPresenter::kGuestOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info_rp{};
    info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info_rp.attachmentCount = 1;
    info_rp.pAttachments = &attachment;
    info_rp.subpassCount = 1;
    info_rp.pSubpasses = &subpass;
    if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &render_pass_output_) != VK_SUCCESS) {
      return false;
    }

    pipeline_ = CreatePipelineOutput(fs_, nullptr);
    if (pipeline_ == VK_NULL_HANDLE) {
      return false;
    }
    // The ramp variants without extras (bilinear and exact texel) are created now; the post-processing ones
    // with per-pixel work and FXAA, the first time they are requested (PipelineRamp).
    if (ramp_gamma_ && (PipelineRamp(0) == VK_NULL_HANDLE || PipelineRamp(1) == VK_NULL_HANDLE)) {
      return false;
    }
    REXLOG_INFO("[native] C2: game gamma ramp on the output (fh1_native_gamma_ramp) = {}",
                ramp_gamma_ ? "SI" : "no");
    // Parts C3-C6: without the required capabilities only copies and presentation remain.
    draws_ = DrawsVulkan::Create(vulkan_device_, memory_, this);
    return true;
  }

  // Pipeline of the output pass with the fragment shader fs (and its specialization constants, if any).
  VkPipeline CreatePipelineOutput(VkShaderModule fs, const VkSpecializationInfo* special) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = special;
    VkPipelineVertexInputStateCreateInfo entry{};
    entry.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo sampling{};
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_2[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_2;
    VkGraphicsPipelineCreateInfo info_pipeline{};
    info_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info_pipeline.stageCount = 2;
    info_pipeline.pStages = stages;
    info_pipeline.pVertexInputState = &entry;
    info_pipeline.pInputAssemblyState = &assembly;
    info_pipeline.pViewportState = &viewport_state;
    info_pipeline.pRasterizationState = &rasterization;
    info_pipeline.pMultisampleState = &sampling;
    info_pipeline.pColorBlendState = &blend;
    info_pipeline.pDynamicState = &dynamic;
    info_pipeline.layout = layout_pipeline_;
    info_pipeline.renderPass = render_pass_output_;
    info_pipeline.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info_pipeline, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pipeline;
  }

  // Variant of the output pass with the ramp, by index: 4 * fxaa + 2 * grading + exact (specialization
  // constants 2, 1 and 0 of the shader). Created the first time it is requested; VK_NULL_HANDLE if it fails
  // (and it is logged).
  VkPipeline PipelineRamp(uint32_t index) {
    VkPipeline& pipeline = pipelines_ramp_[index];
    if (pipeline == VK_NULL_HANDLE && !pipelines_ramp_failed_[index]) {
      const VkSpecializationMapEntry entries[3] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                    {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
      const VkBool32 values[3] = {(index & 1) ? VK_TRUE : VK_FALSE, (index & 2) ? VK_TRUE : VK_FALSE,
                                   (index & 4) ? VK_TRUE : VK_FALSE};
      const VkSpecializationInfo special{3, entries, sizeof(values), values};
      const auto before = std::chrono::steady_clock::now();
      pipeline = CreatePipelineOutput(fs_ramp_, &special);
      if (pipeline == VK_NULL_HANDLE) {
        pipelines_ramp_failed_[index] = true;
        REXLOG_ERROR("[native] C2: could not create variant {} of the output pass", index);
      } else {
        REXLOG_INFO("[native] C2: variant {} of the output pass (exact {}, grading {}, FXAA {}) created in {:.1f} "
                    "ms",
                    index, (index & 1) ? "yes" : "no", (index & 2) ? "yes" : "no", (index & 4) ? "yes" : "no",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
      }
    }
    return pipeline;
  }

  bool Copy(const RegistersCopy& reg) override {
    // Draws since the previous copy: those of the render target being resolved now (rear-view mirror
    // diagnostic).
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    const uint64_t draws_before = drawn - drawn_last_copy_;
    drawn_last_copy_ = drawn;
    if (draws_) {
      draws_->FinishPass();  // copying and clearing are not allowed inside a pass
      // No longer on every copy; see GetResolved and Prepare.
      if (invalidate_every_copy_) {
        draws_->InvalidateTextures();
      }
    }
    if (Record()) {
      MarkGpu(kGpuCopies);  // GPU time of copies and clears (C2 report)
    }
    const uint32_t control = reg.rb_copy_control;
    const uint32_t source = control & 0x7;
    const bool clear_color = (control >> 8) & 0x1;
    const uint32_t command = (control >> 20) & 0x3;
    const bool copy_2 = command == uint32_t(xenos::CopyCommand::kRaw) ||
                        command == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch_guest = reg.rb_surface_info & 0x3FFF;
    const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
    if (msaa != uint32_t(xenos::MsaaSamples::k1X) && warned_.insert(1).second) {
      REXLOG_INFO("[native] C2: target with MSAA: used with 1 sample");
    }
    // FH1 (fh1_native_ssaa): the scene's 4x passes are drawn into the image of twice the pitch at twice the size
    // (DrawsVulkan::BeginPass). The resolve shrinks it: a linear blit of 2 to 1 is the average of the four samples.
    const bool ssaa = Ssaa(msaa, pitch_guest);
    const int32_t m = ssaa ? 2 : 1;
    const uint32_t pitch = pitch_guest * uint32_t(m);
    if (source >= xenos::kMaxColorRenderTargets) {
      // From depth: the copy goes to a resolved texture with the host depth
      // format, which the draws sample as k_24_8. With a depth source no color
      // is cleared (IsClearingColor, graphics/util/draw.h:534-538).
      const bool clears_2 = ((control >> 9) & 0x1) && clear_depth_;
      if (copy_2) {
        CopyDepth(reg, pitch_guest, clears_2, ssaa);
      }
      if (clears_2) {
        ClearDepth(reg, pitch);
      }
      return true;
    }
    const uint32_t info_color = reg.rb_color_info[source];
    const uint32_t format_color = (info_color >> 16) & 0xF;
    if (!FormatColorAccepted(format_color)) {
      return Reject(100 + format_color, "render target format not supported yet");
    }
    int32_t x0, y0, x1, y1;
    if (!Rectangle(reg, pitch_guest, x0, y0, x1, y1)) {
      return false;
    }

    // First the objects (creating or recreating can submit work), then recording.
    Image* target_render = GetTarget(info_color & 0xFFF, format_color, pitch);
    if (!target_render) {
      return false;
    }
    // The game has just said which area of this render target matters to it. It is the data RestoreContent
    // uses to stop copying the bottom rows that nobody draws or reads (the scene render target is created
    // 1280x1280 to draw 1280x720).
    NoteAreaUseful(*target_render, y1 * m);
    NoteUseClear(*target_render, uint32_t(std::max(x1, 0)) * uint32_t(m), uint32_t(std::max(y1, 0)) * uint32_t(m));
    uint32_t base_resolved = 0;
    Resolved* resolved = nullptr;
    uint32_t dx = 0, dy = 0;
    if (copy_2) {
      const uint32_t info_target = reg.rb_copy_dest_info;
      const uint32_t format_target = (info_target >> 7) & 0x3F;
      {  // NFSC diagnostic: how the game resolves its HDR scene (copy_dest_number bits 13-15, exp_bias bits 16-21)
        static uint32_t seen = 0;
        if ((format_color > 1 || (presented_ >= 240 && presented_ < 244)) && (seen < 14 || ((presented_ >= 3500 && presented_ < 3503) || (presented_ >= 240 && presented_ < 244)))) {
          ++seen;
          REXLOG_INFO("[fh1] resolve rect x0={} y0={} x1={} y1={} dest_base={:08X} dest_pitch={} dest_h={} window_offset={:08X} frame={}",
                      x0, y0, x1, y1, reg.rb_copy_dest_base, reg.rb_copy_dest_pitch & 0x3FFF,
                      (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, 0u, presented_);
          REXLOG_INFO("[fh1] resolve: render target format {} -> copy dest format {} number {} exp_bias {} swap {} (info {:08X}) "
                      "control {:08X}",
                      format_color, format_target, (info_target >> 13) & 0x7, int32_t((info_target >> 16) << 26) >> 26,
                      (info_target >> 24) & 1, info_target, control);
        }
      }
      if ((info_target >> 3) & 0x1) {
        Reject(3, "copy to a 3D texture or array: not yet");
      } else if (format_target != uint32_t(xenos::ColorFormat::k_8_8_8_8) &&
                 // FH1: the design creator resolves an 8-bit-per-pixel picture (k_8) from an 8_8_8_8 target: the
                 // console keeps the red channel. Kept here as the whole 8_8_8_8 picture; a k_8 fetch reads red.
                 format_target != uint32_t(xenos::ColorFormat::k_8) &&
                 format_target != uint32_t(xenos::ColorFormat::k_8_8_8_8_A) &&
                 format_target != uint32_t(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16) &&
                 // FH1: its final image is resolved from the 2_10_10_10 render target as k_2_10_10_10 (same 32 bits per
                 // texel); stored in the 8-bit resolved texture like the other 32-bit formats for now.
                 format_target != uint32_t(xenos::ColorFormat::k_2_10_10_10) &&
                 // FH1: the post-processing chain (exposure, bloom) resolves 16- and 32-bit-per-channel targets.
                 FormatCopyFh1(format_target) == VK_FORMAT_UNDEFINED) {
        Reject(200 + format_target, "copy format not supported yet");
      } else {
        const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
        const uint32_t height_target = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
        // 4 bytes per texel: base in multiples of 32 texels (GetResolveInfo).
        const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
        const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
        // FH1: 64-bit formats have 8 bytes per texel (log2 3) for the tiled offset.
        // FH1: the HDR scene (float target) is resolved as k_2_10_10_10 with a negative exp_bias (1/4, 1/16) so that
        // values above 1 survive. An 8-bit resolved texture cut them at 1 (no bloom, flat reflections); the resolved
        // texture is a float image like its source and the bias is applied by the fetch (ImageNative::exp_bias).
        const bool resolved_hdr = REXCVAR_GET(fh1_native_resolved_hdr) &&
                                  target_render->format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                                  FormatCopyFh1(format_target) == VK_FORMAT_UNDEFINED;
        const VkFormat format_copy_fh1 = resolved_hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : FormatCopyFh1(format_target);
        const bool target_8 = format_target == uint32_t(xenos::ColorFormat::k_8);
        const uint32_t log2_bytes = target_8 ? 0 : BytesTexelLog2Fh1(FormatCopyFh1(format_target));
        const uint32_t base =
            reg.rb_copy_dest_base +
            uint32_t(OffsetTile2D(int32_t(base_x), int32_t(base_y), pitch_target, log2_bytes));
        dx = uint32_t(x0) - base_x;
        dy = uint32_t(y0) - base_y;
        // NFSC: Carbon renders its 64-bit HDR scene in two tiles and resolves each one to its own address, which is
        // an exact number of 32-row stripes into the texture of the first tile. If this destination lies inside a
        // resolved texture that already exists, copy into that texture at the matching row.
        // FH1: the design creator paints the car's 2048x2048 livery one side of the car after another and resolves
        // each to its rectangle of that texture: the destination address is a tile in the middle of it, at any
        // column. Taken as textures of their own, the pieces were read by nothing and the livery kept its clear
        // color (a flat cyan car).
        uint32_t row_extra = 0, base_container = 0, column_extra = 0;
        Resolved* container =
            target_8 ? nullptr
                     : FindResolvedContainer(base & 0x1FFFFFFF, pitch_target, row_extra, base_container,
                                             resolved_hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : kFormatColor, 1,
                                             resolved_hdr ? nullptr : &column_extra, dx + uint32_t(x1 - x0),
                                             dy + uint32_t(y1 - y0));
        if (container) {
          resolved = container;
          dy += row_extra;
          dx += column_extra;
          if (column_extra && pieces_logged_ < 8) {
            ++pieces_logged_;
            REXLOG_INFO("[fh1] resolve of a piece: {}x{} goes to ({},{}) of the {}x{} texture at {:08X}", x1 - x0,
                        y1 - y0, dx, dy, container->image.width, container->image.height, base_container);
          }
          base_resolved = base_container;
          if (!tile_registered_) {
            tile_registered_ = true;
            REXLOG_INFO("[native] C2: tiled resolve: the copy to {:08X} goes inside the resolved texture of "
                        "{:08X}, from row {}",
                        base & 0x1FFFFFFF, base_container, row_extra);
          }
        } else {
          uint32_t width_target = pitch_target;
          if (!width_hint_.empty()) {
            if (const auto hint = width_hint_.find((uint64_t(pitch_target) << 32) | (base & 0x1FFFFFFF));
                hint != width_hint_.end()) {
              width_target = hint->second;
            }
          }
          resolved = GetResolved(base & 0x1FFFFFFF, width_target, height_target, format_target,
                                     (info_target >> 24) & 0x1,
                                     format_copy_fh1 != VK_FORMAT_UNDEFINED ? format_copy_fh1 : kFormatColor);
          base_resolved = base & 0x1FFFFFFF;
        }
        if (resolved) {
          if (const int32_t exp_bias = int32_t((info_target >> 16) << 26) >> 26; resolved->image.exp_bias != exp_bias) {
            resolved->image.exp_bias = exp_bias;
            if (draws_) draws_->InvalidateTextures();
          }
          if (base_resolved && resolved->image.width >= 1280 && resolved->image.height >= 720) {
            last_resolved_screen_ = base_resolved;  // FH1: see Present (Swap of a front buffer nobody resolved)
          }
          NoteCopy(base & 0x1FFFFFFF, pitch_target, height_target, draws_before);
          if (composite_there_is_ || composite_stale_ != 0) {  // fh1_native_lazy_composite
            CompositeBeforeOfWriteTexture(base & 0x1FFFFFFF, x0 == 0 && y0 == 0 &&
                                                                   uint32_t(x1 - x0) >= resolved->image.width &&
                                                                   uint32_t(y1 - y0) >= resolved->image.height);
          }
          if (diag_window_ || !diag_watched_.empty()) {  // fh1_native_diag_readers_s
            DiagWrite(base & 0x1FFFFFFF, info_color & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                          resolved->image.width, resolved->image.height, draws_before, false);
          }
        }
        if (resolved && REXCVAR_GET(fh1_native_diag_resolved) &&
            resolved_frame_.size() < 64 &&
            std::find(resolved_frame_.begin(), resolved_frame_.end(),
                      base & 0x1FFFFFFF) == resolved_frame_.end()) {
          resolved_frame_.push_back(base & 0x1FFFFFFF);
        }
      }
    }

    if (!Record()) {
      return false;
    }
    Prepare(*target_render);
    // FH1: a fill through the depth buffer that no draw followed is still owed to this target, and the resolve
    // reads it: the design creator fills a livery pass's target, draws nothing into it when the car has no decal
    // there, and resolves it (the fill never arrived and the car showed the color of an earlier clear, cyan).
    if (!fills_depth_.empty()) {
      ApplyFillDepth(*target_render, info_color & 0xFFF, pitch);
    }
    if (resolved) {
      Prepare(resolved->image);
      // What the game asks for and what fits in the render target.
      const uint32_t requested_width =
          std::min(uint32_t(x1 - x0), target_render->width / uint32_t(m) - uint32_t(x0));
      const uint32_t requested_height =
          std::min(uint32_t(y1 - y0), target_render->height / uint32_t(m) - uint32_t(y0));
      const uint32_t fits_width = resolved->image.width > dx ? resolved->image.width - dx : 0;
      const uint32_t fits_height = resolved->image.height > dy ? resolved->image.height - dy : 0;
      const uint32_t width = std::min(requested_width, fits_width);
      const uint32_t height = std::min(requested_height, fits_height);
      // With the scene at a higher resolution than the render target (fh1_internal_resolution = 1920x1080
      // and a 1280x720 front buffer), copying 1 to 1 takes only a piece: the image comes out cropped, with the
      // car and the HUD out of frame. When it does not fit, it is shrunk with a linear blit, which is exactly
      // the scaling wanted. If it fits, it is copied as always, bit for bit.
      // It only shrinks when the scene really has to be reduced (by a factor of 1.25 or more) and both images
      // are color: vkCmdBlitImage with a linear filter on a depth target is not valid and brings the process
      // down. Mismatches of a few pixels (320x184 -> 320x180) are still cropped as always.
      const bool shrink = blit_ != nullptr && fits_width && fits_height &&
                           target_render->format == kFormatColor &&
                           resolved->image.format == kFormatColor &&
                           (requested_width * 4 >= fits_width * 5 || requested_height * 4 >= fits_height * 5);
      if (ssaa) {
        if (width && height && dx < resolved->image.width && dy < resolved->image.height) {
          ResolveFrontPrevious(base_resolved, dx == 0 && dy == 0 && width == resolved->image.width &&
                                                  height == resolved->image.height);
          VkImageBlit average{};
          average.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          average.srcOffsets[0] = {x0 * 2, y0 * 2, 0};
          average.srcOffsets[1] = {(x0 + int32_t(width)) * 2, (y0 + int32_t(height)) * 2, 1};
          average.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          average.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
          average.dstOffsets[1] = {int32_t(dx + width), int32_t(dy + height), 1};
          BarrierGlobal(commands_work_);
          blit_(commands_work_, target_render->image, VK_IMAGE_LAYOUT_GENERAL, resolved->image.image,
                VK_IMAGE_LAYOUT_GENERAL, 1, &average, VK_FILTER_LINEAR);
          ++copies_;
          if (ssaa_resolves_++ == 0) {
            REXLOG_INFO("[fh1] supersampled scene: {}x{} of the {}x{} target averaged into {}x{} at ({},{}) of the "
                        "resolved texture",
                        width * 2, height * 2, target_render->width, target_render->height, width, height, dx, dy);
          }
          NoteCopy(width, height);
          ResolvedWritten(base_resolved, uint64_t(width) * height * 4);
          ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
        }
      } else if (shrink && uint32_t(x0) < target_render->width && uint32_t(y0) < target_render->height) {
        // fh1_native_lazy_front. If this texture had a deferred copy, it is dropped if this resolve
        // covers it entirely, and recorded first otherwise.
        ResolveFrontPrevious(base_resolved, dx == 0 && dy == 0 && fits_width == resolved->image.width &&
                                                   fits_height == resolved->image.height);
        VkImageBlit reduction{};
        reduction.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.srcOffsets[0] = {x0, y0, 0};
        reduction.srcOffsets[1] = {x0 + int32_t(requested_width), y0 + int32_t(requested_height), 1};
        reduction.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        reduction.dstOffsets[1] = {int32_t(dx + fits_width), int32_t(dy + fits_height), 1};
        BarrierGlobal(commands_work_);
        blit_(commands_work_, target_render->image, VK_IMAGE_LAYOUT_GENERAL,
              resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &reduction, VK_FILTER_LINEAR);
        ++copies_;
        ++reductions_;
        if (reductions_ <= 4) {
          REXLOG_INFO("[resolution] the {}x{} scene is shrunk to {}x{} when resolved (supersampling)",
                      requested_width, requested_height, fits_width, fits_height);
        }
        NoteCopy(fits_width, fits_height);
        // A linear-filter blit reads the large rectangle and writes the small one, so it costs more than a copy
        // of the output size. The pixels read are recorded, since they are what dominates.
        ResolvedWritten(base_resolved, uint64_t(requested_width) * requested_height);
        ReadResolved(reg, *resolved, x0, y0, dx, dy, fits_width, fits_height);
      } else if (width && height && uint32_t(x0) < target_render->width &&
                 uint32_t(y0) < target_render->height && dx < resolved->image.width &&
                 dy < resolved->image.height) {
        /*
         * The same as is done with depth. If the whole render target is resolved to a texture of the same size,
         * the two images are swapped and not a single pixel is copied. That is 4 copies of 1280x720 per frame,
         * 78 % of the traffic.
         */
        const bool whole_color = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                                  width == target_render->width && height == target_render->height;
        /*
         * For color the clear is a requirement, unlike for depth. Tested without it: 16,807 color swaps and
         * 17,350 restores, almost one for one; the game draws on top again without clearing and the copy is
         * paid anyway, only later and with two operations instead of one.
         */
        if (whole_color && clear_color && ResolveWithoutCopy() &&
            REXCVAR_GET(fh1_native_swap_color)) {
          // With x0 = y0 = 0 the tiling offset is 0, so the address is the base as is.
          if (SwapWithResolved(*target_render, *resolved, reg.rb_copy_dest_base & 0x1FFFFFFF)) {
            ++swaps_color_;
            ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
            return true;
          }
        }
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = {x0, y0, 0};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
        copy.extent = {width, height, 1};
        // fh1_native_lazy_front. The previous copy to this texture, if still deferred, is dropped (this
        // one covers it entirely) or recorded first; and if this texture is a front buffer that only the Swap
        // reads, it is deferred.
        ResolveFrontPrevious(base_resolved,
                                dx == 0 && dy == 0 && width == resolved->image.width && height == resolved->image.height);
        if (!DeferCopyFront(base_resolved, *target_render, *resolved, copy) &&
            !DeferComposite(base_resolved, *target_render, *resolved, copy,
                              (info_color & 0xFFF) | (pitch << 12))) {
          BarrierGlobal(commands_work_);
          CopyImages(commands_work_, target_render->image, VK_IMAGE_LAYOUT_GENERAL,
                         resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          ++copies_;
          NoteCopy(width, height);
          ResolvedWritten(base_resolved, uint64_t(width) * height);
          ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
        }
      }
    }
    if (dump_rt_pending_ && resolved && resolved->image.width == 1280 && resolved->image.height == 720 &&
        dump_res_done_ < 6) {
      if (draws_) {
        draws_->FinishPass();
      }
      char name[96];
      std::snprintf(name, sizeof(name), "resolved_after_copy_%u_dest%08X.png", dump_res_done_++,
                    reg.rb_copy_dest_base);
      DumpImage(resolved->image, name);
    }
    // FH1: and, for the whole frame after the dump, every 8-bit color image of 300 pixels or wider right after its
    // copy (frame_<order>_dest<address>_<size>.png): the post-processing chain step by step.
    if (dump_frame_pending_ && resolved && DumpBytesTexel(resolved->image.format) &&
        (resolved->image.width >= 300 || resolved->image.format != kFormatColor) && dump_frame_done_ < 160) {
      if (draws_) {
        draws_->FinishPass();
      }
      char name[96];
      std::snprintf(name, sizeof(name), "frame_%02u_dest%08X_%ux%u.png", dump_frame_done_++, reg.rb_copy_dest_base,
                    resolved->image.width, resolved->image.height);
      DumpImage(resolved->image, name);
    }
    if (dump_rt_pending_ && clear_color && dump_rt_done_ < 4 && target_render && target_render->width >= 1280) {
      if (draws_) {
        draws_->FinishPass();
      }
      char name[64];
      std::snprintf(name, sizeof(name), "rt_before_clear_%u_%ux%u.png", dump_rt_done_++, target_render->width,
                    target_render->height);
      DumpImage(*target_render, name);
      if (dump_rt_done_ >= 4) dump_rt_pending_ = false;
    }
    if (clear_color) {
      const uint64_t value_raw =
          uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
      /*
       * Two things that were missing here.
       *  1. The clear leaves the whole render target with a known color, so if its content had gone away
       *     in a swap it no longer has to be brought back. Depth already did this (ClearDepth);
       *     color did not, and that is why, when fh1_native_swap_color was turned on, there were
       *     17,350 restores against 16,807 swaps: almost all of them restored only to clear right after.
       *  2. If the render target is already cleared to that same color and nothing has been drawn since
       *     then, the vkCmdClearColorImage does not change a single bit and is skipped.
       */
      if (composite_there_is_ && composite_.source == ((info_color & 0xFFF) | (pitch << 12))) {
        CompositeBeforeOfWriteSource();  // fh1_native_lazy_composite (its source is cleared)
      }
      if (!diag_watched_.empty()) {  // fh1_native_diag_readers_s (a source is cleared)
        DiagSourceWritten((info_color & 0xFFF) | (pitch << 12));
      }
      target_render->content_invalid = false;
      fills_depth_.erase((uint64_t(info_color & 0xFFF) << 20) | pitch);  // the clear replaces a pending depth fill
      const uint64_t value_state =
          REXCVAR_GET(fh1_native_diag_clear) ? ~uint64_t(0) : value_raw;
      if (ClearRedundant(*target_render, value_state)) {
        ++clears_skipped_;
      } else {
        // fh1_native_lazy_front. If this render target is the source of a deferred front buffer, the
        // clear goes to a spare image and the one with the content is kept for the front buffer (no copy).
        RotateFrontBeforeOfClear(*target_render);
        MarkGpu(kGpuClears);  // clears, separate from copies (C2 report)
        VkClearColorValue color{};
        if (REXCVAR_GET(fh1_native_diag_clear)) {
          color = ColorDiagnostic(info_color & 0xFFF, format_color, pitch);
        } else if (format_color == 2 || format_color == 3 || format_color == 10 || format_color == 12) {
          // FH1: the clear value is packed like the target: three 10-bit fields and 2 bits of alpha. For the float
          // formats (3 and 12) each field is the console's 7e3 float (7 bits of mantissa, 3 of exponent, largest
          // value 31.875). Read as four bytes, the reflection cube map's 00701003 (nearly black) became a bright
          // blue (0.01 0.06 0.44) that showed wherever the cube's scene has a gap, and the cars mirrored it.
          const bool is_float = format_color == 3 || format_color == 12;
          for (uint32_t j = 0; j < 3; ++j) {
            const uint32_t field = uint32_t(value_raw >> (j * 10)) & 0x3FF;
            const uint32_t exponent = field >> 7;
            const float mantissa = float(field & 0x7F) * (1.0f / 128.0f);
            color.float32[j] = !is_float   ? float(field) * (1.0f / 1023.0f)
                               : exponent ? std::ldexp(1.0f + mantissa, int(exponent) - 3)
                                          : mantissa * 0.25f;
          }
          color.float32[3] = float((value_raw >> 30) & 0x3) * (1.0f / 3.0f);
        } else {
          for (uint32_t j = 0; j < 4; ++j) {
            color.float32[j] = float((value_raw >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
          }
        }
        // fh1_native_clear_useful_area. Only the rows in use; the bottom band, if needed.
        if (!ClearColorAreaUseful(*target_render, color)) {
          BarrierGlobal(commands_work_);
          dfn_.vkCmdClearColorImage(commands_work_, target_render->image,
                                    VK_IMAGE_LAYOUT_GENERAL, &color, 1, &kRangeColor);
          RemoveBand(*target_render);
        }
        ++clears_;
        NoteClearDiag(*target_render, info_color & 0xFFF, format_color, pitch, false, false);
      }
    }
    if (((control >> 9) & 0x1) && clear_depth_) {
      ClearDepth(reg, pitch);
    }
    return true;
  }

  void ClearDepth(const RegistersCopy& reg, uint32_t pitch) {
    const uint32_t info = reg.rb_depth_info;
    Image* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth || !Record()) {
      return;
    }
    Prepare(*depth);
    BeforeOfWriteDepth(*depth);  // fh1_native_lazy_depth
    ShadowMinimumBeforeOfClear(*depth);   // fh1_native_shadow_minimum
    depth->content_invalid = false;  // The clear gives it contents
    const VkClearDepthStencilValue input_value{float(reg.rb_depth_clear >> 8) / 16777215.0f,
                                         reg.rb_depth_clear & 0xFF};
    // If it is already cleared with this same value and nobody has drawn anything since then, the clear does
    // not change a single bit. Mind the path below: the per-pass clear also resets the ZCULL plane, so that
    // one is never skipped (only the vkCmdClearDepthStencilImage one).
    if (depth->accepts_target_of_copy &&
        ClearRedundant(*depth, uint64_t(reg.rb_depth_clear))) {
      ++clears_skipped_depth_;
      return;
    }
    MarkGpu(kGpuClears);  // clears, separate from copies (C2 report)
    ++clears_depth_;
    // fh1_native_diag_clears. Without TRANSFER_DST the clear is done by opening a pass (ZCULL).
    NoteClearDiag(*depth, info & 0xFFF, (info >> 16) & 0x1, pitch, true,
                      !depth->accepts_target_of_copy);
    // ZCULL: see Prepare. Without TRANSFER_DST the clear has to be done by opening a pass.
    if (!depth->accepts_target_of_copy && draws_) {
      // This bool cannot be dropped. If the clear pass cannot be opened, the depth keeps the previous frame's
      // content and the ZCULL hi-Z is not reset either (only a loadOp = CLEAR resets it), so the whole frame
      // culls against stale data. Without TRANSFER_DST there is no alternative path, so at least it is
      // counted and reported.
      if (!draws_->ClearDepthInPass(commands_work_, *depth, input_value.depth,
                                             input_value.stencil)) {
        if (++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT clear the {}x{} depth by opening a pass; it keeps the previous "
                      "content (failure {})",
                      depth->width, depth->height, clears_in_pass_failed_);
        }
      }
      return;
    }
    BarrierGlobal(commands_work_);
    clear_depth_(commands_work_, depth->image, VK_IMAGE_LAYOUT_GENERAL, &input_value, 1,
                        &kRangeDepth);
  }

  // Depth copy: the rectangle of the depth render target to a resolved texture
  // of the same host format, at the base GetResolveInfo computes
  // (graphics/util/draw.cpp:945-1010; 4 bytes per texel, like k_24_8).
  // clears: this same game command clears the render target right after resolving it. Only then is it worth
  // swapping the images: if the game kept drawing on top, the content would have to be brought back and the
  // copy would be paid anyway (measured: one restore per frame on the shadow map).
  // FH1 (fh1_native_ssaa): whether a surface is a supersampled one (the same test as DrawsVulkan::BeginPass).
  // The color is averaged by a blit. The depth cannot be (AMD has no blit for depth formats): its resolved texture
  // stays at twice the size, like the scaled shadow map's, since everything samples it with coordinates 0..1 and
  // no filtering, which picks one of each pixel's four samples.
  bool Ssaa(uint32_t msaa, uint32_t pitch_guest) const {
    return msaa == uint32_t(xenos::MsaaSamples::k4X) && pitch_guest > 640 && REXCVAR_GET(fh1_native_ssaa);
  }
  uint64_t ssaa_resolves_ = 0;

  void CopyDepth(const RegistersCopy& reg, uint32_t pitch, bool clears_2, bool ssaa = false) {
    const uint32_t info_target = reg.rb_copy_dest_info;
    if ((info_target >> 3) & 0x1) {
      Reject(3, "copy to a 3D texture or array: not yet");
      return;
    }
    int32_t x0, y0, x1, y1;
    if (!copy_image_ || !Rectangle(reg, pitch, x0, y0, x1, y1)) {
      return;
    }
    const uint32_t info = reg.rb_depth_info;
    ++copies_depth_;  // the game asks to resolve the depth to a texture
    Image* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, ssaa ? pitch * 2 : pitch);
    if (!depth) {
      return;
    }
    const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t height_target = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    uint32_t base =
        reg.rb_copy_dest_base +
        uint32_t(OffsetTile2D(int32_t(base_x), int32_t(base_y), pitch_target, 2));
    const uint32_t dx = uint32_t(x0) - base_x;
    uint32_t dy = uint32_t(y0) - base_y;
    const uint32_t format_texture = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
    // NFSC: with predicated tiling the scene depth is resolved in horizontal strips (race starts: 1280x256, 1280x256,
    // 1280x208), each one from row 0 of the depth render target to its own address, a whole number of 32-row stripes
    // into the texture of the first strip. The soft particles (tyre smoke) sample that one texture, so every strip has
    // to land inside it at its row; before, the later strips went to textures nobody reads and the smoke faded against
    // stale depth below row 256 (hard-edged grey blocks under the cars at the start of races, owner 2026-10-02).
    Resolved* container = nullptr;
    if (!depth->width_guest || depth->width_guest == depth->width) {  // not the scaled shadow map
      uint32_t row_extra = 0, base_container = 0;
      container = FindResolvedContainer(base & 0x1FFFFFFF, pitch_target, row_extra, base_container,
                                              format_depth_, ssaa ? 2 : 1);
      if (container) {
        if (!strips_depth_warned_) {
          strips_depth_warned_ = true;
          REXLOG_INFO("[fh1] depth resolve in strips: the copy to {:08X} goes inside the depth texture {:08X} "
                      "from row {}", base & 0x1FFFFFFF, base_container, row_extra);
        }
        base = base_container;
        dy += row_extra;
      }
    }
    // With the scaled shadow map, the resolved texture stays at that same size.
    // The scene picks it by address, without looking at the size, and samples it with normalized coordinates
    // and point sampling, so 1024 read with the UVs of 1600 gives the same texel as 1024 upscaled to 1600
    // with NEAREST: the image does not change. What is saved is the upscaling blit (1.78 ms on the console)
    // and the larger half of the copy (10.24 MB -> 4.2 MB, twice per frame).
    uint32_t width_resolved = pitch_target, height_resolved = height_target;
    if (ssaa) {
      width_resolved *= 2;
      height_resolved *= 2;
    }
    if (depth->width_guest && depth->width_guest != depth->width) {
      width_resolved = uint32_t(uint64_t(pitch_target) * depth->width / depth->width_guest);
      height_resolved = uint32_t(uint64_t(height_target) * depth->height / depth->height_guest);
    }
    // If the previous copy to this address is still deferred, it is recorded first (exact; it does not happen
    // in a race: the scene clear resolves it earlier). See fh1_native_lazy_depth.
    if (!pending_.empty() && pending_.count(base & 0x1FFFFFFF)) {
      RecordCopyPending(base & 0x1FFFFFFF);
      ++lazy_copied_write_;
    }
    Resolved* resolved = container ? container
                                     : GetResolved(base & 0x1FFFFFFF, width_resolved, height_resolved,
                                                       format_texture, false, format_depth_);
    if (!resolved || !Record()) {
      return;
    }
    // Depth is also included in the per-render-target inventory. Previously only the square cubemap faces were
    // recorded, so the shadow map (the most expensive copy in the frame) did not show up.
    NoteCopy(base & 0x1FFFFFFF, width_resolved, height_resolved, 0);
    if (diag_window_ || !diag_watched_.empty()) {  // fh1_native_diag_readers_s
      DiagWrite(base & 0x1FFFFFFF, info & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                    resolved->image.width, resolved->image.height, 0, true);
    }
    Prepare(*depth);
    Prepare(resolved->image);
    if (ssaa) {
      // All four samples of each pixel, with their stencil: the resolved depth is twice the size (see Ssaa).
      NoteAreaUseful(*depth, y1 * 2);
      NoteUseClear(*depth, uint32_t(std::max(x1, 0)) * 2, uint32_t(std::max(y1, 0)) * 2);
      if (uint32_t(x0) * 2 >= depth->width || uint32_t(y0) * 2 >= depth->height ||
          dx * 2 >= resolved->image.width || dy * 2 >= resolved->image.height) {
        return;
      }
      const uint32_t width_s = std::min({uint32_t(x1 - x0) * 2, depth->width - uint32_t(x0) * 2,
                                         resolved->image.width - dx * 2});
      const uint32_t height_s = std::min({uint32_t(y1 - y0) * 2, depth->height - uint32_t(y0) * 2,
                                          resolved->image.height - dy * 2});
      if (!width_s || !height_s) {
        return;
      }
      if (depth->content_invalid && REXCVAR_GET(fh1_native_resolve_valid_content)) {
        RestoreContent(*depth, true);
      }
      VkImageCopy whole{};
      whole.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
      whole.srcOffset = {x0 * 2, y0 * 2, 0};
      whole.dstSubresource = whole.srcSubresource;
      whole.dstOffset = {int32_t(dx * 2), int32_t(dy * 2), 0};
      whole.extent = {width_s, height_s, 1};
      ShadowMinimumCopySince(*depth);
      BarrierGlobal(commands_work_);
      CopyImages(commands_work_, depth->image, VK_IMAGE_LAYOUT_GENERAL, resolved->image.image,
                 VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
      ++copies_;
      NoteCopy(width_s, height_s);
      ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width_s) * height_s);
      return;
    }
    NoteAreaUseful(*depth, y1);  // the area the game really resolves
    NoteUseClear(*depth, uint32_t(std::max(x1, 0)), uint32_t(std::max(y1, 0)));
    // With the scaled shadow map, the rectangle the guest sends is in 1600-pixel units even though the image
    // is smaller. Everything below reasons in guest pixels.
    const uint32_t width_guest = depth->width_guest ? depth->width_guest : depth->width;
    const uint32_t height_guest = depth->height_guest ? depth->height_guest : depth->height;
    const bool scaled = width_guest != depth->width || height_guest != depth->height;
    const uint32_t width = std::min({uint32_t(x1 - x0), width_guest - uint32_t(x0),
                                     resolved->image.width - dx});
    const uint32_t height = std::min({uint32_t(y1 - y0), height_guest - uint32_t(y0),
                                    resolved->image.height - dy});
    if (!width || !height || uint32_t(x0) >= width_guest ||
        uint32_t(y0) >= height_guest || dx >= resolved->image.width ||
        dy >= resolved->image.height) {
      return;
    }
    // fh1_native_resolve_valid_content. If the content of this render target went away in an earlier
    // swap and nobody brought it back, it is in another texture: it is brought back before reading it (the
    // menu flicker).
    if (depth->content_invalid) {
      if (REXCVAR_GET(fh1_native_resolve_valid_content)) {
        RestoreContent(*depth, true);
      } else {
        ++resolve_content_old_;  // previous behavior: the old image is resolved
      }
    }
    if (scaled) {
      // Source and destination have the same reduced size, so it is a normal 1 to 1 copy
      // with the rectangle converted to image pixels. No blit, no scaling.
      const auto aX = [&](int32_t v) {
        return int32_t(int64_t(v) * depth->width / width_guest);
      };
      const auto aY = [&](int32_t v) {
        return int32_t(int64_t(v) * depth->height / height_guest);
      };
      const uint32_t width_img = std::min(
          {uint32_t(std::max(1, aX(x0 + int32_t(width)) - aX(x0))),
           depth->width - uint32_t(aX(x0)), resolved->image.width - uint32_t(aX(int32_t(dx)))});
      const uint32_t height_img = std::min(
          {uint32_t(std::max(1, aY(y0 + int32_t(height)) - aY(y0))),
           depth->height - uint32_t(aY(y0)), resolved->image.height - uint32_t(aY(int32_t(dy)))});
      if (!width_img || !height_img) {
        return;
      }
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.srcOffset = {aX(x0), aY(y0), 0};
      copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.dstOffset = {aX(int32_t(dx)), aY(int32_t(dy)), 0};
      copy.extent = {width_img, height_img, 1};
      BarrierGlobal(commands_work_);
      CopyImages(commands_work_, depth->image, VK_IMAGE_LAYOUT_GENERAL,
                     resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      ++copies_;
      ++shadows_uploads_;
      NoteCopy(width_img, height_img);
      ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width_img) * height_img);
      return;
    }
    // If the whole render target is resolved to a texture of the same size, the images are swapped and
    // nothing is copied (the shadow map is 1600x1600: 10 MB per copy).
    if (ResolveWithoutCopy()) {  // and if it is not swapped, why not
      const bool whole_2 = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                          width == depth->width && height == depth->height;
      // The clear is no longer a requirement (see fh1_native_swap_without_clear).
      const bool can = whole_2 && (clears_2 || REXCVAR_GET(fh1_native_swap_without_clear));
      if (can) {
        if (SwapWithResolved(*depth, *resolved, base & 0x1FFFFFFF)) {
          if (!clears_2) {
            ++swaps_without_clear_;
          }
          ShadowMinimumAfterSwap(*depth, base & 0x1FFFFFFF, clears_2);
          return;
        }
        ++without_swap_[2];  // the swap itself could not be done
      } else {
        /*
         * The label was backwards ever since fh1_native_swap_without_clear existed. With that setting
         * on, `can` = whole, so everything that lands here is "not resolved whole" and the log counted it as
         * "the command does not clear the render target". It is now split by the real cause: if the resolve
         * is whole, the cause is the clear; otherwise, it is the size.
         */
        ++without_swap_[whole_2 ? 0 : 1];  // 0: the command does not clear the render target; 1: not the whole render target
        if (warnings_without_swap_ < 8) {
          ++warnings_without_swap_;
          REXLOG_INFO("[native] C2 no swap: {}x{} of {}x{} at ({},{})->({},{}), clears {} (base {:03X})",
                      width, height, depth->width, depth->height, x0, y0, dx, dy, clears_2,
                      base & 0xFFF);
        }
      }
    }
    // FH1: the stencil goes with the depth, as in the console's resolve (its motion blur reads it from the texture:
    // TextureResolvedBytes).
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    copy.srcOffset = {x0, y0, 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
    copy.extent = {width, height, 1};
    // If this address is only requested by the composition without blur, the copy is deferred
    // (fh1_native_lazy_depth): it is done as soon as a draw really samples it; if nobody does,
    // it is not.
    ShadowMinimumCopySince(*depth);  // fh1_native_shadow_minimum
    if (DeferCopyDepth(base & 0x1FFFFFFF, *depth, *resolved, copy)) {
      return;
    }
    BarrierGlobal(commands_work_);
    CopyImages(commands_work_, depth->image, VK_IMAGE_LAYOUT_GENERAL,
                   resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ++copies_;
    NoteCopy(width, height);
    ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width) * height);
  }

  void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) override {
    ramp_game_ = ramp;
    RecomputeRamp();
  }

  // The output table = the game's ramp followed by the post-processing, with the formula and order of
  // GoldenEye-Recomp (ge_grade.cs.hlsl): temperature, brightness, contrast, tint, saturation, vibrance and
  // gamma. Everything that is single-channel is computed here for the 256 entries, so without saturation,
  // vibrance, vignette or scanlines there is no per-pixel work (and when off, the table is the ramp as
  // is). With them (grading_), the table stops before the gamma and without clamping, and the shader
  // does the rest.
  void RecomputeRamp() {
    const fh1::settings::Postprocess& p = postprocess_;
    grading_ = p.active && (p.saturation != 1.0f || p.vibrance != 0.0f || p.vignette > 0.0f || p.lines > 0.0f);
    const float tint[3] = {p.tint_r, p.tint_g, p.tint_b};
    const float inverse_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        float x = float(ramp_game_[i][c] & 0x3FF) / 1023.0f;
        if (p.active) {
          x += c == 0 ? p.temperature * 0.10f : c == 2 ? -p.temperature * 0.10f : 0.0f;
          x += p.brightness;
          x = (x - 0.5f) * p.contrast + 0.5f;
          x = x + (x * tint[c] - x) * p.tint;
          if (!grading_) {
            x = std::clamp(std::pow(std::max(x, 0.0001f), inverse_gamma), 0.0f, 1.0f);
          }
        }
        ramp_values_[i * 4 + c] = x;
      }
      ramp_values_[i * 4 + 3] = 1.0f;
    }
    const size_t extra = 256 * 4;
    ramp_values_[extra] = p.saturation;
    ramp_values_[extra + 1] = p.vibrance;
    ramp_values_[extra + 2] = inverse_gamma;
    ramp_values_[extra + 3] = 0.0f;
    ramp_values_[extra + 4] = p.vignette;
    ramp_values_[extra + 5] = p.lines;
    ramp_values_[extra + 6] = 0.0f;
    ramp_values_[extra + 7] = 0.0f;
    ++version_ramp_;
  }

  bool Present(rex::ui::Presenter* presenter_value, const TextureSwap& swap, uint32_t width,
                 uint32_t height) override {
    fh1::reflection_demand::NoteSwap();  // fh1_reflection_low_demand
#if REX_PLATFORM_SWITCH
    rex::watchdog::NoteProgress();  // freeze watchdog (sdk/src/core/watchdog.cpp): a frame was presented
#endif
    ++presentations_stamp_;  // see StampMemory
    // The per-draw diagnostic window opens here, on the PM4 ring thread, which is the one that records the
    // draws. Inside the paint call it would be another thread and the window would catch an arbitrary piece
    // of the frame (33 of 500 shadow draws were measured).
    OpenWindowDiagnostic();
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(0);  // the profiler's "game N fps", which read 0.0 with the native renderer
#endif
    // Histogram of intervals between Swaps (C2 report): whether they come in vsync steps or spread out.
    {
      const auto now = std::chrono::steady_clock::now();
      if (last_swap_ != std::chrono::steady_clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_swap_).count();
#if REX_PLATFORM_SWITCH
        // Frames over 45 ms go to stack sampling. The window starts one normal frame (33 ms) earlier, because
        // the game thread prepares the frame ahead of the ring. Ticks at 19.2 MHz.
        if (ms > 45.0) {
          uint64_t fin;
          asm volatile("mrs %0, cntpct_el0" : "=r"(fin));  // libnx's armGetSystemTick
          const uint64_t is_long = uint64_t((ms + 33.3) * 19200.0);
          RexSwitchPerfHitch(fin > is_long ? fin - is_long : 0, fin);
        }
#endif
        const uint32_t bucket = ms < 15.0    ? 0
                                : ms < 18.0  ? 1
                                : ms < 25.0  ? 2
                                : ms < 30.0  ? 3
                                : ms < 36.0  ? 4
                                : ms < 50.0  ? 5
                                : ms < 60.0  ? 6
                                : ms < 75.0  ? 7
                                : ms < 100.0 ? 8
                                : ms < 150.0 ? 9
                                             : 10;
        ++buckets_swap_[bucket];
        // The worst frame of the interval. The average is not felt; the peak is.
        if (ms > worst_swap_ms_) {
          worst_swap_ms_ = ms;
        }
        fh1::guard30::Beat(ms);  // the 30 FPS guard decides with this
        /*
         * What happens in a frame that runs long.
         *
         * Measured on the console: 7.3 % of frames exceed 50 ms and there are peaks of 2.1 seconds. Pipelines
         * are stable (116, 11 ms) and the texture cache no longer evicts, so the cause is something else.
         *
         * When a frame runs long, what that frame did (not the running total) is dumped. If the culprit is
         * loading textures, or creating pipelines, or a big copy, or simply that the game sent three times as
         * many draws, it shows here. It only writes when there is a stutter, so it costs nothing in the normal
         * case.
         */
        /*
         * The previous dump was not enough, and this is why.
         *
         * The stutters are 60-70 ms frames (median 67, against an average of 39.34) with an exactly normal
         * render load: 14 copies, 4 clears, 2 swaps, 1 restore, the same as any other frame. And they are not
         * quantized to the vblank (34 of them fall between 60.0 and 62.5 ms, which is not a multiple of
         * 16.67). So there are ~26 extra ms that are not in the drawing and the old dump did not see them.
         *
         * So now the time breakdown is dumped, not only the work count:
         *   - GPU work of that frame, with the timestamps we already measure. If it goes up to 60 ms it is
         *     scene load; if it stays at 33 the time went to the CPU or to waiting.
         *   - how long the thread took to record and how long to present.
         *   - new textures and pipelines, the usual suspects of an isolated peak.
         * With that, the log says where the 26 ms come from without guessing.
         */
        /*
         * The cap of 200 was blinding us for half a race.
         *
         * The 200 warnings ran out at t=135 s of a 234 s session, and the following intervals had 96 frames
         * over 50 ms without a single line. So exactly the most played stretch was a black hole. With 1000 a
         * whole session fits, and since it only writes when there is a stutter it costs nothing in the normal
         * case.
         */
        if (ms > double(REXCVAR_GET(fh1_hitch_ms)) && warnings_hitch_ < 1000) {
          ++warnings_hitch_;
          /* gpu_ns_ are raw GPU timestamps: multiply by 1.627 to get real milliseconds (see nfsc-nx docs/nfsmw-nx/measuring.md). */
          const double gpu_ms = double(gpu_ns_ - hitch_gpu_ns_) / 1e6 * 1.627;
          // The three [hitch] lines go to the report thread (FH1_REPORT_RING).
          FH1_REPORT_RING(
              "[hitch] frame of {:.1f} ms (GPU {:.1f} real, record {:.1f}; complete {:.1f}, pools {:.1f}): {} "
              "copies, {} clears, {} swaps, {} restores, {} GPU waits",
              ms, gpu_ms, double(ns_record_ - hitch_ns_record_) / 1e6,
              double(ns_complete_ - hitch_ns_complete_) / 1e6, double(ns_reset_pools_ - hitch_ns_pools_) / 1e6,
              copies_ - hitch_copies_,
              clears_ - hitch_clears_, swaps_ - hitch_resolves_,
              restores_ - hitch_restores_, waits_gpu_ - hitch_waits_);
          /*
           * Who waits for whom in this frame, in ms (see fh1_hitch_waits.h).
           * The game's and the ring's waits overlap: they do not add up to the frame, they say where it went.
           */
          namespace e = fh1::waits;
          const auto delta = [&](e::Type t) {
            return double(e::g_ns[t].load(std::memory_order_relaxed) - hitch_waits_ns_[t]) / 1e6;
          };
          const auto times = [&](e::Type t) {
            return e::g_times[t].load(std::memory_order_relaxed) - hitch_waits_times_[t];
          };
          FH1_REPORT_RING(
              "[hitch] waits (ms): game: executor handoff {:.1f} ({}), preparer handoff {:.1f} ({}), room in the "
              "ring {:.1f} ({}), inside sub_826E8EE8 {:.1f} ({}; vtable {:08X}, caller {:08X}) | ring: no work "
              "{:.1f} ({}), WAIT_REG_MEM {:.1f} ({}), GPU fence {:.1f}, output {:.1f} | textures checked {:.1f} "
              "MB, deferred {}",
              delta(e::kHandoffExecutor), times(e::kHandoffExecutor), delta(e::kHandoffPreparer),
              times(e::kHandoffPreparer), delta(e::kRoomRing), times(e::kRoomRing),
              delta(e::kGameMiddle), times(e::kGameMiddle), e::g_vtable_middle.load(std::memory_order_relaxed),
              e::g_caller_middle.load(std::memory_order_relaxed), delta(e::kRingWithoutWork),
              times(e::kRingWithoutWork), delta(e::kRingRegMem), times(e::kRingRegMem),
              double(ns_waits_gpu_ - hitch_ns_waits_gpu_) / 1e6,
              double(ns_wait_output_ - hitch_ns_wait_output_) / 1e6,
              double(e::g_bytes_fingerprint.load(std::memory_order_relaxed) - hitch_bytes_fingerprint_) / 1048576.0,
              e::g_fingerprints_deferred.load(std::memory_order_relaxed) - hitch_fingerprints_deferred_);
          // What the ring spent this frame on.
          const auto dif = [](const std::atomic<uint64_t>& a, uint64_t before) {
            return a.load(std::memory_order_relaxed) - before;
          };
          // And how long the ring waited for the vertex copy thread (WaitUploads). The wait before each
          // submit is inside "working" and the one before returning the read pointer is outside: it is read
          // separately, not added.
          // And what the ring spent creating textures (image, memory and view; "textures" starts afterwards),
          // how many the binding thread bound and how long the ring waited for it
          // (fh1_native_texture_binding_thread).
          FH1_REPORT_RING("[hitch] ring: {} draws, working {:.1f} ms, textures {:.1f} ms ({} uploads, {:.1f} MB; "
                           "{} created; fingerprints {:.1f} + {:.1f} ms); waiting for the vertex copy thread "
                           "{:.1f} ms ({} times), helping it {:.1f} ms ({} copies); creating textures {:.1f} ms on "
                           "the ring, {} bound on the thread, waiting for it {:.1f} ms; fingerprints on the "
                           "thread: {} textures in {:.1f} ms of the thread, copies on the ring {:.1f} ms, {} done "
                           "by the ring in {:.1f} ms, waiting for it {:.1f} ms",
                      dif(e::g_draws, hitch_draws_), double(dif(e::g_ns_ring_working, hitch_ns_ring_)) / 1e6,
                      double(dif(e::g_ns_textures, hitch_ns_textures_)) / 1e6,
                      dif(e::g_textures_uploads, hitch_textures_uploads_),
                      double(dif(e::g_bytes_uploaded, hitch_bytes_uploaded_)) / 1048576.0,
                      dif(e::g_textures_created, hitch_textures_created_),
                      double(dif(e::g_ns_fingerprint_raw, hitch_ns_fingerprint_raw_)) / 1e6,
                      double(dif(e::g_ns_fingerprint_data, hitch_ns_fingerprint_data_)) / 1e6,
                      double(dif(e::g_ns_waiting_copies, hitch_ns_waiting_copies_)) / 1e6,
                      dif(e::g_waits_copies, hitch_waits_copies_),
                      double(dif(e::g_ns_helping_copies, hitch_ns_helping_copies_)) / 1e6,
                      dif(e::g_copies_helped, hitch_copies_helped_),
                      double(dif(e::g_ns_create_textures, hitch_ns_create_textures_)) / 1e6,
                      dif(e::g_textures_bound_thread, hitch_textures_bound_thread_),
                      double(dif(e::g_ns_waiting_bindings, hitch_ns_waiting_bindings_)) / 1e6,
                      // the texture fingerprint thread (fh1_native_texture_fingerprint_thread)
                      dif(e::g_textures_fingerprint_thread, hitch_textures_fingerprint_thread_),
                      double(dif(e::g_ns_fingerprint_thread, hitch_ns_fingerprint_thread_)) / 1e6,
                      double(dif(e::g_ns_fingerprint_snapshot, hitch_ns_fingerprint_snapshot_)) / 1e6,
                      dif(e::g_textures_fingerprint_ring, hitch_textures_fingerprint_ring_),
                      double(dif(e::g_ns_fingerprint_ring, hitch_ns_fingerprint_ring_)) / 1e6,
                      double(dif(e::g_ns_waiting_fingerprints, hitch_ns_waiting_fingerprints_)) / 1e6);
          /*
           * The game's side. Four of the seven race stutters in one run come from the preparer (the executor
           * waiting for the handoff, or waiting for commands inside the list, with the ring idle), and the wait
           * without commands did not show up in any line. "Outside the list" is the preparer's simulation plus
           * its handoff wait (the one in the line above): the difference is what the game itself takes.
           */
          FH1_REPORT_RING("[hitch] game: executor without commands {:.1f} ms ({}) | the preparer: filling the "
                           "list {:.1f} ms ({}), of which TreeCull {:.1f} ms ({}); outside the list {:.1f} ms ({})",
                               delta(e::kExecutorWithoutCommands), times(e::kExecutorWithoutCommands),
                               delta(e::kPreparerList), times(e::kPreparerList),
                               delta(e::kPreparerScenery), times(e::kPreparerScenery),
                               delta(e::kPreparerOutside), times(e::kPreparerOutside));
        }
        for (uint32_t t = 0; t < fh1::waits::kNumTypes; ++t) {
          hitch_waits_ns_[t] = fh1::waits::g_ns[t].load(std::memory_order_relaxed);
          hitch_waits_times_[t] = fh1::waits::g_times[t].load(std::memory_order_relaxed);
        }
        hitch_ns_waits_gpu_ = ns_waits_gpu_;
        hitch_draws_ = fh1::waits::g_draws.load(std::memory_order_relaxed);
        hitch_ns_ring_ = fh1::waits::g_ns_ring_working.load(std::memory_order_relaxed);
        hitch_ns_textures_ = fh1::waits::g_ns_textures.load(std::memory_order_relaxed);
        hitch_textures_uploads_ = fh1::waits::g_textures_uploads.load(std::memory_order_relaxed);
        hitch_bytes_uploaded_ = fh1::waits::g_bytes_uploaded.load(std::memory_order_relaxed);
        hitch_textures_created_ = fh1::waits::g_textures_created.load(std::memory_order_relaxed);
        hitch_ns_fingerprint_raw_ = fh1::waits::g_ns_fingerprint_raw.load(std::memory_order_relaxed);
        hitch_ns_fingerprint_data_ = fh1::waits::g_ns_fingerprint_data.load(std::memory_order_relaxed);
        hitch_bytes_fingerprint_ = fh1::waits::g_bytes_fingerprint.load(std::memory_order_relaxed);
        hitch_fingerprints_deferred_ = fh1::waits::g_fingerprints_deferred.load(std::memory_order_relaxed);
        hitch_ns_waiting_copies_ = fh1::waits::g_ns_waiting_copies.load(std::memory_order_relaxed);  // 170
        hitch_waits_copies_ = fh1::waits::g_waits_copies.load(std::memory_order_relaxed);
        hitch_ns_helping_copies_ = fh1::waits::g_ns_helping_copies.load(std::memory_order_relaxed);  // 185
        hitch_copies_helped_ = fh1::waits::g_copies_helped.load(std::memory_order_relaxed);
        // Creating textures and the binding thread (fh1_native_texture_binding_thread).
        hitch_ns_create_textures_ = fh1::waits::g_ns_create_textures.load(std::memory_order_relaxed);
        hitch_textures_bound_thread_ = fh1::waits::g_textures_bound_thread.load(std::memory_order_relaxed);
        hitch_ns_waiting_bindings_ = fh1::waits::g_ns_waiting_bindings.load(std::memory_order_relaxed);
        // The texture fingerprint thread (fh1_native_texture_fingerprint_thread).
        hitch_textures_fingerprint_thread_ = fh1::waits::g_textures_fingerprint_thread.load(std::memory_order_relaxed);
        hitch_ns_fingerprint_thread_ = fh1::waits::g_ns_fingerprint_thread.load(std::memory_order_relaxed);
        hitch_ns_fingerprint_snapshot_ = fh1::waits::g_ns_fingerprint_snapshot.load(std::memory_order_relaxed);
        hitch_textures_fingerprint_ring_ = fh1::waits::g_textures_fingerprint_ring.load(std::memory_order_relaxed);
        hitch_ns_fingerprint_ring_ = fh1::waits::g_ns_fingerprint_ring.load(std::memory_order_relaxed);
        hitch_ns_waiting_fingerprints_ = fh1::waits::g_ns_waiting_fingerprints.load(std::memory_order_relaxed);
        hitch_ns_wait_output_ = ns_wait_output_;
        hitch_gpu_ns_ = gpu_ns_;
        hitch_ns_record_ = ns_record_;
        hitch_ns_complete_ = ns_complete_;
        hitch_ns_pools_ = ns_reset_pools_;
        hitch_copies_ = copies_;
        hitch_restores_ = restores_;
        hitch_waits_ = waits_gpu_;
        hitch_clears_ = clears_;
        hitch_resolves_ = swaps_;
      }
      last_swap_ = now;
    }
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    DiagReadersToPresent(base);  // fh1_native_diag_readers_s
    // fh1_native_lazy_front. Before submitting the work: either it is drawn from the image that
    // holds the content (no copy) or the deferred copy is recorded now, ahead of the output.
    Image* const source_front = FrontToPresent(base, width, height);
    DumpResolvedSiDue(source_front);  // fh1_dump_resolved_at_s
    const bool tile =
        presenter_value && REXCVAR_GET(fh1_native_diag_resolved) && ComposeTile(base);
    // Without waiting for the GPU: the output goes after the work on the same queue.
    if (!presenter_value || !SendWork(false)) {
      return false;
    }
    if (tile) {
      bool painted_tile = false;
      presenter_value->RefreshGuestOutput(
          1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
            auto& context_id =
                static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
            painted_tile = PaintOutput(context_id, tile_, 1280, 720);
            return painted_tile;
          });
      if (painted_tile) {
        ++presented_;
      }
      return painted_tile;
    }
    auto it = resolved_.find(base);
    // FH1: while it loads at boot the game resolves its trademark screen once into a texture and then presents
    // front buffers it has not resolved anything into. Show the last screen-sized resolved texture then, instead
    // of nothing.
    if ((it == resolved_.end() || !it->second.image.prepared) && last_resolved_screen_) {
      it = resolved_.find(last_resolved_screen_);
    }
    if (it == resolved_.end() || !it->second.image.prepared) {
      {  // FH1 diagnostic: which front buffer the game presents and what has been resolved so far (first 6 times)
        static uint32_t seen = 0;
        if (seen < 6) {
          ++seen;
          std::string known;
          for (const auto& [address, r] : resolved_) {
            known += fmt::format(" {:08X}{}", address, r.image.prepared ? "" : "(not prepared)");
          }
          REXLOG_INFO("[fh1] Swap of front buffer {:08X} ({}x{}) without a resolved texture; resolved so far:{}", base,
                      width, height, known);
        }
      }
      Reject(4, "Swap without a resolved texture: nothing is painted");
      return false;
    }
    Resolved& resolved = it->second;
    const uint32_t w = std::min(width ? width : resolved.image.width, resolved.image.width);
    const uint32_t h = std::min(height ? height : resolved.image.height, resolved.image.height);
    bool painted = false;
    // How much of RefreshGuestOutput belongs to the SDK (before and after the callback) and how much to
    // PaintOutput.
    const auto before_refresh = std::chrono::steady_clock::now();
    auto entry_call = before_refresh;
    auto output_call = before_refresh;
    bool call = false;
    presenter_value->RefreshGuestOutput(
        w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
          entry_call = std::chrono::steady_clock::now();
          call = true;
          auto& context_id =
              static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
          painted = PaintOutput(context_id, source_front ? *source_front : resolved.image, w, h,
                                 source_front != nullptr);  // fh1_native_lazy_front
          output_call = std::chrono::steady_clock::now();
          return painted;
        });
    if (call) {
      ns_before_call_ += Ns(before_refresh, entry_call);
      ns_in_call_ += Ns(entry_call, output_call);
      ns_after_call_ += Ns(output_call, std::chrono::steady_clock::now());
      ++refreshes_;
    }
    if (painted) {
      ++presented_;
      // NFSC: the F2 monitor reads these (the Xenos plugin records them itself; the native renderer did not).
      const auto now_stats = std::chrono::steady_clock::now();
      if (presented_stats_ == 0) {
        rex::SetRendererName("Native Vulkan");
        start_window_fps_ = now_stats;
      } else {
        const double ms = std::chrono::duration<double, std::milli>(now_stats - last_presented_stats_).count();
        rex::RecordPresentedFrame(ms);
        times_window_fps_.push_back(float(ms));
      }
      ++presented_stats_;
      last_presented_stats_ = now_stats;
      // NFSC: the same "[fps]" log line as the Xenos path (CommandProcessor::NoteFramePresented), every 5 s, so
      // test_auto.ps1 / fps_summary.py measure the native renderer too.
      const double window_s = std::chrono::duration<double>(now_stats - start_window_fps_).count();
      if (window_s >= 5.0 && !times_window_fps_.empty()) {
        std::vector<float>& t = times_window_fps_;
        const double worst = *std::max_element(t.begin(), t.end());
        const double late_2 = 100.0 * double(std::count_if(t.begin(), t.end(), [](float v) { return v > 17.2f; })) /
                             double(t.size());
        // "missed": a gap over 25 ms means a screen refresh was skipped (on a 60 Hz display the next possible
        // gap is 33 ms). Gaps of 17-25 ms are present-call timing jitter that nobody sees; "late" counts them.
        const double missed = 100.0 * double(std::count_if(t.begin(), t.end(), [](float v) { return v > 25.0f; })) /
                              double(t.size());
        const size_t slow = std::max<size_t>(1, t.size() / 100);
        std::nth_element(t.begin(), t.begin() + (slow - 1), t.end(), std::greater<float>());
        double sum = 0;
        for (size_t i = 0; i < slow; ++i) sum += t[i];
        REXLOG_INFO("[fps] {:.1f} frames/s over {:.1f} s, worst frame gap {:.1f} ms, 1% low {:.1f} fps, late {:.0f}%, "
                    "missed {:.2f}%",
                    double(t.size()) / window_s, window_s, worst, sum > 0 ? 1000.0 * double(slow) / sum : 0.0,
                    late_2, missed);
        t.clear();
        start_window_fps_ = now_stats;
      }
    }
    return painted;
  }

  // fh1_native_lazy_composite (see the cvar's comment). PM4 ring thread only.
  // It applies to the (address, source) pair that the diagnostic's watching has seen for kCompositeALook
  // writes without any read after the source was written again. Until then, it copies as always (WATCHING).
  bool CompositeApplicable(uint32_t address, uint32_t source) {
    if (composite_off_ || !REXCVAR_GET(fh1_native_lazy_composite)) {
      return false;
    }
    for (const WatchedDiag& v : diag_watched_) {
      if (v.address != address || v.source != source) {
        continue;
      }
      if (v.late != 0 || v.writes < kCompositeALook) {
        return false;
      }
      if (composite_applying_ != address) {
        composite_applying_ = address;
        FH1_REPORT_RING("[native] C2 lazy composite: {:08X} from {:03X}/{}: {} writes watched, {} read before "
                         "their source was written again and 0 after: APPLYING (the copy is deferred; it is "
                         "recorded before the first draw that reads it and dropped when its source is written)",
                             address, source & 0xFFF, source >> 12, v.writes, v.before);
      }
      return true;
    }
    return false;
  }

  // From Copy's 1 to 1 path: true if the copy is deferred (not recorded now).
  bool DeferComposite(uint32_t address, Image& target, const Resolved& resolved, const VkImageCopy& copy,
                        uint32_t source) {
    if (composite_there_is_ || REXCVAR_GET(fh1_native_diag_resolved) || REXCVAR_GET(fh1_native_swap_color)) {
      return false;
    }
    if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
        copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
        target.format != kFormatColor || resolved.image.format != kFormatColor) {
      return false;
    }
    // The small ones are read back for the guest (ReadResolved): never those.
    const int32_t texels_read = REXCVAR_GET(fh1_native_read_resolved_texels);
    if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
      return false;
    }
    if (!CompositeApplicable(address, source)) {
      return false;
    }
    composite_.address = address;
    composite_.source = source;
    composite_.target = &target;
    composite_.source_vk = target.image;
    composite_.texture_vk = resolved.image.image;
    composite_.copy = copy;
    composite_there_is_ = true;
    ResolvedWritten(address);  // new contents (deferred)
    ++composite_deferred_;
    return true;
  }

  // Records the deferred copy now (outside a pass). Exact: its source has not been touched since it was
  // deferred.
  void RecordComposite() {
    if (!composite_there_is_) {
      return;
    }
    composite_there_is_ = false;
    const CompositePending p = composite_;
    const auto r = resolved_.find(p.address);
    if (r == resolved_.end() || r->second.image.image != p.texture_vk || !p.target ||
        p.target->image != p.source_vk || !copy_image_ || !Record()) {
      composite_stale_ = p.address;
      CompositeDifference(p.address, "the deferred copy can no longer be recorded (the source image or the texture "
                                     "changed)");
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // it can arrive from a draw: the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    CopyImages(commands_work_, p.source_vk, VK_IMAGE_LAYOUT_GENERAL, p.texture_vk, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &p.copy);
    ++copies_;
    NoteCopy(p.copy.extent.width, p.copy.extent.height);
    ResolvedWritten(p.address, uint64_t(p.copy.extent.width) * p.copy.extent.height);
  }

  // Its source is about to be written and nobody has read it: it is dropped (or recorded, if the guard
  // turned it off). The texture is left without it until the next whole write; if someone samples it
  // before that, the guard fires.
  void CompositeBeforeOfWriteSource() {
    if (!composite_there_is_) {
      return;
    }
    if (composite_off_) {
      RecordComposite();
      ++composite_recorded_other_;
      return;
    }
    composite_there_is_ = false;
    composite_stale_ = composite_.address;
    composite_pixels_saved_ += uint64_t(composite_.copy.extent.width) * composite_.copy.extent.height;
    ++composite_dropped_source_;
  }

  // Before each draw with a deferred copy or a stale texture: if the draw samples it, the deferred copy is
  // recorded first (exact) and the stale one trips the guard; if it draws into its source, the deferred
  // copy is dropped.
  void CompositeBeforeOfDraw(const RequestDraw& p) {
    if (!p.ps || !p.register_values) {
      return;  // without a PS, no color is sampled or written
    }
    const uint32_t* r = p.register_values;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_entry >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_entry) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
          ((f[5] >> 9) & 0x3) != uint32_t(xenos::DataDimension::k2DOrStacked)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      if (composite_there_is_ && base == composite_.address) {
        RecordComposite();
        ++composite_recorded_read_;
      } else if (composite_stale_ != 0 && base == composite_stale_) {
        ++composite_reads_late_;
        CompositeDifference(base, "a draw samples it after its copy was dropped (it sees the scene from before the "
                                  "composition)");
        composite_stale_ = 0;
      }
    }
    if (composite_there_is_ && DrawWritesIn(r, composite_.source)) {
      CompositeBeforeOfWriteSource();
    }
  }

  // Before each resolve to a texture (from Copy, before copying anything): another write to the deferred
  // address. If it covers it entirely, the deferred copy is unnecessary (nobody has read it: exact);
  // otherwise it is recorded first.
  void CompositeBeforeOfWriteTexture(uint32_t address, bool whole) {
    if (composite_there_is_ && composite_.address == address) {
      if (whole) {
        composite_there_is_ = false;
        composite_pixels_saved_ += uint64_t(composite_.copy.extent.width) * composite_.copy.extent.height;
        ++composite_replaced_;
      } else {
        RecordComposite();
        ++composite_recorded_other_;
      }
    }
    if (whole && composite_stale_ == address) {
      composite_stale_ = 0;  // whole new content: it no longer misses the dropped copy
    }
  }

  // An image that is destroyed (Destroy) cannot stay in the deferred copy.
  void CompositeToDestroy(const Image& image) {
    if (!composite_there_is_ || image.image == VK_NULL_HANDLE) {
      return;
    }
    if (image.image == composite_.source_vk) {
      composite_there_is_ = false;  // without its source there is no copy to make: the texture becomes stale
      composite_stale_ = composite_.address;
    } else if (image.image == composite_.texture_vk) {
      composite_there_is_ = false;  // the texture is rebuilt: its content is lost just as before
    }
  }

  void CompositeDifference(uint32_t address, const char* reason) {
    ++composite_differences_;
    if (composite_off_) {
      return;
    }
    composite_off_ = true;
    REXLOG_ERROR("[native] C2 lazy composite: DIFFERENCE at {:08X}: {}. Off for the rest of the session: always "
                 "copied, as before build 184",
                 address, reason);
  }

  // With the diagnostic's summary (every fh1_native_diag_readers_s seconds).
  void CompositeReport() {
    if (!composite_deferred_ && !composite_differences_) {
      return;
    }
    const uint64_t frames = presented_ - composite_presented_previous_;
    const double mp = double(composite_pixels_saved_ - composite_pixels_previous_) / 1e6;
    const double by_frame = frames ? mp / double(frames) : 0.0;
    FH1_REPORT_RING("[native] C2 lazy composite (build 184): {} copies deferred since startup; {} recorded before "
                     "a draw that reads it (exact) and {} before another write; {} dropped when their source was "
                     "written and {} when another whole one covered it: {:.2f} Mpixels per frame not copied "
                     "(~{:.2f} real ms); late reads {}{}",
                         composite_deferred_, composite_recorded_read_, composite_recorded_other_,
                         composite_dropped_source_, composite_replaced_, by_frame, by_frame * 0.60,
                         composite_reads_late_,
                         composite_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
    composite_pixels_previous_ = composite_pixels_saved_;
    composite_presented_previous_ = presented_;
  }

  // fh1_native_diag_readers_s (see the cvar's comment). PM4 ring thread only.
  // On each Swap, with the front buffer it draws: opens the window (two whole frames, Swap to Swap) or closes
  // it.
  void DiagReadersToPresent(uint32_t front) {
    if (diag_window_) {
      if (resolved_.count(front)) {
        DiagRead(front, kReaderSwap, 0, 0);
      }
      if (++diag_frame_ >= 2) {
        DiagReadersReport();
        diag_window_ = false;
        diag_writes_.clear();
      }
      return;
    }
    const int32_t every = REXCVAR_GET(fh1_native_diag_readers_s);
    if (every <= 0) {
      diag_watched_.clear();  // turned off at run time: nothing is watched either
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - diag_readers_last_ < std::chrono::seconds(every)) {
      return;
    }
    diag_readers_last_ = now;
    diag_window_ = true;
    diag_frame_ = 0;
    diag_writes_.clear();
  }

  // A logical write to the resolved texture at `address`: the resolve the game requests (color or depth).
  void DiagWrite(uint32_t address, uint32_t source, uint32_t pitch, int32_t x0, int32_t y0,
                     uint32_t requested_width, uint32_t requested_height, uint32_t width, uint32_t height, uint64_t draws,
                     bool depth) {
    const uint32_t de = (source & 0xFFF) | (pitch << 12);
    for (WatchedDiag& v : diag_watched_) {
      if (v.address == address) {
        v.pending = de == v.source;  // from here on, whoever reads it needs the watched copy
        v.source_written = false;
        v.writes += v.pending ? 1 : 0;
      }
    }
    if (!diag_window_) {
      return;
    }
    std::vector<WriteDiag>& list = diag_writes_[address];
    if (list.size() >= kDiagMaxWrites) {
      ++diag_writes_lost_;
      return;
    }
    WriteDiag e;
    e.frame = int32_t(diag_frame_);
    e.order = 1;
    for (const WriteDiag& other : list) {
      e.order += other.frame == e.frame ? 1 : 0;
    }
    e.source = de;
    e.requested_width = requested_width;
    e.requested_height = requested_height;
    e.width = width;
    e.height = height;
    e.draws = draws;
    e.depth = depth;
    e.whole = x0 == 0 && y0 == 0 && requested_width >= width && requested_height >= height;
    list.push_back(std::move(e));
  }

  // A read of `address` in the window: it goes to its last write (or to the one before the window).
  void DiagRead(uint32_t address, uint32_t ps, uint32_t reg_entry, uint32_t target) {
    std::vector<WriteDiag>& list = diag_writes_[address];
    if (list.empty()) {
      list.emplace_back();  // what it already held before the window (frame -1)
    }
    WriteDiag& e = list.back();
    for (ReaderDiag& l : e.readers) {
      if (l.ps == ps && l.reg_entry == reg_entry && l.target == target) {
        ++l.times;
        return;
      }
    }
    if (e.readers.size() >= kDiagMaxReaders) {
      ++e.others;
      return;
    }
    e.readers.push_back(ReaderDiag{ps, reg_entry, target, 1});
  }

  // The resolved textures the PS of this draw samples, read from its fetch constants the way DrawsVulkan
  // reads them (sampler registers 0-15; 2D by their address and cubemaps by their 6 faces), bypassing the
  // caches.
  void DiagReadsDraw(const RequestDraw& p) {
    if (!p.ps || !p.register_values) {
      return;
    }
    const uint32_t* r = p.register_values;
    const uint32_t target = (r[kDiagRegColorInfo] & 0xFFF) | ((r[kDiagRegSurfaceInfo] & 0x3FFF) << 12);
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_entry >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_entry) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      const uint32_t dimension = (f[5] >> 9) & 0x3;
      if (dimension == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        if (resolved_.count(base)) {
          DiagRead(base, p.ps->number, s.reg_entry, target);
          WatchRead(base, p.ps->number);
        }
      } else if (dimension == uint32_t(xenos::DataDimension::kCube)) {
        // The faces of a resolved cubemap are consecutive, as DrawsVulkan looks for them (8888: 4 bytes per
        // texel, rows and columns aligned to 32 and each face to 4 KB).
        const uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
        const uint32_t pitch = std::max<uint32_t>(((f[0] >> 22) & 0x1FF) << 5, 1);
        const uint64_t stride =
            (uint64_t((pitch + 31) & ~uint32_t(31)) * 4 * ((height + 31) & ~uint32_t(31)) + 4095) & ~uint64_t(4095);
        for (uint32_t c = 0; c < 6; ++c) {
          const uint32_t face = uint32_t((uint64_t(base) + c * stride) & 0x1FFFFFFF);
          if (resolved_.count(face)) {
            DiagRead(face, p.ps->number, s.reg_entry, target);
          }
        }
      }
    }
    DiagSourceWrittenByDraw(r);  // after its reads: the draw reads its textures before writing
  }

  // Outside the window, with something watched: only the 2D address of each sampler is compared with the
  // watched ones.
  void DiagWatch(const RequestDraw& p) {
    if (!p.ps || !p.register_values) {
      return;
    }
    const uint32_t* r = p.register_values;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_entry >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_entry) * 6;
      if ((f[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        WatchRead(((f[1] >> 12) << 12) & 0x1FFFFFFF, p.ps->number);
      }
    }
    DiagSourceWrittenByDraw(r);
  }

  // A read of a watched address after its watched write. Before the source is written again, a deferred
  // copy would cover it (it would be recorded right before that draw); afterwards, dropping the copy when
  // the source is written would not be exact.
  void WatchRead(uint32_t address, uint32_t ps) {
    for (WatchedDiag& v : diag_watched_) {
      if (v.address != address || !v.pending) {
        continue;
      }
      if (!v.source_written) {
        if (v.before++ == 0) {
          v.ps_before = ps;
          FH1_REPORT_RING("[native] C2 readers (build 184): {:08X}, written from {:03X}/{}, is read by PS n{} "
                           "BEFORE its source is written again (after {} watched writes): a deferred copy covers it",
                               address, v.source & 0xFFF, v.source >> 12, ps, v.writes);
        }
      } else if (v.late++ == 0) {
        v.ps_late = ps;
        FH1_REPORT_RING("[native] C2 readers (build 184): {:08X}, written from {:03X}/{}, is read by PS n{} AFTER "
                         "its source is written (after {} watched writes): dropping that copy when the source is "
                         "written would NOT be exact",
                             address, v.source & 0xFFF, v.source >> 12, ps, v.writes);
      }
    }
  }

  // A draw (or a clear) on render target `target` (EDRAM base | pitch << 12): if it is the source of a
  // pending watched write, its content is no longer in the source.
  void DiagSourceWritten(uint32_t target) {
    for (WatchedDiag& v : diag_watched_) {
      if (v.pending && !v.source_written && v.source == target) {
        v.source_written = true;
      }
    }
  }

  // Whether a draw writes to `target`: its four color render targets with their write mask. Over-reporting
  // is harmless (it is treated as written earlier); under-reporting is not.
  static bool DrawWritesIn(const uint32_t* r, uint32_t target) {
    const uint32_t mask = r[kDiagRegColorMask];
    const uint32_t pitch = (r[kDiagRegSurfaceInfo] & 0x3FFF) << 12;
    for (uint32_t i = 0; i < 4; ++i) {
      if (((mask >> (4 * i)) & 0xF) != 0 && ((r[kDiagRegsColorInfo[i]] & 0xFFF) | pitch) == target) {
        return true;
      }
    }
    return false;
  }

  void DiagSourceWrittenByDraw(const uint32_t* r) {
    for (WatchedDiag& v : diag_watched_) {
      if (v.pending && !v.source_written && DrawWritesIn(r, v.source)) {
        v.source_written = true;
      }
    }
  }

  // When the window closes: one line per address (if its pattern has changed) and the summary line.
  void DiagReadersReport() {
    ++diag_windows_;
    std::vector<uint32_t> addresses;
    addresses.reserve(diag_writes_.size());
    for (const auto& [address, list] : diag_writes_) {
      addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    const uint64_t texels_cpu = uint64_t(std::max<int32_t>(REXCVAR_GET(fh1_native_read_resolved_texels), 0));
    std::string spare_2;
    uint32_t writes = 0, new_ones = 0, equal = 0;
    for (const uint32_t address : addresses) {
      const std::vector<WriteDiag>& list = diag_writes_[address];
      const auto r = resolved_.find(address);
      const uint32_t width = r != resolved_.end() ? r->second.image.width : 0;
      const uint32_t height = r != resolved_.end() ? r->second.image.height : 0;
      // The small ones are also copied to guest memory and read by the CPU (the exposure): they are never
      // unnecessary.
      const bool cpu = width && uint64_t(width) * height <= texels_cpu;
      std::string line;
      uint64_t signature = address;
      for (size_t i = 0; i < list.size(); ++i) {
        const WriteDiag& e = list[i];
        if (e.frame < 0) {
          line += " [before]";
        } else {
          ++writes;
          const bool blit = e.requested_width * 4 >= e.width * 5 || e.requested_height * 4 >= e.height * 5;
          line += fmt::format(" [{}.{}] {} {}x{} from {:03X}/{}{} ({} draws before)", e.frame, e.order,
                               e.depth ? "depth" : (blit ? "blit" : "1:1"), e.requested_width,
                               e.requested_height, e.source & 0xFFF, e.source >> 12, e.whole ? "" : ", parcial",
                               e.draws);
        }
        signature = (signature ^ (uint64_t(uint32_t(e.frame + 1)) << 48 | uint64_t(e.order) << 40 | e.source)) *
                0x100000001B3ull;
        if (e.readers.empty()) {
          const bool next = i + 1 < list.size();
          if (cpu) {
            line += " -> no draw (the CPU reads it)";
          } else if (e.frame >= 0 && next && list[i + 1].whole) {
            line += " -> NOBODY before the next one, which covers it whole: UNUSED";
            spare_2 += fmt::format(" {:08X} [{}.{}] from {:03X}/{}", address, e.frame, e.order,
                                  e.source & 0xFFF, e.source >> 12);
            signature ^= 0x5A5Au;
            AddWatched(address, e.source);
          } else {
            line += next ? " -> nobody before the next one (which does not cover it whole)"
                               : " -> nobody until the end of the window";
          }
        } else {
          line += " ->";
          for (const ReaderDiag& l : e.readers) {
            if (l.ps == kReaderSwap) {
              line += fmt::format(" Swap x{}", l.times);
            } else {
              line += fmt::format(" PS n{} s{} x{} at {:03X}/{}", l.ps, l.reg_entry, l.times, l.target & 0xFFF,
                                   l.target >> 12);
            }
            signature = (signature ^ (uint64_t(l.ps) << 32 | uint64_t(l.reg_entry) << 26 | (l.target & 0x3FFFFFF))) *
                    0x100000001B3ull;
          }
          if (e.others) {
            line += fmt::format(" (and {} more readers)", e.others);
          }
        }
        line += ";";
      }
      uint64_t& previous = diag_signatures_[address];
      if (previous == signature && diag_windows_ > 3) {
        ++equal;  // the same pattern as the last time it was written: not repeated
        continue;
      }
      previous = signature;
      ++new_ones;
      FH1_REPORT_RING("[native] C2 readers (build 184, window {}): {:08X} {}x{}:{}", diag_windows_, address,
                           width, height, line);
    }
    std::string watched;
    for (const WatchedDiag& v : diag_watched_) {
      watched += fmt::format(" {:08X} from {:03X}/{}: {} writes; {} reads before the source is written again{} and "
                             "{} after{};",
                               v.address, v.source & 0xFFF, v.source >> 12, v.writes, v.before,
                               v.before ? fmt::format(" (the first, PS n{})", v.ps_before) : std::string(),
                               v.late,
                               v.late ? fmt::format(" (the first, PS n{}): dropping it would NOT be exact", v.ps_late)
                                         : std::string(v.writes ? " (dropping it when the source is written would "
                                                                  "be exact)"
                                                                    : ""));
    }
    FH1_REPORT_RING("[native] C2 readers of the resolved textures (build 184, window {}, 2 frames every {} s): {} "
                     "addresses and {} writes; {} lines written and {} equal to the last one written (not "
                     "repeated){}; writes nobody reads before another that covers them whole:{} | watched in every "
                     "frame:{}",
                         diag_windows_, REXCVAR_GET(fh1_native_diag_readers_s), addresses.size(), writes,
                         new_ones, equal,
                         diag_writes_lost_
                             ? fmt::format(" ({} writes not noted because of the cap of {} per address)",
                                           diag_writes_lost_, kDiagMaxWrites)
                             : std::string(),
                         spare_2.empty() ? std::string(" none") : spare_2,
                         watched.empty() ? std::string(" none") : watched);
    CompositeReport();  // fh1_native_lazy_composite
  }

  // A write that is unnecessary in a window becomes watched in every frame (at most kDiagMaxWatched).
  void AddWatched(uint32_t address, uint32_t source) {
    for (const WatchedDiag& v : diag_watched_) {
      if (v.address == address && v.source == source) {
        return;
      }
    }
    if (diag_watched_.size() < kDiagMaxWatched) {
      WatchedDiag v;
      v.address = address;
      v.source = source;
      diag_watched_.push_back(v);
    }
  }

  // Per-draw diagnostic window, one whole frame every N seconds. It is decided
  // in the Swap because the upload buffer rotates several times per frame.
  void OpenWindowDiagnostic() {
    const int32_t every = REXCVAR_GET(fh1_native_per_draw_statistics_s);
    if (every <= 0) {
      window_diagnostic_ = false;
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (window_diagnostic_) {
      window_diagnostic_ = false;  // that frame was already measured
      return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_window_).count() >= every) {
      last_window_ = now;
      window_diagnostic_ = true;
      window_read_ = false;
    }
  }

  void Statistics(uint64_t& copies, uint64_t& clears, uint64_t& presented,
                    uint64_t& rejections) const override {
    copies = copies_;
    clears = clears_;
    presented = presented_;
    rejections = rejections_;
  }

  // Depth copies (the only ones that force the depth tile to be stored).
  uint64_t CopiesDepth() const override { return copies_depth_; }
  uint64_t ClearsDepth() const override { return clears_depth_; }

  void TimeGpu(uint64_t& nanoseconds, uint64_t& jobs) const override {
    nanoseconds = gpu_ns_;
    jobs = gpu_jobs_;
  }

  uint64_t MarkGpuFinalNs() const override { return uint64_t(double(last_mark_gpu_) * period_mark_ns_); }
  uint64_t JobsMarksPrecise() const override { return gpu_jobs_precise_; }

  void WaitsGpu(uint64_t& times, uint64_t& nanoseconds) const override {
    times = waits_gpu_;
    nanoseconds = ns_waits_gpu_;
  }

  void DurationJobsGpu(uint64_t& times, uint64_t& nanoseconds) const override {
    times = jobs_gpu_;
    nanoseconds = ns_work_gpu_;
  }

  void CostRecord(uint64_t cost[6]) const override {
    cost[0] = recordings_;
    cost[1] = ns_record_;
    cost[2] = ns_reset_pools_;
    cost[3] = reads_written_;
    cost[4] = texels_written_;
    cost[5] = ns_write_reads_;
  }

  // C2 report: waits of the ring for the GPU in CompleteOne.
  uint64_t waits_gpu_ = 0;
  uint64_t ns_waits_gpu_ = 0;
  uint64_t jobs_gpu_ = 0;     // work units timed with the wall clock
  uint64_t ns_work_gpu_ = 0;   // and how long they took from submit to fence
  // C2 report: cost of Record and of the readbacks.
  uint64_t recordings_ = 0;
  uint64_t ns_record_ = 0;
  uint64_t ns_complete_ = 0;  // NFSC: part of ns_record_ spent finishing the slot (fence, timers, occlusion results)
  uint64_t hitch_ns_complete_ = 0, hitch_ns_pools_ = 0;
  uint64_t ns_reset_pools_ = 0;
  uint64_t reads_written_ = 0;
  uint64_t texels_written_ = 0;
  uint64_t ns_write_reads_ = 0;

  void TimeGpuByCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const override {
    nanoseconds = gpu_categories_ns_;
  }

  void StatisticsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                            std::array<uint64_t, kGpuCategories>& vertices,
                            std::array<uint64_t, kGpuCategories>& primitives) const override {
    fragments = fragments_category_;
    vertices = vertices_category_;
    primitives = primitives_category_;
  }

  void CopiesBySize(std::array<uint64_t, 4>& copies, std::array<uint64_t, 4>& pixels) const override {
    copies = copies_bucket_;
    pixels = pixels_bucket_;
  }

  void StatisticsByShader(std::vector<uint64_t>& fragments, std::vector<uint64_t>& draws,
                             uint64_t& frames) const override {
    fragments = fragments_by_shader_;
    draws = draws_by_shader_;
    frames = frames_diagnostic_;
  }

  void CostPresent(uint64_t cost[12]) const override {
    const uint64_t values[12] = {waits_output_,   ns_wait_output_,  submissions_work_,  ns_lock_work_,
                                  ns_submit_work_, submissions_output_,     ns_lock_output_, ns_submit_output_,
                                  refreshes_,        ns_before_call_, ns_in_call_,     ns_after_call_};
    std::copy(std::begin(values), std::end(values), cost);
  }

  void IntervalsBetweenSwaps(std::array<uint64_t, kBucketsSwap>& buckets, uint64_t& overlaps,
                            double& worst_ms) const override {
    buckets = buckets_swap_;
    worst_ms = worst_swap_ms_;
    worst_swap_ms_ = 0.0;  // reset on every report: what matters is the worst of this interval
    overlaps = overlaps_gpu_;
  }

  bool Draw(const RequestDraw& request) override {
    if (composite_there_is_ || composite_stale_ != 0) {
      CompositeBeforeOfDraw(request);  // fh1_native_lazy_composite
    }
    if (diag_window_) {
      DiagReadsDraw(request);  // fh1_native_diag_readers_s
    } else if (!diag_watched_.empty()) {
      DiagWatch(request);
    }
    return draws_ && draws_->Draw(request);
  }

  StatisticsDraws StatisticsOfDraws() const override {
    return draws_ ? draws_->Statistics() : StatisticsDraws{};
  }

  void WaitUploads() override {
    if (draws_) {
      draws_->WaitUploads();
    }
  }

  size_t CopiesPending() const override {  // fence measurement only
    return draws_ ? draws_->CopiesPending() : 0;
  }

  // --- ContextTargets (parts C3-C6) ----------------------------------------

  VkCommandBuffer CommandsWork() override {
    return Record() ? commands_work_ : VK_NULL_HANDLE;
  }

  VkCommandBuffer CommandsUpload() override {
    if (!Record()) {
      return VK_NULL_HANDLE;
    }
    if (!recording_upload_) {
      VkCommandBufferBeginInfo start{};
      start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (dfn_.vkBeginCommandBuffer(commands_upload_, &start) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      recording_upload_ = true;
    }
    return commands_upload_;
  }

  uint64_t GenerationCommands() const override { return generation_commands_; }

  ImageNative* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) override {
    Image* image = GetTarget(base, format, pitch);
    if (!image || !Record()) {
      return nullptr;
    }
    Prepare(*image);
    BeforeOfWriteColor(*image);  // fh1_native_lazy_front
    // Mandatory since color is also swapped. Here a pass starts that draws on top; if the content went away
    // in a swap and nobody has cleared it, it has to be brought back. Without this, the first frame that
    // draws without clearing shows the previous frame.
    RestoreContent(*image);
    if (!fills_depth_.empty()) {
      ApplyFillDepth(*image, base, pitch);  // fh1_native_depth_fill_color
    }
    return image;
  }

  // FH1 (fh1_native_depth_fill_color). On the console a color target and a depth buffer with the same base are the
  // same EDRAM, and Direct3D clears a color target through it: at evening FH1 points the depth buffer at the
  // shadow / headlight mask (base 2D0) and draws a depth-only rectangle with Z = 1 and stencil FF, which leaves
  // FFFFFFFF = white in every pixel; the headlight pass then multiplies its light into green, blue and alpha.
  // Here the two are separate images, so the mask kept the daytime picture (blue and alpha 0 on everything but the
  // sky, green multiplied down to 0 frame after frame) and the car paint, which reads the headlight light from
  // those channels, came out flat green. The fill is noted by the draw; the next pass that draws into the 8-bit
  // color target of that base and pitch first gets the depth buffer's bytes (the pass of TextureResolvedBytes:
  // red = stencil, then the 24-bit depth from its lowest byte, which is the EDRAM word).
  // A fill reaches the rectangle its draw covered, not the whole target: Direct3D clears the tile-aligned part of
  // a target through the depth buffer and the rest with ordinary rectangles (the design creator's livery passes).
  void NoteFillDepth(uint32_t base, uint32_t format, uint32_t pitch, int32_t x0, int32_t y0, int32_t x1,
                     int32_t y1, bool word_known, uint32_t word) override {
    if (!REXCVAR_GET(fh1_native_depth_fill_color)) {
      return;
    }
    const auto [it, first] = fills_depth_.try_emplace((uint64_t(base) << 20) | pitch,
                                                      FillDepth{format, x0, y0, x1, y1, word_known, word});
    if (!first) {
      // Two fills of one target with different words: the bytes come from the depth image, which has both.
      it->second.word_known = it->second.word_known && word_known && it->second.word == word;
      it->second.format = format;
      it->second.x0 = std::min(it->second.x0, x0);
      it->second.y0 = std::min(it->second.y0, y0);
      it->second.x1 = std::max(it->second.x1, x1);
      it->second.y1 = std::max(it->second.y1, y1);
    }
  }

  void ApplyFillDepth(Image& color, uint32_t base, uint32_t pitch) {
    const auto it = fills_depth_.find((uint64_t(base) << 20) | pitch);
    if (it == fills_depth_.end()) {
      return;
    }
    const FillDepth fill = it->second;
    const uint32_t format_depth = fill.format;
    fills_depth_.erase(it);
    if (color.format != kFormatColor || !DepthBytesReady()) {
      return;
    }
    VkRect2D area;
    area.offset = {std::clamp(fill.x0, 0, int32_t(color.width)), std::clamp(fill.y0, 0, int32_t(color.height))};
    area.extent = {uint32_t(std::clamp(fill.x1, area.offset.x, int32_t(color.width)) - area.offset.x),
                   uint32_t(std::clamp(fill.y1, area.offset.y, int32_t(color.height)) - area.offset.y)};
    if (!area.extent.width || !area.extent.height) {
      return;
    }
    const DepthBytesSource* source = nullptr;
    if (!fill.word_known) {
      const Image* depth = GetDepth(base, format_depth, pitch);
      if (!depth || !depth->prepared || depth->width != color.width || depth->height != color.height) {
        return;
      }
      source = DepthBytesSourceOf(depth->image);
      if (!source) {
        return;
      }
    }
    if (!Record()) {
      return;
    }
    VkFramebuffer& framebuffer = fills_depth_framebuffers_[color.image];
    if (framebuffer == VK_NULL_HANDLE) {
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = depth_bytes_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &color.view;
      info.width = color.width;
      info.height = color.height;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
        framebuffer = VK_NULL_HANDLE;
        return;
      }
      REXLOG_INFO("[fh1] color target base {:03X} pitch {} is filled through the depth buffer on the same EDRAM: "
                  "its bytes are written before the next draw into it",
                  base, pitch);
    }
    if (draws_) {
      draws_->FinishPass();
    }
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = depth_bytes_pass_;
    begin.framebuffer = framebuffer;
    begin.renderArea = area;  // the pass does not load: what is outside the render area stays as it is
    dfn_.vkCmdBeginRenderPass(commands_work_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(color.width), float(color.height), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(commands_work_, 0, 1, &viewport);
    dfn_.vkCmdSetScissor(commands_work_, 0, 1, &area);
    if (fill.word_known) {
      // The EDRAM word from its lowest byte: red = stencil, then the depth's three bytes.
      VkClearAttachment clear{};
      clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      for (uint32_t j = 0; j < 4; ++j) {
        clear.clearValue.color.float32[j] = float((fill.word >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
      }
      const VkClearRect rect{area, 0, 1};
      dfn_.vkCmdClearAttachments(commands_work_, 1, &clear, 1, &rect);
    } else {
      dfn_.vkCmdBindPipeline(commands_work_, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_bytes_pipeline_);
      dfn_.vkCmdBindDescriptorSets(commands_work_, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_bytes_layout_, 0, 1,
                                   &source->set, 0, nullptr);
      const uint32_t float24 = format_depth == 1 ? 1 : 0;  // kD24FS8
      dfn_.vkCmdPushConstants(commands_work_, depth_bytes_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float24),
                              &float24);
      dfn_.vkCmdDraw(commands_work_, 3, 1, 0, 0);
    }
    dfn_.vkCmdEndRenderPass(commands_work_);
    BarrierGlobal(commands_work_);
    if (draws_) {
      draws_->ForgetStateBound();
    }
    color.content_invalid = false;
    state_target_[&color].clear_clean = false;  // no longer the color of its last clear
    ++fills_depth_applied_;
  }

  ImageNative* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) override {
    Image* image = GetDepth(base, format, pitch);
    if (!image || !Record()) {
      return nullptr;
    }
    Prepare(*image);
    BeforeOfWriteDepth(*image);  // fh1_native_lazy_depth
    // A pass that draws on top starts here, so if the content went away in a swap and nobody has cleared it
    // since then, it has to be brought back.
    RestoreContent(*image);
    return image;
  }

  const ImageNative* TextureResolved(uint32_t address) override {
    const auto it = resolved_.find(address);
    // Watchdog of fh1_native_restore_by_swap. This is the only door through which draws reach a
    // resolved texture, so if someone requests one that is lent out (its content was given back to the render
    // target and it has not been resolved again) the lending is not safe and the previous frame's image would
    // be sampled. It must stay at 0 in the log.
    if (!lent_.empty() && lent_.count(address)) {
      ++lent_read_;
    }
    if (it == resolved_.end()) {
      return nullptr;
    }
    // FH1: the game wrote its own data at this address after resolving there (StampMemory): read it from memory.
    if (it->second.stamp_checked != presentations_stamp_) {
      it->second.stamp_checked = presentations_stamp_;
      if (!it->second.overwritten &&
          StampMemory(address, it->second.image.width, it->second.image.height,
                      BytesTexelGuest(it->second.format_guest)) != it->second.stamp_memory) {
        it->second.overwritten = true;
        REXLOG_INFO("[native] C2: the game wrote over the resolved texture at {:08X} ({}x{}): read from memory "
                    "until the next resolve",
                    address, it->second.image.width, it->second.image.height);
      }
    }
    if (it->second.overwritten) {
      return nullptr;
    }
    // And how many times each address is requested, for the per-render-target copy report. A resolved
    // texture that is copied every frame and never requested is a copy that is not needed. It is a ++ on the
    // entry that has already been looked up: it costs nothing even though this is called thousands of times
    // per frame.
    ++it->second.reads;
    if (address == fh1::reflection_demand::kAddress) {
      fh1::reflection_demand::NoteRead();  // fh1_reflection_low_demand
    }
    // fh1_native_lazy_depth. Who requests each depth texture; if its copy is deferred and this is
    // a real sampling, it is recorded now, before this draw.
    if (it->second.image.format == format_depth_) {
      NoteReadDepth(address);
    } else if (!front_presented_.empty() && front_presented_.count(address)) {
      NoteReadFront(address);  // fh1_native_lazy_front
    }
    return it->second.image.prepared ? &it->second.image : nullptr;
  }

  // --- FH1: a resolved depth fetched as k_8_8_8_8 ---------------------------------------------------------------
  // FH1's motion-blur velocity and depth-of-field shaders fetch the resolved scene depth as a color texture and
  // rebuild the 24-bit depth from three of its bytes; the fourth is the stencil, which picks the previous-frame
  // matrix of the object under the pixel. On the console the resolve writes those bytes to memory. Here a resolved
  // depth is a host depth image, and sampled as color it gave the depth value in every channel: the velocity came
  // out wrong everywhere (the whole scene smeared with the car standing still, speckles on every edge).
  // The bytes are written into an 8-bit color image by a small pass (shaders/fh1_depth_pack.hlsl), once per
  // resolve and only when a draw asks for them. nullptr if it cannot be done: the caller samples the depth as before.
  // FH1: a resolve only says the pitch of its texture (a multiple of 32 texels), not its width. The game's small
  // pictures are narrower: the reflection cube map's levels from 16x16 down to 1x1, the smallest bloom levels.
  // Kept in an image as wide as the pitch, a fetch with coordinates 0..1 read the empty columns too: each smaller
  // cube level came out darker than the one before, down to black (cars lost their reflections whenever they
  // sampled those levels). The fetch constant has the real width: the first draw that fetches such a texture
  // notes it here and the next resolve to that address creates the image that wide.
  void HintWidthResolved(uint32_t address, uint32_t width) override {
    const uint64_t key = (uint64_t((width + 31) & ~31u) << 32) | address;
    auto [it, fresh] = width_hint_.try_emplace(key, width);
    if (fresh && width_hint_.size() <= 64) {
      REXLOG_INFO("[fh1] resolved texture at {:08X} is fetched {} wide (pitch {}): its image takes that width",
                  address, width, (width + 31) & ~31u);
    }
    it->second = width;
  }
  std::unordered_map<uint64_t, uint32_t> width_hint_;

  const ImageNative* TextureResolvedBytes(uint32_t address) override {
    const auto it = resolved_.find(address);
    if (it == resolved_.end() || !it->second.image.prepared || it->second.image.format != format_depth_ ||
        !DepthBytesReady()) {
      return nullptr;
    }
    const Image& depth = it->second.image;
    DepthBytes& bytes = depth_bytes_[address];
    if (bytes.image.image != VK_NULL_HANDLE &&
        (bytes.image.width != depth.width || bytes.image.height != depth.height)) {
      SendWork(true);  // another size at that address: the old image may still be in use
      WaitGpu();
      DestroyDepthBytes(bytes);
    }
    if (bytes.image.image == VK_NULL_HANDLE) {
      if (!Create(bytes.image, depth.width, depth.height,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        return nullptr;
      }
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = depth_bytes_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &bytes.image.view;
      info.width = depth.width;
      info.height = depth.height;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &bytes.framebuffer) != VK_SUCCESS) {
        bytes.framebuffer = VK_NULL_HANDLE;
        DestroyDepthBytes(bytes);
        return nullptr;
      }
      bytes.dirty = true;
      REXLOG_INFO("[fh1] resolved depth at {:08X} ({}x{}) is fetched as a color texture: its bytes are written "
                  "after each resolve",
                  address, depth.width, depth.height);
    }
    if (!bytes.dirty) {
      return &bytes.image;
    }
    const DepthBytesSource* source = DepthBytesSourceOf(depth.image);
    if (!source || !Record()) {
      return nullptr;
    }
    if (draws_) {
      draws_->FinishPass();  // it arrives from a draw: the pass goes outside the draw's own
    }
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    if (!bytes.image.prepared) {
      VkImageMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = bytes.image.image;
      barrier.subresourceRange = kRangeColor;
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      dfn_.vkCmdPipelineBarrier(commands_work_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      bytes.image.prepared = true;
    }
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = depth_bytes_pass_;
    begin.framebuffer = bytes.framebuffer;
    begin.renderArea = {{0, 0}, {depth.width, depth.height}};
    dfn_.vkCmdBeginRenderPass(commands_work_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(depth.width), float(depth.height), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(commands_work_, 0, 1, &viewport);
    dfn_.vkCmdSetScissor(commands_work_, 0, 1, &begin.renderArea);
    dfn_.vkCmdBindPipeline(commands_work_, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_bytes_pipeline_);
    dfn_.vkCmdBindDescriptorSets(commands_work_, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_bytes_layout_, 0, 1,
                                 &source->set, 0, nullptr);
    const uint32_t float24 = it->second.format_guest == 23 ? 1 : 0;  // k_24_8_FLOAT
    dfn_.vkCmdPushConstants(commands_work_, depth_bytes_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float24),
                            &float24);
    dfn_.vkCmdDraw(commands_work_, 3, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(commands_work_);
    BarrierGlobal(commands_work_);
    if (draws_) {
      draws_->ForgetStateBound();
    }
    bytes.dirty = false;
    ++depth_bytes_passes_;
    return &bytes.image;
  }

  struct DepthBytes {
    Image image;  // 8-bit color, the size of the resolved depth
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    bool dirty = true;  // the depth was resolved again since the bytes were written
  };
  // The depth and the stencil of one host depth image as sampled images. Kept per image: a resolve that swaps
  // images (fh1_native_resolve_without_copy) alternates between two of them.
  struct DepthBytesSource {
    VkImageView depth = VK_NULL_HANDLE;
    VkImageView stencil = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };

  // The pass, the pipeline and the descriptor pool, created at the first request. false if they cannot be.
  bool DepthBytesReady() {
    if (depth_bytes_pipeline_ != VK_NULL_HANDLE) {
      return true;
    }
    if (depth_bytes_failed_ || !REXCVAR_GET(fh1_native_depth_bytes)) {
      return false;
    }
    depth_bytes_failed_ = true;  // until everything below is created
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_set{};
    info_set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_set.bindingCount = 2;
    info_set.pBindings = bindings;
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info_layout.setLayoutCount = 1;
    info_layout.pSetLayouts = &depth_bytes_set_layout_;
    info_layout.pushConstantRangeCount = 1;
    info_layout.pPushConstantRanges = &range;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kDepthBytesSources};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.maxSets = kDepthBytesSources;
    info_pool.poolSizeCount = 1;
    info_pool.pPoolSizes = &size;
    VkAttachmentDescription attachment{};
    attachment.format = kFormatColor;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // the pass writes every texel
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info_pass{};
    info_pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info_pass.attachmentCount = 1;
    info_pass.pAttachments = &attachment;
    info_pass.subpassCount = 1;
    info_pass.pSubpasses = &subpass;
    VkShaderModuleCreateInfo info_vs{};
    info_vs.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info_vs.codeSize = sizeof(fh1::kDepthPackVsSpirv);
    info_vs.pCode = fh1::kDepthPackVsSpirv;
    VkShaderModuleCreateInfo info_ps = info_vs;
    info_ps.codeSize = sizeof(fh1::kDepthPackPsSpirv);
    info_ps.pCode = fh1::kDepthPackPsSpirv;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_set, nullptr, &depth_bytes_set_layout_) != VK_SUCCESS ||
        dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &depth_bytes_layout_) != VK_SUCCESS ||
        dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &depth_bytes_pool_) != VK_SUCCESS ||
        dfn_.vkCreateRenderPass(device_, &info_pass, nullptr, &depth_bytes_pass_) != VK_SUCCESS ||
        dfn_.vkCreateShaderModule(device_, &info_vs, nullptr, &depth_bytes_vs_) != VK_SUCCESS ||
        dfn_.vkCreateShaderModule(device_, &info_ps, nullptr, &depth_bytes_ps_) != VK_SUCCESS) {
      REXLOG_ERROR("[fh1] could not create the objects of the depth-as-bytes pass: depth fetched as color stays wrong");
      return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = depth_bytes_vs_;
    stages[0].pName = "VsMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = depth_bytes_ps_;
    stages[1].pName = "PsMain";
    VkPipelineVertexInputStateCreateInfo entry{};
    entry.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo sampling{};
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_2[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_2;
    VkGraphicsPipelineCreateInfo info_pipeline{};
    info_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info_pipeline.stageCount = 2;
    info_pipeline.pStages = stages;
    info_pipeline.pVertexInputState = &entry;
    info_pipeline.pInputAssemblyState = &assembly;
    info_pipeline.pViewportState = &viewport_state;
    info_pipeline.pRasterizationState = &rasterization;
    info_pipeline.pMultisampleState = &sampling;
    info_pipeline.pColorBlendState = &blend;
    info_pipeline.pDynamicState = &dynamic;
    info_pipeline.layout = depth_bytes_layout_;
    info_pipeline.renderPass = depth_bytes_pass_;
    info_pipeline.basePipelineIndex = -1;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info_pipeline, nullptr,
                                       &depth_bytes_pipeline_) != VK_SUCCESS) {
      depth_bytes_pipeline_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[fh1] could not create the pipeline of the depth-as-bytes pass: depth fetched as color stays wrong");
      return false;
    }
    depth_bytes_failed_ = false;
    return true;
  }

  const DepthBytesSource* DepthBytesSourceOf(VkImage image) {
    if (const auto it = depth_bytes_sources_.find(image); it != depth_bytes_sources_.end()) {
      return &it->second;
    }
    if (depth_bytes_sources_.size() >= kDepthBytesSources) {
      return nullptr;
    }
    DepthBytesSource source;
    VkImageViewCreateInfo info_view{};
    info_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info_view.image = image;
    info_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info_view.format = format_depth_;
    info_view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorPool = depth_bytes_pool_;
    reserve.descriptorSetCount = 1;
    reserve.pSetLayouts = &depth_bytes_set_layout_;
    bool created = dfn_.vkCreateImageView(device_, &info_view, nullptr, &source.depth) == VK_SUCCESS;
    info_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    created = created && dfn_.vkCreateImageView(device_, &info_view, nullptr, &source.stencil) == VK_SUCCESS;
    if (created && !depth_bytes_sets_free_.empty()) {  // the set of a destroyed image
      source.set = depth_bytes_sets_free_.back();
      depth_bytes_sets_free_.pop_back();
    } else {
      created = created && dfn_.vkAllocateDescriptorSets(device_, &reserve, &source.set) == VK_SUCCESS;
    }
    if (!created) {
      if (source.depth != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, source.depth, nullptr);
      if (source.stencil != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, source.stencil, nullptr);
      return nullptr;
    }
    const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, source.depth, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, source.stencil, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = source.set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[i].pImageInfo = &images[i];
    }
    dfn_.vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    return &depth_bytes_sources_.emplace(image, source).first->second;
  }

  // The image is about to be destroyed (Destroy, with the GPU idle for it): its views go first.
  void ForgetDepthBytesSource(VkImage image) {
    if (depth_bytes_sources_.empty() || image == VK_NULL_HANDLE) {
      return;
    }
    const auto it = depth_bytes_sources_.find(image);
    if (it == depth_bytes_sources_.end()) {
      return;
    }
    depth_bytes_sets_free_.push_back(it->second.set);
    dfn_.vkDestroyImageView(device_, it->second.depth, nullptr);
    dfn_.vkDestroyImageView(device_, it->second.stencil, nullptr);
    depth_bytes_sources_.erase(it);
  }

  void DestroyDepthBytes(DepthBytes& bytes) {
    if (bytes.framebuffer != VK_NULL_HANDLE) dfn_.vkDestroyFramebuffer(device_, bytes.framebuffer, nullptr);
    bytes.framebuffer = VK_NULL_HANDLE;
    if (bytes.image.image != VK_NULL_HANDLE) {
      if (draws_) {
        draws_->ForgetImage(bytes.image.image);
      }
      Destroy(bytes.image);
    }
    bytes.dirty = true;
  }

  // Shutdown, with the GPU idle.
  void DestroyDepthBytes() {
    for (auto& [address, bytes] : depth_bytes_) {
      DestroyDepthBytes(bytes);
    }
    depth_bytes_.clear();
    for (auto& [image, source] : depth_bytes_sources_) {
      dfn_.vkDestroyImageView(device_, source.depth, nullptr);
      dfn_.vkDestroyImageView(device_, source.stencil, nullptr);
    }
    depth_bytes_sources_.clear();
    for (auto& [image, framebuffer] : fills_depth_framebuffers_) {
      if (framebuffer != VK_NULL_HANDLE) dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    fills_depth_framebuffers_.clear();
    if (depth_bytes_pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, depth_bytes_pipeline_, nullptr);
    if (depth_bytes_vs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, depth_bytes_vs_, nullptr);
    if (depth_bytes_ps_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, depth_bytes_ps_, nullptr);
    if (depth_bytes_pass_ != VK_NULL_HANDLE) dfn_.vkDestroyRenderPass(device_, depth_bytes_pass_, nullptr);
    if (depth_bytes_pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, depth_bytes_pool_, nullptr);
    if (depth_bytes_layout_ != VK_NULL_HANDLE) dfn_.vkDestroyPipelineLayout(device_, depth_bytes_layout_, nullptr);
    if (depth_bytes_set_layout_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorSetLayout(device_, depth_bytes_set_layout_, nullptr);
    depth_bytes_pipeline_ = VK_NULL_HANDLE;
  }

  static constexpr uint32_t kDepthBytesSources = 64;
  std::unordered_map<uint32_t, DepthBytes> depth_bytes_;  // by the address of the resolved depth
  std::unordered_map<VkImage, DepthBytesSource> depth_bytes_sources_;
  std::vector<VkDescriptorSet> depth_bytes_sets_free_;
  VkDescriptorSetLayout depth_bytes_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout depth_bytes_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool depth_bytes_pool_ = VK_NULL_HANDLE;
  VkRenderPass depth_bytes_pass_ = VK_NULL_HANDLE;
  VkShaderModule depth_bytes_vs_ = VK_NULL_HANDLE;
  VkShaderModule depth_bytes_ps_ = VK_NULL_HANDLE;
  VkPipeline depth_bytes_pipeline_ = VK_NULL_HANDLE;
  bool depth_bytes_failed_ = false;
  uint64_t depth_bytes_passes_ = 0;
  // fh1_native_depth_fill_color: depth buffers filled by a depth-only rectangle and not yet copied to the color
  // target on their EDRAM (key = base << 20 | pitch, value = depth format), and the framebuffers of those targets.
  struct FillDepth {
    uint32_t format;
    int32_t x0, y0, x1, y1;  // in the pixels of the image
    bool word_known;         // the EDRAM word is known from the draw (one Z, stencil replaced)
    uint32_t word;           // depth << 8 | stencil
  };
  std::unordered_map<uint64_t, FillDepth> fills_depth_;
  std::unordered_map<VkImage, VkFramebuffer> fills_depth_framebuffers_;
  uint64_t fills_depth_applied_ = 0;

  // fh1_native_lazy_depth. DrawsVulkan sets it around the textures of the final composition
  // when its depth sampling is dead (no blur).
  void ReadsOfDepthDead(bool dead) override { reads_depth_dead_ = dead; }

  // fh1_native_shadow_minimum, what the draws ask (see the cvar's comment).
  // When a pass is opened on this depth: if it is the car pass of the current cycle, each draw is validated.
  bool PassOfCarsShadow(const ImageNative* depth, bool only_depth) override {
    if (!depth || depth != sm_target_cycle_ || sm_state_ != kSmCars) {
      return false;
    }
    if (!only_depth) {
      ShadowMinimumMissCycle("the car pass has a color target", 0, true);
    }
    return true;
  }

  // A draw of the car pass. `exact`: it leaves in the depth the minimum of what was there and of its
  // fragments.
  void DrawOfCarsShadow(bool exact_2, uint32_t control_z) override {
    ++sm_draws_cars_;
    if (!exact_2) {
      ++sm_draws_no_exact_;
      ShadowMinimumMissCycle("a car draw does not keep the minimum (the clue is its Z control)", control_z, true);
    }
  }

  // The texture with cars that the draws must look at: whenever it holds the cars only, and otherwise while
  // watching or applying. 0 = none.
  uint32_t AddressShadowCars() const override {
    return (sm_virtual_ || (sm_on_ && sm_phase_ != kSmOff)) ? sm_cars_ : 0;
  }

  // A draw samples the texture with cars. `capable`: its pixel shader has tfetch2DShadowMin in that register
  // (`ps` is its number, for the log). Returns the partner to take the minimum with, or nullptr to sample
  // it as is.
  const ImageNative* PartnerShadowCars(bool capable, uint32_t ps) override {
    if (sm_virtual_) {
      // It holds the cars only: without the minimum, the world would miss its shadows.
      const auto it = resolved_.find(sm_world_);
      const bool world = it != resolved_.end() && it->second.image.prepared;
      if (!sm_virtual_valid_ || !capable || !world) {
        ++sm_reads_unable_;
        ShadowMinimumDifference(!sm_virtual_valid_ ? "the cars-only texture is sampled and the world one is "
                                                     "already from another cycle (the clue is the pixel shader)"
                               : !capable ? "a pixel shader without tfetch2DShadowMin samples the cars-only "
                                            "texture (the clue is its number)"
                                        : "the world texture is not there (the clue is the pixel shader)",
                               ps);
        return nullptr;
      }
      ++sm_reads_minimum_;
      return &it->second.image;
    }
    // It is the usual one (world and cars): who reads it and when is watched, so the minimum can be applied
    // without surprises.
    if (sm_state_ != kSmFree) {
      ++sm_reads_a_out_of_time_;
      ShadowMinimumNoFit("the car texture is sampled between the two resolves (the clue is the pixel shader)",
                         ps);
    }
    if (!capable) {
      ++sm_reads_unable_;
      ShadowMinimumNoFit("a pixel shader without tfetch2DShadowMin samples the car texture: a library without the "
                         "minimum? (the clue is its number)",
                         ps);
      return nullptr;
    }
    const auto it = resolved_.find(sm_cars_);
    if (!sm_on_ || sm_phase_ == kSmOff || it == resolved_.end() || !it->second.image.prepared) {
      ++sm_reads_normals_;
      return nullptr;
    }
    // Minimum with itself (the same texel): the pipeline with the bit already exists when applying starts.
    ++sm_reads_si_same_;
    return &it->second.image;
  }

  // fh1_native_diag_clears. What is known about the clears of a render target.
  struct UseClear {
    uint32_t base = 0;             // EDRAM base, guest format and pitch (for the report)
    uint32_t format = 0;
    uint32_t pitch = 0;
    bool depth = false;
    bool in_pass = false;          // cleared by opening a pass (depth without TRANSFER_DST: ZCULL)
    bool open = false;          // there is a clear whose use is being measured
    uint32_t used_width = 0;      // since the last clear: the largest x1 and y1 used
    uint32_t used_height = 0;
    uint32_t max_width = 0;        // the same since startup (only grows)
    uint32_t max_height = 0;
    uint64_t cycles = 0;           // clears with their use already measured, since startup
    uint64_t clears = 0;         // in the report interval
    uint64_t pixels_clears = 0;
    uint64_t pixels_used = 0;   // of the cycles closed in the interval
    // fh1_native_clear_useful_area. The bottom band the last clear left uncleared.
    bool band = false;
    uint32_t band_since = 0;
    VkImage band_image = VK_NULL_HANDLE;
    VkClearColorValue band_color{};
    uint32_t bands_completed = 0;
    bool area_useful_off = false;  // its useful area is not stable: cleared entirely
  };

  // What is drawn in a pass on that render target (its renderArea). Called by DrawsVulkan in BeginPass.
  void NoteAreaOfPass(const ImageNative* image, uint32_t width, uint32_t height) override {
    if (image) {
      NoteUseClear(*image, width, height);
    }
  }

  // A whole clear of that image. Closes the previous cycle (what has been used since the previous clear) and
  // opens another.
  void NoteClearDiag(const Image& image, uint32_t base, uint32_t format, uint32_t pitch, bool depth,
                         bool in_pass) {
    if (!diag_clears_) {
      return;
    }
    UseClear& u = use_clears_[&image];
    if (u.open) {
      u.pixels_used += uint64_t(u.used_width) * u.used_height;
      ++u.cycles;
    }
    u.base = base;
    u.format = format;
    u.pitch = pitch;
    u.depth = depth;
    u.in_pass = in_pass;
    ++u.clears;
    u.pixels_clears += uint64_t(image.width) * image.height;
    u.used_width = 0;
    u.used_height = 0;
    u.open = true;
  }

  // The rectangle from 0 to width and from 0 to height of that image is used (a pass, a resolve, a restore
  // or a swap). It only counts on render targets that have been cleared at least once.
  void NoteUseClear(const Image& image, uint32_t width, uint32_t height) {
    // fh1_native_clear_useful_area. While any band remains uncleared it is checked even if the diagnostic
    // was turned off at run time: a pending band is always completed before it is used.
    if (use_clears_.empty() || (!diag_clears_ && !bands_pending_)) {
      return;
    }
    const auto it = use_clears_.find(&image);
    if (it == use_clears_.end()) {
      return;
    }
    UseClear& u = it->second;
    width = std::min(width, image.width);
    height = std::min(height, image.height);
    // fh1_native_clear_useful_area. If this use reaches the band the last clear left uncleared (or its
    // first row, which a linear blit can read), it is cleared now, before the use: the result is the same
    // as if it had been cleared entirely.
    if (u.band && (u.band_image != image.image || height >= u.band_since)) {
      CompleteBand(image, u);
    }
    if (!diag_clears_) {
      return;  // it was only checked because of the band
    }
    u.used_width = std::max(u.used_width, width);
    u.used_height = std::max(u.used_height, height);
    u.max_width = std::max(u.max_width, width);
    u.max_height = std::max(u.max_height, height);
  }

  // fh1_native_clear_useful_area. Clears only the rows that render target uses, if they are already known.
  // Returns false if it has to be cleared entirely, as always.
  bool ClearColorAreaUseful(Image& target, const VkClearColorValue& color) {
    if (!clear_area_useful_ || !diag_clears_ || !draws_ || target.format != kFormatColor) {
      return false;
    }
    const auto it = use_clears_.find(&target);
    if (it == use_clears_.end()) {
      return false;
    }
    UseClear& u = it->second;
    if (u.area_useful_off || u.cycles < kAreaUsefulCycles || u.max_height == 0) {
      return false;
    }
    // Up to the multiple of 64 that leaves at least one row of margin below what is used: a linear blit (the
    // resolve that shrinks the scene) can read the row below its rectangle, and that row must be cleared.
    const uint32_t height = std::min(target.height, (u.max_height + 64u) & ~63u);
    if (height >= target.height) {
      return false;
    }
    if (!draws_->ClearColorInPass(commands_work_, target, color, VkRect2D{{0, 0}, {target.width, height}})) {
      return false;
    }
    if (!u.band) {
      ++bands_pending_;
    }
    u.band = true;
    u.band_since = height;
    u.band_image = target.image;
    u.band_color = color;
    ++area_useful_clears_;
    area_useful_pixels_ += uint64_t(target.width) * (target.height - height);
    return true;
  }

  // The render target has been cleared entirely: no band is left.
  void RemoveBand(const Image& target) {
    if (use_clears_.empty()) {
      return;
    }
    const auto it = use_clears_.find(&target);
    if (it != use_clears_.end() && it->second.band) {
      it->second.band = false;
      if (bands_pending_) {
        --bands_pending_;
      }
    }
  }

  // Clears the band that was left uncleared, before a use that reaches it (outside a pass: called by Copy,
  // RestoreContent, SwapWithResolved and BeginPass before opening its own).
  void CompleteBand(const Image& image, UseClear& u) {
    u.band = false;
    if (bands_pending_) {
      --bands_pending_;
    }
    ++u.bands_completed;
    ++area_useful_completed_;
    if (u.bands_completed >= 3) {
      u.area_useful_off = true;  // its useful area is not stable: from now on it is cleared entirely
    }
    const bool same_image = u.band_image == image.image;
    const uint32_t height_band = image.height > u.band_since ? image.height - u.band_since : 0;
    if (same_image && (!height_band || (draws_ && Record() &&
                                         draws_->ClearColorInPass(
                                             commands_work_, image, u.band_color,
                                             VkRect2D{{0, int32_t(u.band_since)}, {image.width, height_band}})))) {
      return;
    }
    // The image changed without going through a clear (should not happen) or the band could not be cleared.
    ++area_useful_misses_;
    if (!area_useful_global_off_) {
      area_useful_global_off_ = true;
      clear_area_useful_ = false;
      REXLOG_ERROR("[native] C2 clear useful area: DIFFERENCE, could not complete the {}x{} band from row {} ({}). "
                   "Off for the rest of the session: the whole image is cleared",
                   image.width, image.height, u.band_since, same_image ? "the pass failed" : "the image changed");
    }
  }

  // Every 20 s, per render target, what was cleared against what was used (C2 clears per target).
  // Clears cost ~0.075 ms real per Mpixel (measured, see "C2 skipped clears").
  void ReportClears() {
    diag_clears_ = REXCVAR_GET(fh1_native_diag_clears);
    clear_area_useful_ = REXCVAR_GET(fh1_native_clear_useful_area) && !area_useful_global_off_;
    const auto now = std::chrono::steady_clock::now();
    if (report_clears_ == std::chrono::steady_clock::time_point{}) {
      report_clears_ = now;
      presented_report_clears_ = presented_;
      return;
    }
    if (now - report_clears_ < std::chrono::seconds(20)) {
      return;
    }
    report_clears_ = now;
    const uint64_t frames = presented_ - presented_report_clears_;
    presented_report_clears_ = presented_;
    if (diag_clears_ && frames > 0 && !use_clears_.empty()) {
      struct Row {
        const UseClear* use;
        uint32_t width;
        uint32_t height;
        double clear;  // Mpixels per frame
        double used;
      };
      std::vector<Row> rows;
      double total_clear = 0.0, total_used = 0.0;
      for (const auto& [image, u] : use_clears_) {
        if (!u.clears) {
          continue;
        }
        const double clear = double(u.pixels_clears) / 1e6 / double(frames);
        // Each clear closes the previous one's cycle: in steady state, as many closed cycles as clears.
        const double used = std::min(clear, double(u.pixels_used) / 1e6 / double(frames));
        rows.push_back({&u, image->width, image->height, clear, used});
        total_clear += clear;
        total_used += used;
      }
      std::sort(rows.begin(), rows.end(),
                [](const Row& a, const Row& b) { return a.clear - a.used > b.clear - b.used; });
      std::string list;
      for (const Row& row : rows) {
        const UseClear& u = *row.use;
        list += fmt::format(" | {:03X}/{} {} {}x{}{}: {:.2f} per frame, {:.2f} Mpixels cleared, {:.2f} used (up to "
                            "{}x{})",
                             u.base, u.pitch, u.depth ? "depth" : "color", row.width, row.height,
                             u.in_pass ? " (per pass, ZCULL)" : "", double(u.clears) / double(frames),
                             row.clear, row.used, u.max_width, u.max_height);
      }
      FH1_REPORT_RING("[native] C2 clears per target (build 184, {} frames): {:.2f} Mpixels cleared and {:.2f} "
                       "used per frame (spare {:.2f}: ~{:.2f} real ms){}",
                           frames, total_clear, total_used, total_clear - total_used,
                           (total_clear - total_used) * 0.075, list);
    }
    if (area_useful_clears_ || area_useful_misses_) {  // fh1_native_clear_useful_area
      const double mp =
          double(area_useful_pixels_ - area_useful_pixels_previous_) / 1e6 / double(std::max<uint64_t>(frames, 1));
      FH1_REPORT_RING("[native] C2 clear useful area (build 184): {} color clears trimmed since startup; {:.2f} "
                       "Mpixels per frame not cleared (~{:.2f} real ms); {} bands completed before a use; {} "
                       "failures{}",
                           area_useful_clears_, mp, mp * 0.075, area_useful_completed_, area_useful_misses_,
                           area_useful_global_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the "
                                                                                         "same)");
      area_useful_pixels_previous_ = area_useful_pixels_;
    }
    for (auto& [image, u] : use_clears_) {
      u.clears = 0;
      u.pixels_clears = 0;
      u.pixels_used = 0;
    }
  }

  // A request for a resolved depth texture (see fh1_native_lazy_depth).
  void NoteReadDepth(uint32_t address) {
    if (reads_depth_dead_) {
      last_read_dead_[address] = presented_;
      if (!pending_.empty()) {
        const auto p = pending_.find(address);
        if (p != pending_.end()) {
          p->second.read_dead = true;
        }
      }
      return;
    }
    last_read_live_[address] = presented_;
    if (!pending_.empty() && pending_.count(address)) {
      RecordCopyPending(address);
      ++lazy_copied_read_;
    }
    if (!stale_.empty() && stale_.erase(address)) {
      ++lazy_reads_late_;
      if (!lazy_off_) {
        lazy_off_ = true;
        REXLOG_ERROR("[native] C2 lazy depth: DIFFERENCE, a draw samples {:08X} after its copy was dropped (late "
                     "read: it reads an old depth). Off for the rest of the session: always copied",
                     address);
      }
    }
  }

  // Records the deferred copy of that address, outside a pass, and removes it from the list.
  void RecordCopyPending(uint32_t address) {
    const auto p = pending_.find(address);
    if (p == pending_.end()) {
      return;
    }
    const CopyPending pending = p->second;
    pending_.erase(p);
    const auto r = resolved_.find(address);
    if (r == resolved_.end() || r->second.image.image != pending.target_vk || !pending.source ||
        pending.source->image != pending.source_vk || !copy_image_ || !Record()) {
      stale_.insert(address);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // it can arrive from a draw (TextureResolved): the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    CopyImages(commands_work_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.target_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    NoteCopy(pending.copy.extent.width, pending.copy.extent.height);
    ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
  }

  // Before any write to a depth render target. The deferred copies that come from it are recorded (same
  // frame, or the composition has not requested them yet) or dropped (nobody has sampled them).
  void BeforeOfWriteDepth(const Image& image) {
    if (pending_.empty() || image.image == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.source_vk != image.image) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      // Dropped even within the same frame if the composite without blur already requested it (read_dead).
      // Keeping same-frame copies saved nothing: the game always rewrites the source within the same frame,
      // and the reader watch saw 0 reads after the rewrite. The guard catches a late read.
      if (!it->second.read_dead) {
        ++it;  // RecordCopyPending erases that entry: the iterator is already on the next one
        RecordCopyPending(address);
        ++lazy_copied_write_;
      } else {
        lazy_pixels_saved_ += uint64_t(it->second.copy.extent.width) * it->second.copy.extent.height;
        it = pending_.erase(it);
        stale_.insert(address);
        ++lazy_dropped_;
      }
    }
  }

  // Defers the copy if, lately, the only reader of that address is the composite without blur.
  bool DeferCopyDepth(uint32_t address, const Image& source, const Resolved& resolved,
                               const VkImageCopy& copy) {
    if (lazy_off_ || !REXCVAR_GET(fh1_native_lazy_depth)) {
      return false;
    }
    const auto dead_2 = last_read_dead_.find(address);
    if (dead_2 == last_read_dead_.end() || presented_ - dead_2->second > kLazyFrames) {
      return false;  // the composite without blur has not requested it lately
    }
    const auto live = last_read_live_.find(address);
    if (live != last_read_live_.end() && presented_ - live->second <= kLazyFrames) {
      return false;  // something really samples it: copy as usual
    }
    CopyPending& pending = pending_[address];
    pending.source = &source;
    pending.source_vk = source.image;
    pending.target_vk = resolved.image.image;
    pending.copy = copy;
    pending.frame = presented_;
    pending.read_dead = false;
    ResolvedWritten(address);  // new contents (deferred): no longer lent or stale
    ++lazy_deferred_;
    return true;
  }

  // fh1_native_lazy_front. Deferred copy to a front buffer, and the render target's spare images.
  struct FrontPending {
    Image* target = nullptr;           // source render target (targets_ never erases: the pointer stays valid)
    VkImage source_vk = VK_NULL_HANDLE;  // image holding the content: the target's own or a retained one
    int32_t retained = -1;               // index in front_images_ if it is no longer in the target
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
  };
  struct ImageFront {
    Image image;              // same size, format and usage as the target: swapped with it
    uint32_t retained_by = 0;  // address of the front buffer that retains it; 0 = free
  };

  // Is this address a front buffer read only by the Swap? A Swap presented it recently and no draw has
  // sampled it in that time.
  bool IsFrontOnlyOfSwap(uint32_t address) const {
    const auto presented_3 = front_presented_.find(address);
    if (presented_3 == front_presented_.end() || presented_ - presented_3->second > kFrontFrames) {
      return false;
    }
    const auto read_2 = front_read_.find(address);
    return read_2 == front_read_.end() || presented_ - read_2->second > kFrontFrames;
  }

  // The retained image of a deferred copy becomes free again.
  void FreeRetained(const FrontPending& pending) {
    if (pending.retained >= 0 && size_t(pending.retained) < front_images_.size()) {
      front_images_[size_t(pending.retained)].retained_by = 0;
    }
  }

  // The image holding the deferred copy's content, if it is still the same one; otherwise nullptr.
  Image* SourceFront(const FrontPending& pending) {
    Image* source = nullptr;
    if (pending.retained >= 0) {
      if (size_t(pending.retained) < front_images_.size()) {
        source = &front_images_[size_t(pending.retained)].image;
      }
    } else {
      source = pending.target;
    }
    return source && source->image == pending.source_vk && source->prepared ? source : nullptr;
  }

  // Records the deferred copy to that front buffer (outside a pass) and removes it from the list.
  void RecordCopyFront(uint32_t address) {
    const auto p = front_pending_.find(address);
    if (p == front_pending_.end()) {
      return;
    }
    const FrontPending pending = p->second;
    front_pending_.erase(p);
    const auto r = resolved_.find(address);
    Image* const source = SourceFront(pending);
    if (r == resolved_.end() || r->second.image.image != pending.texture_vk || !source || !copy_image_ ||
        !Record()) {
      FreeRetained(pending);
      front_stale_.insert(address);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // may come from a draw (TextureResolved): the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    CopyImages(commands_work_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.texture_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    NoteCopy(pending.copy.extent.width, pending.copy.extent.height);
    ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
    front_stale_.erase(address);
    FreeRetained(pending);
  }

  // Another resolve (or an image swap) reaches that texture. If it covers the whole texture, the deferred
  // copy is unnecessary and is dropped: nobody will see its content. Otherwise it is recorded first.
  void ResolveFrontPrevious(uint32_t address, bool whole) {
    if (whole && !front_stale_.empty()) {
      front_stale_.erase(address);  // fully covered: it is no longer missing any copy
    }
    if (front_pending_.empty()) {
      return;
    }
    const auto p = front_pending_.find(address);
    if (p == front_pending_.end()) {
      return;
    }
    if (!whole) {
      RecordCopyFront(address);
      ++front_copied_write_;
      return;
    }
    front_pixels_saved_ += uint64_t(p->second.copy.extent.width) * p->second.copy.extent.height;
    FreeRetained(p->second);
    front_pending_.erase(p);
    ++front_replaced_;
  }

  // Defers the copy if it is 1:1, covers the whole texture from the target's corner, and that address is
  // read only by the Swap.
  bool DeferCopyFront(uint32_t address, Image& target, const Resolved& resolved, const VkImageCopy& copy) {
    if (front_off_ || !REXCVAR_GET(fh1_native_lazy_front) || REXCVAR_GET(fh1_native_diag_resolved)) {
      return false;
    }
    if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
        copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
        target.format != kFormatColor || resolved.image.format != kFormatColor) {
      return false;
    }
    // Small ones are read back for the guest (ReadResolved): those are never deferred.
    const int32_t texels_read = REXCVAR_GET(fh1_native_read_resolved_texels);
    if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
      return false;
    }
    if (!IsFrontOnlyOfSwap(address)) {
      return false;
    }
    FrontPending& pending = front_pending_[address];
    pending.target = &target;
    pending.source_vk = target.image;
    pending.retained = -1;
    pending.texture_vk = resolved.image.image;
    pending.copy = copy;
    ResolvedWritten(address);  // new contents (deferred)
    front_stale_.erase(address);
    ++front_deferred_;
    return true;
  }

  // Before writing to a color target in any way other than a full clear (a pass, a restore, an image
  // swap): deferred copies whose source is its image are recorded now (exact).
  void BeforeOfWriteColor(const Image& target) {
    if (front_pending_.empty() || target.image == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = front_pending_.begin(); it != front_pending_.end();) {
      if (it->second.retained >= 0 || it->second.source_vk != target.image) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      ++it;  // RecordCopyFront erases that entry: the iterator is already on the next one
      RecordCopyFront(address);
      ++front_copied_write_;
    }
  }

  // Before a full clear of a color target. If its image is the source of a deferred front buffer, the
  // target takes a free spare image of the same size and the one holding the content is retained for the
  // front buffer: the clear does not need the old content, so nothing is copied. Without a spare (or with
  // two front buffers from the same image), the copy is recorded first, as usual.
  void RotateFrontBeforeOfClear(Image& target) {
    if (front_pending_.empty() || target.image == VK_NULL_HANDLE) {
      return;
    }
    uint32_t base = 0;
    uint32_t how_many = 0;
    for (const auto& [address, pending] : front_pending_) {
      if (pending.retained < 0 && pending.source_vk == target.image) {
        base = address;
        ++how_many;
      }
    }
    if (how_many == 0) {
      return;
    }
    int32_t free = -1;
    if (how_many == 1) {
      for (size_t i = 0; i < front_images_.size(); ++i) {
        const Image& candidate_2 = front_images_[i].image;
        if (front_images_[i].retained_by == 0 && candidate_2.image != VK_NULL_HANDLE &&
            candidate_2.width == target.width && candidate_2.height == target.height &&
            candidate_2.format == target.format) {
          free = int32_t(i);
          break;
        }
      }
      if (free < 0 && front_images_.size() < kFrontImagesMax) {
        ImageFront new_entry;
        // Same usage flags as GetTarget: the image becomes the render target.
        if (Create(new_entry.image, target.width, target.height,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  target.format)) {
          Prepare(new_entry.image);
          front_images_.push_back(new_entry);
          free = int32_t(front_images_.size()) - 1;
          REXLOG_INFO("[native] C2 lazy front buffer: spare image {} of {}x{} for the front buffer's target",
                      front_images_.size(), target.width, target.height);
        }
      }
    }
    if (free < 0) {
      ++front_without_spare_;
      BeforeOfWriteColor(target);  // the copy is recorded before the clear, as usual
      return;
    }
    Image& spare = front_images_[size_t(free)].image;
    std::swap(target.image, spare.image);
    std::swap(target.memory_block, spare.memory_block);
    std::swap(target.view, spare.view);
    std::swap(target.view_srgb, spare.view_srgb);
    std::swap(target.prepared, spare.prepared);
    front_images_[size_t(free)].retained_by = base;
    front_pending_[base].retained = free;  // source_vk is still the content image, now retained
    if (draws_) {
      draws_->InvalidateImages(target.image, spare.image);
    }
    ++front_rotations_;
  }

  // A draw samples a front buffer. If its copy is deferred, it is recorded now (before the draw).
  void NoteReadFront(uint32_t address) {
    front_read_[address] = presented_;
    if (!front_pending_.empty() && front_pending_.count(address)) {
      RecordCopyFront(address);
      ++front_copied_read_;
    }
    if (!front_stale_.empty() && front_stale_.erase(address)) {
      ++front_reads_late_;
      if (!front_off_) {
        front_off_ = true;
        REXLOG_ERROR("[native] C2 lazy front buffer: DIFFERENCE, a draw samples front buffer {:08X} without its "
                     "last copy. Off for the rest of the session: always copied",
                     address);
      }
    }
  }

  // The Swap of that front buffer, before the submission. Returns the image that can be presented without
  // a copy (the target's or the retained one, with the front buffer in its corner) or nullptr: in that
  // case any deferred copy is recorded here (in the same submission, ahead of the output) and the output
  // reads the texture as usual.
  Image* FrontToPresent(uint32_t address, uint32_t width, uint32_t height) {
    front_presented_[address] = presented_;
    if (!front_stale_.empty() && front_stale_.erase(address)) {
      ++front_reads_late_;
      if (!front_off_) {
        front_off_ = true;
        REXLOG_ERROR("[native] C2 lazy front buffer: DIFFERENCE, the Swap paints front buffer {:08X} without its "
                     "last copy. Off for the rest of the session: always copied",
                     address);
      }
    }
    if (front_pending_.empty()) {
      return nullptr;
    }
    if (REXCVAR_GET(fh1_native_diag_resolved)) {
      // The diagnostic grid shows every resolved texture: if it is enabled at runtime, no copy stays deferred.
      while (!front_pending_.empty()) {
        RecordCopyFront(front_pending_.begin()->first);  // removes it from the list, whether it succeeds or not
        ++front_copied_swap_;
      }
      return nullptr;
    }
    const auto p = front_pending_.find(address);
    if (p == front_pending_.end()) {
      return nullptr;
    }
    const auto r = resolved_.find(address);
    Image* const source = SourceFront(p->second);
    bool can = source && r != resolved_.end() && r->second.image.image == p->second.texture_vk &&
                 r->second.image.prepared && ramp_gamma_ && !fh1::settings::AntialiasingFxaa() &&
                 !front_off_;
    if (can) {
      // Same computation as Present: the output has the texture's size (exact variant) and the copy
      // covers it.
      const Image& texture = r->second.image;
      const uint32_t w = std::min(width ? width : texture.width, texture.width);
      const uint32_t h = std::min(height ? height : texture.height, texture.height);
      can = w == texture.width && h == texture.height && p->second.copy.extent.width == w &&
              p->second.copy.extent.height == h;
    }
    if (!can) {
      RecordCopyFront(address);
      ++front_copied_swap_;
      return nullptr;
    }
    if (p->second.retained >= 0) {
      ++front_painted_retained_;
    } else {
      ++front_painted_target_;
    }
    return source;
  }

  // Upload buffer full: submit what has been recorded and continue in the other slot, with its empty
  // upload buffer (Record only waits if that slot's last submission is still on the GPU).
  bool SendYWait() override { return SendWork(false) && Record(); }

  // The function above submits and continues; this one waits. Same order as every other place that must
  // destroy something the GPU might still be reading (see the resolved texture that changes size).
  bool WaitGpuOfAll() override {
    WaitGpu();
    return Record();
  }

  // See fh1_native_targets.h. Only when a read-back of 32x32 texels or less is on its way: the 64x64 ones are the
  // exposure's, every frame, and the game takes those a frame late without harm; waiting for the GPU at every
  // interrupt would cost the frame rate.
  bool StartPrewarmEarly() override { return draws_ && draws_->StartPrewarmEarly(); }
  void ProgressPrewarm(uint32_t& done, uint32_t& total, bool& finished) const override {
    if (draws_) {
      draws_->ProgressPrewarm(done, total, finished);
    } else {
      done = 0;
      total = 0;
      finished = true;
    }
  }

  void FinishReads() override {
    if (!reads_urgent_) {
      return;
    }
    reads_urgent_ = false;
    WaitGpu();  // submits what is recorded and completes every slot: WriteReads puts them in guest memory
    for (const uint64_t key : reads_once_) {  // fh1_native_read_one_off: the GPU has nothing pending any more
      for (uint64_t slot = 0; slot < 2; ++slot) {
        const auto it = reads_.find(key ^ (slot << 63));
        if (it != reads_.end()) {
          DestroyRead(it->second);
          reads_.erase(it);
        }
      }
    }
    reads_once_.clear();
    if (reads_finished_++ < 8) {
      REXLOG_INFO("[fh1] read-backs of tiny resolved textures finished before the game's fence ({} so far)",
                  reads_finished_);
    }
  }
  bool reads_urgent_ = false;
  uint64_t reads_finished_ = 0;

  void MarkGpu(uint32_t category) override {
    if (queries_ == VK_NULL_HANDLE || !recording_) {
      return;
    }
    SlotWork& slot = slots_[slot_];
    if (slot.categories.size() + 1 >= kMarksBySlot ||
        (!slot.categories.empty() && slot.categories.back() == category)) {
      return;  // no room (the last one is for the end mark) or same category continues
    }
    write_mark_(commands_work_,
                    slot.marks_precise ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    queries_, slot_ * kMarksBySlot + uint32_t(slot.categories.size()));
    slot.categories.push_back(uint8_t(category));
  }

  // The game's occlusion queries (see fh1_native_targets.h).
  void BeginOcclusion(uint32_t base) override {
    if (occlusion_open_ != 0) {
      uint64_t discarded = 0;
      FinishOcclusion(0, discarded);
    }
    if (!draws_ || occlusions_ == VK_NULL_HANDLE) {
      return;
    }
    if (occlusions_game_.size() >= 256) {
      occlusions_game_.clear();  // spans that were never read (should not happen): leftovers are ignored
    }
    occlusion_open_ = next_occlusion_++;
    occlusions_game_[occlusion_open_].base = base;
    draws_->OcclusionOpen(true);
  }

  bool FinishOcclusion(uint32_t base, uint64_t& sample_total) override {
    if (occlusion_open_ != 0) {
      if (draws_) {
        draws_->OcclusionOpen(false);
      }
      const auto it = occlusions_game_.find(occlusion_open_);
      occlusion_open_ = 0;
      if (it != occlusions_game_.end()) {
        it->second.finished = true;
        if (it->second.ranges_pending == 0) {
          PublishOcclusion(it);  // no draws counted: 0 samples
        }
      }
    }
    const auto measurement = occlusion_by_base_.find(base);
    if (measurement == occlusion_by_base_.end()) {
      return false;
    }
    sample_total = measurement->second;
    return true;
  }

  void StatisticsOcclusion(uint64_t values[5]) const override {
    std::copy(std::begin(statistics_occlusion_), std::end(statistics_occlusion_), values);
  }

  uint32_t BeginQueryOcclusion() override {
    if (occlusions_ == VK_NULL_HANDLE || occlusion_open_ == 0 || !recording_) {
      return UINT32_MAX;
    }
    const auto it = occlusions_game_.find(occlusion_open_);
    if (it == occlusions_game_.end()) {
      return UINT32_MAX;
    }
    SlotWork& slot = slots_[slot_];
    if (slot.occlusions.size() >= kOcclusionsBySlot) {
      ++statistics_occlusion_[1];
      it->second.failed = true;
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kOcclusionsBySlot + uint32_t(slot.occlusions.size());
    dfn_.vkCmdBeginQuery(commands_work_, occlusions_, index,
                         occlusion_precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    slot.occlusions.emplace_back(index, occlusion_open_);
    ++it->second.ranges_pending;
    ++statistics_occlusion_[0];
    return index;
  }

  void FinishQueryOcclusion(uint32_t index) override {
    if (occlusions_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_work_, occlusions_, index);
    }
  }

  // fh1_reflection_visibility. Our own query around a single draw (the one that samples the reflection,
  // or the witness), inside the open pass. UINT32_MAX if there is no pool, nothing is being recorded or
  // there is no room left.
  uint32_t BeginQueryVisibility(bool witness) override {
    if (visibility_ == VK_NULL_HANDLE || !recording_) {
      return UINT32_MAX;
    }
    SlotWork& slot = slots_[slot_];
    if (slot.visibility.size() >= kVisibilityBySlot) {
      ++visibility_without_room_;
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kVisibilityBySlot + uint32_t(slot.visibility.size());
    dfn_.vkCmdBeginQuery(commands_work_, visibility_, index, 0);
    slot.visibility.emplace_back(index, witness ? kVisibilityWitness : kVisibilityWater);
    return index;
  }

  void FinishQueryVisibility(uint32_t index) override {
    if (visibility_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_work_, visibility_, index);
    }
  }

  // One query per pass, from before vkCmdBeginRenderPass to after EndRenderPass.
  uint32_t BeginStatistics(uint32_t category) override {
    // With the per-draw diagnostic enabled this one is not opened: two queries of the same type cannot be
    // active at once, and the per-pass query would enclose the per-draw ones.
    if (statistics_ == VK_NULL_HANDLE || !recording_ ||
        !REXCVAR_GET(fh1_native_pipeline_statistics) ||
        REXCVAR_GET(fh1_native_per_draw_statistics_s) > 0) {
      return UINT32_MAX;
    }
    SlotWork& slot = slots_[slot_];
    if (slot.statistics.size() >= kStatisticsBySlot) {
      ++statistics_without_room_;
      if (statistics_without_room_ % 200 == 1) {
        REXLOG_WARN("[native] C2: no room for pass statistics ({} times): the work has more than {} passes", statistics_without_room_, kStatisticsBySlot);
      }
      return UINT32_MAX;
    }
    const uint32_t index =
        slot_ * kStatisticsBySlot + uint32_t(slot.statistics.size());
    dfn_.vkCmdBeginQuery(commands_work_, statistics_, index, 0);
    slot.statistics.emplace_back(index, uint8_t(category));
    return index;
  }

  void FinishStatistics(uint32_t index) override {
    if (statistics_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_work_, statistics_, index);
    }
  }

  // One query per draw, tagged with its pixel shader.
  uint32_t BeginStatisticsDraw(uint32_t label, uint32_t category) override {
    if (statistics_draw_ == VK_NULL_HANDLE || !recording_ || !window_diagnostic_) {
      return UINT32_MAX;
    }
    SlotWork& slot = slots_[slot_];
    if (slot.statistics_draw.size() >= kStatisticsDrawBySlot) {
      ++statistics_draw_without_room_;
      if (statistics_draw_without_room_ % 2000 == 1) {
        REXLOG_WARN("[native] C2: no room for per-draw statistics ({} times)",
                    statistics_draw_without_room_);
      }
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kStatisticsDrawBySlot +
                            uint32_t(slot.statistics_draw.size());
    dfn_.vkCmdBeginQuery(commands_work_, statistics_draw_, index, 0);
    slot.statistics_draw.emplace_back(
        index, uint16_t((category % kGpuCategories) * kLabelsShader + label % kLabelsShader));
    return index;
  }

  void FinishStatisticsDraw(uint32_t index) override {
    if (statistics_draw_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_work_, statistics_draw_, index);
    }
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejections_;
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C2: {} (cause {})", text, cause);
    }
    return false;
  }

  // Rectangle covered by the copy, as in GetResolveInfo (draw.cpp:787-889).
  bool Rectangle(const RegistersCopy& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                  int32_t& x1, int32_t& y1) {
    const uint32_t type = reg.fetch_vertices[0] & 0x3;
    const uint32_t address = reg.fetch_vertices[0] >> 2;
    const auto order = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
    const uint32_t size = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
    if (type != uint32_t(xenos::FetchConstantType::kVertex) || size != 3 * 2) {
      return Reject(5, "copy vertices in an unsupported format");
    }
    const uint8_t* vertices = memory_->TranslatePhysical(address * 4);
    const float middle_pixel =
        (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
    int32_t fixed[6];
    for (int i = 0; i < 6; ++i) {
      float input_value;
      std::memcpy(&input_value, vertices + i * 4, sizeof(input_value));
      fixed[i] = Fixed16p8(xenos::GpuSwap(input_value, order) + middle_pixel);
    }
    x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;

    const int32_t offset_x = ExtendSign15(reg.pa_sc_window_offset & 0x7FFF);
    const int32_t offset_y = ExtendSign15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
    if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
      x0 += offset_x;
      y0 += offset_y;
      x1 += offset_x;
      y1 += offset_y;
    }
    // Window scissor (GetScissor without clamping to the pitch).
    int32_t left = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
    int32_t up = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
    int32_t right = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
    int32_t down = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
    if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
      left += offset_x;
      up += offset_y;
      right += offset_x;
      down += offset_y;
    }
    left = std::max(left, 0);
    up = std::max(up, 0);
    right = std::max(right, left);
    down = std::max(down, up);
    x0 = std::clamp(x0, left, right);
    y0 = std::clamp(y0, up, down);
    x1 = std::clamp(x1, left, right);
    y1 = std::clamp(y1, up, down);
    // D3D9 alinea a 8 (kResolveAlignmentPixels).
    x0 &= ~int32_t(7);
    y0 &= ~int32_t(7);
    x1 = (x1 + 7) & ~int32_t(7);
    y1 = (y1 + 7) & ~int32_t(7);
    const int32_t pitch_aligned = int32_t(pitch & ~uint32_t(7));
    x0 = std::min(x0, pitch_aligned);
    x1 = std::min(x1, pitch_aligned);
    if (x0 >= x1 || y0 >= y1) {
      return Reject(6, "empty copy rectangle");
    }
    return true;
  }

  // NFSC: some Xenos colour formats are the SAME 32-bit pixel layout with a different shader-output encoding
  // (k_2_10_10_10_FLOAT_AS_16_16_16_16 is k_2_10_10_10_FLOAT, k_2_10_10_10_AS_10_10_10_10 is k_2_10_10_10). On the
  // hardware both live in the same EDRAM, so draws in one format blend over what the other left there: Carbon's final
  // composite (format 12) is blended over the scene (format 3) at the same EDRAM base. They must share one image.
  static uint32_t FormatTargetCanonical(uint32_t format) {
    switch (format) {
      case 12: return 3;
      case 10: return 2;
      // FH1: k_8_8_8_8_GAMMA and k_8_8_8_8 are the same 32 bits in the EDRAM. The game draws its videos (boot logos,
      // intro, menu backgrounds) into the gamma format and resolves them with the color info set to plain 8_8_8_8:
      // with one image per format the resolve read a target nobody had drawn (white from its own clear).
      case 1: return 0;
      default: return format;
    }
  }

  Image* GetTarget(uint32_t base, uint32_t format, uint32_t pitch) {
    format = FormatTargetCanonical(format);
    if (!pitch) {
      Reject(7, "render target with pitch 0");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    auto it = targets_.find(key);
    if (it != targets_.end()) {
      return &it->second;
    }
    /*
     * The height comes from the pitch, so the scene's color target measures 1280x1280 to draw 1280x720
     * and can never be swapped with its resolved texture. With a pitch of 1280 or less the game never
     * draws below 720 (measured viewport and scissor), so 720 rows would be enough and the sizes would
     * match. Above 1280 (the internal 1080p mode) the larger height is needed.
     */
    /*
     * Creating the color target 720 rows high (instead of deriving the height from the pitch), so that it
     * matches the resolved texture and the images can be swapped instead of copied, was tried: it breaks
     * the scene (the screen fills with a yellow smear). Reverted.
     */
    const uint32_t height = std::min(kHeightMaximumTarget, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    Image image;
    // NFSC: the HDR formats get a real float image (format is already canonical: 12 -> 3, 10 -> 2).
    const bool hdr = REXCVAR_GET(fh1_hdr_float) &&
                     format == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
    const VkFormat format_fh1 = FormatHostFh1(format);
    const VkFormat format_host = format_fh1 != VK_FORMAT_UNDEFINED ? format_fh1
                                 : hdr                             ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                                   : kFormatColor;
    if (!Create(image, pitch, height,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               format_host)) {
      Reject(8, "could not create a render target");
      return nullptr;
    }
    const VkClearColorValue diag = ColorDiagnostic(base, format, pitch);
    REXLOG_INFO("[native] C2: render target base {:03X}, format {}, {}x{} (diagnostic color {:02X}{:02X}{:02X})",
                base, format, pitch, height, uint32_t(std::lround(diag.float32[0] * 255.0f)),
                uint32_t(std::lround(diag.float32[1] * 255.0f)),
                uint32_t(std::lround(diag.float32[2] * 255.0f)));
    if (format_host != kFormatColor) {
      images_hdr_[image.image] = format_host;
      REXLOG_INFO("[fh1] HDR render target base {:03X} {}x{} is R16G16B16A16_SFLOAT", base, pitch, height);
    }
    return &targets_.emplace(key, image).first->second;
  }
  // NFSC: float scene targets (fh1_hdr_float); FH1: every render target that is not 8-bit, with its format.
  std::unordered_map<VkImage, VkFormat> images_hdr_;
  // FH1: resolved textures replaced at their address by another size or format (GetResolved).
  std::unordered_map<uint64_t, Resolved> parked_;
  static uint64_t KeyParked(uint32_t base, uint32_t width, uint32_t height, VkFormat format) {
    return (uint64_t(base) << 32) ^ (uint64_t(width) << 20) ^ (uint64_t(height) << 6) ^ uint64_t(format) * 0x9E3779B1u;
  }

  // NFSC: every image-to-image copy goes through here. A plain copy cannot convert between the float scene target and
  // the 8-bit textures, so when exactly one side is float the regions are blitted (nearest, 1 to 1) instead.
  void CopyImages(VkCommandBuffer cmd, VkImage source, VkImageLayout layer_source, VkImage target,
                      VkImageLayout layer_target, uint32_t n, const VkImageCopy* regions) {
    // FH1: compared by format (several wide formats now), not only "float or not".
    const auto is = images_hdr_.find(source), it = images_hdr_.find(target);
    const VkFormat fs = is != images_hdr_.end() ? is->second : VK_FORMAT_UNDEFINED;
    const VkFormat ft = it != images_hdr_.end() ? it->second : VK_FORMAT_UNDEFINED;
    if (fs == ft || !blit_) {
      copy_image_(cmd, source, layer_source, target, layer_target, n, regions);
      return;
    }
    for (uint32_t i = 0; i < n; ++i) {
      const VkImageCopy& r = regions[i];
      VkImageBlit b{};
      b.srcSubresource = r.srcSubresource;
      b.srcOffsets[0] = r.srcOffset;
      b.srcOffsets[1] = {r.srcOffset.x + int32_t(r.extent.width), r.srcOffset.y + int32_t(r.extent.height),
                         r.srcOffset.z + int32_t(r.extent.depth)};
      b.dstSubresource = r.dstSubresource;
      b.dstOffsets[0] = r.dstOffset;
      b.dstOffsets[1] = {r.dstOffset.x + int32_t(r.extent.width), r.dstOffset.y + int32_t(r.extent.height),
                         r.dstOffset.z + int32_t(r.extent.depth)};
      blit_(cmd, source, layer_source, target, layer_target, 1, &b, VK_FILTER_NEAREST);
    }
  }

  Image* GetDepth(uint32_t base, uint32_t format, uint32_t pitch) {
    if (!pitch || format_depth_ == VK_FORMAT_UNDEFINED) {
      Reject(11, "no depth target (pitch 0 or format not available)");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    auto it = depths_.find(key);
    if (it != depths_.end()) {
      return &it->second;
    }
    const uint32_t height = std::min(kHeightMaximumTarget, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    // Smaller shadow map.
    //
    // The game requests two 1600x1600 maps per frame, and 1600 is not a quality choice: it is 2000 of
    // the 2048 80x16 tiles of the Xbox 360 EDRAM. The PC version of this same game uses 1024x1024. Here
    // the map is drawn at fh1_native_shadow_scale percent and upscaled when resolved, so the texture
    // the scene samples is the usual one and only the detail goes down.
    //
    // It is recognized by its 1600x1600 size, which no other render target uses. The scale is read only
    // once (when the first map is created) so that changing it at runtime does not leave old images unused.
    uint32_t width_image = pitch, height_image = height;
    uint32_t width_guest = 0, height_guest = 0;
    if (pitch == kSideShadows && height == kSideShadows) {
      if (scale_shadows_ == 0) {
        scale_shadows_ = uint32_t(std::clamp(REXCVAR_GET(fh1_native_shadow_scale), 50, 100));
        if (!blit_ || !depth_scalable_) {
          scale_shadows_ = 100;
        }
      }
      if (scale_shadows_ < 100) {
        width_image = std::max<uint32_t>(64, ((pitch * scale_shadows_ / 100) + 15) & ~15u);
        height_image = std::max<uint32_t>(64, ((height * scale_shadows_ / 100) + 15) & ~15u);
        width_guest = pitch;
        height_guest = height;
        REXLOG_INFO("[native] C2: shadow map at {} %: drawn and resolved at {}x{} (the game asks for {}x{})",
                    scale_shadows_, width_image, height_image, pitch, height);
      }
    }
    // The usage flag that decides whether there is hierarchical depth culling.
    //
    // The driver only assigns a ZCULL plane if the usage flags fit in
    //   DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC | SAMPLED | INPUT_ATTACHMENT
    // (nvk_image.c). TRANSFER_DST disqualifies the image, and we only request it for
    // vkCmdClearDepthStencilImage, which the specification requires with that usage.
    //
    // TRANSFER_DST is dropped from the scene depth, which is 50 % of the frame, and kept for the shadow
    // map. Reason: the scene resolves 1280x720 out of a 1280x1280 image, so it is never "whole" and
    // never swaps with its resolved texture (console logs: thousands of "no swap ... because the
    // command does not clear the target", 0 restores). The shadow map does swap, ~0.83 times per frame, and that
    // swap is worth 2.36 ms measured. So the scene gains ZCULL without losing a single swap.
    //
    // In exchange, that image can no longer be cleared with vkCmdClearDepthStencilImage: it is cleared
    // by opening a pass with loadOp = CLEAR (DrawsVulkan::ClearDepthInPass).
    // With fh1_native_zcull off, every image gets TRANSFER_DST: none receives a ZCULL plane and clears
    // go back to vkCmdClearDepthStencilImage, the path used before ZCULL.
    //
    // =========================================================================================
    // Do not remove TRANSFER_DST from the shadow map. Examined in depth and rejected.
    //
    // 1. It still needs it. The reasoning was that the map is no longer copied but swapped, so the flag
    //    would be unnecessary. Wrong: each swap is paid for with a restore, and a restore is exactly a
    //    copy into this image (RestoreContent -> copy_image_ with target.image as dst, which
    //    requires TRANSFER_DST). Steady race, three consecutive 10 s intervals:
    //        swaps without clear  4679 -> 4885 -> 5145 -> 5369   (+206, +260, +224)
    //        restores             3584 -> 3790 -> 4050 -> 4274   (+206, +260, +224)
    //    Exactly one restore per swap. The 1600x1600 copy did not go away: it is paid later, in
    //    TargetDepth. Without TRANSFER_DST that is an illegal write.
    //    Also, SwapWithResolved requires accepts_target_of_copy, so removing the flag would also
    //    disable the 2 swaps per frame that do pay off.
    //
    // 2. Even if it were possible, it would not help. Fit over 17 race intervals with a constant open
    //    area (5.12 Mtexels): raw ms = 0.0342 x thousands of triangles + 0.023, r2 = 0.982. The
    //    intercept is 0.037 real ms: with both maps open and zero triangles the pass costs nothing.
    //    The whole pass is geometry, and ZCULL discards fragments. The same fit against draws gives
    //    r2 = 0.404. ZCULL ceiling here: 0.037 ms; with the most generous bound (the scale test: -44 %
    //    of area gave -9 % of time), 0.1-0.25 ms. Against the 2.36 ms of the swap that would be lost:
    //    a net loss of 10 to 1.
    //
    // 3. ZCULL for shadows only, without touching the scene depth: in NVK (nvk_cmd_draw.c,
    //    `use_zcull`) a pass opened with loadOp = LOAD_OP_CLEAR enables ZCULL even when the image has
    //    no plane. It is ephemeral, without LOAD_ZCULL/STORE_ZCULL between passes, which a shadow map
    //    drawn whole every time does not need. The shadow pass opens with LOAD, or with DONT_CARE under
    //    fh1_native_shadow_pass_without_load, so it gets no ephemeral ZCULL either. Given point 2, that
    //    does not pay off while the pass is pure geometry.
    // =========================================================================================
    const bool zcull = REXCVAR_GET(fh1_native_zcull);
    const bool is_map_of_shadows = (pitch == kSideShadows && height == kSideShadows);
    const bool con_transfer_dst = is_map_of_shadows || !zcull;
    VkImageUsageFlags use_depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
    if (con_transfer_dst) {
      use_depth |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    Image image;
    // Source of the depth copies (CopyDepth): TRANSFER_SRC is required.
    // SAMPLED: with fh1_native_resolve_without_copy this image can end up being the resolved texture.
    if (!Create(image, width_image, height_image, use_depth, format_depth_)) {
      Reject(12, "could not create a depth target");
      return nullptr;
    }
    image.width_guest = width_guest;
    image.height_guest = height_guest;
    image.accepts_target_of_copy = con_transfer_dst;
    REXLOG_INFO("[native] C2: depth target base {:03X}, format {}, {}x{}", base,
                format, pitch, height);
    return &depths_.emplace(key, image).first->second;
  }

  // NFSC: the resolved colour texture (4 bytes per texel, 32x32 tiles) that contains this address at a whole number of
  // 32-row stripes, if any. row = first row of the destination inside it.
  // The depth copies (k_24_8, also 4 bytes per texel) use it too, with format = the host depth format.
  // scale: 2 for the supersampled scene's depth, whose resolved texture is twice the size (row stays in guest rows).
  // FH1: with column, the address may also be a tile at any column of the texture (column = its first column); then
  // the copy, which ends at (right, bottom) counted from that tile, must fit inside the texture.
  Resolved* FindResolvedContainer(uint32_t base, uint32_t pitch, uint32_t& row, uint32_t& base_container,
                                      VkFormat format = kFormatColor, uint32_t scale = 1, uint32_t* column = nullptr,
                                      uint32_t right = 0, uint32_t bottom = 0) {
    const uint32_t tiles_row = ((pitch + 31) & ~31u) / 32u;
    const uint32_t stripe = tiles_row * 32u * 32u * 4u;  // bytes of one 32-row stripe
    for (auto& [key_base, t] : resolved_) {
      if (t.image.format != format || t.image.width != pitch * scale || base <= key_base) {
        continue;
      }
      const uint32_t delta = base - key_base;
      const uint32_t rows = delta / stripe * 32u;
      if (delta % stripe != 0) {
        const uint32_t columns = (delta % stripe) / 4096u * 32u;
        if (!column || delta % 4096u != 0 || columns + right > t.image.width || rows + bottom > t.image.height) {
          continue;
        }
        *column = columns;
        row = rows;
        base_container = key_base;
        return &t;
      }
      if (rows * scale < t.image.height) {
        row = rows;
        base_container = key_base;
        return &t;
      }
    }
    return nullptr;
  }
  bool tile_registered_ = false;
  uint32_t pieces_logged_ = 0;  // FH1: resolves into a rectangle of a larger texture (log lines)
  uint32_t last_resolved_screen_ = 0;  // FH1: address of the last resolved texture of 1280x720 or more
  bool strips_depth_warned_ = false;  // NFSC: depth resolve in strips (log once)
  uint64_t presented_stats_ = 0;  // NFSC: F2 monitor
  std::chrono::steady_clock::time_point last_presented_stats_{};
  std::chrono::steady_clock::time_point start_window_fps_{};  // NFSC: "[fps]" log line
  std::vector<float> times_window_fps_;

  /*
   * FH1: resolves here go image to image and never reach guest memory, so guest memory at a resolve address keeps
   * whatever the game's CPU code put there. The game reuses such addresses for pictures it decodes itself: the
   * loading-screen artwork is written over a 1280x720 screen copy made at the title screen, and the renderer kept
   * sampling its own stale copy (a black or old-menu background behind the loading text). A small fingerprint of
   * that memory (16 runs of 64 bytes) is taken at every resolve; when a draw asks for the address and the
   * fingerprint has changed, the game has written a texture there, and the address is read from memory like any
   * other texture until the next resolve.
   */
  static uint32_t BytesTexelGuest(uint32_t format_guest) {
    return format_guest == uint32_t(xenos::ColorFormat::k_8) ? 1 : 4;
  }
  uint64_t StampMemory(uint32_t base, uint32_t width, uint32_t height, uint32_t bytes_texel = 4) const {
    const uint64_t bytes = std::min<uint64_t>(uint64_t(width) * height * bytes_texel, 0x20000000ull - std::min<uint64_t>(base, 0x20000000ull));
    if (bytes < 64 || !memory_) {
      return 0;
    }
    const uint8_t* const data = memory_->TranslatePhysical(base);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t run = 0; run < 16; ++run) {
      const uint64_t start = ((bytes - 64) * run / 15) & ~uint64_t(7);
      for (uint32_t i = 0; i < 64; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + start + i, 8);
        h = (h ^ word) * 0x100000001B3ull;
        h ^= h >> 29;
      }
    }
    return h;
  }
  uint64_t presentations_stamp_ = 0;  // advanced once per presented frame: the fingerprints are checked once per frame

  Resolved* GetResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                            bool swap_rb, VkFormat format_host = kFormatColor) {
    Resolved* const resolved = GetResolvedImage(base, width, height, format, swap_rb, format_host);
    if (resolved) {
      const bool was = resolved->overwritten;
      resolved->stamp_memory = StampMemory(base, width, height, BytesTexelGuest(format));
      resolved->stamp_checked = presentations_stamp_;
      resolved->overwritten = false;
      if (was && draws_) {
        draws_->InvalidateTextures();  // the address is sampled from the resolved image again
      }
    }
    return resolved;
  }

  Resolved* GetResolvedImage(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                                 bool swap_rb, VkFormat format_host) {
    if (!width || !height) {
      Reject(9, "copy to a texture of size 0");
      return nullptr;
    }
    auto it = resolved_.find(base);
    if (it != resolved_.end()) {
      if (it->second.image.width == width && it->second.image.height == height &&
          it->second.image.format == format_host) {
        if (draws_ && (it->second.format_guest != format || it->second.swap_rb != swap_rb)) {
          draws_->InvalidateTextures();  // changes the swizzle it is sampled with
        }
        it->second.format_guest = format;
        it->second.swap_rb = swap_rb;
        it->second.image.swap_rb = swap_rb;
        return &it->second;
      }
      // FH1: its bloom chain resolves three sizes to the same address every frame. Instead of destroying the image
      // (which waited for the GPU each time: 3 stalls per frame), it is parked and taken back when that size and
      // format come again. Parked images keep their content; the address now names the other one.
      {
        const uint64_t key_old = KeyParked(base, it->second.image.width, it->second.image.height,
                                           it->second.image.format);
        const uint64_t key_new = KeyParked(base, width, height, format_host);
        auto pk = parked_.find(key_new);
        if (pk != parked_.end() && parked_.size() < 256) {
          parked_.emplace(key_old, it->second);
          it->second = pk->second;
          parked_.erase(pk);
          it->second.format_guest = format;
          it->second.swap_rb = swap_rb;
          it->second.image.swap_rb = swap_rb;
          if (draws_) draws_->InvalidateTextures();
          if (!lent_.empty()) lent_.erase(base);
          return &it->second;
        }
        if (parked_.size() < 256) {
          parked_.emplace(key_old, it->second);
          resolved_.erase(it);
          if (!lent_.empty()) lent_.erase(base);
          if (draws_) draws_->InvalidateTextures();
          it = resolved_.end();
        }
      }
    }
    if (it != resolved_.end()) {
      // Another size or format at the same address: the old image may still be in use.
      SendWork(true);
      WaitGpu();
      if (draws_) {
        draws_->ForgetImage(it->second.image.image);
      }
      Destroy(it->second.image);
      resolved_.erase(it);
      // The image at that address no longer exists, so no render target owes it anything.
      if (!lent_.empty()) {
        lent_.erase(base);
      }
      ShadowMinimumResolvedWritten(base);  // the new image does not have its content
    }
    Resolved resolved;
    // The attachment usage (color or depth) is needed for fh1_native_resolve_without_copy: this image can
    // end up being the render target it is swapped with.
    const VkImageUsageFlags use_target = IsDepthFormat(format_host)
                                              ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!Create(resolved.image, width, height,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | use_target,
               format_host)) {
      Reject(10, "could not create a resolved texture");
      return nullptr;
    }
    resolved.format_guest = format;
    resolved.swap_rb = swap_rb;
    resolved.image.swap_rb = swap_rb;
    if (format_host != kFormatColor && !IsDepthFormat(format_host)) {
      images_hdr_[resolved.image.image] = format_host;  // FH1: copies to it convert by format
    }
    REXLOG_INFO("[native] C2: resolved texture at {:08X}, {}x{}, format {}", base, width, height,
                format);
    if (draws_) {
      draws_->InvalidateTextures();  // that address is now sampled from the resolved texture
    }
    return &resolved_.emplace(base, resolved).first->second;
  }

  // One more copy in its size bucket, to tell whether the copy milliseconds come from pixels or from a
  // fixed cost per copy.
  void NoteCopy(uint32_t width, uint32_t height) {
    const uint64_t pixels = uint64_t(width) * height;
    for (uint32_t c = 0; c < kBucketsCopy; ++c) {
      if (pixels <= kPixelsBucket[c]) {
        ++copies_bucket_[c];
        pixels_bucket_[c] += pixels;
        return;
      }
    }
  }

  // The setting, or its alternating test.
  bool ResolveWithoutCopy() {
    const int32_t toggle = REXCVAR_GET(fh1_native_resolve_without_copy_toggle_s);
    if (toggle <= 0) {
      return REXCVAR_GET(fh1_native_resolve_without_copy);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_)
                              .count();
    const bool without_copy = (seconds / toggle) % 2 == 1;
    if (without_copy != alternation_noted_) {
      alternation_noted_ = without_copy;
      REXLOG_INFO("[native] C2 resolve: {}", without_copy ? "without copying (swap)" : "copying");
    }
    return without_copy;
  }

  static bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
           format == VK_FORMAT_X8_D24_UNORM_PACK32;
  }

  // Resolve without copying. The whole render target becomes the resolved texture and that texture's
  // old image stays as the render target. What the game sees is the same; what is saved is moving the
  // pixels (the two 1600x1600 shadow map copies are 41 MB per frame). false if it is not possible.
  bool SwapWithResolved(Image& target, Resolved& resolved, uint32_t base) {
    if (target.width != resolved.image.width || target.height != resolved.image.height ||
        target.format != resolved.image.format) {
      return false;
    }
    // ZCULL safety net. If the render target was created without TRANSFER_DST (so it can have a ZCULL
    // plane), swapping it would put it in the resolved texture's place, which does receive copies: an
    // illegal write. And the other way round, the resolved texture (with TRANSFER_DST and no plane) would
    // become the depth target and ZCULL would switch off on alternate frames. Fall back to the usual copy
    // path instead.
    if (!target.accepts_target_of_copy) {
      return false;
    }
    // fh1_native_diag_clears. The whole image becomes a sampled texture, so all of it counts as used
    // (noted before the images change places).
    NoteUseClear(target, target.width, target.height);
    // Careful: ForgetImage must not be called here. It frees the descriptor slot and reuses it in the
    // same frame, so draws already recorded with it end up reading another image (flicker). Bumping the
    // generation is enough: later draws resolve their view again, and existing views stay valid because
    // they go with their image.
    // fh1_native_lazy_depth. The render target changes image: first, whatever it has deferred
    // as a source is resolved; and the texture receives the whole target, so its deferred copy is no
    // longer needed.
    if (!pending_.empty()) {
      BeforeOfWriteDepth(target);
      if (pending_.erase(base)) {
        ++lazy_replaced_;
      }
    }
    // fh1_native_lazy_front. Same for front buffers: deferred copies whose source is this target
    // are recorded first, and the texture receives the whole target (its deferred copy is unnecessary).
    if (!front_pending_.empty()) {
      BeforeOfWriteColor(target);
      ResolveFrontPrevious(base, true);
    }
    std::swap(target.image, resolved.image.image);
    std::swap(target.memory_block, resolved.image.memory_block);
    std::swap(target.view, resolved.image.view);
    std::swap(target.view_srgb, resolved.image.view_srgb);
    std::swap(target.prepared, resolved.image.prepared);
    resolved.image.swap_rb = resolved.swap_rb;  // belongs to the content, not the image
    resolved.image.content_invalid = false;
    target.content_invalid = true;  // its content is now in the resolved texture
    target.resolved_base = base;
    ++swaps_;
    ResolvedWritten(base);   // if it was lent, it has valid content again
    ForgetClear(target);  // and the target keeps another image: its clear no longer holds
    if (draws_) {
      // Only what points to these two images: invalidating the whole cache here costs more than the copy
      // it saves (measured on PC: +0.18 ms of scene per frame).
      draws_->InvalidateImages(target.image, resolved.image.image);
    }
    return true;
  }

  // If the render target lost its content in an image swap and the game is about to draw on top, the
  // content is brought back from the resolved texture.
  //
  // In a race this is not rare: it happens once for every swap without a clear (measured: +206/+260/+224
  // swaps and +206/+260/+224 restores in three consecutive intervals). That is 1.66 real ms per frame
  // that no bucket of the copy inventory accounted for. Here they are counted, trimmed to the useful
  // area and, if requested, done without copying a single pixel.
  //
  // for_resolve = requested by a resolve (fh1_native_resolve_valid_content), not by a pass that
  // will draw on top. Then it is only copied: no lending (it would leave the source texture without
  // content just when the car body is about to read it) and no fh1_native_shadow_minimum clear (that
  // is for the car pass).
  void RestoreContent(Image& target, bool for_resolve = false) {
    if (!target.content_invalid) {
      return;
    }
    target.content_invalid = false;
    // fh1_native_lazy_depth. The resolved texture is about to be read (if its copy was
    // deferred, it is recorded first) and the render target written (first, whatever it has deferred as
    // a source).
    if (!pending_.empty()) {
      if (pending_.count(target.resolved_base)) {
        RecordCopyPending(target.resolved_base);
        ++lazy_copied_read_;
      }
      BeforeOfWriteDepth(target);
    }
    // fh1_native_lazy_front. Same for front buffers: if the texture about to be read has a
    // deferred copy, it is recorded first; and so are deferred copies whose source is this render target.
    if (!front_pending_.empty()) {
      if (front_pending_.count(target.resolved_base)) {
        RecordCopyFront(target.resolved_base);
        ++front_copied_read_;
      }
      BeforeOfWriteColor(target);
    }
    auto it = resolved_.find(target.resolved_base);
    if (it == resolved_.end() || it->second.image.width != target.width ||
        it->second.image.height != target.height || it->second.image.format != target.format ||
        !copy_image_ || !Record()) {
      if (for_resolve) {
        ++resolve_without_source_;  // unknown where to bring it from; resolve whatever is there
      }
      return;
    }
    if (for_resolve) {
      // fh1_native_resolve_valid_content. Only the copy, with its barriers.
      CopyOfLapForResolve(target, it->second.image);
      return;
    }
    // The content does not need duplicating, only to be in the render target: swapping the two images
    // back puts it where it belongs without moving a byte. The resolved texture stays lent until the next
    // resolve to that address; 'borrowed reads' checks that nobody looks at it in the meantime.
    // Both must already be in GENERAL: the swap moves the image, not its layout, and preparing after the
    // swap would put the barrier on the wrong image.
    /*
     * Why the restore does not swap.
     *
     * Restores copy 2.56 Mpixels per frame = 2.00 real ms, and 17 log reports in a row show a saving of
     * 0.00. This path exists precisely to avoid the copy, and the counter says "0 by swap": it
     * never activates. It has three conditions, so each one is counted instead of guessing which one
     * fails. Three uint64 increments on a path that already copies 10 MB: zero cost.
     */
    if (REXCVAR_GET(fh1_native_restore_by_swap)) {
      if (!target.accepts_target_of_copy) {
        ++no_swap_sin_transfer_dst_;
      } else if (!target.prepared) {
        ++no_swap_target_without_prepare_;
      } else if (!it->second.image.prepared) {
        ++no_swap_resolved_without_prepare_;
      } else if (target.width != it->second.image.width ||
                 target.height != it->second.image.height) {
        // Not a condition of the if below, but it does matter: the swap moves the whole image, and if the
        // sizes do not match it would leave the render target with the wrong dimensions. If this count is
        // high, the lever cannot work as written.
        ++no_swap_sizes_different_;
      }
    }
    if (REXCVAR_GET(fh1_native_restore_by_swap) && target.accepts_target_of_copy &&
        target.prepared && it->second.image.prepared) {
      const uint32_t base = target.resolved_base;
      std::swap(target.image, it->second.image.image);
      std::swap(target.memory_block, it->second.image.memory_block);
      std::swap(target.view, it->second.image.view);
      std::swap(target.view_srgb, it->second.image.view_srgb);
      std::swap(target.prepared, it->second.image.prepared);
      it->second.image.content_invalid = false;
      if (draws_) {
        draws_->InvalidateImages(target.image, it->second.image.image);
      }
      lent_.insert(base);
      ++loans_;
      ++restores_;
      pixels_restore_saved_ += uint64_t(target.width) * target.height;
      ForgetClear(target);
      return;
    }
    // fh1_native_shadow_minimum. The car pass of the shadow map: while it is being applied, the render
    // target is cleared to 1.0 instead of copying the world back (the world draws take the minimum).
    if (ShadowMinimumInTimeOfRestore(target)) {
      return;
    }
    Prepare(it->second.image);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    // Render targets are created with height = max(720, pitch): the scene measures 1280x1280 to draw
    // 1280x720 and the blur targets 320x720 to draw 320x180. What lies below the useful area is never
    // drawn or resolved, so copying it wastes bandwidth. The useful area is the largest y1 the game has
    // asked to resolve from this target; until there is data, the whole target is copied.
    uint32_t height = target.height;
    if (REXCVAR_GET(fh1_native_restore_useful_area)) {
      const auto e = state_target_.find(&target);
      if (e != state_target_.end() && e->second.height_used &&
          e->second.height_used < target.height) {
        height = e->second.height_used;
        ++restores_clipped_;
        pixels_restore_saved_ += uint64_t(target.width) * (target.height - height);
      }
    }
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {target.width, height, 1};
    NoteUseClear(target, target.width, height);  // fh1_native_diag_clears
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    CopyImages(commands_work_, it->second.image.image, VK_IMAGE_LAYOUT_GENERAL,
                   target.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ++restores_;
    pixels_restored_ += uint64_t(target.width) * height;
    NoteCopy(target.width, height);  // so the copy inventory adds up to the measured ms
    ForgetClear(target);
  }

  /*
   * fh1_native_resolve_valid_content. The render target gets its content back from the resolved
   * texture where the image swap left it, just before a resolve reads it.
   *
   * The copy goes between two barriers, and here they are needed: the source is the image that was just
   * drawn as the depth target (the previous resolve moved it into the texture by swapping), and what is
   * copied will be sampled as soon as the current resolve swaps it. NVK only waits for the GPU and
   * flushes the texture cache at a barrier (in a race the restore still goes without them, as always:
   * this does not touch it). Each one costs a pipeline drain, twice per frame in the menu. The whole
   * target is copied: that is what the EDRAM held and what the resolve reads.
   */
  void CopyOfLapForResolve(Image& target, Image& source) {
    Prepare(source);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr VkPipelineStageFlags kWrites =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dfn_.vkCmdPipelineBarrier(commands_work_, kWrites, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                              nullptr, 0, nullptr);
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {target.width, target.height, 1};
    NoteUseClear(target, target.width, target.height);  // fh1_native_diag_clears, like the restore
    MarkGpu(kGpuCopies);
    BarrierGlobal(commands_work_);
    CopyImages(commands_work_, source.image, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_GENERAL,
                   1, &copy);
    // And the copied data made visible to what follows: the current resolve (copy or swap), the draws that
    // sample the texture that receives it, and the pass that draws to the render target again.
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_work_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 1, &barrier, 0, nullptr, 0, nullptr);
    ++restores_for_resolve_;
    pixels_restored_ += uint64_t(target.width) * target.height;
    NoteCopy(target.width, target.height);
    ForgetClear(target);
  }

  // --- fh1_native_shadow_minimum (see the cvar comment) --------------------------------------------------

  // A failure while applying may change the image of this very frame: DIFFERENCE in the log and it
  // switches off for the session. Textures that already hold only the cars keep being served with the
  // minimum until they are written again.
  void ShadowMinimumDifference(const char* reason, uint32_t datum) {
    ++sm_differences_;
    if (sm_phase_ != kSmOff) {
      sm_phase_ = kSmOff;
      REXLOG_ERROR("[native] C2 shadow by minimum: DIFFERENCE ({}; clue {:08X}). Turned off for the session: the "
                   "shadow map is copied again as before",
                   reason, datum);
    }
  }

  // Something the minimum would not reproduce, seen before any image changed: not applied this session.
  void ShadowMinimumNoFit(const char* reason, uint32_t datum) {
    if (sm_phase_ != kSmOff) {
      sm_phase_ = kSmOff;
      REXLOG_WARN("[native] C2 shadow by minimum: not applied in this session ({}; clue {:08X}); the shadow map is "
                  "still copied as always",
                  reason, datum);
    }
  }

  // A failure within the current cycle. If the render target was already cleared (applying) it is a
  // DIFFERENCE; otherwise either the minimum does not work for this game (permanent) or the cycle simply
  // does not count (transient: menus, loading).
  void ShadowMinimumMissCycle(const char* reason, uint32_t datum, bool permanent) {
    sm_cycle_clean_ = false;
    if (sm_cycle_clear_) {
      ShadowMinimumDifference(reason, datum);
    } else if (permanent) {
      ShadowMinimumNoFit(reason, datum);
    }
  }

  // The setting for the cycle that starts: the cvar, its alternating test and the cheap PCF.
  bool ShadowMinimumOn() {
    if (!REXCVAR_GET(fh1_native_shadow_minimum) || !REXCVAR_GET(fh1_native_cheap_pcf)) {
      return false;
    }
    const int32_t toggle = REXCVAR_GET(fh1_native_shadow_minimum_toggle_s);
    if (toggle <= 0 || sm_phase_ != kSmApplying) {
      return true;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                           sm_start_alternation_)
                              .count();
    const bool with_minimum = (seconds / toggle) % 2 == 0;
    if (with_minimum != sm_range_noted_) {
      sm_range_noted_ = with_minimum;
      REXLOG_INFO("[native] C2 shadow by minimum (toggle): {}", with_minimum ? "with the minimum" : "copying");
    }
    return with_minimum;
  }

  // After resolving a whole depth target by swapping images (CopyDepth). Without a clear it
  // starts a cycle (the texture receives the world); with a clear, after the car pass, it ends it (the
  // texture receives the cars).
  void ShadowMinimumAfterSwap(const Image& target, uint32_t base, bool clears_2) {
    if (target.width != kSideShadows || target.height != kSideShadows) {
      return;
    }
    if (!clears_2) {
      if (sm_state_ == kSmCars && sm_cycle_clear_) {
        ShadowMinimumDifference("the cars-only target is resolved without clearing", base);
      }
      if (sm_state_ != kSmFree) {
        sm_clean_consecutive_ = 0;  // the previous cycle did not close: count from scratch
      }
      sm_state_ = kSmAfterWorld;
      sm_target_cycle_ = &target;
      sm_world_cycle_ = base;
      sm_cycle_clean_ = true;
      sm_cycle_clear_ = false;
      sm_world_rewritten_ = false;
      sm_on_ = ShadowMinimumOn();
      return;
    }
    if (sm_state_ != kSmCars || &target != sm_target_cycle_) {
      if (sm_state_ != kSmFree) {
        sm_clean_consecutive_ = 0;
      }
      sm_state_ = kSmFree;
      return;
    }
    sm_state_ = kSmFree;
    ++sm_cycles_;
    if (sm_cycle_clear_) {
      // The texture has just received only the cars: from here on it is sampled paired with the world.
      if (base != sm_cars_) {
        ShadowMinimumDifference("the car resolve goes to another texture", base);
        sm_cars_ = base;  // it holds only the cars: serve that one
      }
      sm_virtual_ = true;
      sm_virtual_valid_ = !sm_world_rewritten_;
      if (sm_world_rewritten_) {
        ShadowMinimumDifference("the world texture was written again inside the cycle", sm_world_);
      }
      ++sm_cycles_applied_;
      return;
    }
    // Observing (or applying with the setting off in this cycle): the texture is the usual one, with the
    // world and the cars.
    if (base != sm_cars_ || sm_world_cycle_ != sm_world_ || sm_target_cycle_ != sm_target_) {
      if (sm_phase_ == kSmApplying) {
        // A cycle different from the learned one (split screen, another mode): it cannot be served.
        // Stop applying.
        ShadowMinimumNoFit("a shadow-map cycle different from the learned one appears (the clue is its texture)", base);
        return;
      }
      sm_cars_ = base;  // learned (or changed): count again from zero
      sm_world_ = sm_world_cycle_;
      sm_target_ = sm_target_cycle_;
      sm_clean_consecutive_ = 0;
    }
    sm_clean_consecutive_ = (sm_cycle_clean_ && !sm_world_rewritten_) ? sm_clean_consecutive_ + 1 : 0;
    if (sm_phase_ == kSmWatching && sm_on_ && sm_clean_consecutive_ >= kShadowMinimumCycles) {
      sm_phase_ = kSmApplying;
      REXLOG_INFO("[native] C2 shadow by minimum: APPLYING after {} clean cycles in a row (target {}x{}, world at "
                  "{:08X}, cars at {:08X}): the car pass is drawn on the cleared target and not copied",
                  sm_clean_consecutive_, target.width, target.height, sm_world_, sm_cars_);
    }
  }

  // From RestoreContent: the render target lost its content in a swap and is about to be drawn on.
  // If this is the car pass of the current cycle it is noted, and while applying, the target is cleared
  // to 1.0 instead of bringing the world back (true: no copy needed).
  bool ShadowMinimumInTimeOfRestore(Image& target) {
    if (&target != sm_target_cycle_ || sm_state_ != kSmAfterWorld) {
      if (&target == sm_target_cycle_ && sm_state_ == kSmCars) {
        ShadowMinimumMissCycle("the car target is restored again", target.resolved_base, false);
      }
      return false;
    }
    if (target.resolved_base != sm_world_cycle_) {
      ShadowMinimumMissCycle("the target is restored from another texture", target.resolved_base, false);
      return false;
    }
    sm_state_ = kSmCars;
    // Only with the learned cycle: the same render target and the same world texture as in the observing
    // phase.
    if (sm_phase_ != kSmApplying || !sm_on_ || &target != sm_target_ || sm_world_cycle_ != sm_world_ ||
        !sm_cars_ || !clear_depth_ || !target.accepts_target_of_copy) {
      return false;
    }
    Prepare(target);
    MarkGpu(kGpuClears);
    const VkClearDepthStencilValue input_value{1.0f, 0};
    BarrierGlobal(commands_work_);
    clear_depth_(commands_work_, target.image, VK_IMAGE_LAYOUT_GENERAL, &input_value, 1, &kRangeDepth);
    NoteUseClear(target, target.width, target.height);  // fh1_native_diag_clears: like the restore
    ForgetClear(target);  // not a game clear: the game's next clear cannot be skipped
    sm_cycle_clear_ = true;
    sm_pixels_saved_ += uint64_t(target.width) * target.height;
    return true;
  }

  // A copy (not a swap) from a depth target: if it is the current cycle's target, something reads its
  // content mid-cycle. With the target already cleared, what gets copied is only the cars.
  void ShadowMinimumCopySince(const Image& source) {
    if (&source == sm_target_cycle_ && sm_state_ != kSmFree) {
      ShadowMinimumMissCycle("copying from the shadow-map target in the middle of the cycle", 0, false);
    }
  }

  // The game clears the current cycle's target without the second resolve: its content is lost the same
  // way on both paths, but in this cycle the texture with the cars is not renewed while the world one
  // is. The cycle does not count.
  void ShadowMinimumBeforeOfClear(const Image& target) {
    if (&target == sm_target_cycle_ && sm_state_ != kSmFree) {
      sm_clean_consecutive_ = 0;
      sm_state_ = kSmFree;
    }
  }

  // A resolved texture receives new content (ResolvedWritten) or is recreated (GetResolved).
  void ShadowMinimumResolvedWritten(uint32_t base) {
    if (sm_virtual_) {
      if (base == sm_cars_) {
        sm_virtual_ = false;  // new content: no longer only the cars
      } else if (base == sm_world_) {
        sm_virtual_valid_ = false;  // its pair changes: reading it now would not give its cycle's minimum
      }
    }
    if (sm_state_ != kSmFree && base == sm_world_cycle_) {
      sm_world_rewritten_ = true;
    }
  }

  // C2 report line (every 10 s, with the other copy lines). The ms use 0.60 per Mpixel copied and 0.075
  // per Mpixel cleared.
  void ReportShadowMinimum() {
    if (!sm_cycles_ && !sm_differences_ && !sm_reads_unable_ && !sm_reads_a_out_of_time_) {
      return;
    }
    const double frames = double(presented_ - presented_report_copies_);
    const double byFrame = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(sm_pixels_saved_ - sm_pixels_saved_previous_) / 1e6 * byFrame;
    static constexpr const char* kPhases[3] = {"watching", "APPLYING", "off"};
    FH1_REPORT_RING(
        "[native] C2 shadow by minimum (build 184): phase {}; {} cycles since startup ({} clean in a row, {} "
        "applied); {:.2f} Mpixels per frame cleared instead of copied (~{:.2f} real ms of copying less and ~{:.2f} "
        "of clearing more); car draws {} ({} not exact); reads of {:08X} since startup: {} with the world as "
        "partner, {} with itself, {} without minimum, {} between the two resolves, {} without tfetch2DShadowMin; "
        "DIFFERENCES {}{}",
        kPhases[sm_phase_ < 3 ? sm_phase_ : 2], sm_cycles_, sm_clean_consecutive_, sm_cycles_applied_, mp, mp * 0.60,
        mp * 0.075, sm_draws_cars_, sm_draws_no_exact_, sm_cars_, sm_reads_minimum_, sm_reads_si_same_,
        sm_reads_normals_, sm_reads_a_out_of_time_, sm_reads_unable_, sm_differences_,
        sm_differences_ ? " *** THE IMAGE MAY HAVE CHANGED: see the ERROR in the log ***" : " (0 = the image is "
                                                                                            "the same)");
    sm_pixels_saved_previous_ = sm_pixels_saved_;
  }

  // The resolved texture has just received new content, so if it was lent it no longer is. An empty set
  // (the normal case) touches nothing. `pixels` = the pixels actually copied (0 if the resolve swapped
  // images), so that the per-target inventory shows where the traffic goes.
  void ResolvedWritten(uint32_t base, uint64_t pixels = 0) {
    ShadowMinimumResolvedWritten(base);  // fh1_native_shadow_minimum
    if (!depth_bytes_.empty()) {  // FH1: its bytes image is redone at the next fetch
      const auto bytes = depth_bytes_.find(base);
      if (bytes != depth_bytes_.end()) {
        bytes->second.dirty = true;
      }
    }
    if (!lent_.empty()) {
      lent_.erase(base);
    }
    if (!stale_.empty()) {
      stale_.erase(base);  // New contents: no dropped copy is missing any more
    }
    if (pixels) {
      const auto c = copies_by_target_.find(base);
      if (c != copies_by_target_.end()) {
        c->second.pixels += pixels;
      }
    }
  }

  // The render target's content is no longer that of its last clear (something was copied over it or
  // it changed image). The game's next clear cannot be skipped.
  //
  // It also answers the other question about clears: if the target's content changes without anything
  // drawn since it was cleared, that clear was wiped out by a swap or a restore and served no purpose.
  // It is the equivalent of a loadOp = CLEAR that gets thrown away: these are DONT_CARE candidates.
  void ForgetClear(const Image& target) {
    const auto e = state_target_.find(&target);
    if (e == state_target_.end()) {
      return;
    }
    if (e->second.clear_clean &&
        e->second.draws_to_clear == (draws_ ? draws_->Drawn() : 0)) {
      ++clears_useless_;
      pixels_clears_useless_ += uint64_t(target.width) * target.height;
    }
    e->second.clear_clean = false;
  }

  // The largest rectangle the game resolves from this render target: the area it actually uses.
  void NoteAreaUseful(const Image& target, int32_t y1) {
    if (y1 > 0) {
      auto& e = state_target_[&target];
      e.height_used = std::max(e.height_used, std::min(uint32_t(y1), target.height));
    }
  }

  // A clear that does not change a single bit. It is skipped if the render target is already cleared
  // to that same value and nothing has been drawn, anywhere, since then. Returns true if it must be
  // skipped.
  bool ClearRedundant(const Image& target, uint64_t input_value) {
    auto& e = state_target_[&target];
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    if (REXCVAR_GET(fh1_native_skip_repeated_clears) && e.clear_clean &&
        e.value_clear == input_value && e.draws_to_clear == drawn) {
      pixels_clears_skipped_ += uint64_t(target.width) * target.height;
      return true;
    }
    e.clear_clean = true;
    e.value_clear = input_value;
    e.draws_to_clear = drawn;
    return false;
  }

  bool Create(Image& image, uint32_t width, uint32_t height, VkImageUsageFlags use,
             VkFormat format = kFormatColor) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    // FH1: 8-bit color images also get an sRGB view (ImageNative::view_srgb).
    info.flags = format == kFormatColor ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = use;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
            image.memory_block)) {
      // The GPU can run out of memory here too. Render targets are few and large, so one that does not fit
      // shows up at once: the texture cache is asked to release memory and the allocation is retried once.
      if (!draws_ || !draws_->ReleaseTexturesByMissingOfMemory() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
              image.memory_block)) {
        REXLOG_ERROR("[native] C2: out of GPU memory for a {}x{} target (format {}) and releasing the cache was "
                     "not enough",
                     width, height, uint32_t(format));
        return false;
      }
    }
    VkImageViewCreateInfo info_view{};
    info_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info_view.image = image.image;
    info_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info_view.format = format;
    info_view.subresourceRange = IsDepthFormat(format) ? kRangeDepth : kRangeColor;  // FH1: float color images too
    if (dfn_.vkCreateImageView(device_, &info_view, nullptr, &image.view) != VK_SUCCESS) {
      Destroy(image);
      return false;
    }
    image.view_srgb = VK_NULL_HANDLE;
    if (format == kFormatColor) {
      info_view.format = VK_FORMAT_R8G8B8A8_SRGB;
      if (dfn_.vkCreateImageView(device_, &info_view, nullptr, &image.view_srgb) != VK_SUCCESS) {
        image.view_srgb = VK_NULL_HANDLE;  // without it, gamma draws and fetches stay raw
      }
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  void Destroy(Image& image) {
    images_hdr_.erase(image.image);  // NFSC
    ForgetDepthBytesSource(image.image);  // FH1
    CompositeToDestroy(image);  // fh1_native_lazy_composite
    // fh1_native_lazy_front. A deferred copy cannot keep a destroyed image. If the texture is
    // what gets destroyed (GetResolved recreates it with another size or format), its content is lost
    // just as before: the copy is simply dropped. If it is the source (only at shutdown: render targets
    // are not destroyed, and whatever leaves a target through a swap is recorded first), the texture lacks
    // that copy: it becomes stale and, if something requests it before another full resolve, the guard
    // trips.
    if (!front_pending_.empty() && image.image != VK_NULL_HANDLE) {
      for (auto it = front_pending_.begin(); it != front_pending_.end();) {
        if (it->second.texture_vk == image.image) {
          FreeRetained(it->second);
          it = front_pending_.erase(it);
        } else if (it->second.source_vk == image.image) {
          front_stale_.insert(it->first);
          FreeRetained(it->second);
          it = front_pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // fh1_native_lazy_depth. Without the image there is no copy to do: its texture becomes
    // stale (if something samples it before the next resolve, the guard trips).
    if (!pending_.empty() && image.image != VK_NULL_HANDLE) {
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.source_vk == image.image || it->second.target_vk == image.image) {
          stale_.insert(it->first);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // The DrawsVulkan framebuffers that use this view are destroyed before it: Vulkan may give the same
    // handle to a new view and FramebufferDe would return a stale one (see
    // fh1_native_framebuffers_forget_views). At shutdown, draws_ is already gone and so are its
    // framebuffers.
    if (draws_ && image.view != VK_NULL_HANDLE) {
      draws_->ForgetView(image.view);
    }
    if (draws_ && image.view_srgb != VK_NULL_HANDLE) {
      draws_->ForgetView(image.view_srgb);
    }
    if (image.view_srgb != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view_srgb, nullptr);
    image.view_srgb = VK_NULL_HANDLE;
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);
    if (image.memory_block != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.memory_block, nullptr);
    image = Image{};
  }

  // First time: to GENERAL (valid for copy, clear and sampling) and cleared to zero.
  void Prepare(Image& image) {
    if (image.prepared) {
      return;
    }
    // In the upload command buffer, which runs before the work one (the rear-view mirror cubemap copies
    // its resolved faces there and, on its first frame, read them unprepared).
    const VkCommandBuffer commands = CommandsUpload();
    if (commands == VK_NULL_HANDLE) {
      return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const bool depth = image.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                             image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    barrier.image = image.image;
    barrier.subresourceRange = depth ? kRangeDepth : kRangeColor;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    if (depth) {
      // ZCULL: images created without TRANSFER_DST (the ones eligible for a ZCULL plane) cannot be
      // cleared with vkCmdClearDepthStencilImage. They are cleared by opening a pass with
      // loadOp = CLEAR. The barrier above (UNDEFINED -> GENERAL) stays as it is: it is what makes the
      // driver zero the ZCULL plane, and without it the hardware kills the context.
      if (!image.accepts_target_of_copy && draws_) {
        if (!draws_->ClearDepthInPass(commands, image, 1.0f, 0) &&
            ++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT prepare the {}x{} depth by opening a clear pass (failure {})",
                      image.width, image.height, clears_in_pass_failed_);
        }
      } else if (clear_depth_) {
        const VkClearDepthStencilValue far_depth{1.0f, 0};
        BarrierGlobal(commands);
        clear_depth_(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &far_depth, 1,
                            &kRangeDepth);
      }
    } else {
      const VkClearColorValue zero{};
      BarrierGlobal(commands);
      dfn_.vkCmdClearColorImage(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &zero,
                                1, &kRangeColor);
    }
    image.prepared = true;
    if (draws_) {
      draws_->InvalidateTextures();  // TextureResolved only returns prepared ones
    }
  }

  bool Record() {
    if (recording_) {
      return true;
    }
    const auto before_record = std::chrono::steady_clock::now();
    const bool recording = BeginRecording();
    ns_record_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_record).count());
    ++recordings_;
    return recording;
  }

  bool BeginRecording() {
    // Slots rotate: while one frame is recorded, the previous ones can still be on the GPU. It only waits
    // if this slot's last submission has not finished yet, so the more slots there are, the further ahead
    // the CPU can get before it has to stop.
    slot_ = (slot_ + 1) % slots_used_;
    SlotWork& slot = slots_[slot_];
    const auto before_complete = std::chrono::steady_clock::now();
    Complete(slot);
    const auto before_pools = std::chrono::steady_clock::now();
    ns_complete_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(before_pools - before_complete).count());
    dfn_.vkResetCommandPool(device_, slot.pool_work, 0);
    dfn_.vkResetCommandPool(device_, slot.pool_upload, 0);
    ns_reset_pools_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_pools).count());
    commands_work_ = slot.work;
    commands_upload_ = slot.upload;
    recording_upload_ = false;
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(commands_work_, &start) != VK_SUCCESS) {
      return false;
    }
    slot.categories.clear();
    if (queries_ != VK_NULL_HANDLE) {
      dfn_.vkCmdResetQueryPool(commands_work_, queries_, slot_ * kMarksBySlot,
                               kMarksBySlot);
    }
    if (occlusions_ != VK_NULL_HANDLE) {
      DiscardOcclusions(slot);  // Complete already read the last submission's; these were never submitted
      dfn_.vkCmdResetQueryPool(commands_work_, occlusions_, slot_ * kOcclusionsBySlot,
                               kOcclusionsBySlot);
    }
    // fh1_reflection_visibility. Any left over belong to work that was never submitted: without a
    // measurement, they count as visible. The reset always happens when there is a pool, even if the
    // guard trips mid-frame.
    if (visibility_ != VK_NULL_HANDLE) {
      if (!slot.visibility.empty()) {
        fh1::reflection_demand::NoteVisible(false);
        slot.visibility.clear();
      }
      dfn_.vkCmdResetQueryPool(commands_work_, visibility_, slot_ * kVisibilityBySlot,
                               kVisibilityBySlot);
    }
    /*
     * Do not reset pools that this frame will not use.
     *
     * This used to happen always, with any toml: 64 statistics queries + 2048 per-draw statistics
     * queries = 2,144 resets per frame. And in our NVK on Tegra the bulk path is excluded for the layout
     * of these pools (nvk_query_pool.c:351: everything that is not a timestamp falls into
     * ALIGNED_INTERLEAVED), so each reset is a 5-dword SET_REPORT_SEMAPHORE with
     * RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE and PIPELINE_LOCATION_ALL: 2,144 chained writes that
     * wait on each other, 42.9 KB of command stream, at the start of every frame.
     *
     * And they are almost never used: the per-pass statistics are effectively disabled (see the `> 0`
     * check in BeginStatistics: with statistics_by_draw_s set, the per-pass query is never
     * opened), and the per-draw ones are only used one frame every 20 seconds.
     *
     * The reset comes before the first MarkGpu, so its cost does not show up in any category of the
     * breakdown: it is swallowed by the "gap between jobs" (gap between submissions). Estimated at
     * 0.3-1.1 real ms per frame; the measured upper bound (the minimum gap over a whole session) is
     * 2.81 ms.
     *
     * With both off, 32 resets remain instead of 2,144: -98.5 %. The CPU vectors are cleared anyway,
     * which costs nothing.
     */
    slot.statistics.clear();  // those never submitted are not read
    slot.statistics_draw.clear();
    if (statistics_ != VK_NULL_HANDLE && REXCVAR_GET(fh1_native_pipeline_statistics) &&
        REXCVAR_GET(fh1_native_per_draw_statistics_s) <= 0) {
      dfn_.vkCmdResetQueryPool(commands_work_, statistics_,
                               slot_ * kStatisticsBySlot, kStatisticsBySlot);
    }
    if (statistics_draw_ != VK_NULL_HANDLE && window_diagnostic_) {
      dfn_.vkCmdResetQueryPool(commands_work_, statistics_draw_,
                               slot_ * kStatisticsDrawBySlot,
                               kStatisticsDrawBySlot);
    }
    // The timestamp mode is decided per whole submission.
    if (toggle_marks_s_ > 0) {
      const int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - start_marks_)
                                   .count();
      const bool precise = (seconds / toggle_marks_s_) % 2 == 1;
      if (precise != marks_precise_) {
        marks_precise_ = precise;
        REXLOG_INFO("[native] timestamp test: {} (work {})", precise ? "precise" : "normal",
                    generation_commands_);
      }
    }
    slot.marks_precise = marks_precise_;
    recording_ = true;
    MarkGpu(kGpuOthers);
    ++generation_commands_;
    if (draws_) {
      draws_->UseSlot(slot_);
    }
    return true;
  }

  // Waits for that slot's submission. Earlier submissions that have already finished are collected
  // first, so read-backs are written in frame order.
  /*
   * This was what prevented CPU and GPU from overlapping.
   *
   * It used to wait as well for every earlier slot still pending, so each frame drained the whole GPU
   * before recording continued. That is why the report always said "overlapped jobs on the GPU 0",
   * why adding a third slot changed nothing (it waited for all of them, however many there were) and
   * why the wait moved elsewhere instead of disappearing.
   *
   * Now it only blocks on the slot about to be reused, the only one whose resources are needed. Earlier
   * ones are collected only if they have already finished (polling without waiting), so their
   * timestamps are read in order when possible; one still running is collected when its turn comes.
   */
  void Complete(SlotWork& slot) {
    for (SlotWork& other : slots_) {
      if (&other != &slot && other.pending && other.order < slot.order &&
          dfn_.vkGetFenceStatus(device_, other.fence) == VK_SUCCESS) {
        CompleteOne(other);
      }
    }
    CompleteOne(slot);
  }

  void CompleteOne(SlotWork& slot) {
    if (!slot.pending) {
      return;
    }
    const auto before_wait = std::chrono::steady_clock::now();
    WaitFenceWatched(dfn_, device_, slot.fence, "command buffer slot");
    ns_waits_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before_wait)
                                    .count());
    ++waits_gpu_;
    if (slot.sent.time_since_epoch().count()) {
      ns_work_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - slot.sent)
                                      .count());
      ++jobs_gpu_;
    }
    dfn_.vkResetFences(device_, 1, &slot.fence);
    slot.pending = false;
    const uint32_t marked = uint32_t(slot.categories.size());
    if (queries_ != VK_NULL_HANDLE && marked >= 2) {
      std::array<uint64_t, kMarksBySlot> marks{};
      const uint32_t index = uint32_t(&slot - slots_.data());
      if (read_queries_(device_, queries_, index * kMarksBySlot, marked,
                          sizeof(uint64_t) * marked, marks.data(), sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          marks[marked - 1] >= marks[0]) {
        for (uint32_t i = 0; i + 1 < marked; ++i) {
          if (marks[i + 1] >= marks[i] && slot.categories[i] < kGpuCategories) {
            gpu_categories_ns_[slot.categories[i]] +=
                uint64_t(double(marks[i + 1] - marks[i]) * period_mark_ns_);
          }
        }
        // GPU gap since the end of the previous submission (C2 report). Submissions are read in the order
        // they were sent; if one could not be read, mark_end_previous_ is 0 and that gap is not counted.
        if (mark_end_previous_ != 0) {
          if (marks[0] >= mark_end_previous_) {
            gpu_categories_ns_[kGpuGapBetweenJobs] +=
                uint64_t(double(marks[0] - mark_end_previous_) * period_mark_ns_);
          } else {
            ++overlaps_gpu_;
          }
        }
        mark_end_previous_ = marks[marked - 1];
        last_mark_gpu_ = marks[marked - 1];
        gpu_ns_ += uint64_t(double(marks[marked - 1] - marks[0]) * period_mark_ns_);
        ++gpu_jobs_;
        if (slot.marks_precise) {
          ++gpu_jobs_precise_;
        }
      } else {
        mark_end_previous_ = 0;  // without this submission's timestamps, the next gap is not counted
      }
    } else {
      mark_end_previous_ = 0;
    }
    slot.categories.clear();
    ReadOcclusions(slot);
    ReadVisibility(slot);
    ReadStatistics(slot);
    ReadStatisticsDraw(slot);
    WriteReads(slot.reads);
  }

  // Per-draw statistics of a finished submission, added to its pixel shader.
  void ReadStatisticsDraw(SlotWork& slot) {
    if (slot.statistics_draw.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.statistics_draw.size());
    std::vector<uint64_t> values(size_t(n) * kCountersStatistic, 0);
    if (statistics_draw_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, statistics_draw_, slot.statistics_draw.front().first, n,
                        sizeof(uint64_t) * values.size(), values.data(),
                        sizeof(uint64_t) * kCountersStatistic,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t label = slot.statistics_draw[i].second;
        fragments_by_shader_[label] += values[i * kCountersStatistic + 2];
        ++draws_by_shader_[label];
      }
      // A diagnostic frame may span several submissions: the first one of each window counts.
      if (!window_read_) {
        window_read_ = true;
        ++frames_diagnostic_;
      }
    }
    slot.statistics_draw.clear();
  }

  // Pass statistics of a finished submission, added to their category.
  void ReadStatistics(SlotWork& slot) {
    if (slot.statistics.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.statistics.size());
    std::array<uint64_t, kStatisticsBySlot * kCountersStatistic> values{};
    if (statistics_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, statistics_, slot.statistics.front().first, n,
                        sizeof(uint64_t) * kCountersStatistic * n, values.data(),
                        sizeof(uint64_t) * kCountersStatistic,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = slot.statistics[i].second;
        if (c < kGpuCategories) {
          vertices_category_[c] += values[i * kCountersStatistic + 0];
          primitives_category_[c] += values[i * kCountersStatistic + 1];
          fragments_category_[c] += values[i * kCountersStatistic + 2];
        }
      }
    }
    slot.statistics.clear();
  }

  // Occlusion queries of a finished submission. They add to their game query, which is published once
  // it has its Issue(END) and all its spans have been read.
  void ReadOcclusions(SlotWork& slot) {
    if (slot.occlusions.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.occlusions.size());
    std::array<uint64_t, kOcclusionsBySlot> counts{};
    const bool read_3 = occlusions_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, occlusions_, slot.occlusions.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = occlusions_game_.find(slot.occlusions[i].second);
      if (it == occlusions_game_.end()) {
        continue;
      }
      if (read_3) {
        it->second.sample_total += counts[i];
      } else {
        it->second.failed = true;
      }
      if (it->second.ranges_pending) {
        --it->second.ranges_pending;
      }
      if (it->second.finished && it->second.ranges_pending == 0) {
        PublishOcclusion(it);
      }
    }
    slot.occlusions.clear();
  }

  // fh1_reflection_visibility. Our own queries of a finished submission: for each water draw, whether
  // it left any sample; the witness must always leave some. If they cannot be read, the water counts as
  // visible and the witness counts neither for nor against.
  void ReadVisibility(SlotWork& slot) {
    if (slot.visibility.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.visibility.size());
    std::array<uint64_t, kVisibilityBySlot> counts{};
    const bool read_3 = visibility_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, visibility_, slot.visibility.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      if (slot.visibility[i].second == kVisibilityWitness) {
        if (read_3) {
          fh1::reflection_demand::NoteWitness(counts[i] != 0);
        }
      } else if (!read_3) {
        fh1::reflection_demand::NoteVisible(false);
      } else if (counts[i] != 0) {
        fh1::reflection_demand::NoteVisible(true);
      } else {
        fh1::reflection_demand::NoteHidden();
      }
    }
    slot.visibility.clear();
  }

  // Spans recorded in a submission that was never sent: their game query is not published.
  void DiscardOcclusions(SlotWork& slot) {
    for (const auto& [index, id] : slot.occlusions) {
      auto it = occlusions_game_.find(id);
      if (it != occlusions_game_.end()) {
        it->second.failed = true;
        if (it->second.ranges_pending) {
          --it->second.ranges_pending;
        }
        if (it->second.finished && it->second.ranges_pending == 0) {
          PublishOcclusion(it);
        }
      }
    }
    slot.occlusions.clear();
  }

  void PublishOcclusion(std::unordered_map<uint64_t, QueryOcclusionGame>::iterator it) {
    if (!it->second.failed) {
      if (occlusion_by_base_.size() >= 64 && !occlusion_by_base_.count(it->second.base)) {
        occlusion_by_base_.clear();
      }
      occlusion_by_base_[it->second.base] = it->second.sample_total;
      ++statistics_occlusion_[2];
      statistics_occlusion_[3] += it->second.sample_total;
      statistics_occlusion_[4] = std::max(statistics_occlusion_[4], it->second.sample_total);
    }
    occlusions_game_.erase(it);
  }

  bool SendWork(bool wait) {
    if (recording_) {
      if (draws_) {
        draws_->BeforeOfSend();  // closes the pass and publishes the upload buffer
      }
      recording_ = false;
      std::array<VkCommandBuffer, 2> buffers{};
      uint32_t n = 0;
      if (recording_upload_) {
        recording_upload_ = false;
        if (dfn_.vkEndCommandBuffer(commands_upload_) != VK_SUCCESS) {
          return false;
        }
        buffers[n++] = commands_upload_;
      }
      if (queries_ != VK_NULL_HANDLE) {
        // Final mark: closes the last span (MarkGpu leaves room for it).
        SlotWork& marked_2 = slots_[slot_];
        write_mark_(commands_work_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                        slot_ * kMarksBySlot + uint32_t(marked_2.categories.size()));
        marked_2.categories.push_back(kGpuFin);
      }
      if (dfn_.vkEndCommandBuffer(commands_work_) != VK_SUCCESS) {
        return false;
      }
      buffers[n++] = commands_work_;
      VkSubmitInfo submission{};
      submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submission.commandBufferCount = n;
      submission.pCommandBuffers = buffers.data();
      {
        const auto before_lock = std::chrono::steady_clock::now();  // "C2: present" report
        const auto queue = vulkan_device_->AcquireQueue(family_, 0);
        const auto before_submission = std::chrono::steady_clock::now();
        if (dfn_.vkQueueSubmit(queue.queue(), 1, &submission, slots_[slot_].fence) != VK_SUCCESS) {
          return false;
        }
        ns_lock_work_ += Ns(before_lock, before_submission);
        ns_submit_work_ += Ns(before_submission, std::chrono::steady_clock::now());
        ++submissions_work_;
      }
      SlotWork& slot = slots_[slot_];
      slot.pending = true;
      slot.order = ++submissions_;
      slot.sent = std::chrono::steady_clock::now();
      // Read-backs recorded in this submission are written when it finishes.
      slot.reads.insert(slot.reads.end(), reads_pending_.begin(),
                             reads_pending_.end());
      reads_pending_.clear();
    }
    if (wait) {
      Complete(slots_[slot_]);
    }
    return true;
  }

  void WaitGpu() {
    if (recording_) {
      SendWork(false);
    }
    for (SlotWork& slot : slots_) {
      Complete(slot);
    }
    WaitOutputs();
  }

  // All pending outputs, before destroying or reusing what they use.
  void WaitOutputs() {
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (outputs_pending_[i]) {
        WaitFenceWatched(dfn_, device_, fences_output_[i], "all output slots");
        dfn_.vkResetFences(device_, 1, &fences_output_[i]);
        outputs_pending_[i] = false;
      }
    }
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
  // FH1 debug: "x.png" -> "x_float_tm.png" (a float image, tone-mapped), and its numbers in the log.
  static std::string DumpNameFloat(const std::string& name) {
    const size_t dot = name.rfind('.');
    return name.substr(0, dot) + "_float_tm" + (dot == std::string::npos ? "" : name.substr(dot));
  }
  static void DumpLogFloat(const std::string& name, const DumpFloatStats& stats) {
    const double n = double(std::max<uint64_t>(stats.pixels, 1));
    REXLOG_INFO("[fh1] dump_resolved float {}: max {:.3f} {:.3f} {:.3f} {:.3f} mean {:.4f} {:.4f} {:.4f} {:.4f} "
                "negative {} nan {}",
                name, stats.maximum[0], stats.maximum[1], stats.maximum[2], stats.maximum[3], stats.sum[0] / n,
                stats.sum[1] / n, stats.sum[2] / n, stats.sum[3] / n, stats.negative, stats.nan);
  }
  // NFSC debug: one colour image to dump_resolved/<name>.png (waits for the GPU; debug only).
  void DumpImage(const Image& image, const std::string& name) {
    const uint32_t bytes_texel = DumpBytesTexel(image.format);
    if (!image.prepared || !bytes_texel || !Record()) {
      return;
    }
    const VkDeviceSize bytes = VkDeviceSize(image.width) * image.height * bytes_texel;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory_block = VK_NULL_HANDLE;
    uint32_t type = 0;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory_block, &type)) {
      return;
    }
    void* mapped = nullptr;
    if (dfn_.vkMapMemory(device_, memory_block, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {image.width, image.height, 1};
    dfn_.vkCmdCopyImageToBuffer(commands_work_, image.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    SendWork(true);
    WaitGpu();
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> alpha;
    std::string name_file = name;
    if (image.format == kFormatColor) {
      rgba.resize(size_t(bytes));
      std::memcpy(rgba.data(), mapped, rgba.size());
      alpha.resize(rgba.size());
      for (size_t i = 0; i < rgba.size(); i += 4) {
        alpha[i] = alpha[i + 1] = alpha[i + 2] = rgba[i + 3];
        alpha[i + 3] = 255;
      }
      for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    } else {  // FH1: a float image, tone-mapped for the PNG
      DumpFloatStats stats;
      DumpFloatToRgba(image.format, static_cast<const uint8_t*>(mapped), image.width, image.height, rgba, alpha,
                      stats);
      name_file = DumpNameFloat(name);
      DumpLogFloat(name_file, stats);
    }
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::current_path() / dump_folder_;
    std::filesystem::create_directories(folder, ec);
    FH1_DUMP_LOG(folder, name_file, rgba, image.width, image.height);
    FH1_DUMP_LOG(folder, std::string("alpha_") + name_file, alpha, image.width, image.height);
    dfn_.vkUnmapMemory(device_, memory_block);
    dfn_.vkDestroyBuffer(device_, buffer, nullptr);
    dfn_.vkFreeMemory(device_, memory_block, nullptr);
  }
  uint32_t dump_res_done_ = 0;
  std::string dump_folder_ = "dump_resolved";
  uint32_t dump_count_ = 0;
  bool dump_rt_pending_ = false;
  bool dump_photo_request_ = false;
  uint32_t dump_rt_done_ = 0;
  bool dump_frame_pending_ = false;
  uint32_t dump_frame_done_ = 0;

  // NFSC debug (fh1_dump_resolved_at_s): once, every resolved colour image goes to a PNG file, read straight from
  // the GPU. The names say address, size and the order the frame resolved them.
  void DumpResolvedSiDue(const Image* presented_2) {
    static bool done = false;
    static const auto start = std::chrono::steady_clock::now();
    const int32_t seconds = REXCVAR_GET(fh1_dump_resolved_at_s);
    dump_frame_pending_ = false;  // the frame after the dump has ended
    if (seconds == -1) {
      // NFSC: -1 = when a file named dump_now appears in the working folder (a script creates it at the right moment);
      // checked twice a second, the file is removed.
      static uint32_t count = 0;
      std::error_code ec;
      if (!dump_photo_request_ && (++count % 30 != 0 || !std::filesystem::remove("dump_now", ec))) {
        return;
      }
      dump_photo_request_ = false;  // fh1_native_diag_photo
      // FH1: on demand it can be asked again; each dump after the first gets its own folder (dump_resolved_2, ...).
      if (++dump_count_ > 1) {
        dump_folder_ = "dump_resolved_" + std::to_string(dump_count_);
      }
      dump_res_done_ = dump_rt_done_ = dump_frame_done_ = 0;
    } else if (done || seconds <= 0 ||
               std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
      return;
    }
    done = true;
    dump_rt_pending_ = true;  // the next colour resolves also dump their render target before it is cleared
    dump_frame_pending_ = true;
    if (draws_) {
      draws_->FinishPass();
    }
    struct DumpEntry {
      uint32_t base, width, height;
      VkFormat format = kFormatColor;
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory memory_block = VK_NULL_HANDLE;
      uint8_t* data = nullptr;
    };
    std::vector<DumpEntry> list;
    if (!Record()) {
      return;
    }
    std::vector<std::pair<uint32_t, const Image*>> sources;
    for (auto& [base, t] : resolved_) {
      sources.push_back({base, &t.image});
    }
    for (auto& [base, bytes] : depth_bytes_) {  // FH1: resolved depths as bytes, shown as 0xDB000000 | address bits
      sources.push_back({0xDB000000u | (base >> 8), &bytes.image});
    }
    {  // the render targets themselves: "base" is shown as 0xDD000000 | key bits, to tell them apart
      uint32_t i = 0;
      for (auto& [key, image] : targets_) {
        sources.push_back({0xDD000000u | (i++ << 16), &image});
        REXLOG_INFO("[fh1] dump_resolved: render target #{} key {:016X} {}x{}", i - 1, key, image.width, image.height);
      }
    }
    for (auto& [base, image_ptr] : sources) {
      const Image& image = *image_ptr;
      const uint32_t bytes_texel = DumpBytesTexel(image.format);
      if (!image.prepared || !bytes_texel) {
        continue;
      }
      DumpEntry v{base, image.width, image.height, image.format};
      uint32_t type = 0;
      const VkDeviceSize bytes = VkDeviceSize(v.width) * v.height * bytes_texel;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kReadback, v.buffer, v.memory_block, &type)) {
        continue;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, v.memory_block, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        continue;
      }
      v.data = static_cast<uint8_t*>(mapped);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {v.width, v.height, 1};
      dfn_.vkCmdCopyImageToBuffer(commands_work_, image.image, VK_IMAGE_LAYOUT_GENERAL, v.buffer, 1, &copy);
      list.push_back(v);
    }
    SendWork(true);
    WaitGpu();
    std::error_code ec;
    const std::filesystem::path folder = std::filesystem::current_path() / dump_folder_;
    std::filesystem::create_directories(folder, ec);
    uint32_t n = 0;
    for (DumpEntry& v : list) {
      char name[96];
      std::snprintf(name, sizeof(name), "%02u_%08X_%ux%u.png", n++, v.base, v.width, v.height);
      if (v.format == kFormatColor) {
        std::vector<uint8_t> rgba(size_t(v.width) * v.height * 4);
        std::memcpy(rgba.data(), v.data, rgba.size());
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        FH1_DUMP_LOG(folder, name, rgba, v.width, v.height);
      } else {  // FH1: a float image, tone-mapped for the PNG
        std::vector<uint8_t> rgba, alpha;
        DumpFloatStats stats;
        DumpFloatToRgba(v.format, v.data, v.width, v.height, rgba, alpha, stats);
        const std::string name_float = DumpNameFloat(name);
        DumpLogFloat(name_float, stats);
        FH1_DUMP_LOG(folder, name_float, rgba, v.width, v.height);
        if (v.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
          FH1_DUMP_LOG(folder, std::string("alpha_") + name_float, alpha, v.width, v.height);
        }
      }
      dfn_.vkUnmapMemory(device_, v.memory_block);
      dfn_.vkDestroyBuffer(device_, v.buffer, nullptr);
      dfn_.vkFreeMemory(device_, v.memory_block, nullptr);
    }
    if (presented_2) {
      DumpImage(*presented_2, "presented_front.png");
      REXLOG_INFO("[fh1] dump_resolved: the presented image is {}x{}", presented_2->width, presented_2->height);
    } else {
      REXLOG_INFO("[fh1] dump_resolved: the Swap presents the resolved texture itself");
    }
    REXLOG_INFO("[fh1] dump_resolved: {} images saved in {}", n, folder.string());
  }

  // Diagnostic (fh1_native_diag_resolved): the frame's resolved textures in a 4x4 grid over
  // 1280x720, in the order of their first copy. Cells without a texture stay dark purple.
  bool ComposeTile(uint32_t base_swap) {
    std::vector<uint32_t> bases;
    bases.swap(resolved_frame_);
    if (bases.empty() || !blit_) {
      return false;
    }
    if (tile_.image == VK_NULL_HANDLE &&
        !Create(tile_, 1280, 720, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      blit_ = nullptr;
      return false;
    }
    if (draws_) {
      draws_->FinishPass();
    }
    if (!Record()) {
      return false;
    }
    Prepare(tile_);
    const VkClearColorValue background{{0.12f, 0.0f, 0.12f, 1.0f}};
    BarrierGlobal(commands_work_);
    dfn_.vkCmdClearColorImage(commands_work_, tile_.image, VK_IMAGE_LAYOUT_GENERAL, &background,
                              1, &kRangeColor);
    std::string list;
    for (size_t i = 0; i < bases.size() && i < 16; ++i) {
      const auto it = resolved_.find(bases[i]);
      if (it == resolved_.end() || !it->second.image.prepared ||
          it->second.image.format != kFormatColor) {
        continue;  // depth ones do not support blits to color
      }
      const Image& image = it->second.image;
      const int32_t x = int32_t(i % 4) * 320;
      const int32_t y = int32_t(i / 4) * 180;
      VkImageBlit copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.srcOffsets[1] = {int32_t(image.width), int32_t(image.height), 1};
      copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.dstOffsets[0] = {x + 2, y + 2, 0};
      copy.dstOffsets[1] = {x + 318, y + 178, 1};
      BarrierGlobal(commands_work_);
      blit_(commands_work_, image.image, VK_IMAGE_LAYOUT_GENERAL, tile_.image,
            VK_IMAGE_LAYOUT_GENERAL, 1, &copy, VK_FILTER_LINEAR);
      list += fmt::format(" {}{}:{:08X} {}x{}", i, bases[i] == base_swap ? "*" : "", bases[i],
                           image.width, image.height);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_warning_tile_ >= std::chrono::seconds(10)) {
      last_warning_tile_ = now;
      REXLOG_INFO("[native] resolved diag: {} in the frame (* = the Swap's):{}", bases.size(),
                  list);
    }
    return true;
  }

  // Rear-view mirror diagnostic: for each resolved texture, how many copies there are and how many had
  // draws since the previous copy (the car's cubemap faces are 256x256). A face resolved without draws
  // copies again what the render target already had.
  // It is no longer limited to square textures up to 512. The copy inventory has to be complete
  // (address, size, Mpixels and reads) because it is the only thing that tells whether a copy is
  // needed. There are ~20 addresses in a race, which fit easily in one line.
  void NoteCopy(uint32_t base, uint32_t width, uint32_t height, uint64_t draws) {
    if (base == fh1::reflection_demand::kAddress) {
      fh1::reflection_demand::NoteCopy();  // fh1_reflection_low_demand
    }
    CopyTarget& d = copies_by_target_[base];
    d.width = width;
    d.height = height;
    ++d.copies;
    d.draws += draws;
    d.with_draws += draws != 0 ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - report_copies_ < std::chrono::seconds(10)) {
      return;
    }
    report_copies_ = now;
    std::vector<std::pair<uint32_t, CopyTarget>> order(copies_by_target_.begin(),
                                                         copies_by_target_.end());
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string list;
    uint32_t written = 0;
    for (const auto& [address, c] : order) {
      if (++written > 48) {  // defensive cap: ~20 in a race, but a load can touch many more
        list += fmt::format(" (and {} more addresses)", order.size() - 48);
        break;
      }
      const auto r = resolved_.find(address);
      const uint64_t reads = r != resolved_.end() ? r->second.reads : 0;
      if (r != resolved_.end()) {
        r->second.reads = 0;
      }
      list += fmt::format(" {:08X} {}x{}: {} copies, {} with draws ({:.1f} draws per copy), {:.2f} Mpixels, {} "
                          "reads{};",
                           address, c.width, c.height, c.copies, c.with_draws,
                           c.copies ? double(c.draws) / double(c.copies) : 0.0,
                           double(c.pixels) / 1e6, reads,
                           c.copies && !reads ? " *** NADIE LA LEE ***" : "");
    }
    copies_by_target_.clear();
    FH1_REPORT_RING("[native] C2 faces resolved since the previous report:{}", list);
    if (swaps_ || restores_) {
      // Restores are the verdict. If they go up, the swap without a clear saves nothing: the same copy is
      // paid, just later.
      FH1_REPORT_RING("[native] C2 resolves without copying: {} swaps ({} without clearing, {} of COLOR) and {} "
                       "restores since startup -> {}",
                  swaps_, swaps_without_clear_, swaps_color_, restores_,
                  restores_ == 0 ? "not a single restore: the saving is clean"
                                       : "*** there are restores: the saving is NOT clean ***");
      // fh1_native_resolve_valid_content. In the menu, one per frame with shadows; in a race, 0. The
      // "without source" count is resolves of a render target whose content is no longer anywhere (must be
      // 0); the other count is the resolves that, with the setting off, would read the stale image.
      if (restores_for_resolve_ || resolve_without_source_ || resolve_content_old_) {
        FH1_REPORT_RING("[native] C2 resolve with valid content (build 193): {} times the content was brought "
                         "back before resolving (before, the resolve read the previous frame's image: the menu "
                         "flicker); {} without a source; {} read as in 192 (setting off)",
                             restores_for_resolve_, resolve_without_source_, resolve_content_old_);
      }
      // And what they cost per frame, which is the only thing that decides. 0.78 real ms per Mpixel
      // copied and 0.075 per Mpixel cleared, measured here.
      const double frames = double(presented_ - presented_report_copies_);
      const double byFrame = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp_rest = double(pixels_restored_ - pixels_restored_previous_) / 1e6;
      const double mp_saving =
          double(pixels_restore_saved_ - pixels_restore_saved_previous_) / 1e6;
      FH1_REPORT_RING("[native] C2 restores per frame: {:.2f} Mpixels copied ({:.2f} real ms) and {:.2f} Mpixels "
                       "saved ({:.2f} ms); since startup: {} trimmed to the useful area and {} by swap (lent reads "
                       "{}{})",
                  mp_rest * byFrame, mp_rest * byFrame * 0.78, mp_saving * byFrame,
                  mp_saving * byFrame * 0.78, restores_clipped_, loans_,
                  lent_read_,
                  lent_read_ ? " *** THE LENDING IS NOT SAFE ***" : ", clean");
      // And why the restore does not swap, which is what has to be known to fix it.
      if (no_swap_sin_transfer_dst_ || no_swap_target_without_prepare_ ||
          no_swap_resolved_without_prepare_ || no_swap_sizes_different_) {
        FH1_REPORT_RING(
            "[native] C2 restore by swap, times it could NOT: {} without TRANSFER_DST, {} with the target not "
            "prepared, {} with the resolved one not prepared; and of the ones that qualified, {} had different "
            "sizes (those would break the image)",
            no_swap_sin_transfer_dst_, no_swap_target_without_prepare_,
            no_swap_resolved_without_prepare_, no_swap_sizes_different_);
      }
      pixels_restored_previous_ = pixels_restored_;
      pixels_restore_saved_previous_ = pixels_restore_saved_;
    }
    if (clears_skipped_ || clears_skipped_depth_) {
      // Clears that did not change a single bit (fh1_native_skip_repeated_clears). A clear costs
      // 0.075 real ms per Mpixel: ten times less than a copy, but it adds up.
      const double frames = double(presented_ - presented_report_copies_);
      const double byFrame = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(pixels_clears_skipped_ - pixels_clears_skipped_previous_) / 1e6;
      FH1_REPORT_RING("[native] C2 clears skipped: {} color and {} depth since startup; {:.2f} Mpixels per frame "
                       "not cleared ({:.2f} real ms)",
                  clears_skipped_, clears_skipped_depth_, mp * byFrame,
                  mp * byFrame * 0.075);
      pixels_clears_skipped_previous_ = pixels_clears_skipped_;
    }
    if (clears_useless_) {
      // Clears wiped out by a swap or a restore before anything was drawn. These are the real candidates
      // for removal (the equivalent of a loadOp = DONT_CARE).
      const double frames = double(presented_ - presented_report_copies_);
      const double byFrame = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(pixels_clears_useless_ - pixels_clears_useless_previous_) / 1e6;
      FH1_REPORT_RING("[native] C2 clears that were useless (the target changed content without anything being "
                       "drawn): {} since startup; {:.2f} Mpixels per frame ({:.2f} real ms)",
                  clears_useless_, mp * byFrame, mp * byFrame * 0.075);
      pixels_clears_useless_previous_ = pixels_clears_useless_;
    }
    if (lazy_deferred_ || lazy_reads_late_) {  // fh1_native_lazy_depth
      // 0.60 real ms per Mpixel copied: the 1600x1600 shadow map copy raised copies from 1.93 to 3.47 ms.
      const double frames = double(presented_ - presented_report_copies_);
      const double byFrame = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(lazy_pixels_saved_ - lazy_pixels_saved_previous_) / 1e6;
      FH1_REPORT_RING("[native] C2 lazy depth (build 184): {} depth resolves deferred since startup; {} recorded "
                       "when sampled, {} before their source was written again or another resolve, {} replaced by "
                       "a swap and {} dropped without copying (nobody sampled them): {:.2f} Mpixels per frame not "
                       "copied (~{:.2f} real ms); late reads {}{}",
                           lazy_deferred_, lazy_copied_read_, lazy_copied_write_,
                           lazy_replaced_, lazy_dropped_, mp * byFrame, mp * byFrame * 0.60,
                           lazy_reads_late_,
                           lazy_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      lazy_pixels_saved_previous_ = lazy_pixels_saved_;
    }
    if (front_deferred_ || front_reads_late_) {  // fh1_native_lazy_front
      const double frames = double(presented_ - presented_report_copies_);
      const double byFrame = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(front_pixels_saved_ - front_pixels_saved_previous_) / 1e6;
      FH1_REPORT_RING("[native] C2 lazy front buffer (build 184): {} copies to front buffers deferred since "
                       "startup; Swaps painted from the target {} and from a retained image {}; {} clears on a "
                       "spare image ({} without a spare); recorded: {} when sampled, {} before the target was "
                       "written again or another resolve, {} at the Swap (FXAA, no ramp or another output); {} "
                       "dropped without copying (another resolve covers them whole): {:.2f} Mpixels per frame not "
                       "copied (~{:.2f} real ms); {} spare images; late reads {}{}",
                           front_deferred_, front_painted_target_, front_painted_retained_,
                           front_rotations_, front_without_spare_, front_copied_read_,
                           front_copied_write_, front_copied_swap_, front_replaced_, mp * byFrame,
                           mp * byFrame * 0.60, front_images_.size(), front_reads_late_,
                           front_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      front_pixels_saved_previous_ = front_pixels_saved_;
    }
    ReportShadowMinimum();  // fh1_native_shadow_minimum
    ReportClears();  // fh1_native_diag_clears, every 20 s
    presented_report_copies_ = presented_;
    if (without_swap_[0] || without_swap_[1] || without_swap_[2]) {
      FH1_REPORT_RING("[native] C2 depth copies not swapped since the previous report: {} because the command "
                       "does not clear the target, {} because it is not resolved whole, {} because it could not be "
                       "done",
                  without_swap_[0], without_swap_[1], without_swap_[2]);
      without_swap_ = {};
    }
  }

  // Read-back of a small resolved texture: the copied rectangle goes to a host-visible buffer in the
  // same submission; WriteReads moves it into guest memory.
  void ReadResolved(const RegistersCopy& reg, const Resolved& resolved, int32_t x0, int32_t y0,
                    uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
    const int32_t maximum = REXCVAR_GET(fh1_native_read_resolved_texels);
    // FH1: the small float pictures too (fh1_native_read_resolved_float): the levels of the reflection cube map
    // resolved as k_2_10_10_10 and the luminance resolved as k_32_FLOAT. On the console every resolve lands in
    // memory, where the game's own code can read it; here those addresses kept whatever was there before.
    const uint32_t format_guest_read = (reg.rb_copy_dest_info >> 7) & 0x3F;
    const bool float_read =
        REXCVAR_GET(fh1_native_read_resolved_float) &&
        ((resolved.image.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
          format_guest_read == uint32_t(xenos::ColorFormat::k_2_10_10_10)) ||
         (resolved.image.format == VK_FORMAT_R32_SFLOAT &&
          format_guest_read == uint32_t(xenos::ColorFormat::k_32_FLOAT)));
    // FH1: a 16-bit float picture the game reads on the CPU (fh1_native_read_resolved_half_texels).
    const int32_t maximum_half = REXCVAR_GET(fh1_native_read_resolved_half_texels);
    const bool half_read = maximum_half > 0 && resolved.image.format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                           format_guest_read == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT) &&
                           uint64_t(width) * height <= uint64_t(maximum_half);
    const uint64_t key_target =
        (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(width) << 14) ^ uint64_t(height);
    // FH1 (fh1_native_read_one_off): a picture the game makes once (the photo of a car) is read by the game's own
    // code right after its resolve. The first resolves to a destination that has been quiet for 60 frames go to
    // guest memory at once, in the format the game asked for.
    bool one_off = false;
    const int32_t one_off_resolves = REXCVAR_GET(fh1_native_read_one_off);
    if (one_off_resolves > 0) {
      OneOff& seen = one_off_[key_target];
      const uint64_t frame = presented_ + 1;
      if (seen.frame == 0 || seen.frame + 60 < frame) {
        // A destination that keeps coming back every few seconds (the festival's 384x128 screens, crowd pictures)
        // is an effect, not a photo: waiting for the GPU each time cost a late frame every few seconds
        // (28.6-29.6 fps at the festival). After two returns within 20 s it follows the old rules for good.
        if (seen.frame != 0 && seen.frame + 600 >= frame && seen.returns < 2) {
          ++seen.returns;
        }
        seen.streak = 0;
      }
      seen.frame = frame;
      if (seen.returns < 2 && seen.streak < uint32_t(one_off_resolves)) {
        ++seen.streak;
        const VkFormat host = resolved.image.format;
        const bool known =
            (host == kFormatColor && (format_guest_read == uint32_t(xenos::ColorFormat::k_8_8_8_8) ||
                                      format_guest_read == uint32_t(xenos::ColorFormat::k_2_10_10_10))) ||
            (host == VK_FORMAT_R16G16B16A16_SFLOAT &&
             (format_guest_read == uint32_t(xenos::ColorFormat::k_2_10_10_10) ||
              format_guest_read == uint32_t(xenos::ColorFormat::k_8_8_8_8) ||
              format_guest_read == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT))) ||
            (host == VK_FORMAT_R32_SFLOAT && format_guest_read == uint32_t(xenos::ColorFormat::k_32_FLOAT));
        one_off = known && uint64_t(width) * height <= (uint64_t(1) << 22);
        if (one_off_logged_++ < 3000) {
          REXLOG_INFO("[fh1] one-off resolve {}: {:08X}, {}x{} at ({},{}), guest format {}, host format {}, "
                      "frame {}, {} in a row, dest info {:08X} pitch {:08X} control {:08X}",
                      one_off ? "goes to guest memory" : "NOT written (format pair not handled)",
                      reg.rb_copy_dest_base, width, height, x0, y0, format_guest_read, uint32_t(host), presented_,
                      seen.streak, reg.rb_copy_dest_info, reg.rb_copy_dest_pitch, reg.rb_copy_control);
        }
      }
    }
    const bool old_rules = half_read || !(maximum <= 0 || uint64_t(width) * height > uint64_t(maximum) ||
                                          (resolved.image.format != kFormatColor && !float_read));
    if (!one_off && !old_rules) {
      return;
    }
    const uint64_t texels_texture =
        uint64_t(reg.rb_copy_dest_pitch & 0x3FFF) * ((reg.rb_copy_dest_pitch >> 16) & 0x3FFF);
    const bool one_off_waits =
        one_off && texels_texture >= uint64_t(std::max(REXCVAR_GET(fh1_native_read_one_off_wait_texels), 0));
    if (one_off && REXCVAR_GET(fh1_native_diag_photo) && (reg.rb_copy_dest_pitch & 0x3FFF) == 768 &&
        ((reg.rb_copy_dest_pitch >> 16) & 0x3FFF) == 288) {
      std::ofstream("trace_now").put('1');
      dump_photo_request_ = true;
    }
    TargetRead& target = reads_by_target_[key_target];
    target.base = reg.rb_copy_dest_base;
    target.width = width;
    target.height = height;
    const uint64_t every = uint64_t(std::max(REXCVAR_GET(fh1_native_reads_every), 1));
    if (target.copies++ % every != 0 && !one_off) {
      ++target.skipped;  // exposure changes slowly: the guest keeps the previous one
      return;
    }
    const VkDeviceSize bytes =
        VkDeviceSize(width) * height * (resolved.image.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4);
    const uint64_t key = (uint64_t(reg.rb_copy_dest_base) << 32) ^ (uint64_t(uint32_t(x0)) << 16) ^
                           uint64_t(uint32_t(y0));
    // One buffer per slot: the previous submission may still be copying into the other slot's.
    ReadAccess* pointer = &reads_[key ^ (uint64_t(slot_) << 63)];
    if (pointer->bytes < bytes && pointer->buffer != VK_NULL_HANDLE) {
      // Larger: the old buffer may have recorded or submitted copies.
      SendWork(true);
      WaitGpu();
      DestroyRead(*pointer);
      if (!Record()) {
        return;
      }
      pointer = &reads_[key ^ (uint64_t(slot_) << 63)];
      if (pointer->bytes < bytes && pointer->buffer != VK_NULL_HANDLE) {
        DestroyRead(*pointer);  // the GPU has nothing pending any more
      }
    }
    ReadAccess& read = *pointer;
    if (read.bytes < bytes) {
      uint32_t type = 0;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kReadback, read.buffer, read.memory_block, &type)) {
        read = ReadAccess{};
        Reject(16, "could not create a readback buffer for a resolved texture");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, read.memory_block, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        DestroyRead(read);
        Reject(16, "could not create a readback buffer for a resolved texture");
        return;
      }
      read.data = static_cast<uint8_t*>(mapped);
      read.bytes = bytes;
      read.coherent = (vulkan_device_->memory_types().host_coherent >> type) & 0x1;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {int32_t(dx), int32_t(dy), 0};
    copy.imageExtent = {width, height, 1};
    dfn_.vkCmdCopyImageToBuffer(commands_work_, resolved.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                read.buffer, 1, &copy);
    if (uint64_t(width) * height <= 1024 || half_read || one_off_waits) {
      reads_urgent_ = true;  // FinishReads
    }
    if (one_off && bytes > 16384) {
      reads_once_.push_back(key);  // a large buffer used a few times: FinishReads frees it
    }
    if (half_read && reads_half_++ < 8) {
      REXLOG_INFO("[fh1] read-back of a 16-bit float resolve: {:08X}, {}x{} (the game reads it on the CPU)",
                  reg.rb_copy_dest_base, width, height);
    }
    reads_pending_.push_back({&read, reg.rb_copy_dest_base, x0, y0, width, height,
                                    reg.rb_copy_dest_pitch & 0x3FFF,
                                    (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info,
                                    resolved.image.format});
    if (reads_done_++ == 0) {
      REXLOG_INFO("[native] C2: readback of resolved textures of up to {} texels (the first one: {:08X}, {}x{})",
                  maximum, reg.rb_copy_dest_base, width, height);
    }
  }

  // The submission finished: the submitted read-backs go to guest memory. Loaded with its fetch
  // constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the resolved
  // texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
  void WriteReads(std::vector<ReadPending>& reads) {
    const auto before_reads = std::chrono::steady_clock::now();
    if (before_reads - report_reads_ >= std::chrono::seconds(10)) {
      std::string list;
      for (auto it = reads_by_target_.begin(); it != reads_by_target_.end();) {
        TargetRead& d = it->second;
        if (!d.copies) {
          it = reads_by_target_.erase(it);  // no ha vuelto a aparecer
          continue;
        }
        list += fmt::format(" {:08X} {}x{} {}/{};", d.base, d.width, d.height, d.copies - d.skipped,
                             d.copies);
        d.copies = 0;
        d.skipped = 0;
        ++it;
      }
      if (!list.empty()) {
        FH1_REPORT_RING("[native] C2 readbacks per target (done/copies):{}", list);
      }
      report_reads_ = before_reads;
    }
    for (const ReadPending& p : reads) {
      const ReadAccess& read = *p.read;
      if (!read.data) {
        continue;
      }
      ++reads_written_;
      texels_written_ += uint64_t(p.width) * p.height;
      if (!read.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = read.memory_block;
        range.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
      }
      const bool swap_value = (p.info >> 24) & 0x1;
      const uint32_t order_copy = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
      const auto order =
          order_copy <= 3 ? static_cast<xenos::Endian>(order_copy) : xenos::Endian::k8in32;
      // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap): it is computed once with
      // marked bytes. Addresses come from the tile and the tiling table, and the guest's physical memory
      // is contiguous.
      const uint32_t permutation =
          xenos::GpuSwap(swap_value ? uint32_t(0x03000102) : uint32_t(0x03020100), order);
      const uint8_t b0 = uint8_t(permutation), b1 = uint8_t(permutation >> 8),
                    b2 = uint8_t(permutation >> 16), b3 = uint8_t(permutation >> 24);
      uint8_t* const physical = memory_->TranslatePhysical(0);
      const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
      const auto& table = TableTile2DTexel4();
      const uint64_t tiles_by_row = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
      // FH1: float pictures. k_2_10_10_10: each channel times 2^exp_bias, cut to 0..1, in 10 bits (alpha in 2), red
      // and blue exchanged with copy_dest_swap; k_32_FLOAT: the value as it is. Then the copy's byte order.
      if (p.format_host == VK_FORMAT_R16G16B16A16_SFLOAT &&
          ((p.info >> 7) & 0x3F) == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT)) {
        // FH1: sixteen-bit floats, eight bytes per texel: two words (red | green << 16, blue | alpha << 16), red
        // and blue exchanged with copy_dest_swap, each word in the copy's byte order. The console's half float has
        // no infinity: one is written as the largest number, a NaN as 0. Tiled like every texture.
        const int32_t bias = int32_t((p.info >> 16) << 26) >> 26;
        const uint32_t pitch_tiles = uint32_t(tiles_by_row);
        for (uint32_t j = 0; j < p.height; ++j) {
          const uint32_t ty = uint32_t(p.y0) + j;
          if (p.height_target && ty >= p.height_target) {
            break;
          }
          const uint8_t* s = read.data + size_t(j) * p.width * 8;
          for (uint32_t i = 0; i < p.width; ++i, s += 8) {
            const uint32_t tx = uint32_t(p.x0) + i;
            if (tx >= p.pitch) {
              break;
            }
            const uint64_t address = base + TiledOffsetTexel8(tx, ty, pitch_tiles);
            if (address + 8 > 0x20000000) {
              continue;
            }
            uint16_t h[4];
            std::memcpy(h, s, sizeof(h));
            for (uint32_t c = 0; c < 4; ++c) {
              uint32_t exponent = (h[c] >> 10) & 0x1F;
              if (exponent == 0x1F) {
                h[c] = (h[c] & 0x3FF) ? uint16_t(0) : uint16_t((h[c] & 0x8000) | 0x7FFF);
              } else if (bias != 0 && c < 3 && exponent != 0) {
                const int32_t moved = std::clamp(int32_t(exponent) + bias, 0, 31);
                h[c] = moved == 0 ? uint16_t(h[c] & 0x8000) : uint16_t((h[c] & 0x83FF) | (uint32_t(moved) << 10));
              }
            }
            if (swap_value) {
              std::swap(h[0], h[2]);
            }
            const uint32_t words[2] = {xenos::GpuSwap(uint32_t(h[0]) | (uint32_t(h[1]) << 16), order),
                                       xenos::GpuSwap(uint32_t(h[2]) | (uint32_t(h[3]) << 16), order)};
            std::memcpy(physical + address, words, sizeof(words));
          }
        }
        RestampAfterRead(p);
        continue;
      }
      if (p.format_host != VK_FORMAT_R8G8B8A8_UNORM) {
        const bool wide = p.format_host == VK_FORMAT_R16G16B16A16_SFLOAT;
        const bool to_8 = ((p.info >> 7) & 0x3F) == uint32_t(xenos::ColorFormat::k_8_8_8_8);
        const float scale = std::ldexp(1.0f, int32_t((p.info >> 16) << 26) >> 26);
        for (uint32_t j = 0; j < p.height; ++j) {
          const uint32_t ty = uint32_t(p.y0) + j;
          if (p.height_target && ty >= p.height_target) {
            break;
          }
          const uint64_t row = base + ((uint64_t(ty >> 5) * tiles_by_row) << 12);
          const uint16_t* const local = table.data() + size_t(ty & 31) * 32;
          const uint8_t* s = read.data + size_t(j) * p.width * (wide ? 8 : 4);
          for (uint32_t i = 0; i < p.width; ++i, s += wide ? 8 : 4) {
            const uint32_t tx = uint32_t(p.x0) + i;
            if (tx >= p.pitch) {
              break;
            }
            const uint64_t address = row + (uint64_t(tx >> 5) << 12) + local[tx & 31];
            if (address + 4 > 0x20000000) {
              continue;
            }
            uint32_t word = 0;
            if (wide) {
              uint16_t h[4];
              std::memcpy(h, s, sizeof(h));
              uint32_t field[4];
              for (uint32_t c = 0; c < 4; ++c) {
                float v = DumpHalfToFloat(h[c]) * (c < 3 ? scale : 1.0f);
                v = v > 0.0f ? std::min(v, 1.0f) : 0.0f;  // also NaN
                field[c] = uint32_t(v * (c < 3 ? 1023.0f : 3.0f) + 0.5f);
              }
              if (swap_value) {
                std::swap(field[0], field[2]);
              }
              word = field[0] | (field[1] << 10) | (field[2] << 20) | (field[3] << 30);
              if (to_8) {
                // FH1: a float picture resolved as k_8_8_8_8: one byte per channel, in the 8-bit path's order.
                uint8_t c8[4];
                for (uint32_t c = 0; c < 4; ++c) {
                  float v = DumpHalfToFloat(h[c]) * (c < 3 ? scale : 1.0f);
                  v = v > 0.0f ? std::min(v, 1.0f) : 0.0f;
                  c8[c] = uint8_t(v * 255.0f + 0.5f);
                }
                const uint8_t t[4] = {c8[b0], c8[b1], c8[b2], c8[b3]};
                std::memcpy(physical + address, t, 4);
                continue;
              }
            } else {
              std::memcpy(&word, s, sizeof(word));
            }
            word = xenos::GpuSwap(word, order);
            std::memcpy(physical + address, &word, sizeof(word));
          }
        }
        RestampAfterRead(p);
        continue;
      }
      // FH1: an 8-bit picture resolved as k_2_10_10_10 (the final image): ten bits per channel, two of alpha.
      const bool to_10 = ((p.info >> 7) & 0x3F) == uint32_t(xenos::ColorFormat::k_2_10_10_10);
      for (uint32_t j = 0; j < p.height; ++j) {
        const uint32_t ty = uint32_t(p.y0) + j;
        if (p.height_target && ty >= p.height_target) {
          break;
        }
        const uint64_t row = base + ((uint64_t(ty >> 5) * tiles_by_row) << 12);
        const uint16_t* const local = table.data() + size_t(ty & 31) * 32;
        const uint8_t* s = read.data + size_t(j) * p.width * 4;
        for (uint32_t i = 0; i < p.width; ++i, s += 4) {
          const uint32_t tx = uint32_t(p.x0) + i;
          if (tx >= p.pitch) {
            break;
          }
          const uint64_t address = row + (uint64_t(tx >> 5) << 12) + local[tx & 31];
          if (address + 4 > 0x20000000) {
            continue;
          }
          if (to_10) {
            uint32_t field[3] = {(uint32_t(s[0]) * 1023 + 127) / 255, (uint32_t(s[1]) * 1023 + 127) / 255,
                                 (uint32_t(s[2]) * 1023 + 127) / 255};
            if (swap_value) {
              std::swap(field[0], field[2]);
            }
            const uint32_t word = xenos::GpuSwap(
                field[0] | (field[1] << 10) | (field[2] << 20) | (uint32_t(s[3] >> 6) << 30), order);
            std::memcpy(physical + address, &word, sizeof(word));
            continue;
          }
          const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
          uint8_t* const d = physical + address;
          d[0] = t0;
          d[1] = t1;
          d[2] = t2;
          d[3] = t3;
        }
      }
      RestampAfterRead(p);  // FH1: or the texture is taken for one the game wrote over (it was, for the 8-bit ones)
    }
    reads.clear();
    ns_write_reads_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_reads).count());
  }

  // Offset of a texel of eight bytes inside a tiled texture (the console's layout, as Xenia's GetTiledOffset2D with
  // bytes per block log2 = 3).
  static uint64_t TiledOffsetTexel8(uint32_t x, uint32_t y, uint32_t pitch_tiles) {
    const uint32_t macro = ((x >> 5) + (y >> 5) * pitch_tiles) << 10;
    const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << 3;
    const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return (uint64_t(offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }
  uint64_t reads_half_ = 0;

  // FH1: the read-back itself changed the guest memory under a resolved texture: its fingerprint is taken again,
  // or the next draw would think the game wrote there and read the texture from memory.
  void RestampAfterRead(const ReadPending& p) {
    const uint32_t address = p.base & 0x1FFFFFFF;
    const auto it = resolved_.find(address);
    if (it != resolved_.end() && !it->second.overwritten) {
      it->second.stamp_memory = StampMemory(address, it->second.image.width, it->second.image.height,
                                            BytesTexelGuest(it->second.format_guest));
      return;
    }
    // A piece of a larger resolved texture (the scene's strips, the livery's sides): the fingerprint is the
    // container's.
    for (auto& [base, t] : resolved_) {
      if (!t.overwritten && base < address &&
          uint64_t(address) - base < uint64_t(t.image.width) * t.image.height * BytesTexelGuest(t.format_guest)) {
        t.stamp_memory = StampMemory(base, t.image.width, t.image.height, BytesTexelGuest(t.format_guest));
      }
    }
  }
  struct OneOff {
    uint64_t frame = 0;   // presented frame of the last resolve to this destination
    uint32_t streak = 0;  // resolves read at once since it was last quiet
    uint32_t returns = 0;  // times it came back within 20 s of its last resolve (2 = an effect: old rules)
  };
  std::unordered_map<uint64_t, OneOff> one_off_;  // fh1_native_read_one_off
  std::vector<uint64_t> reads_once_;
  uint64_t one_off_logged_ = 0;

  void DestroyRead(ReadAccess& read) {
    if (read.memory_block != VK_NULL_HANDLE) {
      if (read.data) {
        dfn_.vkUnmapMemory(device_, read.memory_block);
      }
      dfn_.vkFreeMemory(device_, read.memory_block, nullptr);
    }
    if (read.buffer != VK_NULL_HANDLE) {
      dfn_.vkDestroyBuffer(device_, read.buffer, nullptr);
    }
    read = ReadAccess{};
  }

  // since_target = the source is a render target's image (or a retained one) with the front buffer in its
  // corner and larger than the output (fh1_native_lazy_front). Only requested with the exact variant
  // and without FXAA.
  bool PaintOutput(VulkanPresenter::VulkanGuestOutputRefreshContext& context_id, Image& source,
                    uint32_t width, uint32_t height, bool since_target = false) {
    // With fh1_native_output_without_wait, the next of 3 slots, and it only waits if that one is still pending.
    // Without it, always slot 0: it waits for the previous output.
    const uint32_t s = output_without_wait_ ? (output_current_ + 1) % kSlotsOutput : 0;
    if (outputs_pending_[s]) {
      const auto before_wait = std::chrono::steady_clock::now();  // "C2: present" report
      WaitFenceWatched(dfn_, device_, fences_output_[s], "output slot before present");
      ns_wait_output_ += Ns(before_wait, std::chrono::steady_clock::now());
      ++waits_output_;
      dfn_.vkResetFences(device_, 1, &fences_output_[s]);
      outputs_pending_[s] = false;
    }
    output_current_ = s;
    // The menu post-processing settings, if they changed (an atomic counter; the cvars are only read then).
    if (ramp_gamma_) {
      const uint64_t version_postprocess = fh1::settings::VersionPostprocess();
      if (version_postprocess != version_postprocess_) {
        version_postprocess_ = version_postprocess;
        postprocess_ = fh1::settings::ReadPostprocess();
        RecomputeRamp();
        const auto& p = postprocess_;
        REXLOG_INFO("[native] C2: post-processing {}: brightness {:.2f}, contrast {:.2f}, saturation {:.2f}, "
                    "vibrance {:.2f}, temperature {:.2f}, gamma {:.2f}, tint {:.2f}/{:.2f}/{:.2f} at {:.2f}, "
                    "vignette {:.2f}, scanlines {:.2f}; {}",
                    p.active ? "on" : "off", p.brightness, p.contrast, p.saturation, p.vibrance,
                    p.temperature, p.gamma, p.tint_r, p.tint_g, p.tint_b, p.tint, p.vignette, p.lines,
                    grading_ ? "with per-pixel work (saturation, vibrance, vignette or scanlines)"
                                : "all in the table, no per-pixel work");
      }
    }
    // The gamma ramp, if it changed since this slot's last output (the GPU is no longer reading it).
    if (ramp_gamma_ && ramps_output_[s].version != version_ramp_) {
      RampOutput& ramp = ramps_output_[s];
      std::memcpy(ramp.data, ramp_values_.data(), sizeof(ramp_values_));
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, ramp.memory_block, ramp.type);
      ramp.version = version_ramp_;
    }
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context_id.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      auto& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        WaitOutputs();  // a pending output could still use this framebuffer
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context_id.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_output_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = width;
      info.height = height;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context_id.image_version();
      framebuffer = f.framebuffer;
    }

    VkDescriptorImageInfo info_image{};
    info_image.imageView = source.view;
    info_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptores_output_[s];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info_image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    dfn_.vkResetCommandPool(device_, pools_output_[s], 0);
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(commands_output_[s], &start) != VK_SUCCESS) {
      return false;
    }
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_output_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = {width, height};
    if (since_target) {
      // That image was just written as a render target (or copy destination) in the frame's work, which goes in
      // another submission: make the output read it fully written.
      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dfn_.vkCmdPipelineBarrier(commands_output_[s],
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    dfn_.vkCmdBeginRenderPass(commands_output_[s], &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(commands_output_[s], 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {width, height}};
    dfn_.vkCmdSetScissor(commands_output_[s], 0, 1, &scissor);
    VkPipeline pipeline = pipeline_;
    if (ramp_gamma_) {
      // With an output the size of the source (the normal case), the exact texel, unfiltered.
      // From a render target, the output has the front buffer's size, not the image's, and has no FXAA
      // (FrontToPresent only requests it that way): texelFetch of the pixel, the same texel the texture
      // would hold.
      const bool exact = since_target || (width == source.width && height == source.height);
      const bool fxaa = !since_target && fh1::settings::AntialiasingFxaa();
      if (fxaa != fxaa_noted_) {
        fxaa_noted_ = fxaa;
        REXLOG_INFO("[native] C2: antialiasing on the output: {}", fxaa ? "FXAA" : "none");
      }
      pipeline = PipelineRamp((fxaa ? 4 : 0) + (grading_ ? 2 : 0) + (exact ? 1 : 0));
      if (pipeline == VK_NULL_HANDLE) {
        pipeline = pipelines_ramp_[exact ? 1 : 0];  // requested variant missing: the usual one
      }
      if (!exact && !warned_ramp_bilinear_) {
        warned_ramp_bilinear_ = true;
        REXLOG_INFO("[native] C2: {}x{} output from a {}x{} source: ramp with bilinear sampling", width,
                    height, source.width, source.height);
      }
    }
    dfn_.vkCmdBindPipeline(commands_output_[s], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(commands_output_[s], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 layout_pipeline_, 0, 1, &descriptores_output_[s], 0, nullptr);
    const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};  // x, y, width, height in NDC
    dfn_.vkCmdPushConstants(commands_output_[s], layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(rectangle), rectangle);
    struct {
      int32_t displacement[2];
      float inverse[2];
    } bilinear = {{0, 0}, {1.0f / float(width), 1.0f / float(height)}};
    dfn_.vkCmdPushConstants(commands_output_[s], layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(bilinear), &bilinear);
    dfn_.vkCmdDraw(commands_output_[s], 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(commands_output_[s]);
    if (since_target) {
      // And nothing submitted later (the next frame draws and clears that image again) writes it before the
      // output has read it.
      dfn_.vkCmdPipelineBarrier(commands_output_[s], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 0, nullptr);
    }
    if (dfn_.vkEndCommandBuffer(commands_output_[s]) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submission{};
    submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands_output_[s];
    {
      const auto before_lock = std::chrono::steady_clock::now();  // "C2: present" report
      const auto queue = vulkan_device_->AcquireQueue(family_, 0);
      const auto before_submission = std::chrono::steady_clock::now();
      if (dfn_.vkQueueSubmit(queue.queue(), 1, &submission, fences_output_[s]) != VK_SUCCESS) {
        return false;
      }
      ns_lock_output_ += Ns(before_lock, before_submission);
      ns_submit_output_ += Ns(before_submission, std::chrono::steady_clock::now());
      ++submissions_output_;
    }
    outputs_pending_[s] = true;
    context_id.SetIs8bpc(true);
    return true;
  }

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  uint32_t family_;

  std::array<VkCommandPool, kSlotsOutput> pools_output_{};  // output slots
  FnCopyImage copy_image_ = nullptr;
  uint32_t reductions_ = 0;  // resolves that had to shrink
  // Those of the slot being recorded (Record switches them).
  VkCommandBuffer commands_work_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_upload_ = VK_NULL_HANDLE;
  // Three slots by default, up to four. With two, the CPU is only one frame ahead and waits for the GPU inside
  // Record() half the time (4,826 ms out of every 10 s on the console). How many are actually used is set by
  // fh1_native_work_slots, so configurations can be compared without another NRO.
  // The fourth: measured by regime, with few draws (open road, GPU-bound) the fence wait inside Record() is
  // 4.9-9.8 ms, while with many (alleys, CPU-bound) it is ~0. So the third slot falls short exactly where the
  // GPU is the bottleneck. It fits easily: the log says "heap 0 (GPU): 482 MB used of 1382 MB
  // budgeted".
  std::array<SlotWork, 4> slots_{};
  uint32_t slots_used_ = 3;  // set by the cvar (fh1_native_work_slots)
  uint32_t slot_ = 1;  // Record starts with slot 0
  uint64_t submissions_ = 0;
  std::array<VkCommandBuffer, kSlotsOutput> commands_output_{};
  std::array<VkFence, kSlotsOutput> fences_output_{};
  bool recording_ = false;
  std::array<bool, kSlotsOutput> outputs_pending_{};
  uint32_t output_current_ = 0;
  bool output_without_wait_ = false;
  bool marks_precise_ = false;
  int32_t toggle_marks_s_ = 0;
  std::chrono::steady_clock::time_point start_marks_{};
  bool invalidate_every_copy_ = false;
  bool recording_upload_ = false;
  uint64_t generation_commands_ = 0;
  FnClearDepth clear_depth_ = nullptr;
  // GPU time of the submissions (timestamps, if the queue supports them).
  using FnWriteMark = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineStageFlags, VkQueryPool,
                                          uint32_t);
  using FnReadQueries = VkResult(VKAPI_PTR*)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t,
                                              void*, VkDeviceSize, VkQueryResultFlags);
  FnWriteMark write_mark_ = nullptr;
  FnReadQueries read_queries_ = nullptr;
  VkQueryPool queries_ = VK_NULL_HANDLE;
  // The game's occlusion queries. The open one (0 = none), those waiting for their spans by number, the last
  // complete count of each D3D structure and, accumulated, spans, spans without room, published queries,
  // published samples and the maximum of a single query.
  VkQueryPool occlusions_ = VK_NULL_HANDLE;
  // Pipeline statistics per pass category.
  VkQueryPool statistics_ = VK_NULL_HANDLE;
  std::array<uint64_t, kGpuCategories> fragments_category_{};
  std::array<uint64_t, kGpuCategories> vertices_category_{};
  std::array<uint64_t, kGpuCategories> primitives_category_{};
  uint64_t statistics_without_room_ = 0;
  VkQueryPool statistics_draw_ = VK_NULL_HANDLE;
  std::vector<uint64_t> fragments_by_shader_;
  std::vector<uint64_t> draws_by_shader_;
  uint64_t frames_diagnostic_ = 0;
  uint64_t statistics_draw_without_room_ = 0;
  std::array<uint64_t, 3> without_swap_{};          // no clear, not whole, not possible
  uint32_t warnings_without_swap_ = 0;
  std::array<uint64_t, kBucketsCopy> copies_bucket_{};
  std::array<uint64_t, kBucketsCopy> pixels_bucket_{};
  bool window_diagnostic_ = false;
  bool window_read_ = false;
  std::chrono::steady_clock::time_point last_window_ = std::chrono::steady_clock::now();
  bool occlusion_precise_ = false;
  VkQueryPool visibility_ = VK_NULL_HANDLE;  // fh1_reflection_visibility
  uint64_t visibility_without_room_ = 0;
  uint64_t occlusion_open_ = 0;
  uint64_t next_occlusion_ = 1;
  std::unordered_map<uint64_t, QueryOcclusionGame> occlusions_game_;
  std::unordered_map<uint32_t, uint64_t> occlusion_by_base_;
  uint64_t statistics_occlusion_[5] = {};
  double period_mark_ns_ = 0.0;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_jobs_ = 0;
  uint64_t gpu_jobs_precise_ = 0;  // measured with precise intermediate marks
  std::array<uint64_t, kGpuCategories> gpu_categories_ns_{};
  uint64_t mark_end_previous_ = 0;  // end timestamp of the last submission read (gap between submissions)
  uint64_t last_mark_gpu_ = 0;  // same, but kept when a read fails (timestamp scale)
  // Breakdown of Present's time (CostPresent).
  uint64_t waits_output_ = 0, ns_wait_output_ = 0;
  uint64_t submissions_work_ = 0, ns_lock_work_ = 0, ns_submit_work_ = 0;
  uint64_t submissions_output_ = 0, ns_lock_output_ = 0, ns_submit_output_ = 0;
  uint64_t refreshes_ = 0, ns_before_call_ = 0, ns_in_call_ = 0, ns_after_call_ = 0;
  static uint64_t Ns(std::chrono::steady_clock::time_point since, std::chrono::steady_clock::time_point until) {
    return until > since ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(until - since).count()) : 0;
  }
  uint64_t overlaps_gpu_ = 0;
  std::array<uint64_t, kBucketsSwap> buckets_swap_{};
  mutable double worst_swap_ms_ = 0.0;  // worst frame since the previous report
  std::chrono::steady_clock::time_point last_swap_{};
  // Resolved texture grid diagnostic (fh1_native_diag_resolved).
  FnBlit blit_ = nullptr;
  // The game's shadow map, and the scale it is being drawn at (0 = not decided yet).
  static constexpr uint32_t kSideShadows = 1600;
  bool depth_scalable_ = false;
  uint32_t scale_shadows_ = 0;
  uint64_t shadows_uploads_ = 0;
  std::vector<uint32_t> resolved_frame_;
  Image tile_;
  std::chrono::steady_clock::time_point last_warning_tile_{};
  // Read-back of small resolved textures (fh1_native_read_resolved_texels).
  std::unordered_map<uint64_t, ReadAccess> reads_;
  std::unordered_map<uint64_t, TargetRead> reads_by_target_;  // report C2 and cadence
  std::chrono::steady_clock::time_point report_reads_{};
  std::vector<ReadPending> reads_pending_;  // recorded in the current submission
  uint64_t reads_done_ = 0;
  // Rear-view mirror diagnostic (NoteCopy).
  uint64_t drawn_last_copy_ = 0;
  std::unordered_map<uint32_t, CopyTarget> copies_by_target_;
  std::chrono::steady_clock::time_point report_copies_{};
  VkFormat format_depth_ = VK_FORMAT_UNDEFINED;
  // Parts C3-C6: draws with the library shaders (nullptr without the required capabilities).
  std::unique_ptr<DrawsVulkan> draws_;

  std::unordered_map<uint64_t, Image> targets_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  // fh1_native_diag_readers_s (DiagReadersToPresent). Ring thread only.
  struct ReaderDiag {
    uint32_t ps = 0;        // PS number in the library (kReaderSwap: the Swap presenting the front buffer)
    uint32_t reg_entry = 0;  // sampler (fetch constant)
    uint32_t target = 0;   // EDRAM base of color 0 | pitch << 12: where it draws
    uint32_t times = 0;     // draws
  };
  struct WriteDiag {
    int32_t frame = -1;  // 0 or 1 within the window; -1: the last one before the window
    uint32_t order = 0;      // n-th write to that address in its frame
    uint32_t source = 0;     // EDRAM base | pitch << 12 of the target being resolved
    uint32_t requested_width = 0;
    uint32_t requested_height = 0;
    uint32_t width = 0;  // the texture's
    uint32_t height = 0;
    uint64_t draws = 0;    // draws since the previous copy (any)
    bool depth = false;
    bool whole = false;  // covers it fully: from (0,0) and at least its size
    uint32_t others = 0;   // readers that do not fit in the list
    std::vector<ReaderDiag> readers;
  };
  // A write classified as SOBRA in a window, watched in every frame since then.
  struct WatchedDiag {
    uint32_t address = 0;
    uint32_t source = 0;          // EDRAM base | pitch << 12 of the watched writes
    bool pending = false;       // the texture's last write comes from that source
    bool source_written = false;  // and the source was drawn to or cleared since then
    uint64_t writes = 0;      // from that source since watching began
    uint64_t before = 0;           // reads before the source is rewritten (a deferred copy covers them)
    uint64_t late = 0;         // reads after the source is written (dropping the copy there would fail)
    uint32_t ps_before = 0;        // PS of the first one of each kind
    uint32_t ps_late = 0;
  };
  static constexpr uint32_t kReaderSwap = UINT32_MAX;
  static constexpr size_t kDiagMaxWrites = 32;
  static constexpr size_t kDiagMaxReaders = 12;
  static constexpr size_t kDiagMaxWatched = 4;
  static constexpr uint32_t kDiagRegSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
  static constexpr uint32_t kDiagRegColorInfo = 0x2001;    // RB_COLOR_INFO
  static constexpr uint32_t kDiagRegFetch = 0x4800;        // SHADER_CONSTANT_FETCH_00_0
  static constexpr uint32_t kDiagRegColorMask = 0x2104;    // RB_COLOR_MASK: 4 bits per color target
  static constexpr uint32_t kDiagRegsColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};  // RB_COLOR_INFO, RB_COLORn_INFO
  bool diag_window_ = false;
  uint32_t diag_frame_ = 0;
  uint64_t diag_windows_ = 0;
  uint64_t diag_writes_lost_ = 0;
  std::chrono::steady_clock::time_point diag_readers_last_ = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, std::vector<WriteDiag>> diag_writes_;
  std::unordered_map<uint32_t, uint64_t> diag_signatures_;  // pattern of the last line written, per address
  std::vector<WatchedDiag> diag_watched_;
  // fh1_native_lazy_composite (DeferComposite). A single deferred copy at a time.
  struct CompositePending {
    uint32_t address = 0;
    uint32_t source = 0;                 // EDRAM base | pitch << 12 of the source target
    Image* target = nullptr;           // that target (targets_ never erases: the pointer stays valid)
    VkImage source_vk = VK_NULL_HANDLE;  // its image when deferred: if it changes, it can no longer be recorded
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
  };
  static constexpr uint64_t kCompositeALook = 900;  // watched writes without late reads (~30 s of racing)
  CompositePending composite_;
  bool composite_there_is_ = false;
  bool composite_off_ = false;
  uint32_t composite_stale_ = 0;   // address whose last copy was dropped; 0 = none
  uint32_t composite_applying_ = 0;  // address of the last APPLYING notice
  uint64_t composite_deferred_ = 0;
  uint64_t composite_recorded_read_ = 0;
  uint64_t composite_recorded_other_ = 0;
  uint64_t composite_dropped_source_ = 0;
  uint64_t composite_replaced_ = 0;
  uint64_t composite_reads_late_ = 0;
  uint64_t composite_differences_ = 0;
  uint64_t composite_pixels_saved_ = 0;
  uint64_t composite_pixels_previous_ = 0;
  uint64_t composite_presented_previous_ = 0;
  std::unordered_map<uint64_t, Image> depths_;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_descriptores_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_descriptores_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOutput> descriptores_output_{};
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_output_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // Output with the game's gamma ramp (fh1_native_gamma_ramp).
  struct RampOutput {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory_block = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    uint32_t type = 0;
    uint64_t version = UINT64_MAX;  // the ramp_values_ version the buffer holds
  };
  bool ramp_gamma_ = false;
  std::array<RampOutput, kSlotsOutput> ramps_output_{};
  VkShaderModule fs_ramp_ = VK_NULL_HANDLE;
  // Index 4 * fxaa + 2 * grading + exact: 0 bilinear (like guest_output_bilinear_ps), 1 exact texel
  // (output the same size as the source); +2 with the saturation, vibrance, vignette or scanline
  // post-processing; +4 with FXAA. Those other than 0 and 1 are created on request (PipelineRamp).
  std::array<VkPipeline, 8> pipelines_ramp_{};
  std::array<bool, 8> pipelines_ramp_failed_{};
  bool warned_ramp_bilinear_ = false;
  // The game's ramp in 10 bits (red, green, blue). Until the game loads its own, the identity the SDK starts
  // with (i * 1023 / 255).
  std::array<std::array<uint16_t, 3>, 256> ramp_game_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  // The output UBO in std140: one vec4 per entry (red, green and blue in 0-1, with the single-channel
  // post-processing applied), followed by the mix (saturation, vibrance, 1 / gamma) and effects (vignette,
  // scanlines). Filled by RecomputeRamp.
  static constexpr size_t kFloatsRamp = 256 * 4 + 8;
  std::array<float, kFloatsRamp> ramp_values_{};
  uint64_t version_ramp_ = 0;
  // Post-processing in effect and the settings version it was computed from.
  fh1::settings::Postprocess postprocess_{};
  uint64_t version_postprocess_ = 0;
  bool grading_ = false;  // shader variant with saturation, vibrance, vignette or scanlines
  bool fxaa_noted_ = false;  // fh1_antialiasing at the last output (to log changes)
  std::array<Framebuffer, VulkanPresenter::kMaxActiveGuestOutputImageVersions> framebuffers_{};
  size_t next_framebuffer_ = 0;

  std::chrono::steady_clock::time_point start_alternation_ = std::chrono::steady_clock::now();
  bool alternation_noted_ = false;
  uint64_t swaps_ = 0;    // resolves without a copy
  uint64_t restores_ = 0;
  uint64_t restores_for_resolve_ = 0;  // fh1_native_resolve_valid_content
  uint64_t resolve_without_source_ = 0;           // the content was in no texture
  uint64_t resolve_content_old_ = 0;      // with the setting off, resolves of the stale image
  uint64_t swaps_without_clear_ = 0;  // the ones that used to copy 1600x1600
  uint64_t swaps_color_ = 0;  // color targets resolved without a copy
  // Copies and clears removed, and what they cost.
  std::unordered_map<const Image*, StateTarget> state_target_;
  uint64_t clears_skipped_ = 0;             // of color
  uint64_t clears_skipped_depth_ = 0;
  uint64_t pixels_clears_skipped_ = 0;
  uint64_t clears_useless_ = 0;             // clears wiped out by a swap with no draw in between
  uint64_t pixels_clears_useless_ = 0;
  uint64_t restores_clipped_ = 0;     // restores that did not copy the whole image
  uint64_t pixels_restored_ = 0;           // the ones actually copied (so the report adds up)
  uint64_t pixels_restore_saved_ = 0;   // the ones not copied, due to useful area or swap
  uint64_t loans_ = 0;                     // restores done by swapping back
  uint64_t lent_read_ = 0;              // times a lent resolved texture was requested (must be 0)
  std::unordered_set<uint32_t> lent_;     // bases whose content is lent to the render target
  // fh1_native_shadow_minimum. State of the shadow map cycle and phase of the guard.
  enum : uint32_t { kSmFree = 0, kSmAfterWorld = 1, kSmCars = 2 };
  enum : uint32_t { kSmWatching = 0, kSmApplying = 1, kSmOff = 2 };
  static constexpr uint64_t kShadowMinimumCycles = 300;  // consecutive clean cycles before applying (~10 s of racing)
  uint32_t sm_phase_ = kSmWatching;
  uint32_t sm_state_ = kSmFree;
  bool sm_on_ = false;                  // cvar, alternation and cheap PCF, read at the start of each cycle
  bool sm_range_noted_ = true;               // fh1_native_shadow_minimum_toggle_s: last interval written to the log
  std::chrono::steady_clock::time_point sm_start_alternation_ = std::chrono::steady_clock::now();
  const Image* sm_target_cycle_ = nullptr;   // the current cycle: 1600x1600 target...
  uint32_t sm_world_cycle_ = 0;                // ...and the texture that received the world (no clear)
  bool sm_cycle_clean_ = false;               // nothing watched has failed in this cycle
  bool sm_cycle_clear_ = false;              // this cycle's car pass draws on the cleared target
  bool sm_world_rewritten_ = false;            // the world texture was rewritten within the cycle
  const Image* sm_target_ = nullptr;         // learned while observing: the target...
  uint32_t sm_world_ = 0;                      // ...the texture without cars (texture[1], 07CEA000)...
  uint32_t sm_cars_ = 0;                     // ...and the texture with cars (texture[0], 086AE000)
  bool sm_virtual_ = false;                    // sm_cars_ holds only the cars: sampled with the minimum
  bool sm_virtual_valid_ = false;             // and sm_world_ still holds the world of that same cycle
  uint64_t sm_clean_consecutive_ = 0;
  uint64_t sm_cycles_ = 0;
  uint64_t sm_cycles_applied_ = 0;
  uint64_t sm_pixels_saved_ = 0;
  uint64_t sm_pixels_saved_previous_ = 0;
  uint64_t sm_draws_cars_ = 0;
  uint64_t sm_draws_no_exact_ = 0;
  uint64_t sm_reads_minimum_ = 0;            // of the cars-only texture, paired with the world
  uint64_t sm_reads_si_same_ = 0;          // of the usual texture, paired with itself (observing)
  uint64_t sm_reads_normals_ = 0;          // of the usual texture, without the minimum
  uint64_t sm_reads_a_out_of_time_ = 0;       // between the two resolves of a cycle
  uint64_t sm_reads_unable_ = 0;         // from a shader without tfetch2DShadowMin on that register
  uint64_t sm_differences_ = 0;
  // fh1_native_diag_clears (see UseClear).
  std::unordered_map<const Image*, UseClear> use_clears_;  // targets_ and depths_ never erase: the key stays valid
  bool diag_clears_ = REXCVAR_GET(fh1_native_diag_clears);
  std::chrono::steady_clock::time_point report_clears_{};
  uint64_t presented_report_clears_ = 0;
  // fh1_native_clear_useful_area.
  bool clear_area_useful_ = REXCVAR_GET(fh1_native_clear_useful_area);
  bool area_useful_global_off_ = false;
  uint32_t bands_pending_ = 0;  // targets with an uncleared band: NoteUseClear checks them even when not measuring
  uint64_t area_useful_clears_ = 0;
  uint64_t area_useful_completed_ = 0;
  uint64_t area_useful_misses_ = 0;
  uint64_t area_useful_pixels_ = 0;
  uint64_t area_useful_pixels_previous_ = 0;
  static constexpr uint64_t kAreaUsefulCycles = 120;  // measured clears of a target before trimming its own
  // fh1_native_lazy_depth. Deferred depth copies, by texture address.
  struct CopyPending {
    const Image* source = nullptr;  // depth target (depths_ never erases: the pointer stays valid)
    VkImage source_vk = VK_NULL_HANDLE;
    VkImage target_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
    uint64_t frame = 0;     // presented_ when it was deferred
    bool read_dead = false;  // the composite without blur already requested it
  };
  std::unordered_map<uint32_t, CopyPending> pending_;
  std::unordered_map<uint32_t, uint64_t> last_read_live_;    // per address, in presented_
  std::unordered_map<uint32_t, uint64_t> last_read_dead_;
  std::unordered_set<uint32_t> stale_;     // dropped copies: their texture lacks the last resolve
  bool reads_depth_dead_ = false;  // set by DrawsVulkan (ReadsOfDepthDead)
  bool lazy_off_ = false;              // the guard saw a late read
  uint64_t lazy_deferred_ = 0;
  uint64_t lazy_copied_read_ = 0;
  uint64_t lazy_copied_write_ = 0;
  uint64_t lazy_replaced_ = 0;
  uint64_t lazy_dropped_ = 0;
  uint64_t lazy_reads_late_ = 0;
  uint64_t lazy_pixels_saved_ = 0;
  uint64_t lazy_pixels_saved_previous_ = 0;
  static constexpr uint64_t kLazyFrames = 30;  // ~1 s: window for "requested" and "nobody samples it"
  // fh1_native_lazy_front (see FrontPending).
  std::unordered_map<uint32_t, FrontPending> front_pending_;  // per texture address
  std::vector<ImageFront> front_images_;                         // spares and retained ones
  std::unordered_map<uint32_t, uint64_t> front_presented_;  // address -> presented_ at its last Swap
  std::unordered_map<uint32_t, uint64_t> front_read_;       // address -> last sampling by a draw
  std::unordered_set<uint32_t> front_stale_;              // missing a copy that can no longer be made
  bool front_off_ = false;
  uint64_t front_deferred_ = 0;
  uint64_t front_painted_target_ = 0;
  uint64_t front_painted_retained_ = 0;
  uint64_t front_rotations_ = 0;
  uint64_t front_without_spare_ = 0;
  uint64_t front_copied_read_ = 0;
  uint64_t front_copied_write_ = 0;
  uint64_t front_copied_swap_ = 0;
  uint64_t front_replaced_ = 0;
  uint64_t front_reads_late_ = 0;
  uint64_t front_pixels_saved_ = 0;
  uint64_t front_pixels_saved_previous_ = 0;
  static constexpr uint64_t kFrontFrames = 8;    // window for "the Swap presents it" and "nobody samples it"
  static constexpr size_t kFrontImagesMax = 3;     // spares of the front buffer's target (~4 MB each at 1040)
  // The values above as they were at the previous report: the new lines are per frame.
  uint64_t pixels_restored_previous_ = 0;
  uint64_t pixels_restore_saved_previous_ = 0;
  uint64_t pixels_clears_skipped_previous_ = 0;
  uint64_t pixels_clears_useless_previous_ = 0;
  uint64_t presented_report_copies_ = 0;
  // State of the previous frame, for the stutter dump.
  uint64_t hitch_copies_ = 0, hitch_clears_ = 0, hitch_resolves_ = 0;
  uint64_t hitch_restores_ = 0, hitch_waits_ = 0;
  // Snapshot of the waits at the previous Swap, for the "[hitch] waits" line.
  uint64_t hitch_waits_ns_[fh1::waits::kNumTypes] = {};
  uint64_t hitch_waits_times_[fh1::waits::kNumTypes] = {};
  uint64_t hitch_ns_waits_gpu_ = 0, hitch_ns_wait_output_ = 0;
  uint64_t hitch_bytes_fingerprint_ = 0, hitch_fingerprints_deferred_ = 0;
  uint64_t hitch_draws_ = 0, hitch_ns_ring_ = 0, hitch_ns_textures_ = 0;
  uint64_t hitch_textures_uploads_ = 0, hitch_bytes_uploaded_ = 0, hitch_textures_created_ = 0;
  uint64_t hitch_ns_fingerprint_raw_ = 0, hitch_ns_fingerprint_data_ = 0;
  uint64_t hitch_ns_waiting_copies_ = 0, hitch_waits_copies_ = 0;
  uint64_t hitch_ns_helping_copies_ = 0, hitch_copies_helped_ = 0;
  uint64_t hitch_ns_create_textures_ = 0, hitch_textures_bound_thread_ = 0;
  uint64_t hitch_ns_waiting_bindings_ = 0;
  uint64_t hitch_textures_fingerprint_thread_ = 0, hitch_ns_fingerprint_thread_ = 0, hitch_ns_fingerprint_snapshot_ = 0;
  uint64_t hitch_textures_fingerprint_ring_ = 0, hitch_ns_fingerprint_ring_ = 0, hitch_ns_waiting_fingerprints_ = 0;
  // Why the restore does not swap. See the block in RestoreContent.
  uint64_t no_swap_sin_transfer_dst_ = 0;
  uint64_t no_swap_target_without_prepare_ = 0;
  uint64_t no_swap_resolved_without_prepare_ = 0;
  uint64_t no_swap_sizes_different_ = 0;
  uint64_t hitch_gpu_ns_ = 0;      // GPU timestamps when the previous frame closed
  uint64_t hitch_ns_record_ = 0;   // ns recording when the previous frame closed
  uint32_t warnings_hitch_ = 0;
  uint64_t copies_ = 0;
  uint64_t copies_depth_ = 0;  // those that force keeping the depth tile
  uint64_t clears_depth_ = 0;  // depth clears (clears_ only counts color ones)
  uint64_t clears_in_pass_failed_ = 0;  // clears through a pass that could not be opened
  uint64_t clears_ = 0;
  uint64_t presented_ = 0;
  uint64_t rejections_ = 0;
  std::unordered_set<uint32_t> warned_;
};

}  // namespace

std::unique_ptr<TargetsNative> TargetsNative::Create(const VulkanDevice* vulkan_device,
                                                        rex::memory::Memory* memory_block) {
  if (!vulkan_device || !memory_block) {
    return nullptr;
  }
  auto targets = std::make_unique<TargetsVulkan>(vulkan_device, memory_block);
  if (!targets->Initialize()) {
    REXLOG_ERROR("[native] C2: could not prepare the target presentation");
    return nullptr;
  }
  return targets;
}

}  // namespace fh1::native
