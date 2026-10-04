// nfsmw - the draw glue in native code: sub_82452730 (with 8245_2690 and 8244_ED58 inside, as LTO used to
// inline them) and the draw-list loop that calls it (sub_82454B50).
//
// What it is (PowerPC read instruction by instruction: nfsmw_recomp.80.cpp, .36, .16 and .41)
//   The Main XThread walks each sorted draw list with sub_82454B50 (r3 = the view, r4 = the list: the count at
//   [list+0] and 8-byte entries from list+8 with the index at +4). For each entry it calls sub_82452730 with
//   r3 = the object (52 bytes at 829A_FA64 + 52 x index), r4 = the view, r5/r6 = two words of the loop's frame
//   (the last effect state). sub_82452730 (frame of 112):
//   1. E = [obj+12]; if [E+24] and the flags [obj+8] (0x400/0x800) or [r5]/[r6] change -> 8244_EA48 (effect
//      state change, which ends in the native pass start). Stores both in [r6]/[r5].
//   2. Depending on [vista+5]: 8245_3E20 or 8245_3D60 (material, matrices...), with 9 arguments (the 9th at
//      r1+84).
//   3. 8245_2690 (frame of 128): the vertex streams of A = [obj+0] (SetStreamSource, up to 6, 32-byte entries
//      from A+68; flag 0x8000 of [A+94+32i] marks the last one) and the index buffer M+32 with M = [[obj+4]]
//      (SetIndices, only if the global index buffer 82A2_D15C changes). Returns ([A+24] - [M+24]) / 2.
//   4. 8244_ED58 (frame of 112): depending on [E+6], effect parameters (sub_826992F0) and DrawIndexedVertices
//      (sub_82593C50) with the second draw 8244_EDF8 if 0x10, or the virtual call [[E]+20] if 0x20.
//
// Why native
//   In a stack sampling run (race), sub_82452730 plus what LTO inlined into it took 6.73 points of a core in
//   self time (1.8 ms per frame at 36.9 FPS): 5.6 belong to sub_82452730, 8245_2690 and 8244_ED58, and 4.3 are
//   cache-missing loads on the first touch of the object, A, M and E. The Xbox 360 game already prefetched
//   them with dcbt: two in the loop (the next object and the list 128 bytes ahead) and three here (A, [obj+24]
//   and [obj+4]), but the recompiler leaves dcbt as a comment (238 in the whole game). Here they come back as
//   PRFM (cache hints: they neither load nor fault), for the Xbox's 128-byte block (two 64-byte A57 lines) or
//   for the lines about to be read, plus two more hints: E on entry and M as soon as it is known. It also
//   works in registers: no context shuffling and no reloads of r1 after every guest store.
//
// Why it is identical
//   - No floating point: integers, copies and one mulhw. It does not touch the FPCR.
//   - Memory: the same stores, with the same bytes and in the same order relative to loads and calls: the lr
//     and the two zeros of r30/r31 that the generated code puts on the stack, the three stwu, r1+84, [r6] and
//     [r5], the global index buffer, and in the loop the two zeros of its frame, the global and [list+0].
//     Everything the original reads again after a call ([obj+N], [[obj+4]], [A+94+32i], [E+6], the list, the
//     saved lr...) is reread here at the same point. There are no extra loads: the hints come from loads the
//     original makes at that same point.
//   - Registers: before each call and on exit, r3-r12, lr and r1 are exactly as in the original, with all 64
//     bits (the loop's r3 carries the high part of 829A_FA64 as a negative number; r1 is written through its
//     low half like the generated code), and after each call only what the original writes is written.
//     Non-volatile r13-r31, cr, xer and ctr are local variables of the generated code; r0, r2, r13, f and v
//     are not touched.
//   - The virtual call uses the same macro as the generated code (nfsmw_pch.h): same dispatch and same last
//     indirect target.
//   Note: the native version contains 8245_2690 and 8244_ED58 (it does not call them). If either of them is
//   ever hooked, that hook would not be used on this path: the native version must be redone or turned off
//   (the patch refuses to apply if they are already hooked). And if a codegen changes any of the four
//   functions, run montar.py and review.
//
// Self-checking guard (cvar nfsmw_glue_native; the project's standard guard)
//   Both call functions whose effects cannot be repeated (draws, SetIndices, effects...), so the guard compares
//   the native version with a literal copy of the original (the current generated code as is, without
//   comments, with its calls going through a recorder; literal_copy.py, and the patch checks that it equals
//   the generated code). Both run dry: each call records r0-r13, lr, f0-f13, v0-v13, FPCR, the last indirect
//   target and a sum of the memory written, and clobbers the volatile registers as a real function would. The
//   regions they write are snapshotted first, each dry run is undone, the calls, the exit state and the memory
//   are compared byte by byte, and then the original runs for real (its state is always the one kept). Glue:
//   the first kChecks calls, the first kMinimumOdd of each rare path (view 8245_3E20, 2 or more
//   streams, flags, virtual, second draw) and then 1 in every kPeriod. Loop: the first kChecksLoop
//   and then 1 in every 4096 (lists with more than kMaxLoop draws are not compared; the loop picks them at
//   random, not with a mask, because it is called almost the same number of times every frame). A mismatch
//   writes "[glue] DIFFERENCE" (REXLOG_ERROR) and turns that native version off for the run.
//   Measured in the same run: the glue times 1 in 16 native calls and 1 in 64 through the original, and the
//   loop 1 in 8 and 1 in 32 at random, per draw; the two "[glue]" lines every 10 s give the savings in
//   ms/s (the loop one is approximate: each draw includes everything it calls, about 8 us).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"
#include <rex/platform.h>
// The generated code's macros (REX_RAW_ADDR, REX_LOAD_*, REX_STORE_*, REX_CALL_INDIRECT_FUNC...): the native
// version dispatches the virtual call exactly like the original, and the guard's literal copy compiles as is.
#include "generated/default/nfsmw_pch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_glue_native, true, "NFSMW",
                    "El glue de draw (sub_82452730 con los flows, los indices y el draw de every object) y el "
                    "loop de la list que lo llama (sub_82454B50) en native (build 186), identical y con the pistas de "
                    "cache (dcbt) del game de Xbox 360. Se comprueban en dry contra one copy literal de la original y "
                    "se apagan solos si difieren; false = the original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82452730);  // the original glue
REX_EXTERN(__imp__sub_82454B50);  // the original loop
REX_EXTERN(sub_82452730);         // this file's hook: the loop calls through it, like the original after the patch
// The hooks the original calls, by name (nfsmw_d3d_trace.cpp and nfsmw_d3d_registers_native.cpp).
REX_EXTERN(sub_8258D968);  // SetStreamSource
REX_EXTERN(sub_8258DA60);  // SetIndices
REX_EXTERN(sub_826992F0);  // parametros de effect
REX_EXTERN(sub_82593C50);  // DrawIndexedVertices
// The rest, as the generated code calls them (8244_EA48 through its sub_ alias, the others directly). The
// name is built in pieces on purpose: tools/direct_calls.py treats any address that appears whole in
// app/src as hooked, and their calls from the rest of the game must stay direct (__imp__, inlinable).
#define NFSMW_GLUE_JOIN_(a, b) a##b
#define NFSMW_GLUE_CHANGE_STATE NFSMW_GLUE_JOIN_(sub_8244, EA48)
#define NFSMW_PEGAMENTO_VISTA_E20 NFSMW_GLUE_JOIN_(__imp__sub_8245, 3E20)
#define NFSMW_PEGAMENTO_VISTA_D60 NFSMW_GLUE_JOIN_(__imp__sub_8245, 3D60)
#define NFSMW_GLUE_SECOND_DRAW NFSMW_GLUE_JOIN_(__imp__sub_8244, EDF8)
REX_EXTERN(NFSMW_GLUE_CHANGE_STATE);
REX_EXTERN(NFSMW_PEGAMENTO_VISTA_E20);
REX_EXTERN(NFSMW_PEGAMENTO_VISTA_D60);
REX_EXTERN(NFSMW_GLUE_SECOND_DRAW);

namespace nfsmw::glue_native {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: REX_RAW_ADDR from nfsmw_pch.h, without volatile (C++ itself gives the ordering: accesses are
// not reordered across an opaque call or a store that may alias).
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline uint32_t L8(uint8_t* base, uint32_t d) {
  return *REX_RAW_ADDR(d);
}
[[gnu::always_inline]] inline uint32_t L16(uint8_t* base, uint32_t d) {
  uint16_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 2);
  return __builtin_bswap16(v);
}
[[gnu::always_inline]] inline uint32_t L32(uint8_t* base, uint32_t d) {
  uint32_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 4);
  return __builtin_bswap32(v);
}
[[gnu::always_inline]] inline uint64_t L64(uint8_t* base, uint32_t d) {
  uint64_t v;
  std::memcpy(&v, REX_RAW_ADDR(d), 8);
  return __builtin_bswap64(v);
}
[[gnu::always_inline]] inline void E32(uint8_t* base, uint32_t d, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(REX_RAW_ADDR(d), &v, 4);
}
[[gnu::always_inline]] inline void E64(uint8_t* base, uint32_t d, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(REX_RAW_ADDR(d), &v, 8);
}
// Cache hint (PRFM PLDL1KEEP on the Switch) for the 64-byte line of d: it neither loads nor faults, even if
// d is invalid.
[[gnu::always_inline]] inline void Anticipate(uint8_t* base, uint32_t d) {
  __builtin_prefetch(REX_RAW_ADDR(d));
}
// The Xbox 360 dcbt: the 128-byte block that contains d (two A57 lines).
[[gnu::always_inline]] inline void AnticipateBlock(uint8_t* base, uint32_t d) {
  const uint32_t b = d & ~127u;
  __builtin_prefetch(REX_RAW_ADDR(b));
  __builtin_prefetch(REX_RAW_ADDR(b + 64u));
}
// A zero the compiler cannot remove and the CPU only has once v has arrived (an instruction with a real data
// dependency): the hints it is added to issue after that load and do not compete with its cache miss.
[[gnu::always_inline]] inline uint32_t ZeroAfterRead(uint32_t v) {
#if defined(__aarch64__)
  uint32_t c;
  asm("and %w0, %w1, wzr" : "=r"(c) : "r"(v));
  return c;
#elif defined(__x86_64__)
  uint32_t c = v;
  asm("andl $0, %0" : "+r"(c));  // (tested on the PC; "and" is not a zeroing idiom: it waits for the data)
  return c;
#else
  return v & 0u;
#endif
}
// A list object (52 bytes; it may cross a line).
[[gnu::always_inline]] inline void AnticipateObject(uint8_t* base, uint32_t d) {
  __builtin_prefetch(REX_RAW_ADDR(d));
  __builtin_prefetch(REX_RAW_ADDR(d + 51u));
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (as the PowerPC builds them: lis/addi) and the return address of each call (ctx.lr in the
// generated code).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kLis32093 = uint64_t(int64_t(-32093) * 65536);       // lis rX,-32093 (sign-extended to 64 bits)
constexpr uint32_t kDevice = uint32_t(kLis32093) - 12448u;          // lwz r3,-12448(rX): the D3D device
constexpr uint32_t kIndicesCurrent = uint32_t(kLis32093) - 11940u;      // lwz/stw rY,-11940(rX): the current index buffer
constexpr uint64_t kObjectsMinus4 = uint64_t(int64_t(-32101) * 65536 - 1440);  // r29 of the loop: lis -32101; addi -1440

constexpr uint32_t kFrame = 112;        // sub_82452730: stwu r1,-112(r1)
constexpr uint32_t kFrameFlows = 128;  // 8245_2690
constexpr uint32_t kFrameDraw = 112;  // 8244_ED58
constexpr uint32_t kFrameLoop = 144;   // sub_82454B50
constexpr uint32_t kStackWatched = kFrame + kFrameFlows;  // [r1-240, r1): what the three write on the stack

// sub_82452730
constexpr uint64_t kLapState = 0x824527C8;
constexpr uint64_t kLapE20 = 0x82452808;
constexpr uint64_t kLapD60 = 0x82452814;
constexpr uint64_t kLapFlows = 0x82452824;  // the call to 8245_2690 (its mflr r12)
constexpr uint64_t kLapDraw = 0x8245284C;  // the call to 8244_ED58 (its mflr r12)
// 8245_2690
constexpr uint64_t kLapFlow = 0x824526D0;
constexpr uint64_t kLapIndices = 0x82452714;
// 8244_ED58
constexpr uint64_t kLapEffect = 0x8244ED88;
constexpr uint64_t kLapDraw = 0x8244EDA8;
constexpr uint64_t kLapSecond = 0x8244EDC8;
constexpr uint64_t kLapVirtual = 0x8244EDEC;
// sub_82454B50
constexpr uint64_t kLapLoopPrologue = 0x82454B58;  // bl __savegprlr_26 (the generated code does not run it)
constexpr uint64_t kLapGlue = 0x82454BE4;

enum What : uint32_t {
  kChangeState = 0,  // 8244_EA48
  kVistaE20 = 1,      // 8245_3E20
  kVistaD60 = 2,      // 8245_3D60
  kFlow = 3,         // SetStreamSource
  kIndices = 4,       // SetIndices
  kEffect = 5,        // parametros de effect
  kDraw = 6,       // DrawIndexedVertices
  kSecond = 7,       // 8244_EDF8
  kIndirect = 8,     // [[E]+20]
  kGlue = 9,     // the loop: sub_82452730
};

enum Path : uint32_t {
  kPathState = 1u << 0,     // effect state change (8244_EA48)
  kPathFlags = 1u << 1,   // [E+24] != 0: the [obj+8] flags are checked
  kPathViewE20 = 1u << 2,   // [vista+5] != 0: 8245_3E20 instead of 8245_3D60
  kPathFlows = 1u << 3,     // two or more vertex streams
  kPathIndices = 1u << 4,    // changes the index buffer: SetIndices
  kPathVirtual = 1u << 5,    // [E+6] & 0x20: the virtual call instead of the effect and the draw
  kPathSecond = 1u << 6,    // [E+6] & 0x10: the second draw
  kPaths = 7,
  kCombinations = 1u << kPaths
};
constexpr const char* kNamesPath[kPaths] = {"change de state", "flags", "vista E20", "2+ flows",
                                                  "SetIndices",       "virtual",  "second draw"};
constexpr uint32_t kPathsOdd = kPathFlags | kPathViewE20 | kPathFlows | kPathVirtual | kPathSecond;

// ---------------------------------------------------------------------------------------------------------------
// The real calls: the same functions the original calls, through the same path.
// ---------------------------------------------------------------------------------------------------------------
struct Real {
  [[gnu::always_inline]] static inline void Call(PPCContext& ctx, uint8_t* base, uint32_t que) {
    switch (que) {
      case kChangeState: NFSMW_GLUE_CHANGE_STATE(ctx, base); break;
      case kVistaE20: NFSMW_PEGAMENTO_VISTA_E20(ctx, base); break;
      case kVistaD60: NFSMW_PEGAMENTO_VISTA_D60(ctx, base); break;
      case kFlow: sub_8258D968(ctx, base); break;
      case kIndices: sub_8258DA60(ctx, base); break;
      case kEffect: sub_826992F0(ctx, base); break;
      case kDraw: sub_82593C50(ctx, base); break;
      default: NFSMW_GLUE_SECOND_DRAW(ctx, base); break;
    }
  }
  [[gnu::always_inline]] static inline void Indirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    REX_CALL_INDIRECT_FUNC(target);
  }
  [[gnu::always_inline]] static inline void Glue(PPCContext& ctx, uint8_t* base) {
    sub_82452730(ctx, base);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// The whole sub_82452730, with 8245_2690 and 8244_ED58 inlined. L = Real (real calls) or Recorder (the
// guard, dry run). Returns the path (kPath*).
// ---------------------------------------------------------------------------------------------------------------
template <class L>
uint32_t Native(PPCContext& ctx, uint8_t* base) {
  uint32_t path = 0;
  // --- Prologue: mflr r12; stw r12,-8(r1); std r30,-24(r1); std r31,-16(r1); stwu r1,-112(r1) ---
  const uint64_t lr_entry = ctx.lr;
  ctx.r12.u64 = lr_entry;
  uint64_t r1 = ctx.r1.u64;
  E32(base, uint32_t(r1) - 8u, uint32_t(lr_entry));
  E64(base, uint32_t(r1) - 24u, 0);  // r30 and r31 of the generated code are zeroed local variables
  E64(base, uint32_t(r1) - 16u, 0);
  {
    const uint32_t ea = uint32_t(r1) - kFrame;
    E32(base, ea, uint32_t(r1));
    r1 = (r1 & 0xFFFFFFFF00000000ull) | ea;  // ctx.r1.u32 = ea: the high half is kept
  }
  const uint64_t r31 = ctx.r3.u64;  // mr r31,r3: the object
  const uint64_t r30 = ctx.r4.u64;  // mr r30,r4: la vista
  const uint32_t obj = uint32_t(r31);
  const uint64_t a0 = L32(base, obj + 0);   // lwz r11,0(r31): A
  const uint64_t x0 = L32(base, obj + 24);  // lwz r10,24(r31)
  const uint64_t e0 = L32(base, obj + 12);  // lwz r3,12(r31): E
  const uint64_t r9a = L32(base, uint32_t(e0) + 24);  // lwz r9,24(r3)
  {
    // The hints issue when [E+24] arrives, since it is needed right away: if they issued earlier (or at the
    // same time), their misses would compete with its miss (the PC benchmark measured it: 40-70 extra cycles
    // on entry). What they request is not read until after 8245_3E20/3D60, so waiting for [E+24] costs them
    // nothing. ZeroAfterRead: a zero with a real dependency on the data.
    const uint32_t zero = ZeroAfterRead(uint32_t(r9a));
    const uint32_t da = uint32_t(a0) + zero, dx = uint32_t(x0) + zero, de = uint32_t(e0) + zero;
    // The game's dcbt r0,r11. Here, the lines from A+0 to A+127: A+24, A+36, A+40 and the first two streams
    // (A+68..A+127).
    Anticipate(base, da);
    Anticipate(base, da + 64u);
    Anticipate(base, da + 127u);
    Anticipate(base, dx);        // the game's dcbt r0,r10: matrix A of the matrices (64 bytes)
    Anticipate(base, dx + 63u);
    Anticipate(base, de);        // (new hint) E+0 and E+6, read by 8244_ED58 and by what 8245_3D60 calls
  }
  uint64_t r7 = ctx.r7.u64;
  uint64_t r8 = ctx.r8.u64;
  uint64_t r11;
  if (uint32_t(r9a) == 0) {  // cmplwi cr6,r9,0; beq loc_82452790
    r11 = 0;                 // li r11,0
  } else {
    path |= kPathFlags;
    const uint64_t flags = L32(base, obj + 8);  // lwz r11,8(r31)
    r8 = uint32_t(flags) & 0x400u;              // rlwinm r8,r11,0,21,21
    if (r8 != 0) {                                 // bne loc_82452788
      r11 = 1;
    } else {
      r7 = uint32_t(flags) & 0x800u;  // rlwinm r7,r11,0,20,20
      r11 = r7 != 0 ? 1 : 0;             // beq loc_82452790 / li r11,1
    }
  }
  // --- loc_82452794: the loop's last effect state ---
  const uint32_t p5 = ctx.r5.u32;
  const uint32_t p6 = ctx.r6.u32;
  const uint64_t r4a = L32(base, p5);   // lwz r4,0(r5)
  uint64_t r10 = L32(base, p6);         // lwz r10,0(r6)
  uint64_t r9 = r4a - e0;               // subf r9,r3,r4
  E32(base, p6, uint32_t(r11));         // stw r11,0(r6)
  r10 = r10 - r11;                      // subf r10,r11,r10
  E32(base, p5, uint32_t(e0));          // stw r3,0(r5)
  r9 = r10 | r9;                        // or r9,r10,r9
  if (int32_t(uint32_t(r9)) != 0) {     // cmpwi cr6,r9,0; beq loc_824527C8
    path |= kPathState;
    ctx.r3.u64 = e0;
    ctx.r4.u64 = 0;  // li r4,0
    ctx.r5.u64 = 0;  // li r5,0
    ctx.r6.u64 = r11;  // mr r6,r11
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
    ctx.r10.u64 = r10;
    ctx.r11.u64 = r11;
    ctx.r1.u64 = r1;
    ctx.lr = kLapState;
    L::Call(ctx, base, kChangeState);  // bl 8244_EA48
    r1 = ctx.r1.u64;
  }
  // --- loc_824527C8: the nine arguments of 8245_3E20 / 8245_3D60 ---
  const uint64_t p = L32(base, obj + 4);          // lwz r8,4(r31): P
  Anticipate(base, uint32_t(p));                   // the game's dcbt r0,r8 (only [P+0] is read)
  const uint64_t vista5 = L8(base, uint32_t(r30) + 5);  // lbz r7,5(r30)
  const uint64_t r11b = L32(base, obj + 28);      // lwz r11,28(r31)
  ctx.r8.u64 = r30;                               // mr r8,r30
  ctx.r5.u64 = r31 + 32;                          // addi r5,r31,32
  ctx.r11.u64 = r11b;
  ctx.r10.u64 = L32(base, obj + 20);              // lwz r10,20(r31)
  ctx.r9.u64 = L32(base, obj + 16);               // lwz r9,16(r31)
  ctx.r7.u64 = L32(base, obj + 24);               // lwz r7,24(r31)
  ctx.r6.u64 = L32(base, obj + 8);                // lwz r6,8(r31)
  ctx.r4.u64 = L32(base, obj + 0);                // lwz r4,0(r31)
  ctx.r3.u64 = L32(base, obj + 12);               // lwz r3,12(r31)
  E32(base, uint32_t(r1) + 84u, uint32_t(r11b));  // stw r11,84(r1)
  ctx.r1.u64 = r1;
  if (uint32_t(vista5) != 0) {  // cmplwi cr6,r7,0; beq loc_8245280C
    path |= kPathViewE20;
    ctx.lr = kLapE20;
    L::Call(ctx, base, kVistaE20);  // bl 8245_3E20
  } else {
    ctx.lr = kLapD60;
    L::Call(ctx, base, kVistaD60);  // bl 8245_3D60
  }
  r1 = ctx.r1.u64;

  // --- loc_82452814 ---
  const uint64_t p2 = L32(base, obj + 4);          // lwz r10,4(r31)
  const uint64_t a = L32(base, obj + 0);           // lwz r3,0(r31)
  const uint64_t m = L32(base, uint32_t(p2));      // lwz r4,0(r10): M
  // (new hints) [M+24] and the IB that starts at M+32, read by SetIndices and by the return of 8245_2690.
  Anticipate(base, uint32_t(m) + 24u);
  Anticipate(base, uint32_t(m) + 35u);
  ctx.r10.u64 = p2;
  // ===== 8245_2690 (bl with lr = kLapFlows): mflr r12; bl __savegprlr_27 (lr only); stwu r1,-128(r1) =====
  ctx.r12.u64 = kLapFlows;
  uint64_t r1f;
  {
    const uint32_t ea = uint32_t(r1) - kFrameFlows;
    E32(base, ea, uint32_t(r1));
    r1f = (r1 & 0xFFFFFFFF00000000ull) | ea;
  }
  const uint64_t r27 = a;  // mr r27,r3
  const uint64_t r28 = m;  // mr r28,r4
  uint64_t r30f = 0;       // li r30,0
  uint64_t r31f = r27 + 94;  // addi r31,r27,94
  for (;;) {  // loc_824526B0: one vertex stream
    const uint64_t r11f = r31f - 2;                          // addi r11,r31,-2
    const uint64_t r10f = L16(base, uint32_t(r31f));        // lhz r10,0(r31)
    ctx.r6.u64 = 0;                                          // li r6,0
    ctx.r3.u64 = L32(base, kDevice);                    // lwz r3,-12448(r29)
    ctx.r7.u64 = uint32_t(r10f) & 0x7FFFu;                   // clrlwi r7,r10,17
    ctx.r5.u64 = r11f - 24;                                  // addi r5,r11,-24
    ctx.r4.u64 = L8(base, uint32_t(r11f));                   // lbz r4,0(r11)
    ctx.r10.u64 = r10f;
    ctx.r11.u64 = r11f;
    ctx.r1.u64 = r1f;
    ctx.lr = kLapFlow;
    L::Call(ctx, base, kFlow);  // SetStreamSource, through its hook
    r1f = ctx.r1.u64;
    const uint64_t r9f = L16(base, uint32_t(r31f));  // lhz r9,0(r31)
    const uint64_t r8f = (uint64_t(uint32_t(r9f)) | (r9f << 32)) & 0xFFFF8000u;  // rlwinm r8,r9,0,0,16
    ctx.r9.u64 = r9f;
    ctx.r8.u64 = r8f;
    if (uint32_t(r8f) != 0) {  // cmplwi cr6,r8,0; bne loc_824526F0: it was the last one
      break;
    }
    path |= kPathFlows;
    r30f += 1;  // addi r30,r30,1
    r31f += 32;  // addi r31,r31,32
    if (!(int32_t(uint32_t(r30f)) < 6)) {  // cmpwi cr6,r30,6; blt loc_824526B0
      break;
    }
  }
  // --- loc_824526F0: the index buffer, if it changes ---
  const uint64_t r11g = kLis32093;                         // lis r11,-32093
  const uint64_t r4g = r28 + 32;                           // addi r4,r28,32
  uint64_t r10g = L32(base, kIndicesCurrent);             // lwz r10,-11940(r11)
  E32(base, kIndicesCurrent, uint32_t(r4g));              // stw r4,-11940(r11)
  r10g = r10g - r4g;                                       // subf r10,r4,r10
  ctx.r11.u64 = r11g;
  ctx.r4.u64 = r4g;
  ctx.r10.u64 = r10g;
  if (int32_t(uint32_t(r10g)) != 0) {  // cmpwi cr6,r10,0; beq loc_82452714
    path |= kPathIndices;
    ctx.r3.u64 = L32(base, kDevice);  // lwz r3,-12448(r29)
    ctx.r1.u64 = r1f;
    ctx.lr = kLapIndices;
    L::Call(ctx, base, kIndices);  // SetIndices, through its hook
    r1f = ctx.r1.u64;
  }
  // --- loc_82452714: la lap ---
  {
    const uint64_t r7f = L32(base, uint32_t(r27) + 24);  // lwz r7,24(r27)
    const uint64_t r6f = L32(base, uint32_t(r28) + 24);  // lwz r6,24(r28)
    const uint64_t r5f = r7f - r6f;                      // subf r5,r6,r7
    ctx.r7.u64 = r7f;
    ctx.r6.u64 = r6f;
    ctx.r5.u64 = r5f;
    ctx.r3.s64 = int32_t(uint32_t(r5f)) >> 1;            // srawi r3,r5,1
  }
  r1 = r1f + kFrameFlows;  // addi r1,r1,128 (and b __restgprlr_27: returns without touching lr)

  // ===== Back in sub_82452730 =====
  {
    const uint64_t r9b = L32(base, obj + 0);         // lwz r9,0(r31)
    const uint64_t r8b = 1431633920;                 // lis r8,21845
    const uint64_t r4b = ctx.r3.u64;                 // mr r4,r3
    const uint64_t r3b = L32(base, obj + 12);        // lwz r3,12(r31)
    const uint64_t r7b = r8b | 21846;                // ori r7,r8,21846
    uint64_t r11c = L16(base, uint32_t(r9b) + 40);   // lhz r11,40(r9)
    r11c = uint64_t((int64_t(int32_t(uint32_t(r11c))) * int64_t(int32_t(uint32_t(r7b)))) >> 32);  // mulhw r11,r11,r7
    const uint64_t r10b = __builtin_rotateleft64(uint64_t(uint32_t(r11c)) | (r11c << 32), 1) & 0x1;  // rlwinm r10,r11,1,31,31
    const uint64_t r5b = r11c + r10b;                // add r5,r11,r10
    ctx.r9.u64 = r9b;
    ctx.r8.u64 = r8b;
    ctx.r4.u64 = r4b;
    ctx.r3.u64 = r3b;
    ctx.r7.u64 = r7b;
    ctx.r11.u64 = r11c;
    ctx.r10.u64 = r10b;
    ctx.r5.u64 = r5b;
  }
  // ===== 8244_ED58 (bl with lr = kLapDraw): mflr r12; bl __savegprlr_29 (lr only); stwu r1,-112(r1) =====
  ctx.r12.u64 = kLapDraw;
  uint64_t r1d;
  {
    const uint32_t ea = uint32_t(r1) - kFrameDraw;
    E32(base, ea, uint32_t(r1));
    r1d = (r1 & 0xFFFFFFFF00000000ull) | ea;
  }
  const uint64_t r31d = ctx.r3.u64;  // mr r31,r3: E
  const uint64_t r29d = ctx.r4.u64;  // mr r29,r4
  const uint64_t r30d = ctx.r5.u64;  // mr r30,r5
  {
    const uint64_t r11d = L16(base, uint32_t(r31d) + 6);  // lhz r11,6(r31)
    const uint64_t r10d = (uint64_t(uint32_t(r11d)) | (r11d << 32)) & 0x20;  // rlwinm r10,r11,0,26,26
    ctx.r11.u64 = r11d;
    ctx.r10.u64 = r10d;
    if (uint32_t(r10d) == 0) {  // cmplwi cr6,r10,0; bne loc_8244EDD0
      ctx.r3.u64 = L32(base, uint32_t(r31d) + 28);  // lwz r3,28(r31)
      ctx.r1.u64 = r1d;
      ctx.lr = kLapEffect;
      L::Call(ctx, base, kEffect);  // effect parameters, through its hook
      r1d = ctx.r1.u64;
      ctx.r6.u64 = r29d;                             // mr r6,r29
      ctx.r5.u64 = 0;                                // li r5,0
      ctx.r4.u64 = 4;                                // li r4,4
      ctx.r3.u64 = L32(base, kDevice);          // lis r11,-32093; lwz r3,-12448(r11)
      const uint64_t r11e = __builtin_rotateleft64(uint64_t(uint32_t(r30d)) | (r30d << 32), 1) & 0xFFFFFFFE;  // rlwinm r11,r30,1,0,30
      ctx.r11.u64 = r11e;
      ctx.r7.u64 = r30d + r11e;                      // add r7,r30,r11
      ctx.r1.u64 = r1d;
      ctx.lr = kLapDraw;
      L::Call(ctx, base, kDraw);  // DrawIndexedVertices, through its hook
      r1d = ctx.r1.u64;
      const uint64_t r9e = L16(base, uint32_t(r31d) + 6);  // lhz r9,6(r31)
      const uint64_t r8e = (uint64_t(uint32_t(r9e)) | (r9e << 32)) & 0x10;  // rlwinm r8,r9,0,27,27
      ctx.r9.u64 = r9e;
      ctx.r8.u64 = r8e;
      if (uint32_t(r8e) != 0) {  // cmplwi cr6,r8,0; beq loc_8244EDEC
        path |= kPathSecond;
        ctx.r5.u64 = r30d;  // mr r5,r30
        ctx.r4.u64 = r29d;  // mr r4,r29
        ctx.r3.u64 = r31d;  // mr r3,r31
        ctx.r1.u64 = r1d;
        ctx.lr = kLapSecond;
        L::Call(ctx, base, kSecond);  // bl 8244_EDF8
        r1d = ctx.r1.u64;
      }
    } else {  // loc_8244EDD0: the virtual call [[E]+20]
      path |= kPathVirtual;
      const uint64_t r7d = L32(base, uint32_t(r31d) + 0);  // lwz r7,0(r31)
      ctx.r7.u64 = r7d;
      ctx.r5.u64 = r30d;  // mr r5,r30
      ctx.r4.u64 = r29d;  // mr r4,r29
      ctx.r3.u64 = r31d;  // mr r3,r31
      const uint64_t r6d = L32(base, uint32_t(r7d) + 20);  // lwz r6,20(r7)
      ctx.r6.u64 = r6d;
      ctx.r1.u64 = r1d;
      ctx.lr = kLapVirtual;
      L::Indirect(ctx, base, uint32_t(r6d));  // mtctr r6; bctrl
      r1d = ctx.r1.u64;
    }
  }
  r1 = r1d + kFrameDraw;  // addi r1,r1,112 (and b __restgprlr_29: returns without touching lr)
  // --- Epilogue of sub_82452730: addi r1,r1,112; lwz r12,-8(r1); mtlr r12 (ld r30/r31: unused loads) ---
  r1 = r1 + kFrame;
  ctx.r1.u64 = r1;
  ctx.r12.u64 = L32(base, uint32_t(r1) - 8u);
  ctx.lr = ctx.r12.u64;
  return path;
}

// ---------------------------------------------------------------------------------------------------------------
// The whole sub_82454B50: the list loop. Returns the number of draws it made.
// ---------------------------------------------------------------------------------------------------------------
template <class L>
uint32_t NativeLoop(PPCContext& ctx, uint8_t* base) {
  // --- Prologue: mflr r12; bl __savegprlr_26 (lr only); stwu r1,-144(r1) ---
  ctx.r12.u64 = ctx.lr;
  ctx.lr = kLapLoopPrologue;
  uint64_t r1 = ctx.r1.u64;
  {
    const uint32_t ea = uint32_t(r1) - kFrameLoop;
    E32(base, ea, uint32_t(r1));
    r1 = (r1 & 0xFFFFFFFF00000000ull) | ea;
  }
  const uint64_t r27 = ctx.r4.u64;  // mr r27,r4: la list
  const uint64_t r28 = ctx.r3.u64;  // mr r28,r3: la vista
  const uint64_t r11a = r27 + 8;    // addi r11,r27,8
  AnticipateBlock(base, uint32_t(r11a));  // the game's dcbt r0,r11: the entries
  const uint64_t r11b = L32(base, uint32_t(r11a) + 4);  // lwz r11,4(r11): the index of the first one
  const uint64_t r29 = kObjectsMinus4;                  // lis r10,-32101; addi r29,r10,-1440
  const uint64_t r11c = r11b * 52;                      // mulli r11,r11,52
  const uint64_t r10a = r11c + (r29 + 4);               // addi r10,r29,4; add r10,r11,r10
  AnticipateObject(base, uint32_t(r10a));                // the game's dcbt r0,r10: the first object
  const uint64_t r30a = L64(base, uint32_t(r27));       // ld r30,0(r27): the count
  E32(base, uint32_t(r1) + 84u, 0);                     // stw r26,84(r1) (r26 = 0)
  E32(base, uint32_t(r1) + 80u, 0);                     // stw r26,80(r1)
  ctx.r11.u64 = r11c;
  ctx.r10.u64 = r10a;
  uint32_t draws = 0;
  if (int32_t(uint32_t(r30a)) > 0) {  // cmpwi cr6,r30,0; ble loc_82454BF4
    uint64_t r30 = r30a;
    uint64_t r31 = r27 + 12;  // addi r31,r27,12
    do {  // loc_82454BA4
      const uint64_t r8 = r31 + 4;                         // addi r8,r31,4
      AnticipateBlock(base, uint32_t(r8) + 128u);          // the game's li r7,128; dcbt r7,r8: the list ahead
      const uint64_t r6 = L32(base, uint32_t(r31) + 8);    // lwz r6,8(r31): the index of the next one
      AnticipateObject(base, uint32_t(r6 * 52 + (r29 + 4)));  // the game's mulli r11,r6,52; add r5,r11,r10; dcbt r0,r5
      const uint64_t r4 = L32(base, uint32_t(r31));        // lwz r4,0(r31): the index of this one
      const uint64_t r11 = r4 * 52;                        // mulli r11,r4,52
      ctx.r3.u64 = r11 + (r29 + 4);                        // addi r10,r29,4; add r3,r11,r10
      ctx.r4.u64 = r28;                                    // mr r4,r28
      ctx.r5.u64 = r1 + 84;                                // addi r5,r1,84
      ctx.r6.u64 = r1 + 80;                                // addi r6,r1,80
      ctx.r7.u64 = 128;
      ctx.r8.u64 = r8;
      ctx.r10.u64 = r29 + 4;
      ctx.r11.u64 = r11;
      ctx.r1.u64 = r1;
      ctx.lr = kLapGlue;
      L::Glue(ctx, base);  // sub_82452730, through its hook
      r1 = ctx.r1.u64;
      ++draws;
      r30 -= 1;  // addi r30,r30,-1
      r31 += 8;  // addi r31,r31,8
    } while (uint32_t(r30) != 0);  // cmplwi cr6,r30,0; bne loc_82454BA4
  }
  // --- loc_82454BF4 ---
  ctx.r10.u64 = kLis32093;  // lis r10,-32093
  ctx.r11.u64 = 0;          // mr r11,r26
  E32(base, kIndicesCurrent, 0);  // stw r11,-11940(r10)
  E64(base, uint32_t(r27), 0);     // std r26,0(r27)
  r1 += kFrameLoop;               // addi r1,r1,144 (and b __restgprlr_26: returns without touching lr)
  ctx.r1.u64 = r1;
  return draws;
}

// ---------------------------------------------------------------------------------------------------------------
// Guard: call recorder, memory regions and the literal copies of the originals.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxCalls = 16;    // glue: at most 12 (state, view, 6 streams, indices, 2 for the draw)
constexpr uint32_t kPhotosLoop = 32;     // loop: full snapshot of the first ones
constexpr uint32_t kMaxLoop = 4096;     // and a summary of all of them so far (longer lists: not compared)
constexpr uint32_t kMaxZones = 6;
constexpr uint32_t kMaxBytesZones = 512;

static_assert(offsetof(PPCContext, r13) == offsetof(PPCContext, r3) + 13 * sizeof(PPCRegister),
              "r3, r0, r1, r2, r4..r13 consecutive");
static_assert(offsetof(PPCContext, f13) == offsetof(PPCContext, f0) + 13 * sizeof(PPCRegister), "f0..f13 consecutive");
static_assert(offsetof(PPCContext, v13) == offsetof(PPCContext, v0) + 13 * sizeof(PPCVRegister), "v0..v13 consecutive");

// What a call (or the exit) sees: all volatile registers, the FPCR, the last indirect target and the
// memory written.
struct Photo {
  uint32_t que;
  uint32_t target;
  uint64_t memory_block;    // sum of the regions at that moment: the order of stores relative to calls
  uint64_t r[14];      // r3, r0, r1, r2, r4 ... r13 (PPCContext order)
  uint64_t lr;
  uint64_t f[14];      // f0 ... f13
  uint8_t v[14 * 16];  // v0 ... v13
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

uint64_t Summary(const Photo& f) {  // of the full snapshot (no padding), 8 bytes at a time
  static_assert(sizeof(Photo) % 8 == 0, "Photo en words de 8 bytes");
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&f);
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < sizeof(Photo); i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h;
}

// As a called function would: the volatile registers change (to a value that depends on the call).
void Dirty(PPCContext& c, uint32_t k) {
  const uint64_t mark = 0xC0DE000000000000ull | (uint64_t(k) << 24);
  PPCRegister* const r = &c.r3;  // r3, r0, r1, r2, r4 ... r13
  for (uint32_t i = 0; i < 14; ++i) {
    if (i != 2 && i != 3 && i != 13) {  // nobody touches r1, r2 and r13
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
  bool Due(uint32_t d, uint32_t b) const {
    for (uint32_t i = 0; i < n; ++i) {
      if (uint64_t(d) < uint64_t(address[i]) + bytes[i] && uint64_t(address[i]) < uint64_t(d) + b) {
        return true;
      }
    }
    return false;
  }
  void Copy(uint8_t* base, uint8_t* target) const {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; ++i) {
      std::memcpy(target + o, REX_RAW_ADDR(address[i]), bytes[i]);
      o += bytes[i];
    }
  }
  void Restore(uint8_t* base, const uint8_t* source) const {  // in reverse: if two regions overlap, the earlier one wins
    uint32_t o = total;
    for (uint32_t i = n; i-- > 0;) {
      o -= bytes[i];
      std::memcpy(REX_RAW_ADDR(address[i]), source + o, bytes[i]);
    }
  }
  uint64_t Sum(uint8_t* base) const {  // 8 bytes at a time and the rest one byte at a time
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

constexpr uint32_t kMinimumPointer = 0x10000u;

// The regions the glue writes in a dry run (no calls): the stack [r1-240, r1), [r5], [r6] and the global
// index buffer. They depend only on registers. It also checks that whatever the dry run will read as a
// pointer looks like a pointer and that no region overlaps anything that decides an address: otherwise it
// does not compare (only the original runs).
bool ComputeZones(const PPCContext& ctx, uint8_t* base, Zones& z) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t obj = ctx.r3.u32;
  const uint32_t vista = ctx.r4.u32;
  const uint32_t p5 = ctx.r5.u32;
  const uint32_t p6 = ctx.r6.u32;
  if (stack < kMinimumPointer + kStackWatched || obj < kMinimumPointer || vista < kMinimumPointer ||
      p5 < kMinimumPointer || p6 < kMinimumPointer) {
    return false;
  }
  if (!z.Aggregate(stack - kStackWatched, kStackWatched) || !z.Aggregate(p6, 4) || !z.Aggregate(p5, 4) ||
      !z.Aggregate(kIndicesCurrent, 4)) {
    return false;
  }
  if (z.Due(obj, 32) || z.Due(vista + 5, 1)) {
    return false;
  }
  const uint32_t e = L32(base, obj + 12);
  const uint32_t a = L32(base, obj + 0);
  const uint32_t p = L32(base, obj + 4);
  if (e < kMinimumPointer || a < kMinimumPointer || p < kMinimumPointer || z.Due(e, 32) || z.Due(a, 256) ||
      z.Due(p, 4)) {
    return false;
  }
  const uint32_t m = L32(base, p);
  if (m < kMinimumPointer || z.Due(m + 24, 4)) {
    return false;
  }
  if ((L16(base, e + 6) & 0x20u) != 0) {
    const uint32_t table = L32(base, e);
    if (table < kMinimumPointer || z.Due(table + 20, 4)) {
      return false;
    }
  }
  return true;
}

// The loop: its frame [r1-144, r1-56) (the stwu and the two zeros at r1+80 and r1+84), [list+0] (8) and
// the global.
bool ComputeZonesLoop(const PPCContext& ctx, uint8_t* base, Zones& z, uint32_t& count) {
  const uint32_t stack = ctx.r1.u32;
  const uint32_t list = ctx.r4.u32;
  if (stack < kMinimumPointer + kFrameLoop || list < kMinimumPointer) {
    return false;
  }
  const uint32_t n = uint32_t(L64(base, list));
  count = int32_t(n) > 0 ? n : 0;
  if (count > kMaxLoop) {
    return false;
  }
  if (!z.Aggregate(stack - kFrameLoop, 88) || !z.Aggregate(list, 8) || !z.Aggregate(kIndicesCurrent, 4)) {
    return false;
  }
  return !z.Due(list + 8, 8u * (count + 1u));  // the entries must not lie in a region
}

struct Recording {
  uint32_t n;  // calls made (beyond kMaxCalls only the first are kept and nothing is compared)
  Photo calls[kMaxCalls];
  Photo output;
};
struct RecordingLoop {
  uint32_t n;
  Photo calls[kPhotosLoop];
  uint64_t summary[kMaxLoop];
  Photo output;
};
// Guard state. Global rather than per thread (on the Switch a large TLS is paid by every thread of the
// process): one thread uses it at a time (g_checking); if another thread arrived meanwhile, that call
// only runs the original.
std::atomic<bool> g_checking{false};
std::atomic<bool> g_checking_loop{false};
Recording* t_recording = nullptr;
RecordingLoop* t_recording_loop = nullptr;
const Zones* t_zones = nullptr;
const Zones* t_zones_loop = nullptr;
uint8_t* t_base = nullptr;
Recording g_copy;
Recording g_native;
RecordingLoop g_copy_loop;
RecordingLoop g_native_loop;
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

[[gnu::noinline]] void RecordLoop(PPCContext& ctx) {
  RecordingLoop& g = *t_recording_loop;
  const uint32_t k = g.n++;
  if (k < kMaxLoop) {
    Photo f;
    Photograph(ctx, f);
    f.que = kGlue;
    f.target = 0;
    f.memory_block = t_zones_loop->Sum(t_base);
    g.summary[k] = Summary(f);
    if (k < kPhotosLoop) {
      g.calls[k] = f;
    }
  }
  Dirty(ctx, k);
}

// Dry run: nothing is called; what the call would see is recorded and the volatile registers are clobbered.
struct Recorder {
  static void Call(PPCContext& ctx, uint8_t* base, uint32_t que) { Record(ctx, que, 0); }
  static void Indirect(PPCContext& ctx, uint8_t* base, uint32_t target) { Record(ctx, kIndirect, target); }
  static void Glue(PPCContext& ctx, uint8_t* base) { RecordLoop(ctx); }
};

// ==== Literal copy of sub_82452730 and of the two functions it calls that the native version contains
// ==== (generated by literal_copy.py): the generated code without comments; the calls go through the policy.
// ==== Do not edit by hand: the patch checks that it equals the current generated code.
#define NFSMW_GLUE_CALL(c, b, que) Calls::Call(c, b, que)
#define NFSMW_GLUE_INDIRECT(c, b, target) Calls::Indirect(c, b, target)
#define NFSMW_GLUE_FLOWS(c, b) CopyFlows<Calls>(c, b)
#define NFSMW_GLUE_DRAW(c, b) CopyDraw<Calls>(c, b)
#include "copies_literal/CopyFlows.inc"

#include "copies_literal/CopyDraw.inc"

#include "copies_literal/CopyGlue.inc"
#undef NFSMW_GLUE_CALL
#undef NFSMW_GLUE_INDIRECT
#undef NFSMW_GLUE_FLOWS
#undef NFSMW_GLUE_DRAW
// ==== End of the literal copy of the glue

// ==== Literal copy of sub_82454B50, generated by tools/literal_copy.py. Do not edit by hand.
#define NFSMW_LOOP_CALL(c, b) Calls::Glue(c, b)
#include "copies_literal/CopyLoop.inc"
#undef NFSMW_LOOP_CALL
// ==== End of the literal copy of the loop

// ---------------------------------------------------------------------------------------------------------------
// Counters, report, guards and calls.
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kChecks = 20000;   // glue: first calls checked
constexpr uint64_t kMinimumOdd = 2000;        // and the first ones of each rare path
constexpr uint64_t kWindowOdd = 1000000;   // (while within these calls: afterwards, 1 in every kPeriod)
constexpr uint64_t kPeriod = 4096;           // afterwards, 1 in every kPeriod (power of 2)
constexpr uint64_t kMeasurementOriginal = 64;      // 1 in 64 runs the original, timed (power of 2)
// Loop: each check walks the whole list twice in a dry run (~0.5 us per draw: ~1 ms for the large list),
// so only a few at the start and then very rarely (1 in 4096: one every ~5 s, ~0.2 ms/s).
constexpr uint64_t kChecksLoop = 500;
constexpr uint32_t kBitsPeriodLoop = 12;        // afterwards, 1 in 4096 at random (Due)
constexpr uint32_t kBitsMeasurementOriginalLoop = 5;  // 1 in 32 at random through the original, timed

enum Reason : uint32_t { kByOff = 0, kByCheck = 1, kByMeasurement = 2, kReasons = 3 };

// Counters without atomic read-modify-write (the A57 has no LSE): the Main XThread writes them; if another
// thread counted at the same time some counts would be lost, which does not matter for the report.
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_by_combination[kCombinations];  // native calls in the period, per path combination
std::atomic<uint64_t> g_original[kReasons];             // in the period
std::atomic<uint64_t> g_ns_native{0}, g_samples_native{0};
std::atomic<uint64_t> g_ns_original{0}, g_samples_original{0};
std::atomic<uint64_t> g_checked{0};                   // since startup, all without mismatches
std::atomic<uint64_t> g_checked_path[kPaths];
std::atomic<uint64_t> g_skipped{0};                      // guard runs not compared (unbounded regions, too many calls)
std::atomic<uint32_t> g_odd_ready{0};                  // paths odd con kMinimumOdd checked
std::atomic<bool> g_odd_open{true};
std::atomic<bool> g_off{false};
// The loop
std::atomic<uint64_t> g_calls_loop{0};
std::atomic<uint64_t> g_native_loop{0}, g_draws_loop{0};
std::atomic<uint64_t> g_original_loop[kReasons];
std::atomic<uint64_t> g_ns_native_loop{0}, g_draws_native_loop{0};
std::atomic<uint64_t> g_ns_original_loop{0}, g_draws_original_loop{0};
std::atomic<uint64_t> g_checked_loop{0};
std::atomic<uint64_t> g_skipped_loop{0};
std::atomic<bool> g_off_loop{false};
std::atomic<int64_t> g_next_ms{0};
std::atomic<int8_t> g_active{-1};

template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// 1 in 2^bits loop calls, spread at random: the loop is called almost the same number of times every
// frame, and with a mask (n & 255) the same list would always be checked or timed.
inline bool Due(uint64_t n, uint32_t bits, uint64_t which) {
  return ((n * 0x9E3779B97F4A7C15ull) >> (64 - bits)) == which;
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline bool Active() {
  int8_t a = g_active.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_glue_native) ? 1 : 0;
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
    REXLOG_INFO("[glue] glue de draw (sub_82452730) y su loop (sub_82454B50) {}",
                Active() ? "en native (build 186): empiezan checking contra la copy literal de la original"
                         : "por la original (nfsmw_glue_native = false)");
    return;
  }
  // The glue
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
  const double saving = (k_n && k_o) ? (us_o - us_n) * double(native) / 10.0 / 1000.0 : 0.0;
  NFSMW_REPORT_DEFERRED(
      "[glue] last 10 s: {:.0f} calls/s; {} en native ({} {}, {} {}, {} {}, {} {}, {} {}, {} {}, {} {}), {} "
      "por la original (off {}, check {}, measurement {}); native {:.3f} us/call ({} cronometradas), original "
      "{:.3f} us/call ({}) con todo lo que llaman: saving {:.2f} ms/s; checked contra la copy literal since el "
      "arranque: {} (sin compare {}){}",
      double(native + total_original) / 10.0, native, kNamesPath[0], by_path[0], kNamesPath[1],
      by_path[1], kNamesPath[2], by_path[2], kNamesPath[3], by_path[3], kNamesPath[4],
      by_path[4], kNamesPath[5], by_path[5], kNamesPath[6], by_path[6], total_original,
      original[kByOff], original[kByCheck], original[kByMeasurement], us_n, k_n, us_o, k_o, saving,
      g_checked.load(std::memory_order_relaxed), g_skipped.load(std::memory_order_relaxed),
      g_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "");
  // The loop
  const uint64_t nb = g_native_loop.exchange(0, std::memory_order_relaxed);
  const uint64_t db = g_draws_loop.exchange(0, std::memory_order_relaxed);
  uint64_t ob[kReasons];
  uint64_t total_ob = 0;
  for (uint32_t m = 0; m < kReasons; ++m) {
    ob[m] = g_original_loop[m].exchange(0, std::memory_order_relaxed);
    total_ob += ob[m];
  }
  const uint64_t ns_nb = g_ns_native_loop.exchange(0, std::memory_order_relaxed);
  const uint64_t d_nb = g_draws_native_loop.exchange(0, std::memory_order_relaxed);
  const uint64_t ns_ob = g_ns_original_loop.exchange(0, std::memory_order_relaxed);
  const uint64_t d_ob = g_draws_original_loop.exchange(0, std::memory_order_relaxed);
  const double us_nb = d_nb ? double(ns_nb) / double(d_nb) / 1000.0 : 0.0;
  const double us_ob = d_ob ? double(ns_ob) / double(d_ob) / 1000.0 : 0.0;
  const double saving_b = (d_nb && d_ob) ? (us_ob - us_nb) * double(db) / 10.0 / 1000.0 : 0.0;
  NFSMW_REPORT_DEFERRED(
      "[glue] loop de la list, last 10 s: {:.0f} calls/s y {:.0f} draws/s en native; {} por la original "
      "(off {}, check {}, measurement {}); native {:.3f} us/draw ({} draws timed), original {:.3f} "
      "us/draw ({}): saving {:.2f} ms/s; checked since el arranque: {} (sin compare {}){}",
      double(nb) / 10.0, double(db) / 10.0, total_ob, ob[kByOff], ob[kByCheck], ob[kByMeasurement], us_nb,
      d_nb, us_ob, d_ob, saving_b, g_checked_loop.load(std::memory_order_relaxed),
      g_skipped_loop.load(std::memory_order_relaxed),
      g_off_loop.load(std::memory_order_relaxed) ? " | OFF por difference" : "");
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

// The first difference in the memory they leave behind ("" if equal).
std::string DifferenceMemory(const Zones& z, const uint8_t* copy, const uint8_t* native) {
  uint32_t o = 0;
  for (uint32_t i = 0; i < z.n; ++i) {
    for (uint32_t b = 0; b < z.bytes[i]; ++b) {
      if (copy[o + b] != native[o + b]) {
        const uint32_t m = z.bytes[i] - b < 8u ? z.bytes[i] - b : 8u;
        return fmt::format("memory_block en 0x{:08X} (zone {} +{}): original {} native {}", z.address[i] + b, i, b,
                           Hex(copy + o + b, m), Hex(native + o + b, m));
      }
    }
    o += z.bytes[i];
  }
  return std::string();
}

// Glue guard: dry runs of the literal copy and of the native version, each one undone, then compared, and
// then the original for real.
[[gnu::noinline]] void Check(PPCContext& ctx, uint8_t* base) {
  Zones z;
  if (g_checking.exchange(true, std::memory_order_acquire)) {  // another thread is in the guard
    Add(g_skipped, uint64_t(1));
    Add(g_original[kByCheck], uint64_t(1));
    __imp__sub_82452730(ctx, base);
    return;
  }
  if (!ComputeZones(ctx, base, z)) {
    g_checking.store(false, std::memory_order_release);
    Add(g_skipped, uint64_t(1));
    Add(g_original[kByCheck], uint64_t(1));
    __imp__sub_82452730(ctx, base);
    return;
  }
  Recording& copy = g_copy;
  Recording& native = g_native;
  z.Copy(base, g_before);
  const PPCContext entry = ctx;
  const auto volver = [&]() {
    z.Restore(base, g_before);
    ctx = entry;
    ctx.fpscr.setcsr(ctx.fpscr.csr);  // the real FPCR, same as on entry
  };
  t_zones = &z;
  t_base = base;
  copy.n = 0;
  t_recording = &copy;
  CopyGlue<Recorder>(ctx, base);
  Photograph(ctx, copy.output);
  z.Copy(base, g_mem_copy);
  volver();
  native.n = 0;
  t_recording = &native;
  const uint32_t path = Native<Recorder>(ctx, base);
  Photograph(ctx, native.output);
  z.Copy(base, g_mem_native);
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
      que = DifferenceMemory(z, g_mem_copy, g_mem_native);
    }
  }
  const bool too_many = copy.n > kMaxCalls;
  g_checking.store(false, std::memory_order_release);
  __imp__sub_82452730(ctx, base);  // the original for real, from the entry state: its state is kept
  Add(g_original[kByCheck], uint64_t(1));
  if (que.empty() && too_many) {
    Add(g_skipped, uint64_t(1));
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
          REXLOG_INFO("[glue] path {}: {} calls checked contra la copy literal de la original, 0 "
                      "differences",
                      kNamesPath[b], kMinimumOdd);
        }
      }
    }
    if (total == kChecks) {
      REXLOG_INFO("[glue] {} calls checked contra la copy literal de la original (en every call que "
                  "does: r0-r13, lr, f0-f13, v0-v13, FPCR, last indirect y la memory_block written; y la output, la stack, "
                  "[r5], [r6] y la global de indices), 0 differences: en native, y sigue checking 1 de every {}",
                  total, kPeriod);
    }
    return;
  }
  g_off.store(true, std::memory_order_relaxed);  // the state is already the original's
  REXLOG_ERROR("[glue] DIFFERENCE con la original ({}{}; path 0x{:X}): entry object 0x{:08X}, vista 0x{:08X}, "
               "r5 0x{:08X}, r6 0x{:08X}, stack 0x{:08X}. Glue native OFF para el rest de la session: se "
               "queda la original",
               que, where ? fmt::format(", en la call {}", where) : std::string(", a la output"), path,
               entry.r3.u32, entry.r4.u32, entry.r5.u32, entry.r6.u32, entry.r1.u32);
}

// Loop guard: the same, with a summary of each call (the first kPhotosLoop with a full snapshot).
[[gnu::noinline]] void CheckLoop(PPCContext& ctx, uint8_t* base) {
  Zones z;
  uint32_t count = 0;
  if (g_checking_loop.exchange(true, std::memory_order_acquire)) {
    Add(g_skipped_loop, uint64_t(1));
    Add(g_original_loop[kByCheck], uint64_t(1));
    __imp__sub_82454B50(ctx, base);
    return;
  }
  if (!ComputeZonesLoop(ctx, base, z, count)) {
    g_checking_loop.store(false, std::memory_order_release);
    Add(g_skipped_loop, uint64_t(1));
    Add(g_original_loop[kByCheck], uint64_t(1));
    __imp__sub_82454B50(ctx, base);
    return;
  }
  RecordingLoop& copy = g_copy_loop;
  RecordingLoop& native = g_native_loop;
  z.Copy(base, g_before);
  const PPCContext entry = ctx;
  const auto volver = [&]() {
    z.Restore(base, g_before);
    ctx = entry;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
  };
  t_zones_loop = &z;
  t_base = base;
  copy.n = 0;
  t_recording_loop = &copy;
  CopyLoop<Recorder>(ctx, base);
  Photograph(ctx, copy.output);
  z.Copy(base, g_mem_copy);
  volver();
  native.n = 0;
  t_recording_loop = &native;
  NativeLoop<Recorder>(ctx, base);
  Photograph(ctx, native.output);
  z.Copy(base, g_mem_native);
  volver();
  t_recording_loop = nullptr;

  std::string que;
  uint32_t where = 0;
  if (copy.n != native.n) {
    que = fmt::format("{} calls front a {}", copy.n, native.n);
  } else if (copy.n <= kMaxLoop) {
    for (uint32_t k = 0; k < copy.n && que.empty(); ++k) {
      if (k < kPhotosLoop) {
        que = Difference(copy.calls[k], native.calls[k]);
      } else if (copy.summary[k] != native.summary[k]) {
        que = "lo que ve la call (register_values o memory_block written; summary different)";
      }
      where = k + 1;
    }
    if (que.empty()) {
      que = Difference(copy.output, native.output);
      where = 0;
    }
    if (que.empty()) {
      que = DifferenceMemory(z, g_mem_copy, g_mem_native);
    }
  }
  const bool too_many = copy.n > kMaxLoop;
  g_checking_loop.store(false, std::memory_order_release);
  __imp__sub_82454B50(ctx, base);  // the original for real
  Add(g_original_loop[kByCheck], uint64_t(1));
  if (que.empty() && too_many) {
    Add(g_skipped_loop, uint64_t(1));
    return;
  }
  if (que.empty()) {
    const uint64_t total = g_checked_loop.load(std::memory_order_relaxed) + 1;
    g_checked_loop.store(total, std::memory_order_relaxed);
    if (total == kChecksLoop) {
      REXLOG_INFO("[glue] loop de la list: {} calls checked contra la copy literal de la original (every "
                  "draw que manda y la output, su frame, [list+0] y la global de indices), 0 differences: en native, "
                  "y sigue checking 1 de every {} al azar",
                  total, 1u << kBitsPeriodLoop);
    }
    return;
  }
  g_off_loop.store(true, std::memory_order_relaxed);
  REXLOG_ERROR("[glue] DIFFERENCE en el loop de la list con la original ({}{}): entry vista 0x{:08X}, list "
               "0x{:08X} ({} draws), stack 0x{:08X}. Loop native OFF para el rest de la session: se queda la "
               "original",
               que, where ? fmt::format(", en la call {}", where) : std::string(", a la output"), entry.r3.u32,
               entry.r4.u32, count, entry.r1.u32);
}

// The rare paths this call will take, read from memory beforehand (it only decides whether the guard runs
// it). They are loads the original always makes ([vista+5], E, [E+24], [E+6], A and [A+94]), and only those
// of the paths still missing: reading A this early defeats the A hint while the window lasts.
uint32_t Predict(const PPCContext& ctx, uint8_t* base, uint32_t pending_2) {
  const uint32_t obj = ctx.r3.u32;
  uint32_t c = 0;
  if ((pending_2 & kPathViewE20) != 0 && L8(base, ctx.r4.u32 + 5) != 0) {
    c |= kPathViewE20;
  }
  if ((pending_2 & (kPathFlags | kPathVirtual | kPathSecond)) != 0) {
    const uint32_t e = L32(base, obj + 12);
    if (L32(base, e + 24) != 0) {
      c |= kPathFlags;
    }
    const uint32_t b = L16(base, e + 6);
    if ((b & 0x20u) != 0) {
      c |= kPathVirtual;
    } else if ((b & 0x10u) != 0) {
      c |= kPathSecond;
    }
  }
  if ((pending_2 & kPathFlows) != 0 && (L16(base, L32(base, obj + 0) + 94) & 0x8000u) == 0) {
    c |= kPathFlows;
  }
  return c & pending_2;
}

}  // namespace

void Glue(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_calls.load(std::memory_order_relaxed) + 1;
  g_calls.store(n, std::memory_order_relaxed);
  if ((n & (kPeriod - 1)) == 0) [[unlikely]] {
    Report();
  }
  if (!Active() || g_off.load(std::memory_order_relaxed)) [[unlikely]] {
    Add(g_original[kByOff], uint64_t(1));
    __imp__sub_82452730(ctx, base);
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
    } else if (Predict(ctx, base, pending_2) != 0) {
      Check(ctx, base);
      return;
    }
  }
  if ((n & (kMeasurementOriginal - 1)) == kMeasurementOriginal / 2) [[unlikely]] {
    const int64_t t0 = NowNs();
    __imp__sub_82452730(ctx, base);
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

void Loop(PPCContext& ctx, uint8_t* base) {
  const uint64_t n = g_calls_loop.load(std::memory_order_relaxed) + 1;
  g_calls_loop.store(n, std::memory_order_relaxed);
  if (!Active() || g_off_loop.load(std::memory_order_relaxed)) [[unlikely]] {
    Add(g_original_loop[kByOff], uint64_t(1));
    __imp__sub_82454B50(ctx, base);
    return;
  }
  if (n <= kChecksLoop || Due(n, kBitsPeriodLoop, 0)) [[unlikely]] {
    CheckLoop(ctx, base);
    return;
  }
  if (Due(n, kBitsMeasurementOriginalLoop, 16)) [[unlikely]] {
    const uint32_t count = uint32_t(L64(base, ctx.r4.u32));  // the list count (the original reads it the same way)
    const int64_t t0 = NowNs();
    __imp__sub_82454B50(ctx, base);
    Add(g_ns_original_loop, uint64_t(NowNs() - t0));
    Add(g_draws_original_loop, uint64_t(int32_t(count) > 0 ? count : 0));
    Add(g_original_loop[kByMeasurement], uint64_t(1));
    return;
  }
  if (Due(n, 3, 5)) [[unlikely]] {
    const int64_t t0 = NowNs();
    const uint32_t d = NativeLoop<Real>(ctx, base);
    Add(g_ns_native_loop, uint64_t(NowNs() - t0));
    Add(g_draws_native_loop, uint64_t(d));
    Add(g_native_loop, uint64_t(1));
    Add(g_draws_loop, uint64_t(d));
    return;
  }
  const uint32_t d = NativeLoop<Real>(ctx, base);
  Add(g_native_loop, uint64_t(1));
  Add(g_draws_loop, uint64_t(d));
}

}  // namespace nfsmw::glue_native

// The generated code's calls go to sub_82452730 (2) and to sub_82454B50 (6): the patch turns them back from
// __imp__ to sub_. The indirect-call dispatch table already points to both hooks.
REX_HOOK_RAW(sub_82452730) {  // the draw glue for one list object
  nfsmw::glue_native::Glue(ctx, base);
}
REX_HOOK_RAW(sub_82454B50) {  // the draw-list loop
  nfsmw::glue_native::Loop(ctx, base);
}
