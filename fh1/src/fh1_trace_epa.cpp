// fh1 - probe on the physics penetration solver (car-collision investigation, ROADMAP).
//
// sub_82D440E0 runs a GJK distance query (sub_82D41D48) and then, for overlapping shapes, the
// EPA penetration solver sub_82D43E40(r3 = solver state, r4/r5 = where to store the two witness
// points). It returns 0 on success, 1 when the polytope has no faces left. The caller then
// normalizes (point B - point A) into the contact normal: identical points give a NaN normal
// (the --fh1_nan_trap hit #27 in sub_82D440E0). With --fh1_trace_epa this counts calls, results
// and identical witness points every 10 s and logs the first calls in full. Read-only.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fh1_trace_epa, false, "FH1",
                    "Debug: count the EPA penetration solver's results (sub_82D43E40) and "
                    "witness points that coincide");

namespace {

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

float BeF(const uint8_t* base, uint32_t address) {
  uint32_t w = Be32(base, address);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

std::atomic<uint64_t> g_calls{0}, g_ok{0}, g_fail{0}, g_same{0}, g_nan{0}, g_logged{0};
std::atomic<int64_t> g_last_report{0};

void Report() {
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = g_last_report.load();
  if (now - last < 10 || !g_last_report.compare_exchange_strong(last, now)) return;
  REXLOG_INFO("[epa] calls {} ok {} failed {} identical-points {} nan-points {}", g_calls.load(),
              g_ok.load(), g_fail.load(), g_same.load(), g_nan.load());
}

}  // namespace

// Narrow-phase dispatcher sub_82D42638(r3 = pair list): [r3+20] = pair count, [r3+4] = array of
// pair pointers. Per pair: +48/+52 = the two shapes (type byte at +0, < 6), +64/+68 = the two
// bodies, +76 = contacts the handler produced (table 832AF138[typeA * 6 + typeB]). Counted per
// type combination: pairs tested and pairs with contacts.
namespace {
std::atomic<uint64_t> g_pairs[6][6], g_hits[6][6], g_dispatch{0};
std::atomic<int64_t> g_last_pairs{0};
}  // namespace

REX_EXTERN(__imp__sub_82D42638);
REX_HOOK_RAW(sub_82D42638) {
  if (!REXCVAR_GET(fh1_trace_epa)) {
    __imp__sub_82D42638(ctx, base);
    return;
  }
  uint32_t list = ctx.r3.u32;
  int32_t count = int32_t(Be32(base, list + 20));
  uint32_t array = Be32(base, list + 4);
  __imp__sub_82D42638(ctx, base);
  g_dispatch++;
  for (int32_t k = 0; k < count && k < 4096; ++k) {
    uint32_t pair = Be32(base, array + 4 * k);
    uint32_t sa = Be32(base, pair + 48), sb = Be32(base, pair + 52);
    if (!sa || !sb) continue;
    uint8_t ta = base[sa], tb = base[sb];
    if (ta >= 6 || tb >= 6) continue;
    g_pairs[ta][tb]++;
    if (int32_t(Be32(base, pair + 76)) > 0) g_hits[ta][tb]++;
  }
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = g_last_pairs.load();
  if (now - last >= 10 && g_last_pairs.compare_exchange_strong(last, now)) {
    std::string s;
    for (int a = 0; a < 6; ++a)
      for (int b = 0; b < 6; ++b)
        if (g_pairs[a][b]) s += fmt::format(" {}x{}: {} tested {} with contacts;", a, b,
                                            g_pairs[a][b].load(), g_hits[a][b].load());
    REXLOG_INFO("[pairs] dispatches {}{}", g_dispatch.load(), s);
  }
}

// GJK overlap test sub_82D43290(r3 = state, r4/r5 = witness points, r6): returns 1 = overlap
// (the simplex solver reported 2), 0 = apart. [state+144] = iterations. Each iteration calls
// the progress test sub_82D420F0(r3 = state, v1 = 0, v2 = w = support point, v3 = direction,
// v4 = previous) - nonzero = stop. Counted: results, iterations, stops; first calls in full.
namespace {
std::atomic<uint64_t> g_gjk{0}, g_gjk_overlap{0}, g_gjk_iters{0}, g_prog{0}, g_prog_stop{0},
    g_prog_logged{0}, g_gjk_logged{0};
std::atomic<int64_t> g_last_gjk{0};

std::string Vec(const PPCVRegister& v) {
  // Host layout is reversed: f32[3] = guest x.
  return fmt::format("({:.4g} {:.4g} {:.4g} {:.4g})", v.f32[3], v.f32[2], v.f32[1], v.f32[0]);
}
}  // namespace

REX_EXTERN(__imp__sub_82D420F0);
REX_HOOK_RAW(sub_82D420F0) {
  if (!REXCVAR_GET(fh1_trace_epa)) {
    __imp__sub_82D420F0(ctx, base);
    return;
  }
  uint32_t simplex = Be32(base, ctx.r3.u32 + 156);
  std::string in = fmt::format(
      "w {} dir {} prev {} | best-so-far {} simplex {:08X} v ({} {} {} {}) count? {:08X} {:08X}",
      Vec(ctx.v2), Vec(ctx.v3), Vec(ctx.v4), BeF(base, ctx.r3.u32 + 128), simplex,
      BeF(base, simplex + 176), BeF(base, simplex + 180), BeF(base, simplex + 184),
      BeF(base, simplex + 188), Be32(base, simplex + 0), Be32(base, simplex + 4));
  __imp__sub_82D420F0(ctx, base);
  g_prog++;
  bool stop = (ctx.r3.u32 & 0xFF) != 0;
  if (stop) g_prog_stop++;
  if (g_prog_logged.fetch_add(1) < 40) {
    REXLOG_INFO("[gjk] progress test {} -> {}", in, stop ? "STOP" : "go on");
  }
}

// Duplicate-point check inside the progress test: sub_82D0F5D8(r3 = simplex, v1 = w).
namespace {
std::atomic<uint64_t> g_dup_logged{0};
}
REX_EXTERN(__imp__sub_82D0F5D8);
REX_HOOK_RAW(sub_82D0F5D8) {
  if (!REXCVAR_GET(fh1_trace_epa) || g_dup_logged.load() >= 40) {
    __imp__sub_82D0F5D8(ctx, base);
    return;
  }
  uint32_t simplex = ctx.r3.u32;
  std::string w = Vec(ctx.v1);
  __imp__sub_82D0F5D8(ctx, base);
  g_dup_logged++;
  REXLOG_INFO("[gjk] duplicate check simplex {:08X} w {} -> {} (lr {:08X})", simplex, w,
              ctx.r3.u32 & 0xFF, uint32_t(ctx.lr));
}

REX_EXTERN(__imp__sub_82D43290);
REX_HOOK_RAW(sub_82D43290) {
  if (!REXCVAR_GET(fh1_trace_epa)) {
    __imp__sub_82D43290(ctx, base);
    return;
  }
  uint32_t state = ctx.r3.u32, pa = ctx.r4.u32, pb = ctx.r5.u32;
  __imp__sub_82D43290(ctx, base);
  uint32_t result = ctx.r3.u32;
  uint32_t iters = Be32(base, state + 144);
  g_gjk++;
  if (result) g_gjk_overlap++;
  g_gjk_iters += iters;
  if (g_gjk_logged.fetch_add(1) < 40) {
    REXLOG_INFO("[gjk] result {} iterations {} A ({} {} {}) B ({} {} {})", result, iters,
                BeF(base, pa), BeF(base, pa + 4), BeF(base, pa + 8), BeF(base, pb),
                BeF(base, pb + 4), BeF(base, pb + 8));
  }
  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = g_last_gjk.load();
  if (now - last >= 10 && g_last_gjk.compare_exchange_strong(last, now)) {
    REXLOG_INFO("[gjk] tests {} overlap {} avg iterations {:.2f}; progress tests {} stops {}",
                g_gjk.load(), g_gjk_overlap.load(), double(g_gjk_iters.load()) / g_gjk.load(),
                g_prog.load(), g_prog_stop.load());
  }
}

REX_EXTERN(__imp__sub_82D43E40);
REX_HOOK_RAW(sub_82D43E40) {
  if (!REXCVAR_GET(fh1_trace_epa)) {
    __imp__sub_82D43E40(ctx, base);
    return;
  }
  uint32_t state = ctx.r3.u32, pa = ctx.r4.u32, pb = ctx.r5.u32;
  __imp__sub_82D43E40(ctx, base);
  uint32_t result = ctx.r3.u32;
  g_calls++;
  (result == 0 ? g_ok : g_fail)++;
  float a[4], b[4];
  for (int i = 0; i < 4; ++i) {
    a[i] = BeF(base, pa + 4 * i);
    b[i] = BeF(base, pb + 4 * i);
  }
  bool same = a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
  bool nan = std::isnan(a[0]) || std::isnan(a[1]) || std::isnan(a[2]) || std::isnan(b[0]) ||
             std::isnan(b[1]) || std::isnan(b[2]);
  if (result == 0 && same) g_same++;
  if (nan) g_nan++;
  if (g_logged.fetch_add(1) < 25 || ((same || nan) && g_logged.load() < 200)) {
    REXLOG_INFO("[epa] result {} iterations {} verts {} A ({} {} {} {}) B ({} {} {} {}) lr {:08X}",
                result, Be32(base, state + 400), Be32(base, state + 7900), a[0], a[1], a[2], a[3],
                b[0], b[1], b[2], b[3], uint32_t(ctx.lr));
  }
  Report();
}
