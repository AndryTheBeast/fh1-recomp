// fh1 - native renderer: shader microcode versions.
//
// IM_LOAD WITHOUT memcmp. On every IM_LOAD (op 27) LoadShaderCached (fh1_native_system.cpp) used to
// compare the whole microcode against guest memory with memcmp, because the D3D patches vertex shaders in
// place: ~0.6 us per IM_LOAD and ~41,000 IM_LOADs per second in a race (~25 ms/s of the PM4 ring thread). Now
// whoever writes microcode reports it here, and the ring only reads it again if someone has written to its
// slot since the last time it checked.
//
// WHO WRITES MICROCODE THAT AN IM_LOAD READS (recompiled code, read instruction by instruction)
//   - sub_825A3AF0 (the FlushState shaders) is the only one that writes IM_LOAD packets: the VS from [VS+40]
//     with [VS+600] bytes and the PS from [PS+12] with [PS+60] bytes, with the physical address from Physical().
//   - The constructors, already hooked in fh1_native_hooks.cpp: sub_8259C038 (VS) allocates physical
//     memory, copies the microcode from the container and leaves the address in [VS+40] (sub_8259BF68);
//     sub_8259BC90 (PS) does the same, in [PS+12]. The memory of a freed shader can come back for another
//     one at the same address: that is why they report.
//   - The VS fetch patcher, sub_825A2FB8 (r3 device, r4 VS, r5 destination, r6 declaration), which writes
//     12 bytes per fetch at r5 + 12 * index. It only has two call sites:
//       * sub_825A3AF0, in place (r5 = [VS+40], return address 0x825A3C7C): when the declaration or the
//         strides change and the GPU has already passed the fence of that VS's last IM_LOAD ([VS+8] against
//         the completed fence);
//       * sub_825A37D8, on a copy of the VS in the ring itself that goes out as IM_LOAD_IMMEDIATE (r5 = the
//         copy, return address 0x825A38D0): when the GPU has not passed that fence yet or outputs the PS does
//         not read must be nulled (sub_825A36A8, also on the copy). It does not touch the memory of any
//         IM_LOAD.
//   - Nobody else writes to the microcode of a live shader. If anything did, the ring's guard
//     (LoadShaderCached) would catch it; and a call to the patcher that is neither of the two turns the
//     shortcut off.
//
// THE PROTOCOL BETWEEN THREADS
//   Nobody sleeps or wakes anybody here: the ring only decides whether it can trust its copy, and when in
//   doubt it does the usual memcmp.
//   - Per slot (hash of the physical start address, the one the IM_LOAD carries), two counters that only go
//     up, start and fin, and a global counter that goes up when each write finishes.
//   - Writer (game or loading thread): start++ and a full barrier before writing; fin++ and global++
//     (release) after. With several writers at once on the same slot, start != fin while any remains.
//   - Ring, when checking with memcmp (the usual path): reads global, start and fin (acquire), does the
//     memcmp or the copy, issues an acquire barrier and rereads start. Only if start == end (nobody halfway)
//     and the reread matches (nobody started while it was reading) does it record in the cache way: valid
//     with (start, global). If a writer started during the read and the memcmp saw any of its bytes, thanks
//     to the two barriers the reread already sees its start++.
//   - Ring, when looking up: if global matches the way's, nobody has finished writing since then and it is
//     valid; otherwise, acquire barrier (whoever bumped global bumped its slot's start first) and, if start
//     is unchanged, it is valid and the way's global is refreshed. Otherwise, memcmp.
//   - Why this is enough: the D3D writes the IM_LOAD that uses a patch after finishing the patch, and
//     publishes it with the write to CP_RB_WPTR (store release in WriteRegisterMmio), which the ring reads
//     with acquire before reading the packet: when it processes that IM_LOAD the ring already sees global and
//     start bumped. A patch while the ring processes an earlier IM_LOAD of the same VS should not happen (the
//     D3D checks the fence before patching in place); if it did, the ring would use the previous contents,
//     which are the ones of that IM_LOAD, and the guard would see it.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/platform.h>

namespace fh1::native::microcode {

constexpr uint32_t kBitsSlot = 12;
constexpr uint32_t kSlots = uint32_t(1) << kBitsSlot;  // 4,096 slots of 8 bytes: 32 KB

struct Slot {
  std::atomic<uint32_t> start{0};
  std::atomic<uint32_t> fin{0};
};
inline Slot g_slots[kSlots];
inline std::atomic<uint32_t> g_global{0};

// Counts for the ring report, since startup.
inline std::atomic<uint64_t> g_patches_in_its_room{0};
inline std::atomic<uint64_t> g_patches_in_copy{0};
inline std::atomic<uint64_t> g_creations{0};
// Calls to the patcher that are neither of the two known ones. With one of them, the ring turns the shortcut off.
inline std::atomic<uint64_t> g_patches_others{0};
inline std::atomic<uint32_t> g_other_return{0};
inline std::atomic<uint32_t> g_other_target{0};

#if defined(NFSMW_NATIVE_SHADER_LIBRARY)
// With the experimental library the constructors are hooked by nfsmw-nx's shader hooks, which does not report:
// no shortcut.
inline constexpr bool kCreationsWatched = false;
#else
inline constexpr bool kCreationsWatched = true;
#endif

// The physical address sub_825A3AF0 puts in the IM_LOAD from the object's virtual one (rlwinm
// r10,r28,12,20,31; addi r10,r10,512; rlwinm r10,r10,0,19,19; clrlwi r9,r28,3; add): the low 29 bits, plus
// 4 KB from 0xE0000000.
inline uint32_t Physical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + ((((address >> 20) & 0xFFFu) + 0x200u) & 0x1000u);
}

// Slot of the physical start address of a microcode (without the two type bits of the IM_LOAD).
inline uint32_t SlotOf(uint32_t physical) {
  return ((physical >> 2) * 0x9E3779B1u) >> (32 - kBitsSlot);
}

// A guest word read from a hook (on Win32 the area from 0xE0000000 is 0x1000 higher, like
// REX_PHYS_HOST_OFFSET in the generated code's precompiled header).
inline uint32_t ReadGuest32(const uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32
  const uint32_t displacement = address >= 0xE0000000u ? 0x1000u : 0u;
#else
  const uint32_t displacement = 0u;
#endif
  uint32_t input_value;
  std::memcpy(&input_value, base + address + displacement, 4);
  return __builtin_bswap32(input_value);
}

// Count without atomic read-modify-write (A57 without LSE). Almost always written by a single thread; if two
// coincided some count would be lost, but it would never stay at zero after counting.
inline void Count(std::atomic<uint64_t>& count) {
  count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// ---- Writers (game or loading thread) ----

// Before writing to the microcode whose start address falls in that slot.
inline void BeginWrite(uint32_t slot) {
  g_slots[slot].start.fetch_add(1, std::memory_order_relaxed);
  // The start++ must be visible before any new byte of the microcode (dmb ish on AArch64).
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

// After writing. Everything before (the bytes, the fin++, the counts) is visible before the new global.
inline void FinishWrite(uint32_t slot) {
  g_slots[slot].fin.fetch_add(1, std::memory_order_release);
  g_global.fetch_add(1, std::memory_order_release);
}

// From the constructors, after the original (object in r3; 0 if it failed). The memory is new and no live
// IM_LOAD reads it: it is enough for the versions to go up before the game can use the shader, and the
// object does not leave the constructor until this returns.
inline void NotifyCreation(const uint8_t* base, uint32_t object, bool vertices) {
  if (!object) {
    return;
  }
  const uint32_t address = ReadGuest32(base, object + (vertices ? 40u : 12u));
  if (!address) {
    return;
  }
  const uint32_t slot = SlotOf(Physical(address) & ~uint32_t(3));
  BeginWrite(slot);
  Count(g_creations);
  FinishWrite(slot);
}

// ---- PM4 ring thread ----

// Fast path: the global without a barrier. The CP_RB_WPTR chain (release in the game, acquire in the ring)
// already guarantees seeing whatever was bumped before the IM_LOAD being processed was written.
inline uint32_t GlobalRelaxed() {
  return g_global.load(std::memory_order_relaxed);
}

// If the global no longer matches the cache way's: acquire barrier (synchronizes with the release global++ of
// the writer, which bumped its start first) and the slot's start.
inline uint32_t StartAfterGlobal(uint32_t slot) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return g_slots[slot].start.load(std::memory_order_relaxed);
}

struct ReadAccess {
  uint32_t global = 0;
  uint32_t start = 0;
  uint32_t fin = 0;
};

// Before reading the guest microcode to compare or copy it.
inline ReadAccess BeforeOfRead(uint32_t slot) {
  ReadAccess l;
  l.global = g_global.load(std::memory_order_acquire);
  l.start = g_slots[slot].start.load(std::memory_order_acquire);
  l.fin = g_slots[slot].fin.load(std::memory_order_acquire);
  return l;
}

// After: true if what was read is valid with the versions from BeforeOfRead (nobody was writing or started
// meanwhile).
inline bool AfterOfRead(uint32_t slot, const ReadAccess& l) {
  std::atomic_thread_fence(std::memory_order_acquire);
  return l.start == l.fin && g_slots[slot].start.load(std::memory_order_relaxed) == l.start;
}

}  // namespace fh1::native::microcode
