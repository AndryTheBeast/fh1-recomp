// nfsmw - per-draw matrices (sub_824538D0) in native code.
//
// WHAT IT IS (PowerPC read instruction by instruction in nfsmw_recomp.78.cpp:16080)
//   Called by the game thread on every draw with r3 = material object ([r3+12] = handle table, [r3+28] =
//   effect), r4 = matrix A (4 rows of 16 bytes) and r5 = view block (rows P at +240, rows Q at +304 and the
//   position E at +48). 384-byte frame.
//   1. Cache: 0x82A2D190 keeps (r5, r4, r3) of the previous call. If they are the same, it does nothing else.
//   2. M1 = A x P, M2 = A x Q and M3 = A x C, with C the pass matrix (0x82C40CC0 + 384 * [0x82A2D1A0]): each
//      row is a vmulfp128 and three vmaddfp. They go to the frame (r1+128, +192 and +256).
//   3. The rigid inverse of A (xyz rows of A0..A2 with -A3.Ai in w) and, with it, E' (E = [r5+48..56] with
//      w = [0x82063038]) and G' (G = [[0x82A2C4F8] + 288]): vmsum3fp128, vmsum4fp128 and vmrghw. They go to
//      r1+80 and r1+112.
//   4. Depending on the handles in the table, the effect's writers: 82449C00 (matrix) with M1 (+572), A
//      (+568), M3 (+468) and M2 (+576); 82449988 (vector) with E' (+284), G' (+292) and, if there is a
//      handle at +564, with the vector at +16 of the pass record normalized by sub_8215_A588 into r1+320.
//
// WHY NATIVE
//   Stack sampling: 7.2 % of a core of the game thread including its frameless leaves. Recompiled (13,696
//   bytes in the ELF, with the vector body duplicated by the conditional enableFlushMode) every vector
//   instruction goes through the context, every lvx/stvx rereads VectorMaskL from memory, and every call
//   writes the FPCR (msr fpcr) four or five times, plus once more in sub_8215_A588. Here everything stays in
//   registers and the FPCR is written at most twice (the vector part in flush mode and the normalization in
//   scalar mode). Vector part, one path (devkitA64 -O3): 595 instructions, 265 memory accesses and 5 msr in
//   the recompiled ELF; here 394, 78 and 1. The normalization goes from 251 instructions with its own msr
//   to about 60 inlined ones. The writers are called just as in the original, through their hooks
//   (nfsmw_material_native.cpp): their work does not change.
//
// WHY IT IS BIT-IDENTICAL
//   - Floating point: the same simde functions as the generated code, with the same operands and in the
//     same order. The nfsmw and nfsmw_recomp targets are compiled with FMA contraction (without
//     -ffp-contract=off) and GCC fuses "a*b + c" depending on who else uses each product; this was read
//     in the ELF and is reproduced the same way here:
//       * The 12 rows of M1, M2 and M3: the first product (vmulfp128, a single-use local variable) is fused
//         with the rounded second one: fma(X0, a0, r(X1*a1)), then fma(X2, a2, .) and fma(X3, a3, .). Here
//         the same: the first product in its local variable, first, and the rest in the same form.
//       * The three vmaddfp of the inverse (v10, v0, v12): the original stores the first product in the
//         context (a second use) and the second one is fused: fma((1,1,1,0), Ai, r(dp * (0,0,0,1))). Here
//         that first product goes through Opaque() (an empty asm) so that GCC has to do the same.
//       * The constants -1, (0,0,0,1) and (1,1,1,0) also go through Opaque(): the original computes them at
//         run time (scvtf, ext) and GCC must not turn "x * -1" into "-x" (with a NaN it is not the same).
//       * simde_mm_dp_ps is the same on both sides (on the Switch vmulq_f32 + faddp + faddp).
//     Checked in devkitA64's GIMPLE with the Switch options (12 of 12 rows and 3 of 3 of the inverse, the
//     same as the original compiled separately) and on PC with FMA (0 differences).
//     The denormal flush mode is the original's: the whole vector part with FZ set (the original clears it
//     only for its lfs/stfs, which are word copies here) and the normalization without it. It exits in the
//     mode the original exits in (with FZ, or without it if it normalized).
//   - NaN: with several NaNs in one operation, which one propagates depends on the order of the operands
//     inside the instruction, which the compiler chooses (the ELF has products with the operands reversed
//     relative to simde). That is why, if any floating-point input is NaN (A, P, Q, C, E, its w, G, or the
//     vector and the two constants of the normalization), the original does the work (the whole function,
//     or only sub_8215_A588). Without NaN in the input, any NaN that appears is the default NaN and the order
//     can no longer change a single bit; infinities, denormals and signed zeros depend only on the
//     operations, which are the same (PC test with and without FMA).
//   - Memory: the same final state byte for byte. Prologue (frame back link, r3 at r1+404 and r4 at
//     r1+412), the cache, r1+96, the 12 rows, E', G', the three floats of the normalization and what the
//     writers write. The stfs to r1+80..92 are not written because the stvx of E' overwrites that same whole
//     block. The original reads some inputs after writing to its frame and here they are read before: if
//     any lands inside the frame, or r1 is not 16-byte aligned, the original runs. If it is left to the
//     original after the frame has been written (NaN), it does not matter: it writes it again in full before
//     reading it, and the cache is left as it was. The reads of the handle table and of the effect are
//     repeated after each writer, as in the original.
//   - Registers: interprocedural liveness analysis of all the generated code (56,338 functions): after the
//     16 direct call sites only r3 is live (in 2) and f1 (which is not touched). r3 is left as the original
//     leaves it, r1 as its stwu + addi, and r12/lr like the original; the writers get the same
//     r3/r4/r5/lr/r1. cr6, xer, r28-r31 and v19-v31 are local variables in the generated code.
//
// SELF-CHECKING GUARD (cvar nfsmw_matrices_native; project rule)
//   The first kChecks calls of each path (cache and computation) and then 1 of every 4096: the
//   native version records each write (with copies of the two writers, 82449C00 and 82449988, identical to
//   the ones in nfsmw_material_native.cpp), it is undone, the original runs (with the real hooks) and the
//   recorded bytes, the whole frame (416 bytes), r3, r1 and the FPCR are compared. The original's result is
//   always kept. A single difference turns the native version off for the session and writes
//   "[matrices] DIFFERENCE". "[matrices]" line every 10 s.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports
#include <rex/platform.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_matrices_native, true, "NFSMW",
                    "Matrices por draw (sub_824538D0: A x P, A x Q, A x C y la inverse rigida de A) en native "
                    "(build 176), identico bit a bit. Se comprueba contra la original (the primeras 100.000 calls "
                    "de every path y after 1 de every 4096) y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824538D0);
// The writers by their usual name: the hook in nfsmw_material_native.cpp (or the original, if there is none).
REX_EXTERN(sub_82449C00);
REX_EXTERN(sub_82449988);
// The original normalization. The name is assembled from parts on purpose: tools/direct_calls.py treats
// any address that appears whole in app/src as hooked, and its 70 calls from the generated code must stay
// direct.
#define NFSMW_MATRICES_JOIN_(a, b) a##b
#define NFSMW_MATRICES_NORMALIZE NFSMW_MATRICES_JOIN_(__imp__sub_8215, A588)
REX_EXTERN(NFSMW_MATRICES_NORMALIZE);

namespace nfsmw::matrices {
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
[[gnu::always_inline]] inline uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Pointer(base, address), 4);
  return __builtin_bswap32(v);
}
// 16 bytes from an aligned address, in memory order.
[[gnu::always_inline]] inline void ReadBlock(uint8_t* base, uint32_t aligned, uint8_t* output) {
  std::memcpy(output, Pointer(base, aligned), 16);
}

// ---------------------------------------------------------------------------------------------------------------
// Write with or without recording (as in nfsmw_material_native.cpp). Without recording it costs nothing; when
// recording (guard) it saves address, size and previous bytes to undo.
// ---------------------------------------------------------------------------------------------------------------
enum Class : uint8_t { kPure = 0, kOr = 1 };

struct Annotation {
  uint32_t address;
  uint8_t bytes;
  uint8_t kind;
  uint8_t mask;
  uint8_t before[16];
  uint8_t after[16];
};

struct Register {
  Annotation* a;
  uint32_t capacity;
  uint32_t n = 0;
  bool full = false;
};

template <bool kNote>
struct Memory {
  uint8_t* base;
  Register* reg_entry;

  [[gnu::always_inline]] inline void Note(uint32_t address, uint32_t bytes, uint8_t kind, uint8_t mask) {
    if constexpr (kNote) {
      if (reg_entry->n >= reg_entry->capacity) {
        reg_entry->full = true;
        return;
      }
      Annotation& e = reg_entry->a[reg_entry->n++];
      e.address = address;
      e.bytes = uint8_t(bytes);
      e.kind = kind;
      e.mask = mask;
      std::memcpy(e.before, Pointer(base, address), bytes);
    } else {
      (void)address;
      (void)bytes;
      (void)kind;
      (void)mask;
    }
  }
  [[gnu::always_inline]] inline void Write32(uint32_t address, uint32_t input_value) {
    Note(address, 4, kPure, 0);
    input_value = __builtin_bswap32(input_value);
    std::memcpy(Pointer(base, address), &input_value, 4);
  }
  [[gnu::always_inline]] inline void Write16B(uint32_t aligned, const uint8_t* bytes) {
    Note(aligned, 16, kPure, 0);
    std::memcpy(Pointer(base, aligned), bytes, 16);
  }
  [[gnu::always_inline]] inline void MarkDirty(uint32_t address, uint8_t old, uint8_t bit) {
    Note(address, 1, kOr, bit);
    *Pointer(base, address) = uint8_t(old | bit);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset; checked in the PC test).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kLast = 0x82A2D190;        // lis r11,-32093; addi r11,r11,-11888: r5 (+0), r4 (+4), r3 (+8)
constexpr uint32_t kIndexPass = 0x82A2D1A0;    // lwz r30,-11872(0x82A30000)
constexpr uint32_t kPasses = 0x82C3F590;         // lis r11,-32060; addi r28,r11,-2672: register_values de 384 bytes
constexpr uint32_t kMatrixPass = 5936;          // addi r8,r28,5936: C in the pass record
constexpr uint32_t kVectorPass = 5760 + 16;     // addi r10,r28,5760 ... addi r4,r11,16: vector a normalize
constexpr uint32_t kPointerG = 0x82A2C4F8;      // lwz r11,-15112(0x82A30000); addi r10,r11,288
constexpr uint32_t kUno = 0x82063038;           // lfs f0,12344(0x82060000): w of E; the 1 of the normalization
constexpr uint32_t kCero = 0x82061CE8;          // lfs f12,-4944(0x82063038): the 0 of the normalization
constexpr uint32_t kFrame = 384;                // stwu r1,-384(r1)
// Return addresses of each bl (ctx.lr in the generated code)
constexpr uint32_t kLapPrologue = 0x824538D8;  // bl __savegprlr (which the generated code does not execute)
constexpr uint32_t kLap572 = 0x82453C30;
constexpr uint32_t kLap568 = 0x82453C4C;
constexpr uint32_t kLapNormalize = 0x82453C7C;
constexpr uint32_t kLap564 = 0x82453C8C;
constexpr uint32_t kLap468 = 0x82453CA8;
constexpr uint32_t kLap576 = 0x82453CC4;
constexpr uint32_t kLap284 = 0x82453D1C;
constexpr uint32_t kLap292 = 0x82453D3C;

// ---------------------------------------------------------------------------------------------------------------
// The two writers, copied unchanged from nfsmw_material_native.cpp. Only the guard uses them, to record what
// they are going to write; the normal path calls the hooks, like the original.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kTableBits = 0x8290DB68;   // lis r8,-32111; addi r8,r8,-9368: the mask of each bit
constexpr uint32_t kTableRows = 0x8208F7C0;  // lis r10,-32247; addi r7,r10,-2112: row masks of 82449C00

struct Group {
  uint32_t dirty;
  uint32_t table;
  uint32_t target;
};
inline Group ReadGroup(uint8_t* base, uint32_t effect, uint32_t handle) {
  if ((handle & 1u) == 0) {
    return {effect, Read32(base, effect + 264), Read32(base, effect + 296)};
  }
  return {Read32(base, effect + 256), Read32(base, Read32(base, effect + 268)),
          Read32(base, Read32(base, effect + 300))};
}
inline uint32_t Index(uint32_t handle) {
  return (handle >> 1) & 0x1FFFFu;
}
inline uint32_t Entry(uint32_t handle) {
  return (handle >> 15) & 0x1FFF8u;
}
inline uint32_t Register16(uint32_t word) {
  return (word & 0xFFFFu) << 4;
}

// 82449988: aligned 16-byte vector. Leaves r3 = old dirty byte.
template <bool A>
inline uint32_t Writer9988(Memory<A>& m, uint32_t effect, uint32_t handle, uint32_t p) {
  uint8_t* const base = m.base;
  uint8_t block[16];
  ReadBlock(base, p & ~0xFu, block);
  const Group g = ReadGroup(base, effect, handle);
  const uint32_t index = Index(handle);
  const uint8_t bit = Read8(base, kTableBits + (index & 7u));
  const uint32_t dirty = g.dirty + (index >> 3);
  const uint8_t old = Read8(base, dirty);
  m.MarkDirty(dirty, old, bit);
  const uint32_t word = Read32(base, g.table + Entry(handle) + 4);
  m.Write16B((Register16(word) + g.target) & ~0xFu, block);
  return old;
}

// 82449C00: 4x4 matrix (the rows given by the entry, transposed). Leaves r3 = the mask of the dirty bit.
template <bool A>
inline uint32_t WriterC00(Memory<A>& m, uint32_t effect, uint32_t handle, uint32_t p) {
  uint8_t* const base = m.base;
  uint32_t row[4][4];
  for (uint32_t i = 0; i < 4; ++i) {
    ReadBlock(base, (p + 16 * i) & ~0xFu, reinterpret_cast<uint8_t*>(row[i]));
  }
  const Group g = ReadGroup(base, effect, handle);
  const uint32_t entry = g.table + Entry(handle);
  const uint32_t word0 = Read32(base, entry);
  const uint32_t word1 = Read32(base, entry + 4);
  const uint32_t rows = ((word0 >> 4) & 7u) + 1;
  uint32_t mask[4];
  ReadBlock(base, (((rows << 2) & 0xFFFFFFF0u) + kTableRows) & ~0xFu, reinterpret_cast<uint8_t*>(mask));
  const uint32_t index = Index(handle);
  const uint8_t bit = Read8(base, kTableBits + (index & 7u));
  const uint32_t dirty = g.dirty + (index >> 3);
  const uint8_t old = Read8(base, dirty);
  m.MarkDirty(dirty, old, bit);
  const uint32_t target = Register16(word1) + g.target;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t output[4];
    for (uint32_t k = 0; k < 4; ++k) {
      output[k] = (row[i][k] & ~mask[k]) | (row[k][i] & mask[k]);
    }
    m.Write16B((target + 16 * i) & ~0xFu, reinterpret_cast<const uint8_t*>(output));
  }
  return bit;
}

// ---------------------------------------------------------------------------------------------------------------
// Vectors as in the generated code: lvx128 = aligned block reversed byte by byte (lane 3 is PowerPC word 0).
// The reversal is VectorMaskL's (its first 16 bytes), but with a constant GCC knows.
// ---------------------------------------------------------------------------------------------------------------
using V = simde__m128i;
using F = simde__m128;

[[gnu::always_inline]] inline V Invert(V v) {
  return simde_mm_shuffle_epi8(v, simde_mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}
[[gnu::always_inline]] inline V Lvx(uint8_t* base, uint32_t address) {
  V v;
  std::memcpy(&v, Pointer(base, address & ~0xFu), 16);
  return Invert(v);
}
template <bool A>
[[gnu::always_inline]] inline void Stvx(Memory<A>& m, uint32_t address, V v) {
  const V x = Invert(v);
  uint8_t bytes[16];
  std::memcpy(bytes, &x, 16);
  m.Write16B(address & ~0xFu, bytes);
}
[[gnu::always_inline]] inline F Fl(V v) {
  return simde_mm_castsi128_ps(v);
}
[[gnu::always_inline]] inline V En(F f) {
  return simde_mm_castps_si128(f);
}
template <int kI>
[[gnu::always_inline]] inline V Splat(V v) {  // vspltw: 0xFF = word 0, 0xAA = 1, 0x55 = 2, 0x00 = 3
  return simde_mm_shuffle_epi32(v, kI);
}
// vmulfp128 vD,vA,vB and vmaddfp vD,vA,vB,vC in the exact form of the generated code.
[[gnu::always_inline]] inline F Mul(V a, V b) {
  return simde_mm_mul_ps(Fl(a), Fl(b));
}
[[gnu::always_inline]] inline F Madd(V a, V b, F c) {
  return simde_mm_add_ps(simde_mm_mul_ps(Fl(a), Fl(b)), c);
}

// A value GCC cannot see into (an empty asm): it can neither fuse it with an addition nor fold it as a constant.
template <class T>
[[gnu::always_inline]] inline T Opaque(T v) {
#if defined(SIMDE_ARM_NEON_A64V8_NATIVE)
  __asm__("" : "+w"(v));
#elif defined(SIMDE_X86_SSE2_NATIVE)
  __asm__("" : "+x"(v));
#else
  __asm__("" : "+m"(v));
#endif
  return v;
}

// Compiler barrier: no memory reads or writes cross the FPCR change.
[[gnu::always_inline]] inline void Barrier() {
  __asm__ __volatile__("" ::: "memory");
}

[[gnu::always_inline]] inline bool EsNaN(uint32_t bits) {
  return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}
// Absolute value of each word (already in host order): a float is NaN if and only if it exceeds 0x7F800000.
[[gnu::always_inline]] inline V Abs(V bits) {
  return simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7FFFFFFF));
}
[[gnu::always_inline]] inline V Larger(V a, V b) {  // signed maximum; after Abs all are >= 0
  return simde_mm_max_epi32(a, b);
}
[[gnu::always_inline]] inline bool There_isNaN(V larger) {
  const V nan = simde_mm_cmpgt_epi32(larger, simde_mm_set1_epi32(0x7F800000));
  uint64_t x[2];
  std::memcpy(x, &nan, 16);
  return (x[0] | x[1]) != 0;
}

// Does [start, start + n) touch the frame [frame, frame + 384)?
[[gnu::always_inline]] inline bool InTheFrame(uint32_t start, uint32_t n, uint32_t frame) {
  return uint64_t(start) + n > frame && uint64_t(start) < uint64_t(frame) + kFrame;
}

// ---------------------------------------------------------------------------------------------------------------
// The calls to the writers: on the normal path, through their hooks with the context as the original leaves
// it; in the guard, with the copies above, recording.
// ---------------------------------------------------------------------------------------------------------------
struct WritersByHook {
  PPCContext& ctx;
  uint8_t* base;
  // At the start (cache path or computation path, not when it is left to the original): mflr r12, the prologue
  // bl and the stwu.
  void Begin(uint32_t frame) {
    ctx.r12.u64 = ctx.lr;
    ctx.lr = kLapPrologue;
    ctx.r1.u32 = frame;
  }
  uint32_t Matrix(uint32_t effect, uint32_t handle, uint32_t p, uint32_t lap) {
    ctx.r5.u64 = p;
    ctx.r3.u64 = effect;
    ctx.r4.u64 = handle;
    ctx.lr = lap;
    sub_82449C00(ctx, base);
    return ctx.r3.u32;
  }
  uint32_t Vector(uint32_t effect, uint32_t handle, uint32_t p, uint32_t lap) {
    ctx.r5.u64 = p;
    ctx.r3.u64 = effect;
    ctx.r4.u64 = handle;
    ctx.lr = lap;
    sub_82449988(ctx, base);
    return ctx.r3.u32;
  }
};

template <bool A>
struct WritersCopy {
  Memory<A>& m;
  uint32_t calls = 0;
  void Begin(uint32_t) {}
  uint32_t Matrix(uint32_t effect, uint32_t handle, uint32_t p, uint32_t) {
    ++calls;
    return WriterC00(m, effect, handle, p);
  }
  uint32_t Vector(uint32_t effect, uint32_t handle, uint32_t p, uint32_t) {
    ++calls;
    return Writer9988(m, effect, handle, p);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// sub_8215_A588 (nfsmw_recomp.24.cpp:2273): normalizes the 3 floats of s into d; with a length equal to
// [kCero], it leaves ([kUno], [kCero], [kCero]). In double precision and in scalar mode, operation by operation
// like the generated code (the fma in double and then to float, the same as the original). With a NaN in the
// input the original runs.
// ---------------------------------------------------------------------------------------------------------------
template <bool A>
inline void Normalize(Memory<A>& m, PPCContext& ctx, uint32_t d, uint32_t s) {
  uint8_t* const base = m.base;
  ctx.fpscr.disableFlushMode();  // its first instruction
  Barrier();
  const uint32_t wy = Read32(base, s + 4);
  const uint32_t wx = Read32(base, s + 0);
  const uint32_t wz = Read32(base, s + 8);
  const uint32_t w_zero = Read32(base, kCero);
  const uint32_t w_one = Read32(base, kUno);
  if (EsNaN(wx) || EsNaN(wy) || EsNaN(wz) || EsNaN(w_zero) || EsNaN(w_one)) [[unlikely]] {
    if constexpr (A) {  // the original writes these three words (and the guard undoes them)
      m.Note(d, 4, kPure, 0);
      m.Note(d + 4, 4, kPure, 0);
      m.Note(d + 8, 4, kPure, 0);
    }
    ctx.r3.u64 = d;
    ctx.r4.u64 = s;
    ctx.lr = kLapNormalize;
    NFSMW_MATRICES_NORMALIZE(ctx, base);
    return;
  }
  PPCRegister t;
  t.u32 = wy;
  double f0 = double(t.f32);
  f0 = double(float(f0 * f0));
  t.u32 = wx;
  const double f13 = double(t.f32);
  t.u32 = wz;
  double f12 = double(t.f32);
  const double f11 = double(float(std::fma(f13, f13, f0)));
  const double f10 = double(float(std::fma(f12, f12, f11)));
  t.u32 = w_zero;
  f12 = double(t.f32);
  f0 = double(float(sqrt(f10)));
  if (f0 == f12) {  // cr6.compare + beq: equal and no NaN
    // lfs/stfs of [kUno] and two stfs of f12: without NaN, copies of the word
    m.Write32(d + 0, w_one);
    m.Write32(d + 4, w_zero);
    m.Write32(d + 8, w_zero);
    return;
  }
  t.u32 = w_one;
  f0 = double(float(double(t.f32) / f0));
  t.u32 = wy;
  const double y = double(t.f32);
  t.u32 = wz;
  const double z = double(t.f32);
  t.f32 = float(double(float(f13 * f0)));
  m.Write32(d + 0, t.u32);
  t.f32 = float(double(float(y * f0)));
  m.Write32(d + 4, t.u32);
  t.f32 = float(double(float(z * f0)));
  m.Write32(d + 8, t.u32);
}

// ---------------------------------------------------------------------------------------------------------------
// sub_824538D0 whole.
// ---------------------------------------------------------------------------------------------------------------
enum class Path : uint8_t { kCache, kCalculation, kOriginal };
enum Reason : uint32_t { kByStack = 0, kByFrame = 1, kPorNaN = 2, kReasons = 3 };

struct Output {
  Path path;
  uint32_t r3;         // kCalculation: the r3 the original leaves
  bool normalized;      // kCalculation: sub_8215_A588 was called (the original exits in scalar mode)
  uint32_t reason;     // kOriginal: why
};

template <bool A, class E>
Output Native(Memory<A>& m, PPCContext& ctx, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t stack, E& esc) {
  uint8_t* const base = m.base;
  const uint32_t r1 = stack - kFrame;
  // Prologue: stwu r1,-384(r1); stw r3,404(r1); stw r4,412(r1). The cache, like the original: read and write
  // before checking whether it is the same input.
  m.Write32(r1, stack);
  m.Write32(r1 + 404, r3);
  m.Write32(r1 + 412, r4);
  const uint32_t last8 = Read32(base, kLast + 8);  // lwz r10,8(r11)
  const uint32_t last0 = Read32(base, kLast + 0);  // lwz r9,0(r11)
  const uint32_t last4 = Read32(base, kLast + 4);  // lwz r8,4(r11)
  m.Write32(kLast + 4, r4);                  // stw r4,4(r11)
  m.Write32(kLast + 8, r3);                  // stw r3,8(r11)
  m.Write32(kLast + 0, r5);                  // stw r5,0(r11)
  if (((last0 - r5) | (last8 - r3) | (last4 - r4)) == 0) {  // or, or, cmpwi cr6,r10,0, beq
    esc.Begin(r1);
    return {Path::kCache, 0, false, 0};
  }

  // --- The addresses, without reading any float yet ---
  ctx.fpscr.enableFlushMode();  // the one of "vmulfp128 v22": the whole vector part runs with FZ set
  Barrier();
  auto a_la_original = [&](uint32_t reason) -> Output {
    m.Write32(kLast + 4, last4);  // the cache as it was: the original must see the previous entry
    m.Write32(kLast + 8, last8);
    m.Write32(kLast + 0, last0);
    return {Path::kOriginal, 0, false, reason};
  };
  if ((stack & 0xFu) != 0) [[unlikely]] {
    return a_la_original(kByStack);
  }
  const uint32_t index = Read32(base, kIndexPass);                    // lwz r30,-11872(r11)
  const uint32_t pass_c = kPasses + kMatrixPass + ((3u * index) << 7);  // rlwinm, add, rlwinm, add
  const uint32_t pointer_g = Read32(base, kPointerG);                   // lwz r11,-15112(r11)
  const uint32_t pos_g = pointer_g + 288;                               // addi r10,r11,288
  // The original reads some inputs after writing to its frame: none of them may land inside it.
  if (InTheFrame(r4 & ~0xFu, 64, r1) || InTheFrame((r5 + 240) & ~0xFu, 128, r1) || InTheFrame(r5 + 48, 12, r1) ||
      InTheFrame(pass_c & ~0xFu, 64, r1) || InTheFrame(pos_g & ~0xFu, 16, r1) || InTheFrame(kIndexPass, 4, r1) ||
      InTheFrame(kPointerG, 4, r1) || InTheFrame(kUno, 4, r1)) [[unlikely]] {
    return a_la_original(kByFrame);
  }

  // --- M1 = A x P, M2 = A x Q, M3 = A x C, in the order of the generated code (nfsmw_recomp.78.cpp:16147-16394)
  // and with the loads where the original does them. The absolute value of each input goes into the maximum: at
  // the end it says whether there was a NaN.
  const V a0 = Lvx(base, r4);        // lvx128 v12,r0,r4
  const V s00 = Splat<0xFF>(a0);     // vspltw v7,v12,0
  const V s01 = Splat<0xAA>(a0);     // vspltw v3,v12,1
  const V a1 = Lvx(base, r4 + 16);   // lvx128 v11,r0,r9
  const V a2 = Lvx(base, r4 + 32);   // lvx128 v10,r0,r8
  const V s10 = Splat<0xFF>(a1);     // vspltw v6,v11,0
  const V s20 = Splat<0xFF>(a2);     // vspltw v5,v10,0
  const V q0 = Lvx(base, r5 + 304);  // lvx128 v13,r0,r10
  const V a3 = Lvx(base, r4 + 48);   // lvx128 v9,r0,r7
  const V s30 = Splat<0xFF>(a3);     // vspltw v4,v9,0
  V larger = Larger(Larger(Abs(a0), Abs(a1)), Larger(Abs(a2), Abs(a3)));
  F v22 = Mul(q0, s00);              // vmulfp128 v22,v13,v7
  const V p0 = Lvx(base, r5 + 240);  // lvx128 v0,r0,r11
  F v30 = Mul(p0, s00);              // vmulfp128 v30,v0,v7
  F v28 = Mul(p0, s10);              // vmulfp128 v28,v0,v6
  F v27 = Mul(p0, s20);              // vmulfp128 v27,v0,v5
  const V s11 = Splat<0xAA>(a1);     // vspltw v2,v11,1
  F v26 = Mul(p0, s30);              // vmulfp128 v26,v0,v4
  const V s21 = Splat<0xAA>(a2);     // vspltw v1,v10,1
  const V c0 = Lvx(base, pass_c);    // lvx128 v8,r0,r9
  const V s31 = Splat<0xAA>(a3);     // vspltw v31,v9,1
  F v29 = Mul(c0, s00);              // vmulfp128 v29,v8,v7
  const V p1 = Lvx(base, r5 + 256);  // lvx128 v0,r0,r4
  F v25 = Mul(c0, s10);              // vmulfp128 v25,v8,v6
  const V q1 = Lvx(base, r5 + 320);  // lvx128 v7,r0,r8
  F v24 = Mul(c0, s20);              // vmulfp128 v24,v8,v5
  F v23 = Mul(c0, s30);              // vmulfp128 v23,v8,v4
  const V c1 = Lvx(base, pass_c + 16);  // lvx128 v8,r0,r3
  const F v21 = Mul(q0, s10);        // vmulfp128 v21,v13,v6
  const F v20 = Mul(q0, s20);        // vmulfp128 v20,v13,v5
  const F v19 = Mul(q0, s30);        // vmulfp128 v19,v13,v4
  larger = Larger(larger, Larger(Larger(Abs(q0), Abs(p0)), Larger(Abs(c0), Larger(Abs(p1), Larger(Abs(q1), Abs(c1))))));
  v30 = Madd(p1, s01, v30);          // vmaddfp v30,v0,v3,v30
  const V s02 = Splat<0x55>(a0);     // vspltw v6,v12,2
  v28 = Madd(p1, s11, v28);          // vmaddfp v28,v0,v2,v28
  const V s12 = Splat<0x55>(a1);     // vspltw v5,v11,2
  v27 = Madd(p1, s21, v27);          // vmaddfp v27,v0,v1,v27
  const V s22 = Splat<0x55>(a2);     // vspltw v4,v10,2
  v26 = Madd(p1, s31, v26);          // vmaddfp v26,v0,v31,v26
  const V c2 = Lvx(base, pass_c + 32);  // lvx128 v13,r0,r6
  v22 = Madd(q1, s01, v22);          // vmaddfp v22,v7,v3,v22
  const V p2 = Lvx(base, r5 + 272);  // lvx128 v0,r0,r7
  v29 = Madd(c1, s01, v29);          // vmaddfp v29,v8,v3,v29
  const V s32 = Splat<0x55>(a3);     // vspltw v3,v9,2
  v25 = Madd(c1, s11, v25);          // vmaddfp v25,v8,v2,v25
  v24 = Madd(c1, s21, v24);          // vmaddfp v24,v8,v1,v24
  v23 = Madd(c1, s31, v23);          // vmaddfp v23,v8,v31,v23
  const V q2 = Lvx(base, r5 + 336);  // lvx128 v8,r0,r5
  const F v2 = Madd(q1, s11, v21);   // vmaddfp v2,v7,v2,v21
  const F v1 = Madd(q1, s21, v20);   // vmaddfp v1,v7,v1,v20
  const F v31 = Madd(q1, s31, v19);  // vmaddfp v31,v7,v31,v19
  v30 = Madd(p2, s02, v30);          // vmaddfp v30,v0,v6,v30
  v28 = Madd(p2, s12, v28);          // vmaddfp v28,v0,v5,v28
  v27 = Madd(p2, s22, v27);          // vmaddfp v27,v0,v4,v27
  v26 = Madd(p2, s32, v26);          // vmaddfp v26,v0,v3,v26
  v29 = Madd(c2, s02, v29);          // vmaddfp v29,v13,v6,v29
  v25 = Madd(c2, s12, v25);          // vmaddfp v25,v13,v5,v25
  v24 = Madd(c2, s22, v24);          // vmaddfp v24,v13,v4,v24
  v23 = Madd(c2, s32, v23);          // vmaddfp v23,v13,v3,v23
  const F v6 = Madd(q2, s02, v22);   // vmaddfp v6,v8,v6,v22
  const V s03 = Splat<0x00>(a0);     // vspltw v7,v12,3
  const V p3 = Lvx(base, r5 + 288);  // lvx128 v0,r0,r4
  const F v5 = Madd(q2, s12, v2);    // vmaddfp v5,v8,v5,v2
  const V s13 = Splat<0x00>(a1);     // vspltw v11,v11,3
  const F v4 = Madd(q2, s22, v1);    // vmaddfp v4,v8,v4,v1
  const V s23 = Splat<0x00>(a2);     // vspltw v10,v10,3
  const F m1_0 = Madd(p3, s03, v30);  // vmaddfp v2,v0,v7,v30     M1 row 0
  const F m1_1 = Madd(p3, s13, v28);  // vmaddfp v1,v0,v11,v28    M1 row 1
  const V s33 = Splat<0x00>(a3);      // vspltw v9,v9,3
  const F v3 = Madd(q2, s32, v31);    // vmaddfp v3,v8,v3,v31
  const F m1_2 = Madd(p3, s23, v27);  // vmaddfp v31,v0,v10,v27   M1 row 2
  const V c3 = Lvx(base, pass_c + 48);  // lvx128 v13,r0,r3
  const F m3_0 = Madd(c3, s03, v29);  // vmaddfp v30,v13,v7,v29   M3 row 0
  const V q3 = Lvx(base, r5 + 352);   // lvx128 v12,r0,r11
  const F m1_3 = Madd(p3, s33, v26);  // vmaddfp v0,v0,v9,v26     M1 row 3
  const F m2_0 = Madd(q3, s03, v6);   // vmaddfp v7,v12,v7,v6     M2 row 0
  const F m3_1 = Madd(c3, s13, v25);  // vmaddfp v29,v13,v11,v25  M3 row 1
  const F m2_1 = Madd(q3, s13, v5);   // vmaddfp v11,v12,v11,v5   M2 row 1
  const F m3_2 = Madd(c3, s23, v24);  // vmaddfp v28,v13,v10,v24  M3 row 2
  const F m2_2 = Madd(q3, s23, v4);   // vmaddfp v10,v12,v10,v4   M2 row 2
  const F m3_3 = Madd(c3, s33, v23);  // vmaddfp v13,v13,v9,v23   M3 row 3
  const F m2_3 = Madd(q3, s33, v3);   // vmaddfp v9,v12,v9,v3     M2 row 3
  larger = Larger(larger, Larger(Larger(Abs(c2), Abs(p2)), Larger(Larger(Abs(q2), Abs(p3)), Larger(Abs(c3), Abs(q3)))));
  // To the frame. If it turns out at the end that there was a NaN and it is left to the original, nothing
  // happens: it writes all of this again before reading it.
  m.Write32(r1 + 96, r5 + 48);  // stw r6,96(r1)
  Stvx(m, r1 + 128, En(m1_0));     // stvx v2
  Stvx(m, r1 + 256, En(m3_0));     // stvx v30
  Stvx(m, r1 + 144, En(m1_1));     // stvx v1
  Stvx(m, r1 + 192, En(m2_0));     // stvx v7
  Stvx(m, r1 + 160, En(m1_2));     // stvx v31
  Stvx(m, r1 + 272, En(m3_1));     // stvx v29
  Stvx(m, r1 + 208, En(m2_1));     // stvx v11
  Stvx(m, r1 + 176, En(m1_3));     // stvx v0
  Stvx(m, r1 + 288, En(m3_2));     // stvx v28
  Stvx(m, r1 + 224, En(m2_2));     // stvx v10
  Stvx(m, r1 + 304, En(m3_3));     // stvx v13
  Stvx(m, r1 + 240, En(m2_3));     // stvx v9

  // --- The rigid inverse of A and E' = E x inv, G' = G x inv (16397-16577) ---
  const F uno = simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1));                // vspltisw v27,1; vcfsx v12,v27,0
  const F minus_one = Opaque(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(-1)));  // vspltisw v6,-1; vcfsx v0,v6,0
  const V zero = simde_mm_set1_epi32(0);                                     // vspltisw v8,0
  const V b3 = Lvx(base, r4 + 48);   // lvx128 v11,r0,r5 (segunda read de A: r31 = [r1+412] = r4)
  const V b0 = Lvx(base, r4);        // lvx128 v10,r0,r31
  const V b2 = Lvx(base, r4 + 32);   // lvx128 v7,r0,r3
  const V b1 = Lvx(base, r4 + 16);   // lvx128 v9,r0,r4
  const V gv = Lvx(base, pos_g);     // lvx128 v13,r0,r10
  const F a3n = simde_mm_mul_ps(Fl(b3), minus_one);                          // vmulfp128 v11,v11,v0
  const F w1 = Opaque(Fl(simde_mm_alignr_epi8(zero, En(uno), 12)));           // vsldoi v0,v8,v12,4: (0,0,0,1)
  const F xyz1 = Opaque(Fl(simde_mm_alignr_epi8(En(uno), zero, 12)));         // vsldoi v12,v12,v8,4: (1,1,1,0)
  // E: the lvx128 of r1+80 (16-byte aligned) reads the words of the stfs at +80, +84, +88 and +92. Those stfs
  // are not written: the stvx of E' overwrites that same whole block.
  const uint32_t ex = Read32(base, r5 + 48);  // lfs f0,0(r11), f13,4(r11), f12,8(r11) con r11 = [r1+96] = r5+48
  const uint32_t ey = Read32(base, r5 + 52);
  const uint32_t ez = Read32(base, r5 + 56);
  const uint32_t ew = Read32(base, kUno);     // lfs f0,12344(r11); stfs f0,92(r1)
  const V ev = simde_mm_set_epi32(int32_t(ex), int32_t(ey), int32_t(ez), int32_t(ew));
  const F e = Fl(ev);
  larger = Larger(larger, Larger(Abs(gv), Abs(ev)));
  const F d8 = simde_mm_dp_ps(a3n, Fl(b0), 0xEF);            // vmsum3fp128 v8,v11,v10
  const F d6 = simde_mm_dp_ps(a3n, Fl(b1), 0xEF);            // vmsum3fp128 v6,v11,v9
  const F d11 = simde_mm_dp_ps(a3n, Fl(b2), 0xEF);           // vmsum3fp128 v11,v11,v7
  const F m8 = Opaque(simde_mm_mul_ps(d8, w1));               // vmulfp128 v8,v8,v0 (the original stores it: 2 uses)
  const F m6 = Opaque(simde_mm_mul_ps(d6, w1));               // vmulfp128 v6,v6,v0
  const F g4 = simde_mm_dp_ps(Fl(gv), w1, 0xFF);             // vmsum4fp128 v4,v13,v0
  const F m5 = Opaque(simde_mm_mul_ps(d11, w1));              // vmulfp128 v5,v11,v0
  const F i0 = simde_mm_add_ps(simde_mm_mul_ps(xyz1, Fl(b0)), m8);  // vmaddfp v10,v12,v10,v8
  const F ew1 = simde_mm_dp_ps(e, w1, 0xFF);                 // vmsum4fp128 v8,v11,v0
  const F i1 = simde_mm_add_ps(simde_mm_mul_ps(xyz1, Fl(b1)), m6);  // vmaddfp v0,v12,v9,v6
  const F i2 = simde_mm_add_ps(simde_mm_mul_ps(xyz1, Fl(b2)), m5);  // vmaddfp v12,v12,v7,v5
  const F e0 = simde_mm_dp_ps(e, i0, 0xFF);                  // vmsum4fp128 v9,v11,v10
  const F e1 = simde_mm_dp_ps(e, i1, 0xFF);                  // vmsum4fp128 v7,v11,v0
  const F g0 = simde_mm_dp_ps(Fl(gv), i0, 0xFF);             // vmsum4fp128 v10,v13,v10
  const F g1 = simde_mm_dp_ps(Fl(gv), i1, 0xFF);             // vmsum4fp128 v0,v13,v0
  const F e2 = simde_mm_dp_ps(e, i2, 0xFF);                  // vmsum4fp128 v11,v11,v12
  const F g2 = simde_mm_dp_ps(Fl(gv), i2, 0xFF);             // vmsum4fp128 v13,v13,v12
  const V t12 = simde_mm_unpackhi_epi32(En(ew1), En(e1));    // vmrghw v12,v7,v8
  const V t0 = simde_mm_unpackhi_epi32(En(g4), En(g1));      // vmrghw v0,v0,v4
  const V t11 = simde_mm_unpackhi_epi32(En(e2), En(e0));     // vmrghw v11,v9,v11
  const V t13 = simde_mm_unpackhi_epi32(En(g2), En(g0));     // vmrghw v13,v10,v13
  const V ep = simde_mm_unpackhi_epi32(t12, t11);            // vmrghw v10,v11,v12: E'
  const V gp = simde_mm_unpackhi_epi32(t0, t13);             // vmrghw v9,v13,v0:   G'
  uint32_t table = Read32(base, r3 + 12);                    // lwz r10,12(r29)
  const uint32_t handle572 = Read32(base, table + 572);       // lwz r4,572(r10)
  Stvx(m, r1 + 80, ep);                                      // stvx v10 (overwrites the stfs at +80..+92)
  Stvx(m, r1 + 112, gp);                                     // stvx v9
  Barrier();  // all the vector work, before any mode change or call
  if (There_isNaN(larger)) [[unlikely]] {  // a NaN in the input: the original, from the start (see header)
    return a_la_original(kPorNaN);
  }
  esc.Begin(r1);

  // --- The writers, rereading the table and the effect like the original (16578-16736) ---
  uint32_t r3_final = 0;
  bool normalized = false;
  if (handle572 != 0) {
    r3_final = esc.Matrix(Read32(base, r3 + 28), handle572, r1 + 128, kLap572);
  }
  table = Read32(base, r3 + 12);  // loc_82453C30
  const uint32_t handle568 = Read32(base, table + 568);
  if (handle568 != 0) {
    r3_final = esc.Matrix(Read32(base, r3 + 28), handle568, r4, kLap568);  // mr r5,r31 (= [r1+412])
  }
  table = Read32(base, r3 + 12);  // loc_82453C4C
  const uint32_t handle564 = Read32(base, table + 564);
  if (handle564 != 0) {
    normalized = true;
    Normalize(m, ctx, r1 + 320, kPasses + kVectorPass + ((3u * index) << 7));
    r3_final = esc.Vector(Read32(base, r3 + 28), handle564, r1 + 320, kLap564);  // mr r4,r9
  }
  table = Read32(base, r3 + 12);  // loc_82453C8C
  const uint32_t handle468 = Read32(base, table + 468);
  if (handle468 != 0) {
    r3_final = esc.Matrix(Read32(base, r3 + 28), handle468, r1 + 256, kLap468);
  }
  table = Read32(base, r3 + 12);  // loc_82453CA8: lwz r3,12(r29)
  r3_final = table;
  const uint32_t handle576 = Read32(base, table + 576);
  if (handle576 != 0) {
    r3_final = esc.Matrix(Read32(base, r3 + 28), handle576, r1 + 192, kLap576);
  }
  table = Read32(base, r3 + 12);  // loc_82453CC4
  const uint32_t handle284 = Read32(base, table + 284);
  const bool con292 = Read32(base, table + 292) != 0;  // decided before the +284 writer
  if (handle284 != 0) {
    r3_final = esc.Vector(Read32(base, r3 + 28), handle284, r1 + 80, kLap284);
  }
  if (con292) {
    table = Read32(base, r3 + 12);  // loc_82453D1C: the handle is read again
    r3_final = esc.Vector(Read32(base, r3 + 28), Read32(base, table + 292), r1 + 112, kLap292);
  }
  return {Path::kCalculation, r3_final, normalized, 0};
}

// ---------------------------------------------------------------------------------------------------------------
// Counters, report and shutdown (no read-modify-write atomics: A57 without LSE).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kChecks = 100000;  // of each path; then 1 of every kPeriod
constexpr uint64_t kPeriod = 4096;
constexpr uint32_t kMaxAnnotations = 96;      // 6 from the prologue and the cache, 1 + 12 + 2 from the frame, 3 from the
                                              // normalization and up to 4 x 5 + 3 x 2 from the writers
constexpr uint32_t kFrameWatched = kFrame + 32;  // the frame and the caller's r3/r4 slots (up to r1+416)

enum Type : uint32_t { kTypeCache = 0, kTypeCalculation = 1, kTypes = 2 };
constexpr const char* kNames[kTypes] = {"cache", "calculation"};

struct Counters {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> native{0};       // for the report period
  std::atomic<uint64_t> original{0};    // for the period: turned off or left to the original
  std::atomic<uint64_t> checked{0};   // in the period
  std::atomic<uint64_t> checked_total{0};
};
Counters g_c[kTypes];
std::atomic<uint64_t> g_reasons[kReasons];  // for the period: why it went to the original (stack, frame, NaN)
std::atomic<bool> g_off{false};
std::atomic<int64_t> g_next_ms{0};

template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

std::atomic<int8_t> g_active{-1};
inline bool Active() {
  int8_t a = g_active.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_matrices_native) ? 1 : 0;
    g_active.store(a, std::memory_order_relaxed);
  }
  return a != 0;
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
    REXLOG_INFO("[matrices] sub_824538D0 en native (build 176); se comprueban contra la original the primeras {} "
                "calls de every path (cache y calculation) y after 1 de every {}",
                kChecks, kPeriod);
    return;
  }
  std::string line;
  for (uint32_t t = 0; t < kTypes; ++t) {
    Counters& c = g_c[t];
    line += fmt::format(" | {}: {} native, {} original, {} checked", kNames[t],
                         c.native.exchange(0, std::memory_order_relaxed),
                         c.original.exchange(0, std::memory_order_relaxed),
                         c.checked.exchange(0, std::memory_order_relaxed));
  }
  NFSMW_REPORT_DEFERRED("[matrices] last 10 s{} | a la original por stack desalineada {}, entry en el frame {}, NaN {}{}",
              line, g_reasons[kByStack].exchange(0, std::memory_order_relaxed),
              g_reasons[kByFrame].exchange(0, std::memory_order_relaxed),
              g_reasons[kPorNaN].exchange(0, std::memory_order_relaxed),
              g_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "");
}

std::string Hex(const uint8_t* bytes, uint32_t n) {
  static const char kDigits[] = "0123456789ABCDEF";
  std::string s;
  for (uint32_t i = 0; i < n; ++i) {
    s += kDigits[bytes[i] >> 4];
    s += kDigits[bytes[i] & 15];
  }
  return s;
}

void Photograph(Register& r, uint8_t* base) {
  for (uint32_t i = 0; i < r.n; ++i) {
    std::memcpy(r.a[i].after, Pointer(base, r.a[i].address), r.a[i].bytes);
  }
}
void Undo(const Register& r, uint8_t* base) {
  for (uint32_t i = r.n; i-- > 0;) {
    std::memcpy(Pointer(base, r.a[i].address), r.a[i].before, r.a[i].bytes);
  }
}
uint32_t FirstDifferent(const Register& r, uint8_t* base) {
  for (uint32_t i = 0; i < r.n; ++i) {
    if (std::memcmp(Pointer(base, r.a[i].address), r.a[i].after, r.a[i].bytes) != 0) {
      return i;
    }
  }
  return r.n;
}

// The bytes of the frame (and the caller's slots) one by one: what is in guest memory.
void CopyFrame(uint8_t* base, uint32_t since, uint8_t* output) {
  for (uint32_t i = 0; i < kFrameWatched; ++i) {
    output[i] = *Pointer(base, since + i);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Normal path and guard.
// ---------------------------------------------------------------------------------------------------------------
inline void Fast(PPCContext& ctx, uint8_t* base, Counters& c) {
  Memory<false> m{base, nullptr};
  WritersByHook esc{ctx, base};
  const uint32_t stack = ctx.r1.u32;
  const Output s = Native(m, ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, stack, esc);
  if (s.path == Path::kOriginal) [[unlikely]] {
    Add(c.original, uint64_t(1));
    Add(g_reasons[s.reason], uint64_t(1));
    __imp__sub_824538D0(ctx, base);  // from the entry state (the native version has not touched the context)
    return;
  }
  if (s.path == Path::kCalculation) {
    ctx.r3.u64 = s.r3;
  }
  ctx.r1.s64 = ctx.r1.s64 + kFrame;  // addi r1,r1,384 (Begin did the stwu)
  Add(c.native, uint64_t(1));
}

void NoticeChecked(uint32_t type) {
  Counters& c = g_c[type];
  Add(c.checked, uint64_t(1));
  const uint64_t total = c.checked_total.load(std::memory_order_relaxed) + 1;
  c.checked_total.store(total, std::memory_order_relaxed);
  if (total == kChecks && !g_off.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[matrices] sub_824538D0 (path {}): {} calls checked contra la original byte a byte, 0 "
                "differences: path native en marcha",
                kNames[type], kChecks);
  }
}

// Guard: the native version recording, undo, the original, compare. Always leaves the original's state.
[[gnu::noinline]] void Check(PPCContext& ctx, uint8_t* base, uint32_t type) {
  const PPCRegister r3e = ctx.r3, r4e = ctx.r4, r5e = ctx.r5, r1e = ctx.r1, r12e = ctx.r12;
  const uint64_t lre = ctx.lr;
  const uint32_t csr_e = ctx.fpscr.csr;
  const uint32_t frame = r1e.u32 - kFrame;
  Annotation annotations[kMaxAnnotations];
  Register reg{annotations, kMaxAnnotations};
  Memory<true> m{base, &reg};
  WritersCopy<true> esc{m};
  const Output s = Native(m, ctx, r3e.u32, r4e.u32, r5e.u32, r1e.u32, esc);
  const uint32_t csr_native = ctx.fpscr.csr;
  Photograph(reg, base);
  Undo(reg, base);
  // The frame the original has to leave: the current one with what the native version wrote on top.
  uint8_t expected[kFrameWatched];
  CopyFrame(base, frame, expected);
  for (uint32_t i = 0; i < reg.n; ++i) {
    for (uint32_t b = 0; b < reg.a[i].bytes; ++b) {
      const uint32_t k = reg.a[i].address + b - frame;
      if (k < kFrameWatched) {
        expected[k] = reg.a[i].after[b];
      }
    }
  }
  // The original from the entry state.
  ctx.r3 = r3e;
  ctx.r4 = r4e;
  ctx.r5 = r5e;
  ctx.r1 = r1e;
  ctx.r12 = r12e;
  ctx.lr = lre;
  if (ctx.fpscr.csr != csr_e) {
    ctx.fpscr.csr = csr_e;
    ctx.fpscr.setcsr(csr_e);
  }
  __imp__sub_824538D0(ctx, base);
  if (s.path == Path::kOriginal) {
    Add(g_c[type].original, uint64_t(1));
    Add(g_reasons[s.reason], uint64_t(1));
    return;  // the native version stepped aside without computing: nothing to compare
  }

  const uint32_t fm = uint32_t(PPCFPSCRRegister::FlushMask);
  const uint32_t csr_expected =
      s.path == Path::kCache ? csr_e : (s.normalized ? (csr_e & ~fm) : (csr_e | fm));
  const uint64_t r3_expected = s.path == Path::kCache ? r3e.u64 : uint64_t(s.r3);
  PPCRegister r1x = r1e;
  r1x.u32 = frame;           // stwu r1,-384(r1)
  r1x.s64 = r1x.s64 + kFrame;  // addi r1,r1,384
  uint8_t now[kFrameWatched];
  CopyFrame(base, frame, now);
  const uint32_t bad = FirstDifferent(reg, base);
  uint32_t byte_frame = kFrameWatched;
  for (uint32_t k = 0; k < kFrameWatched; ++k) {
    if (now[k] != expected[k]) {
      byte_frame = k;
      break;
    }
  }
  const char* reason = nullptr;
  if (reg.full) reason = "too_many writes";
  else if (bad < reg.n) reason = "bytes different";
  else if (byte_frame < kFrameWatched) reason = "frame de stack different";
  else if (ctx.r3.u64 != r3_expected) reason = "r3 different";
  else if (ctx.r1.u64 != r1x.u64) reason = "r1 different";
  else if (ctx.fpscr.csr != csr_expected) reason = "FPCR de la original different";
  else if (csr_native != csr_expected) reason = "FPCR de la native different";
  NoticeChecked(type);
  if (!reason) {
    return;
  }
  g_off.store(true, std::memory_order_relaxed);  // the state is already the original's
  const Annotation* a = bad < reg.n ? &reg.a[bad] : nullptr;
  REXLOG_INFO("[matrices] DIFFERENCE en sub_824538D0 ({}; path {}, check {}): r3 0x{:08X} r4 0x{:08X} r5 "
              "0x{:08X} r1 0x{:08X}; writers {}; address 0x{:08X} native {} original {}; first byte different "
              "del frame: {}; r3 native 0x{:X} original 0x{:X}; FPCR entry 0x{:X} native 0x{:X} original 0x{:X}. "
              "Path native OFF para always, se queda la original",
              reason, kNames[type], g_c[type].checked_total.load(std::memory_order_relaxed), r3e.u32, r4e.u32,
              r5e.u32, r1e.u32, esc.calls, a ? a->address : 0u, a ? Hex(a->after, a->bytes) : std::string("-"),
              a ? Hex(Pointer(base, a->address), a->bytes) : std::string("-"),
              byte_frame < kFrameWatched ? fmt::format("r1+{}", byte_frame) : std::string("ninguno"), r3_expected,
              ctx.r3.u64, csr_e, csr_native, ctx.fpscr.csr);
}

// The same input as last time? (the cache path; the native version checks it again)
inline bool SameEntry(uint8_t* base, const PPCContext& ctx) {
  return Read32(base, kLast + 0) == ctx.r5.u32 && Read32(base, kLast + 4) == ctx.r4.u32 &&
         Read32(base, kLast + 8) == ctx.r3.u32;
}

inline void Matrices(PPCContext& ctx, uint8_t* base) {
  if (!Active()) {
    __imp__sub_824538D0(ctx, base);
    return;
  }
  const uint32_t type = SameEntry(base, ctx) ? kTypeCache : kTypeCalculation;
  Counters& c = g_c[type];
  const uint64_t n = c.calls.load(std::memory_order_relaxed) + 1;
  c.calls.store(n, std::memory_order_relaxed);
  if (g_off.load(std::memory_order_relaxed)) [[unlikely]] {
    Add(c.original, uint64_t(1));
    __imp__sub_824538D0(ctx, base);
  } else if (n <= kChecks || (n & (kPeriod - 1)) == 0) [[unlikely]] {
    Check(ctx, base, type);
  } else {
    Fast(ctx, base, c);
  }
  if ((n & (kPeriod - 1)) == 0) [[unlikely]] {
    Report();
  }
}

}  // namespace
}  // namespace nfsmw::matrices

// The hook. The 16 calls in the generated code must go to sub_824538D0 and not to __imp__sub_824538D0:
// tools/direct_calls.py leaves them as sub_824538D0 because this file names the address.
REX_HOOK_RAW(sub_824538D0) {
  nfsmw::matrices::Matrices(ctx, base);
}
