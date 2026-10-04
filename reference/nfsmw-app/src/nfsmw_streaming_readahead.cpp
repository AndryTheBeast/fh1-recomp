// nfsmw - the streamer requests the zone too late
//
// What it fixes
//   Facade popping and the stutter at the two corners and the alley are the same event: the detailed
//   scenery section arrives late. Until it is there, the game draws the distant stand-ins of the 'Z'
//   sections (Scenery.cpp:927) and the detailed one pops in when
//   GetScenerySectionHeader(section) stops being nullptr. It is not mesh LOD: when an object has no
//   reduced model of its own, pModel[0] and pModel[2] are the same pointer (Scenery.cpp:401-421).
//
//   The lever is when the section is requested, and TrackStreamer::GetPredictedZone decides that: it
//   projects the player's position forward and the zone at that point is the one queued for loading
//   (TrackStreamer::DetermineCurrentZones -> DetermineStreamingSections).
//   The game looks 1.5 s ahead and never more than 100 m. At 25 m/s (the speed of a corner or an
//   alley) that is 37 meters. That is why it arrives late exactly where it is noticeable.
//
// Where it is in the recompiled binary   (see "The proof" below)
//   sub_824BE0A8 = TrackStreamer::GetPredictedZone(StreamingPositionEntry* r4)
//                  app/generated/default/nfsmw_recomp.99.cpp:18500
//   sub_824BEF30 = TrackStreamer::DetermineCurrentZones   nfsmw_recomp.129.cpp:17543  (only caller)
//   sub_824BE4E8 = TrackStreamer::DetermineStreamingSections  nfsmw_recomp.88.cpp:18083
//   sub_824BA290 = TrackPathManager::FindZone             nfsmw_recomp.37.cpp:18450
//   sub_824C5DA0 = VisibleSectionManager::FindDrivableSection  nfsmw_recomp.77.cpp:18030
//   sub_824B9690 = TrackStreamingBarrier::Intersects
//   Globals: TheTrackPathManager = 0x82C59DA0, TheVisibleSectionManager = 0x82C52C48
//   Game constants (floats, big endian, in guest memory):
//     0x82060E74 = 1.5f      seconds of lookahead          (lfs f29,-5724(r28), r28 = 0x820624D0)
//     0x82040200 = 100.0f    cap in meters                 (lfs f31,512(r10),  r10 = 0x82040000)
//     0x820B069C = 178.816f  MPH2MPS(400), no prediction above it  (lfs f27,1692(r11))
//     0x82061CE8 = 0.0f      (the same base nfsmw_scenery_lod.cpp already used)
//
// The proof that sub_824BE0A8 is GetPredictedZone
//   1. Anchor: DetermineStreamingSections calls GetScenerySectionNumber('Y'/'X'/'Z', 0), which is
//      inline and equals (letter-'A'+1)*100 -> 2500 / 2400 / 2600. Those three immediates in a row
//      appear in only one place in the 141 MB of generated code: sub_824BE4E8. And its first call
//      is to sub_824BE340, which is RemoveCurrentStreamingSections, as in the source.
//   2. From there, in address order, sub_824BE0A8 falls where the source puts GetPredictedZone
//      (TrackStreamer.cpp:1457) and its body matches instruction by instruction:
//        lfs f13,12(r31) / fmuls / lfs f0,16(r31) / fmadds / fsqrts f30   -> speed = bLength(Velocity)
//        bl 0x824ba290 with r5=6 and r6=0                                 -> FindZone(&Position, 6, zone)
//        lfs f0,20(r30) ; fcmpu vs 0.0 ; fabs                             -> zone->GetElevation()
//        fcmpu f30 vs f27 ; ble                                           -> speed > MPH2MPS(400)
//        fmuls f7,f30,f29 ; fcmpu f7,f31 ; ble                            -> (speed*1.5f) > 100.0f
//        fdivs f0,f31,f30 ; fmadds f2,f4,f0,f6                            -> pos + vel*(100.0f/speed)
//        fmadds f10,f12,f29,f0                                            -> pos + vel*1.5f
//        bl 0x824c5da0 ; loop of 4 over zone+48 ; loop over 16-byte barriers ; extsh r3
//   3. The caller does `addi r4,r31,-36` and first checks `lbz r11,-8(r31)`: r31-8 is the same
//      object +0x1C, which is exactly StreamingPositionEntry::PositionSet (TrackStreamer.hpp:88).
//
// Why the constants are not patched, which would be the obvious way
//   Because the 1.5f and the 100.0f live in the executable's shared constant tables. The base of the
//   100.0f (0x82040000) is loaded from 126 different places and offset 512 appears 108 times; the
//   1.5f's base is the same table nfsmw_scenery_lod uses. Changing those four bytes would touch
//   half the game. Ruled out with data, not out of caution.
//
// How it is done instead: the velocity is faked, at exactly the right moment
//   The key is that GetPredictedZone reads the velocity twice:
//     a) on entry, to compute `speed` (it stays in f30, which is callee-saved and is not recomputed);
//     b) further down, again from memory, to build the offset (lfs f4,12(r31)).
//   And between (a) and (b) there is always a call to FindZone (sub_824BA290), because the offset is
//   computed inside the loop and the loop starts there.
//
//   So: the real velocity is left in place on entry (so `speed` is the real one and both comparisons,
//   the 178.8 m/s one and the 100 m one, are decided as always) and the fake velocity is written
//   right after the first FindZone. Then:
//       if the game took the 1.5 s branch    ->  predict = pos + v_false * 1.5f
//       if it took the 100 m cap branch      ->  predict = pos + v_false * (100.0f / speed_real)
//   In both cases |v_false| can be chosen so the projected distance is exactly the desired one, with
//   no cap and without touching any shared constant. And since `speed` is still the real one, the
//   178.8 m/s (644 km/h) guard cannot trip by accident.
//
// Safety
//   - The velocity is always restored on exit, to the exact previous value. GetPredictedZone does
//     not write to the entry: it only reads. The window is microseconds long and on the same thread.
//   - If sub_824BA290 were not FindZone, or if the map had no prediction zones, the change is not
//     applied and the game behaves as always. The "applied" counter says so.
//   - Safety net: with the diagnostic on, it is also called without the lookahead. If the lookahead
//     overshoots and the prediction collapses (it returns 0, or stays in the current zone, which is
//     what the function returns when it cannot predict), the earlier zone is used and it is counted
//     as a rescue. It can never end up worse than the original game.
//   - Calling twice is safe: verified that both functions it uses only write to their own caches.
//     FindZone stores at TheTrackPathManager+12+76*type a box around the queried position, the hit
//     count and a query counter (and since the position is the same in both calls, the second one
//     comes from the cache and is almost free); on first reading it seemed to write to the caller's
//     r4, but r4 is reassigned first (addi r4,r31,20) and what it touches is the box, not the entry.
//     FindDrivableSection only moves a node to the front of its MRU list. Neither touches simulation
//     state.
//
// What is not touched
//   GetLoadingPriority (the queue priority) has not been located with certainty and is not touched:
//   it only orders what has already been requested. This function decides what gets requested.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

/*
 * Why 250 % and 160 m, and not x4 and 300 m
 *
 * At 25 m/s (corner, alley) the game looks 37.5 m ahead. With 250 % that is 94 m: a bit more than
 * twice the margin exactly where the popping is visible, and at that distance the facade is already
 * outside the detail the eye tracks. At 60 m/s (highway) the game was already capped at 100 m and
 * goes to 160, which is the ceiling.
 *
 * Going higher has a real, measured cost: each extra zone is megabytes read from the SD, and a
 * measured run shows the game rereads 79 % to 95 % of the bytes every lap. Requesting 10 seconds
 * ahead (300 m at 30 m/s) can trade one stutter for four. With 250/160 the number of live sections
 * rises only a little and the 360 retail pool (~195 MB, `lis r10,3125` in nfsmw_recomp.44.cpp:13766)
 * has plenty of room.
 *
 * Both values can be changed from the toml without rebuilding; 100 % is exactly the original game.
 */
REXCVAR_DEFINE_INT32(nfsmw_streaming_readahead, 250, "NFSMW",
                     "Percentage over la readahead del streamer. 100 = el game original "
                     "(1,5 s por ahead). 250 = looks 2,5 times mas far_depth. El popping de fachadas "
                     "y el hitch al enter en zone new_entry salen de here")
    .range(100, 600);

REXCVAR_DEFINE_INT32(nfsmw_streaming_ceiling_m, 160, "NFSMW",
                     "Meters maximos que se le leaves look por ahead al streamer. 0 = el cap del "
                     "game (100 m). Ojo: every metro de mas son sections de mas que read de la SD")
    .range(0, 400);

REXCVAR_DEFINE_BOOL(nfsmw_streaming_diag, true, "NFSMW",
                    "Llama a la prediccion tambien SIN lead para poder compare zone a zone y "
                    "para rescatar el result si el lead se pasa de is_long. Cuesta dos "
                    "calls por service del streamer (2 por frame as mucho)");

namespace nfsmw::streaming_readahead {
namespace {

// Guest addresses. See the header comment for how they were found.
constexpr uint32_t kDirSeconds = 0x82060E74;  // float 1.5f
constexpr uint32_t kDirCeiling = 0x82040200;     // float 100.0f
constexpr uint32_t kDirVelMax = 0x820B069C;    // float 178.816f = MPH2MPS(400)

// StreamingPositionEntry (TrackStreamer.hpp:82). Confirmed in the disassembly.
constexpr uint32_t kOffVelX = 12;        // bVector2 Velocity
constexpr uint32_t kOffVelY = 16;
constexpr uint32_t kOffZoneCurrent = 36;  // int16 CurrentZone (0x24)

constexpr uint32_t kAddressMaxima = 0xE0000000u;  // same as in nfsmw_shadows_lod.cpp
constexpr uint32_t kEveryHow_manyLines = 1024;       // ~ one line every 8-17 s

// Sanity check on what is read from game memory: if it does not look like 1.5 s / 100 m, the
// addresses are not what we think and nothing at all must be done.
constexpr float kSecondsMin = 0.1f, kSecondsMax = 10.0f;
constexpr float kCeilingMin = 10.0f, kCeilingMax = 2000.0f;
constexpr float kLengthFalseMax = 1000.0f;  // m/s; only a vector, not a real speed

uint32_t Read32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}

int32_t Read16With(const uint8_t* base, uint32_t dir) {
  uint16_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return int16_t(__builtin_bswap16(v));
}

float ReadFloat(const uint8_t* base, uint32_t dir) {
  const uint32_t v = Read32(base, dir);
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

void WriteFloat(uint8_t* base, uint32_t dir, float f) {
  uint32_t v = 0;
  std::memcpy(&v, &f, sizeof(v));
  v = __builtin_bswap32(v);
  std::memcpy(base + dir, &v, sizeof(v));
}

// Handoff state between the two hooks. Per thread on purpose: the streamer runs on the game thread,
// but half the world calls FindZone and there the hook must be a no-op.
thread_local uint32_t t_entry = 0;   // the StreamingPositionEntry being predicted
thread_local float t_vel_x = 0.0f;     // fake velocity to leave in place
thread_local float t_vel_y = 0.0f;
thread_local bool t_armed = false;    // true only inside the lookahead call
thread_local bool t_applied = false;  // set by the FindZone hook when it actually writes

std::atomic<bool> g_presented{false};    // the "this is what the game does" line
std::atomic<bool> g_constants_mal{false};

// Counters. All relaxed: they are not compared with each other, only read when printing.
std::atomic<uint64_t> g_calls{0};   // times the hook was entered
std::atomic<uint64_t> g_acted{0};   // of those, the ones where a farther lookahead was requested
std::atomic<uint64_t> g_applied{0};  // of those, the ones where the handoff actually wrote
std::atomic<uint64_t> g_different{0};  // times the requested zone changes due to the lookahead
std::atomic<uint64_t> g_rescues{0};   // times the lookahead overshot and the earlier zone was returned
std::atomic<uint64_t> g_without_handoff{0};  // armed but FindZone was never called with the entry
// Sums for the "before" and "after" averages, in meters.
std::atomic<uint64_t> g_sum_before_mm{0};
std::atomic<uint64_t> g_sum_after_mm{0};

void Note(const uint8_t* base, int32_t mult_pct, int32_t ceiling_cvar) {
  if (g_presented.exchange(true)) return;
  REXLOG_INFO(
      "[stream] el game looks {:.2f} s por ahead y never mas de {:.1f} m (y leaves de predict por "
      "encima de {:.1f} m/s); con readahead={} % y ceiling={} m se le asks look mas far_depth",
      double(ReadFloat(base, kDirSeconds)), double(ReadFloat(base, kDirCeiling)),
      double(ReadFloat(base, kDirVelMax)), mult_pct,
      ceiling_cvar > 0 ? ceiling_cvar : int32_t(ReadFloat(base, kDirCeiling)));
}

void MaybePrint() {
  const uint64_t n = g_calls.load(std::memory_order_relaxed);
  if (n == 0 || (n % kEveryHow_manyLines) != 0) return;
  const uint64_t act = g_acted.load(std::memory_order_relaxed);
  const uint64_t apl = g_applied.load(std::memory_order_relaxed);
  const double before = act ? double(g_sum_before_mm.load(std::memory_order_relaxed)) / act / 1000.0 : 0.0;
  const double desp = act ? double(g_sum_after_mm.load(std::memory_order_relaxed)) / act / 1000.0 : 0.0;
  REXLOG_INFO(
      "[stream] C8 readahead: {} calls, {} con lead, {} applied ({} sin handoff); "
      "looks {:.1f} m -> {:.1f} m de media; {} times asks one zone different; {} rescues",
      n, act, apl, g_without_handoff.load(std::memory_order_relaxed), before, desp,
      g_different.load(std::memory_order_relaxed), g_rescues.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace nfsmw::streaming_readahead

// =================================================================================================
// TrackPathManager::FindZone(bVector2* pos, int type, TrackPathZone* prev)
//
// The handoff. It changes nothing by itself: it only writes the fake velocity when inside
// GetPredictedZone (t_armed) and the call is on the same entry being predicted (GetPredictedZone
// calls it with r4 = &entry->Position, which is the entry + 0). Otherwise it is two comparisons and
// nothing else.
//
// r4 is read before calling the original on purpose: r4 is volatile and the original clobbers it.
// =================================================================================================
REX_EXTERN(__imp__sub_824BA290);
REX_HOOK_RAW(sub_824BA290) {
  using namespace nfsmw::streaming_readahead;
  const uint32_t arg = ctx.r4.u32;
  __imp__sub_824BA290(ctx, base);
  if (!t_armed || arg == 0 || arg != t_entry) {
    return;
  }
  WriteFloat(base, t_entry + kOffVelX, t_vel_x);
  WriteFloat(base, t_entry + kOffVelY, t_vel_y);
  t_applied = true;
}

// =================================================================================================
// TrackStreamer::GetPredictedZone(StreamingPositionEntry* r4) -> short
// Returns the section number of the zone the streamer is going to queue for loading.
// =================================================================================================
REX_EXTERN(__imp__sub_824BE0A8);
REX_HOOK_RAW(sub_824BE0A8) {
  using namespace nfsmw::streaming_readahead;

  g_calls.fetch_add(1, std::memory_order_relaxed);
  const int32_t mult_pct = REXCVAR_GET(nfsmw_streaming_readahead);
  const int32_t ceiling_cvar = REXCVAR_GET(nfsmw_streaming_ceiling_m);
  const uint32_t entry = ctx.r4.u32;

  if (mult_pct <= 100 || g_constants_mal.load(std::memory_order_relaxed) || entry == 0 ||
      entry + kOffZoneCurrent + 2u >= kAddressMaxima) {
    __imp__sub_824BE0A8(ctx, base);
    MaybePrint();
    return;
  }

  // The game's own constants. If they are not what we expect, the addresses are wrong and this turns
  // off for good: better to do nothing than to move something we do not understand.
  const float k_seg = ReadFloat(base, kDirSeconds);
  const float k_ceiling = ReadFloat(base, kDirCeiling);
  if (!(k_seg >= kSecondsMin && k_seg <= kSecondsMax) ||
      !(k_ceiling >= kCeilingMin && k_ceiling <= kCeilingMax)) {
    if (!g_constants_mal.exchange(true)) {
      REXLOG_WARN("[stream] the constants del streamer no cuadran (seconds={} cap={}): "
                  "readahead DISABLED, el game se queda as was_writable",
                  double(k_seg), double(k_ceiling));
    }
    __imp__sub_824BE0A8(ctx, base);
    MaybePrint();
    return;
  }
  Note(base, mult_pct, ceiling_cvar);

  const float vx = ReadFloat(base, entry + kOffVelX);
  const float vy = ReadFloat(base, entry + kOffVelY);
  const float s = std::sqrt(vx * vx + vy * vy);
  // Stopped or nearly so: there is no direction to look ahead in and the game does not predict either.
  if (!std::isfinite(s) || s <= 1.0f) {
    __imp__sub_824BE0A8(ctx, base);
    MaybePrint();
    return;
  }

  const float ceiling = ceiling_cvar > 0 ? float(ceiling_cvar) : k_ceiling;
  const float before = std::min(k_seg * s, k_ceiling);                          // what the game looks at
  const float after = std::min(k_seg * s * (float(mult_pct) / 100.0f), ceiling);  // what we want
  if (!(after > before + 0.5f)) {
    __imp__sub_824BE0A8(ctx, base);
    MaybePrint();
    return;
  }

  /*
   * The length of the fake vector. The game picks the branch with the real speed (f30), which is
   * already computed before anything is written:
   *     cap branch     (s*k_seg  > k_ceiling):  predict = pos + v_false * (k_ceiling / s)
   *     normal branch  (s*k_seg <= k_ceiling):  predict = pos + v_false * k_seg
   * Solved so that the projected distance is exactly "after".
   */
  float is_long = (k_seg * s > k_ceiling) ? (after * s / k_ceiling) : (after / k_seg);
  if (!std::isfinite(is_long) || is_long <= 0.0f) {
    __imp__sub_824BE0A8(ctx, base);
    MaybePrint();
    return;
  }
  is_long = std::min(is_long, kLengthFalseMax);

  t_entry = entry;
  t_vel_x = vx / s * is_long;
  t_vel_y = vy / s * is_long;
  t_applied = false;

  // Reference without lookahead: it measures the real "before" value and serves the rescue. The
  // function only reads the entry, so calling it twice has no side effects.
  const bool diag = REXCVAR_GET(nfsmw_streaming_diag);
  int32_t zone_base = 0;
  if (diag) {
    // All 64 bits are saved, not just the low half: in the recompiled code `mr` copies the whole
    // register, and leaving the high half inconsistent is the kind of bug nobody sees.
    const uint64_t r3_orig = ctx.r3.u64;
    const uint64_t r4_orig = ctx.r4.u64;
    const uint64_t lr_orig = ctx.lr;
    t_armed = false;  // keep the handoff from touching anything in the reference call
    __imp__sub_824BE0A8(ctx, base);
    zone_base = int32_t(int16_t(uint16_t(ctx.r3.u32)));
    ctx.r3.u64 = r3_orig;
    ctx.r4.u64 = r4_orig;
    ctx.lr = lr_orig;
  }

  t_armed = true;
  __imp__sub_824BE0A8(ctx, base);
  t_armed = false;

  // Always restore, whether it was applied or not: writing back the same value costs nothing.
  WriteFloat(base, entry + kOffVelX, vx);
  WriteFloat(base, entry + kOffVelY, vy);
  t_entry = 0;

  g_acted.fetch_add(1, std::memory_order_relaxed);
  g_sum_before_mm.fetch_add(uint64_t(before * 1000.0f), std::memory_order_relaxed);
  g_sum_after_mm.fetch_add(uint64_t(after * 1000.0f), std::memory_order_relaxed);
  if (t_applied) {
    g_applied.fetch_add(1, std::memory_order_relaxed);
  } else {
    // The handoff did not engage: FindZone was never called with this entry, so the velocity was
    // never changed and the result is the game's own. If this number resembles the "con lead"
    // one, the hook is not doing anything and sub_824BA290 needs checking.
    g_without_handoff.fetch_add(1, std::memory_order_relaxed);
  }

  if (diag) {
    const int32_t zone_ade = int32_t(int16_t(uint16_t(ctx.r3.u32)));
    if (zone_ade != zone_base) {
      g_different.fetch_add(1, std::memory_order_relaxed);
      /*
       * Rescue. If the projected point falls outside the zones the map has linked,
       * predict_position_used stays false and the function returns FindDrivableSection of the
       * current position, that is: zero lookahead, worse than the game. It is detected because the
       * lookahead result is 0 or the zone we are already in, while the game's was a different one.
       * In that case the game's result is returned, so this cannot make anything worse.
       */
      const int32_t zone_current = Read16With(base, entry + kOffZoneCurrent);
      if (zone_base != 0 && (zone_ade == 0 || zone_ade == zone_current)) {
        ctx.r3.s64 = int64_t(int16_t(zone_base));
        g_rescues.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  MaybePrint();
}

namespace nfsmw::streaming_readahead {
// Available for the native renderer's periodic report, next to nfsmw::scenery_lod::Summary()
// (nfsmw_native_system.cpp:2793). It is not needed there: the hook already prints its own
// "[stream] C8" line every 1,024 calls.
std::string Summary() {
  const uint64_t n = g_calls.load(std::memory_order_relaxed);
  const uint64_t act = g_acted.load(std::memory_order_relaxed);
  return "readahead: " + std::to_string(n) + " predicciones, " + std::to_string(act) +
         " con lead, " + std::to_string(g_applied.load(std::memory_order_relaxed)) +
         " applied, " + std::to_string(g_different.load(std::memory_order_relaxed)) +
         " zones different_2";
}
}  // namespace nfsmw::streaming_readahead
