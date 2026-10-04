// nfsmw - the dump of the changed registers of the game's D3D, in native code.
//
// WHAT sub_825A2AA0 IS
//   An Xbox 360 D3D function that the game calls from FlushState on every draw (~2,400 per
//   frame): it walks a 64-bit mask of registers marked as changed and, for each run of
//   consecutive bits, writes a type-0 PM4 packet into the command ring (header + the values
//   of those registers). It produces a good share of the ~83,000 packets per frame measured
//   on the console.
//
// WHY NATIVE
//   Recompiled, each copied word is a byte-reversed load, a byte-reversed store (both
//   through volatile pointers) and the PowerPC registers reread and rewritten in the
//   context: ~15-20 instructions per word. A reversed load followed by a reversed store is
//   a plain copy, so here it is a memcpy.
//
// WHY IT IS BIT-IDENTICAL
//   Only integers are involved. The original PowerPC (read instruction by instruction):
//
//     r29 = r6 - 4; r4 = [r3+0]
//     loop:   z = cntlzd(mask); r9 = [r3+4]; r28 = z + r5; r29 += z*4; mask <<= z
//             n = cntlzd(~mask)                                 // consecutive 1 bits, 1..64
//             if (r4 + n*4 < r9, unsigned):                     // fits in the ring
//                 [r4+4] = ((n-1) << 16) | r28; r4 += 4          // header
//                 n times: r29 += 4; r4 += 4; [r4] = [r29]       // word copy
//                 mask <<= n (bitwise: 64 shifts leave 0)
//             else: r4 = sub_825A29E8(r3, r4, r28, r29, n, 1); r29 += n*4; mask <<= n
//             r5 = r28 + n
//             repeat while mask != 0
//     [r3+0] = r4
//
//   Note: the loop runs once even if the mask arrives as 0, and in that case it would copy
//   ~2^32 words. The callers never do that, but to be exact in every case, with mask 0 the
//   original is called. The ring-full path is also left to the game (sub_825A29E8), with the
//   same registers and the same stack frame.
//
// HOW TO CHECK IT
//   The "[d3d_registers]" log line (every 10 s): calls, copied words and how many times the
//   game's path was taken. If something looked wrong, nfsmw_d3d_registers_native = false
//   restores the original.
//
// At the end of the file: FlushState with a single marker packet (phase 2 of the Direct3D-level renderer,
// cvar nfsmw_d3d_marker) and the counter for the ring-full path (sub_825A29E8).

#include "nfsmw_native_hooks.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports
#include "nfsmw_hitch_waits.h"     // TreeCull in the [hitch] game log line
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <type_traits>  // std::conditional_t in SetTextureNative

REXCVAR_DEFINE_BOOL(nfsmw_d3d_registers_native, true, "NFSMW",
                    "DumpEntry de register_values cambiados del D3D del game (sub_825A2AA0) en native: one copy "
                    "en time de ~15-20 instructions por word. Result identico bit a bit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

// Same as REX_PHYS_HOST_OFFSET in nfsmw_pch.h: on Win32 physical addresses are 0x1000 higher.
inline uint32_t Offset(uint32_t address) {
#if REX_PLATFORM_WIN32
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}
inline uint8_t* Pointer(uint8_t* base, uint32_t address) {
  return base + address + Offset(address);
}
inline uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Pointer(base, address), 4);
  return __builtin_bswap32(v);
}
inline void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  input_value = __builtin_bswap32(input_value);
  std::memcpy(Pointer(base, address), &input_value, 4);
}

// Copies n 32-bit words as they are (no swapping: swapping on read and on write cancels out). If the
// run crossed the 0xE0000000 boundary (where the offset changes on Win32), word by word.
inline void CopyWords(uint8_t* base, uint32_t target, uint32_t source, uint32_t n) {
  const uint32_t bytes = n * 4;
  const bool crosses = Offset(target) != Offset(target + bytes - 1) ||
                     Offset(source) != Offset(source + bytes - 1);
  if (!crosses) {
    std::memmove(Pointer(base, target), Pointer(base, source), bytes);
    return;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, Pointer(base, source + i * 4), 4);
    std::memcpy(Pointer(base, target + i * 4), &v, 4);
  }
}

// Report counters. Written by the game thread that draws; no read-modify-write atomics (on the A57
// each fetch_add is an ldxr/stxr loop).
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_words{0};
std::atomic<uint64_t> g_slow{0};
std::atomic<int64_t> g_next_ms{0};

template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

void Report() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  const int64_t next = g_next_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_next_ms.store(now + 10000, std::memory_order_relaxed);
  if (next == 0) {
    REXLOG_INFO("[d3d_registers] dump de register_values del D3D en native (sub_825A2AA0)");
    return;
  }
  const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
  const uint64_t words = g_words.exchange(0, std::memory_order_relaxed);
  NFSMW_REPORT_DEFERRED("[d3d_registers] last 10 s: {} calls, {} words copied ({:.1f} por call), {} por el "
              "path del game (ring full o mask 0)",
              calls, words, calls ? double(words) / double(calls) : 0.0,
              g_slow.exchange(0, std::memory_order_relaxed));
}

}  // namespace

REX_EXTERN(__imp__sub_825A2AA0);
REX_EXTERN(sub_825A29E8);

REX_HOOK_RAW(sub_825A2AA0) {
  static const bool active = REXCVAR_GET(nfsmw_d3d_registers_native);
  uint64_t mask = ctx.r4.u64;
  // Phase 1 of the Direct3D-level renderer. Register base_register + i comes from r6 + 4 * i.
  nfsmw::native::LearnGroupMirror(ctx.r5.u32, mask, ctx.r6.u32 - ctx.r3.u32);
  if (!active || mask == 0) {
    if (active) {
      Add(g_slow, uint64_t(1));
    }
    __imp__sub_825A2AA0(ctx, base);
    return;
  }

  const uint32_t vulkan_device = ctx.r3.u32;
  uint32_t reg_entry = ctx.r5.u32;      // r5
  uint32_t source = ctx.r6.u32 - 4;    // r29
  uint32_t write = Read32(base, vulkan_device + 0);  // r4
  uint64_t copied = 0;
  uint32_t r3_final = vulkan_device;  // the original leaves in r3 what the last slow call returned

  do {
    const uint32_t z = uint32_t(__builtin_clzll(mask));  // mask != 0 here
    const uint32_t fin = Read32(base, vulkan_device + 4);      // r9, reread on every iteration
    const uint32_t r = z + reg_entry;                          // r28
    source += z * 4;
    mask <<= z;                                            // z <= 63
    const uint64_t inverted = ~mask;
    const uint32_t n = inverted ? uint32_t(__builtin_clzll(inverted)) : 64u;  // r30, 1..64
    if (uint32_t(write + n * 4) < fin) {
      Write32(base, write + 4, ((n - 1) << 16) | r);
      CopyWords(base, write + 8, source + 4, n);
      write += 4 + n * 4;
      copied += n;
    } else {
      // Ring full: the game handles it, like the original (r3..r8 and its 144-byte frame).
      Add(g_slow, uint64_t(1));
      const uint32_t stack = ctx.r1.u32;
      ctx.r1.u64 = stack - 144;
      Write32(base, stack - 144, stack);  // stwu r1,-144(r1): chain de frames
      ctx.r3.u64 = vulkan_device;
      ctx.r4.u64 = write;
      ctx.r5.u64 = r;
      ctx.r6.u64 = source;
      ctx.r7.u64 = n;
      ctx.r8.u64 = 1;
      ctx.lr = 0x825A2B0C;  // the one of "bl 0x825a29e8" at 0x825A2B08
      sub_825A29E8(ctx, base);
      ctx.r1.u64 = stack;
      write = ctx.r3.u32;
      r3_final = ctx.r3.u32;
    }
    source += n * 4;
    mask = n >= 64 ? 0 : (mask << n);
    reg_entry = r + n;
  } while (mask != 0);

  Write32(base, vulkan_device + 0, write);
  ctx.r3.u64 = r3_final;
  ctx.r4.u64 = write;
  ctx.r5.u64 = reg_entry;

  Add(g_calls, uint64_t(1));
  Add(g_words, copied);
  if ((g_calls.load(std::memory_order_relaxed) & 1023) == 0) {
    Report();
  }
}

// ---------------------------------------------------------------------------------------------------
// Measurement only.
//
// Other candidates for a native rewrite involve floating point (GetVisibleState: the box against the
// 6 view planes), many branches (TreeCull and DrawAScenery of the scenery) or a lot of code (the effect
// parameter upload, sub_826992F0). Rewriting them without knowing their cost would be a gamble, and a
// rounding error in culling shows up as popping. These hooks change nothing (they call the original):
// they count the calls and time 1 in every 16 (inclusive time: TreeCull includes DrawAScenery).
// "[measurement]" log line every 10 s: calls/s and estimated ms per second.
// The counters are not read-modify-write atomics (A57 without LSE); if two threads count at the same
// time a count can be lost, which does not matter for this purpose.
// ---------------------------------------------------------------------------------------------------
namespace {
struct Measurement {
  const char* name;
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> sample_total{0};
  std::atomic<uint64_t> ns{0};
};
Measurement g_m_visible{"GetVisibleState (8243E7D8)"};
Measurement g_m_draw{"DrawAScenery (824C2850)"};
Measurement g_m_tree{"TreeCull (824C2F48)"};
Measurement g_m_effect{"parametros de effect (826992F0)"};
Measurement* const kMeasurements[] = {&g_m_visible, &g_m_draw, &g_m_tree, &g_m_effect};
std::atomic<int64_t> g_next_measurement_ms{0};

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void ReportMeasurement() {
  const int64_t now = NowNs() / 1000000;
  const int64_t next = g_next_measurement_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_next_measurement_ms.store(now + 10000, std::memory_order_relaxed);
  if (next == 0) {
    return;
  }
  std::string line;
  for (Measurement* m : kMeasurements) {
    const uint64_t n = m->calls.exchange(0, std::memory_order_relaxed);
    const uint64_t k = m->sample_total.exchange(0, std::memory_order_relaxed);
    const uint64_t ns = m->ns.exchange(0, std::memory_order_relaxed);
    const double ms_s = k ? double(ns) / double(k) * double(n) / 1e6 / 10.0 : 0.0;
    line += fmt::format(" | {}: {:.0f} calls/s, {:.2f} ms/s", m->name, double(n) / 10.0, ms_s);
  }
  NFSMW_REPORT_DEFERRED("[measurement] last 10 s{}", line);
}

template <typename F>
inline void Measure(Measurement& m, F&& call) {
  const uint64_t n = m.calls.load(std::memory_order_relaxed) + 1;
  m.calls.store(n, std::memory_order_relaxed);
  if ((n & 15) != 0) {
    call();
    return;
  }
  const int64_t t0 = NowNs();
  call();
  Add(m.ns, uint64_t(NowNs() - t0));
  Add(m.sample_total, uint64_t(1));
}
}  // namespace

// ---------------------------------------------------------------------------------------------------
// eViewPlatInterface::GetVisibleState (sub_8243E7D8) in native code, bit-identical.
//
// WHAT IT DOES (PowerPC read instruction by instruction: nfsmw_recomp.5.cpp and, for the helper, .87.cpp)
//   Input: r3 = view (6 planes of 16 bytes at [r3] + 192), r4 and r5 = minimum and maximum corners of the
//   box (3 floats each), r6 = 4x4 matrix or 0. With a matrix, its helper (8243_E740) transforms the box: for
//   each axis, row * min and row * max, the min() and the max() of the two, added to row 3. Then: center
//   c = (min + max) * 0.5 with w = 1, half-extent e = max - c and, for each plane P, d = c . P (4 components)
//   and r = e . |P| (3). "outside" (outside) = min(0.5, d0 + r0, ..., d5 + r5) and "inside" (inside) =
//   min(0.5, d0 - r0, ..., d5 - r5), lane by lane. r3 = 0 if some lane of "outside" is not >= 0 (NaN included),
//   2 if all lanes of "inside" are, and 1 otherwise.
//
// WHY NATIVE
//   Measured ([measurement] log line): ~82,000 calls/s at 0.53 us, about 44 ms/s of the game thread. Recompiled,
//   each vector instruction reads and writes its register in the context, the box goes through the stack
//   and through f0-f13 in double precision, and each call writes the FPCR twice (disableFlushMode for the lfs
//   and enableFlushMode for the vector part). Here everything stays in registers and the FPCR is written at
//   most once.
//
// WHY IT IS BIT-IDENTICAL
//   - The same simde and rex::ppc functions as the generated code, with the operands in the same order (with
//     a NaN, simde_mm_min_ps returns the second one). The constants (0.5, the xyz mask, the 1 of w and the
//     sign bit) come from the same expressions. The box (and the matrix), operation by operation like the
//     original.
//   - The 12 dot products of the planes (vmsum4fp128 / vmsum3fp128 = simde_mm_dp_ps): on the Switch and on
//     PC, dp_ps multiplies and adds in pairs, ((x0 + x1) + (x2 + x3)), with faddp or DPPS; here they are done
//     four at a time with simde_mm_hadd_ps, which is that same sum (faddp / haddps): same products, same
//     sums, same order. The two chains min(0.5, x0, ..., x5) only serve to see whether they are >= 0: without
//     NaN they are the true minimum (>= 0 if and only if all six are); with any NaN the original's chain is
//     done, value by value. None of those values leaves the function (v0-v13, see registers): only r3. Where
//     dp_ps is not native, everything is done one by one, like the original.
//   - The same denormal mode: enableFlushMode() before the vector part, and the thread leaves in that mode,
//     as with the original. Its disableFlushMode only matters for the lfs, which are not needed here (see
//     registers).
//   - Same compiler and same options as the generated code: on the Switch this file and nfsmw_recomp get the
//     same FLAGS, without -ffp-contract=off (only the SDK has that). The center's "vmaddfp" comes out as
//     fmla in the original (disassembled) and here (devkitA64, same expression). Even if one side did not
//     fuse it, the only possible difference would be the sign of a zero in c, which changes no comparison.
//   - Memory: the same reads and the same writes in its 128-byte frame below r1 (lr at r1 - 8, the frame
//     back link, the box at +80 and +96 or, with a matrix, the two transformed 16-byte vectors): the stack
//     ends up identical byte for byte. If r1 is not 16-byte aligned, or some read lands inside that frame
//     (the original would do it after writing the frame), the original runs.
//   - Registers: r3, and r12 and lr as its epilogue leaves them; r1 untouched. The other volatiles the
//     original changes (r4-r11, f0, f9-f13 and v0-v13) are read by nobody: liveness analysis of all the
//     generated code (56,338 functions). At its 17 direct call sites, only r3 and f1 (which it does not
//     touch) are live afterwards. The two branches to it (8243_E7D0 and 8221_E7A0) are only reached through
//     indirect calls, and there the caller cannot rely on another function's volatiles: out of the game's
//     12,943, the analysis flags 47 (r4-r10 read by variadic functions or by paths it cannot tell apart, and
//     v1 as a returned vector); 40 do not set up r5 and r6, which this one reads on entry, and the other 7,
//     reviewed one by one, call other things. cr6, xer, v30 and v31 are local variables in the generated code.
//
// SELF-CHECKING GUARD (project rule)
//   The first kChecks calls and then 1 of every kPeriod: the native version computes and writes its
//   frame, what it left is saved, the frame is undone, the original runs and r3, r12, lr, r1, the denormal
//   mode and the 128 bytes of the frame are compared (the 32 of the box only if there is no NaN: the original
//   passes it through lfs/stfs in double precision, and a signaling NaN becomes quiet). In those calls the
//   original's state is kept. A single difference turns the native version off for the rest of the session
//   ("[visible] DIFFERENCE" in the log, with the data). "[visible]" line every 10 s with the counts. If
//   something looked wrong: nfsmw_visible_native = false.
// ---------------------------------------------------------------------------------------------------
#include <rex/ppc/intrinsics.h>

REXCVAR_DEFINE_BOOL(nfsmw_visible_native, true, "NFSMW",
                    "eViewPlatInterface::GetVisibleState (sub_8243E7D8: la box contra los 6 planes de la vista) en "
                    "native (build 174), identico bit a bit. Se comprueba contra la original (the primeras 200.000 "
                    "calls y after 1 de every 4096) y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8243E7D8);

namespace {
namespace visible {

using V = simde__m128;

constexpr uint64_t kChecks = 200000;  // first calls checked (~2.5 s of racing)
constexpr uint64_t kPeriod = 4096;           // then 1 of every kPeriod (a power of 2)

// Like the other counters in the file: no atomic read-modify-write (A57 without LSE).
std::atomic<bool> g_off{false};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_native{0};      // last 10 s
std::atomic<uint64_t> g_original{0};   // last 10 s: misaligned stack or a read inside the frame
std::atomic<uint64_t> g_checked{0};  // since startup, all without differences
std::atomic<int64_t> g_next_ms{0};

// lvx128 exactly as the generated code translates it: the aligned 16-byte block, reversed with VectorMaskL
// (lane 3 is PowerPC word 0).
inline V Lvx(uint8_t* base, uint32_t address) {
  const uint32_t ea = address & ~0xFu;
  return simde_mm_castsi128_ps(
      simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(Pointer(base, ea))),
                            simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

// stvx as is.
inline void Stvx(uint8_t* base, uint32_t address, V v) {
  const uint32_t ea = address & ~0xFu;
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(Pointer(base, ea)),
                       simde_mm_shuffle_epi8(simde_mm_castps_si128(v),
                                             simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

inline V Y(V a, simde__m128i mask) {  // vand vD,a,mask
  return simde_mm_castsi128_ps(simde_mm_and_si128(simde_mm_castps_si128(a), mask));
}

inline V Absolute(V flat, simde__m128i signo) {  // vandc vD,flat,v13 = simde_mm_andnot_si128(v13, flat)
  return simde_mm_castsi128_ps(simde_mm_andnot_si128(signo, simde_mm_castps_si128(flat)));
}

// Does a read of n bytes at "address" land inside the 128-byte frame that the original writes below r1?
inline bool InTheFrame(uint32_t address, uint32_t n, uint32_t frame) {
  return uint32_t(address - frame + n - 1) < 127u + n;
}

inline uint32_t Large32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline bool EsNaN(uint32_t bits) {
  return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
}

// Do the 32 bytes of the box in the frame (+80 to +111) contain a NaN?
inline bool BoxWithNaN(const uint8_t* frame) {
  for (uint32_t i = 80; i < 112; i += 4) {
    if (EsNaN(Large32(frame + i))) {
      return true;
    }
  }
  return false;
}

struct Calculation {
  uint32_t r3;
  bool matrix;
  uint32_t box[6];  // bits of min.x, min.y, min.z, max.x, max.y and max.z
  V mn;              // the box the original leaves in its frame (transformed, if there is a matrix)
  V mx;
};

// Where simde_mm_dp_ps is native, it adds in pairs, ((x0 + x1) + (x2 + x3)): on the Switch vmulq_f32 +
// vaddvq_f32 (two faddp) and on PC (SSE4.1) DPPS. simde_mm_hadd_ps does those same sums in one instruction
// (faddp / haddps, which is not fused with the product), so the 12 dot products come out bit-identical four
// at a time. Where it is not native (portable simde adds in a different order), simde_mm_dp_ps one by one,
// like the original. The PC test forces the second path with NFSMW_VISIBLE_BY_PAIRS=0.
#ifndef NFSMW_VISIBLE_BY_PAIRS
#if defined(SIMDE_ARM_NEON_A64V8_NATIVE) || (defined(SIMDE_X86_SSE4_1_NATIVE) && defined(SIMDE_X86_SSE3_NATIVE))
#define NFSMW_VISIBLE_BY_PAIRS 1
#else
#define NFSMW_VISIBLE_BY_PAIRS 0
#endif
#endif
constexpr bool kByPairs = NFSMW_VISIBLE_BY_PAIRS != 0;

// The product of a vmsum3fp128 (e . |P|): lane 0 (w) at +0.0, like the 0xEF mask of simde_mm_dp_ps.
inline V SinW(V e, V flat, simde__m128i signo, simde__m128i xyz) {
  return Y(simde_mm_mul_ps(e, Absolute(flat, signo)), xyz);
}

inline V Pair(V x) {  // [a b c d] -> [a+b c+d a+b c+d]
  return simde_mm_hadd_ps(x, x);
}

// The original's chain with a NaN: min(0.5, x0, ..., x5) with simde_mm_min_ps, each value repeated in the 4
// lanes as in the original (lanes [x0 x1 x2 x3] and [x4 x5 x4 x5]). Is it >= 0?
[[gnu::noinline]] bool ChainWithNaN(V middle, V x0123, V x45) {
  alignas(16) float a[4];
  alignas(16) float b[4];
  simde_mm_store_ps(a, x0123);
  simde_mm_store_ps(b, x45);
  V m = simde_mm_min_ps(middle, simde_mm_set1_ps(a[0]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(a[1]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(a[2]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(a[3]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(b[0]));
  m = simde_mm_min_ps(m, simde_mm_set1_ps(b[1]));
  return simde_mm_movemask_ps(simde_mm_cmpge_ps(m, simde_mm_setzero_ps())) == 0xF;
}

// The same, fast: without NaN that chain is the true minimum, and it is >= 0 if and only if all six are.
[[gnu::always_inline]] inline bool NoNegatives(V middle, V x0123, V x45) {
  const V zero = simde_mm_setzero_ps();
  if (simde_mm_movemask_ps(simde_mm_and_ps(simde_mm_cmpge_ps(x0123, zero), simde_mm_cmpge_ps(x45, zero))) == 0xF) {
    return true;  // all six >= 0 (with a NaN the comparison is already false)
  }
  if (simde_mm_movemask_ps(simde_mm_or_ps(simde_mm_cmpunord_ps(x0123, x0123), simde_mm_cmpunord_ps(x45, x45))) == 0) {
    return false;  // some < 0 and none is NaN
  }
  return ChainWithNaN(middle, x0123, x45);
}

// The helper 8243_E740 (nfsmw_recomp.87.cpp), in its order: the box times the matrix (rows at m, +16, +32, +48).
[[gnu::always_inline]] inline void BoxByMatrix(uint8_t* base, uint32_t m, V& mn, V& mx) {
  const simde__m128i v0 = simde_mm_castps_si128(mn);
  const simde__m128i v13 = simde_mm_castps_si128(mx);
  const V v8 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v0, 0xFF));   // vspltw v8,v0,0
  const V v7 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v13, 0xFF));  // vspltw v7,v13,0
  const V row0 = Lvx(base, m);                                           // lvx128 v12,r0,r3
  const V v6 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v0, 0xAA));   // vspltw v6,v0,1
  const V v4 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v0, 0x55));   // vspltw v4,v0,2
  const V v5 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v13, 0xAA));  // vspltw v5,v13,1
  const V a0 = simde_mm_mul_ps(row0, v8);                                // vmulfp128 v0,v12,v8
  const V v3 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(v13, 0x55));  // vspltw v3,v13,2
  const V b0 = simde_mm_mul_ps(row0, v7);                                // vmulfp128 v13,v12,v7
  const V row1 = Lvx(base, m + 16);                                      // lvx128 v11,r0,r11
  const V a1 = simde_mm_mul_ps(row1, v6);                                // vmulfp128 v12,v11,v6
  const V row2 = Lvx(base, m + 32);                                      // lvx128 v9,r0,r9
  const V b1 = simde_mm_mul_ps(row1, v5);                                // vmulfp128 v11,v11,v5
  const V row3 = Lvx(base, m + 48);                                      // lvx128 v10,r0,r10
  const V a2 = simde_mm_mul_ps(row2, v4);                                // vmulfp128 v8,v9,v4
  const V b2 = simde_mm_mul_ps(row2, v3);                                // vmulfp128 v9,v9,v3
  const V lo0 = simde_mm_min_ps(a0, b0);                                  // vminfp v7,v0,v13
  const V hi0 = simde_mm_max_ps(a0, b0);                                  // vmaxfp v0,v0,v13
  const V lo1 = simde_mm_min_ps(a1, b1);                                  // vminfp v13,v12,v11
  const V hi1 = simde_mm_max_ps(a1, b1);                                  // vmaxfp v12,v12,v11
  const V lo2 = simde_mm_min_ps(a2, b2);                                  // vminfp v11,v8,v9
  const V lo = simde_mm_add_ps(row3, lo0);                               // vaddfp v7,v10,v7
  const V hi = simde_mm_add_ps(row3, hi0);                               // vaddfp v0,v10,v0
  const V hi2 = simde_mm_max_ps(a2, b2);                                  // vmaxfp v10,v8,v9
  mn = simde_mm_add_ps(simde_mm_add_ps(lo, lo1), lo2);                    // vaddfp v13,v7,v13; vaddfp v13,v13,v11
  mx = simde_mm_add_ps(simde_mm_add_ps(hi, hi1), hi2);                    // vaddfp v0,v0,v12; vaddfp v0,v0,v10
}

// The whole original, without touching memory or the context (except the denormal mode, like the original).
// false = the original has to run, and in that case nothing has been touched.
[[gnu::always_inline]] inline bool Compute(PPCContext& ctx, uint8_t* base, Calculation& k) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t frame = stack - 128;  // stwu r1,-128(r1)
  const uint32_t vista = ctx.r3.u32;
  const uint32_t pmin = ctx.r4.u32;
  const uint32_t pmax = ctx.r5.u32;
  const uint32_t mat = ctx.r6.u32;
  if ((stack & 0xFu) != 0 || InTheFrame(pmin, 12, frame) || InTheFrame(pmax, 12, frame) || InTheFrame(vista, 4, frame) ||
      (mat != 0 && InTheFrame(mat & ~0xFu, 64, frame))) {
    return false;
  }
  const uint32_t planes = Read32(base, vista) + 192u;  // lwz r11,0(r8); addi r11,r11,192
  if (InTheFrame(planes & ~0xFu, 96, frame)) {
    return false;
  }
  // lfs of the box, stfs to r1+80 and r1+96 and lvx128 back: lanes 3, 2 and 1 = x, y, z. Lane 0 (w) would be
  // whatever was on the stack: it is always masked before use, and the matrix helper does not read it.
  for (uint32_t i = 0; i < 3; ++i) {
    k.box[i] = Read32(base, pmin + 4 * i);
    k.box[3 + i] = Read32(base, pmax + 4 * i);
  }
  V mn = simde_mm_castsi128_ps(simde_mm_set_epi32(int32_t(k.box[0]), int32_t(k.box[1]), int32_t(k.box[2]), 0));
  V mx = simde_mm_castsi128_ps(simde_mm_set_epi32(int32_t(k.box[3]), int32_t(k.box[4]), int32_t(k.box[5]), 0));
  ctx.fpscr.enableFlushMode();  // the one of "vcfux v10,v13,1" (and the helper's): the original leaves in this mode
  k.matrix = mat != 0;
  if (k.matrix) {  // cmplwi cr6,r6,0; beq; bl to the helper with r4 = r1+80 and r5 = r1+96
    BoxByMatrix(base, mat, mn, mx);
  }
  k.mn = mn;
  k.mx = mx;

  const simde__m128i v11 = simde_mm_set1_epi32(int(0x0));                // vspltisw v11,0
  const simde__m128i ones = simde_mm_set1_epi32(int(0xFFFFFFFF));        // vspltisw v0,-1
  const simde__m128i uno = simde_mm_set1_epi32(int(0x1));                // vspltisw v13,1
  const simde__m128i xyz = simde_mm_alignr_epi8(ones, v11, 12);          // vsldoi v12,v0,v11,4
  const V middle = simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(uno),  // vcfux v10,v13,1
                                  simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))));
  const V w1 = simde_mm_castsi128_ps(                                    // vcfux v3,v13,0; vsldoi v3,v11,v3,4
      simde_mm_alignr_epi8(v11, simde_mm_castps_si128(rex::ppc::simde_mm_cvtepu32_ps_(uno)), 12));
  const simde__m128i signo = simde_mm_sllv_epi32(ones, simde_mm_and_si128(ones, simde_mm_set1_epi32(0x1F)));  // vslw
  const V zero = simde_mm_castsi128_ps(v11);

  const V a = Y(mn, xyz);                                  // vand v0,v2,v12
  const V b = Y(mx, xyz);                                  // vand v12,v1,v12
  const V sum = simde_mm_add_ps(a, b);                    // vaddfp v0,v0,v12
  const V p0 = Lvx(base, planes);                          // lvx128 v9,r0,r11
  const V p1 = Lvx(base, planes + 16);                     // lvx128 v8,r0,r9
  const V p2 = Lvx(base, planes + 32);                     // lvx128 v7,r0,r8
  const V p3 = Lvx(base, planes + 48);                     // lvx128 v6,r0,r7
  const V p4 = Lvx(base, planes + 64);                     // lvx128 v5,r0,r6
  const V p5 = Lvx(base, planes + 80);                     // lvx128 v4,r0,r5
  const V c = simde_mm_add_ps(simde_mm_mul_ps(sum, middle), w1);  // vmaddfp v0,v0,v10,v3
  const V e = simde_mm_sub_ps(b, c);                       // vsubfp v12,v12,v0
  if constexpr (kByPairs) {
    // d = c . P and r = e . |P| of the six planes (vmsum4fp128 / vmsum3fp128), in lanes [0 1 2 3] and [4 5 4 5];
    // "outside" with d + r and "inside" with d - r, with the same operand order as the original.
    const V d0123 = simde_mm_hadd_ps(simde_mm_hadd_ps(simde_mm_mul_ps(c, p0), simde_mm_mul_ps(c, p1)),
                                     simde_mm_hadd_ps(simde_mm_mul_ps(c, p2), simde_mm_mul_ps(c, p3)));
    const V d45 = Pair(simde_mm_hadd_ps(simde_mm_mul_ps(c, p4), simde_mm_mul_ps(c, p5)));
    const V r0123 = simde_mm_hadd_ps(simde_mm_hadd_ps(SinW(e, p0, signo, xyz), SinW(e, p1, signo, xyz)),
                                     simde_mm_hadd_ps(SinW(e, p2, signo, xyz), SinW(e, p3, signo, xyz)));
    const V r45 = Pair(simde_mm_hadd_ps(SinW(e, p4, signo, xyz), SinW(e, p5, signo, xyz)));
    if (!NoNegatives(middle, simde_mm_add_ps(d0123, r0123), simde_mm_add_ps(d45, r45))) {
      k.r3 = 0;
    } else {
      k.r3 = NoNegatives(middle, simde_mm_sub_ps(d0123, r0123), simde_mm_sub_ps(d45, r45)) ? 2u : 1u;
    }
    return true;
  }
  // One by one, like the original.
  const V d0 = simde_mm_dp_ps(c, p0, 0xFF);                // vmsum4fp128 v9,v0,v9
  const V d1 = simde_mm_dp_ps(c, p1, 0xFF);                // vmsum4fp128 v8,v0,v8
  const V r0 = simde_mm_dp_ps(e, Absolute(p0, signo), 0xEF);  // vandc v2,v9,v13; vmsum3fp128 v3,v12,v2
  const V d2 = simde_mm_dp_ps(c, p2, 0xFF);                // vmsum4fp128 v9,v0,v7
  const V d3 = simde_mm_dp_ps(c, p3, 0xFF);                // vmsum4fp128 v7,v0,v6
  const V r2 = simde_mm_dp_ps(e, Absolute(p2, signo), 0xEF);  // vandc v3,v7,v13; vmsum3fp128 v3,v12,v3
  const V r1 = simde_mm_dp_ps(e, Absolute(p1, signo), 0xEF);  // vandc v1,v8,v13; vmsum3fp128 v6,v12,v1
  const V r3 = simde_mm_dp_ps(e, Absolute(p3, signo), 0xEF);  // vandc v2,v6,v13; vmsum3fp128 v10,v12,v2
  const V d4 = simde_mm_dp_ps(c, p4, 0xFF);                // vmsum4fp128 v9,v0,v5
  const V d5 = simde_mm_dp_ps(c, p5, 0xFF);                // vmsum4fp128 v0,v0,v4
  const V r4 = simde_mm_dp_ps(e, Absolute(p4, signo), 0xEF);  // vandc v3,v5,v13; vmsum3fp128 v13,v12,v3
  const V r5 = simde_mm_dp_ps(e, Absolute(p5, signo), 0xEF);  // vandc v5,v4,v13; vmsum3fp128 v12,v12,v5
  // "outside": min(0.5, d + r) plane by plane, always with the accumulator as the first operand (vminfp
  // v30,v10,v2; v3,v30,v2; v6,v3,v6; v7,v6,v2; v12,v7,v8; v12,v12,v9).
  V outside = simde_mm_min_ps(middle, simde_mm_add_ps(d0, r0));  // vaddfp v2,v9,v3
  outside = simde_mm_min_ps(outside, simde_mm_add_ps(d1, r1));    // vaddfp v2,v8,v6
  outside = simde_mm_min_ps(outside, simde_mm_add_ps(d2, r2));    // vaddfp v6,v9,v3
  outside = simde_mm_min_ps(outside, simde_mm_add_ps(d3, r3));    // vaddfp v2,v7,v10
  outside = simde_mm_min_ps(outside, simde_mm_add_ps(d4, r4));    // vaddfp v8,v9,v13
  outside = simde_mm_min_ps(outside, simde_mm_add_ps(d5, r5));    // vaddfp v9,v0,v12
  // "inside": min(0,5, d - r) (vminfp v1,v10,v31; v8,v1,v8; v8,v8,v9; v10,v8,v10; v13,v10,v13; v0,v13,v0).
  V inside = simde_mm_min_ps(middle, simde_mm_sub_ps(d0, r0));  // vsubfp v31,v9,v3
  inside = simde_mm_min_ps(inside, simde_mm_sub_ps(d1, r1));   // vsubfp v8,v8,v6
  inside = simde_mm_min_ps(inside, simde_mm_sub_ps(d2, r2));   // vsubfp v9,v9,v3
  inside = simde_mm_min_ps(inside, simde_mm_sub_ps(d3, r3));   // vsubfp v10,v7,v10
  inside = simde_mm_min_ps(inside, simde_mm_sub_ps(d4, r4));   // vsubfp v13,v9,v13
  inside = simde_mm_min_ps(inside, simde_mm_sub_ps(d5, r5));   // vsubfp v0,v0,v12
  // vcmpgefp. + mfocrf + not + rlwinm: 0 if not all are >= 0; if they are, vcmpgefp. of "inside" + rlwimi + rlwinm.
  if (simde_mm_movemask_ps(simde_mm_cmpge_ps(outside, zero)) != 0xF) {
    k.r3 = 0;
  } else {
    k.r3 = simde_mm_movemask_ps(simde_mm_cmpge_ps(inside, zero)) == 0xF ? 2u : 1u;
  }
  return true;
}

// The original's writes to its stack frame, with what would remain at the end.
[[gnu::always_inline]] inline void WriteFrame(uint8_t* base, uint32_t stack, uint64_t lr, const Calculation& k) {
  const uint32_t frame = stack - 128;
  Write32(base, stack - 8, uint32_t(lr));  // mflr r12; stw r12,-8(r1)
  Write32(base, frame, stack);             // stwu r1,-128(r1)
  if (k.matrix) {
    Stvx(base, frame + 80, k.mn);  // stvx v13,r0,r4 of the helper (overwrites the stfs of the box)
    Stvx(base, frame + 96, k.mx);  // stvx v0,r0,r5
  } else {
    for (uint32_t i = 0; i < 3; ++i) {
      Write32(base, frame + 80 + 4 * i, k.box[i]);      // stfs f0,80 / f13,84 / f12,88
      Write32(base, frame + 96 + 4 * i, k.box[3 + i]);  // stfs f11,96 / f10,100 / f9,104
    }
  }
}

// Normal path. false = nothing touched: let the original run.
inline bool Native(PPCContext& ctx, uint8_t* base) {
  Calculation k;
  if (!Compute(ctx, base, k)) {
    return false;
  }
  WriteFrame(base, ctx.r1.u32, ctx.lr, k);
  ctx.r3.u64 = k.r3;
  ctx.r12.u64 = uint32_t(ctx.lr);  // epilogue: lwz r12,-8(r1); mtlr r12
  ctx.lr = ctx.r12.u64;
  return true;
}

// Guard: the native version and then the original over the same frame; the original's state is kept.
void Check(PPCContext& ctx, uint8_t* base, uint64_t n) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t frame = stack - 128;
  const uint32_t vista = ctx.r3.u32;
  const uint32_t pmin = ctx.r4.u32;
  const uint32_t pmax = ctx.r5.u32;
  const uint32_t mat = ctx.r6.u32;
  const uint64_t lr = ctx.lr;
  const uint64_t r1 = ctx.r1.u64;
  Calculation k;
  if (Offset(frame) != Offset(stack - 1) || !Compute(ctx, base, k)) {
    Add(g_original, uint64_t(1));
    __imp__sub_8243E7D8(ctx, base);
    return;
  }
  uint8_t* const p = Pointer(base, frame);
  uint8_t before[128];
  uint8_t native[128];
  std::memcpy(before, p, 128);
  WriteFrame(base, stack, lr, k);
  std::memcpy(native, p, 128);
  const uint32_t csr_native = ctx.fpscr.csr;
  std::memcpy(p, before, 128);  // the original starts from the same frame
  __imp__sub_8243E7D8(ctx, base);

  const char* que = nullptr;
  uint32_t byte = 0;
  if (ctx.r3.u64 != k.r3) {
    que = "r3";
  } else if (ctx.r12.u64 != uint32_t(lr)) {
    que = "r12";
  } else if (ctx.lr != uint32_t(lr)) {
    que = "lr";
  } else if (ctx.r1.u64 != r1) {
    que = "r1";
  } else if (ctx.fpscr.csr != csr_native) {
    que = "mode de denormales";
  } else {
    const bool con_nan = BoxWithNaN(native) || BoxWithNaN(p);
    for (uint32_t i = 0; i < 128 && !que; ++i) {
      if (con_nan && i >= 80 && i < 112) {
        continue;  // NaN in the box: the original passes it through double (lfs/stfs) and can change its payload
      }
      if (p[i] != native[i]) {
        que = "frame de stack";
        byte = i;
      }
    }
  }
  if (que) {
    g_off.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[visible] DIFFERENCE con la original ({}, byte +{} del frame) en la call {}: r3 native {} y original "
                "{}; vista 0x{:08X}, min 0x{:08X}, max 0x{:08X}, matrix 0x{:08X}, stack 0x{:08X}, box {:08X} {:08X} "
                "{:08X} / {:08X} {:08X} {:08X}. Path native OFF para el rest de la session: se queda la original",
                que, byte, n, k.r3, ctx.r3.u64, vista, pmin, pmax, mat, stack, k.box[0], k.box[1], k.box[2],
                k.box[3], k.box[4], k.box[5]);
    return;
  }
  Add(g_checked, uint64_t(1));
  if (n == kChecks) {
    REXLOG_INFO("[visible] {} calls checked contra la original (r3, r12, lr, r1, mode de denormales y frame de "
                "stack), 0 differences: GetVisibleState en native, y sigue checking 1 de every {}",
                g_checked.load(std::memory_order_relaxed), kPeriod);
  }
}

void Call(PPCContext& ctx, uint8_t* base) {
  static const bool active = REXCVAR_GET(nfsmw_visible_native);
  if (!active || g_off.load(std::memory_order_relaxed)) {
    __imp__sub_8243E7D8(ctx, base);
    return;
  }
  const uint64_t n = g_calls.load(std::memory_order_relaxed) + 1;
  g_calls.store(n, std::memory_order_relaxed);
  if (n <= kChecks || (n & (kPeriod - 1)) == 0) {
    Check(ctx, base, n);
    return;
  }
  if (Native(ctx, base)) {
    Add(g_native, uint64_t(1));
    return;
  }
  Add(g_original, uint64_t(1));
  __imp__sub_8243E7D8(ctx, base);
}

void Report() {
  const int64_t now = NowNs() / 1000000;
  const int64_t next = g_next_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_next_ms.store(now + 10000, std::memory_order_relaxed);
  const bool active = REXCVAR_GET(nfsmw_visible_native);
  if (next == 0) {
    REXLOG_INFO("[visible] GetVisibleState (8243E7D8) {}",
                active ? "en native (build 174): begins checking contra la original"
                       : "por la original (nfsmw_visible_native = false)");
    return;
  }
  if (!active) {
    return;
  }
  NFSMW_REPORT_DEFERRED("[visible] last 10 s: {} en native, {} por la original (stack desalineada o read en su frame){}; "
              "checked contra la original since el arranque: {} (the primeras {} calls y after 1 de every {})",
              g_native.exchange(0, std::memory_order_relaxed), g_original.exchange(0, std::memory_order_relaxed),
              g_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "",
              g_checked.load(std::memory_order_relaxed), kChecks, kPeriod);
}

}  // namespace visible
}  // namespace

REX_HOOK_RAW(sub_8243E7D8) {  // eViewPlatInterface::GetVisibleState: native (see above)
  Measure(g_m_visible, [&] { visible::Call(ctx, base); });
  if ((g_m_visible.calls.load(std::memory_order_relaxed) & 4095) == 0) {
    ReportMeasurement();
    visible::Report();
  }
}

REX_EXTERN(__imp__sub_824C2850);
// DrawAScenery is native (nfsmw_scenery_native.cpp, cvar nfsmw_scenery_native, with its guard).
// The measurement stays here for comparison: the recompiled version took 0.83 us per call in a race.
namespace nfsmw::scenery_native {
void DrawAScenery(PPCContext& ctx, uint8_t* base);
}
REX_HOOK_RAW(sub_824C2850) {  // ScenerySectionHeader::DrawAScenery
  Measure(g_m_draw, [&] { nfsmw::scenery_native::DrawAScenery(ctx, base); });
}

REX_EXTERN(__imp__sub_824C2F48);
REX_HOOK_RAW(sub_824C2F48) {  // ScenerySectionHeader::TreeCull
  // Also its exact time, for the [hitch] game log line (7,400 calls/s: two clock reads each). The 1-in-16
  // measurement of the [measurement] line is unchanged.
  const int64_t start_ns = NowNs();
  Measure(g_m_tree, [&] { __imp__sub_824C2F48(ctx, base); });
  nfsmw::waits::Add(nfsmw::waits::kPreparerScenery, uint64_t(NowNs() - start_ns));
}

// ---------------------------------------------------------------------------------------------------
// sub_826992F0, the upload of effect parameters to the device, in native code.
//
// WHAT IT DOES (read instruction by instruction)
//   Two groups (the effect's and the shared one), each with 8 lists. For each 64-bit word of the
//   group's "dirty" mask, AND with the list's mask; each run of consecutive bits is a set of
//   consecutive 16-byte entries of a table. Lists 0 and 1: VS and PS floating-point constants
//   -> copy of 1-4 16-byte vectors to the device (register + 120 or + 376) and set their bits in
//   the device's dirty mask (+16 or +24). Lists 2-7: integers, booleans and the rest, through
//   calls to D3D functions. At the end, dcbzl of two 128-byte lines (clears the dirty bits).
//
//   Measured: ~150 ms/s of the game thread (~20 %), and in recompiled PowerPC each copied vector
//   is two loads/stores with byte permutation plus 8 reloads from the stack.
//
// WHAT IT DOES HERE
//   Lists 0 and 1 of both groups, in native code: a plain 16-byte copy (lvx128 + stvx128 with the
//   same permutation cancel out) and the same integers as the original. If any of lists 2-7 of
//   either group has work to do, the whole original is called: that way the D3D calls, their
//   registers and their stack are exactly the game's.
//
// SELF-CHECKING GUARD
//   The first kChecks calls through the native path run twice: the native version
//   records each write (address and previous bytes), its results are saved, it is undone, the
//   original runs and they are compared byte by byte. A single difference turns the native path
//   off for good ("[effects] DIFFERENCE" line in the log). "[effects]" line every 10 s with
//   the counts.
// ---------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(nfsmw_d3d_effects_native, true, "NFSMW",
                    "Submission de parametros de effect (sub_826992F0) en native: whole con "
                    "nfsmw_d3d_effects_native_all (build 171); sin el, solo the constants de comma float "
                    "y, con integers, booleans o textures, la original. Se comprueba contra la original y se "
                    "apaga sola si difiere")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
inline uint64_t Read64(uint8_t* base, uint32_t address) {
  uint64_t v;
  std::memcpy(&v, Pointer(base, address), 8);
  return __builtin_bswap64(v);
}

// Write log for the guard: address and previous bytes, to undo.
struct Undo {
  uint32_t address;
  uint32_t bytes;
  uint8_t before[128];
};

struct Writer {
  uint8_t* base;
  std::vector<Undo>* reg_entry;  // nullptr outside the check

  void Note(uint32_t address, uint32_t bytes) {
    if (!reg_entry) {
      return;
    }
    Undo d;
    d.address = address;
    d.bytes = bytes;
    std::memcpy(d.before, Pointer(base, address), bytes);
    reg_entry->push_back(d);
  }
  void Copy16(uint32_t target, uint32_t source) {
    Note(target, 16);
    std::memmove(Pointer(base, target), Pointer(base, source), 16);
  }
  void Write64(uint32_t address, uint64_t input_value) {
    Note(address, 8);
    input_value = __builtin_bswap64(input_value);
    std::memcpy(Pointer(base, address), &input_value, 8);
  }
  void Cero128(uint32_t address) {
    Note(address, 128);
    std::memset(Pointer(base, address), 0, 128);
  }
};

struct GroupEffect {
  uint32_t count;    // 64-bit words of the mask ([this+288] or [this+292])
  uint32_t dirty;    // the group's dirty mask (this or [this+256])
  uint32_t table;     // r28 o r27
  uint32_t lists;    // offset of list 0 within the table (0 or 32)
  uint32_t entries;  // [table+64] o [table+68]
  uint32_t r23;
  uint32_t r24;
};

// Some of lists 2-7 have work: those call D3D functions -> the original.
bool There_isCalls(uint8_t* base, const GroupEffect& g) {
  for (uint32_t k = 2; k < 8; ++k) {
    const uint32_t list = Read32(base, g.table + g.lists + 4 * k);
    for (uint32_t i = 0; i < g.count; ++i) {
      if (Read64(base, g.dirty + i * 8) & Read64(base, list + i * 8)) {
        return true;
      }
    }
  }
  return false;
}

// Lists 0 (field 4, register+120, device dirty bits +16) and 1 (field 8, +376, +24).
uint64_t ListFloat(Writer& w, const GroupEffect& g, uint32_t vulkan_device, uint32_t k) {
  uint8_t* const base = w.base;
  const uint32_t field = k == 0 ? 4 : 8;
  const uint32_t bias = k == 0 ? 120 : 376;
  const uint32_t dirty_device = vulkan_device + (k == 0 ? 16 : 24);
  const uint64_t r18 = uint64_t(1) << 63;
  uint64_t vectors = 0;
  const uint32_t list = Read32(base, g.table + g.lists + 4 * k);
  for (uint32_t i = 0; i < g.count; ++i) {
    uint64_t m = Read64(base, g.dirty + i * 8) & Read64(base, list + i * 8);
    uint32_t e = g.entries + i * 1024;
    while (m != 0) {
      const uint32_t z = uint32_t(__builtin_clzll(m));
      e += z * 16;
      m <<= z;
      const uint64_t inv = ~m;
      const uint32_t n = inv ? uint32_t(__builtin_clzll(inv)) : 64u;
      const uint32_t fin = e + n * 16;
      m = n >= 64 ? 0 : (m << n);
      do {
        const uint32_t w0 = Read32(base, e);
        uint32_t wc = Read32(base, e + field);
        const uint32_t t = g.r23 + (((w0 << 17) | (w0 >> 15)) & 0x1FFF8u);
        const uint32_t idx = Read32(base, t + 4);
        const uint32_t source = g.r24 + (((idx << 4) | (idx >> 28)) & 0xFFFF0u);
        const uint32_t target = vulkan_device + ((((wc & 0x3FFu) + bias) << 4) & 0xFFFFFFF0u);
        uint32_t j = 0;
        do {
          w.Copy16((target + j * 16) & ~0xFu, (source + j * 16) & ~0xFu);
          ++vectors;
          ++j;
          wc = Read32(base, e + field);
        } while (j <= ((wc >> 10) & 3u));
        wc = Read32(base, e + field);
        e += 16;
        const uint64_t lo = wc & 0x3FFu;
        const uint64_t a = (lo >> 2) & 0x3FFFFFFFu;                          // r10
        const uint64_t b = ((((wc >> 10) & 3u) + lo) >> 2) & 0x3FFFFFFFu;     // r9
        uint64_t d = (b - a) & 0xFFFFFFFFu;
        uint64_t s = d & 0x7F;
        if (s > 0x3F) {
          s = 0x3F;
        }
        const uint64_t bits = uint64_t(int64_t(r18) >> s);
        const uint8_t a8 = uint8_t(a);
        const uint64_t v = (a8 & 0x40) ? 0 : (bits >> (a8 & 0x7F));
        w.Write64(dirty_device, v | Read64(base, dirty_device));
      } while (e < fin);
    }
  }
  return vectors;
}

constexpr uint32_t kChecks = 256;
std::atomic<bool> g_effects_off{false};
std::atomic<uint32_t> g_effects_checked{0};
std::atomic<uint64_t> g_effects_native{0};
std::atomic<uint64_t> g_effects_original{0};
std::atomic<uint64_t> g_effects_vectors{0};
std::atomic<int64_t> g_effects_next_ms{0};

// The whole native path. Returns false (without having written anything) if the original has to run.
bool EffectsNative(Writer& w, uint32_t self, uint64_t& vectors) {
  uint8_t* const base = w.base;
  const uint32_t table_a = Read32(base, self + 536);
  GroupEffect a;
  a.count = Read32(base, self + 288);
  a.dirty = self;
  a.table = table_a;
  a.lists = 0;
  a.entries = Read32(base, table_a + 64);
  a.r23 = Read32(base, self + 264);
  a.r24 = Read32(base, self + 296);
  // r27: the table of the shared group (cntlzw/or/rlwinm/and/andc of the original).
  const uint32_t table_b = (Read32(base, self + 696) != 0 && Read32(base, 0x828E0B40u) != 0)
                               ? Read32(base, 0x8290E564u)
                               : table_a;
  GroupEffect b;
  b.count = Read32(base, self + 292);
  b.dirty = Read32(base, self + 256);
  b.table = table_b;
  b.lists = 32;
  b.entries = Read32(base, table_b + 68);
  b.r23 = Read32(base, Read32(base, self + 268));
  b.r24 = Read32(base, Read32(base, self + 300));
  if (There_isCalls(base, a) || There_isCalls(base, b)) {
    return false;
  }
  const uint32_t vulkan_device = Read32(base, self + 700);
  vectors += ListFloat(w, a, vulkan_device, 0);
  vectors += ListFloat(w, a, vulkan_device, 1);
  vectors += ListFloat(w, b, vulkan_device, 0);
  vectors += ListFloat(w, b, vulkan_device, 1);
  w.Cero128(self & ~127u);                      // dcbzl r0,r20
  w.Cero128(Read32(base, self + 256) & ~127u);  // lwz r11,256(r20); dcbzl r0,r11
  return true;
}

// State and counts of the full path (nfsmw_d3d_effects_native_all, further down).
// Like the other counters in this file, written by the drawing thread, without atomic
// read-modify-write.
struct TallyAll {
  std::atomic<uint64_t> native{0};      // calls done entirely in native code (checked ones included)
  std::atomic<uint64_t> with_lists{0};   // of those, with work in lists 2-7 (formerly left to the original)
  std::atomic<uint64_t> vectors{0};     // constants de comma float copied
  std::atomic<uint64_t> integers{0};      // entries de constants whole
  std::atomic<uint64_t> booleans{0};    // entries de constants booleanas
  std::atomic<uint64_t> textures{0};     // SetTexture calls done in native code
  std::atomic<uint64_t> rare{0};        // texture releases (done by the original)
  std::atomic<uint64_t> several{0};       // integers of 2-4 registers (vectors 2-4 from the original's stack)
  std::atomic<uint64_t> abandoned{0};  // checks abandoned because of a texture release
  std::atomic<uint64_t> full{0};       // checks abandoned because the layer is full (more than 8 KB written)
};
TallyAll g_todo;
constexpr uint32_t kChecksAll = 512;        // first calls of the full path that are checked
constexpr uint64_t kPeriodAll = 4096;              // then 1 of every kPeriodAll
std::atomic<bool> g_all_off{false};             // a difference turns off only the full path
std::atomic<bool> g_all_pending{false};           // the last check was abandoned: check the next one
std::atomic<uint32_t> g_all_checked{0};         // checks complete, sin differences
std::atomic<uint32_t> g_all_checked_lists{0};  // of those, with integers, booleans or textures
std::atomic<uint64_t> g_all_calls{0};

void ReportEffects() {
  const int64_t now = NowNs() / 1000000;
  const int64_t next = g_effects_next_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_effects_next_ms.store(now + 10000, std::memory_order_relaxed);
  if (next == 0) {
    return;
  }
  const uint64_t native = g_effects_native.exchange(0, std::memory_order_relaxed);
  const uint64_t original = g_effects_original.exchange(0, std::memory_order_relaxed);
  const uint64_t vectors = g_effects_vectors.exchange(0, std::memory_order_relaxed);
  const uint64_t todo = g_todo.native.exchange(0, std::memory_order_relaxed);
  if (native != 0 || original != 0 || todo == 0) {  // the lists 0-1 path (floating-point constants only)
    REXLOG_INFO("[effects] last 10 s: {} en native ({:.1f} vectors every one), {} por la original "
                "(integers/booleans){}; checked contra la original: {} de {}",
                native, native ? double(vectors) / double(native) : 0.0, original,
                g_effects_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "",
                g_effects_checked.load(std::memory_order_relaxed), kChecks);
  }
  if (todo != 0 || g_all_off.load(std::memory_order_relaxed)) {  // The full path
    const uint64_t vectors_all = g_todo.vectors.exchange(0, std::memory_order_relaxed);
    const uint64_t several = g_todo.several.exchange(0, std::memory_order_relaxed);
    NFSMW_REPORT_DEFERRED("[effects] path complete, last 10 s: {} en native ({:.1f} vectors every one; {} con lists "
                "2-7: {} integers, {} booleans, {} textures, {} liberaciones por la original{}){}; checked "
                "contra la original since el arranque: {} ({} con lists 2-7; the primeras {} y luego 1 de every "
                "{}); abandoned en estos 10 s: {} por one liberacion y {} por layer full",
                todo, todo ? double(vectors_all) / double(todo) : 0.0,
                g_todo.with_lists.exchange(0, std::memory_order_relaxed),
                g_todo.integers.exchange(0, std::memory_order_relaxed),
                g_todo.booleans.exchange(0, std::memory_order_relaxed),
                g_todo.textures.exchange(0, std::memory_order_relaxed),
                g_todo.rare.exchange(0, std::memory_order_relaxed),
                several ? fmt::format(", {} integers de 2-4 register_values", several) : std::string(),
                g_all_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "",
                g_all_checked.load(std::memory_order_relaxed),
                g_all_checked_lists.load(std::memory_order_relaxed), kChecksAll, kPeriodAll,
                g_todo.abandoned.exchange(0, std::memory_order_relaxed),
                g_todo.full.exchange(0, std::memory_order_relaxed));
  }
}
}  // namespace

REX_EXTERN(__imp__sub_826992F0);

// ---------------------------------------------------------------------------------------------------
// The whole of sub_826992F0 in native code (cvar nfsmw_d3d_effects_native_all).
//
// WHY
//   53 % of its ~72,000 calls/s had something in lists 2-7 (integers, booleans or textures) and went
//   entirely through the original: ~7 % of a core of the game thread in stack sampling. Lists 2-7
//   and the five small D3D functions they call are now handled here.
//
// WHAT EACH LIST DOES (PowerPC read instruction by instruction)
//   0 and 1  VS and PS floating-point constants: the same as the lists 0-1 path.
//   2 and 3  integer constants. vctsxs (float -> int32, truncating, saturated) of the parameter's
//            vector; its word 0 and words 1-3 of the default value table ([table+80] +
//            16 * byte 13 or 12 of the entry; vrlimi128 with mask 7) form a vector on the stack, from
//            which the D3D function (8259_B6B8 VS, 8259_B708 PS) stores (byte 11 << 16) | (byte 7 << 8) |
//            byte 3 into word 2536 (or 2552) + device register, and sets bit 0x80000000 of its dirty
//            mask (+32).
//   4 and 5  boolean constants. vctuxs (float -> uint32, saturated) of the vector; the D3D function
//            (8259_B578 VS, 8259_B5D0 PS) moves bit 0 of each word to bit (register & 31) of word
//            2528 (or 2532) + register / 32, and sets the same bit 0x80000000 of +32.
//   6 and 7  textures: the D3D SetTexture (8258_A648) with the pointer from the parameter's table. Its
//            rare call, releasing the texture that leaves the slot when it runs out of uses
//            (sub_82594C70), is left to the original, with the stack and registers it would have.
//   At the end, dcbzl of the effect's two dirty lines, as in the lists 0-1 path.
//
// WHY IT IS BIT-IDENTICAL
//   - Same order of reads and writes: group A list by list and then group B. Whatever the original
//     rereads on each word (the word count, the list and entry pointers, the dirty mask) and on each
//     entry is reread; what it keeps in registers on entry (device, table A, group B dirty mask,
//     r23/r24 of both groups) is read once; the group B table, after group A.
//   - The conversions are the same SDK functions the recompiled code uses
//     (rex::ppc::simde_mm_vctsxs and simde_mm_vctuxs, with the same lvx128 load and the same
//     vrlimi128), and enableFlushMode() is called before each one, as the original does, to leave
//     the thread in the same mode.
//   - The rest are 32-bit integers with the same rlwinm/rlwimi masks. The comparison in SetTexture's
//     rlwinm. is 32-bit, as the recompiled code does it.
//   - Integers of 2-4 registers: the D3D function reads vectors 2-4 from the original's stack
//     (sp - 448 + 176 ... + 223), where it does not write; those same bytes are read from the guest stack.
//   - r3 on return is what the original would leave (this, the device or whatever SetTexture leaves):
//     no caller reads it (all 24 call sites checked), but two tail jumps to this function
//     (8244_8730 and 8244_ED38) pass it on to whoever called them.
//   - No new hook: the D3D functions are done in here and the game's other calls to them do not
//     change. Their addresses are split with "_" on purpose: tools/direct_calls.py treats any
//     82xxxxxx address that appears in the sources as hooked and would stop making them direct calls.
//   - Not used with nfsmw_d3d_trace on: SetTexture has its trace hook there, which would not see
//     calls from here (its NotifyVideo does nothing with SetTexture).
//
// SELF-CHECKING GUARD (extended)
//   The first kChecksAll calls and then 1 of every kPeriodAll: the native version runs with
//   its writes in a separate layer (it does not touch game memory or the floating-point mode, and calls
//   nothing), then the original runs over the untouched memory, and the results are compared: every
//   byte written by the native version, the device area the original can write (+16..+10272 and the
//   texture pointers +12704..+12832: that way anything the original writes and the native version does
//   not also shows up), r3, r1 and the floating-point mode. If the SetTexture release is needed (or the
//   layer fills up: more than 8 KB written), that check is abandoned (the original does it) and the next
//   one is checked. A difference turns off only the full path and the lists 0-1 path comes back, with
//   its own guard ("[effects] DIFFERENCE del path complete" line in the log).
// ---------------------------------------------------------------------------------------------------
#include <rex/ppc/intrinsics.h>

REXCVAR_DEFINE_BOOL(nfsmw_d3d_effects_native_all, true, "NFSMW",
                    "sub_826992F0 WHOLE en native (build 171): tambien the constants whole y booleanas y "
                    "the textures (SetTexture). Se comprueba contra la original (the primeras 512 calls y "
                    "after 1 de every 4096) y se apaga sola si difiere; false = as la build 154 (solo the "
                    "constants de comma float). Necesita nfsmw_d3d_effects_native")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, nfsmw_d3d_trace);

REX_EXTERN(__imp__sub_82594C70);

namespace {

// rlwinm/rlwimi: 32-bit rotation (N from 1 to 31).
template <unsigned N>
inline uint32_t Rotl32(uint32_t x) {
  static_assert(N > 0 && N < 32, "rotation de 1 a 31");
  return (x << N) | (x >> (32 - N));
}

inline uint32_t ReadLarge32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Memory for the full path, direct: the game's.
struct MemoryDirect {
  static constexpr bool kChecking = false;
  uint8_t* base;
  PPCContext* ctx;

  uint8_t L8(uint32_t d) const { return *Pointer(base, d); }
  uint32_t L32(uint32_t d) const { return Read32(base, d); }
  uint64_t L64(uint32_t d) const { return Read64(base, d); }
  void Read16(uint32_t d, uint8_t* target) const { std::memcpy(target, Pointer(base, d), 16); }
  void E32(uint32_t d, uint32_t v) { Write32(base, d, v); }
  void E64(uint32_t d, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(Pointer(base, d), &v, 8);
  }
  void Copy16(uint32_t target, uint32_t source) {
    std::memmove(Pointer(base, target), Pointer(base, source), 16);
  }
  void Cero128(uint32_t d) { std::memset(Pointer(base, d), 0, 128); }
  void ModeVector() { ctx->fpscr.enableFlushMode(); }
  bool Full() const { return false; }
};

// Memory for the check: reads the game's and writes to a separate layer (a byte table keyed by address);
// that way the native version touches nothing and the original then runs over the untouched memory.
struct MemoryLayer {
  static constexpr bool kChecking = true;
  static constexpr uint32_t kGaps = 16384;        // power of 2 (one game call writes ~0.5 KB)
  static constexpr uint32_t kLimit = kGaps / 2;  // bytes written; beyond this the check is abandoned
  uint8_t* base;
  PPCContext* ctx;
  std::vector<uint32_t> addresses = std::vector<uint32_t>(kGaps, 0u);  // address + 1; 0 = free
  std::vector<uint8_t> values = std::vector<uint8_t>(kGaps, 0);
  uint32_t used = 0;
  bool mode_vector = false;

  uint32_t Gap(uint32_t d) const {
    uint32_t h = (d * 2654435761u) >> 18;  // 14 bits
    while (addresses[h] != 0 && addresses[h] != d + 1) {
      h = (h + 1) & (kGaps - 1);
    }
    return h;
  }
  bool Written(uint32_t d) const { return addresses[Gap(d)] != 0; }
  void Read(uint32_t d, uint8_t* target, uint32_t n) const {
    const uint8_t* p = Pointer(base, d);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t h = Gap(d + i);
      target[i] = addresses[h] != 0 ? values[h] : p[i];
    }
  }
  void Write(uint32_t d, const uint8_t* source, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t h = Gap(d + i);
      if (addresses[h] == 0) {
        addresses[h] = d + i + 1;
        ++used;
      }
      values[h] = source[i];
    }
  }
  uint8_t L8(uint32_t d) const {
    uint8_t b;
    Read(d, &b, 1);
    return b;
  }
  uint32_t L32(uint32_t d) const {
    uint8_t b[4];
    Read(d, b, 4);
    return ReadLarge32(b);
  }
  uint64_t L64(uint32_t d) const {
    uint8_t b[8];
    Read(d, b, 8);
    return (uint64_t(ReadLarge32(b)) << 32) | ReadLarge32(b + 4);
  }
  void Read16(uint32_t d, uint8_t* target) const { Read(d, target, 16); }
  void E32(uint32_t d, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    Write(d, b, 4);
  }
  void E64(uint32_t d, uint64_t v) {
    E32(d, uint32_t(v >> 32));
    E32(d + 4, uint32_t(v));
  }
  void Copy16(uint32_t target, uint32_t source) {
    uint8_t b[16];
    Read(source, b, 16);
    Write(target, b, 16);
  }
  void Cero128(uint32_t d) {
    const uint8_t zero[128] = {};
    Write(d, zero, 128);
  }
  void ModeVector() { mode_vector = true; }  // the original will set it; the thread is not touched here
  bool Full() const { return used > kLimit; }
};

// lvx128 exactly as the codegen translates it: 16 aligned bytes, reversed (lane 3 is word 0).
template <typename M>
inline simde__m128i LoadVector(const M& m, uint32_t address) {
  alignas(16) uint8_t raw[16];
  m.Read16(address & ~0xFu, raw);
  return simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(raw)),
                               simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL)));
}

// stvx as is: the 16 bytes as they would be on the guest stack (here, in an aligned buffer).
inline void SaveVector(uint8_t* target, simde__m128i v) {
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(target),
                       simde_mm_shuffle_epi8(v, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL))));
}

struct GroupAll {
  uint32_t count;    // address of the word count (this + 288 or + 292): reread on every word
  uint32_t dirty;    // dirty mask: this (group A) or [this+256] read on entry (group B)
  uint32_t table;     // r28 (group A) o r27 (group B)
  uint32_t lists;    // offset of list 0 in the table: 0 or 32
  uint32_t entries;  // address of the entry pointer (table + 64 or + 68): reread on every word
  uint32_t r23;
  uint32_t r24;
};

template <typename M>
struct Todo {
  M m;
  uint32_t self = 0;         // r3 on entry (r20 and r22 of the original)
  uint32_t stack = 0;         // r1 al enter
  uint32_t vulkan_device = 0;  // r21 = [this+700]
  uint64_t r3 = 0;           // what the original would leave in r3
  bool touched_device = false;
  uint32_t integers = 0;
  uint32_t booleans = 0;
  uint32_t textures = 0;
  uint32_t rare = 0;
  uint32_t several = 0;
  uint64_t vectors = 0;
};

// Lists 0 and 1: VS floating-point constants (register + 120, dirty +16) and PS ones (+376, +24).
// The same as ListFloat, entry by entry.
template <typename M>
void EntryFloat(Todo<M>& c, const GroupAll& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t field = ps ? 8 : 4;
  const uint32_t bias = ps ? 376 : 120;
  const uint32_t dirty_device = c.vulkan_device + (ps ? 24 : 16);
  const uint32_t w0 = m.L32(e);
  uint32_t wc = m.L32(e + field);
  const uint32_t t = g.r23 + (Rotl32<17>(w0) & 0x1FFF8u);
  const uint32_t idx = m.L32(t + 4);
  const uint32_t source = g.r24 + (Rotl32<4>(idx) & 0xFFFF0u);
  const uint32_t target = c.vulkan_device + ((((wc & 0x3FFu) + bias) << 4) & 0xFFFFFFF0u);
  uint32_t j = 0;
  do {
    m.Copy16((target + j * 16) & ~0xFu, (source + j * 16) & ~0xFu);  // lvx128 + stvx128: plain copy
    ++c.vectors;
    ++j;
    wc = m.L32(e + field);
  } while (j <= ((wc >> 10) & 3u));
  wc = m.L32(e + field);
  const uint64_t lo = wc & 0x3FFu;
  const uint64_t a = (lo >> 2) & 0x3FFFFFFFu;                       // r10
  const uint64_t b = ((((wc >> 10) & 3u) + lo) >> 2) & 0x3FFFFFFFu;  // r9
  const uint64_t d = (b - a) & 0xFFFFFFFFu;
  uint64_t s = d & 0x7F;
  if (s > 0x3F) {
    s = 0x3F;
  }
  const uint64_t bits = uint64_t(int64_t(uint64_t(1) << 63) >> s);  // srad r9,r18,r9
  const uint8_t a8 = uint8_t(a);
  const uint64_t v = (a8 & 0x40) ? 0 : (bits >> (a8 & 0x7F));        // srd r10,r9,r10
  m.E64(dirty_device, v | m.L64(dirty_device));
}

// Lists 2 and 3: VS and PS integer constants, with the D3D function that stores them.
template <typename M>
void EntryWhole(Todo<M>& c, const GroupAll& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t w0 = m.L32(e + 0);                                // lwz r11,0(r31)
  const uint32_t default = m.L8(e + (ps ? 12 : 13));               // lbz r10,13(r31) o 12(r31)
  const uint32_t table_default = m.L32(g.table + 80);              // lwz r9,80(r28) o 80(r27)
  const uint32_t t = (Rotl32<17>(w0) & 0x1FFF8u) + g.r23;
  const uint32_t idx = m.L32(t + 4);
  const uint32_t source = (Rotl32<4>(idx) & 0xFFFF0u) + g.r24;
  // lvx128 v0 / vctsxs v0,v0,0 / lvx128 v13 / vrlimi128 v0,v13,7,0 / stvx v0 (original's stack + 160):
  // word 0 converted and words 1-3 from the default value table, with the recompiled code's intrinsics.
  m.ModeVector();
  const simde__m128i v0 = rex::ppc::simde_mm_vctsxs(simde_mm_castsi128_ps(LoadVector(m, source)));
  const simde__m128i v13 = LoadVector(m, (default << 4) + table_default);
  alignas(16) uint8_t vector[16];
  SaveVector(vector, simde_mm_castps_si128(simde_mm_blend_ps(
                            simde_mm_castsi128_ps(v0), simde_mm_permute_ps(simde_mm_castsi128_ps(v13), 228), 7)));
  const uint32_t wc = m.L32(e + (ps ? 8 : 4));                     // lwz r11,4(r31) o 8(r31)
  const uint32_t reg_entry = Rotl32<20>(wc) & 0xFFu;                // rlwinm r4,r11,20,24,31
  const uint32_t count = (Rotl32<12>(wc) & 3u) + 1;               // rlwinm r10,r11,12,30,31; addi r6,r10,1
  // The D3D function: per register, (byte 11 << 16) | (byte 7 << 8) | byte 3 of a 16-byte vector.
  uint32_t target = ((reg_entry + (ps ? 2552u : 2536u)) << 2) + c.vulkan_device;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t b3, b7, b11;
    if (i == 0) {
      b3 = vector[3];
      b7 = vector[7];
      b11 = vector[11];
    } else {
      // Vectors 2-4: the original's stack (sp - 448 + 160 + 16 i), where it writes nothing.
      const uint32_t p = c.stack - 448 + 160 + 16 * i;
      b11 = m.L8(p + 11);
      b7 = m.L8(p + 7);
      b3 = m.L8(p + 3);
    }
    m.E32(target, (b11 << 16) | (b7 << 8) | b3);
    target += 4;
  }
  m.E64(c.vulkan_device + 32, m.L64(c.vulkan_device + 32) | 0x80000000ull);  // ld; oris r10,r10,32768; std
  c.r3 = c.vulkan_device;                                                  // mr r3,r21
  ++c.integers;
  if (count > 1) {
    ++c.several;
  }
}

// Lists 4 and 5: VS and PS boolean constants, with the D3D function that stores them.
template <typename M>
void EntryBoolean(Todo<M>& c, const GroupAll& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t w0 = m.L32(e + 0);
  const uint32_t t = (Rotl32<17>(w0) & 0x1FFF8u) + g.r23;
  const uint32_t idx = m.L32(t + 4);
  const uint32_t source = (Rotl32<4>(idx) & 0xFFFF0u) + g.r24;
  // lvx128 v0 / vctuxs v0,v0,0 / stvx v0 (the original's stack + 256).
  m.ModeVector();
  alignas(16) uint8_t vector[16];
  SaveVector(vector, rex::ppc::simde_mm_vctuxs(simde_mm_castsi128_ps(LoadVector(m, source))));
  const uint32_t wc = m.L32(e + (ps ? 8 : 4));                     // lwz r11,4(r31) o 8(r31)
  uint32_t reg_entry = m.L8(e + (ps ? 14 : 15));                    // lbz r4,15(r31) o 14(r31)
  const uint32_t count = (Rotl32<2>(wc) & 3u) + 1;                // rlwinm r11,r11,2,30,31; addi r6,r11,1
  // The D3D function: bit 0 of each word of the vector goes to bit (register & 31) of word
  // 2528 (VS) or 2532 (PS) + register / 32 of the device.
  for (uint32_t i = 0; i < count; ++i, ++reg_entry) {
    const uint32_t input_value = ReadLarge32(vector + 4 * i) & 1u;
    const uint32_t bit = reg_entry & 31u;
    const uint32_t address = (((Rotl32<27>(reg_entry) & 0x07FFFFFFu) + (ps ? 2532u : 2528u)) << 2) + c.vulkan_device;
    const uint32_t before = m.L32(address);
    m.E32(address, (before & ~(1u << bit)) | (input_value << bit));
  }
  m.E64(c.vulkan_device + 32, m.L64(c.vulkan_device + 32) | 0x80000000ull);
  c.r3 = c.vulkan_device;
  ++c.booleans;
}

// The D3D SetTexture (8258_A648) in native code, in the original's order. Returns false if the texture
// leaving the slot must be released while checking (a call cannot be undone).
template <typename M>
bool SetTextureNative(Todo<M>& c, uint32_t gap, uint32_t texture) {
  // With MemoryDirect, a local copy (two pointers nobody changes): that way the compiler knows that guest
  // writes do not touch it and keeps base in a register instead of rereading it from c.m after every write
  // (15 fewer loads per call). MemoryLayer (the check) stays by reference: it holds state.
  std::conditional_t<M::kChecking, M&, M> m = c.m;
  const uint32_t vulkan_device = c.vulkan_device;
  const uint32_t r30 = (gap + 3176) << 2;                          // addi; rlwinm r30,r11,2,0,29
  const uint32_t previous = m.L32(r30 + vulkan_device);                // lwzx r3,r30,r7
  if constexpr (!M::kChecking) {
    // Cache hints (PRFM: they neither read, write nor fault; they change neither memory, registers nor the
    // order of the real reads and writes). The texture leaving the slot is read and written at the end
    // ([previous] and [previous+4]), about 140 instructions after reading the new one: further than the A57
    // window (128), so if both missed they were paid in series. And the new object is read from +0 to +36: if
    // it crosses a cache line, the second line is requested right away. The check (MemoryLayer) does not use
    // them.
    if (previous != 0) {
      __builtin_prefetch(Pointer(m.base, previous), 1);
    }
    if (texture != 0) {
      __builtin_prefetch(Pointer(m.base, texture + 36));
    }
  }
  if (texture != 0) {
    const uint32_t common = m.L32(texture + 0);                       // lwz r10,0(r5)
    const uint32_t t16 = m.L32(texture + 16);                        // lwz r8,16(r5)
    const uint32_t fc = (gap + 48) * 24 + vulkan_device;             // texture fetch constant of the slot
    m.E32(texture + 0, common + 0x80000u);                            // stw r10,0(r5)
    const uint32_t fc0 = m.L32(fc + 0);                              // lwz r10,0(r11)
    const uint32_t fc4 = m.L32(fc + 4);                              // lwz r29,4(r11)
    uint32_t r8 = (fc0 & 0x3FFC00u) | (t16 & 0xFFC003FFu);           // rlwimi r8,r10,0,10,21
    const uint32_t fc12 = m.L32(fc + 12);                            // lwz r10,12(r11)
    const uint32_t fc16 = m.L32(fc + 16);                            // lwz r28,16(r11)
    const uint32_t fc20 = m.L32(fc + 20);                            // lwz r27,20(r11)
    m.E32(fc + 0, r8);                                               // stw r8,0(r11)
    r8 = m.L32(texture + 20);                                        // lwz r8,20(r5)
    uint32_t r6 = ((Rotl32<12>(r8) & 0xFFFu) + 512) & 0x1000u;       // rlwinm; addi; rlwinm r6,r6,0,19,19
    r8 = r6 + r8;
    r6 = fc12;                                                       // mr r6,r10
    r8 = (fc4 & 0xE0000000u) | (r8 & 0x1FFFFFFFu);                   // rlwimi r8,r29,0,0,2
    r8 = (fc4 & 0x800u) | (r8 & 0xFFFFF7FFu);                        // rlwimi r8,r29,0,20,20
    const uint32_t r29 = fc16;                                       // rotlwi r29,r28,0
    m.E32(fc + 4, r8);                                               // stw r8,4(r11)
    m.E32(fc + 8, m.L32(texture + 24));                              // lwz r8,24(r5); stw r8,8(r11)
    r8 = m.L32(texture + 28);                                        // lwz r8,28(r5)
    m.E32(fc + 16, fc16);                                            // stw r28,16(r11)
    r8 = (fc12 & 0x7FF80000u) | (r8 & 0x8007FFFFu);                  // rlwimi r8,r10,0,1,12
    uint32_t r10 = fc12;
    r10 = (Rotl32<31>(r6) & 0x0007FFFFu) | (r10 & 0xFFF80000u);      // rlwimi r10,r6,31,13,31
    r10 = (Rotl32<31>(r6) & 0x7FF00000u) | (r10 & 0x800FFFFFu);      // rlwimi r10,r6,31,1,11
    m.E32(fc + 12, r8);                                              // stw r8,12(r11)
    r6 = Rotl32<13>(r10) & 0xFFFu;                                   // rlwinm r6,r10,13,20,31
    r8 = m.L32(texture + 36);                                        // lwz r8,36(r5)
    r10 = (((Rotl32<12>(r8) & 0xFFFu) + 512) & 0x1000u) + r8;
    r10 = (fc20 & 0xE00001FFu) | (r10 & 0x1FFFFE00u);                // rlwimi r10,r27,0,23,2
    m.E32(fc + 20, r10);                                             // stw r10,20(r11)
    const uint32_t r9 = vulkan_device + gap;                         // add r9,r7,r4
    r10 = m.L8(r9 + 12098);                                          // lbz r10,12098(r9)
    const uint32_t r8b = (r10 >> 2) - 1u;                            // rlwinm r8,r10,30,2,31; addi r8,r8,-1
    r6 = r6 & r8b;                                                   // and r6,r6,r8
    r10 = (r10 & ~r8b) + r6;                                         // andc r10,r10,r8; add r10,r6,r10
    r10 = (r29 & 0xFFFFFFFCu) | (r10 & 3u);                          // rlwimi r10,r29,0,0,29
    m.E32(fc + 16, r10);                                             // stw r10,16(r11)
    const uint32_t r6b = Rotl32<30>(m.L32(texture + 32)) & 0xFu;     // lwz r6,32(r5); rlwinm r6,r6,30,28,31
    uint32_t r8c = m.L8(r9 + 12046);                                 // lbz r8,12046(r9)
    if (r6b > r8c) {                                                 // cmplw cr6,r6,r8; ble
      r8c = r6b;
    }
    r10 = (Rotl32<2>(r8c) & 0x3Cu) | (r10 & 0xFFFFFFC3u);            // rlwimi r10,r8,2,26,29
    m.E32(fc + 16, r10);                                             // stw r10,16(r11)
    uint32_t r8d = Rotl32<26>(m.L32(texture + 32)) & 0xFu;           // lwz r8,32(r5); rlwinm r8,r8,26,28,31
    const uint32_t r9b = m.L8(r9 + 12072);                           // lbz r9,12072(r9)
    if (!(r8d < r9b)) {                                              // cmplw cr6,r8,r9; blt
      r8d = r9b;
    }
    r10 = (Rotl32<6>(r8d) & 0x3C0u) | (r10 & 0xFFFFFC3Fu);           // rlwimi r10,r8,6,22,25
    m.E32(fc + 16, r10);                                             // stw r10,16(r11)
    const uint32_t h8 = gap & 0xFFu;                               // srd r11,r4,r6 con r6 = gap
    const uint64_t bit = (h8 & 0x40u) ? 0 : ((uint64_t(1) << 63) >> (h8 & 0x7Fu));
    m.E64(vulkan_device + 32, bit | m.L64(vulkan_device + 32));          // ld r10,16(r9); or; std r11,16(r9)
  }
  m.E32(r30 + vulkan_device, texture);                                 // stwx r5,r30,r7
  c.r3 = previous;
  if (previous == 0) {
    return true;
  }
  const uint32_t fence = m.L32(vulkan_device + 10396);                 // lwz r10,10396(r7)
  const uint32_t common = m.L32(previous + 0) - 0x80000u;             // lwz r11,0(r3); subf r11,r31,r11
  m.E32(previous + 4, fence);                                        // stw r10,4(r3)
  m.E32(previous + 0, common);                                        // stw r11,0(r3)
  // clrlwi r10,r11,8; rlwinm. r10,r10,0,24,12; bne: the recompiled code compares the low 32 bits.
  if ((common & 0x00FFFFFFu & 0xFFF800FFu) != 0) {
    return true;
  }
  ++c.rare;
  if constexpr (M::kChecking) {
    return false;
  } else {
    // The release is done by the game, with the stack and registers it would have: r1 in SetTexture's
    // frame (448 + 128 bytes below the entry one, with both frame back links), r3 the texture
    // and lr the return address inside SetTexture. It only reads r3 (per its PowerPC).
    PPCContext& ctx = *m.ctx;
    uint8_t* const base = m.base;
    const uint64_t r1 = ctx.r1.u64;
    Write32(base, c.stack - 448, c.stack);              // stwu r1,-448(r1) of the original
    Write32(base, c.stack - 448 - 128, c.stack - 448);  // stwu r1,-128(r1) de SetTexture
    ctx.r1.u32 = c.stack - 448 - 128;
    ctx.r3.u64 = previous;
    ctx.lr = 0x8258A7C4;                                 // the instruction after its bl
    __imp__sub_82594C70(ctx, base);
    ctx.r1.u64 = r1;
    c.r3 = ctx.r3.u64;
    return true;
  }
}

// Lists 6 and 7: textures (list 7 reads the second word of the entry, like the PS lists).
template <typename M>
bool EntryTexture(Todo<M>& c, const GroupAll& g, uint32_t e, bool ps) {
  M& m = c.m;
  const uint32_t w0 = m.L32(e + 0);                                  // lwz r11,0(r31)
  const uint32_t wc = m.L32(e + (ps ? 8 : 4));                       // lwz r10,4(r31) o 8(r31)
  const uint32_t t = (Rotl32<17>(w0) & 0x1FFF8u) + g.r23;
  const uint32_t gap = Rotl32<10>(wc) & 0xFFu;                     // rlwinm r4,r10,10,24,31
  const uint32_t idx = m.L32(t + 4);
  const uint32_t texture = m.L32((Rotl32<4>(idx) & 0xFFFF0u) + g.r24);  // lwzx r5,r11,r24
  ++c.textures;
  return SetTextureNative(c, gap, texture);
}

// One list of one group, walked like the original: for each word of the dirty mask AND the list's
// mask, each run of consecutive bits is a set of consecutive 16-byte entries.
template <uint32_t K, typename M>
bool List(Todo<M>& c, const GroupAll& g) {
  M& m = c.m;
  for (uint32_t i = 0; i < m.L32(g.count); ++i) {
    const uint32_t list = m.L32(g.table + g.lists + 4 * K);
    const uint32_t entries = m.L32(g.entries);
    uint64_t mask = m.L64(g.dirty + 8 * i) & m.L64(list + 8 * i);
    uint32_t e = entries + 1024 * i;
    while (mask != 0) {
      const uint32_t z = uint32_t(__builtin_clzll(mask));  // cntlzd
      e += z * 16;
      mask <<= z;
      const uint64_t inverted = ~mask;
      const uint32_t n = inverted ? uint32_t(__builtin_clzll(inverted)) : 64u;  // bits consecutive, 1..64
      const uint32_t fin = e + n * 16;
      mask = n >= 64 ? 0 : (mask << n);
      do {
        c.touched_device = true;
        if constexpr (K <= 1) {
          EntryFloat(c, g, e, K == 1);
        } else if constexpr (K <= 3) {
          EntryWhole(c, g, e, K == 3);
        } else if constexpr (K <= 5) {
          EntryBoolean(c, g, e, K == 5);
        } else {
          if (!EntryTexture(c, g, e, K == 7)) {
            return false;
          }
        }
        if constexpr (M::kChecking) {
          if (m.Full()) {
            return false;
          }
        }
        e += 16;
      } while (e < fin);
    }
  }
  return true;
}

template <typename M>
bool WalkGroup(Todo<M>& c, const GroupAll& g) {
  return List<0>(c, g) && List<1>(c, g) && List<2>(c, g) && List<3>(c, g) && List<4>(c, g) &&
         List<5>(c, g) && List<6>(c, g) && List<7>(c, g);
}

// The whole of sub_826992F0. With MemoryDirect it never returns false.
template <typename M>
bool EffectsAll(Todo<M>& c) {
  M& m = c.m;
  const uint32_t self = c.self;
  // What the original reads on entry and keeps in registers or on its stack.
  const uint32_t b_r24 = m.L32(m.L32(self + 300));
  const uint32_t b_r23 = m.L32(m.L32(self + 268));
  c.vulkan_device = m.L32(self + 700);
  const uint32_t table_a = m.L32(self + 536);
  const uint32_t dirty_b = m.L32(self + 256);
  const GroupAll a{self + 288, self, table_a, 0, table_a + 64, m.L32(self + 264), m.L32(self + 296)};
  if (!WalkGroup(c, a)) {
    return false;
  }
  // r27, the table of the shared group: after group A, as in the original.
  const uint32_t table_b =
      (m.L32(self + 696) != 0 && m.L32(0x828E0B40u) != 0) ? m.L32(0x8290E564u) : table_a;
  const GroupAll b{self + 292, dirty_b, table_b, 32, table_b + 68, b_r23, b_r24};
  if (!WalkGroup(c, b)) {
    return false;
  }
  m.Cero128(self & ~127u);               // dcbzl r0,r20
  m.Cero128(m.L32(self + 256) & ~127u);  // lwz r11,256(r20); dcbzl r0,r11
  return !m.Full();
}

template <typename M>
void CountAll(const Todo<M>& c) {
  Add(g_todo.native, uint64_t(1));
  Add(g_todo.vectors, c.vectors);
  if (c.integers + c.booleans + c.textures == 0) {
    return;
  }
  Add(g_todo.with_lists, uint64_t(1));
  Add(g_todo.integers, uint64_t(c.integers));
  Add(g_todo.booleans, uint64_t(c.booleans));
  Add(g_todo.textures, uint64_t(c.textures));
  Add(g_todo.rare, uint64_t(c.rare));
  Add(g_todo.several, uint64_t(c.several));
}

// Device area the original can write (constant registers < 256 of each class, texture slots < 32):
// dirty masks, texture fetch constants, floating-point, boolean and integer constants (+16..+10272),
// and the texture pointers (+12704..+12832).
constexpr uint32_t kZone1Start = 16;
constexpr uint32_t kZone1End = 10272;
constexpr uint32_t kZone2Start = 12704;
constexpr uint32_t kZone2End = 12832;

bool AllActive() {
  const bool requested = REXCVAR_GET(nfsmw_d3d_effects_native_all);
  const bool trace = REXCVAR_GET(nfsmw_d3d_trace);
  if (requested && trace) {
    REXLOG_INFO("[effects] nfsmw_d3d_trace active: sin el path complete, para que rex_d3d.log vea all "
                "the calls a SetTexture; se usa el de la build 154");
  }
  return requested && !trace;
}

void EffectsAllCall(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_all_calls.load(std::memory_order_relaxed) + 1;
  g_all_calls.store(n, std::memory_order_relaxed);
  const uint32_t checked = g_all_checked.load(std::memory_order_relaxed);
  const bool should_check = checked < kChecksAll || n % kPeriodAll == 0 ||
                         g_all_pending.load(std::memory_order_relaxed);
  if (!should_check) {
    Todo<MemoryDirect> c{MemoryDirect{base, &ctx}};
    c.self = ctx.r3.u32;
    c.stack = ctx.r1.u32;
    c.r3 = ctx.r3.u64;
    EffectsAll(c);  // with direct memory it is never abandoned
    ctx.r3.u64 = c.r3;
    CountAll(c);
    return;
  }

  // --- Guard: the native version with its writes in a separate layer, the original, and compare ---
  Todo<MemoryLayer> c{MemoryLayer{base, &ctx}};
  c.self = ctx.r3.u32;
  c.stack = ctx.r1.u32;
  c.r3 = ctx.r3.u64;
  if (!EffectsAll(c)) {
    // The SetTexture release was needed (or the layer filled up): this one is done by the original and
    // the next one is checked.
    Add(c.m.Full() ? g_todo.full : g_todo.abandoned, uint64_t(1));
    g_all_pending.store(true, std::memory_order_relaxed);
    __imp__sub_826992F0(ctx, base);
    return;
  }
  const uint32_t vulkan_device = c.vulkan_device;
  const bool zone = c.touched_device && Offset(vulkan_device) == Offset(vulkan_device + kZone2End);
  std::vector<uint8_t> before;  // the device area before the original (the native version has not touched it)
  if (zone) {
    const uint8_t* p = Pointer(base, vulkan_device);
    before.assign(p + kZone1Start, p + kZone1End);
    before.insert(before.end(), p + kZone2Start, p + kZone2End);
  }
  const uint32_t csr_before = ctx.fpscr.csr;
  const uint64_t r1_before = ctx.r1.u64;
  __imp__sub_826992F0(ctx, base);
  CountAll(c);

  const MemoryLayer& layer = c.m;
  const char* que = nullptr;
  uint32_t address = 0;
  for (uint32_t h = 0; h < MemoryLayer::kGaps && !que; ++h) {
    if (layer.addresses[h] != 0 && *Pointer(base, layer.addresses[h] - 1) != layer.values[h]) {
      que = "byte write_pos por la native";
      address = layer.addresses[h] - 1;
    }
  }
  if (!que && zone) {
    const uint8_t* p = Pointer(base, vulkan_device);
    auto look = [&](uint32_t since, uint32_t until, const uint8_t* previous) {
      for (uint32_t i = since; i < until && !que; ++i) {
        if (p[i] != previous[i - since] && !layer.Written(vulkan_device + i)) {
          que = "zone del vulkan_device written por la original y no por la native";
          address = vulkan_device + i;
        }
      }
    };
    look(kZone1Start, kZone1End, before.data());
    look(kZone2Start, kZone2End, before.data() + (kZone1End - kZone1Start));
  }
  if (!que && ctx.r3.u64 != c.r3) {
    que = "r3";
  }
  if (!que && ctx.r1.u64 != r1_before) {
    que = "r1";
  }
  const uint32_t csr_expected =
      layer.mode_vector ? (csr_before | uint32_t(PPCFPSCRRegister::FlushMask)) : csr_before;
  if (!que && ctx.fpscr.csr != csr_expected) {
    que = "mode de comma float";
  }
  if (que) {
    g_all_off.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[effects] DIFFERENCE del path complete con la original ({}) en 0x{:08X} (call {}, this "
                "0x{:08X}, vulkan_device 0x{:08X}; r3 0x{:X} native, 0x{:X} original): path complete OFF "
                "para always; sigue el de la build 154",
                que, address, n, c.self, vulkan_device, c.r3, ctx.r3.u64);
    return;
  }
  g_all_pending.store(false, std::memory_order_relaxed);
  if (c.integers + c.booleans + c.textures != 0) {
    Add(g_all_checked_lists, uint32_t(1));
  }
  g_all_checked.store(checked + 1, std::memory_order_relaxed);
  if (checked + 1 == kChecksAll) {
    REXLOG_INFO("[effects] path complete: {} calls checked contra la original byte a byte ({} con "
                "integers, booleans o textures), 0 differences; en marcha, y sigue checking 1 de every {}",
                kChecksAll, g_all_checked_lists.load(std::memory_order_relaxed), kPeriodAll);
  }
}
}  // namespace

namespace {
void EffectsCall(PPCContext& ctx, uint8_t* base) {
  static const bool active = REXCVAR_GET(nfsmw_d3d_effects_native);
  static const bool todo = AllActive();
  if (!active || g_effects_off.load(std::memory_order_relaxed)) {
    __imp__sub_826992F0(ctx, base);
    return;
  }
  // The whole function in native code, with its guard. If that guard turns it off after a difference, the
  // code below takes over: the lists 0-1 path (floating-point constants only), with its own guard.
  if (todo && !g_all_off.load(std::memory_order_relaxed)) {
    EffectsAllCall(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  uint64_t vectors = 0;
  const uint32_t checked = g_effects_checked.load(std::memory_order_relaxed);
  if (checked >= kChecks) {
    Writer w{base, nullptr};
    if (EffectsNative(w, self, vectors)) {
      Add(g_effects_native, uint64_t(1));
      Add(g_effects_vectors, vectors);
      return;  // r3 is still this, as in the original without calls
    }
    Add(g_effects_original, uint64_t(1));
    __imp__sub_826992F0(ctx, base);
    return;
  }

  // --- Guard: native recording, undo, original, compare ---
  std::vector<Undo> reg_entry;
  reg_entry.reserve(256);
  Writer w{base, &reg_entry};
  if (!EffectsNative(w, self, vectors)) {
    Add(g_effects_original, uint64_t(1));
    __imp__sub_826992F0(ctx, base);
    return;
  }
  std::vector<uint8_t> native;  // what the native version left at each recorded write
  for (const Undo& d : reg_entry) {
    const uint8_t* p = Pointer(base, d.address);
    native.insert(native.end(), p, p + d.bytes);
  }
  for (auto it = reg_entry.rbegin(); it != reg_entry.rend(); ++it) {  // undo, from last to first
    std::memcpy(Pointer(base, it->address), it->before, it->bytes);
  }
  __imp__sub_826992F0(ctx, base);
  size_t displacement = 0;
  bool equal = true;
  uint32_t address_bad = 0;
  // native[] was read after the whole native run: it is its final state at every address it touched.
  for (size_t i = 0; i < reg_entry.size() && equal; ++i) {
    const Undo& d = reg_entry[i];
    if (std::memcmp(Pointer(base, d.address), native.data() + displacement, d.bytes) != 0) {
      equal = false;
      address_bad = d.address;
    }
    displacement += d.bytes;
  }
  if (!equal) {
    g_effects_off.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[effects] DIFFERENCE con la original en 0x{:08X} (call {} de la check, this "
                "0x{:08X}): path native OFF para always; se queda la original",
                address_bad, checked + 1, self);
    return;
  }
  g_effects_checked.store(checked + 1, std::memory_order_relaxed);
  if (checked + 1 == kChecks) {
    REXLOG_INFO("[effects] {} calls checked contra la original byte a byte, 0 differences: path "
                "native en marcha",
                kChecks);
  }
}
}  // namespace

REX_HOOK_RAW(sub_826992F0) {  // upload of effect parameters to the device
  Measure(g_m_effect, [&] { EffectsCall(ctx, base); });
  if ((g_m_effect.calls.load(std::memory_order_relaxed) & 4095) == 0) {
    ReportEffects();
  }
}

// ---------------------------------------------------------------------------------------------------
// Phase 2 of the Direct3D-level renderer: the composite FlushState marker. See docs/native-renderer.md.
//
// WHAT CHANGES
//   On every draw, FlushState (825A40C0) dumps the dirty registers of the device mirror with sub_825A2AA0
//   (groups 0x2000-0x2380 and booleans), sub_825A2C58 (VS and PS constants) and sub_825A2B60 (fetch):
//   ~7 type-0 packets and ~6 padding words (type 2) per draw, measured. The PM4 ring thread, at 95 % load,
//   decodes each packet separately: ~2 us per draw on registers alone.
//   With this, FlushState writes a single type-3 NOP packet with all the runs inside (format: kMarkerMagic
//   in nfsmw_native_hooks.h), at the same place in the ring where the packets would go. The order relative
//   to the IM_LOADs, draws, resolves, fences and the Swap is the same. The data travels inside the ring and
//   not in a separate queue: that way, when the D3D replays a recorded buffer (BeginTiling/EndTiling), the
//   marker is applied again just as the type-0 packets would be, and the pace at which the game notifies
//   the ring does not change. 825A2D80 (streams) and 825A3AF0 (shaders, IM_LOAD) are still called as they
//   are, in the same order.
//
// SELF-CHECKING GUARD
//   Watching phase: the game's dumps run as always and, after their packets, a marker in check mode.
//     - Right here, on the game thread: the packets the dumps have just written must be exactly the
//       marker's runs (register, count, values and order; padding is skipped).
//     - On the PM4 ring thread, when reading the marker: the registers those packets left must hold what
//       the marker says (NoteCheckMarker). That also covers the recorded buffer replayed later.
//   With kChecksGame matches here and kChecksRing in the ring, and no mismatch, it
//   switches to applying: only the marker. Even then, 1 of every kCheckEvery FlushState calls, and those
//   that carry a group not yet checked kMinimumByGroup times, go through the check path again. A single
//   difference on either side turns it off for the rest of the session ("[d3d_marker] DIFFERENCE" in the
//   log). If the marker does not fit in the space the D3D has reserved ([dev+0] to [dev+4]), the game's
//   dumps run instead, since they know how to request space (sub_825A29E8).
// ---------------------------------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(nfsmw_d3d_marker, true, "NFSMW",
                    "Renderer native (25/09, build 170, phase 2 del renderer a level de Direct3D): FlushState "
                    "writes UN packet con all sus register_values en time de ~13 (type 0 y fill). Begins checking "
                    "contra el path del game y se apaga solo si algo difiere. false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, nfsmw_native_shadow_d3d);

REX_EXTERN(__imp__sub_825A2D80);  // streams: writes the vertex fetches to the mirror
REX_EXTERN(__imp__sub_825A3AF0);  // shaders: IM_LOAD to the ring
REX_EXTERN(__imp__sub_825A2C58);  // dump of the VS and PS constants
REX_EXTERN(__imp__sub_825A2B60);  // dump of the fetches
REX_EXTERN(__imp__sub_825A29E8);  // ring-full path of the three dumps

namespace {
namespace marker {

using nfsmw::native::kMarkerApply;
using nfsmw::native::kMarkerCheck;
using nfsmw::native::kMarkerMagic;

// The FlushState groups, in the order it dumps them.
enum Group : uint32_t { kG2300, kG2380, kGVs, kGPs, kGFetch, kGBool, kG2000, kG2100, kG2180, kG2200, kG2280, kGroups };
constexpr const char* kNameGroup[kGroups] = {"0x2300", "0x2380", "VS",     "PS",     "fetch", "bool",
                                               "0x2000", "0x2100", "0x2180", "0x2200", "0x2280"};
// Bound on runs with alternating bits in every mask: 19+4+32+32+16+1+8+11+3+6+11 = 143.
constexpr uint32_t kMaxRanges = 160;
constexpr uint64_t kChecksGame = 20000;
constexpr uint64_t kChecksRing = 20000;
constexpr uint32_t kMinimumByGroup = 64;
constexpr uint64_t kCheckEvery = 1024;  // potencia de 2
constexpr uint32_t kFill = 0x80000000u;  // type-2 packet 825A2C58 and 825A2B60 use to align their data

struct Range {
  uint32_t reg_entry;  // first reg_entry
  uint32_t count;    // words
  uint32_t source;    // mirror in the device (guest address)
};

struct DumpEntry {
  Range ranges[kMaxRanges];
  uint32_t n;         // ranges
  uint32_t words;  // run headers plus values
  uint32_t groups;    // bit g: group g has something
  bool overflowed;    // should never happen (kMaxRanges); if it does, the game's path
  uint64_t mask[kGroups];  // the one the game's dump would get (for the D3D shadow state)
  uint32_t register_base[kGroups];
  uint32_t source_base[kGroups];
};

// FlushState is called by one thread at a time (the one using the D3D); the ring counters are written only
// by the PM4 ring thread. No atomic read-modify-write (A57 without LSE).
std::atomic<bool> g_consumer{false};
std::atomic<bool> g_off{false};
std::atomic<bool> g_applying{false};
std::atomic<uint64_t> g_equal_game{0};
std::atomic<uint64_t> g_equal_ring{0};
std::atomic<bool> g_different_ring{false};
std::atomic<uint32_t> g_different_sequence{0};
std::atomic<uint32_t> g_different_register{0};
std::atomic<uint32_t> g_different_in_registers{0};
std::atomic<uint32_t> g_different_in_marker{0};
std::atomic<uint32_t> g_checked_group[kGroups];
std::atomic<uint32_t> g_groups_ready{0};  // bit g: group g was already checked kMinimumByGroup times
std::atomic<uint32_t> g_sequence{0};
std::atomic<uint64_t> g_turn{0};
std::atomic<uint64_t> g_ring_full{0};  // calls to sub_825A29E8 (ring full during a dump)
// Report every 10 s.
std::atomic<uint64_t> g_i_calls{0};
std::atomic<uint64_t> g_i_with_registers{0};
std::atomic<uint64_t> g_i_applied{0};
std::atomic<uint64_t> g_i_checked{0};
std::atomic<uint64_t> g_i_without_room{0};
std::atomic<uint64_t> g_i_no_comparable{0};
std::atomic<uint64_t> g_i_ranges{0};
std::atomic<uint64_t> g_i_words{0};
std::atomic<int64_t> g_i_next_ms{0};

inline void Write64(uint8_t* base, uint32_t address, uint64_t input_value) {
  input_value = __builtin_bswap64(input_value);
  std::memcpy(Pointer(base, address), &input_value, 8);
}

// The runs of consecutive bits of a mask, walked like the game's dumps: bit b (counting from the top) is
// register register_base + b * por_bit and its mirror starts at source_base + 4 * b * por_bit.
inline void AddRanges(DumpEntry& v, uint32_t g, uint64_t mask, uint32_t register_base, uint32_t source_base,
                         uint32_t por_bit) {
  v.groups |= 1u << g;
  v.mask[g] = mask;
  v.register_base[g] = register_base;
  v.source_base[g] = source_base;
  uint32_t bit = 0;
  while (mask != 0) {
    if (v.n == kMaxRanges) {
      v.overflowed = true;
      return;
    }
    const uint32_t z = uint32_t(__builtin_clzll(mask));
    bit += z;
    mask <<= z;  // z <= 63: the mask is not zero
    const uint64_t inverted = ~mask;
    const uint32_t n = inverted ? uint32_t(__builtin_clzll(inverted)) : 64u;
    Range& t = v.ranges[v.n++];
    t.reg_entry = register_base + bit * por_bit;
    t.count = n * por_bit;
    t.source = source_base + bit * por_bit * 4;
    v.words += 1 + t.count;
    bit += n;
    mask = n >= 64 ? 0 : (mask << n);
  }
}

// The same groups, masks and mirrors as FlushState, in its order (read from the PowerPC). The +0x30 block
// only counts if +0x30 was nonzero on entry, as in the original.
inline void Collect(uint8_t* base, uint32_t dev, bool con_30, DumpEntry& v) {
  v.n = 0;
  v.words = 0;
  v.groups = 0;
  v.overflowed = false;
  if (con_30) {
    const uint64_t m = Read64(base, dev + 0x30);
    if (const uint64_t r4 = m & 0xFFFFFFFFFC000000ull) {
      AddRanges(v, kG2300, r4, 0x2300, dev + 0x2DF8, 1);
    }
    if (uint32_t(m) & 0x03FC0000u) {
      AddRanges(v, kG2380, (m << 38) & 0xFF00000000000000ull, 0x2380, dev + 0x2E90, 1);
    }
  }
  if (const uint64_t m = Read64(base, dev + 0x10)) {
    AddRanges(v, kGVs, m, 0x4000, dev + 0x780, 16);
  }
  if (const uint64_t m = Read64(base, dev + 0x18)) {
    AddRanges(v, kGPs, m, 0x4400, dev + 0x1780, 16);
  }
  if (const uint64_t m = Read64(base, dev + 0x20)) {
    if (const uint64_t r4 = m & 0xFFFFFFFF00000000ull) {
      AddRanges(v, kGFetch, r4, 0x4800, dev + 0x480, 6);
    }
    if (uint32_t(m) & 0x80000000u) {
      AddRanges(v, kGBool, 0xFFFFFFFFFF000000ull, 0x4900, dev + 0x2780, 1);
    }
    if (uint32_t(m) & 0x3FFFC000u) {
      AddRanges(v, kG2000, (m << 34) & 0xFFFF000000000000ull, 0x2000, dev + 0x2CC0, 1);
    }
  }
  if (const uint64_t m = Read64(base, dev + 0x28)) {
    if (const uint64_t r4 = m & 0xFFFFF80000000000ull) {
      AddRanges(v, kG2100, r4, 0x2100, dev + 0x2D0C, 1);
    }
    if (m & 0x000007C000000000ull) {
      AddRanges(v, kG2180, (m << 21) & 0xF800000000000000ull, 0x2180, dev + 0x2D60, 1);
    }
    if (m & 0x0000003FFC000000ull) {
      AddRanges(v, kG2200, (m << 26) & 0xFFF0000000000000ull, 0x2200, dev + 0x2D74, 1);
    }
    if (uint32_t(m) & 0x03FFFFE0u) {
      AddRanges(v, kG2280, (m << 38) & 0xFFFFF80000000000ull, 0x2280, dev + 0x2DA4, 1);
    }
  }
}

// What FlushState leaves in the device besides the packets: the masks it looked at, set to zero, and, if
// it dumps the booleans, 0xFFFFFFFFFF000000 at +0x2CB0.
inline void CleanAsFlushState(uint8_t* base, uint32_t dev, bool con_30, const DumpEntry& v) {
  if (con_30) {
    Write64(base, dev + 0x30, 0);
  }
  for (const uint32_t field : {0x10u, 0x18u, 0x20u, 0x28u}) {
    if (Read64(base, dev + field) != 0) {
      Write64(base, dev + field, 0);
    }
  }
  if (v.groups & (1u << kGBool)) {
    Write64(base, dev + 0x2CB0, 0xFFFFFFFFFF000000ull);
  }
}

// Phase 2b: the kWordsDraw words of the Draw* record: function, VS, PS, the four arguments and the
// snapshot of the D3D shadow state (high and low). Read by ReadDrawOfMarker in nfsmw_native_system.cpp.
inline void WriteDraw(uint8_t* base, uint32_t p, const nfsmw::native::RegisterDraw& r) {
  // Bits 8-23 carry the game's vegetation verdict (nfsmw_d3d_game_vegetation).
  Write32(base, p, uint32_t(r.function) | (uint32_t(r.vegetation) << 8));
  Write32(base, p + 4, r.vs);
  Write32(base, p + 8, r.ps);
  for (uint32_t i = 0; i < 4; ++i) {
    Write32(base, p + 12 + 4 * i, r.args[i]);
  }
  Write32(base, p + 28, uint32_t(r.shadow >> 32));
  Write32(base, p + 32, uint32_t(r.shadow));
}

// The marker, if it fits entirely in the space the D3D has reserved: it is written from [dev+0]+4 and the
// last word can be [dev+4], the same criterion as the dumps (write + 4 * words < end). [dev+0] is left at
// the last one.
// Phase 2b: with mode_draw, the Draw* record goes before the runs; with no dump (v null, mode 0), a
// marker with only the record.
inline bool WriteMarker(uint8_t* base, uint32_t dev, const DumpEntry* v, uint32_t mode, uint32_t sequence,
                             uint32_t mode_draw, const nfsmw::native::RegisterDraw* draw) {
  const uint32_t words_draw = mode_draw ? nfsmw::native::kWordsDraw : 0u;
  const uint32_t load = 2 + words_draw + (v ? v->words : 0u);  // words after the PM4 header
  if (load > 0x4000) {
    return false;
  }
  const uint32_t write = Read32(base, dev + 0);
  const uint32_t fin = Read32(base, dev + 4);
  if (uint64_t(write) + uint64_t(load) * 4 >= uint64_t(fin)) {
    return false;
  }
  uint32_t p = write + 4;
  Write32(base, p, 0xC0001000u | ((load - 1) << 16));
  Write32(base, p + 4, kMarkerMagic | mode | (mode_draw << 4));
  Write32(base, p + 8, sequence);
  p += 12;
  if (mode_draw) {
    WriteDraw(base, p, *draw);
    p += words_draw * 4;
  }
  for (uint32_t i = 0; v && i < v->n; ++i) {
    const Range& t = v->ranges[i];
    Write32(base, p, (t.count << 16) | t.reg_entry);
    CopyWords(base, p + 4, t.source, t.count);
    p += 4 + t.count * 4;
  }
  Write32(base, dev + 0, p - 4);
  return true;
}

struct Difference {
  const char* que = "";
  uint32_t range = 0;
  uint32_t reg_entry = 0;
  uint32_t expected = 0;
  uint32_t seen = 0;
};

// Watching phase: what the game's dumps have just written between since+4 and until (inclusive) must be
// exactly the list of runs: a type-0 header for each run with its register and count, and its values,
// which are those of the mirror. The alignment padding (type 2) is skipped.
inline bool MatchPackets(uint8_t* base, uint32_t since, uint32_t until, const DumpEntry& v, Difference& d) {
  uint32_t p = since + 4;
  const uint32_t fin = until + 4;
  for (uint32_t i = 0; i < v.n; ++i) {
    const Range& t = v.ranges[i];
    for (uint32_t fill = 0; fill < 3 && p < fin && Read32(base, p) == kFill; ++fill) {
      p += 4;
    }
    const uint32_t header = ((t.count - 1) << 16) | t.reg_entry;
    if (p >= fin || Read32(base, p) != header) {
      d = Difference{"header", i, t.reg_entry, header, p < fin ? Read32(base, p) : 0};
      return false;
    }
    p += 4;
    if ((fin - p) / 4 < t.count) {
      d = Difference{"packet short", i, t.reg_entry, t.count, (fin - p) / 4};
      return false;
    }
    for (uint32_t k = 0; k < t.count; ++k) {
      const uint32_t seen = Read32(base, p + 4 * k);
      const uint32_t expected = Read32(base, t.source + 4 * k);
      if (seen != expected) {
        d = Difference{"input_value", i, t.reg_entry + k, expected, seen};
        return false;
      }
    }
    p += 4 * t.count;
  }
  if (p != fin) {
    d = Difference{"spare_2 words", v.n, 0, fin, p};
    return false;
  }
  return true;
}

// FlushState's 112-byte frame, opened only when game code has to be called, like the original:
// mflr r12; stw r12,-8(r1); std r30,-24(r1); std r31,-16(r1); stwu r1,-112(r1). With the non-volatile
// registers as locals of the generated code, r30 and r31 are saved as zero.
struct Frame {
  PPCContext& ctx;
  uint8_t* base;
  uint64_t lr;
  uint32_t stack = 0;
  bool open = false;
  Frame(PPCContext& c, uint8_t* b) : ctx(c), base(b), lr(c.lr) {}
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  void Open() {
    if (open) {
      return;
    }
    open = true;
    stack = ctx.r1.u32;
    Write32(base, stack - 8, uint32_t(lr));
    Write64(base, stack - 24, 0);
    Write64(base, stack - 16, 0);
    Write32(base, stack - 112, stack);
    ctx.r1.u64 = stack - 112;
  }
  ~Frame() {
    if (open) {
      ctx.r1.u64 = stack;
    }
  }
};

inline void Arguments(PPCContext& ctx, uint32_t dev, uint64_t mask, uint32_t reg_entry, uint32_t source,
                       uint32_t lap) {
  ctx.r3.u64 = dev;
  ctx.r4.u64 = mask;
  ctx.r5.u64 = reg_entry;
  ctx.r6.u64 = source;
  ctx.lr = lap;
}

// The end of FlushState as is (from loc_825A4110): the game's dumps, in their order, with their arguments,
// their return addresses and the masks zeroed after each group.
void DumpAsTheGame(PPCContext& ctx, uint8_t* base, Frame& frame, uint32_t dev, bool con_30) {
  if (con_30) {
    uint64_t m = Read64(base, dev + 0x30);
    if (const uint64_t r4 = m & 0xFFFFFFFFFC000000ull) {
      frame.Open();
      Arguments(ctx, dev, r4, 0x2300, dev + 0x2DF8, 0x825A4130);
      sub_825A2AA0(ctx, base);
    }
    m = Read64(base, dev + 0x30);
    if (uint32_t(m) & 0x03FC0000u) {
      frame.Open();
      Arguments(ctx, dev, (m << 38) & 0xFF00000000000000ull, 0x2380, dev + 0x2E90, 0x825A4154);
      sub_825A2AA0(ctx, base);
    }
    Write64(base, dev + 0x30, 0);
  }
  if (const uint64_t m = Read64(base, dev + 0x10)) {
    frame.Open();
    Arguments(ctx, dev, m, 0x4000, dev + 0x780, 0x825A4178);
    __imp__sub_825A2C58(ctx, base);
    Write64(base, dev + 0x10, 0);
  }
  if (const uint64_t m = Read64(base, dev + 0x18)) {
    frame.Open();
    Arguments(ctx, dev, m, 0x4400, dev + 0x1780, 0x825A419C);
    __imp__sub_825A2C58(ctx, base);
    Write64(base, dev + 0x18, 0);
  }
  if (Read64(base, dev + 0x20) != 0) {
    uint64_t m = Read64(base, dev + 0x20);
    if (const uint64_t r4 = m & 0xFFFFFFFF00000000ull) {
      frame.Open();
      Arguments(ctx, dev, r4, 0x4800, dev + 0x480, 0x825A41C8);
      __imp__sub_825A2B60(ctx, base);
    }
    m = Read64(base, dev + 0x20);
    if (uint32_t(m) & 0x80000000u) {
      frame.Open();
      Write64(base, dev + 0x2CB0, 0xFFFFFFFFFF000000ull);
      Arguments(ctx, dev, 0xFFFFFFFFFF000000ull, 0x4900, dev + 0x2780, 0x825A41F4);
      sub_825A2AA0(ctx, base);
    }
    m = Read64(base, dev + 0x20);
    if (uint32_t(m) & 0x3FFFC000u) {
      frame.Open();
      Arguments(ctx, dev, (m << 34) & 0xFFFF000000000000ull, 0x2000, dev + 0x2CC0, 0x825A4218);
      sub_825A2AA0(ctx, base);
    }
    Write64(base, dev + 0x20, 0);
  }
  if (Read64(base, dev + 0x28) != 0) {
    uint64_t m = Read64(base, dev + 0x28);
    if (const uint64_t r4 = m & 0xFFFFF80000000000ull) {
      frame.Open();
      Arguments(ctx, dev, r4, 0x2100, dev + 0x2D0C, 0x825A4244);
      sub_825A2AA0(ctx, base);
    }
    m = Read64(base, dev + 0x28);
    if (m & 0x000007C000000000ull) {
      frame.Open();
      Arguments(ctx, dev, (m << 21) & 0xF800000000000000ull, 0x2180, dev + 0x2D60, 0x825A4270);
      sub_825A2AA0(ctx, base);
    }
    m = Read64(base, dev + 0x28);
    if (m & 0x0000003FFC000000ull) {
      frame.Open();
      Arguments(ctx, dev, (m << 26) & 0xFFF0000000000000ull, 0x2200, dev + 0x2D74, 0x825A429C);
      sub_825A2AA0(ctx, base);
    }
    m = Read64(base, dev + 0x28);
    if (uint32_t(m) & 0x03FFFFE0u) {
      frame.Open();
      Arguments(ctx, dev, (m << 38) & 0xFFFFF80000000000ull, 0x2280, dev + 0x2DA4, 0x825A42C0);
      sub_825A2AA0(ctx, base);
    }
    Write64(base, dev + 0x28, 0);
  }
}

void NoteEqualGame(const DumpEntry& v) {
  Add(g_equal_game, uint64_t(1));
  uint32_t ready = g_groups_ready.load(std::memory_order_relaxed);
  for (uint32_t g = 0; g < kGroups; ++g) {
    if ((v.groups >> g) & 1) {
      const uint32_t times = g_checked_group[g].load(std::memory_order_relaxed) + 1;
      g_checked_group[g].store(times, std::memory_order_relaxed);
      if (times >= kMinimumByGroup) {
        ready |= 1u << g;
      }
    }
  }
  g_groups_ready.store(ready, std::memory_order_relaxed);
}

uint32_t DecideMode(uint32_t groups) {
  if (!g_applying.load(std::memory_order_relaxed)) {
    const uint64_t game = g_equal_game.load(std::memory_order_relaxed);
    const uint64_t ring = g_equal_ring.load(std::memory_order_relaxed);
    if (game < kChecksGame || ring < kChecksRing) {
      return kMarkerCheck;
    }
    g_applying.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marker] {} dumps checked en el thread_value del game y {} en el del ring, 0 differences: "
                "FlushState writes ya solo el marker (1 de every {} se sigue checking)",
                game, ring, kCheckEvery);
  }
  if ((groups & ~g_groups_ready.load(std::memory_order_relaxed)) != 0) {
    return kMarkerCheck;  // a group that has not been checked enough times yet
  }
  const uint64_t turn = g_turn.load(std::memory_order_relaxed) + 1;
  g_turn.store(turn, std::memory_order_relaxed);
  return (turn & (kCheckEvery - 1)) == 0 ? kMarkerCheck : kMarkerApply;
}

// The group of a register, for the log.
const char* NameGroup(uint32_t reg_entry) {
  if (reg_entry >= 0x4000) {
    return kNameGroup[reg_entry >= 0x4900 ? kGBool : reg_entry >= 0x4800 ? kGFetch : reg_entry >= 0x4400 ? kGPs : kGVs];
  }
  switch (reg_entry & ~0x7Fu) {
    case 0x2000:
      return kNameGroup[kG2000];
    case 0x2100:
      return kNameGroup[kG2100];
    case 0x2180:
      return kNameGroup[kG2180];
    case 0x2200:
      return kNameGroup[kG2200];
    case 0x2280:
      return kNameGroup[kG2280];
    case 0x2300:
      return kNameGroup[kG2300];
    case 0x2380:
      return kNameGroup[kG2380];
    default:
      return "?";
  }
}

void Turn_off(const char* side, const char* que, uint32_t sequence, uint32_t reg_entry, uint32_t expected,
            uint32_t seen) {
  if (g_off.load(std::memory_order_relaxed)) {
    return;
  }
  g_off.store(true, std::memory_order_relaxed);
  REXLOG_ERROR("[d3d_marker] DIFFERENCE en el {} ({}): marker {}, group {}, reg_entry {:04X}, expected {:08X}, "
               "seen {:08X}. Path del marker OFF para el rest de la session: FlushState vuelve al del game",
               side, que, sequence, NameGroup(reg_entry), reg_entry, expected, seen);
}

void ReportMarker() {
  const int64_t now = NowNs() / 1000000;
  const int64_t next = g_i_next_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_i_next_ms.store(now + 10000, std::memory_order_relaxed);
  if (next == 0) {
    REXLOG_INFO("[d3d_marker] marker compuesto de FlushState active (phase 2 del renderer a level de "
                "Direct3D): begins checking");
    return;
  }
  const uint64_t calls = g_i_calls.exchange(0, std::memory_order_relaxed);
  const uint64_t with_registers = g_i_with_registers.exchange(0, std::memory_order_relaxed);
  const uint64_t applied = g_i_applied.exchange(0, std::memory_order_relaxed);
  const uint64_t ranges = g_i_ranges.exchange(0, std::memory_order_relaxed);
  const uint64_t words = g_i_words.exchange(0, std::memory_order_relaxed);
  uint32_t ready = g_groups_ready.load(std::memory_order_relaxed);
  uint32_t n_ready = 0;
  for (; ready; ready &= ready - 1) {
    ++n_ready;
  }
  NFSMW_REPORT_DEFERRED("[d3d_marker] last 10 s: {} FlushState ({} con register_values): {} solo con marker ({:.1f} ranges y "
              "{:.1f} words every uno), {} checked contra el game, {} sin room en el ring, {} no comparable "
              "| phase {} | checked: game {} de {}, ring {} de {}, groups ready {} de {}",
              calls, with_registers, applied, applied ? double(ranges) / double(applied) : 0.0,
              applied ? double(words) / double(applied) : 0.0,
              g_i_checked.exchange(0, std::memory_order_relaxed),
              g_i_without_room.exchange(0, std::memory_order_relaxed),
              g_i_no_comparable.exchange(0, std::memory_order_relaxed),
              g_off.load(std::memory_order_relaxed) ? "OFF"
              : g_applying.load(std::memory_order_relaxed) ? "applying"
                                                             : "watching",
              g_equal_game.load(std::memory_order_relaxed), kChecksGame,
              g_equal_ring.load(std::memory_order_relaxed), kChecksRing, n_ready, uint32_t(kGroups));
}

}  // namespace marker
}  // namespace

namespace nfsmw::native {

void ActivateConsumerMarkers(bool active) {
  marker::g_consumer.store(active, std::memory_order_release);
}

void NoteCheckMarker(bool equal, uint32_t sequence, uint32_t reg_entry, uint32_t in_registers,
                                uint32_t in_marker) {
  if (equal) {
    Add(marker::g_equal_ring, uint64_t(1));
    return;
  }
  if (marker::g_different_ring.load(std::memory_order_relaxed)) {
    return;
  }
  marker::g_different_sequence.store(sequence, std::memory_order_relaxed);
  marker::g_different_register.store(reg_entry, std::memory_order_relaxed);
  marker::g_different_in_registers.store(in_registers, std::memory_order_relaxed);
  marker::g_different_in_marker.store(in_marker, std::memory_order_relaxed);
  marker::g_different_ring.store(true, std::memory_order_release);
}

}  // namespace nfsmw::native

// Ring-full path of the dumps: nothing changes, it is only counted. With the ring full, a dump's packets
// end up split between two stretches of the ring and the watching phase cannot compare them.
REX_HOOK_RAW(sub_825A29E8) {
  Add(marker::g_ring_full, uint64_t(1));
  __imp__sub_825A29E8(ctx, base);
}

// From the FlushState hook (nfsmw_d3d_trace.cpp). false = nothing touched: let the original run.
bool NfsmwFlushStateMarker(PPCContext& ctx, uint8_t* base) {
  using namespace marker;
  // Phase 2b: the record of the Draw* that called this FlushState, if its hook left it pending. It leaves
  // from here in the marker or through the queue, before that Draw* writes its DRAW_INDX.
  nfsmw::native::RegisterDraw draw;
  const bool with_draw = nfsmw::native::TakeDrawInProgress(draw);
  static const bool active = REXCVAR_GET(nfsmw_d3d_marker);
  if (!active || !g_consumer.load(std::memory_order_relaxed) || g_off.load(std::memory_order_relaxed)) {
    if (with_draw) {
      nfsmw::native::DeliverDraw(draw, 0, false);
    }
    return false;
  }
  if (g_different_ring.load(std::memory_order_acquire)) {
    Turn_off("thread_value del ring", "register_values after los packets", g_different_sequence.load(std::memory_order_relaxed),
           g_different_register.load(std::memory_order_relaxed),
           g_different_in_marker.load(std::memory_order_relaxed),
           g_different_in_registers.load(std::memory_order_relaxed));
    if (with_draw) {
      nfsmw::native::DeliverDraw(draw, 0, false);
    }
    return false;
  }
  const uint32_t dev = ctx.r3.u32;
  const uint64_t lr = ctx.lr;
  Frame frame(ctx, base);
  // 1. Streams and shaders: the game's code, in the same order and with the same conditions as FlushState.
  const uint64_t m30 = Read64(base, dev + 0x30);
  if (m30 != 0) {
    if (m30 & 0x400) {
      frame.Open();
      ctx.r3.u64 = dev;
      ctx.lr = 0x825A40F8;
      __imp__sub_825A2D80(ctx, base);
    }
    if (Read64(base, dev + 0x30) & 0x1E0) {
      frame.Open();
      ctx.r3.u64 = dev;
      ctx.lr = 0x825A4110;
      __imp__sub_825A3AF0(ctx, base);
    }
  }
  // 2. The runs of all the dumps, with the masks left by streams and shaders.
  DumpEntry v;
  Collect(base, dev, m30 != 0, v);
  const uint32_t sequence = g_sequence.load(std::memory_order_relaxed) + 1;
  g_sequence.store(sequence, std::memory_order_relaxed);
  const uint32_t mode = (v.n == 0 || v.overflowed) ? 0u : DecideMode(v.groups);
  const uint32_t mode_draw = with_draw ? nfsmw::native::DecideModeDraw() : 0u;  // phase 2b
  bool draw_in_marker = false;
  const uint64_t calls = g_i_calls.load(std::memory_order_relaxed) + 1;
  g_i_calls.store(calls, std::memory_order_relaxed);
  Add(g_i_with_registers, uint64_t(v.n != 0));
  if (mode == kMarkerApply && WriteMarker(base, dev, &v, kMarkerApply, sequence, mode_draw, &draw)) {
    // 3a. Only the marker.
    draw_in_marker = mode_draw != 0;
    CleanAsFlushState(base, dev, m30 != 0, v);
    static const bool shadow = REXCVAR_GET(nfsmw_native_shadow_d3d);
    if (shadow) {  // what the 825A2AA0 hook would do per group (phase 1, as a shadow)
      for (uint32_t g = 0; g < kGroups; ++g) {
        if ((v.groups >> g) & 1) {
          nfsmw::native::LearnGroupMirror(v.register_base[g], v.mask[g], v.source_base[g] - dev);
        }
      }
    }
    ctx.r3.u64 = dev;
    Add(g_i_applied, uint64_t(1));
    Add(g_i_ranges, uint64_t(v.n));
    Add(g_i_words, uint64_t(v.words - v.n));
  } else {
    // 3b. The game's dumps and, in check mode, the comparison and the check marker after them.
    if (mode == kMarkerApply) {
      Add(g_i_without_room, uint64_t(1));
    }
    const uint32_t since = Read32(base, dev + 0);
    const uint64_t slow = g_ring_full.load(std::memory_order_relaxed);
    DumpAsTheGame(ctx, base, frame, dev, m30 != 0);
    if (mode == kMarkerCheck) {
      const uint32_t until = Read32(base, dev + 0);
      Difference d;
      if (g_ring_full.load(std::memory_order_relaxed) != slow || until < since) {
        Add(g_i_no_comparable, uint64_t(1));
      } else if (!MatchPackets(base, since, until, v, d)) {
        Turn_off("thread_value del game", d.que, sequence, d.reg_entry, d.expected, d.seen);
      } else {
        NoteEqualGame(v);
        if (WriteMarker(base, dev, &v, kMarkerCheck, sequence, mode_draw, &draw)) {
          draw_in_marker = mode_draw != 0;
          Add(g_i_checked, uint64_t(1));
        } else {
          Add(g_i_without_room, uint64_t(1));
        }
      }
    }
  }
  // Phase 2b: if no register marker has taken the draw (nothing to dump, no space, ring full), one with
  // only the draw; and to the queue whatever has to go there (failure, or check mode).
  if (mode_draw != 0 && !draw_in_marker) {
    draw_in_marker = WriteMarker(base, dev, nullptr, 0, sequence, mode_draw, &draw);
  }
  if (with_draw) {
    nfsmw::native::DeliverDraw(draw, mode_draw, draw_in_marker);
  }
  // Like the original's epilogue: lwz r12,-8(r1); mtlr r12.
  ctx.lr = lr;
  ctx.r12.u64 = uint32_t(lr);
  if ((calls & 4095) == 0) {
    ReportMarker();
  }
  return true;
}

