// nfsmw - shadow trimming: update the shadow maps 1 of every N frames
//
// ===========================================================================
//  Why
//  In the menu and in a race, the two 1600x1600 shadow maps are more than
//  half of each frame's resolve area (~5.6 of ~9.9 screens) and, in a race,
//  ~584 of ~1,820 draws. It is an optional trade of image quality for FPS.
//
//  The pass (recompiled code)
//    sub_82443B18  shadow pass. Called by sub_82445660 (race) and
//                  sub_82444D80 (menus). It leaves its parameters in globals
//                  and queues on the render queue (sub_823C8378) the commands
//                  for one or two shadow maps; in a race it draws both within
//                  the same call, it does not alternate between frames.
//  If a frame does not call it, the maps keep what was last drawn and the
//  scene uses them anyway: the shadow lags behind.
//
//  What this file does
//  With nfsmw_shadows_every = N (1..8) only 1 of every N calls goes through.
//  With 1, the default, it behaves like the game. It applies immediately,
//  also from the settings menu (L+R+Right, NFSMW category), to compare live
//  with the F3 counter.
// ===========================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(nfsmw_shadows_every, 1, "NFSMW",
                     "Update los maps de shadows 1 de every N frames (1 = as el game)")
    .range(1, 8);

/*
 * One cascade instead of two.
 *
 * sub_82443B18 takes a boolean in r4: with 0 it draws one shadow map, otherwise two. The game itself passes
 * 0 in the menus and 1 in a race. They are two complete 1600x1600 maps (views 13 and 14 of the table at
 * 0x82A38070), not two halves of one.
 *
 * Measured on the console: the whole pass costs 11.3 ms of real GPU time and ~8.7 ms of CPU with ~660 draws
 * per frame, and 91 % of that is per-draw cost, not pixels (which is why shrinking the map did not help).
 * Dropping one map removes its half of the draws, its pass and its 1600x1600 resolve.
 *
 * What is lost: the second map's texture stays frozen with whatever it last held, because the scene keeps
 * sampling both. If that map is the far-range one, distant shadows stay stuck while the car moves. This
 * must be checked in motion.
 */
REXCVAR_DEFINE_INT32(nfsmw_shadows_maps, 2, "NFSMW",
                     "Maps de shadows que dibuja el game en race: 2 (as el game) o 1. Con 1 se "
                     "ahorra la half de los draws del pass, pero el second map se queda congelado")
    .range(1, 2);

/*
 * The 30 FPS guard.
 *
 * The goal is not a fast average: it is that no frame drops below 30. With FIFO presentation that is
 * binary: if the work does not fit in 33.3 ms, the frame slips to the next vblank and shows for 50 ms.
 * There is no middle ground.
 *
 * What this cannot do: degrade the frame in flight. By the time its cost is known it has already been
 * recorded, submitted and is being presented. No mechanism can save the first spike. What it does prevent
 * is staying below 30 for a sustained period: as soon as the average of the last frames approaches the
 * ceiling, the next frame already carries less work.
 *
 * The lever is the shadow, and it is the only one: it is the only lever with the three properties needed.
 * It is decided at the start of the frame, on the guest thread, and it is a whole block.
 *
 * Why there is no flicker. An earlier version had a step that updated the shadows 1 of every 3 frames. On
 * the console the guard went to the top step and stayed there 99 % of the time (the work did not fit by a
 * long way), so skipping frames stopped being an occasional lifeline and became the normal state: the
 * shadows flickered, and it was obvious at first sight.
 *
 * So skipping shadow frames is not part of the ladder. The remaining steps change the shadow but do not
 * make it flicker:
 *   1. one map instead of two  -> the second one freezes, it does not flicker
 *   2. no cast shadows         -> less visible, but consistently so
 *
 * And stepping down requires 30 s with margin, not 2: oscillating every two seconds between "with
 * shadows" and "without shadows" looks worse than the drop it fixes.
 *
 * It is also the test. The report says which step it was on and how many times it had to act. If it says
 * "step 0 100 % of the time, 0 raises", the work fit easily and the guard was not needed. If it says
 * anything else, it tells exactly how much was missing.
 */
REXCVAR_DEFINE_BOOL(nfsmw_guard_30, true, "NFSMW",
                    "Si un range se mantiene por debajo de 30 FPS, clip the shadows en el frame "
                    "next until volver a 30. Escalones: 1 un solo map, 2 sin shadows proyectadas. "
                    "Ninguno salta frames, asi que no parpadea. Low de step sola cuando sobra "
                    "margin (30 s) y never deshace lo que pongas tu");
REXCVAR_DEFINE_INT32(nfsmw_guard_30_budget_us, 31000, "NFSMW",
                     "Ceiling de frame en microseconds para la guard. 31000 leaves 2,3 ms de margin over "
                     "los 33333 de un frame de 30 FPS")
    .range(16000, 66000);

/*
 * Distance culling of what goes into the shadow map.
 *
 * The cost of the shadow pass follows triangles, not draws. The first regression, over 22 intervals and
 * reproduced in two builds with loads 2.6x apart, gave 10,000 triangles = 0.80 ms real. The map draws
 * 90,000 triangles per frame, more than the visible image (67,000), at twice the triangles per object.
 * That is why shrinking the map did not help (-44 % area gave -9 % time): area is not the problem.
 *
 * ---------------------------------------------------------------------------------------------
 * The figure above is outdated. The right one is 0.556 ms real per 10,000 triangles.
 *
 * New regression over 17 ten-second race intervals in which the open area is constant (5.12 Mtexels =
 * the two 1600x1600 maps, 2.0 passes), so the only thing that varies is the triangle count:
 *
 *     ms_raw = 0.03420 x k_triangles + 0.023        r2 = 0.982   (51 to 96 k triangles)
 *     -> x1.627 -> 10,000 triangles = 0.556 ms real
 *
 * The drop from 0.80 is expected: the translator's branch merging (1806 -> 641 branches) came in between.
 * The cvar descriptions that promised savings based on 0.80 were inflated 1.4x; they are corrected below.
 *
 * The same fit answers the ZCULL question: the intercept is 0.023 ms raw = 0.037 ms real. So with both
 * 1600x1600 maps open and zero triangles the pass costs nothing measurable. The whole pass is geometry.
 * For contrast, the same fit against draws gives r2 = 0.404 and an intercept of 2.44 ms real, a model
 * with no physical meaning. The triangle count rules, and only it.
 *
 * ZCULL rejects fragments, not triangles: it does not touch setup, vertices or recording. Its ceiling in
 * this pass is those 0.037 ms. Even with the most generous bound (the map scale test, where -44 % area
 * gave -9 % time, so at most 20 % of the pass is per-fragment work: 0.74 ms real), ZCULL could only
 * remove the hidden fraction of that, and without near-to-far draw order from the light it would only
 * catch about half of it. Realistic ceiling: 0.1-0.25 ms. In exchange TRANSFER_DST would have to be
 * removed from the map, which is what disqualifies it in the driver, and that costs the 1600x1600 swap
 * (2.36 ms measured). Net loss, 10 to 1. The details are in nfsmw_native_targets.cpp, next to the
 * TRANSFER_DST decision. Not to be reopened without new data.
 * ---------------------------------------------------------------------------------------------
 *
 * Where the lever is, read from the binary. `WorldModel::Render` decides whether to draw with
 *
 *     eView::GetPixelSize(pos, 35.0f) < view->PixelMinSize   ->  not drawn
 *
 * and `GetPixelSize` returns `radio * H / (distance - radio)`. So the cutoff distance is
 *
 *     35 * (1 + H / PixelMinSize)
 *
 * `PixelMinSize` is field +36 of the view (view 13 at 0x82A38644 and view 14 at 0x82A386B4, view table at
 * 0x82A38070 with 112 bytes per entry) and the game recomputes it every frame in sub_8243EC28, so it has
 * to be written afterwards: right here, in the pass hook, which runs after that recomputation and before
 * drawing.
 *
 * Raising it shortens the distance at which an object stops casting a shadow. Shadows of small, distant
 * objects are lost, which are the least visible ones; nearby ones are untouched.
 */

/*
 * Where the extra triangles are: the shadow map does not reduce detail.
 *
 * Counted over 22 intervals, summing whole frames:
 *     shadow map       2,948 draws   1,193 k triangles  ->  405 triangles per draw
 *     visible scene   16,770 draws   1,321 k triangles  ->   79 triangles per draw
 * The shadow map puts 5.1 times more triangles per object than the visible image, and it takes 47 % of all
 * the frame's triangles with 15 % of the draws.
 *
 * And the split appears exactly when the race starts. In the menu and the first intervals the ratio is
 * 0.79x, 0.83x, 0.85x, 1.05x (the same detail in both); as soon as the car is driven it jumps to 2.9x,
 * 3.5x, 6.8x, 7.8x, 8.1x. That is exactly the signature of the scene lowering mesh detail with distance
 * and the shadow map not doing so: in the menu almost everything is close and both match.
 *
 * So the shadow pass is not expensive because it draws too much, but because it draws each object with
 * the full-detail mesh when its shadow covers four pixels. The lever is not removing objects: it is using
 * the same reduced mesh as the scene. That is not decided here (the native renderer only sees GPU
 * registers, with no object id or mesh level); it is decided in the guest.
 *
 * What does not work, so it is not repeated:
 *   - Filtering by the draw's triangle count: it would remove simple-mesh objects, which are exactly the
 *     cheapest ones. The opposite of what is needed.
 *   - Shrinking the map (nfsmw_native_shadow_scale): the pass area is worth 0.037 ms.
 *   - ZCULL: it rejects fragments, and there is no fragment cost here to reject.
 */
/*
 * The default is 150 (it was 100), for this reason.
 *
 * Measured in a race: the shadow map pushes 65.7 k triangles per frame, almost as many as the visible
 * scene (68.9 k), and it does so with 153 draws against the scene's 922. That is 405 triangles per object
 * against 79. The shadow map does not lower mesh detail with distance and the scene does: in the menu,
 * where everything is close, both match (0.79x-1.05x); as soon as the car is driven, the ratio jumps to
 * 2.9x, 6.8x, 8.1x.
 *
 * In other words, the pass draws every car and every building with the full-detail mesh to cast a shadow
 * that covers four pixels. At 0.556 ms real per 10,000 triangles, those 65.7 k are 3.65 ms of the 3.69
 * measured: the pass is geometry and nothing else.
 *
 * 150 shortens the distance at which an object stops casting a shadow to two thirds. Shadows of small,
 * distant objects are lost (the least visible ones); nearby ones are untouched. It stays at 150 rather
 * than 200 because a shadow that pops in as you approach is noticeable, and the game must keep looking
 * right. The log says how many triangles it removes ("k triangles" in the C6 line of area per render
 * target type).
 */
REXCVAR_DEFINE_INT32(nfsmw_shadows_cut, 150, "NFSMW",
                     "Cut por distance del map de shadows, en percentage. 100 = as el game. 200 = el "
                     "double de exigente, o sea la half de distance: los objects pequenos y lejanos dejan de "
                     "proyectar shadow. El pass cuesta 0,556 ms real por every 10.000 triangles y son los "
                     "triangles el 99 % de su cost, asi que here es where se recorta de truth");

namespace nfsmw::shadows_cut {
namespace {
constexpr uint32_t kVista13 = 0x82A38620;   // 0x82A38070 + 13 * 112
constexpr uint32_t kVista14 = 0x82A38690;
constexpr uint32_t kOffPixelMinSize = 36;
constexpr uint32_t kOffH = 12;              // projection scale, for the report
std::atomic<bool> g_noted{false};

/*
 * The cutoff used to compound itself.
 *
 * An earlier version read PixelMinSize and wrote read * percentage / 100. That only works if the game
 * recomputes it between two of our calls. sub_8243EC28 does recompute it (it writes it as an integer at
 * vista+36: `fctiwz f9,f10` + `stfiwx f9,0,r9`), but nothing guarantees that it runs every frame or in
 * every mode. As soon as it skips one, with cutoff = 200 the value doubles again, and again, until it
 * saturates at 4096: from then on no object casts a shadow and the map comes out empty. Silently, because
 * the log was only written once (g_noted).
 *
 * Fixed by keeping the value the game computes. If what the view holds is exactly what we wrote last
 * time, the game has not recomputed it: start from the saved original, not from our value. That way the
 * cutoff is always the same no matter how often the call repeats.
 */
uint32_t g_base_vista[2] = {0, 0};     // the PixelMinSize the game computes
uint32_t g_written_view[2] = {0, 0};  // the last value we wrote

uint32_t Read32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}
void Write32(uint8_t* base, uint32_t dir, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + dir, &v, sizeof(v));
}
float AsFloat(uint32_t v) {
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}
}  // namespace

void Apply(uint8_t* base) {
  const int32_t percentage = REXCVAR_GET(nfsmw_shadows_cut);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t vista = i == 0 ? kVista13 : kVista14;
    const uint32_t read = Read32(base, vista + kOffPixelMinSize);
    if (read == 0 || read > 4096) {
      continue;  // does not look like a pixel size: leave it alone
    }
    // If the value is exactly what we wrote last time, the game has not recomputed it since then: the
    // original is the one we saved, not what we just read.
    const bool is_ours = g_written_view[i] != 0 && read == g_written_view[i];
    const uint32_t original = (is_ours && g_base_vista[i] != 0) ? g_base_vista[i] : read;
    g_base_vista[i] = original;
    if (percentage <= 100) {
      // Cutoff disabled: if we had modified it, restore the game's value and forget it.
      if (is_ours && original != read) {
        Write32(base, vista + kOffPixelMinSize, original);
      }
      g_written_view[i] = 0;
      continue;
    }
    const uint64_t raw = uint64_t(original) * uint64_t(percentage) / 100u;
    const uint32_t new_value = uint32_t(raw > 4096 ? 4096 : raw);
    Write32(base, vista + kOffPixelMinSize, new_value);
    g_written_view[i] = new_value;
    if (!g_noted.exchange(true)) {
      REXLOG_INFO("[clips] shadows: cut por distance {} -> {} px (H = {:.1f}; la distance de cut "
                  "pasa de {:.0f} a {:.0f} units)",
                  original, new_value, double(AsFloat(Read32(base, vista + kOffH))),
                  35.0 * (1.0 + double(AsFloat(Read32(base, vista + kOffH))) / double(original)),
                  35.0 * (1.0 + double(AsFloat(Read32(base, vista + kOffH))) / double(new_value)));
    }
  }
}
}  // namespace nfsmw::shadows_cut

// The guard's line goes out inside the ring report; the report thread writes it
// (nfsmw_native_deferred_reports, defined in nfsmw_native_system.cpp).
namespace nfsmw::native {
void ReportDeferred(std::string line);
}  // namespace nfsmw::native

namespace nfsmw::guard30 {
namespace {

constexpr unsigned kWindow = 32;        // frames examined to decide
constexpr unsigned kDwell = 60;    // minimum frames before stepping up (~2 s)
constexpr unsigned kDwellLow = 900;  // and 30 s before stepping down: oscillating looks worse
constexpr int kStepMax = 2;
constexpr double kHysteresisMs = 4.0;    // stepping down needs this much extra margin

double g_window[kWindow] = {};
unsigned g_pos = 0;
unsigned g_seen = 0;
int g_step = 0;
unsigned g_in_step = 0;
std::atomic<int> g_step_published{0};
std::atomic<uint64_t> g_frames_by_step[kStepMax + 1];
std::atomic<uint64_t> g_uploads{0};
std::atomic<uint64_t> g_drops{0};

}  // namespace

/*
 * A presented frame, with how long it took. Called by Present() on the ring thread, the only one that
 * enters here: the window needs no lock.
 */
void Beat(double ms) {
  if (!REXCVAR_GET(nfsmw_guard_30)) {
    if (g_step != 0) {
      g_step = 0;
      g_step_published.store(0, std::memory_order_relaxed);
      REXLOG_INFO("[guard30] off: se devuelven the shadows complete");
    }
    return;
  }
  g_window[g_pos] = ms;
  g_pos = (g_pos + 1) % kWindow;
  if (g_seen < kWindow) {
    ++g_seen;
    return;  // nothing is decided until the window is full
  }
  ++g_in_step;
  g_frames_by_step[g_step].fetch_add(1, std::memory_order_relaxed);
  if (g_in_step < kDwell) {
    return;
  }
  double sum = 0.0;
  for (unsigned i = 0; i < kWindow; ++i) {
    sum += g_window[i];
  }
  const double media = sum / double(kWindow);
  const double ceiling = double(REXCVAR_GET(nfsmw_guard_30_budget_us)) / 1000.0;
  const bool can_lower = g_in_step >= kDwellLow;
  if (media > ceiling && g_step < kStepMax) {
    ++g_step;
    g_in_step = 0;
    g_step_published.store(g_step, std::memory_order_relaxed);
    g_uploads.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[guard30] {:.1f} ms de media por frame (ceiling {:.1f}): step {}", media, ceiling,
                g_step);
  } else if (can_lower && media < ceiling - kHysteresisMs && g_step > 0) {
    --g_step;
    g_in_step = 0;
    g_step_published.store(g_step, std::memory_order_relaxed);
    g_drops.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO("[guard30] {:.1f} ms de media por frame, sobra margin: step {}", media, g_step);
  }
}

int Step() { return g_step_published.load(std::memory_order_relaxed); }

/* The guard only tightens: if one map was already requested, it never goes back to two. */
int MapsEffective(int of_user) {
  return Step() >= 1 ? 1 : of_user;
}
/* Step 2: skip the shadow pass. Constant, no flicker. */
bool WithoutShadows(bool of_user) {
  return of_user || Step() >= 2;
}

void Report() {
  uint64_t total = 0, v[kStepMax + 1];
  for (int i = 0; i <= kStepMax; ++i) {
    v[i] = g_frames_by_step[i].load(std::memory_order_relaxed);
    total += v[i];
  }
  if (total == 0) {
    return;
  }
  ::nfsmw::native::ReportDeferred(fmt::format(
      "[guard30] frames por step: 0 (dos maps) {} = {:.1f} % | 1 (un map) {} | "
      "2 (sin shadows) {} | uploads {} | drops {} -> {}",
      v[0], 100.0 * double(v[0]) / double(total), v[1], v[2],
      g_uploads.load(std::memory_order_relaxed), g_drops.load(std::memory_order_relaxed),
      v[0] == total ? "NO HIZO MISSING: el work cabia en 33,3 ms"
                    : "*** hizo missing clip: el work NO cabia ***"));
}

}  // namespace nfsmw::guard30

namespace nfsmw::clip_shadows {
namespace {

std::atomic<uint32_t> g_calls{0};
std::atomic<int32_t> g_every_noted{1};
std::atomic<int32_t> g_maps_noted{2};

}  // namespace
}  // namespace nfsmw::clip_shadows

REX_EXTERN(__imp__sub_82443B18);
REX_HOOK_RAW(sub_82443B18) {
  using namespace nfsmw::clip_shadows;
  // A single map, which is what the game itself does in the menus (r4 = 0).
  if (nfsmw::guard30::MapsEffective(REXCVAR_GET(nfsmw_shadows_maps)) <= 1) {
    if (g_maps_noted.exchange(1, std::memory_order_relaxed) != 1) {
      REXLOG_INFO("[clips] shadows: un solo map en time de dos");
    }
    ctx.r4.u64 = 0;
  } else {
    g_maps_noted.store(2, std::memory_order_relaxed);
  }
  nfsmw::shadows_cut::Apply(base);  // distance cutoff, before drawing
  const int32_t every = REXCVAR_GET(nfsmw_shadows_every);
  if (every <= 1) {
    g_every_noted.store(1, std::memory_order_relaxed);
    __imp__sub_82443B18(ctx, base);
    return;
  }
  if (g_every_noted.exchange(every, std::memory_order_relaxed) != every) {
    REXLOG_INFO("[clips] shadows: se actualizan 1 de every {} frames", every);
  }
  if (g_calls.fetch_add(1, std::memory_order_relaxed) % uint32_t(every) == 0) {
    __imp__sub_82443B18(ctx, base);
  }
}

/*
 * =================================================================================================
 *  The visible scene: the same per-object LOD, and why it is not simply raised here
 * =================================================================================================
 *
 * The mechanism is the same as the shadow cutoff above and the cubemap faces
 * (nfsmw_cubemap_detail_minimum): `WorldModel::Render` and `CarRender` bail out before drawing when
 *
 *     eView::GetPixelSize(pos, 35.0f) < view->PixelMinSize
 *
 * with `GetPixelSize = radio * H / (distance - radio)`. `PixelMinSize` is field +0x24 of the view and
 * `eView::eView()` sets it to 4 for every view. The scene view is view 1 (0x82A38070 + 1 * 112 =
 * 0x82A380E0), the same one nfsmw_clips_race.cpp uses. It saves CPU and GPU at once because the
 * object never even reaches the draw list.
 *
 * Where it is written. The game recomputes it in sub_8243EC28 (`fctiwz` + `stfiwx` on vista+36), which
 * sub_82441100 calls once per active view per frame with the view in r4. So the hook goes on that function
 * and writes afterwards: the value read there is always the freshly recomputed one, never ours, and so,
 * unlike the shadow cutoff, there is no need to save the original here: an absolute value is written and
 * it cannot compound itself.
 *
 * Why it defaults to 0 (= like the game) and not to a higher value. The scene costs 22.55 ms real and it
 * is fragment shading: 6.5 M fragments over 0.92 M pixels. An object removed by the threshold is, by
 * definition, one that covers fewer than PixelMinSize pixels: going from 4 to 6 removes objects of at
 * most 36 pixels each. Even if 300 objects per frame went away (a quarter of the 1,207 the scene draws),
 * that would be 11 k fragments out of 6.5 M: 0.17 %, 0.04 ms. On the GPU this gains nothing, and the GPU
 * is what needs fixing in the scene pass.
 *
 * What it does gain is CPU: at 10.0 us per draw measured, 300 fewer draws are ~3 ms of CPU per frame. But
 * in a race the frame is GPU-bound (49.7 ms real GPU against ~29 of CPU), so those 3 ms do not turn into
 * a single FPS except in the alley stretches, which are CPU-bound.
 *
 * And what it costs visually. The distance at which an object of radius R disappears is
 * R * H / PixelMinSize. Raising the threshold does not suddenly erase distant objects: it brings that
 * distance closer in inverse proportion, so from 4 to 6 the cutoff distance drops 33 % for every object.
 * At 200 km/h (55 m/s) that means an object pops in about one second of travel closer than it does now,
 * and popping is the first thing anyone notices. On top of that the shadow map has its own cutoff
 * (nfsmw_shadows_cut): if the scene culls before the shadow does, the shadow of a missing object remains.
 *
 * So the lever exists, is in place and measured, but shipping it enabled would trade graphics for FPS
 * that do not materialize. The values:
 *
 *   value   cutoff distance       what is lost
 *   ------  --------------------  -------------------------------------------------------------
 *     4     like the game         nothing
 *     5     -20 %                 only what no longer reaches 4 output pixels: the scene draws at
 *                                 1280x720 and is resolved to 1024x576, so the game's 4 pixels
 *                                 are 3.2 in what is shown. The only value with a real argument.
 *     6     -33 %                 street furniture starts visibly popping in at mid distance
 *     8     -50 %                 clearly visible; only for measuring the savings ceiling
 *
 * The first time through, the view's H and the actual cutoff distance for each value are logged.
 */
REXCVAR_DEFINE_INT32(nfsmw_scene_detail_minimum, 0, "NFSMW",
                     "Size minimum en pixels para que un object se dibuje en la scene que se ve "
                     "(eView::PixelMinSize de la vista 1; el game usa 4). 0 = dejar el input_value del game. "
                     "Subirlo ACERCA la distance de cut de ALL los objects en proporcion inverse "
                     "(6 = -33 %), asi que se paga en popping; y en GPU casi no da nothing, porque lo que "
                     "quita son objects de minus de ese size en pixels. Ver la table del file")
    .range(0, 64);

namespace nfsmw::scene_detail {
namespace {

constexpr uint32_t kBaseViews = 0x82A38070;
constexpr uint32_t kBytesPorVista = 112;
constexpr uint32_t kViewScene = 1;
constexpr uint32_t kAddressScene = kBaseViews + kViewScene * kBytesPorVista;  // 0x82A380E0
constexpr uint32_t kOffH = 0x0C;          // float: the view's projection scale
constexpr uint32_t kOffNear = 0x10;      // float
constexpr uint32_t kOffFar = 0x14;      // float
constexpr uint32_t kOffDetail = 0x24;    // int PixelMinSize
constexpr float kRadioOfGame = 35.0f;   // the one WorldModel::Render passes to GetPixelSize

std::atomic<bool> g_noted_view{false};
std::atomic<int32_t> g_noted_value{0};

uint32_t Read32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}
void Write32(uint8_t* base, uint32_t dir, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + dir, &v, sizeof(v));
}
float AsFloat(uint32_t v) {
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}
double Cut(double h, double p) { return p > 0.0 ? kRadioOfGame * (1.0 + h / p) : 0.0; }

}  // namespace

/* Called right after sub_8243EC28, with the view it just recomputed. */
void Apply(uint8_t* base, uint32_t vista) {
  if (vista != kAddressScene) {
    return;
  }
  const uint32_t read = Read32(base, vista + kOffDetail);
  if (read == 0 || read > 4096) {
    return;  // does not look like a pixel size: touch nothing
  }
  if (!g_noted_view.exchange(true)) {
    const double h = double(AsFloat(Read32(base, vista + kOffH)));
    REXLOG_INFO("[clips] scene (vista {}): PixelMinSize del game {}, H {:.1f}, near {:.2f}, far_depth "
                "{:.1f}; distance de cut de un object de radio {:.0f}: con 4 {:.0f}, con 5 {:.0f}, con "
                "6 {:.0f}, con 8 {:.0f} units",
                kViewScene, read, h, double(AsFloat(Read32(base, vista + kOffNear))),
                double(AsFloat(Read32(base, vista + kOffFar))), double(kRadioOfGame),
                Cut(h, 4.0), Cut(h, 5.0), Cut(h, 6.0), Cut(h, 8.0));
  }
  const int32_t requested = REXCVAR_GET(nfsmw_scene_detail_minimum);
  // This setting only tightens: it never lowers the game's threshold, whatever the cvar says.
  if (requested <= 0 || uint32_t(requested) <= read) {
    if (g_noted_value.exchange(0, std::memory_order_relaxed) != 0) {
      REXLOG_INFO("[clips] scene: se devuelve el detail minimum del game ({})", read);
    }
    return;
  }
  Write32(base, vista + kOffDetail, uint32_t(requested));
  if (g_noted_value.exchange(requested, std::memory_order_relaxed) != requested) {
    REXLOG_INFO("[clips] scene: los objects de minus de {} pixels dejan de dibujarse (el game usa "
                "{}): la distance de cut se queda en el {:.0f} % de la suya",
                requested, read, 100.0 * double(read) / double(requested));
  }
}

}  // namespace nfsmw::scene_detail

/*
 * eView::Update (or equivalent): recomputes the view passed in r4 and leaves PixelMinSize at +0x24.
 * sub_82441100 calls it once per active view per frame, before drawing anything. Hooking here is the
 * only way to guarantee our value is the last one written.
 */
REX_EXTERN(__imp__sub_8243EC28);
REX_HOOK_RAW(sub_8243EC28) {
  const uint32_t vista = ctx.r4.u32;  // r4 may be clobbered inside: save it first
  __imp__sub_8243EC28(ctx, base);
  nfsmw::scene_detail::Apply(base, vista);
}

/*
 * =================================================================================================
 *  The menu with shadows, so it can be tested on the PC (nfsmw_test_menu_shadows)
 * =================================================================================================
 *
 * sub_824455B8 draws the menu scene and chooses between two paths:
 *   sub_82444D80  the garage with the shadow pass (sub_82443B18 with r4 = 0: one 1600x1600 map)
 *   sub_82445300  the same garage without the shadow pass
 * It takes the first one if sub_822D71A8(*(0x82A2C900), 0x82077C2C) finds the object, its +28 is not null,
 * that object's +120 is 0 and the global pointer 0x82A2D1B4 is null (sub_8245DD60 fills it when opening a
 * video with sub_826D76A8, and sub_82287380 and sub_8245DE38 release it).
 *
 * On the console, with the profile loaded, it takes the first one (07CEA000 and 086AE000 resolved in 99 %
 * of menu frames). On the PC, without a profile, it takes the second: not a single shadow pass, which is
 * why the menu flicker could not be reproduced there. This setting evaluates the four conditions and, if
 * the only one failing is the video, hides it while the path is chosen. Testing only, on the PC; when off
 * it touches nothing.
 */
REXCVAR_DEFINE_BOOL(nfsmw_test_menu_shadows, false, "NFSMW",
                    "Tests (build 193): anota por que la scene del menu va con o sin pass de shadows y, si solo lo "
                    "impide el pointer de video 0x82A2D1B4, lo oculta al choose para que el menu dibuje sus shadows "
                    "as en la consola con profile. Solo para reproducir en el PC el parpadeo del menu")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::test_menu_shadows {
namespace {
constexpr uint32_t kMovie = 0x82A2D1B4;
constexpr uint32_t kListFront = 0x82A2C900;
constexpr uint32_t kNameFront = 0x82077C2C;

std::atomic<uint64_t> g_with_shadows{0};
uint64_t g_calls = 0;
uint64_t g_forced = 0;
uint64_t g_with_shadows_noted = 0;
uint64_t g_calls_noted = 0;
uint32_t g_last_state = 0xFFFFFFFFu;

uint32_t Read32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}
void Write32(uint8_t* base, uint32_t dir, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + dir, &v, sizeof(v));
}
}  // namespace
}  // namespace nfsmw::test_menu_shadows

REX_EXTERN(__imp__sub_822D71A8);
REX_EXTERN(__imp__sub_82444D80);
REX_EXTERN(__imp__sub_824455B8);

// The menu scene with shadows: only counted.
REX_HOOK_RAW(sub_82444D80) {
  nfsmw::test_menu_shadows::g_with_shadows.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82444D80(ctx, base);
}

REX_HOOK_RAW(sub_824455B8) {
  using namespace nfsmw::test_menu_shadows;
  if (!REXCVAR_GET(nfsmw_test_menu_shadows)) {
    __imp__sub_824455B8(ctx, base);
    return;
  }
  // The four conditions, as the game checks them. sub_822D71A8 is a list search that moves the found item
  // to the front: calling it once more changes nothing. The registers are restored as they were.
  const PPCContext saved = ctx;
  ctx.r3.u64 = Read32(base, kListFront);
  ctx.r4.u64 = kNameFront;
  __imp__sub_822D71A8(ctx, base);
  const uint32_t object = ctx.r3.u32;
  ctx = saved;
  const uint32_t child = object ? Read32(base, object + 28) : 0;
  const uint32_t field120 = child ? Read32(base, child + 120) : 0xFFFFFFFFu;
  const uint32_t movie = Read32(base, kMovie);
  const bool solo_video = object && child && field120 == 0 && movie != 0;
  ++g_calls;
  if (solo_video) {
    ++g_forced;
    Write32(base, kMovie, 0);
  }
  __imp__sub_824455B8(ctx, base);
  if (solo_video && Read32(base, kMovie) == 0) {
    Write32(base, kMovie, movie);  // restored if nobody changed it inside
  }
  const uint32_t state = (object ? 1u : 0u) | (child ? 2u : 0u) | (field120 == 0 ? 4u : 0u) | (movie ? 8u : 0u);
  const uint64_t with_shadows = g_with_shadows.load(std::memory_order_relaxed);
  if (state != g_last_state || g_calls - g_calls_noted >= 300) {
    REXLOG_INFO("[test] menu con shadows (build 193): object {:08X}, +28 {:08X}, +120 {:08X}, video {:08X} -> {}; "
                "{} calls since la line previous, {} con shadows, {} forced since el arranque",
                object, child, field120, movie,
                !object || !child || field120 != 0 ? "SIN shadows (falla el object del front)"
                : movie                        ? "sin shadows por el video: se oculta el video al choose"
                                                  : "con shadows, as en la consola",
                g_calls - g_calls_noted, with_shadows - g_with_shadows_noted, g_forced);
    g_last_state = state;
    g_calls_noted = g_calls;
    g_with_shadows_noted = with_shadows;
  }
}
