// nfsmw - ScenerySectionHeader::DrawAScenery (sub_824C2850) in native code.
//
// WHAT IT IS (PowerPC read instruction by instruction in nfsmw_recomp.70.cpp:18450-19374)
//   Called by the thread that prepares the frames (XThreadA6918080 in one console log; not the Main
//   XThread, which runs them) from TreeCull (sub_824C2F48), sub_824C3078 and sub_824C2EE8: once per
//   candidate scenery object of each view. In a race, 87,500 calls per second on average and 124,000 in the
//   alley. r3 = ScenerySectionHeader, r4 = instance number, r5 = SceneryCullInfo, r6 = visibility state
//   (1 = partial). 304-byte frame.
//   1. The instance: 64 bytes at [r3+32] + 64 * r4. If the view has a preculler section ([r5+196] >= 0)
//      and the instance's bit in the table [r3+48] says "not visible", out.
//   2. The instance's ExcludeFlags (+24) against the view's (+132), mask 0x080000FF (with 0x08000040 if
//      the instance carries 0x08000000 or 0x40): if any match, out.
//   3. Partial visibility: the instance's box at r1+80 and r1+112 and GetVisibleState (sub_8243E7D8,
//      through its hook).
//   4. InlinedViewGetPixelSize (sub_824C2720, a single caller: inlined here): the distance to the camera,
//      written to r1+80, and the size in pixels (fctiwz, which goes through r1-16).
//   5. The mesh: threshold of 32 px (23 in view 20 with 0x1000) in views with 0x800 or 0x1000, 32 with
//      0x20, 23 with view mode >= 3 and 18 in the normal camera, where LOD decides: distance < K
//      ([0x82062AC8]) or pixels / max(Density, 6) >= 8.7 -> the full one ([info+40]); otherwise the
//      reduced one ([info+48]).
//   6. With bit 0x200 of the instance, a SceneryDrawInfo without a matrix. Otherwise eFrameMallocMatrix
//      (820E_A568), the instance's rotation (8243_F8D8: 9 16-bit integers times 1/8192, inlined), the
//      position and, depending on the view's bits, the extra height for shadows (0x100), the mirror
//      (0x800) and the vegetation wind (0x4000: CreateWindRotMatrix 824F_D7C0 and bMulMatrix 8263_D020,
//      which are called as they are). In views 1 and 2 it looks for position markers in the eSolid for
//      the light flares (8221_9EA8).
//
// WHY NATIVE
//   It is the costliest game function left on the thread that prepares the frames: 5.5 % of a core
//   including its frameless leaves in stack sampling, and 72.6 ms/s on average in a race (0.83 us per
//   call, inclusive) with peaks of 109 ms/s. That thread is on the critical path: in the alley measurement
//   window the Main XThread spends 1,606 ms out of every 10 s waiting for commands the preparer has not
//   written yet, and in 3 of the 7 race stutters the one running late is the preparer. Recompiled, every
//   instruction goes through the context and through volatile loads and stores; the rotation, the
//   allocation and the pixel size are three more calls. Here everything stays in registers and only
//   GetVisibleState and, with wind, the two wind functions are called.
//
// WHY IT IS BIT-IDENTICAL
//   - Floating point: the same computation, operation by operation and with the same expression as the
//     generated code (double(float(a op b)) for fadds/fsubs/fmuls/fdivs, std::fma for fmadds, sqrt for
//     fsqrts and the same fctiwz expression). Between product and sum there is always an explicit float()
//     or std::fma, so GCC's FMA contraction cannot change anything. With any NaN in a floating-point
//     input (box, position, camera, radius, Density and the constants) the original runs: with two NaNs,
//     which one propagates depends on the operand order the compiler picks. Without NaN in the input,
//     lfs + stfs is a copy of the word.
//   - Denormal mode: the original's, without flush (disableFlushMode) throughout the scalar part, with a
//     compiler barrier after the change so that no conversion is placed before it; it exits without
//     flush, like the original.
//   - Memory: the same writes with the same bytes: the prologue (three doubles set to zero, which are
//     local variables in the generated code, and the frame back link), the box, the distance, the fctiwz
//     at r1-16, the std of the size, the std of the rotation at r1-32, -24 and -16, the matrix allocator
//     (0x82A2C3B4 and, when out of space, 0x82A2C3C4/C8), the matrix, the SceneryDrawInfo and [cull+140].
//   - Registers: the three callers (TreeCull, sub_824C3078 and sub_824C2EE8) only read local variables
//     after the call, and through their return r3 and f1 stay live (the liveness analysis over all the
//     generated code gives the same after GetVisibleState in those functions). r3, f1, r1, r12 and lr are
//     left as the original leaves them; GetVisibleState and the wind functions touch them themselves.
//     cr6, xer, r22-r31 and f29-f31 are local variables of the generated code.
//   - The flares with position markers (views 1 and 2) and a nonzero nfsmw_scenery_detail go to the
//     whole original, which calls its own hooks. This is decided before writing anything outside the
//     stack.
//
// SELF-CHECKING GUARD (cvar nfsmw_scenery_native; project rule)
//   The first kChecks calls, the first kMinimumWind of the wind path and then 1 of every
//   kPeriod: the stack (kStack bytes below r1), [cull+140], the free SceneryDrawInfo, the matrix allocator
//   and the block that would be allocated are snapshotted; the native version runs, what it leaves is
//   saved and undone (whole memory and context), the original runs and they are compared byte by byte,
//   along with r3, f1, r1, r12, lr and the FPCR. The original's state is always kept. A single difference
//   turns the native version off for the session and writes "[scenery] DIFFERENCE" (REXLOG_ERROR).
//   "[scenery]" line every 10 s with the counts.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_scenery_native, true, "NFSMW",
                    "ScenerySectionHeader::DrawAScenery (sub_824C2850: culling, size en pixels, LOD y SceneryDrawInfo "
                    "de every object de scenery) en native (build 184), identico bit a bit. Se comprueba contra la "
                    "original (the primeras 100.000 calls, the primeras 5.000 con wind y after 1 de every 4096) "
                    "y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The LOD setting of nfsmw_scenery_lod.cpp (its hook of sub_824C2720). Nonzero: the original runs, since
// it uses it.
REXCVAR_DECLARE(int32_t, nfsmw_scenery_detail);

REX_EXTERN(__imp__sub_824C2850);  // la original
REX_EXTERN(sub_8243E7D8);         // GetVisibleState through its hook, like the original (itself native)
// CreateWindRotMatrix and bMulMatrix, the originals. The name is assembled from parts on purpose:
// tools/direct_calls.py treats any address that appears whole in app/src as hooked, and their calls
// from the generated code must remain direct (__imp__, inlinable).
#define NFSMW_SCENERY_JOIN_(a, b) a##b
#define NFSMW_SCENERY_WIND NFSMW_SCENERY_JOIN_(__imp__sub_824F, D7C0)
#define NFSMW_SCENERY_MULTIPLY NFSMW_SCENERY_JOIN_(__imp__sub_8263, D020)
REX_EXTERN(NFSMW_SCENERY_WIND);
REX_EXTERN(NFSMW_SCENERY_MULTIPLY);

namespace nfsmw::scenery_native {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: the same translation as REX_RAW_ADDR / REX_LOAD / REX_STORE in nfsmw_pch.h.
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline uint32_t Offset(uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}
[[gnu::always_inline]] inline uint8_t* Pointer(uint8_t* base, uint32_t address) {
  return base + address + Offset(address);
}
[[gnu::always_inline]] inline uint8_t Read8(uint8_t* base, uint32_t address) {
  return *Pointer(base, address);
}
[[gnu::always_inline]] inline uint16_t Read16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, Pointer(base, address), 2);
  return __builtin_bswap16(v);
}
[[gnu::always_inline]] inline uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Pointer(base, address), 4);
  return __builtin_bswap32(v);
}
[[gnu::always_inline]] inline void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  input_value = __builtin_bswap32(input_value);
  std::memcpy(Pointer(base, address), &input_value, 4);
}
[[gnu::always_inline]] inline void Write64(uint8_t* base, uint32_t address, uint64_t input_value) {
  input_value = __builtin_bswap64(input_value);
  std::memcpy(Pointer(base, address), &input_value, 8);
}

// lfs: the guest word as a float, widened to double, like "double(temp.f32)" in the generated code.
[[gnu::always_inline]] inline double Lfs(uint32_t word) {
  float f;
  std::memcpy(&f, &word, 4);
  return double(f);
}
// stfs: the double to float, like "temp.f32 = float(ctx.fN.f64)".
[[gnu::always_inline]] inline uint32_t Stfs(double d) {
  const float f = float(d);
  uint32_t word;
  std::memcpy(&word, &f, 4);
  return word;
}
[[gnu::always_inline]] inline bool EsNaN(uint32_t word) {
  return (word & 0x7FFFFFFFu) > 0x7F800000u;
}
// fctiwz, with the same expression as the generated code.
[[gnu::always_inline]] inline int64_t Fctiwz(double v) {
  return std::isnan(v) ? int64_t(0x80000000U) : (v >= double(INT_MAX)) ? INT_MAX : simde_mm_cvttsd_si32(simde_mm_load_sd(&v));
}
// Compiler barrier: no memory reads or writes (nor anything that depends on them) cross an FPCR change.
[[gnu::always_inline]] inline void Barrier() {
  __asm__ __volatile__("" ::: "memory");
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset) and return addresses of each bl (ctx.lr in the generated code).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kFrame = 304;                   // stwu r1,-304(r1)
constexpr uint32_t kStack = 1536;                   // what is watched below r1: this frame, GetVisibleState and the wind
constexpr uint32_t kSix = 0x82060E70;             // lfs f31,-3704(r27): 6.0, the Density minimum
constexpr uint32_t kDistanceK = 0x82062AC8;       // lfs f13,3552(r27): K, 25.0 as measured on the console
constexpr uint32_t kThresholdLod = 0x82060BBC;        // lfs f13,-4396(r27): 8,7
constexpr uint32_t kUno = 0x82063038;              // lfs f29,4944(r27): the w of the position
constexpr uint32_t kHeightExtra = 0x82063970;      // lfs f0,7304(r27): EnvMapShadowExtraHeight
constexpr uint32_t kSixty = 0x82057114;          // lfs f0,28948(0x82050000): the 60 of the wind
constexpr uint32_t kCero = 0x82061CE8;             // lfs f13,7400(0x82060000) of the rotation: the w column
constexpr uint32_t kScaleRotation = 0x820AFF50;   // lfs f0,-176(0x820B0000) of the rotation: 1/8192
constexpr uint32_t kModeView = 0x82A2CEE4;        // lwz r11,-12572(0x82A30000): eGetCurrentViewMode()
constexpr uint32_t kReserveCurrent = 0x82A2C3B4;    // eFrameMalloc: the free pointer
constexpr uint32_t kReserveEnd = 0x82A2C3B8;       //   y su final
constexpr uint32_t kReserveExhausted = 0x82A2C3C4;   // out of space: the flag
constexpr uint32_t kReserveLost = 0x82A2C3C8;   // and the requested bytes
constexpr uint32_t kViews = 0x82A38070;           // addi r26,r11,-32656 con r11 = 0x82A40000
constexpr uint32_t kVista1 = kViews + 112;        // eGetView(1)
constexpr uint32_t kVista2 = kViews + 224;        // eGetView(2)
constexpr uint32_t kMagic360 = 0xB60B60B7u;        // lis r4,-18933; ori r10,r4,24759: divide by 360
constexpr uint32_t kLapPrologue = 0x824C2858;    // bl __savegprlr_22 (the generated code does not execute it)
constexpr uint32_t kLapVisible = 0x824C2968;
constexpr uint32_t kLapPixels = 0x824C299C;
constexpr uint32_t kLapReserve = 0x824C2B58;
constexpr uint32_t kLapRotation = 0x824C2B70;
constexpr uint32_t kLapWind = 0x824C2C74;
constexpr uint32_t kLapMultiply = 0x824C2C84;
constexpr uint32_t kLapMarker = 0x824C2CE8;

// [start, start + n) due la stack watched [stack - kStack, stack)?
[[gnu::always_inline]] inline bool InStack(uint32_t start, uint32_t n, uint32_t stack) {
  return uint64_t(start) + n > uint64_t(stack) - kStack && uint64_t(start) < uint64_t(stack);
}

enum Path : uint32_t { kDiscard = 0, kWithoutMatrix = 1, kWithMatrix = 2, kWithWind = 3, kPaths = 4 };
constexpr const char* kNamesPath[kPaths] = {"discard", "sin matrix", "con matrix", "con wind"};
enum Reason : uint32_t {
  kByOff = 0,
  kByDetail = 1,
  kByStack = 2,
  kPorNaN = 3,
  kByFlares = 4,
  kByCheck = 5,  // the wind, until its first kMinimumWind are checked: left to the guard
  kReasons = 6
};

struct Output {
  bool done;       // true: the state is the original's; false: neither memory outside the stack nor context touched
  uint32_t reason;  // if not done
  uint32_t path;  // if done
};

constexpr uint64_t kChecks = 100000;  // first calls checked (a little over 1 s of racing)
constexpr uint64_t kMinimumWind = 5000;      // and the first ones of the wind, which calls two originals
constexpr uint64_t kPeriod = 4096;           // then 1 of every kPeriod (a power of 2)

// Counters (no read-modify-write atomics: A57 without LSE). Written by the thread that prepares the
// frames; if another thread counted at the same time some count would be lost, which does not matter for
// the report.
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_native[kPaths];      // for the report period
std::atomic<uint64_t> g_original[kReasons];   // for the report period
std::atomic<uint64_t> g_checked[kPaths];  // since startup, all without differences
std::atomic<uint64_t> g_checked_total{0};
std::atomic<bool> g_off{false};
std::atomic<int64_t> g_next_ms{0};
std::atomic<int8_t> g_active{-1};

template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline bool Active() {
  int8_t a = g_active.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_scenery_native) ? 1 : 0;
    g_active.store(a, std::memory_order_relaxed);
  }
  return a != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The whole of sub_824C2850. kGuard: called by Check (nothing is left to the guard because of the wind).
// ---------------------------------------------------------------------------------------------------------------
template <bool kGuard>
Output Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t yo = ctx.r3.u32;      // ScenerySectionHeader
  const uint32_t number = ctx.r4.u32;  // number de instance
  const uint32_t cull = ctx.r5.u32;    // SceneryCullInfo (r25)
  const uint64_t state = ctx.r6.u64;  // visibility_state (r26)
  const uint32_t stack = ctx.r1.u32;
  const uint32_t frame = stack - kFrame;

  // Nothing that is read can be on the stack that this function and the ones it calls write (the guard
  // watches that).
  if (stack < kStack || InStack(cull, 200, stack)) {
    return {false, kByStack, 0};
  }
  const uint32_t inst = Read32(base, yo + 32) + (number << 6);  // lwz r9,32(r3); rlwinm r10,r4,6,0,25; add r30,r10,r9
  if (InStack(inst, 64, stack)) {
    return {false, kByStack, 0};
  }

  const PPCRegister r1e = ctx.r1, r3e = ctx.r3, r4e = ctx.r4, r5e = ctx.r5, r6e = ctx.r6, r12e = ctx.r12, f1e = ctx.f1;
  const uint64_t lre = ctx.lr;
  // Leaving it to the original halfway: up to that point only the stack has been written (which the original
  // writes again the same way) and the context, which is left as it was.
  const auto a_la_original = [&](uint32_t reason) -> Output {
    ctx.r1 = r1e;
    ctx.r3 = r3e;
    ctx.r4 = r4e;
    ctx.r5 = r5e;
    ctx.r6 = r6e;
    ctx.r12 = r12e;
    ctx.f1 = f1e;
    ctx.lr = lre;
    return {false, reason, 0};
  };
  // loc_824C2B3C: addi r1,r1,304; lfd f29-f31 (disableFlushMode); b __restgprlr_22.
  const auto exit = [&](uint32_t path) -> Output {
    ctx.r1.s64 = ctx.r1.s64 + kFrame;
    ctx.fpscr.disableFlushMode();
    return {true, 0, path};
  };

  // --- Prologue ---
  ctx.r12.u64 = ctx.lr;             // mflr r12
  ctx.lr = kLapPrologue;          // bl __savegprlr_22
  ctx.fpscr.disableFlushMode();     // the one of "stfd f29,-112(r1)"
  Write64(base, stack - 112, 0);  // stfd f29, f30 and f31: in the generated code, local variables set to zero
  Write64(base, stack - 104, 0);
  Write64(base, stack - 96, 0);
  Write32(base, frame, stack);    // stwu r1,-304(r1)
  ctx.r1.u32 = frame;

  // --- 1. The preculler: the instance's bit in the view's section ---
  const int32_t section = int32_t(Read32(base, cull + 196));  // lwz r11,196(r25); cmpwi; blt
  if (section >= 0) {
    const uint32_t row = uint32_t(int32_t(int16_t(Read16(base, inst + 28)))) << 7;  // lhz; extsh; rlwinm 7,0,24
    const uint32_t byte = Read8(base, row + uint32_t(section >> 3) + Read32(base, yo + 48));  // srawi; add; lbzx
    if ((byte & (1u << (uint32_t(section) & 7u))) != 0) {  // slw r11,r4,r7; and; cmpwi; bne
      return exit(kDiscard);                              // r3 = this, sin touch
    }
  }

  // --- 2. ExcludeFlags ---
  const uint32_t flags = Read32(base, inst + 24);  // lwz r11,24(r30) (and the later lwz r9,24(r30))
  uint32_t marked = flags;
  if ((marked & 0x08000000u) != 0 || (marked & 0x40u) != 0) {
    marked |= 0x08000040u;  // oris r11,r11,2048; ori r11,r11,64
  }
  const uint32_t flags_view = Read32(base, cull + 132);  // lwz r5,132(r25) (and the later ones)
  if (((marked ^ 0xFFFFFF60u) & flags_view & 0x080000FFu) != 0) {  // xor; and; clrlwi 4; rlwinm 0,24,4
    return exit(kDiscard);
  }
  // lhz r9,62(r30); extsh; x 72 (rlwinm, add, rlwinm); add r31,r11,r10 con r10 = [r3+24]
  const uint32_t info = uint32_t(int32_t(int16_t(Read16(base, inst + 62)))) * 72u + Read32(base, yo + 24);
  if (InStack(info, 72, stack)) {
    return a_la_original(kByStack);
  }

  // --- 3. Visibility parcial ---
  uint64_t visibility = state;               // r26
  if (int32_t(uint32_t(state)) == 1) {        // cmpwi cr6,r26,1
    uint32_t box[6];
    for (uint32_t i = 0; i < 6; ++i) {
      box[i] = Read32(base, inst + 4 * i);
    }
    if (EsNaN(box[0]) | EsNaN(box[1]) | EsNaN(box[2]) | EsNaN(box[3]) | EsNaN(box[4]) | EsNaN(box[5])) {
      return a_la_original(kPorNaN);           // lfs + stfs through double would quiet a signaling NaN
    }
    Write32(base, frame + 80, box[0]);     // stfs f0,80 / f13,84 / f12,88: the minimum
    Write32(base, frame + 84, box[1]);
    Write32(base, frame + 88, box[2]);
    Write32(base, frame + 112, box[3]);    // stfs f11,112 / f10,116 / f9,120: the maximum
    Write32(base, frame + 116, box[4]);
    Write32(base, frame + 120, box[5]);
    ctx.r6.s64 = 0;
    ctx.r5.s64 = ctx.r1.s64 + 112;
    ctx.r4.s64 = ctx.r1.s64 + 80;
    ctx.r3.u64 = Read32(base, cull + 128);     // lwz r3,128(r25): la vista
    ctx.lr = kLapVisible;
    sub_8243E7D8(ctx, base);                   // GetVisibleState
    visibility = ctx.r3.u64;                  // mr r26,r3
    if (int32_t(uint32_t(visibility)) == 0) {
      return exit(kDiscard);
    }
  }

  // --- 4. InlinedViewGetPixelSize (sub_824C2720), inlined and operation by operation ---
  ctx.fpscr.disableFlushMode();  // the one of "lfs f8,56(r31)" (GetVisibleState exits with flush on)
  Barrier();                     // no computation before this point
  const uint32_t w_radio = Read32(base, info + 56);
  const uint32_t w_six = Read32(base, kSix);
  const uint32_t w_x = Read32(base, inst + 32);
  const uint32_t w_y = Read32(base, inst + 36);
  const uint32_t w_z = Read32(base, inst + 40);
  const uint32_t w_cx = Read32(base, cull + 160);
  const uint32_t w_cy = Read32(base, cull + 164);
  const uint32_t w_cz = Read32(base, cull + 168);
  const uint32_t w_dx = Read32(base, cull + 176);
  const uint32_t w_dy = Read32(base, cull + 180);
  const uint32_t w_dz = Read32(base, cull + 184);
  const uint32_t w_h = Read32(base, cull + 192);
  if (EsNaN(w_radio) | EsNaN(w_six) | EsNaN(w_x) | EsNaN(w_y) | EsNaN(w_z) | EsNaN(w_cx) | EsNaN(w_cy) |
      EsNaN(w_cz) | EsNaN(w_dx) | EsNaN(w_dy) | EsNaN(w_dz) | EsNaN(w_h)) {
    return a_la_original(kPorNaN);
  }
  const double six = Lfs(w_six);                            // f31
  const double radio = double(float(Lfs(w_radio) + six));    // fadds f1,f8,f31
  ctx.f1.f64 = radio;
  ctx.lr = kLapPixels;                                    // bl 0x824c2720
  PPCRegister minus;
  minus.f64 = radio;
  minus.u64 ^= 0x8000000000000000ull;                         // fneg f11,f1
  const double ey = double(float(Lfs(w_y) - Lfs(w_cy)));      // fsubs f0,f0,f10
  double ez = double(float(Lfs(w_z) - Lfs(w_cz)));            // fsubs f13,f13,f9
  double ex = double(float(Lfs(w_x) - Lfs(w_cx)));            // fsubs f12,f12,f7
  const double t6 = double(float(Lfs(w_dy) * ey));            // fmuls f6,f8,f0
  const double t3 = double(float(std::fma(Lfs(w_dz), ez, t6)));  // fmadds f3,f5,f13,f6
  const double t2 = double(float(std::fma(Lfs(w_dx), ex, t3)));  // fmadds f2,f4,f12,f3
  uint64_t r3;
  uint32_t w_distance = 0;
  if (t2 < minus.f64) {                                       // fcmpu cr6,f2,f11; bge
    r3 = 0;                                                   // li r3,0: behind the camera
  } else {
    ez = double(float(ez * ez));                              // fmuls f13,f13,f13
    const double h = Lfs(w_h);                                // lfs f11,192(r3)
    ex = double(float(std::fma(ex, ex, ez)));                 // fmadds f12,f12,f12,f13
    const double d2 = double(float(std::fma(ey, ey, ex)));    // fmadds f10,f0,f0,f12
    double tam = h;                                           // fmr f12,f11
    const double distance = double(float(std::sqrt(d2)));   // fsqrts f0,f10
    w_distance = Stfs(distance);
    Write32(base, frame + 80, w_distance);                // stfs f0,0(r6), r6 = r1+80
    const double rest = double(float(distance - radio));    // fsubs f13,f0,f1
    if (rest > radio) {                                      // fcmpu cr6,f13,f1; ble
      const double t9 = double(float(h / rest));             // fdivs f9,f11,f13
      tam = double(float(t9 * radio));                        // fmuls f12,f9,f1
    }
    PPCRegister whole;
    whole.s64 = Fctiwz(tam);                                 // fctiwz f8,f12
    Write32(base, frame - 16, whole.u32);                 // stfiwx f8,0,r11 con r11 = r1-16
    r3 = whole.u32;                                          // lwz r3,-16(r1)
  }
  if (int32_t(uint32_t(r3)) <= 1) {                           // cmpwi cr6,r3,1; ble
    ctx.r3.u64 = r3;
    return exit(kDiscard);
  }
  if ((flags & 0x2000000u) != 0) {                         // rlwinm r7,r9,0,6,6; addi r3,r3,10
    r3 = uint64_t(int64_t(r3) + 10);
  }
  const int32_t pixels = int32_t(uint32_t(r3));
  ctx.r3.u64 = r3;  // what remains in r3 if it is discarded from here

  // --- 5. La mesh (r28) ---
  uint32_t model;
  if ((flags_view & 0x800u) != 0 || (flags_view & 0x1000u) != 0) {  // loc_824C2A90
    const uint32_t vista = Read32(base, cull + 128);
    int32_t threshold = 32;
    if (int32_t(Read32(base, vista + 4)) == 20 && (flags_view & 0x1000u) != 0) {
      threshold = 23;
    }
    if (pixels < threshold) {
      return exit(kDiscard);
    }
    if ((flags & 0x80u) != 0) {
      model = Read32(base, info + 48);                                    // loc_824C2AC8
    } else if ((flags & 0x100u) != 0 || (flags & 0x1000000u) != 0) {
      model = Read32(base, info + 40);                                    // loc_824C2AF8
    } else {
      model = Read32(base, info + 52);
    }
  } else if ((flags_view & 0x20u) != 0) {
    if (pixels < 32) {
      return exit(kDiscard);
    }
    model = Read32(base, info + 48);
  } else if (int32_t(Read32(base, kModeView)) >= 3) {
    if (pixels < 23) {
      return exit(kDiscard);
    }
    model = Read32(base, info + 48);
  } else {                                                                  // loc_824C2A10
    if (pixels < 18) {
      return exit(kDiscard);
    }
    const uint32_t good = Read32(base, info + 40);                        // r10 = pModel[0]
    model = good;                                                         // loc_824C2A88 if there are no more
    if (good != 0) {
      const uint32_t solid = Read32(base, good + 12);
      if (solid != 0 && int32_t(int16_t(Read16(base, solid + 20))) >= 40) {
        const uint32_t w_density = Read32(base, solid + 156);
        const uint32_t w_k = Read32(base, kDistanceK);
        const uint32_t w_threshold = Read32(base, kThresholdLod);
        if (EsNaN(w_density) | EsNaN(w_k) | EsNaN(w_threshold)) {
          return a_la_original(kPorNaN);
        }
        double density = Lfs(w_density);                                 // lfs f0,156(r11)
        if (density < six) {                                             // fcmpu cr6,f0,f31; bge
          density = six;                                                 // fmr f0,f31
        }
        const int64_t px64 = int64_t(pixels);                             // extsw r6,r3
        const bool near = Lfs(w_distance) < Lfs(w_k);                    // lfs f7,80(r1); lfs f13,3552(r27); fcmpu
        Write64(base, frame + 80, uint64_t(px64));                      // std r6,80(r1)
        const double f5 = double(px64);                                    // lfd f6,80(r1); fcfid f5,f6
        const double f4 = double(float(f5));                               // frsp f4,f5
        const double quotient = double(float(f4 / density));              // fdivs f0,f4,f0
        if (!near) {                                                      // blt cr6 -> la good
          const double threshold_lod = Lfs(w_threshold);                         // lfs f13,-4396(r27)
          if (quotient < threshold_lod || std::isnan(quotient)) {             // blt / bso -> loc_824C2AC8
            model = Read32(base, info + 48);                              // la reduced
          }
        }
      }
    }
  }
  if (model == 0) {                                                        // loc_824C2AFC: cmplwi cr6,r28,0; beq
    return exit(kDiscard);
  }

  // --- 6. SceneryDrawInfo sin matrix ---
  if ((flags & 0x200u) != 0) {
    const uint32_t cap = Read32(base, cull + 144);
    const uint32_t actual = Read32(base, cull + 140);
    if (actual >= cap) {                                                   // cmplw; bge: full
      return exit(kDiscard);
    }
    const uint64_t model_y_state = uint64_t(model) + visibility;        // add r3,r28,r26
    Write32(base, cull + 140, actual + 12u);                             // stw r4,140(r25)
    Write32(base, actual + 0, uint32_t(model_y_state));                // stw r3,0(r11)
    Write32(base, actual + 4, 0u);                                       // stw r10,4(r11)
    Write32(base, actual + 8, inst);                                     // stw r30,8(r11)
    ctx.r3.u64 = model_y_state;
    return exit(kWithoutMatrix);
  }

  // --- 7. With a matrix (loc_824C2B50). Before writing anything outside the stack: flares, wind, constants ---
  const uint32_t vista = Read32(base, cull + 128);
  const bool vista_1_o_2 = vista == kVista1 || vista == kVista2;
  const uint32_t solid = Read32(base, model + 12);
  if (vista_1_o_2 && solid != 0 && Read32(base, solid + 128) != 0 && Read8(base, solid + 27) != 0) {
    return a_la_original(kByFlares);  // with position markers: the flare loop belongs to the original
  }
  const bool wind = (flags_view & 0x4000u) != 0 && solid != 0 && (Read16(base, solid + 14) & 0x80u) != 0;
  if constexpr (!kGuard) {
    if (wind && g_checked[kWithWind].load(std::memory_order_relaxed) < kMinimumWind) [[unlikely]] {
      return a_la_original(kByCheck);
    }
  }
  const bool height = ((flags_view & flags) & 0x100u) != 0;  // lwz r9,132(r25); lwz r8,24(r30); and; 0x100
  const bool mirror = (flags_view & 0x800u) != 0;               // lwz r5,132(r25); 0x800
  const uint32_t w_uno = Read32(base, kUno);
  const uint32_t w_cero = Read32(base, kCero);
  const uint32_t w_scale = Read32(base, kScaleRotation);
  const uint32_t w_height = height ? Read32(base, kHeightExtra) : 0u;
  const uint32_t w_sixty = wind ? Read32(base, kSixty) : 0u;
  if (EsNaN(w_uno) | EsNaN(w_cero) | EsNaN(w_scale) | EsNaN(w_height) | EsNaN(w_sixty)) {
    return a_la_original(kPorNaN);
  }

  // eFrameMallocMatrix(1) (820E_A568), inlined: 64 bytes from the frame allocator.
  ctx.lr = kLapReserve;
  const uint32_t reserve = Read32(base, kReserveCurrent);
  const uint32_t fin = Read32(base, kReserveEnd);
  uint32_t matrix = 0;
  if (uint32_t(reserve + 64u) < fin) {                    // cmplw cr6,r10,r8; blt
    Write32(base, kReserveCurrent, reserve + 64u);
    matrix = reserve;
  } else {                                                // out of space: the flag and the requested bytes
    Write32(base, kReserveExhausted, 1u);
    Write32(base, kReserveLost, Read32(base, kReserveLost) + 64u);
  }
  if (matrix == 0) {                                      // mr r31,r3; cmplwi cr6,r31,0; beq
    ctx.r3.u64 = 0;
    return exit(kWithMatrix);
  }

  // The rotation (8243_F8D8), inlined: each 16-bit integer (std + lfd + fcfid + frsp) times the scale (fmuls).
  ctx.lr = kLapRotation;
  const double scale = Lfs(w_scale);
  const auto rotation = [&](uint32_t displacement) -> uint32_t {
    const double a = double(int64_t(int16_t(Read16(base, inst + displacement))));  // lhz; extsh; std; lfd; fcfid
    const double b = double(float(a));                                              // frsp
    return Stfs(double(float(b * scale)));                                          // fmuls; stfs
  };
  Write32(base, matrix + 12, w_cero);                  // stfs f13,12(r4): without NaN, the word of kCero
  Write32(base, matrix + 4, rotation(46));
  Write32(base, matrix + 8, rotation(48));
  Write32(base, matrix + 0, rotation(44));
  Write32(base, matrix + 28, w_cero);
  Write32(base, matrix + 24, rotation(54));
  Write32(base, matrix + 20, rotation(52));
  Write32(base, matrix + 16, rotation(50));
  Write32(base, matrix + 44, w_cero);
  Write32(base, matrix + 36, rotation(58));
  Write32(base, matrix + 40, rotation(60));
  Write32(base, matrix + 32, rotation(56));
  // What its std leave in its work area (r1-32, -24 and -16 of the leaf, which has no frame): the last ones.
  Write64(base, frame - 32, uint64_t(int64_t(int16_t(Read16(base, inst + 50)))));
  Write64(base, frame - 24, uint64_t(int64_t(int16_t(Read16(base, inst + 60)))));
  Write64(base, frame - 16, uint64_t(int64_t(int16_t(Read16(base, inst + 56)))));
  double f1 = double(float(double(int64_t(int16_t(Read16(base, inst + 58))))));  // its last f1: frsp f1,f3

  // The position and the w (lfs + stfs without NaN: the same words).
  Write32(base, matrix + 48, w_x);
  Write32(base, matrix + 52, w_y);
  Write32(base, matrix + 56, w_z);
  Write32(base, matrix + 60, w_uno);
  if (height) {                                           // fmr f2,f13; lfs f0,7304(r27); fadds f1,f2,f0; stfs
    f1 = double(float(Lfs(w_z) + Lfs(w_height)));
    Write32(base, matrix + 56, Stfs(f1));
  }
  if (mirror) {                                           // lfs f0,40(r31); fneg; stfs: without NaN, the sign bit
    Write32(base, matrix + 40, Read32(base, matrix + 40) ^ 0x80000000u);
  }
  ctx.f1.f64 = f1;

  // The SceneryDrawInfo with a matrix.
  const uint32_t cap = Read32(base, cull + 144);         // lwz r3,144(r25)
  const uint32_t actual = Read32(base, cull + 140);       // lwz r29,140(r25)
  ctx.r3.u64 = cap;
  if (actual >= cap) {                                   // full: the allocated matrix stays, as in the original
    return exit(kWithMatrix);
  }
  Write32(base, cull + 140, actual + 12u);             // stw r11,140(r25)
  Write32(base, actual + 0, uint32_t(uint64_t(model) + visibility));  // add r10,r28,r26; stw r10,0(r29)
  if (wind) {
    // lfs f12,48(r31); fmuls by 60; fctiwz; stfiwx to r1+80; lwz; and the remainder of dividing by 360 (mulhw,
    // srawi...).
    const double f11 = double(float(Lfs(w_x) * Lfs(w_sixty)));
    PPCRegister f10;
    f10.s64 = Fctiwz(f11);
    Write32(base, frame + 80, f10.u32);
    const uint64_t r11 = f10.u32;                                                         // lwz r11,80(r1)
    const int64_t r10 = (int64_t(int32_t(uint32_t(r11))) * int64_t(int32_t(kMagic360))) >> 32;  // mulhw r10,r11,r10
    const uint64_t r9 = uint64_t(r10) + r11;                                              // add r9,r10,r11
    const int64_t r10b = int64_t(int32_t(uint32_t(r9)) >> 8);                             // srawi r10,r9,8
    const uint64_t r8 = uint64_t(r10b) + ((uint32_t(r10b) >> 31) & 1u);                   // rlwinm r9,r10,1,31,31; add
    const int64_t r7 = static_cast<int64_t>(r8 * uint64_t(360));                          // mulli r7,r8,360
    ctx.r5.u64 = r11 - uint64_t(r7);                                                      // subf r5,r7,r11
    ctx.r3.u64 = vista;                                   // lwz r3,128(r25)
    ctx.r4.s64 = ctx.r1.s64 + 128;
    ctx.r6.u64 = matrix;
    ctx.lr = kLapWind;
    NFSMW_SCENERY_WIND(ctx, base);                    // CreateWindRotMatrix(vista, r1+128, desfase, matrix)
    ctx.r5.u64 = matrix;
    ctx.r4.s64 = ctx.r1.s64 + 128;
    ctx.r3.u64 = matrix;
    ctx.lr = kLapMultiply;
    NFSMW_SCENERY_MULTIPLY(ctx, base);               // bMulMatrix(matrix, matrix, r1+128)
  }
  Write32(base, actual + 8, inst);                     // stw r30,8(r29)
  Write32(base, actual + 4, matrix);                   // stw r31,4(r29)
  if (vista_1_o_2 && solid != 0) {                       // 8221_9EA8(solid, 0): without markers it returns 0
    ctx.lr = kLapMarker;
    ctx.r3.u64 = 0;
  }
  return exit(wind ? kWithWind : kWithMatrix);
}

// ---------------------------------------------------------------------------------------------------------------
// Report, guard and call.
// ---------------------------------------------------------------------------------------------------------------
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
    REXLOG_INFO("[scenery] DrawAScenery (sub_824C2850) {}",
                Active() ? "en native (build 184): begins checking contra la original"
                         : "por la original (nfsmw_scenery_native = false)");
    return;
  }
  uint64_t native[kPaths];
  uint64_t total = 0;
  for (uint32_t c = 0; c < kPaths; ++c) {
    native[c] = g_native[c].exchange(0, std::memory_order_relaxed);
    total += native[c];
  }
  uint64_t original[kReasons];
  uint64_t total_original = 0;
  for (uint32_t m = 0; m < kReasons; ++m) {
    original[m] = g_original[m].exchange(0, std::memory_order_relaxed);
    total_original += original[m];
  }
  NFSMW_REPORT_DEFERRED(
      "[scenery] last 10 s: {} en native ({} discards, {} sin matrix, {} con matrix, {} con wind), {} por la "
      "original (off {}, nfsmw_scenery_detail {}, stack {}, NaN {}, flares {}, wind a should_check {}); "
      "checked contra la original since el arranque: {} ({} discards, {} sin matrix, {} con matrix, {} con "
      "wind; the primeras {}, the primeras {} con wind y after 1 de every {}){}",
      total, native[kDiscard], native[kWithoutMatrix], native[kWithMatrix], native[kWithWind], total_original,
      original[kByOff], original[kByDetail], original[kByStack], original[kPorNaN],
      original[kByFlares], original[kByCheck], g_checked_total.load(std::memory_order_relaxed),
      g_checked[kDiscard].load(std::memory_order_relaxed), g_checked[kWithoutMatrix].load(std::memory_order_relaxed),
      g_checked[kWithMatrix].load(std::memory_order_relaxed), g_checked[kWithWind].load(std::memory_order_relaxed),
      kChecks, kMinimumWind, kPeriod,
      g_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "");
}

// A memory area that the native version or the original can write: the state before, what the native version
// left, and the comparison.
struct Zone {
  const char* name;
  uint32_t address;
  uint32_t bytes;
  uint8_t* before;
  uint8_t* native;
};

std::string Hex(const uint8_t* bytes, uint32_t n) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string s;
  for (uint32_t i = 0; i < n; ++i) {
    s += kDigits[bytes[i] >> 4];
    s += kDigits[bytes[i] & 15];
  }
  return s;
}

// Guard: the native version, undo, the original, and compare. Always leaves the original's state.
[[gnu::noinline]] void Check(PPCContext& ctx, uint8_t* base) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t cull = ctx.r5.u32;
  if (stack < kStack || InStack(cull, 200, stack)) {
    Add(g_original[kByStack], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  // Snapshots from before. The stack in a per-thread buffer (1.5 KB x 2); the rest is small.
  static thread_local uint8_t stack_before[kStack];
  static thread_local uint8_t stack_native[kStack];
  uint8_t small_before[4 + 12 + 4 + 8 + 64];
  uint8_t small_native[4 + 12 + 4 + 8 + 64];
  Zone zones[6];
  uint32_t nz = 0;
  uint32_t used = 0;
  const auto aggregate = [&](const char* name, uint32_t address, uint32_t bytes) {
    zones[nz++] = {name, address, bytes, small_before + used, small_native + used};
    used += bytes;
  };
  zones[nz++] = {"stack", stack - kStack, kStack, stack_before, stack_native};
  aggregate("cull+140", cull + 140, 4);
  const uint32_t actual = Read32(base, cull + 140);
  if (actual < Read32(base, cull + 144)) {
    aggregate("SceneryDrawInfo", actual, 12);
  }
  aggregate("reserve", kReserveCurrent, 4);
  aggregate("reserve exhausted", kReserveExhausted, 8);
  // The block that would be allocated. With the pointer at 0 the original does not write to it (it exits
  // with r3 = 0): it is not checked.
  const uint32_t reserve = Read32(base, kReserveCurrent);
  if (reserve != 0 && uint32_t(reserve + 64u) < Read32(base, kReserveEnd)) {
    aggregate("matrix", reserve, 64);
  }
  for (uint32_t i = 0; i < nz; ++i) {
    std::memcpy(zones[i].before, Pointer(base, zones[i].address), zones[i].bytes);
  }
  const auto undo = [&]() {
    for (uint32_t i = nz; i-- > 0;) {
      std::memcpy(Pointer(base, zones[i].address), zones[i].before, zones[i].bytes);
    }
  };
  const PPCContext entry = ctx;
  const auto volver_a_the_entry = [&]() {
    ctx = entry;
    ctx.fpscr.setcsr(ctx.fpscr.csr);  // the real FPCR, like the copy's
  };

  const Output s = Native<true>(ctx, base);
  if (!s.done) {
    undo();
    volver_a_the_entry();
    Add(g_original[s.reason], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  for (uint32_t i = 0; i < nz; ++i) {
    std::memcpy(zones[i].native, Pointer(base, zones[i].address), zones[i].bytes);
  }
  const uint64_t r3n = ctx.r3.u64, f1n = ctx.f1.u64, r1n = ctx.r1.u64, r12n = ctx.r12.u64, lrn = ctx.lr;
  const uint32_t csrn = ctx.fpscr.csr;
  undo();
  volver_a_the_entry();
  __imp__sub_824C2850(ctx, base);

  const char* que = nullptr;
  const Zone* bad = nullptr;
  uint32_t byte = 0;
  for (uint32_t i = 0; i < nz && !bad; ++i) {
    const uint8_t* now = Pointer(base, zones[i].address);
    for (uint32_t k = 0; k < zones[i].bytes; ++k) {
      if (now[k] != zones[i].native[k]) {
        bad = &zones[i];
        byte = k;
        break;
      }
    }
  }
  if (bad) {
    que = "memory_block";
  } else if (ctx.r3.u64 != r3n) {
    que = "r3";
  } else if (ctx.f1.u64 != f1n) {
    que = "f1";
  } else if (ctx.r1.u64 != r1n) {
    que = "r1";
  } else if (ctx.r12.u64 != r12n) {
    que = "r12";
  } else if (ctx.lr != lrn) {
    que = "lr";
  } else if (ctx.fpscr.csr != csrn) {
    que = "FPCR";
  }
  if (!que) {
    Add(g_checked[s.path], uint64_t(1));
    const uint64_t total = g_checked_total.load(std::memory_order_relaxed) + 1;
    g_checked_total.store(total, std::memory_order_relaxed);
    if (total == kChecks) {
      REXLOG_INFO("[scenery] {} calls checked contra la original (r3, f1, r1, r12, lr, FPCR, la stack, "
                  "[cull+140], el SceneryDrawInfo, la reserve y la matrix), 0 differences: DrawAScenery en native, y "
                  "sigue checking 1 de every {}",
                  total, kPeriod);
    }
    if (s.path == kWithWind && g_checked[kWithWind].load(std::memory_order_relaxed) == kMinimumWind) {
      REXLOG_INFO("[scenery] wind: {} calls checked contra la original, 0 differences: tambien en native",
                  kMinimumWind);
    }
    return;
  }
  g_off.store(true, std::memory_order_relaxed);  // the state is already the original's
  const uint32_t n = bad ? (bad->bytes - byte < 8u ? bad->bytes - byte : 8u) : 0u;
  REXLOG_ERROR("[scenery] DIFFERENCE con la original ({}{}{}; path {}): native {} original {}; r3 native 0x{:X} "
               "original 0x{:X}, f1 0x{:016X} / 0x{:016X}, r12 0x{:X} / 0x{:X}, lr 0x{:X} / 0x{:X}, FPCR 0x{:X} / "
               "0x{:X}; entry: this 0x{:08X}, instance {}, cull 0x{:08X}, state {}, stack 0x{:08X}. Path native "
               "OFF para el rest de la session: se queda la original",
               que, bad ? " en " : "", bad ? fmt::format("{} +{}", bad->name, byte) : std::string(),
               kNamesPath[s.path], bad ? Hex(bad->native + byte, n) : std::string("-"),
               bad ? Hex(Pointer(base, bad->address) + byte, n) : std::string("-"), r3n, ctx.r3.u64, f1n,
               ctx.f1.u64, r12n, ctx.r12.u64, lrn, ctx.lr, csrn, ctx.fpscr.csr, entry.r3.u32, entry.r4.u32,
               entry.r5.u32, entry.r6.u64, entry.r1.u32);
}

}  // namespace

// The hook in nfsmw_d3d_registers_native.cpp calls it inside its measurement ("[measurement] DrawAScenery").
void DrawAScenery(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_calls.load(std::memory_order_relaxed) + 1;
  g_calls.store(n, std::memory_order_relaxed);
  if ((n & (kPeriod - 1)) == 0) [[unlikely]] {
    Report();
  }
  if (!Active() || g_off.load(std::memory_order_relaxed)) [[unlikely]] {
    Add(g_original[kByOff], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  if (REXCVAR_GET(nfsmw_scenery_detail) != 0) [[unlikely]] {
    Add(g_original[kByDetail], uint64_t(1));
    __imp__sub_824C2850(ctx, base);
    return;
  }
  if (n <= kChecks || (n & (kPeriod - 1)) == 0) [[unlikely]] {
    Check(ctx, base);
    return;
  }
  const Output s = Native<false>(ctx, base);
  if (s.done) [[likely]] {
    Add(g_native[s.path], uint64_t(1));
    return;
  }
  if (s.reason == kByCheck) {
    Check(ctx, base);
    return;
  }
  Add(g_original[s.reason], uint64_t(1));
  __imp__sub_824C2850(ctx, base);
}

}  // namespace nfsmw::scenery_native
