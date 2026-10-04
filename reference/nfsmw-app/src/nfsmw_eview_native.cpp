// nfsmw - eViewPlatInterface::Render (sub_8243E358) in native code.
//
// WHAT IT IS
//   Render(this = view, r4 = eModel, r5 = matrix, r6 = light, r7 = flags, r8 = bone palette) walks the
//   model's submeshes and, for each visible one, prepares the draw packet. It is the bottleneck all geometry
//   goes through (scene, shadows, reflections). Read instruction by instruction in nfsmw_recomp.105.cpp:16383:
//     r29 = [model+12] (eSolid); if it is 0, out. If [r29+14] & 0x800 and the byte 0x82A2CFB7 != 0, out.
//     r28 = [r29] (the mesh); calls 8250_0010(model, r29, matrix) and, if [model+16] != 0, 8221_8B18(model,
//     r1+128) (puts the replacement textures into the solid's table).
//     For each submesh i < [r28+16] (256 bytes each from [r28+20]):
//       r30 = [e+28] (material); if [r28+64] != 0, it is remapped through the table 0x8293CC68 and
//       [r28+64] = 0.
//       If !(r7 & 4) and !([e+36] & 1): GetVisibleState(view, e, e+12, matrix); if it returns 0, next.
//       The 5 textures: [[r29+44] + 8 * byte[e+43..47] + 4] -> r1+96..112 (whenever it is visible).
//       If [texture0+36] != [0x82A45274]: arg9 = byte[e+42] == 255 ? 0 : [[r29+60] + 8 * byte + 4],
//       r1+92 = r8, r1+84 = arg9, and the packet 8245_2868(view, e, r29, r7, r30, r1+96, matrix, r6).
//     At the end, 8221_8AB8(model, r1+128) (returns the replacement textures).
//
// WHY NATIVE
//   Stack sampling on the console: 2.8-4.4 % self time of a core of the draw thread, 6.5 % with its leaves.
//   Per instruction: two thirds of the samples land on 8 loads that miss in cache: the first read of each
//   submesh (+0x1D8, the hottest: the PowerPC requested it with dcbt, which the recompiled code drops),
//   [texture0+36] (+0x2F4), the texture table and the model entry. The remaining third is the recompiled
//   code itself: every PowerPC register ends up written to the context and, since writes to guest memory
//   may overlap the context, r1 is reread after each one (5 times per submesh just for r1+96..112).
//   Here: the registers live in variables, the context is only written to make calls, and the submesh i+2
//   and the first texture and material of i+1 are requested in advance (no side effects: PRFM).
//   Measured on PC (x86, indicative only), with the original packet and the replacements inside:
//     - with a cold cache (4096 models in 128 MB, as on the console): 270-430 ns less per call (20-32 ns
//       per submesh, 10-13 % of the whole call). Without the prefetches, almost nothing: the saving comes
//       from hiding misses.
//     - with a warm cache: 25-40 ns less per call. Prefetching all five textures cost 40-80 ns when warm
//       without gaining when cold: only the first one is prefetched.
//   On aarch64 (devkitA64 with the Switch options), the path of a visible, drawn submesh: about 120
//   instructions plus about 26 for the prefetch, against about 170 in the original (counted by hand).
//
// THE PACKETS (8245_2868 and 8245_4428): NOT MOVED TO NATIVE CODE
//   Their matrix copy loop (the "almost all copies" part of the code) has not a single sample in the
//   profile: it only runs with bones. Their cost is recording the draw (52-byte record, sort key and bucket)
//   and the misses on [texture0+81/84], [material+4/6] and the texture keys, which the prefetch here already
//   requests earlier. What would be left to save is ~50 instructions per draw (~0.2-0.4 % of a core) in
//   exchange for the most delicate part: the copy passes 3 of every 4 words through double and the fourth
//   as is (seen in the disassembly), which depends on how GCC compiles each build, and with the heap
//   exhausted it writes 1 KB to address 0.
//
// WHY IT IS BIT-IDENTICAL
//   - Only integers are involved. The same reads and writes of guest memory, in the same relative order:
//     the frame back link (stwu), [r28+64] = 0 and r1+84/92/96..112 (stack nobody reads any more, but
//     memory ends up identical). The addresses, with the same arithmetic (64 bits where the original adds
//     in 64 and the address is the low part).
//   - The same calls, to the same names (sub_X: they go through their hooks, as in the original, whose
//     calls tools/direct_calls.py leaves as sub_X), in the same order, with the same 64-bit arguments,
//     the same lr and r1 = the 784-byte frame.
//   - What is prefetched is read-only and PRFM (which never faults): +28 and +43 of submesh i+1, which
//     exists and whose +28 the original always reads, and the solid's texture table only when the original
//     has already read it in that call (a solid that is not drawn could have it at 0).
//   - Registers: interprocedural liveness analysis of the 56,338 functions: after its 30 direct call sites
//     only r3 and f1 are live. r3: the input one if it exits early (paths 1 and 2) and the model (the r3 of
//     8221_8AB8, which does not touch it) if it walks the mesh. f1 and the denormal mode are only touched
//     by the functions it calls, which are the same and in the same order. It also leaves r1, r12 and lr
//     as the original does on all three paths, and r9-r11 on path 2; nobody reads the other volatiles.
//     Render never appears as a pointer in the code (no lis/addi forms its address): only its 30 bl call it.
//
// SELF-CHECKING GUARD (cvar nfsmw_eview_native; project rule)
//   Render calls functions with side effects (8221_8B18 changes the solid's texture table, the packet
//   records draws in global lists), so it cannot be run twice. The first kChecksRender calls and
//   then 1 of every 4096:
//     1. The 9 words Render can write are snapshotted (back link, [r28+64] and r1+84..112).
//     2. The original runs for real (it is in charge). Its calls to 8250_0010, 8221_8B18, the packet and
//        8221_8AB8 go through the hooks in this file, which in "trace mode" record the arguments and, for
//        the packet, the 7 stack words it reads.
//     3. On entry to 8221_8AB8 (the last call, with memory as the loop saw it: replacement textures in
//        place, [r28+64] already 0), the native version is replayed in full as a dry run: it reads through a
//        shadow (its own writes and the snapshot of [r28+64]), writes nothing, calls nobody except
//        GetVisibleState (pure: it only writes its frame below r1, which is saved and restored afterwards),
//        and every call it would make is compared with the recorded one.
//     4. The call list, the arguments, the 9 words and, on return, r3/r1/lr (and r12, r9-r11 on the short
//        paths) must match. A difference turns the native version off for the session and writes
//        "[eview] DIFFERENCE".
//   The final state is always the original's. One thread at a time (if another one is checking, the
//   original runs at first and the native version afterwards). If the trace fills up (more than 1021 drawn
//   submeshes), that call does not count. "[eview]" line every 10 s: native calls, original calls, checked
//   calls and the average time of 1 in 16 native calls (with GetVisibleState and the packet included).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_eview_native, true, "NFSMW",
                    "eViewPlatInterface::Render (sub_8243E358, el loop de submallas de every model) en native, "
                    "identico bit a bit (build 176). Se comprueba contra la original al begin y 1 de every 4096 "
                    "calls after, y se apaga sola si difiere; false = la original")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// This file only names by their 8 digits the functions it hooks or that were already hooked
// (tools/direct_calls.py treats any 82xxxxxx address in app/src as hooked); the rest are split (8245_4428).
REX_EXTERN(__imp__sub_8243E358);  // original Render (the hook is in nfsmw_shadows_lod.cpp and calls Render() from here)
REX_EXTERN(__imp__sub_82452868);  // draw packet
REX_EXTERN(__imp__sub_82500010);
REX_EXTERN(__imp__sub_82218B18);
REX_EXTERN(__imp__sub_82218AB8);
REX_EXTERN(sub_82452868);  // hooks in this file (called by name, like the original)
REX_EXTERN(sub_82500010);
REX_EXTERN(sub_82218B18);
REX_EXTERN(sub_82218AB8);
REX_EXTERN(sub_8243E7D8);  // GetVisibleState: its hook is in nfsmw_d3d_registers_native.cpp

namespace nfsmw::eview {
void Render(PPCContext& ctx, uint8_t* base);

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Guest memory: the same translation as REX_LOAD / REX_STORE in nfsmw_pch.h.
// ---------------------------------------------------------------------------------------------------------------
inline uint32_t Offset(uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}
inline uint8_t* Pointer(uint8_t* base, uint32_t address) {
  return base + address + Offset(address);
}
inline uint32_t Read8(uint8_t* base, uint32_t address) {
  return *Pointer(base, address);
}
inline uint32_t Read16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, Pointer(base, address), 2);
  return __builtin_bswap16(v);
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
// PRFM: no side effects and it never faults, even if the address is invalid.
inline void Anticipate(uint8_t* base, uint32_t address) {
  __builtin_prefetch(Pointer(base, address), 0, 3);
}
// Only for the PC test (measuring with and without prefetches): always 1 in the game.
#ifndef NFSMW_EVIEW_ANTICIPATE
#define NFSMW_EVIEW_ANTICIPATE 1
#endif

// ---------------------------------------------------------------------------------------------------------------
// Fixed addresses (PowerPC lis + offset; checked in the PC test) and return addresses of Render's calls (the lr
// it sets before each bl; the trace hooks use it to recognize their own call).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint32_t kFlagOutput = 0x82A2CFB7;  // lis r11,-32093; lbz r9,-12361(r11)
constexpr uint32_t kTableRemap = 0x8293CC68;   // lis r10,-32108; addi r19,r10,-13208
constexpr uint32_t kKeySkip = 0x82A45274;    // lis r10,-32092; lwz r25,21108(r10)
constexpr uint32_t kFrame = 784;                 // stwu r1,-784(r1)
constexpr uint64_t kLrPrologue = 0x8243E360;      // bl 826b_dce8 (saves r16-r31: in the recompiled code it writes nothing)
constexpr uint64_t kLr500010 = 0x8243E3C4;
constexpr uint64_t kLrB18 = 0x8243E3DC;
constexpr uint64_t kLrVisible = 0x8243E48C;
constexpr uint64_t kLrPacket = 0x8243E554;
constexpr uint64_t kLrAB8 = 0x8243E574;
constexpr uint64_t kR11Flag = 0xFFFFFFFF82A30000ull;  // lis r11,-32093 (signed, in 64 bits)

struct Entry {
  uint64_t r1, lr, r3, r4, r5, r6, r7, r8;
};
inline Entry ReadEntry(const PPCContext& ctx) {
  return {ctx.r1.u64, ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64};
}
// The frame: stwu only changes the low part of r1 (ctx.r1.u32 = ea).
inline uint64_t Frame64(uint64_t r1) {
  return (r1 & 0xFFFFFFFF00000000ull) | uint32_t(uint32_t(r1) - kFrame);
}

// ---------------------------------------------------------------------------------------------------------------
// The body, a single one for the normal path and for the guard's replay. P is the policy: where it reads
// from, where it writes to, and what "calling" means. Line numbers of nfsmw_recomp.105.cpp in parentheses.
// ---------------------------------------------------------------------------------------------------------------
template <class P>
[[gnu::always_inline]] inline void Body(P& p, const Entry& e) {
  const uint64_t frame64 = Frame64(e.r1);
  const uint32_t R1 = uint32_t(frame64);
  p.E32(R1, uint32_t(e.r1));  // stwu r1,-784(r1) (16409)
  p.Prologue(e, frame64);      // mflr r12; bl (lr = 0x8243E360); r1 = frame
  const uint32_t model = uint32_t(e.r4);
  const uint32_t r29 = p.L32(model + 12);  // lwz r29,12(r17)
  if (r29 == 0) {
    p.Epilogue(frame64);
    return;
  }
  const uint32_t flags = p.L16(r29 + 14);  // lhz r11,14(r29)
  if ((flags & 0x800u) != 0) {
    const uint32_t exit = p.L8(kFlagOutput);
    if (exit != 0) {
      p.OutputFlag(flags, exit);
      p.Epilogue(frame64);
      return;
    }
  }
  const uint32_t r28 = p.L32(r29);            // lwz r28,0(r29)
  const uint32_t list = p.L32(r28 + 20);     // lwz r8,20(r28); dcbt r0,r8
  p.AdvanceInitial(list);
  p.Call500010(e.r4, r29, e.r5);            // (16460)
  if (p.L32(model + 16) != 0) {              // lwz r7,16(r17)
    p.CallB18(e.r4, frame64 + 128);         // (16473)
  }
  if (int32_t(p.L32(r28 + 16)) > 0) {         // lwz r6,16(r28); ble
    const uint32_t r16 = uint32_t(e.r7) & 4u;             // rlwinm r16,r22,0,29,29
    const uint32_t r25 = p.L32(kKeySkip);
    // Only for prefetching: the solid's texture table, once the original has already read it in this call (a
    // drawn submesh). Not touched before that: a solid that is not drawn could have it at 0.
    uint32_t table_textures = 0;
    uint64_t r27 = 0;     // offset of the submesh (addi r27,r27,256)
    uint64_t index = 0;  // ctx.r11 at the loop header
    for (;;) {
      const uint64_t r23 = index + 1;                        // addi r23,r11,1 (16501)
      const int32_t how_many = int32_t(p.L32(r28 + 16));        // lwz r5,16(r28)
      if (int32_t(uint32_t(r23)) < how_many) {                 // dcbt r0,r4 of the next one (16512)
        p.AdvanceLoop(uint32_t(r27 + p.L32(r28 + 20)) + 256u, int64_t(int32_t(uint32_t(r23))) + 1 < how_many,
                        table_textures);
      }
      const uint32_t base20 = p.L32(r28 + 20);                // lwz r10,20(r28)
      const uint32_t remap = p.L32(r28 + 64);               // lwz r11,64(r28)
      const uint64_t r31 = r27 + base20;                      // add r31,r27,r10
      const uint32_t sub = uint32_t(r31);
      uint32_t r30 = p.L32(sub + 28);                         // lwz r30,28(r31)
      if (remap != 0) {
        const uint32_t v = p.L32((p.L16(r30 + 4) << 2) + remap);  // lhz r3,4(r30); rotlwi; lwzx r11,r10,r11
        if (int32_t(v) != -1) {
          r30 = p.L32((v << 2) + kTableRemap);             // rlwinm r9,r11,2,0,29; lwzx r30,r9,r19
        }
        p.E32(r28 + 64, 0);                                  // stw r18,64(r28)
      }
      bool draw = true;
      if (r16 == 0 && (p.L32(sub + 36) & 1u) == 0) {          // lwz r8,36(r31); clrlwi r7,r8,31
        draw = int32_t(p.Visible(e.r3, r31, r31 + 12, e.r5)) != 0;  // (16566); cmpwi r3,0
      }
      if (draw) {
        const uint32_t b43 = p.L8(sub + 43);
        const uint32_t b44 = p.L8(sub + 44);
        const uint32_t table = p.L32(r29 + 44);
        const uint32_t b45 = p.L8(sub + 45);
        const uint32_t b46 = p.L8(sub + 46);
        const uint32_t b47 = p.L8(sub + 47);
        const uint32_t t0 = p.L32((b43 << 3) + table + 4);   // lwz r11,4(r6)
        const uint32_t t1 = p.L32((b44 << 3) + table + 4);   // lwz r8,4(r5)
        const uint32_t t2 = p.L32((b45 << 3) + table + 4);   // lwz r7,4(r4)
        const uint32_t t3 = p.L32((b46 << 3) + table + 4);   // lwz r6,4(r3)
        const uint32_t t4 = p.L32((b47 << 3) + table + 4);   // lwz r5,4(r10)
        const uint32_t key = p.L32(t0 + 36);               // lwz r9,36(r11)
        table_textures = table;
        p.E32(R1 + 96, t0);                                  // stw r11,96(r1) ... stw r5,112(r1) (16617-16627)
        p.E32(R1 + 100, t1);
        p.E32(R1 + 104, t2);
        p.E32(R1 + 108, t3);
        p.E32(R1 + 112, t4);
        if (key != r25) {                                  // cmplw r9,r25; beq
          const uint32_t b42 = p.L8(sub + 42);                // lbz r10,42(r31)
          uint32_t light = 0;                                   // mr r11,r18
          if (b42 != 255u) {
            const uint32_t t60 = p.L32(r29 + 60);             // lwz r9,60(r29)
            light = p.L32((b42 << 3) + t60 + 4);                // lwz r11,4(r4)
          }
          p.E32(R1 + 92, uint32_t(e.r8));                     // stw r20,92(r1)
          p.E32(R1 + 84, light);                                // stw r11,84(r1)
          p.Packet(e.r3, r31, r29, e.r7, r30, frame64 + 96, e.r5, e.r6);  // (16669)
        }
      }
      const int32_t n = int32_t(p.L32(r28 + 16));            // lwz r3,16(r28) (16672)
      index = r23;
      r27 += 256;
      if (!(int32_t(uint32_t(index)) < n)) {
        break;
      }
    }
  }
  p.CallAB8(e.r4, frame64 + 128);  // (16688)
  p.Epilogue(frame64);
}

// ---------------------------------------------------------------------------------------------------------------
// Policy of the normal path: the real memory and the real calls.
// ---------------------------------------------------------------------------------------------------------------
struct Real {
  PPCContext& ctx;
  uint8_t* base;

  uint32_t L8(uint32_t d) const { return Read8(base, d); }
  uint32_t L16(uint32_t d) const { return Read16(base, d); }
  uint32_t L32(uint32_t d) const { return Read32(base, d); }
  void E32(uint32_t d, uint32_t v) const { Write32(base, d, v); }

  void Prologue(const Entry& e, uint64_t frame64) const {
    ctx.r12.u64 = e.lr;  // mflr r12
    ctx.lr = kLrPrologue;
    ctx.r1.u64 = frame64;
  }
  void Epilogue(uint64_t frame64) const {
    ctx.r1.u64 = frame64 + kFrame;  // addi r1,r1,784 (in 64 bits, like the original)
  }
  void OutputFlag(uint32_t flags, uint32_t exit) const {
    ctx.r11.u64 = kR11Flag;
    ctx.r10.u64 = flags & 0x800u;
    ctx.r9.u64 = exit;
  }

  // --- prefetches (only here; in the replay they do nothing) ---
  void AdvanceInitial(uint32_t list) const {
    if constexpr (!NFSMW_EVIEW_ANTICIPATE) return;
    Anticipate(base, list);
    Anticipate(base, list + 63);
    Anticipate(base, list + 256);
    Anticipate(base, list + 256 + 63);
  }
  // sig = submesh i+1 (requested one iteration ago). Submesh i+2 is requested and, from i+1, its material and
  // its first texture: the two pointers that miss the most in the profile (the key [texture0+36] here, and
  // +81/+84 of the texture and +4/+6 of the material in the packet). Not the other four textures: they are
  // usually shared and prefetching them cost more than it saved with a warm cache (measured on PC).
  // Reads: +28 and +43 of submesh i+1, which exists (i+1 < n) and whose +28 the original always reads; and the
  // texture table only if the original has already read it (table != 0). The rest are PRFM, which never fault.
  void AdvanceLoop(uint32_t sig, bool there_is_other, uint32_t table) const {
    if constexpr (!NFSMW_EVIEW_ANTICIPATE) return;
    if (there_is_other) {
      Anticipate(base, sig + 256);
      Anticipate(base, sig + 256 + 63);
    }
    Anticipate(base, Read32(base, sig + 28) + 4);
    if (table != 0) {
      const uint32_t t0 = Read32(base, (Read8(base, sig + 43) << 3) + table + 4);
      Anticipate(base, t0 + 36);
      Anticipate(base, t0 + 84);
    }
  }

  // --- calls: same argument registers, same lr and r1 = frame (already set in the prologue) ---
  void Call500010(uint64_t r3, uint64_t r4, uint64_t r5) const {
    ctx.r5.u64 = r5;
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLr500010;
    sub_82500010(ctx, base);
  }
  void CallB18(uint64_t r3, uint64_t r4) const {
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrB18;
    sub_82218B18(ctx, base);
  }
  uint32_t Visible(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6) const {
    ctx.r6.u64 = r6;
    ctx.r5.u64 = r5;
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrVisible;
    sub_8243E7D8(ctx, base);
    return ctx.r3.u32;
  }
  void Packet(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9,
               uint64_t r10) const {
    ctx.r10.u64 = r10;
    ctx.r9.u64 = r9;
    ctx.r8.u64 = r8;
    ctx.r7.u64 = r7;
    ctx.r6.u64 = r6;
    ctx.r5.u64 = r5;
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrPacket;
    sub_82452868(ctx, base);
  }
  void CallAB8(uint64_t r3, uint64_t r4) const {
    ctx.r4.u64 = r4;
    ctx.r3.u64 = r3;
    ctx.lr = kLrAB8;
    sub_82218AB8(ctx, base);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Guard: trace of the original's calls and dry replay of the native version.
// ---------------------------------------------------------------------------------------------------------------
enum Type : uint8_t { k500010 = 0, kB18 = 1, kPacket = 2, kAB8 = 3 };
constexpr const char* kNameCall[] = {"8250_0010", "8221_8B18", "packet 8245_2868", "8221_8AB8"};

struct Event {
  uint8_t type;
  uint64_t r[8];       // r3..r10 (the ones not passed, at 0)
  uint32_t stack[7];    // packet: the words at r1+84, +92 and +96..112 that it reads
};

constexpr uint32_t kMaxEvents = 1024;  // 3 + one per drawn submesh
constexpr uint32_t kWords = 9;       // binding, [r28+64] y r1+84..112
constexpr uint32_t kStackDead = 2048;  // below r1: GetVisibleState's frame (128) with a wide margin

struct Shadow {
  uint32_t dir[kWords];
  uint32_t val[kWords];
  uint32_t n = 0;
  bool unexpected = false;  // the native version wrote outside the 9
  uint32_t dir_unexpected = 0;

  void Add(uint8_t* base, uint32_t d) {
    dir[n] = d;
    val[n] = Read32(base, d);
    ++n;
  }
  // Read of n bytes (big-endian) with whatever is in the shadow on top of memory.
  uint32_t Read(uint8_t* base, uint32_t d, uint32_t bytes) const {
    uint32_t v = 0;
    for (uint32_t b = 0; b < bytes; ++b) {
      const uint32_t x = d + b;
      uint32_t byte = *Pointer(base, x);
      for (uint32_t i = 0; i < n; ++i) {
        if (uint32_t(x - dir[i]) < 4u) {
          byte = (val[i] >> (8 * (3 - (x - dir[i])))) & 0xFFu;
        }
      }
      v = (v << 8) | byte;
    }
    return v;
  }
  void Write(uint32_t d, uint32_t v) {
    for (uint32_t i = 0; i < n; ++i) {
      if (dir[i] == d) {
        val[i] = v;
        return;
      }
    }
    if (!unexpected) {
      unexpected = true;
      dir_unexpected = d;
    }
  }
};

struct Trace {
  Entry e;
  uint64_t frame64 = 0;
  Event ev[kMaxEvents];
  uint32_t n = 0;
  bool full = false;
  bool repeated = false;       // the replay has already been done (in 8221_8AB8 or on return)
  const char* miss = nullptr;  // first difference of the replay
  uint32_t where = 0;          // event index
  uint32_t path = 0;         // 1, 2 or 3 depending on the native version
  uint32_t flags = 0;       // path 2: what the native version leaves in r9-r11
  uint32_t exit = 0;
  uint32_t memory_original = 0;  // "memory_block different": what the original left in that word
  Shadow s;
};

// Thread with a Render check in progress (its context) and its trace. One thread at a time: the trace, the
// replay context and the stack copy are global (no allocations and no extra ~5 KB on the host stack).
std::atomic<PPCContext*> g_trace_ctx{nullptr};
Trace g_trace;
PPCContext g_cv;
alignas(16) uint8_t g_stack[kStackDead];

// Replay policy: reads through the shadow, writes nothing, compares each call with the trace.
struct Repetition {
  uint8_t* base;
  Trace& t;
  PPCContext& cv;  // separate context to call GetVisibleState
  uint32_t k = 0;  // next event of the trace
  bool ab8 = false;
  uint64_t ab8_r3 = 0, ab8_r4 = 0;
  uint32_t path = 3;

  uint32_t L8(uint32_t d) const { return t.s.Read(base, d, 1); }
  uint32_t L16(uint32_t d) const { return t.s.Read(base, d, 2); }
  uint32_t L32(uint32_t d) const { return t.s.Read(base, d, 4); }
  void E32(uint32_t d, uint32_t v) { t.s.Write(d, v); }
  void Prologue(const Entry&, uint64_t) {}
  void Epilogue(uint64_t) {}
  void OutputFlag(uint32_t flags, uint32_t exit) {
    path = 2;
    t.flags = flags;
    t.exit = exit;
  }
  void AdvanceInitial(uint32_t) {}
  void AdvanceLoop(uint32_t, bool, uint32_t) {}

  void Miss(const char* que) {
    if (!t.miss) {
      t.miss = que;
      t.where = k;
    }
  }
  void Compare(const Event& x) {
    if (k >= t.n) {
      if (!t.full) {  // with the trace full, whatever is beyond it cannot be compared (not a difference)
        Miss("la native llama de mas");
      }
      ++k;
      return;
    }
    const Event& y = t.ev[k];
    if (y.type != x.type) {
      Miss("other function");
    } else if (std::memcmp(y.r, x.r, sizeof(x.r)) != 0) {
      Miss("others register_values");
    } else if (std::memcmp(y.stack, x.stack, sizeof(x.stack)) != 0) {
      Miss("other stack");
    }
    ++k;
  }
  void Call500010(uint64_t r3, uint64_t r4, uint64_t r5) {
    Compare(Event{k500010, {r3, r4, r5, 0, 0, 0, 0, 0}, {}});
  }
  void CallB18(uint64_t r3, uint64_t r4) { Compare(Event{kB18, {r3, r4, 0, 0, 0, 0, 0, 0}, {}}); }
  uint32_t Visible(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6) {
    cv.r1.u64 = t.frame64;
    cv.r6.u64 = r6;
    cv.r5.u64 = r5;
    cv.r4.u64 = r4;
    cv.r3.u64 = r3;
    cv.lr = kLrVisible;
    sub_8243E7D8(cv, base);
    return cv.r3.u32;
  }
  void Packet(uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9,
               uint64_t r10) {
    Event x{kPacket, {r3, r4, r5, r6, r7, r8, r9, r10}, {}};
    const uint32_t R1 = uint32_t(t.frame64);
    x.stack[0] = L32(R1 + 84);
    x.stack[1] = L32(R1 + 92);
    for (uint32_t i = 0; i < 5; ++i) {
      x.stack[2 + i] = L32(R1 + 96 + 4 * i);
    }
    Compare(x);
  }
  void CallAB8(uint64_t r3, uint64_t r4) {
    ab8 = true;
    ab8_r3 = r3;
    ab8_r4 = r4;
  }
};

// ---------------------------------------------------------------------------------------------------------------
// Counters, report and shutdown (no atomic read-modify-write on the normal path: A57 without LSE).
// ---------------------------------------------------------------------------------------------------------------
constexpr uint64_t kChecksRender = 30000;  // about 1-2 s of racing; then 1 of every kPeriod
constexpr uint64_t kPeriod = 4096;

std::atomic<int8_t> g_active{-1};
std::atomic<bool> g_off{false};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_native{0};           // last 10 s
std::atomic<uint64_t> g_original{0};        // last 10 s
std::atomic<uint64_t> g_checked{0};       // last 10 s
std::atomic<uint64_t> g_checked_total{0};
std::atomic<uint64_t> g_without_check{0};     // trace full or another thread checking (not differences)
std::atomic<uint64_t> g_measurements{0};           // last 10 s: timed native calls (1 in 16)
std::atomic<uint64_t> g_measurements_ns{0};
std::atomic<int64_t> g_next_ms{0};
std::atomic<int64_t> g_since_ms{0};           // start of the report period

template <typename T>
inline void Add(std::atomic<T>& c, T n) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline bool Active() {
  int8_t a = g_active.load(std::memory_order_relaxed);
  if (a < 0) [[unlikely]] {
    a = REXCVAR_GET(nfsmw_eview_native) ? 1 : 0;
    g_active.store(a, std::memory_order_relaxed);
  }
  return a != 0;
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Report() {
  const int64_t now = NowNs() / 1000000;
  const int64_t next = g_next_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  g_next_ms.store(now + 10000, std::memory_order_relaxed);
  const int64_t since = g_since_ms.exchange(now, std::memory_order_relaxed);
  if (next == 0) {
    REXLOG_INFO("[eview] Render (8243E358) en native (build 176): se comprueban contra la original the primeras {} "
                "calls y after 1 de every {}",
                kChecksRender, kPeriod);
    g_measurements.store(0, std::memory_order_relaxed);
    g_measurements_ns.store(0, std::memory_order_relaxed);
    return;
  }
  const double seconds = double(now - since) / 1000.0;
  const uint64_t native = g_native.exchange(0, std::memory_order_relaxed);
  const uint64_t measurements = g_measurements.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_measurements_ns.exchange(0, std::memory_order_relaxed);
  const double us = measurements ? double(ns) / double(measurements) / 1000.0 : 0.0;
  NFSMW_REPORT_DEFERRED("[eview] last {:.1f} s: Render {} native ({:.2f} us por call con GetVisibleState y el packet "
              "inside = {:.1f} ms/s), {} original, {} checked{}; checked since el arranque {} ({} sin poder "
              "should_check)",
              seconds, native, us, seconds > 0 ? us * double(native) / 1000.0 / seconds : 0.0,
              g_original.exchange(0, std::memory_order_relaxed), g_checked.exchange(0, std::memory_order_relaxed),
              g_off.load(std::memory_order_relaxed) ? " | OFF por difference" : "",
              g_checked_total.load(std::memory_order_relaxed), g_without_check.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------------------------------------------
// The normal path.
// ---------------------------------------------------------------------------------------------------------------
[[gnu::always_inline]] inline void RenderNative(PPCContext& ctx, uint8_t* base) {
  Real p{ctx, base};
  const Entry e = ReadEntry(ctx);
  Body(p, e);
}

// ---------------------------------------------------------------------------------------------------------------
// The dry replay (in 8221_8AB8 or, if the original does not get there, on return). Leaves memory and context as
// they were (except the host FPCR, which is restored to the context's).
// ---------------------------------------------------------------------------------------------------------------
[[gnu::noinline]] void Repeat(PPCContext& ctx, uint8_t* base, bool en_ab8) {
  Trace& t = g_trace;
  t.repeated = true;
  const uint32_t R1 = uint32_t(t.frame64);
  // GetVisibleState writes its frame below r1: it is saved and restored (dead stack, but memory stays identical).
  const uint32_t low = R1 - kStackDead;
  const bool stack_ok = Offset(low) == Offset(R1 - 1);
  if (stack_ok) {
    std::memcpy(g_stack, Pointer(base, low), kStackDead);
  }
  g_cv = ctx;  // separate context for GetVisibleState: the real one is not touched
  Repetition p{base, t, g_cv};
  Body(p, t.e);
  if (stack_ok) {
    std::memcpy(Pointer(base, low), g_stack, kStackDead);
  }
  ctx.fpscr.setcsr(ctx.fpscr.csr);  // GetVisibleState (in g_cv) may have changed the denormal mode
  t.path = p.ab8 ? 3u : p.path == 2 ? 2u : 1u;

  if (!t.miss && p.k != t.n && !t.full) {
    p.Miss("la original llama a mas functions");
  }
  if (!t.miss) {
    if (en_ab8 != p.ab8) {
      t.miss = en_ab8 ? "la native no llega a 8221_8AB8" : "la native llama a 8221_8AB8 y la original no";
    } else if (en_ab8 && (p.ab8_r3 != ctx.r3.u64 || p.ab8_r4 != ctx.r4.u64)) {
      t.miss = "8221_8AB8 con others register_values";
    } else if (t.s.unexpected) {
      t.miss = "write outside de the 9 words";
    } else {
      for (uint32_t i = 0; i < t.s.n; ++i) {
        if (Read32(base, t.s.dir[i]) != t.s.val[i]) {
          t.miss = "memory_block different";
          t.where = i;
          t.memory_original = Read32(base, t.s.dir[i]);
          break;
        }
      }
    }
  }
}

void Turn_off(const PPCContext& ctx, uint64_t n, const char* que, bool of_the_trace) {
  g_off.store(true, std::memory_order_relaxed);
  const Trace& t = g_trace;
  std::string detail;
  if (!of_the_trace) {
    detail = fmt::format("; al volver: r1 0x{:X} r3 0x{:X} lr 0x{:X} r12 0x{:X}", ctx.r1.u64, ctx.r3.u64, ctx.lr,
                          ctx.r12.u64);
  } else if (std::strcmp(que, "memory_block different") == 0 && t.where < t.s.n) {
    detail = fmt::format("; word 0x{:08X}: native 0x{:08X}, original 0x{:08X}", t.s.dir[t.where],
                          t.s.val[t.where], t.memory_original);
  } else if (t.where < t.n) {
    const Event& y = t.ev[t.where];
    detail = fmt::format("; call {} de la original: {} r3 0x{:X} r4 0x{:X} r5 0x{:X} r6 0x{:X} r7 0x{:X}",
                          t.where, kNameCall[y.type], y.r[0], y.r[1], y.r[2], y.r[3], y.r[4]);
  } else if (t.n == 0 && t.path == 3) {
    detail = "; la trace esta EMPTY: the calls de la Render original no pasan por los hooks de este file "
              "(missing revert_calls_eview.py en app/generated/default?)";
  }
  REXLOG_INFO("[eview] DIFFERENCE en Render ({}; call {}, path {}): vista 0x{:08X} model 0x{:08X} matrix "
              "0x{:08X} r1 0x{:08X}; la original hizo {} calls{}. Path native OFF para el rest de la "
              "session, se queda la original",
              que, n, t.path, uint32_t(t.e.r3), uint32_t(t.e.r4), uint32_t(t.e.r5), uint32_t(t.e.r1), t.n, detail);
}

// Render guard. Always leaves the original's state.
[[gnu::noinline]] void CheckRender(PPCContext& ctx, uint8_t* base, uint64_t n) {
  PPCContext* free = nullptr;
  if (!g_trace_ctx.compare_exchange_strong(free, &ctx, std::memory_order_acquire, std::memory_order_relaxed)) {
    // Another thread is checking: the original at first; afterwards, the already checked native version.
    Add(g_without_check, uint64_t(1));
    if (n <= kChecksRender) {
      Add(g_original, uint64_t(1));
      __imp__sub_8243E358(ctx, base);
    } else {
      RenderNative(ctx, base);
      Add(g_native, uint64_t(1));
    }
    return;
  }
  Trace& t = g_trace;
  t.e = ReadEntry(ctx);
  t.frame64 = Frame64(t.e.r1);
  t.n = 0;
  t.full = false;
  t.repeated = false;
  t.miss = nullptr;
  t.where = 0;
  t.path = 0;
  // 1. Snapshot of the words Render can write. [r28+64] only if the original is going to walk the mesh (the
  //    same reads it will do before its first call).
  const uint32_t R1 = uint32_t(t.frame64);
  t.s.n = 0;
  t.s.unexpected = false;
  t.s.Add(base, R1);
  const uint32_t r29 = Read32(base, uint32_t(t.e.r4) + 12);
  if (r29 != 0 && ((Read16(base, r29 + 14) & 0x800u) == 0 || Read8(base, kFlagOutput) == 0)) {
    t.s.Add(base, Read32(base, r29) + 64);
  }
  t.s.Add(base, R1 + 84);
  t.s.Add(base, R1 + 92);
  for (uint32_t i = 0; i < 5; ++i) {
    t.s.Add(base, R1 + 96 + 4 * i);
  }
  // 2. The original, for real, with its calls in trace mode (3. the replay happens inside 8221_8AB8).
  __imp__sub_8243E358(ctx, base);
  if (!t.repeated) {
    Repeat(ctx, base, false);  // paths 1 and 2: the original did not reach 8221_8AB8
  }
  g_trace_ctx.store(nullptr, std::memory_order_release);
  // 4. Registers al volver.
  const char* que = t.miss;
  if (!que) {
    const uint64_t frame64 = t.frame64;
    if (ctx.r1.u64 != frame64 + kFrame) {
      que = "r1 different";
    } else if (t.path == 3) {
      if (ctx.r3.u64 != t.e.r4) que = "r3 different";
      else if (ctx.lr != kLrAB8) que = "lr different";
    } else {
      if (ctx.r3.u64 != t.e.r3) que = "r3 different";
      else if (ctx.lr != kLrPrologue || ctx.r12.u64 != t.e.lr) que = "r12/lr different";
      else if (t.path == 2 && (ctx.r11.u64 != kR11Flag || ctx.r10.u64 != (t.flags & 0x800u) ||
                                 ctx.r9.u64 != t.exit)) que = "r9-r11 different";
    }
  }
  if (t.full && !que) {
    Add(g_without_check, uint64_t(1));  // too many submeshes for the trace: not counted as checked
    return;
  }
  if (que) {
    Turn_off(ctx, n, que, que == t.miss);
    return;
  }
  Add(g_checked, uint64_t(1));
  const uint64_t total = g_checked_total.load(std::memory_order_relaxed) + 1;
  g_checked_total.store(total, std::memory_order_relaxed);
  if (total == kChecksRender) {
    REXLOG_INFO("[eview] Render: {} calls checked contra la original (calls, arguments, stack y register_values), "
                "0 differences: path native en marcha, y sigue checking 1 de every {}",
                total, kPeriod);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Trace hooks: on the normal path, one relaxed atomic read and the real function.
// ---------------------------------------------------------------------------------------------------------------
inline bool InTrace(const PPCContext& ctx, uint64_t lr) {
  return g_trace_ctx.load(std::memory_order_relaxed) == &ctx && ctx.lr == lr && ctx.r1.u64 == g_trace.frame64 &&
         !g_trace.repeated;
}
inline void Point(const Event& x) {
  Trace& t = g_trace;
  if (t.n >= kMaxEvents) {
    t.full = true;
    return;
  }
  t.ev[t.n++] = x;
}
[[gnu::noinline]] void TracePacket(PPCContext& ctx, uint8_t* base) {
  Event x{kPacket,
           {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64},
           {}};
  const uint32_t R1 = ctx.r1.u32;
  x.stack[0] = Read32(base, R1 + 84);
  x.stack[1] = Read32(base, R1 + 92);
  for (uint32_t i = 0; i < 5; ++i) {
    x.stack[2 + i] = Read32(base, R1 + 96 + 4 * i);
  }
  Point(x);
}

}  // namespace

// The entry point of Render: the sub_8243E358 hook in nfsmw_shadows_lod.cpp calls it instead of the original.
void Render(PPCContext& ctx, uint8_t* base) {
  if (!Active() || g_off.load(std::memory_order_relaxed)) {
    __imp__sub_8243E358(ctx, base);
    return;
  }
  const uint64_t n = g_calls.load(std::memory_order_relaxed) + 1;
  g_calls.store(n, std::memory_order_relaxed);
  if (n <= kChecksRender || (n & (kPeriod - 1)) == 0) [[unlikely]] {
    CheckRender(ctx, base, n);
  } else if ((n & 15) == 0) [[unlikely]] {
    const int64_t t0 = NowNs();
    RenderNative(ctx, base);
    Add(g_measurements_ns, uint64_t(NowNs() - t0));
    Add(g_measurements, uint64_t(1));
    Add(g_native, uint64_t(1));
  } else {
    RenderNative(ctx, base);
    Add(g_native, uint64_t(1));
  }
  if ((n & (kPeriod - 1)) == 0) [[unlikely]] {
    Report();
  }
}

}  // namespace nfsmw::eview

// Render's calls to these four functions must go to sub_X and not to __imp__sub_X (tools/direct_calls.py
// leaves them as sub_X because they are hooked here). Only Render calls them: the hooks change nothing for anyone
// else.
REX_HOOK_RAW(sub_82452868) {  // the draw packet of each submesh
  using namespace nfsmw::eview;
  if (InTrace(ctx, kLrPacket)) [[unlikely]] {
    TracePacket(ctx, base);
  }
  __imp__sub_82452868(ctx, base);
}
REX_HOOK_RAW(sub_82500010) {
  using namespace nfsmw::eview;
  if (InTrace(ctx, kLr500010)) [[unlikely]] {
    Point(Event{k500010, {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, 0, 0, 0, 0, 0}, {}});
  }
  __imp__sub_82500010(ctx, base);
}
REX_HOOK_RAW(sub_82218B18) {  // puts the model's replacement textures in place
  using namespace nfsmw::eview;
  if (InTrace(ctx, kLrB18)) [[unlikely]] {
    Point(Event{kB18, {ctx.r3.u64, ctx.r4.u64, 0, 0, 0, 0, 0, 0}, {}});
  }
  __imp__sub_82218B18(ctx, base);
}
REX_HOOK_RAW(sub_82218AB8) {  // removes them: Render's last call, where the native version is replayed
  using namespace nfsmw::eview;
  if (InTrace(ctx, kLrAB8)) [[unlikely]] {
    Repeat(ctx, base, true);
  }
  __imp__sub_82218AB8(ctx, base);
}
