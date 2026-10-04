// nfsmw - graphics work trimming during a race
//
// ===========================================================================
//  Why
//  Under Xenos emulation, a race ran at ~3 FPS with ~3,200 draws per frame,
//  at ~10,000 per second: every draw cost the same regardless of its pass.
//
//  How the game renders a race (recompiled code)
//    sub_824411B8  per-frame render: calls sub_82441100 and then
//                  sub_82445660.
//    sub_82441100  sub_8243FF30 enables views 1-5: byte +8 of each view
//                  in the table 0x82A38070 (112 bytes per view). In single
//                  player mode views 1 (scene) and 4 (reflection) remain.
//                  Then sub_824E4A20 -> sub_8243C6E0 enables the 6 faces of
//                  the car cubemap (pointers at 0x82A47E70) according to the
//                  row (counter 0x82A2CF24 % N of 0x828FB964) of the table
//                  0x828FBA88. It only prepares the views 1-17 that are active.
//    sub_82445660  with state 6 (0x82A39AD8) and no video, in this order:
//                  1600x1600 shadows, 640x360 road reflection (views
//                  4 and 5), 256x256 cubemap (views 18-23) and main view.
//    sub_824442E0  reflection pass: returns without doing anything if
//                  byte +8 of view 4 is 0.
//    sub_82444688  cubemap pass: skips each face whose byte +8 is 0.
//
//  What this file does, only in a race (state 6)
//  1. After sub_8243FF30, with nfsmw_reflection_road = false, it disables
//     views 4 and 5 before they are prepared: they are not prepared, drawn or
//     resolved. The reflection texture keeps whatever it last held.
//     Not used on the Switch: the water samples that reflection, and the
//     sea in the coastal area came out black.
//  2. After sub_8243C6E0, with nfsmw_cubemap_faces_max = k (0..6), it leaves
//     at most k faces active per frame and rotates which ones, so all of
//     them get updated. With -1 it keeps the game's choice. The faces in
//     nfsmw_cubemap_faces_always do not count toward that limit: they are
//     updated every frame.
//  Outside a race it touches nothing: menus and garage stay as in retail.
//
//  The rear-view mirror
//  In a console recording the mirror refreshed ~6 times per second with the
//  scene at ~30: the game enables the 6 faces every frame (a single row,
//  "1 1 1 1 1 1") and with a 1-face limit each one is refreshed 1 of every
//  6 frames. The mirror comes from one of those faces.
//  - Pinning face 1 (view 19, mask 2), based on a misread diagnostic, left
//    the mirror just as slow on the console.
//  - Measured frame by frame on the PC (window recording and draws per copy
//    from C2): it comes from face 2, view 20, which C2 resolves into surface
//    07517000, and it is the most detailed one (380-530 draws; the others
//    7-90). With view 19 pinned it is refreshed 1 of every 5 frames; with
//    view 20 pinned (mask 4), every frame.
//  - Mask 4 costs ~+270 draws per frame in a race. With it the mirror was
//    confirmed fixed on the console, with the race at 26-31 FPS. It is the
//    default on the Switch.
//
//  How to disable it without a new build
//  In nfsmw.toml:   nfsmw_reflection_road = true (already the default)
//                   nfsmw_cubemap_faces_max = -1
//                   nfsmw_cubemap_detail_minimum = 0
//
//  ===========================================================================
//  What is really behind the two profile lines (race)
//
//  The C2 profile says "reflection 0.95, 320 (cube y blur) 2.53" raw,
//  that is 1.55 and 4.12 ms real (x1.627). Both labels are misleading:
//
//  1) "reflection" is not the road reflection. The category is decided by the
//     render target width (CategoryOfTarget in nfsmw_native_draws.cpp):
//     everything with a pitch between 640 and 1279 lands there. What really
//     lands there is the 1024x576 output buffer (pitch 1040, height 576 =
//     599,040 texels), two passes per frame: 2 x 599,040 = 1.20 Mtexels,
//     which is exactly what C6 logs ("reflection 1.20 Mtexels en 2.0 passes")...
//     and it logs the same in the menus, where there is no road and no
//     reflection. With 68 draws per frame in a race and 4 in the menu: it is
//     the final composite plus the HUD.
//     The road reflection (640x360, views 4 and 5) was off on the Switch in
//     this measurement and does not appear in any profile line: it was not
//     drawn, not resolved and cost nothing. Lowering its resolution or
//     alternating frames saves nothing because there is nothing to lower.
//     Turning it on costs ms; leaving it off is a visual difference from the
//     Xbox 360 (it is on now, see nfsmw_reflection_road).
//
//  2) "320 (cube y blur)" is two different things mixed by pitch:
//     - The cubemap faces: 256x256 with pitch 320 (aligned to 80) = 81,920
//       texels per pass. Two faces are drawn per frame (C2 "faces
//       resolved_2": each of the five that rotate shows up 41-52 times per
//       ~10 s, that is one per frame among the five, plus the pinned one).
//     - The bloom chain: one 512x288 pass (pitch 560, also lands here) and
//       three or four at 256x144 (pitch 320). Its 128x72 siblings (pitch
//       160) land in "smaller".
//     Measured breakdown of the race frame (0.51 Mtexels, 6.2 passes, 311
//     draws, 4.12 ms real), with the known cost of 0.874 ms of GPU per 100
//     draws:
//       * the 311 cubemap face draws: ~2.7 ms  (66 %)
//       * the 6.2 open passes and their tile load: ~0.9 ms
//       * filling the 0.51 Mtexels: ~0.4 ms
//     So the cubemap does not cost pixels, it costs draws. Lowering the face
//     resolution barely touches those 2.7 ms; removing objects does.
//
//  3) The radial blur is no longer paid for. The real one (acceleration and
//     NOS) is 7 of the 12 samples of the composite quad (p_000139), and
//     nfsmw_native_no_blur removes it by default (1.5-2.0 ms real).
//     The log shows it applied: the specializations of those pipelines carry
//     bit 19 (0x80000). The seven samples read the same texture as the
//     center one (DIFFUSEMAP, the scene), not a separate chain, so removing
//     it leaves no orphan pass to cut. What remains in the "320" bucket is
//     not that blur: it is the bloom, and that one is visible.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports
#include "nfsmw_reflection_on_demand.h"
#include <rex/platform.h>

/*
 * On by default on the Switch as well: without it the sea in the coastal area came out black.
 *
 * The water samples this reflection's texture (views 4 and 5). With the trim, that texture is never
 * repainted during a race: on the console the sea in Heritage & Omega came out black, and on the PC, with
 * the trim forced on, flat and dark, without the sky or the cliff reflected. The trim dates from the Xenos
 * emulation era, when races ran below 10 FPS; today it changes the look, and graphics are not traded for
 * FPS.
 * It costs ~109 draws per frame (14 k triangles at 640x360).
 */
REXCVAR_DEFINE_BOOL(nfsmw_reflection_road, true, "NFSMW",
                    "Draw el reflection de la road (pass 640x360) during la race; el water tambien "
                    "lo samples, sin el el mar sale black")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * The reflection, only when something reads it.
 *
 * Once enabled for the sea, the reflection was drawn in every race frame: ~210 draws on average, up to 280
 * in Heritage & Omega. In one run, 42 % of those draws were for copies nobody read: halfway through
 * Heritage the C2 report said "0 reads", and in Ironwood it was only read in 37 % of frames. With the
 * ring as the bottleneck, that is ~2 ms of CPU per frame wasted in those stretches.
 *
 * The renderer records when the reflection texture is read (nfsmw_reflection_on_demand.h). If it was read in
 * the last 20 frames it is always drawn, as before. Otherwise only 1 of every nfsmw_reflection_refresh: when
 * the water becomes visible again, its first frame reads a reflection at most 3 frames old (~100 ms) and
 * the next one is already up to date.
 * Self-checking guard: in each run, for the first 90 frames it is always drawn and the guard checks that
 * address 0x07C5A000 really is the reflection; if not, or if it keeps being resolved in skipped frames, it
 * turns itself off and says so in the log. false = always draw it.
 */
REXCVAR_DEFINE_BOOL(nfsmw_reflection_low_demand, true, "NFSMW",
                    "Draw el reflection de la road solo cuando algo lo reads; sin reads recientes se renueva 1 de "
                    "every nfsmw_reflection_refresh frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measured on the PC (standing at the Heritage start): with 16 instead of the game's 6 the reflection
 * stays at 295-331 draws per copy (317-327 without it): its objects are already large. It gains nothing,
 * so it is off.
 */
REXCVAR_DEFINE_INT32(nfsmw_reflection_detail_minimum, 0, "NFSMW",
                     "Objects de minus de N pixels que no se dibujan en el reflection de la road (views 4 y 5), as "
                     "nfsmw_cubemap_detail_minimum en el cube; 0 = el input_value del game (measured sin effect con 16)")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_reflection_refresh, 4, "NFSMW",
                     "Con nfsmw_reflection_low_demand: every how_many_2 frames se renueva el reflection mientras nadie lo "
                     "reads (el water que aparece lo ve as mucho con ese delay)")
    .range(1, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * The reflection, only if the water is really visible.
 *
 * Deciding by reads alone, the reflection stopped being refreshed in stretches without reads (up to 75 %),
 * but at the Heritage & Omega start it was read in 100 % of frames, with 175-338 draws per copy, and that
 * is exactly the area that drops to 21 FPS. In PC captures of that start (at 100, 110 and 120 s) the sea
 * does not appear: the game submits the water because it falls inside the frustum, and the terrain covers
 * it completely.
 *
 * Now the ring wraps every draw that samples the reflection in an occlusion query and, when the GPU
 * finishes that work (1-2 frames later), records whether it left any sample. If none of those draws has
 * left a single sample in the last 20 frames, the reflection is refreshed 1 of every
 * nfsmw_reflection_refresh, as when nobody reads it. A draw with no samples wrote nothing to any render
 * target, so that reflection never reaches the image. The only change is when the water reappears: its
 * first 3-4 frames show a reflection up to ~7 frames old.
 * Whatever cannot be measured (no room for the query, a game query already open, the deferred sky) is
 * treated as visible.
 * Self-checking guard: the final race composite (p_000139, full screen) is measured the same way every so
 * often and must leave samples; until 8 good witnesses, the decision is still made by reads, and if any
 * gives 0 it falls back to reads for the whole run and says so in the log. false = by reads.
 */
REXCVAR_DEFINE_BOOL(nfsmw_reflection_visibility, true, "NFSMW",
                    "Con nfsmw_reflection_low_demand: el reflection se renueva en all los frames solo si el water leaves "
                    "alguna sample en pantalla (query de occlusion), y no solo porque se mande a draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::reflection_demand {
namespace {
std::atomic<uint64_t> g_swaps{0};
std::atomic<uint64_t> g_last_read{0};  // Swap number of the last read, plus 1 (0 = never)
std::atomic<uint64_t> g_reads{0};
std::atomic<uint64_t> g_copies{0};
// nfsmw_reflection_visibility. The ring thread writes these and the game thread reads them.
constexpr uint64_t kWitnessesForApply = 8;
enum PhaseVisibility : int { kVisWatching = 0, kVisApplying = 1, kVisOff = 2 };
std::atomic<int> g_phase_visibility{kVisWatching};
std::atomic<uint64_t> g_last_visible{0};  // Swap number when the last query with samples was read, plus 1
std::atomic<uint64_t> g_visibles{0};
std::atomic<uint64_t> g_hidden{0};
std::atomic<uint64_t> g_without_measurement{0};
std::atomic<uint64_t> g_witnesses_ok{0};
std::atomic<uint64_t> g_witnesses_mal{0};
}  // namespace

void NoteRead() {
  g_reads.fetch_add(1, std::memory_order_relaxed);
  g_last_read.store(g_swaps.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void NoteCopy() { g_copies.fetch_add(1, std::memory_order_relaxed); }

void NoteSwap() { g_swaps.fetch_add(1, std::memory_order_relaxed); }

bool MeasureVisibility() {
  return REXCVAR_GET(nfsmw_reflection_visibility) && REXCVAR_GET(nfsmw_reflection_low_demand) &&
         REXCVAR_GET(nfsmw_reflection_road) &&
         g_phase_visibility.load(std::memory_order_relaxed) != kVisOff;
}

bool VisibilityChecked() { return g_phase_visibility.load(std::memory_order_relaxed) == kVisApplying; }

void NoteVisible(bool measured) {
  (measured ? g_visibles : g_without_measurement).fetch_add(1, std::memory_order_relaxed);
  g_last_visible.store(g_swaps.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void NoteHidden() { g_hidden.fetch_add(1, std::memory_order_relaxed); }

void NoteWitness(bool with_samples) {
  if (!with_samples) {
    g_witnesses_mal.fetch_add(1, std::memory_order_relaxed);
    if (g_phase_visibility.exchange(kVisOff, std::memory_order_relaxed) != kVisOff) {
      REXLOG_ERROR("[clips] reflection por visibility: DIFFERENCE, la composition final (pantalla whole) dio 0 "
                   "sample_total en su query de occlusion after {} witnesses buenos; the queries no son de fiar y el "
                   "reflection vuelve a decidirse por reads (as la 191) el rest de la session",
                   g_witnesses_ok.load(std::memory_order_relaxed));
    }
    return;
  }
  const uint64_t ok = g_witnesses_ok.fetch_add(1, std::memory_order_relaxed) + 1;
  int expected = kVisWatching;
  if (ok >= kWitnessesForApply &&
      g_phase_visibility.compare_exchange_strong(expected, kVisApplying, std::memory_order_relaxed)) {
    REXLOG_INFO("[clips] reflection por visibility: comprobado ({} witnesses de la composition final con sample_total, 0 "
                "sin ninguna); since here el reflection se renueva en all los frames solo si el water leaves sample_total en "
                "pantalla",
                ok);
  }
}

}  // namespace nfsmw::reflection_demand

REXCVAR_DEFINE_INT32(nfsmw_cubemap_faces_max, REX_PLATFORM_SWITCH != 0 ? 1 : -1, "NFSMW",
                     "Faces del map de environment del car que se actualizan por frame en "
                     "race (-1 = the que decide el game)")
    .range(-1, 6)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// On the Switch it is 4: face 2 (view 20), the rear-view mirror's. Confirmed fixed on the console (mask 4,
// race at 26-31 FPS). It costs ~+270 draws per frame. It used to be 2: face 1 (view 19), which is not the
// mirror's.
/*
 * The rear-view mirror draws 415 objects into one 256x256 face.
 *
 * That is 86 % of the cubemap cost and 19.6 % of all the draws in the frame. And it is not using more
 * detail: it draws at 61.5 triangles per object, almost the same as the main scene (68.1), in 1/14 of the
 * area. Its density is 6,336 draws per Mpixel against 1,117 for the screen: 5.7 times.
 *
 * The reason is in the binary, not an assumption. sub_82216600 builds the draw-list filter mask of each
 * view and ends by writing it to vista+132. Inside it (nfsmw_recomp.17, loc_822166D4) there is this:
 *
 *     if (viewId == 20) { mask &= ~0x40; mask &= ~0x08000000; }
 *
 * So the game removes, from view 20 and only from it, two culling bits that the other five cubemap faces
 * do carry. That is why the mirror walks the whole world while its siblings draw between 6 and 95 objects.
 *
 * This cvar gives them back. It is reversible in one line and does not touch the mirror's refresh rate,
 * which still updates every frame.
 *
 * Verified on the console with this enabled; not to be re-checked.
 */
REXCVAR_DEFINE_BOOL(nfsmw_mirror_clip, true, "NFSMW",
                    "Return al mirror (vista 20) los dos bits de clip de la list de draw que el "
                    "game le quita a el solo. Dibuja 415 objects where sus hermanas dibujan 6-95, y son el "
                    "19,6 % de los draws del frame");

REXCVAR_DEFINE_INT32(nfsmw_cubemap_faces_always, REX_PLATFORM_SWITCH != 0 ? 4 : 0, "NFSMW",
                     "Faces del map de environment que se actualizan all los frames aunque haya "
                     "limit (bit i = face i de la table del game; no cuentan para "
                     "nfsmw_cubemap_faces_max)")
    .range(0, 63)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * The cubemap costs per draw, so it gets fewer draws.
 *
 * The face refreshed every frame draws between 10 and 100 objects (C2 "faces resolved_2", "draws por
 * copy"), and at 0.874 ms of GPU per 100 draws that is most of the pass's 4.12 ms real. The game decides
 * which object enters a view with eView::PixelMinSize: in WorldModel::Render and in CarRender, if the
 * object's projected size is below that number, the call bails out before drawing (saving CPU and GPU at
 * once). The eView constructor sets it to 4 for every view, so a 256x256 environment map face applies the
 * same threshold as the whole screen.
 *
 * An object covering 8 pixels of a 256x256 face ends up, reflected on the curved body of a moving car,
 * well below one pixel on screen. Raising the threshold only on the rotating faces removes exactly those
 * objects.
 *
 * Since the field offset is deduced from the decompilation (nfsmwdecomp, eView +0x24 in a 112-byte
 * structure) and cannot be tested here, the code checks the layout before writing: the seven views it
 * cares about (the scene and the six faces) must read exactly the constructor's 4. If a single one does
 * not match, it writes nothing and logs it.
 *
 * How to measure it: in the log, "C2 faces resolved_2 since el report previous" gives the draws per copy
 * of each face. On the PC it shows right away because all six are drawn every frame there.
 */
REXCVAR_DEFINE_INT32(nfsmw_cubemap_detail_minimum, 8, "NFSMW",
                     "Size minimum en pixels para que un object se dibuje en one face del map de "
                     "environment del car (eView::PixelMinSize; el game usa 4 en all the views). Solo "
                     "se aplica a the faces que rotate, never a the fixed. 0 = dejar el input_value del game")
    .range(0, 64);

REXCVAR_DEFINE_INT32(nfsmw_cubemap_diag_cycle_s, 0, "NFSMW",
                     "Diagnostic: con N > 0, en race solo se actualiza one face del map de environment "
                     "y cambia de face every N seconds (anota every change), para ver which usa el "
                     "mirror")
    .range(0, 60);

namespace nfsmw::clips_race {
namespace {

constexpr uint32_t kBaseViews = 0x82A38070;
constexpr uint32_t kBytesPorVista = 112;
constexpr uint32_t kViews = 24;
constexpr uint32_t kOffActive = 8;
constexpr uint32_t kViewReflection = 4;
constexpr uint32_t kViewReflectionSecond = 5;
constexpr uint32_t kStateGame = 0x82A39AD8;
constexpr uint32_t kStateRace = 6;
constexpr uint32_t kFaces = 6;
constexpr uint32_t kNumRowsFaces = 0x828FB964;
constexpr uint32_t kRowsFaces = 0x828FBA88;
constexpr uint32_t kViewScene = 1;

/*
 * eView fields (nfsmwdecomp, src/Speed/Indep/Src/Ecstasy/Ecstasy.hpp). The class is 0x68 bytes and is
 * aligned to 16 because of the bVector3 at +0x28, hence the 112 bytes per view in this table. What this
 * file already used matches the decompilation: ID at +4 and Active at +8.
 */
constexpr uint32_t kOffH = 0x0C;               // float, the view's "pixel" scale
constexpr uint32_t kOffNearZ = 0x10;           // float
constexpr uint32_t kOffFarZ = 0x14;            // float
constexpr uint32_t kOffFovBias = 0x18;         // float
constexpr uint32_t kOffFovDegrees = 0x1C;       // float
constexpr uint32_t kOffByN = 0x20;             // int BlackAndWhiteMode
constexpr uint32_t kOffDetailMinimum = 0x24;   // int PixelMinSize
// bVector3 ViewDirection, eView+0x28 (Ecstasy.hpp:166). It is the direction the view points to, and it
// tells which cubemap face is which without guessing.
constexpr uint32_t kOffAddress = 0x28;       // bVector3 ViewDirection (x,y,z floats)
constexpr int32_t kDetailMinimumGame = 4;     // eView::eView() sets it to 4 in every view

void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + address, &v, sizeof(v));
}

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

uint32_t AddressActive(uint32_t vista) {
  return kBaseViews + vista * kBytesPorVista + kOffActive;
}

float ReadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t v = Read32(base, address);
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

uint32_t AddressView(uint32_t vista) {
  return kBaseViews + vista * kBytesPorVista;
}

bool InRace(const uint8_t* base) {
  return Read32(base, kStateGame) == kStateRace;
}

// Only pointers to the start of a view in the table are accepted.
bool EsVista(uint32_t address) {
  return address >= kBaseViews && address < kBaseViews + kViews * kBytesPorVista &&
         (address - kBaseViews) % kBytesPorVista == 0;
}

int NumberOfView(uint32_t address) {
  return EsVista(address) ? int((address - kBaseViews) / kBytesPorVista) : -1;
}

std::atomic<bool> g_table_registered{false};
std::atomic<bool> g_warning_reflection{false};
std::atomic<bool> g_warning_faces{false};
std::atomic<bool> g_warning_pointer{false};
std::atomic<bool> g_warning_views_faces{false};
// Set only once all six faces have been named, not when trying.
std::atomic<bool> g_warning_faces_named{false};
std::atomic<uint32_t> g_rotation{0};
// Faces already drawn at least once in this race. Until all six are, none is trimmed: a delayed
// reflection is acceptable, a black one is not.
constexpr uint32_t kAllTheFaces = (1u << kFaces) - 1u;
uint32_t g_faces_first_used = 0;
std::atomic<bool> g_warning_first_use{false};
std::atomic<bool> g_warning_first_use_missing{false};
std::atomic<bool> g_warning_mirror{false};
std::atomic<int> g_face_diag{-1};
// Environment map minimum detail: 0 = nothing written to that face yet.
// Only touched from the game thread, inside the sub_8243C6E0 hook.
int32_t g_detail_applied[kFaces] = {0, 0, 0, 0, 0, 0};
/*
 * The value the game really puts in each face, learned the first time it is seen. An earlier version
 * assumed it was 4 (what the constructor sets), and so the threshold was never applied: measured on the
 * console, the game uses 12 in the scene and 2 in the cubemap faces, so the "if it is not the game's
 * value, someone else is in charge" guard always rejected it. Result: the six faces cost +2.14 ms without
 * the saving meant to offset them. -1 = not seen yet.
 */
int32_t g_detail_of_game[kFaces] = {-1, -1, -1, -1, -1, -1};
int g_layout_vista = 0;  // 0 = unchecked, 1 = matches, -1 = mismatch (never written)
std::atomic<bool> g_warning_detail{false};

// In how many race frames the mirror face (view 20) is updated, logged every 10 s. Counted after
// sub_8243FF30, which only enables views 1-5: the table keeps the previous frame's final activation, with
// the face limit already applied. Only from the game's main thread. The face being active does not
// guarantee the mirror changes (with the wrong face pinned this read 100 %): the real rate is measured
// on the image.
constexpr uint32_t kViewMirror = 20;
struct CounterMirror {
  int64_t since_ms = 0;
  uint32_t frames = 0;
  uint32_t updated = 0;
};
CounterMirror g_mirror;

void CountMirror(const uint8_t* base) {
  if (!InRace(base)) {
    g_mirror = {};
    return;
  }
  using namespace std::chrono;
  const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  if (g_mirror.since_ms == 0) {
    g_mirror.since_ms = now;  // the race's first frame does not have the limited table yet
    return;
  }
  ++g_mirror.frames;
  if (base[AddressActive(kViewMirror)] != 0) {
    ++g_mirror.updated;
  }
  if (now - g_mirror.since_ms >= 10000) {
    NFSMW_REPORT_DEFERRED("[clips] race, last {:.1f} s: {} frames; la face del mirror (vista 20) se "
                "actualizo en {} ({:.0f} %)",
                double(now - g_mirror.since_ms) / 1000.0, g_mirror.frames,
                g_mirror.updated,
                100.0 * double(g_mirror.updated) / double(std::max<uint32_t>(g_mirror.frames, 1)));
    g_mirror = {now, 0, 0};
  }
}

void RegistrarTableFaces(const uint8_t* base) {
  if (g_table_registered.exchange(true)) {
    return;
  }
  const uint32_t rows = Read32(base, kNumRowsFaces);
  REXLOG_INFO("[clips] cubemap del game: {} row(s) de activacion de faces", rows);
  if (rows == 0 || rows > 32) {
    return;
  }
  for (uint32_t f = 0; f < rows; ++f) {
    const uint32_t d = kRowsFaces + 24 * f;
    REXLOG_INFO("[clips] row {}: {} {} {} {} {} {}", f, Read32(base, d), Read32(base, d + 4),
                Read32(base, d + 8), Read32(base, d + 12), Read32(base, d + 16),
                Read32(base, d + 20));
  }
}

/*
 * Before writing a single byte into the view structure, the location of PixelMinSize must be certain.
 * The required signature is strong: the scene view and the six cubemap faces must read exactly the 4 the
 * eView constructor sets. A wrong offset would land on FovDegrees (a float such as 90), on ViewDirection
 * (floats) or on pCamera (a 0x8xxxxxxx pointer): all seven reading 4 right there is practically
 * impossible. The whole window is also logged once, so it can be confirmed in the console log.
 */
bool CheckLayoutOfView(const uint8_t* base, uint32_t table) {
  if (g_layout_vista != 0) {
    return g_layout_vista > 0;
  }
  /*
   * The field is checked for a plausible value, not a specific number. Requiring exactly 4 (what
   * eView::eView sets) in the scene and the six faces never holds in a race: measured on the console, the
   * scene uses 12 and the faces 2. The check always returned false and nfsmw_cubemap_detail_minimum
   * touched nothing, which cost 2.14 ms.
   */
  const uint32_t scene = AddressView(kViewScene);
  auto plausible = [&](uint32_t v) {
    const int32_t d = int32_t(Read32(base, v + kOffDetailMinimum));
    return d > 0 && d <= 64;
  };
  bool matches = plausible(scene);
  for (uint32_t i = 0; i < kFaces && matches; ++i) {
    const uint32_t vista = Read32(base, table + 4 * i);
    if (!EsVista(vista)) {
      matches = false;
      break;
    }
    matches = plausible(vista);
  }
  const uint32_t face0 = Read32(base, table);
  if (EsVista(face0)) {
    REXLOG_INFO("[clips] cube, face 0 (vista {}): H {:.1f} near {:.2f} far_depth {:.1f} fovbias {:.3f} "
                "fov {:.1f} byn {} detail {}; scene detail {}",
                NumberOfView(face0), ReadFloat(base, face0 + kOffH),
                ReadFloat(base, face0 + kOffNearZ), ReadFloat(base, face0 + kOffFarZ),
                ReadFloat(base, face0 + kOffFovBias), ReadFloat(base, face0 + kOffFovDegrees),
                Read32(base, face0 + kOffByN), int32_t(Read32(base, face0 + kOffDetailMinimum)),
                int32_t(Read32(base, scene + kOffDetailMinimum)));
  }
  g_layout_vista = matches ? 1 : -1;
  if (!matches) {
    REXLOG_WARN("[clips] cube: la structure de la vista no matches (PixelMinSize en +0x{:X} "
                "deberia ser un whole between 1 y 64 en la scene y en the six faces); "
                "nfsmw_cubemap_detail_minimum no va a touch nothing",
                kOffDetailMinimum);
  } else {
    REXLOG_INFO("[clips] cube: la structure de la vista matches; la scene usa PixelMinSize {} y "
                "the faces {} {} {} {} {} {}",
                int32_t(Read32(base, scene + kOffDetailMinimum)),
                int32_t(Read32(base, Read32(base, table) + kOffDetailMinimum)),
                int32_t(Read32(base, Read32(base, table + 4) + kOffDetailMinimum)),
                int32_t(Read32(base, Read32(base, table + 8) + kOffDetailMinimum)),
                int32_t(Read32(base, Read32(base, table + 12) + kOffDetailMinimum)),
                int32_t(Read32(base, Read32(base, table + 16) + kOffDetailMinimum)),
                int32_t(Read32(base, Read32(base, table + 20) + kOffDetailMinimum)));
  }
  return matches;
}

/*
 * Raises the object size threshold only on the rotating faces. Never on the pinned ones
 * (nfsmw_cubemap_faces_always) and never on view 20, whatever the cvars say.
 */
void AdjustDetailCube(uint8_t* base, uint32_t table, uint32_t face, uint32_t vista,
                        uint32_t always) {
  if (face >= kFaces || (always & (1u << face)) || NumberOfView(vista) == int(kViewMirror)) {
    return;
  }
  const int32_t actual = int32_t(Read32(base, vista + kOffDetailMinimum));
  /*
   * The game's value is learned, not assumed. The first time a face is seen, whatever it holds is the
   * game's value (nothing has been written to it yet). Assuming the constructor's 4 did not work: the
   * faces hold 2 in a race. It must be plausible, so garbage from a half-initialized view is not learned.
   */
  if (g_detail_of_game[face] < 0) {
    if (actual <= 0 || actual > 64) {
      return;  // view not initialized: retry on the next frame
    }
    g_detail_of_game[face] = actual;
  }
  const int32_t requested = REXCVAR_GET(nfsmw_cubemap_detail_minimum);
  const int32_t wanted = requested > 0 ? requested : g_detail_of_game[face];
  if (actual == wanted) {
    return;
  }
  // Only overwrite what the game or we put there: any other value means someone else is in charge, so it
  // is left alone.
  if (actual != g_detail_of_game[face] && actual != g_detail_applied[face]) {
    return;
  }
  if (!CheckLayoutOfView(base, table)) {
    return;
  }
  Write32(base, vista + kOffDetailMinimum, uint32_t(wanted));
  g_detail_applied[face] = wanted;
  if (!g_warning_detail.exchange(true)) {
    REXLOG_INFO("[clips] cube: los objects de minus de {} pixels dejan de dibujarse en the faces "
                "que rotate (el game usa {}); the faces fixed no se tocan",
                wanted, kDetailMinimumGame);
  }
}

// Mirror diagnostic: a single active face, changing every 'cycle_s' seconds.
void DiagnosticOneFace(uint8_t* base, uint32_t table, int32_t cycle_s) {
  using namespace std::chrono;
  const int64_t ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  const int face = int((ms / (int64_t(cycle_s) * 1000)) % kFaces);
  for (uint32_t i = 0; i < kFaces; ++i) {
    const uint32_t vista = Read32(base, table + 4 * i);
    if (EsVista(vista) && int(i) != face) {
      base[vista + kOffActive] = 0;
    }
  }
  if (g_face_diag.exchange(face) != face) {
    REXLOG_INFO("[clips] diagnostic: solo la face {} del cubemap (vista {})", face,
                NumberOfView(Read32(base, table + 4 * uint32_t(face))));
  }
}

/*
 * Which cubemap face is which.
 *
 * Only face 2 (view 20, the mirror) had been confirmed, by trial and error: pinning face 1 in the belief
 * that it was the mirror left the mirror just as slow. The other five were unknown, so there was no way
 * to decide which ones are worth refreshing more often for the car body reflection without paying for
 * all of them.
 *
 * `eView` has `bVector3 ViewDirection` at +0x28 (Ecstasy.hpp:166): the direction the view looks at. With
 * it each face is named without guessing. The game's Y axis is height (the cameras use Position.y for
 * elevation in GetPredictedZone), so +Y = sky and -Y = ground.
 *
 * Logged once per run, on entering a race. Cost: six 12-byte reads.
 */
const char* NameOfAddress(float x, float y, float z) {
  const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
  if (ay >= ax && ay >= az) return y > 0 ? "UP (sky)" : "DOWN (floor)";
  if (az >= ax) return z > 0 ? "AHEAD" : "BEHIND";
  return x > 0 ? "RIGHT" : "LEFT";
}

void DiagnosticFacesOfCube(const uint8_t* base, uint32_t table) {
  /*
   * This cannot be read on the first call: there all six faces came out with direction 0,0,0, fov 0 and
   * far 0, because the cubemap views did not have their parameters yet. It waits until face 0 has a
   * believable fov and far distance; until then it retries on the next frame.
   */
  for (uint32_t i = 0; i < kFaces; ++i) {
    const uint32_t vista = Read32(base, table + 4 * i);
    if (!EsVista(vista)) {
      return;  // no valid table yet: retry
    }
  }
  const uint32_t face0 = Read32(base, table);
  const float fov0 = ReadFloat(base, face0 + kOffFovDegrees);
  const float far0 = ReadFloat(base, face0 + kOffFarZ);
  if (!(fov0 > 1.0f) || !(far0 > 1.0f)) {
    return;  // views half-initialized: retry on the next frame
  }
  const uint32_t always = uint32_t(REXCVAR_GET(nfsmw_cubemap_faces_always));
  const int32_t maximum = REXCVAR_GET(nfsmw_cubemap_faces_max);
  for (uint32_t i = 0; i < kFaces; ++i) {
    const uint32_t vista = Read32(base, table + 4 * i);
    const float x = ReadFloat(base, vista + kOffAddress);
    const float y = ReadFloat(base, vista + kOffAddress + 4);
    const float z = ReadFloat(base, vista + kOffAddress + 8);
    const bool fixed = (always & (1u << i)) != 0;
    REXLOG_INFO("[clips] cube face {} = vista {} -> {} (dir {:+.2f} {:+.2f} {:+.2f}); "
                "fov {:.1f} far_depth {:.0f} detail {}; {}",
                i, NumberOfView(vista), NameOfAddress(x, y, z), double(x), double(y), double(z),
                double(ReadFloat(base, vista + kOffFovDegrees)),
                double(ReadFloat(base, vista + kOffFarZ)),
                int32_t(Read32(base, vista + kOffDetailMinimum)),
                fixed ? "ALWAYS (no count para el limit)" : "rota con the demas");
  }
  const int rotate = int(kFaces) - __builtin_popcount(always & 0x3Fu);
  REXLOG_INFO("[clips] cube: {} faces rotate a {} por frame, asi que every one se renueva 1 de "
              "every {:.1f} frames; the fixed, en all",
              rotate, maximum,
              maximum > 0 ? double(rotate) / double(maximum > rotate ? rotate : maximum) : double(rotate));
  g_warning_faces_named.store(true, std::memory_order_relaxed);
}

// nfsmw_reflection_low_demand. Only from the game's main thread (sub_8243FF30 hook).
constexpr uint32_t kReflectionGrace = 20;    // renderer frames without a read before spacing it out
constexpr uint32_t kReflectionWatching = 90;   // frames always drawing it, to check the address
enum class PhaseReflection { kWatching, kApplying, kOff };
struct StateReflection {
  PhaseReflection phase = PhaseReflection::kWatching;
  uint32_t watching = 0;
  uint64_t copies_start = 0;
  uint32_t since_draw = 0;
  int64_t since_ms = 0;
  uint32_t requested_2 = 0;    // race frames in which the game requests it
  uint32_t drawn = 0;  // of those, the ones allowed to draw
  uint64_t copies_report = 0;
  uint64_t reads_report = 0;
  uint64_t visibles_report = 0;   // nfsmw_reflection_visibility
  uint64_t hidden_report = 0;
  uint64_t without_measurement_report = 0;
};
StateReflection g_reflection;

const char* NamePhase(PhaseReflection phase) {
  return phase == PhaseReflection::kApplying ? "APPLYING" : phase == PhaseReflection::kWatching ? "watching" : "OFF";
}

void ReportReflection(int64_t now) {
  namespace rd = nfsmw::reflection_demand;
  StateReflection& e = g_reflection;
  const uint64_t copies = rd::g_copies.load(std::memory_order_relaxed);
  const uint64_t reads = rd::g_reads.load(std::memory_order_relaxed);
  const uint64_t visibles = rd::g_visibles.load(std::memory_order_relaxed);
  const uint64_t hidden = rd::g_hidden.load(std::memory_order_relaxed);
  const uint64_t without_measurement = rd::g_without_measurement.load(std::memory_order_relaxed);
  if (e.since_ms == 0) {
    e.since_ms = now;
    e.copies_report = copies;
    e.reads_report = reads;
    e.visibles_report = visibles;
    e.hidden_report = hidden;
    e.without_measurement_report = without_measurement;
    return;
  }
  if (now - e.since_ms < 10000) {
    return;
  }
  const uint64_t dc = copies - e.copies_report;
  const uint64_t dl = reads - e.reads_report;
  // If copies to that address keep appearing while skipping, it is not the reflection: stop skipping.
  if (e.phase == PhaseReflection::kApplying && e.drawn + 10 + e.drawn / 10 < dc) {
    e.phase = PhaseReflection::kOff;
    REXLOG_ERROR("[clips] reflection low demand: DIFFERENCE, {} copies a {:08X} con solo {} frames de reflection "
                 "drawn; se apaga y el reflection vuelve a dibujarse always",
                 dc, nfsmw::reflection_demand::kAddress, e.drawn);
  }
  NFSMW_REPORT_DEFERRED("[clips] reflection low demand (build 191), last {:.1f} s: requested en {} frames, "
                         "dibujado en {} y saltado en {} ({:.0f} %); {} copies y {} reads de {:08X}; phase {}",
                         double(now - e.since_ms) / 1000.0, e.requested_2, e.drawn, e.requested_2 - e.drawn,
                         100.0 * double(e.requested_2 - e.drawn) / double(std::max<uint32_t>(e.requested_2, 1)), dc, dl,
                         nfsmw::reflection_demand::kAddress, NamePhase(e.phase));
  // What the queries of the draws that sample the reflection reported in this interval.
  if (REXCVAR_GET(nfsmw_reflection_visibility)) {
    const int phase = rd::g_phase_visibility.load(std::memory_order_relaxed);
    NFSMW_REPORT_DEFERRED("[clips] reflection por visibility (build 192): {} draws del water measured, {} con sample_total "
                           "en pantalla y {} tapados del todo; {} sin poder measure (se dan por visibles); witnesses {} "
                           "ok y {} mal; decide por {}",
                           (visibles - e.visibles_report) + (hidden - e.hidden_report),
                           visibles - e.visibles_report, hidden - e.hidden_report,
                           without_measurement - e.without_measurement_report,
                           rd::g_witnesses_ok.load(std::memory_order_relaxed),
                           rd::g_witnesses_mal.load(std::memory_order_relaxed),
                           phase == rd::kVisApplying ? "VISIBILITY"
                           : phase == rd::kVisWatching ? "reads (checking los witnesses)"
                                                     : "reads (guard disparada)");
  }
  e.since_ms = now;
  e.requested_2 = 0;
  e.drawn = 0;
  e.copies_report = copies;
  e.reads_report = reads;
  e.visibles_report = visibles;
  e.hidden_report = hidden;
  e.without_measurement_report = without_measurement;
}

/*
 * nfsmw_reflection_detail_minimum: the same threshold as the cubemap faces (nfsmw_cubemap_detail_minimum) but
 * on the reflection views. At the Heritage & Omega start the reflection is ~436 draws (PC dump) and the
 * game asks it for small objects just as for the scene, although on the water they come out scaled by
 * 0.2-0.6 and distorted by the waves. The game's value is learned the first time (plausible: 1-64) and
 * only its value or ours is overwritten; if the view's ID (+4) does not say 4 or 5, it is left alone.
 */
int32_t g_reflection_detail_game[2] = {-1, -1};
int32_t g_reflection_detail_applied[2] = {0, 0};
std::atomic<bool> g_warning_reflection_detail{false};

void AdjustDetailReflection(uint8_t* base) {
  const int32_t requested = REXCVAR_GET(nfsmw_reflection_detail_minimum);
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t number = i == 0 ? kViewReflection : kViewReflectionSecond;
    const uint32_t vista = AddressView(number);
    if (Read32(base, vista + 4) != number) {
      continue;  // not the view we think it is
    }
    const int32_t actual = int32_t(Read32(base, vista + kOffDetailMinimum));
    if (g_reflection_detail_game[i] < 0) {
      if (actual <= 0 || actual > 64) {
        continue;  // not initialized: try another frame
      }
      g_reflection_detail_game[i] = actual;
    }
    const int32_t wanted = requested > 0 ? std::max(requested, g_reflection_detail_game[i]) : g_reflection_detail_game[i];
    if (actual == wanted || (actual != g_reflection_detail_game[i] && actual != g_reflection_detail_applied[i])) {
      continue;  // already set, or someone else is in charge
    }
    Write32(base, vista + kOffDetailMinimum, uint32_t(wanted));
    g_reflection_detail_applied[i] = wanted;
    if (!g_warning_reflection_detail.exchange(true)) {
      REXLOG_INFO("[clips] reflection: los objects de minus de {} pixels dejan de dibujarse en el reflection de la "
                  "road (vista {}: el game usa {})",
                  wanted, number, g_reflection_detail_game[i]);
    }
  }
}

// After the game enables the frame's views. Keeps or removes views 4 and 5 (the reflection).
void DecideReflection(uint8_t* base) {
  namespace rd = nfsmw::reflection_demand;
  StateReflection& e = g_reflection;
  using namespace std::chrono;
  ReportReflection(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
  const uint32_t a4 = AddressActive(kViewReflection);
  const uint32_t a5 = AddressActive(kViewReflectionSecond);
  if (base[a4] == 0 && base[a5] == 0) {
    return;  // the game does not request it this frame
  }
  ++e.requested_2;
  if (e.phase == PhaseReflection::kWatching) {
    if (e.watching == 0) {
      e.copies_start = rd::g_copies.load(std::memory_order_relaxed);
    }
    ++e.drawn;
    if (++e.watching < kReflectionWatching) {
      return;
    }
    // The ring runs one frame behind: it is enough that 80 % of the requested frames were resolved there.
    const uint64_t copies = rd::g_copies.load(std::memory_order_relaxed) - e.copies_start;
    if (copies * 10 >= uint64_t(kReflectionWatching) * 8) {
      e.phase = PhaseReflection::kApplying;
      REXLOG_INFO("[clips] reflection low demand: comprobado ({} copies a {:08X} en {} frames con el reflection "
                  "requested); since here solo se dibuja si se ha read en los last {} frames, y si no, 1 de every {}",
                  copies, rd::kAddress, kReflectionWatching, kReflectionGrace,
                  std::max<int32_t>(1, REXCVAR_GET(nfsmw_reflection_refresh)));
    } else {
      e.phase = PhaseReflection::kOff;
      REXLOG_WARN("[clips] reflection low demand: OFF; en {} frames con el reflection requested solo hubo {} copies "
                  "a {:08X} (el reflection no se resuelve ahi): se dibuja always",
                  kReflectionWatching, copies, rd::kAddress);
    }
    return;
  }
  if (e.phase == PhaseReflection::kOff) {
    ++e.drawn;
    return;
  }
  const uint64_t swaps = rd::g_swaps.load(std::memory_order_relaxed);
  // Once the witnesses are verified, what counts is whether the water left samples on screen.
  const bool by_visibility = REXCVAR_GET(nfsmw_reflection_visibility) &&
                               rd::g_phase_visibility.load(std::memory_order_relaxed) == rd::kVisApplying;
  const uint64_t last_2 = by_visibility ? rd::g_last_visible.load(std::memory_order_relaxed)
                                          : rd::g_last_read.load(std::memory_order_relaxed);
  const bool read = last_2 != 0 && swaps + 1 - last_2 <= kReflectionGrace;
  const uint32_t refresh = uint32_t(std::max<int32_t>(1, REXCVAR_GET(nfsmw_reflection_refresh)));
  if (read || ++e.since_draw >= refresh) {
    e.since_draw = 0;
    ++e.drawn;
    return;
  }
  base[a4] = 0;
  base[a5] = 0;
}

}  // namespace
}  // namespace nfsmw::clips_race

REX_EXTERN(__imp__sub_8243FF30);
REX_HOOK_RAW(sub_8243FF30) {
  __imp__sub_8243FF30(ctx, base);
  using namespace nfsmw::clips_race;
  CountMirror(base);
  if (!InRace(base)) {
    return;
  }
  if (REXCVAR_GET(nfsmw_reflection_road)) {
    AdjustDetailReflection(base);
    if (REXCVAR_GET(nfsmw_reflection_low_demand)) {
      DecideReflection(base);
    }
    return;
  }
  base[AddressActive(kViewReflection)] = 0;
  base[AddressActive(kViewReflectionSecond)] = 0;
  if (!g_warning_reflection.exchange(true)) {
    REXLOG_INFO("[clips] race: reflection de la road off (views 4 y 5)");
  }
}

/*
 * See nfsmw_mirror_clip. The function writes the mask to r4+132 right before returning; here
 * the two bits are set again afterwards, which is the same as never having removed them.
 */
REX_EXTERN(__imp__sub_82216600);
REX_HOOK_RAW(sub_82216600) {
  const uint32_t pointer_view = ctx.r3.u32;
  const uint32_t target = ctx.r4.u32;
  __imp__sub_82216600(ctx, base);
  if (!REXCVAR_GET(nfsmw_mirror_clip) || target == 0) {
    return;
  }
  using namespace nfsmw::clips_race;
  if (Read32(base, pointer_view + 4) != 20) {  // +4 = view ID
    return;
  }
  const uint32_t before = Read32(base, target + 132);
  const uint32_t after = before | 0x40u | 0x08000000u;
  if (before != after) {
    Write32(base, target + 132, after);
    if (!g_warning_mirror.exchange(true)) {
      REXLOG_INFO("[clips] mirror: se le devuelven los dos bits de clip (mask 0x{:08X} -> 0x{:08X})",
                  before, after);
    }
  }
}

REX_EXTERN(__imp__sub_8243C6E0);
REX_HOOK_RAW(sub_8243C6E0) {
  const uint32_t table = ctx.r3.u32;
  __imp__sub_8243C6E0(ctx, base);
  using namespace nfsmw::clips_race;
  RegistrarTableFaces(base);
  if (!InRace(base)) {
    // Outside a race the first-draw state is forgotten, so the next race draws all six faces once again.
    // The cubemap contents belong to the scenery and are no good for another one.
    g_faces_first_used = 0;
    g_warning_first_use.store(false, std::memory_order_relaxed);
    g_warning_first_use_missing.store(false, std::memory_order_relaxed);
    return;
  }
  if (!g_warning_views_faces.exchange(true)) {
    REXLOG_INFO("[clips] faces del cubemap (table 0x{:08X}): views {} {} {} {} {} {}", table,
                NumberOfView(Read32(base, table)), NumberOfView(Read32(base, table + 4)),
                NumberOfView(Read32(base, table + 8)), NumberOfView(Read32(base, table + 12)),
                NumberOfView(Read32(base, table + 16)), NumberOfView(Read32(base, table + 20)));
  }
  // Which face is which. Separate from the notice above and with its own flag, because trying on the
  // first call (when the cubemap views did not have their parameters yet) gave all six with direction
  // 0,0,0. It now retries until it matches.
  if (!g_warning_faces_named.load(std::memory_order_relaxed)) {
    DiagnosticFacesOfCube(base, table);
  }
  const uint32_t always = uint32_t(REXCVAR_GET(nfsmw_cubemap_faces_always));
  /*
   * The object size threshold goes up here on purpose: it also applies with nfsmw_cubemap_faces_max = -1
   * (the PC value), which is where the effect can be measured.
   */
  for (uint32_t i = 0; i < kFaces; ++i) {
    const uint32_t vista = Read32(base, table + 4 * i);
    if (EsVista(vista)) {
      AdjustDetailCube(base, table, i, vista, always);
    }
  }
  const int32_t cycle_s = REXCVAR_GET(nfsmw_cubemap_diag_cycle_s);
  if (cycle_s > 0) {
    DiagnosticOneFace(base, table, cycle_s);
    return;
  }
  const int32_t maximum = REXCVAR_GET(nfsmw_cubemap_faces_max);
  if (maximum < 0) {
    return;
  }
  uint32_t active[kFaces];
  uint32_t n = 0;
  uint32_t fixed = 0;
  for (uint32_t i = 0; i < kFaces; ++i) {
    const uint32_t vista = Read32(base, table + 4 * i);
    if (vista == 0) {
      continue;
    }
    if (!EsVista(vista)) {
      if (!g_warning_pointer.exchange(true)) {
        REXLOG_WARN("[clips] face {} apunta a 0x{:08X}, outside de la table de views; no se due",
                    i, vista);
      }
      continue;
    }
    if (base[vista + kOffActive] == 0) {
      continue;
    }
    // The first draw is marked here, on the face about to be drawn, pinned faces included. The first
    // version marked it by walking active[], and pinned faces are not in active[] (they leave through
    // the continue below), so on the Switch (faces_always = 4, the mirror) bit 2 was never set, the
    // first-draw phase never ended and the limit was never applied: all six faces were drawn every frame
    // and the cubemap went from 6.0 to 10.0 ms real. It did not show on the PC because there faces_always
    // is 0 and no face is pinned.
    g_faces_first_used |= (1u << i);
    if (always & (1u << i)) {
      ++fixed;  // updated every frame
      continue;
    }
    active[n++] = vista;
  }
  /*
   * The first full round is never trimmed.
   *
   * With nfsmw_cubemap_faces_max = 0 the five non-pinned faces were disabled in every frame, so they were
   * never drawn even once from the start of the race. The cubemap kept whatever it held before (nothing)
   * and the car body, which samples it as an environment map, reflected black: the cars looked dark.
   *
   * A delayed reflection is a defensible performance trade-off. A black reflection is a bug. So whatever
   * the limit, every face is drawn at least once; until then nothing is touched. That is six race frames,
   * paid once.
   */
  if (g_faces_first_used != kAllTheFaces) {
    if (!g_warning_first_use_missing.exchange(true)) {
      REXLOG_INFO("[clips] race: estrenando el cubemap, missing faces (mask 0x{:X} de 0x{:X}); "
                  "until entonces no se recorta", g_faces_first_used, kAllTheFaces);
    }
    return;  // no face is turned off until all of them have been drawn once
  }
  if (!g_warning_first_use.exchange(true)) {
    REXLOG_INFO("[clips] race: the six faces del cubemap ya se han dibujado one time; "
                "a partir de here manda el limit");
  }
  if (n <= uint32_t(maximum)) {
    return;
  }
  const uint32_t start = g_rotation.fetch_add(1, std::memory_order_relaxed) % n;
  for (uint32_t k = 0; k < n; ++k) {
    if ((k + n - start) % n >= uint32_t(maximum)) {
      base[active[k] + kOffActive] = 0;
    }
  }
  if (!g_warning_faces.exchange(true)) {
    REXLOG_INFO("[clips] race: cubemap limitado a {} face(s) por frame mas {} fixed(s) "
                "(mask 0x{:X}; el game activaba {})",
                maximum, fixed, always, n + fixed);
  }
}
