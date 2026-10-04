// nfsmw - start of an effect pass (sub_82448E80) in native code.
//
// WHAT IT IS (PowerPC read instruction by instruction in nfsmw_recomp.94.cpp:16601-17040)
//   Called by the thread that runs the frames (Main XThread) every time a draw changes effect pass: from
//   8244_EA48 (two sites; that one is reached from 8244_F7E8 and from the command list loop) and from two
//   wrappers that jump to it with "b" (8244_8508 and 8244_A400). r3 = the effect (this), r4 = the pass.
//   176-byte frame.
//   1. From the pass: r25 = [r4+8] (its shaders), r26 = [r4+12] (render states) and r30 = [r4+16] (sampler
//      states). They are saved on the stack (r1+84, +88, +92), with this at r1+196 and the pass at r1+204.
//   2. If the previous pass ([this+540]) did not have the same shaders: the dirty masks set to all ones.
//      dcbzl of the 128-byte block of this and 16 bytes of ones for every 2 of [this+288]; and, unless
//      [lis -32114 + 2880] and [this+696] are both nonzero, the same with the buffer at [this+256] and the
//      count [this+292] (with the counter at r1+80 and this reread from r1+196 on every iteration, like
//      the original).
//   3. The pass's vertex shader ([[r25+72]]): AddRef (+4), releases the one the device kept at
//      [dev+20456] (8259_B9B8), stores it there and sets it (8259_C150). Then the pixel shader,
//      [[r25+76]], with SetPixelShader (sub_8259BDC0, through its hook in nfsmw_d3d_trace.cpp, like the
//      original).
//   4. The pass's render states ([r26+16] 8-byte entries from r26+20): function [dev+96+offset] (dev,
//      value). The sampler states ([r30+128] entries from r30+132): function [dev+484+offset] (dev,
//      index, value). These are indirect calls: they go through the dispatch table with
//      REX_CALL_INDIRECT_FUNC, like the original.
//   5. If [this+692] & 3: 16-byte OR into this+320 with [r26] and into this+384..496 with [r30..r30+112].
//   6. [this+532] = the pass and [this+536] = r25.
//
// WHY NATIVE
//   It is the recompiled function with the most self time left in the Main XThread: 5.3 +- 0.4 % of a
//   core in stack sampling (1.4 ms per frame), almost all of it waiting on memory: the effect, the pass,
//   its shaders, the device and the dispatch table of the indirect calls are read in a chain and separated
//   by the calls, so each cache miss is paid in full. Here they are requested in advance with PRFM (cache
//   hints: they read nothing and cannot fault) and the rest stays in registers: no context traffic, no
//   repeated byte swaps and no reloads of volatiles.
//
// WHY IT IS IDENTICAL
//   - No floating point: only integers, copies and bitwise ORs. It does not touch the FPCR.
//   - Memory: the same writes, in the same order relative to the reads and the calls, with the same bytes:
//     the frame back link and the five stw to the stack, the dcbzl (128 bytes of zeros) and the stvx of
//     ones, the counter at r1+80, the AddRef, [dev+20456], the OR masks and [this+532] and [this+536].
//     Everything the original reads again after a call (counts, entries, [r25+76], [this+692]...) is also
//     read afterwards here.
//   - Registers: before each call and on exit, r3-r12, lr, v0-v13 (and r1) are exactly as in the
//     original, not just the arguments: the guard compares all of them on every call, and after the last
//     call only what the original writes is written. r13-r31 (non-volatile), cr, xer and ctr are local
//     variables of the generated code.
//   - The indirect calls use the same macro as the generated code (nfsmw_pch.h), with the same dispatch
//     and the same "last_indirect" if the address is not registered.
//
// SELF-CHECKING GUARD (cvar nfsmw_effect_pass_native; project rule)
//   The original calls functions with effects that cannot be repeated (releasing a shader can free it, and
//   SetPixelShader and the states change the device), so it cannot be run twice. The guard compares the
//   native version with a literal copy of the original (the current generated code as is, without
//   comments and with its four ways of calling routed through a recorder; tools/literal_copy.py extracts
//   it from the generated code): both run dry, with the calls replaced by a recorder that logs r0-r13, lr,
//   f0-f13, v0-v13, FPCR, the last indirect call and a checksum of the written memory, and that then
//   dirties the volatiles the way a called function would. The areas it writes are snapshotted first (the
//   stack, the two mask blocks, the shader refcount, [dev+20456], the OR masks and [this+532]); after each
//   dry run what it leaves is saved and undone; the calls, the exit state and the memory are compared byte
//   by byte; and then the original runs for real. The original's state is always kept. When: the first
//   kChecks calls, the first kMinimumOdd of each rare path (second mask buffer, render states, OR
//   masks) and then 1 of every kPeriod. A difference writes "[effect_pass] DIFFERENCE" (REXLOG_ERROR)
//   and turns the native version off for the session.
//   Measured in the same session: 1 in 16 native calls is timed and 1 in 512 goes through the timed
//   original; the "[effect_pass]" line every 10 s gives the us per call of both and the saving in ms/s.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"
#include <rex/platform.h>
// The generated code's macros (REX_RAW_ADDR, REX_LOAD_*, REX_STORE_*, REX_CALL_INDIRECT_FUNC...): the
// native version dispatches indirect calls exactly like the original and the guard's literal copy compiles
// as is.
#include "generated/default/nfsmw_pch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_effect_pass_native, true, "NFSMW",
                    "Start de pass de un effect (sub_82448E80: masks de dirty, sombreadores y states de render y "
                    "de sampling) en native (build 185), identico. Se comprueba en dry contra one copy literal de la "
                    "original (the primeras 20.000 calls, the primeras 2.000 de every path odd y after 1 de every "
                    "4096) y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82448E80);  // la original
REX_EXTERN(sub_8259BDC0);         // SetPixelShader through its hook (nfsmw_d3d_trace.cpp), like the original
// Releasing the shader (8259_B9B8) and setting it (8259_C150): the originals, called directly, as the
// generated code calls them. The name is assembled from parts on purpose: tools/direct_calls.py treats
// any address that appears whole in app/src as hooked, and its calls from the rest of the game must remain
// direct (__imp__, inlinable).
#define NFSMW_PASS_JOIN_(a, b) a##b
#define NFSMW_PASS_FREE NFSMW_PASS_JOIN_(__imp__sub_8259, B9B8)
#define NFSMW_PASS_VERTICES NFSMW_PASS_JOIN_(__imp__sub_8259, C150)
REX_EXTERN(NFSMW_PASS_FREE);
REX_EXTERN(NFSMW_PASS_VERTICES);

namespace nfsmw::effect_pass_native {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: REX_RAW_ADDR from nfsmw_pch.h, without volatile (C++ itself provides the ordering: nothing is
// moved across an opaque call or a write that may overlap).
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline uint32_t L32(uint8_t* base, uint32_t d) {
  uint32_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 4);
  return __builtin_bswap32(v);
}
[[gnu::always_inline]] inline uint32_t L16(uint8_t* base, uint32_t d) {
  uint16_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 2);
  return __builtin_bswap16(v);
}
[[gnu::always_inline]] inline void E32(uint8_t* base, uint32_t d, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(REX_RAW_ADDR(d), &v, 4);
}
[[gnu::always_inline]] inline void Fill(uint8_t* base, uint32_t d, int input_value, uint32_t n) {
  std::memset(REX_RAW_ADDR(d), input_value, n);
}
// Cache hint (PRFM PLDL1KEEP on the Switch): it neither reads nor faults, even if the address is invalid.
[[gnu::always_inline]] inline void Anticipate(uint8_t* base, uint32_t d) {
  __builtin_prefetch(REX_RAW_ADDR(d));
}
// The same for a line that is about to be written entirely (PRFM PSTL1KEEP): the dcbzl of the masks.
[[gnu::always_inline]] inline void AnticipateWrite(uint8_t* base, uint32_t d) {
  __builtin_prefetch(REX_RAW_ADDR(d), 1);
}
// The dispatch table slot of an indirect call (the one REX_CALL_INDIRECT_FUNC reads).
[[gnu::always_inline]] inline void AnticipateDispatch(uint8_t* base, uint32_t target) {
  if ((uint32_t)(target - REX_CODE_BASE) < REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) {
    __builtin_prefetch(&REX_LOOKUP_FUNC(base, target));
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses and return addresses of each call (ctx.lr in the generated code).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kFrame = 176;                              // stwu r1,-176(r1)
constexpr uint32_t kStackWatched = kFrame + 32;               // [r1-176, r1+32): the frame and r1+20/+28 (196 and 204)
constexpr uint32_t kGlobal = (uint32_t(-32114) << 16) + 2880;  // lis r11,-32114; lwz r11,2880(r11)
constexpr uint32_t kLapPrologue = 0x82448E88;               // bl __savegprlr_24 (the generated code does not execute it)
constexpr uint32_t kLapFree = 0x82448FA8;
constexpr uint32_t kLapVertices = 0x82448FB8;
constexpr uint32_t kLapPixels = 0x82448FC8;
constexpr uint32_t kLapStates = 0x82448FF8;
constexpr uint32_t kLapSampling = 0x82449044;

enum What : uint32_t { kFree = 0, kShaderVertices = 1, kShaderPixels = 2, kIndirect = 3 };

enum Path : uint32_t {
  kPathReset = 1u << 0,  // dirty masks to all ones: the previous pass had other shaders
  kPathSecond = 1u << 1,   // and also the buffer at [this+256]
  kPathFree = 1u << 2,   // the device held another shader: it is released
  kPathStates = 1u << 3,   // the pass's render states
  kPathSampling = 1u << 4,  // the pass's sampler states
  kPathMasks = 1u << 5,  // the ORs of the masks
  kPaths = 6,
  kCombinations = 1u << kPaths
};
constexpr const char* kNamesPath[kPaths] = {"reset",         "second buffer", "release shader",
                                                  "states de render", "sampling",      "masks OR"};
constexpr uint32_t kPathsOdd = kPathSecond | kPathStates | kPathMasks;

// ---------------------------------------------------------------------------------------------------------------
// The real calls: the same functions the original calls, through the same path.
// ---------------------------------------------------------------------------------------------------------------
struct Real {
  [[gnu::always_inline]] static inline void Call(PPCContext& ctx, uint8_t* base, uint32_t que) {
    if (que == kFree) {
      NFSMW_PASS_FREE(ctx, base);
    } else if (que == kShaderVertices) {
      NFSMW_PASS_VERTICES(ctx, base);
    } else {
      sub_8259BDC0(ctx, base);
    }
  }
  [[gnu::always_inline]] static inline void Indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    REX_CALL_INDIRECT_FUNC(target);
  }
};

// The ORs of the masks (loc_82449058, rare path): the 18 reads before the 9 writes, like the original, and
// registers v0-v13 and r3-r11 as it leaves them (vN of the context = the 16 guest bytes reversed).
[[gnu::noinline]] void Masks(PPCContext& ctx, uint8_t* base, uint64_t r31, uint64_t r26, uint64_t r30) {
  const simde__m128i lap = simde_mm_load_si128((simde__m128i*)VectorMaskL);
  const auto load = [&](uint64_t d) {
    return simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(uint32_t(d) & ~0xFu)), lap);
  };
  const simde__m128i a0 = load(r26);         // lvx128 v0,r0,r26
  const simde__m128i a13 = load(r30);        // lvx128 v13,r0,r30
  const simde__m128i a12 = load(r30 + 16);   // lvx128 v12,r0,r29 (r29 = r30+16)
  const simde__m128i a5 = load(r31 + 320);   // lvx128 v5,r0,r11
  const simde__m128i a4 = load(r31 + 384);
  const simde__m128i a3 = load(r31 + 400);
  const simde__m128i a11 = load(r30 + 32);
  const simde__m128i a2 = load(r31 + 416);
  const simde__m128i a10 = load(r30 + 48);
  const simde__m128i a1 = load(r31 + 432);
  const simde__m128i a9 = load(r30 + 64);
  const simde__m128i a31 = load(r31 + 448);
  const simde__m128i a8 = load(r30 + 80);
  const simde__m128i a7 = load(r30 + 96);
  const simde__m128i a6 = load(r30 + 112);
  const simde__m128i a30 = load(r31 + 464);
  const simde__m128i a29 = load(r31 + 480);
  const simde__m128i a28 = load(r31 + 496);
  const simde__m128i v0 = simde_mm_or_si128(a5, a0);  // vor v0,v5,v0
  const simde__m128i v13 = simde_mm_or_si128(a4, a13);
  const simde__m128i v12 = simde_mm_or_si128(a3, a12);
  const simde__m128i v11 = simde_mm_or_si128(a2, a11);
  const simde__m128i v10 = simde_mm_or_si128(a1, a10);
  const simde__m128i v9 = simde_mm_or_si128(a31, a9);
  const simde__m128i v8 = simde_mm_or_si128(a30, a8);
  const simde__m128i v7 = simde_mm_or_si128(a29, a7);
  const simde__m128i v6 = simde_mm_or_si128(a28, a6);
  simde_mm_store_si128((simde__m128i*)ctx.v0.u8, v0);
  simde_mm_store_si128((simde__m128i*)ctx.v1.u8, a1);
  simde_mm_store_si128((simde__m128i*)ctx.v2.u8, a2);
  simde_mm_store_si128((simde__m128i*)ctx.v3.u8, a3);
  simde_mm_store_si128((simde__m128i*)ctx.v4.u8, a4);
  simde_mm_store_si128((simde__m128i*)ctx.v5.u8, a5);
  simde_mm_store_si128((simde__m128i*)ctx.v6.u8, v6);
  simde_mm_store_si128((simde__m128i*)ctx.v7.u8, v7);
  simde_mm_store_si128((simde__m128i*)ctx.v8.u8, v8);
  simde_mm_store_si128((simde__m128i*)ctx.v9.u8, v9);
  simde_mm_store_si128((simde__m128i*)ctx.v10.u8, v10);
  simde_mm_store_si128((simde__m128i*)ctx.v11.u8, v11);
  simde_mm_store_si128((simde__m128i*)ctx.v12.u8, v12);
  simde_mm_store_si128((simde__m128i*)ctx.v13.u8, v13);
  ctx.r11.u64 = r31 + 320;  // addi r11,r31,320 ... addi r3,r31,496
  ctx.r10.u64 = r31 + 384;
  ctx.r9.u64 = r31 + 400;
  ctx.r8.u64 = r31 + 416;
  ctx.r7.u64 = r31 + 432;
  ctx.r6.u64 = r31 + 448;
  ctx.r5.u64 = r31 + 464;
  ctx.r4.u64 = r31 + 480;
  ctx.r3.u64 = r31 + 496;
  const auto save = [&](uint64_t d, simde__m128i v) {
    simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(uint32_t(d) & ~0xFu), simde_mm_shuffle_epi8(v, lap));
  };
  save(r31 + 320, v0);  // stvx v0,r0,r11
  save(r31 + 384, v13);
  save(r31 + 400, v12);
  save(r31 + 416, v11);
  save(r31 + 432, v10);
  save(r31 + 448, v9);
  save(r31 + 464, v8);
  save(r31 + 480, v7);
  save(r31 + 496, v6);
}

// ---------------------------------------------------------------------------------------------------------------
// The whole of sub_82448E80. L = Real (for real) or Recorder (the guard, dry run). Returns the path (kPath*).
// ---------------------------------------------------------------------------------------------------------------
template <class L>
uint32_t Native(PPCContext& ctx, uint8_t* base) {
  uint32_t path = 0;
  // --- Prologue: mflr r12; bl __savegprlr_24 (lr only); stwu r1,-176(r1) ---
  ctx.r12.u64 = ctx.lr;
  ctx.lr = kLapPrologue;
  const uint32_t stack = ctx.r1.u32;
  const uint32_t frame = stack - kFrame;
  E32(base, frame, stack);
  ctx.r1.u32 = frame;
  uint64_t r24 = ctx.r4.u64;                            // mr r24,r4
  uint64_t r31 = ctx.r3.u64;                            // mr r31,r3
  uint64_t r25 = L32(base, uint32_t(r24) + 8);          // lwz r25,8(r24): the pass's shaders
  uint64_t r26 = L32(base, uint32_t(r24) + 12);         // lwz r26,12(r24): the render states
  uint64_t r30 = L32(base, uint32_t(r24) + 16);         // lwz r30,16(r24): the sampler states
  // What the original reads later in a chain, requested now (hints only: the real read happens in its place).
  Anticipate(base, uint32_t(r25) + 72);
  Anticipate(base, uint32_t(r26) + 16);
  Anticipate(base, uint32_t(r30) + 128);
  Anticipate(base, uint32_t(r31) + 692);
  AnticipateWrite(base, uint32_t(r31) & ~127u);      // the block the reset dcbzl zeroes
  uint64_t r11 = L32(base, uint32_t(r31) + 540);        // lwz r11,540(r31): the previous pass
  E32(base, frame + 196, uint32_t(r31));                // stw r31,196(r1)
  E32(base, frame + 204, uint32_t(r24));                // stw r24,204(r1)
  E32(base, frame + 84, uint32_t(r25));                 // stw r25,84(r1)
  E32(base, frame + 88, uint32_t(r26));                 // stw r26,88(r1)
  E32(base, frame + 92, uint32_t(r30));                 // stw r30,92(r1)
  bool reset = true;
  if (uint32_t(r11) != 0) {                             // cmplwi cr6,r11,0; beq loc_82448ED4
    if (uint32_t(r25) == 0) {                           // cmplwi cr6,r25,0; beq loc_82448F78
      reset = false;
    } else {
      r11 = L32(base, uint32_t(r11) + 8);               // lwz r11,8(r11)
      reset = uint32_t(r25) != uint32_t(r11);        // cmplw cr6,r25,r11; beq loc_82448F78
    }
  }
  if (reset) {                                       // --- loc_82448ED4: dirty masks to all ones ---
    path |= kPathReset;
    AnticipateWrite(base, L32(base, uint32_t(r31) + 256) & ~127u);  // the second buffer (hint: it is reread in its place)
    simde_mm_store_si128((simde__m128i*)ctx.v0.u32, simde_mm_set1_epi32(int(0xFFFFFFFF)));  // vspltisw v0,-1
    Fill(base, uint32_t(r31) & ~127u, 0, 128);      // dcbzl r0,r31
    const uint64_t r9 = L32(base, uint32_t(r31) + 288);  // lwz r9,288(r31)
    ctx.r9.u64 = r9;
    uint64_t r10 = 0;                                   // li r10,0
    if (uint32_t(r9) != 0) {                            // cmplwi cr6,r9,0; ble loc_82448F08
      uint64_t r8;
      r11 = r31;                                        // mr r11,r31
      do {                                              // loc_82448EF0
        Fill(base, uint32_t(r11) & ~0xFu, 0xFF, 16);  // stvx v0,r0,r11 (v0 = all ones)
        r8 = L32(base, uint32_t(r31) + 288);            // lwz r8,288(r31)
        r10 += 2;                                       // addi r10,r10,2
        r11 += 16;                                      // addi r11,r11,16
      } while (uint32_t(r10) < uint32_t(r8));           // cmplw cr6,r10,r8; blt
      ctx.r8.u64 = r8;
    }
    ctx.r10.u64 = r10;
    r11 = L32(base, kGlobal);                           // loc_82448F08: lis r11,-32114; lwz r11,2880(r11)
    bool second = true;
    if (uint32_t(r11) != 0) {                           // cmpwi cr6,r11,0; beq loc_82448F24
      const uint64_t r7 = L32(base, uint32_t(r31) + 696);  // lwz r7,696(r31)
      ctx.r7.u64 = r7;
      second = uint32_t(r7) == 0;                      // cmplwi cr6,r7,0; bne loc_82448F78
    }
    if (second) {                                      // --- loc_82448F24: the buffer at [this+256] ---
      path |= kPathSecond;
      const uint64_t r6 = L32(base, uint32_t(r31) + 256);  // lwz r6,256(r31)
      ctx.r6.u64 = r6;
      Fill(base, uint32_t(r6) & ~127u, 0, 128);     // dcbzl r0,r6
      r11 = 0;                                          // li r11,0
      const uint64_t r5 = L32(base, uint32_t(r31) + 292);  // lwz r5,292(r31)
      ctx.r5.u64 = r5;
      E32(base, frame + 80, 0);                         // stw r11,80(r1)
      if (uint32_t(r5) != 0) {                          // cmplwi cr6,r5,0; ble loc_82448F78
        uint64_t r3, r4, r9b, r10b;
        do {                                            // loc_82448F40: the counter on the stack, like the original
          r4 = L32(base, uint32_t(r31) + 256);          // lwz r4,256(r31)
          r3 = __builtin_rotateleft64(uint32_t(r11) | (r11 << 32), 3) & 0xFFFFFFF8u;  // rlwinm r3,r11,3,0,28
          Fill(base, (uint32_t(r4) + uint32_t(r3)) & ~0xFu, 0xFF, 16);  // stvx128 v0,r4,r3
          r9b = L32(base, frame + 80);                  // lwz r9,80(r1)
          r31 = L32(base, frame + 196);                 // lwz r31,196(r1)
          r11 = r9b + 2;                                // addi r11,r9,2
          r10b = L32(base, uint32_t(r31) + 292);        // lwz r10,292(r31)
          E32(base, frame + 80, uint32_t(r11));         // stw r11,80(r1)
        } while (uint32_t(r11) < uint32_t(r10b));       // cmplw cr6,r11,r10; blt
        ctx.r3.u64 = r3;
        ctx.r4.u64 = r4;
        ctx.r9.u64 = r9b;
        ctx.r10.u64 = r10b;
        r24 = L32(base, frame + 204);                   // lwz r24,204(r1)
        r25 = L32(base, frame + 84);                    // lwz r25,84(r1)
        r26 = L32(base, frame + 88);                    // lwz r26,88(r1)
        r30 = L32(base, frame + 92);                    // lwz r30,92(r1)
      }
    }
  }
  ctx.r11.u64 = r11;

  // --- loc_82448F78: the vertex shader ---
  const uint64_t r8 = L32(base, uint32_t(r25) + 72);    // lwz r8,72(r25)
  ctx.r8.u64 = r8;
  const uint64_t r29 = L32(base, uint32_t(r31) + 700);  // lwz r29,700(r31): the device
  Anticipate(base, uint32_t(r29) + 20456);
  const uint64_t r28 = L32(base, uint32_t(r8));         // lwz r28,0(r8)
  {
    // Hints only (what is read here decides nothing: everything is read again in its place, after the calls):
    // the pixel shader, and the function of the first sampler entry and its dispatch slot.
    Anticipate(base, L32(base, uint32_t(r25) + 76));
    if (L32(base, uint32_t(r30) + 128) != 0) {
      const uint32_t function = L32(base, uint32_t(r29 + L16(base, uint32_t(r30) + 134)) + 484);
      AnticipateDispatch(base, function);
    }
  }
  if (uint32_t(r28) != 0) {                             // cmplwi cr6,r28,0; beq loc_82448F98
    const uint64_t count = L32(base, uint32_t(r28) + 4);  // lwz r11,4(r28)
    const uint64_t r7 = count + 1;                     // addi r7,r11,1: AddRef
    ctx.r11.u64 = count;
    ctx.r7.u64 = r7;
    E32(base, uint32_t(r28) + 4, uint32_t(r7));         // stw r7,4(r28)
  }
  const uint64_t old = L32(base, uint32_t(r29) + 20456);  // loc_82448F98: lwz r3,20456(r29)
  ctx.r3.u64 = old;
  if (uint32_t(old) != 0) {                           // cmplwi cr6,r3,0; beq loc_82448FA8
    path |= kPathFree;
    ctx.lr = kLapFree;
    L::Call(ctx, base, kFree);                     // bl 8259_B9B8: lo loose
  }
  ctx.r4.u64 = r28;                                     // loc_82448FA8: mr r4,r28
  E32(base, uint32_t(r29) + 20456, uint32_t(r28));      // stw r28,20456(r29)
  ctx.r3.u64 = r29;                                     // mr r3,r29
  ctx.lr = kLapVertices;
  L::Call(ctx, base, kShaderVertices);            // bl 8259_C150: lo pone

  // --- The pixel shader ---
  const uint64_t r6 = L32(base, uint32_t(r25) + 76);    // lwz r6,76(r25)
  ctx.r6.u64 = r6;
  ctx.r3.u64 = r29;                                     // mr r3,r29
  ctx.r4.u64 = L32(base, uint32_t(r6));                 // lwz r4,0(r6)
  ctx.lr = kLapPixels;
  L::Call(ctx, base, kShaderPixels);             // SetPixelShader, through its hook

  // --- The render states: [r26+16] 8-byte entries from r26+20 ---
  const uint64_t r5 = L32(base, uint32_t(r26) + 16);    // lwz r5,16(r26)
  ctx.r5.u64 = r5;
  if (uint32_t(r5) != 0) [[unlikely]] {                 // cmplwi cr6,r5,0; ble loc_8244900C
    path |= kPathStates;
    uint64_t r28b = r26 + 20;                           // addi r28,r26,20
    uint64_t r27 = 0;                                   // li r27,0
    uint64_t r9;
    do {                                                // loc_82448FDC
      const uint64_t displacement = L32(base, uint32_t(r28b));  // lwz r11,0(r28)
      ctx.r3.u64 = r29;                                 // mr r3,r29
      ctx.r4.u64 = L32(base, uint32_t(r28b) + 4);       // lwz r4,4(r28)
      const uint64_t r11b = r29 + displacement;       // add r11,r29,r11
      ctx.r11.u64 = r11b;
      const uint64_t r10 = L32(base, uint32_t(r11b) + 96);  // lwz r10,96(r11)
      ctx.r10.u64 = r10;
      ctx.lr = kLapStates;
      L::Indirect(ctx, base, uint32_t(r10));           // mtctr r10; bctrl
      r9 = L32(base, uint32_t(r26) + 16);               // lwz r9,16(r26)
      ctx.r9.u64 = r9;
      r27 += 1;                                         // addi r27,r27,1
      r28b += 8;                                        // addi r28,r28,8
    } while (uint32_t(r27) < uint32_t(r9));             // cmplw cr6,r27,r9; blt
  }

  // --- loc_8244900C: the sampler states: [r30+128] 8-byte entries from r30+132 ---
  const uint64_t r8b = L32(base, uint32_t(r30) + 128);  // lwz r8,128(r30)
  ctx.r8.u64 = r8b;
  ctx.r11.u64 = r30 + 132;                              // addi r11,r30,132
  if (uint32_t(r8b) != 0) {                             // cmplwi cr6,r8,0; ble loc_82449058
    path |= kPathSampling;
    uint64_t r28c = r30 + 132 + 2;                      // addi r28,r11,2
    uint64_t r27 = 0;                                   // li r27,0
    uint64_t total = r8b;
    do {                                                // loc_82449024
      const uint64_t r11d = L16(base, uint32_t(r28c));  // lhz r11,0(r28)
      ctx.r11.u64 = r11d;
      ctx.r3.u64 = r29;                                 // mr r3,r29
      ctx.r5.u64 = L32(base, uint32_t(r28c) + 2);       // lwz r5,2(r28)
      const uint64_t r7 = r11d + r29;                   // add r7,r11,r29
      ctx.r7.u64 = r7;
      ctx.r4.u64 = L16(base, uint32_t(r28c) - 2);       // lhz r4,-2(r28)
      const uint64_t r6b = L32(base, uint32_t(r7) + 484);  // lwz r6,484(r7)
      ctx.r6.u64 = r6b;
      if (uint32_t(r27) + 1 < uint32_t(total)) {        // the dispatch slot of the next one, requested now (hint)
        const uint32_t next = L32(base, uint32_t(r29 + L16(base, uint32_t(r28c) + 8)) + 484);
        AnticipateDispatch(base, next);
      }
      ctx.lr = kLapSampling;
      L::Indirect(ctx, base, uint32_t(r6b));           // mtctr r6; bctrl
      total = L32(base, uint32_t(r30) + 128);           // lwz r5,128(r30)
      ctx.r5.u64 = total;
      r27 += 1;                                         // addi r27,r27,1
      r28c += 8;                                        // addi r28,r28,8
    } while (uint32_t(r27) < uint32_t(total));          // cmplw cr6,r27,r5; blt
  }

  // --- loc_82449058: the OR masks ---
  const uint64_t r4 = L32(base, uint32_t(r31) + 692);   // lwz r4,692(r31)
  ctx.r4.u64 = r4;
  const uint64_t r3 = uint32_t(r4) & 0x3u;              // clrlwi r3,r4,30
  ctx.r3.u64 = r3;
  if (r3 != 0) [[unlikely]] {                           // cmplwi cr6,r3,0; beq loc_82449138
    path |= kPathMasks;
    Masks(ctx, base, r31, r26, r30);
  }
  // --- loc_82449138 ---
  E32(base, uint32_t(r31) + 532, uint32_t(r24));        // stw r24,532(r31)
  E32(base, uint32_t(r31) + 536, uint32_t(r25));        // stw r25,536(r31)
  ctx.r1.s64 = ctx.r1.s64 + kFrame;                     // addi r1,r1,176 (and b __restgprlr_24: it just returns)
  return path;
}

// ---------------------------------------------------------------------------------------------------------------
// Guard: call recorder, memory areas and the literal copy of the original.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxCalls = 40;  // 3 direct + the state and sampler entries (few in a race)
constexpr uint32_t kMaxZones = 10;
constexpr uint32_t kMaxBytesZones = 4096;

static_assert(offsetof(PPCContext, r13) == offsetof(PPCContext, r3) + 13 * sizeof(PPCRegister),
              "r3, r0, r1, r2, r4..r13 consecutive");
static_assert(offsetof(PPCContext, f13) == offsetof(PPCContext, f0) + 13 * sizeof(PPCRegister), "f0..f13 consecutive");
static_assert(offsetof(PPCContext, v13) == offsetof(PPCContext, v0) + 13 * sizeof(PPCVRegister), "v0..v13 consecutive");

// What a call (or the exit) sees: all the volatiles, the FPCR, the last indirect call and the written memory.
struct Photo {
  uint32_t que;
  uint32_t target;
  uint64_t memory_block;          // checksum of the areas at that moment: order of the writes relative to the calls
  uint64_t r[14];            // r3, r0, r1, r2, r4 ... r13 (the PPCContext order)
  uint64_t lr;
  uint64_t f[14];            // f0 ... f13
  uint8_t v[14 * 16];        // v0 ... v13
  uint32_t csr;
  uint32_t last_indirect;
};
static_assert(sizeof(Photo) == 8 + 8 + 14 * 8 + 8 + 14 * 8 + 14 * 16 + 8, "Photo sin fill");
constexpr const char* kNamesR[14] = {"r3", "r0", "r1", "r2", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
                                       "r13"};

void Photograph(const PPCContext& c, Photo& f) {
  std::memcpy(f.r, &c.r3, sizeof(f.r));
  f.lr = c.lr;
  std::memcpy(f.f, &c.f0, sizeof(f.f));
  std::memcpy(f.v, &c.v0, sizeof(f.v));
  f.csr = c.fpscr.csr;
  f.last_indirect = c.last_indirect_target;
}

// As a called function would: the volatiles change (with a value that depends on the call).
void Dirty(PPCContext& c, uint32_t k) {
  const uint64_t mark = 0xC0DE000000000000ull | (uint64_t(k) << 24);
  PPCRegister* const r = &c.r3;  // r3, r0, r1, r2, r4 ... r13
  for (uint32_t i = 0; i < 14; ++i) {
    if (i != 2 && i != 3 && i != 13) {  // r1, r2 and r13 are not touched by anyone
      r[i].u64 = mark | (0x10u + i);
    }
  }
  c.lr = mark | 0xFF;
  PPCRegister* const f = &c.f0;
  for (uint32_t i = 0; i < 14; ++i) {
    f[i].u64 = mark | (0x100u + i);
  }
  uint8_t* const v = c.v0.u8;
  for (uint32_t i = 0; i < 14 * 16; ++i) {
    v[i] = uint8_t(0xA5u ^ (k * 29u) ^ i);
  }
  c.last_indirect_target = 0xC0DE0000u | k;
}

struct Zones {
  uint32_t n = 0;
  uint32_t total = 0;
  uint32_t address[kMaxZones];
  uint32_t bytes[kMaxZones];
  bool Aggregate(uint32_t d, uint32_t b) {
    if (n == kMaxZones || total + b > kMaxBytesZones || uint64_t(d) + b > 0xE0000000ull) {
      return false;
    }
    address[n] = d;
    bytes[n] = b;
    ++n;
    total += b;
    return true;
  }
  void Copy(uint8_t* base, uint8_t* target) const {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; ++i) {
      std::memcpy(target + o, REX_RAW_ADDR(address[i]), bytes[i]);
      o += bytes[i];
    }
  }
  void Restore(uint8_t* base, const uint8_t* source) const {
    uint32_t o = total;
    for (uint32_t i = n; i-- > 0;) {
      o -= bytes[i];
      std::memcpy(REX_RAW_ADDR(address[i]), source + o, bytes[i]);
    }
  }
  uint64_t Sum(uint8_t* base) const {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* p = REX_RAW_ADDR(address[i]);
      uint32_t k = 0;
      for (; k + 8 <= bytes[i]; k += 8) {
        uint64_t w;
        std::memcpy(&w, p + k, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
      }
      for (; k < bytes[i]; ++k) {
        h = (h ^ p[k]) * 0x100000001B3ull;
      }
      h = (h ^ address[i]) * 0x100000001B3ull;
    }
    return h;
  }
};

[[gnu::always_inline]] inline bool Overlaps(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
  return uint64_t(a) < uint64_t(b) + m && uint64_t(b) < uint64_t(a) + n;
}

// The areas that the copy and the native version write in a dry run (without calls), computed from what is
// in memory. If anything that determines a write address could change because of an earlier write
// (overlaps with the stack or with the mask buffers) or the buffers cannot be bounded, that call is not
// checked (only the original runs).
bool ComputeZones(const PPCContext& ctx, uint8_t* base, Zones& z) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t yo = ctx.r3.u32;
  const uint32_t pass = ctx.r4.u32;
  if (stack < 0x10000u || yo < 0x10000u || pass < 0x10000u) {
    return false;
  }
  const uint32_t w = stack - kFrame;
  const auto in_stack = [&](uint32_t d, uint32_t n) { return Overlaps(d, n, w, kStackWatched); };
  if (in_stack(pass + 8, 12) || in_stack(yo, 704) || !z.Aggregate(w, kStackWatched)) {
    return false;
  }
  const uint32_t r25 = L32(base, pass + 8);
  const uint32_t previous = L32(base, yo + 540);
  bool reset = true;
  if (previous != 0) {
    if (r25 == 0) {
      reset = false;
    } else {
      if (in_stack(previous + 8, 4)) {
        return false;
      }
      reset = r25 != L32(base, previous + 8);
    }
  }
  uint32_t r1a = 0, r1n = 0, r2a = 0, r2n = 0;
  if (reset) {
    const uint32_t n1 = L32(base, yo + 288);
    if (n1 > 32) {  // 16 blocks of 16 at most: they do not reach [this+256] or [this+288]
      return false;
    }
    r1a = yo & ~127u;
    uint32_t fin = r1a + 128;
    if (n1 != 0) {
      const uint32_t last = ((yo + 16 * ((n1 + 1) / 2 - 1)) & ~0xFu) + 16;
      fin = last > fin ? last : fin;
    }
    r1n = fin - r1a;
    if (in_stack(r1a, r1n) || Overlaps(r1a, r1n, kGlobal, 4) || !z.Aggregate(r1a, r1n)) {
      return false;
    }
    // The second buffer is always snapshotted when there is a reset (snapshotting too much changes nothing).
    const uint32_t p = L32(base, yo + 256);
    const uint32_t n2 = L32(base, yo + 292);
    if (n2 > 128) {
      return false;
    }
    r2a = p & ~127u;
    fin = r2a + 128;
    if (n2 != 0) {
      const uint32_t last = ((p + 16 * ((n2 + 1) / 2 - 1)) & ~0xFu) + 16;
      fin = last > fin ? last : fin;
    }
    r2n = fin - r2a;
    if (r2a < 0x10000u || in_stack(r2a, r2n) || Overlaps(r2a, r2n, yo + 256, 4) || Overlaps(r2a, r2n, yo + 292, 4) ||
        Overlaps(r2a, r2n, yo + 700, 4) || Overlaps(r2a, r2n, yo + 692, 4) || Overlaps(r2a, r2n, pass + 8, 12) ||
        !z.Aggregate(r2a, r2n)) {
      return false;
    }
  }
  const auto in_buffers = [&](uint32_t d, uint32_t n) {
    return in_stack(d, n) || (reset && (Overlaps(d, n, r1a, r1n) || Overlaps(d, n, r2a, r2n)));
  };
  // The vertex shader refcount: [[r25+72]] + 4
  if (in_buffers(r25 + 72, 8)) {
    return false;
  }
  const uint32_t q = L32(base, r25 + 72);
  if (in_buffers(q, 4)) {
    return false;
  }
  const uint32_t vs = L32(base, q);
  if (vs != 0 && !z.Aggregate(vs + 4, 4)) {
    return false;
  }
  // [dev+20456]
  const uint32_t dev = L32(base, yo + 700);
  if (!z.Aggregate(dev + 20456, 4)) {
    return false;
  }
  // The OR masks and the end
  return z.Aggregate((yo + 320) & ~0xFu, 16) && z.Aggregate((yo + 384) & ~0xFu, 128) && z.Aggregate(yo + 532, 8);
}

struct Recording {
  uint32_t n;  // calls made (beyond kMaxCalls only the first ones are kept and nothing is compared)
  Photo calls[kMaxCalls];
  Photo output;
};
// Guard state. Global and not per thread (on the Switch a large TLS is paid by every thread in the
// process): one thread uses it at a time (g_checking); if another thread arrived at the same time, that
// call goes through the original only.
std::atomic<bool> g_checking{false};
Recording* t_recording = nullptr;
const Zones* t_zones = nullptr;
uint8_t* t_base = nullptr;
Recording g_copy;
Recording g_native;
uint8_t g_before[kMaxBytesZones];
uint8_t g_mem_copy[kMaxBytesZones];
uint8_t g_mem_native[kMaxBytesZones];

[[gnu::noinline]] void Record(PPCContext& ctx, uint32_t que, uint32_t target) {
  Recording& g = *t_recording;
  const uint32_t k = g.n++;
  if (k < kMaxCalls) {
    Photo& f = g.calls[k];
    Photograph(ctx, f);
    f.que = que;
    f.target = target;
    f.memory_block = t_zones->Sum(t_base);
  }
  Dirty(ctx, k);
}

// Dry run: nothing is called; what the call would see is recorded and the volatiles are dirtied.
struct Recorder {
  static void Call(PPCContext& ctx, uint8_t* base, uint32_t que) { Record(ctx, que, 0); }
  static void Indirect(PPCContext& ctx, uint8_t* base, uint32_t target) { Record(ctx, kIndirect, target); }
};

// ==== LITERAL COPY of sub_82448E80 (tools/literal_copy.py): the generated code without comments; the calls
// ==== go through the policy. Do not edit by hand: the tool rebuilds it from the current generated code.
#define NFSMW_PASS_CALL(c, b, que) Calls::Call(c, b, que)
#define NFSMW_PASS_INDIRECT(c, b, target) Calls::Indirect(c, b, target)
#include "copies_literal/CopyOriginal.inc"
#undef NFSMW_PASS_CALL
#undef NFSMW_PASS_INDIRECT
// ==== END OF THE LITERAL COPY

// ---------------------------------------------------------------------------------------------------------------
// Counters, report, guard and call.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kChecks = 20000;   // first calls checked
constexpr uint64_t kMinimumOdd = 2000;        // and the first ones of each rare path
constexpr uint64_t kWindowOdd = 5000000;   // (as long as they do not exceed these calls: then 1 of every kPeriod)
constexpr uint64_t kPeriod = 4096;           // then 1 of every kPeriod (a power of 2)
constexpr uint64_t kMeasurementOriginal = 512;     // 1 of every 512 goes through the timed original (a power of 2)

enum Reason : uint32_t { kByOff = 0, kByCheck = 1, kByMeasurement = 2, kReasons = 3 };

// Counters without read-modify-write atomics (A57 without LSE): written by the Main XThread; if another
// thread counted at the same time some count would be lost, which does not matter for the report.
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_by_combination[kCombinations];  // native calls in the period, per combination of paths
std::atomic<uint64_t> g_original[kReasons];             // in the period
std::atomic<uint64_t> g_ns_native{0}, g_samples_native{0};
std::atomic<uint64_t> g_ns_original{0}, g_samples_original{0};
std::atomic<uint64_t> g_checked{0};                   // since startup, all without differences
std::atomic<uint64_t> g_checked_path[kPaths];
std::atomic<uint64_t> g_skipped{0};                      // guards not compared (unbounded areas, too many calls)
std::atomic<uint32_t> g_odd_ready{0};                  // paths odd con kMinimumOdd checked
std::atomic<bool> g_odd_open{true};
std::atomic<bool> g_off{false};
std::atomic<int64_t> g_next_ms{0};
std::atomic<int8_t> g_active{-1};

template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline bool Active() {
  int8_t a = g_active.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_effect_pass_native) ? 1 : 0;
    g_active.store(a, std::memory_order_relaxed);
  }
  return a != 0;
}

void Report() {
  const int64_t now = NowNs() / 1000000;
  const int64_t next = g_next_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_next_ms.store(now + 10000, std::memory_order_relaxed);
  if (next == 0) {
    REXLOG_INFO("[effect_pass] start de pass de effect (sub_82448E80) {}",
                Active() ? "en native (build 185): begins checking contra la copy literal de la original"
                         : "por la original (nfsmw_effect_pass_native = false)");
    return;
  }
  uint64_t by_path[kPaths] = {};
  uint64_t native = 0;
  for (uint32_t c = 0; c < kCombinations; ++c) {
    const uint64_t k = g_by_combination[c].exchange(0, std::memory_order_relaxed);
    native += k;
    for (uint32_t b = 0; b < kPaths; ++b) {
      if (c & (1u << b)) {
        by_path[b] += k;
      }
    }
  }
  uint64_t original[kReasons];
  uint64_t total_original = 0;
  for (uint32_t m = 0; m < kReasons; ++m) {
    original[m] = g_original[m].exchange(0, std::memory_order_relaxed);
    total_original += original[m];
  }
  const uint64_t ns_n = g_ns_native.exchange(0, std::memory_order_relaxed);
  const uint64_t k_n = g_samples_native.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_o = g_ns_original.exchange(0, std::memory_order_relaxed);
  const uint64_t k_o = g_samples_original.exchange(0, std::memory_order_relaxed);
  const double us_n = k_n ? double(ns_n) / double(k_n) / 1000.0 : 0.0;
  const double us_o = k_o ? double(ns_o) / double(k_o) / 1000.0 : 0.0;
  const double calls_s = double(native + total_original) / 10.0;
  const double saving_ms_s = (k_n && k_o) ? (us_o - us_n) * double(native) / 10.0 / 1000.0 : 0.0;
  NFSMW_REPORT_DEFERRED(
      "[effect_pass] last 10 s: {:.0f} calls/s; {} en native ({} {}, {} {}, {} {}, {} {}, {} {}, {} {}), {} por "
      "la original (off {}, check {}, measurement {}); native {:.3f} us/call ({} cronometradas), original "
      "{:.3f} us/call ({}): saving {:.2f} ms/s; checked contra la copy literal de la original since el arranque: "
      "{} (reset {}, second buffer {}, release {}, states {}, sampling {}, masks {}; sin compare {}){}",
      calls_s, native, kNamesPath[0], by_path[0], kNamesPath[1], by_path[1], kNamesPath[2],
      by_path[2], kNamesPath[3], by_path[3], kNamesPath[4], by_path[4], kNamesPath[5],
      by_path[5], total_original, original[kByOff], original[kByCheck], original[kByMeasurement],
      us_n, k_n, us_o, k_o, saving_ms_s, g_checked.load(std::memory_order_relaxed),
      g_checked_path[0].load(std::memory_order_relaxed), g_checked_path[1].load(std::memory_order_relaxed),
      g_checked_path[2].load(std::memory_order_relaxed), g_checked_path[3].load(std::memory_order_relaxed),
      g_checked_path[4].load(std::memory_order_relaxed), g_checked_path[5].load(std::memory_order_relaxed),
      g_skipped.load(std::memory_order_relaxed), g_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "");
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

// The first difference between two snapshots ("" if they are equal).
std::string Difference(const Photo& a, const Photo& b) {
  if (a.que != b.que || a.target != b.target) {
    return fmt::format("call {}/0x{:08X} front a {}/0x{:08X}", a.que, a.target, b.que, b.target);
  }
  for (uint32_t i = 0; i < 14; ++i) {
    if (a.r[i] != b.r[i]) {
      return fmt::format("{} 0x{:X} front a 0x{:X}", kNamesR[i], a.r[i], b.r[i]);
    }
  }
  if (a.lr != b.lr) {
    return fmt::format("lr 0x{:X} front a 0x{:X}", a.lr, b.lr);
  }
  for (uint32_t i = 0; i < 14; ++i) {
    if (a.f[i] != b.f[i]) {
      return fmt::format("f{} 0x{:016X} front a 0x{:016X}", i, a.f[i], b.f[i]);
    }
  }
  for (uint32_t i = 0; i < 14; ++i) {
    if (std::memcmp(a.v + 16 * i, b.v + 16 * i, 16) != 0) {
      return fmt::format("v{} {} front a {}", i, Hex(a.v + 16 * i, 16), Hex(b.v + 16 * i, 16));
    }
  }
  if (a.csr != b.csr) {
    return fmt::format("FPCR 0x{:X} front a 0x{:X}", a.csr, b.csr);
  }
  if (a.last_indirect != b.last_indirect) {
    return fmt::format("last indirect 0x{:X} front a 0x{:X}", a.last_indirect, b.last_indirect);
  }
  if (a.memory_block != b.memory_block) {
    return fmt::format("la memory_block written until ahi (sum 0x{:016X} front a 0x{:016X})", a.memory_block, b.memory_block);
  }
  return std::string();
}

// Guard: the literal copy and the native version run dry, each is undone, they are compared, and then the
// original runs for real.
[[gnu::noinline]] void Check(PPCContext& ctx, uint8_t* base) {
  Zones z;
  if (g_checking.exchange(true, std::memory_order_acquire)) {  // another thread is in the guard
    Add(g_skipped, uint64_t(1));
    Add(g_original[kByCheck], uint64_t(1));
    __imp__sub_82448E80(ctx, base);
    return;
  }
  if (!ComputeZones(ctx, base, z)) {
    g_checking.store(false, std::memory_order_release);
    Add(g_skipped, uint64_t(1));
    Add(g_original[kByCheck], uint64_t(1));
    __imp__sub_82448E80(ctx, base);
    return;
  }
  uint8_t* const before = g_before;
  uint8_t* const mem_copy = g_mem_copy;
  uint8_t* const mem_native = g_mem_native;
  Recording& copy = g_copy;
  Recording& native = g_native;
  z.Copy(base, before);
  const PPCContext entry = ctx;
  const auto volver = [&]() {
    z.Restore(base, before);
    ctx = entry;
    ctx.fpscr.setcsr(ctx.fpscr.csr);  // the real FPCR, like the copy's
  };
  t_zones = &z;
  t_base = base;
  copy.n = 0;
  t_recording = &copy;
  CopyOriginal<Recorder>(ctx, base);
  Photograph(ctx, copy.output);
  z.Copy(base, mem_copy);
  volver();
  native.n = 0;
  t_recording = &native;
  const uint32_t path = Native<Recorder>(ctx, base);
  Photograph(ctx, native.output);
  z.Copy(base, mem_native);
  volver();
  t_recording = nullptr;

  std::string que;
  uint32_t where = 0;
  if (copy.n != native.n) {
    que = fmt::format("{} calls front a {}", copy.n, native.n);
  } else if (copy.n <= kMaxCalls) {
    for (uint32_t k = 0; k < copy.n && que.empty(); ++k) {
      que = Difference(copy.calls[k], native.calls[k]);
      where = k + 1;
    }
    if (que.empty()) {
      que = Difference(copy.output, native.output);
      where = 0;
    }
    if (que.empty()) {
      uint32_t o = 0;
      for (uint32_t i = 0; i < z.n && que.empty(); ++i) {
        for (uint32_t b = 0; b < z.bytes[i]; ++b) {
          if (mem_copy[o + b] != mem_native[o + b]) {
            const uint32_t m = z.bytes[i] - b < 8u ? z.bytes[i] - b : 8u;
            que = fmt::format("memory_block en 0x{:08X} (zone {} +{}): original {} native {}", z.address[i] + b, i, b,
                              Hex(mem_copy + o + b, m), Hex(mem_native + o + b, m));
            break;
          }
        }
        o += z.bytes[i];
      }
    }
  }
  const bool too_many = copy.n > kMaxCalls;
  g_checking.store(false, std::memory_order_release);
  __imp__sub_82448E80(ctx, base);  // the real original, from the entry: its state is kept
  Add(g_original[kByCheck], uint64_t(1));
  if (que.empty() && too_many) {
    Add(g_skipped, uint64_t(1));  // too many calls to compare them all
    return;
  }
  if (que.empty()) {
    const uint64_t total = g_checked.load(std::memory_order_relaxed) + 1;
    g_checked.store(total, std::memory_order_relaxed);
    for (uint32_t b = 0; b < kPaths; ++b) {
      if (path & (1u << b)) {
        const uint64_t k = g_checked_path[b].load(std::memory_order_relaxed) + 1;
        g_checked_path[b].store(k, std::memory_order_relaxed);
        if (k == kMinimumOdd && ((1u << b) & kPathsOdd) != 0) {
          g_odd_ready.store(g_odd_ready.load(std::memory_order_relaxed) | (1u << b), std::memory_order_relaxed);
          REXLOG_INFO("[effect_pass] path {}: {} calls checked contra la copy literal de la original, 0 "
                      "differences",
                      kNamesPath[b], kMinimumOdd);
        }
      }
    }
    if (total == kChecks) {
      REXLOG_INFO("[effect_pass] {} calls checked contra la copy literal de la original (en every call que "
                  "does: r0-r13, lr, f0-f13, v0-v13, FPCR, last indirect y la memory_block written; y la output, la stack, "
                  "los buffers de masks, la count del shader, el vulkan_device y the masks OR), 0 differences: "
                  "en native, y sigue checking 1 de every {}",
                  total, kPeriod);
    }
    return;
  }
  g_off.store(true, std::memory_order_relaxed);  // the state is already the original's
  REXLOG_ERROR("[effect_pass] DIFFERENCE con la original ({}{}; path 0x{:X}): entry this 0x{:08X}, pass "
               "0x{:08X}, stack 0x{:08X}. Path native OFF para el rest de la session: se queda la original",
               que, where ? fmt::format(", en la call {}", where) : std::string(", a la output"), path,
               entry.r3.u32, entry.r4.u32, entry.r1.u32);
}

// The rare paths this call will take, read from memory beforehand (it only decides whether the guard runs).
uint32_t Predict(const PPCContext& ctx, uint8_t* base) {
  const uint32_t yo = ctx.r3.u32;
  const uint32_t pass = ctx.r4.u32;
  uint32_t c = 0;
  const uint32_t r25 = L32(base, pass + 8);
  const uint32_t previous = L32(base, yo + 540);
  if (previous == 0 || (r25 != 0 && r25 != L32(base, previous + 8))) {
    if (L32(base, kGlobal) == 0 || L32(base, yo + 696) == 0) {
      c |= kPathSecond;
    }
  }
  if (L32(base, L32(base, pass + 12) + 16) != 0) {
    c |= kPathStates;
  }
  if ((L32(base, yo + 692) & 3u) != 0) {
    c |= kPathMasks;
  }
  return c;
}

}  // namespace

void StartPass(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_calls.load(std::memory_order_relaxed) + 1;
  g_calls.store(n, std::memory_order_relaxed);
  if ((n & (kPeriod - 1)) == 0) [[unlikely]] {
    Report();
  }
  if (!Active() || g_off.load(std::memory_order_relaxed)) [[unlikely]] {
    Add(g_original[kByOff], uint64_t(1));
    __imp__sub_82448E80(ctx, base);
    return;
  }
  if (n <= kChecks || (n & (kPeriod - 1)) == 0) [[unlikely]] {
    Check(ctx, base);
    return;
  }
  if (g_odd_open.load(std::memory_order_relaxed)) [[unlikely]] {
    const uint32_t pending_2 = kPathsOdd & ~g_odd_ready.load(std::memory_order_relaxed);
    if (pending_2 == 0 || n > kWindowOdd) {
      g_odd_open.store(false, std::memory_order_relaxed);
    } else if ((Predict(ctx, base) & pending_2) != 0) {
      Check(ctx, base);
      return;
    }
  }
  if ((n & (kMeasurementOriginal - 1)) == kMeasurementOriginal / 2) [[unlikely]] {
    const int64_t t0 = NowNs();
    __imp__sub_82448E80(ctx, base);
    Add(g_ns_original, uint64_t(NowNs() - t0));
    Add(g_samples_original, uint64_t(1));
    Add(g_original[kByMeasurement], uint64_t(1));
    return;
  }
  if ((n & 15) == 8) [[unlikely]] {
    const int64_t t0 = NowNs();
    const uint32_t c = Native<Real>(ctx, base);
    Add(g_ns_native, uint64_t(NowNs() - t0));
    Add(g_samples_native, uint64_t(1));
    Add(g_by_combination[c], uint64_t(1));
    return;
  }
  Add(g_by_combination[Native<Real>(ctx, base)], uint64_t(1));
}

}  // namespace nfsmw::effect_pass_native

// The four calls in the generated code go to sub_82448E80 (the patch turns them from __imp__ back to sub_),
// and the dispatch table of the indirect calls already points here.
REX_HOOK_RAW(sub_82448E80) {  // start of an effect pass
  nfsmw::effect_pass_native::StartPass(ctx, base);
}
