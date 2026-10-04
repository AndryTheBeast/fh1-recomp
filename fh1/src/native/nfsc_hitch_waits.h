// nfsc - who waits for whom during stutters.
//
// In the 216 frames over 45 ms of one console run almost nobody is working: the main thread waits for the
// preparer's handoff, the PM4 ring runs out of work or waits for the GPU, and the preparer sleeps inside
// sub_826E8EE8 (COM-style virtual calls, unidentified). Each wait adds its time here, and the "[hitch]" line
// (nfsc_native_targets.cpp) writes how much each one took in that frame.
// It only costs two clock reads per wait, and those waits sleep anyway.

#pragma once

#include <atomic>
#include <cstdint>

namespace nfsc::waits {

enum Type : uint32_t {
  kHandoffExecutor,    // Main XThread waiting for the preparer to hand over the frame (sub_8262D988)
  kHandoffPreparer,  // the preparer waiting for the executor to finish the previous one (sub_8262D988)
  kRoomRing,       // the D3D waiting for the ring to advance (sub_825A5D18)
  kGameMiddle,        // the game inside sub_826E8EE8
  kRingWithoutWork,  // the ring thread with no packets to read
  kRingRegMem,      // the ring thread in a WAIT_REG_MEM (waiting for the game to write)
  // The game's side in the stutter frame ("[hitch] game" line).
  kExecutorWithoutCommands,   // Main XThread with the list open and no new commands (sub_823C83F8)
  kPreparerList,      // the preparer filling the command list (all of sub_82445660: culling and eView)
  kPreparerOutside,      // the preparer between two fills: its simulation plus its handoff wait
  kPreparerScenery,  // TreeCull (sub_824C2F48) with its DrawAScenery calls, inside the fill
  kNumTypes
};

inline std::atomic<uint64_t> g_ns[kNumTypes];
inline std::atomic<uint64_t> g_times[kNumTypes];
inline std::atomic<uint32_t> g_vtable_middle{0};    // vtable of the object of the last call to sub_826E8EE8
inline std::atomic<uint32_t> g_caller_middle{0};  // and its return address

// What the ring re-checks of the textures (XXH3 of the guest bytes) and how many checks are postponed by
// the per-frame budget (nfsc_native_fingerprint_kb_per_frame).
inline std::atomic<uint64_t> g_bytes_fingerprint{0};
inline std::atomic<uint64_t> g_fingerprints_deferred{0};

// What the ring does in the stutter frame. The race stutters are the ring being full (the game waits
// 18-54 ms for space) and the texture checks stay under 8 MB: this shows where the ring's time goes. Only the
// ring thread writes all of this.
inline std::atomic<uint64_t> g_draws{0};             // draws read from the ring
inline std::atomic<uint64_t> g_ns_ring_working{0};  // reading and executing packets
inline std::atomic<uint64_t> g_ns_textures{0};         // check, detile and prepare the textures in use
inline std::atomic<uint64_t> g_textures_uploads{0};    // textures that changed and are uploaded again
inline std::atomic<uint64_t> g_bytes_uploaded{0};
inline std::atomic<uint64_t> g_textures_created{0};
// Of that texture time, what goes into the two fingerprints (XXH3 of the guest memory and of the already
// prepared data).
inline std::atomic<uint64_t> g_ns_fingerprint_raw{0};
inline std::atomic<uint64_t> g_ns_fingerprint_data{0};
// How long the ring thread waits for the vertex copy thread (WaitUploads in nfsc_native_draws.cpp:
// before each submit and before returning the read pointer to the game), and how many times it really waits.
// Previously this was only reported when the game closed. Only the ring thread writes it.
inline std::atomic<uint64_t> g_ns_waiting_copies{0};
inline std::atomic<uint64_t> g_waits_copies{0};
// What the ring copies itself during those waits, from what the thread had not taken yet, and how many
// copies (nfsc_native_uploads_help). g_ns_waiting_copies is therefore only the wait. Ring thread only.
inline std::atomic<uint64_t> g_ns_helping_copies{0};
inline std::atomic<uint64_t> g_copies_helped{0};
// What the ring spends creating textures (image, pool memory and view; "textures X ms" starts after they
// are created and did not include it), how long it waits for the binding thread and how many textures that
// thread bound (nfsc_native_texture_binding_thread in nfsc_native_draws.cpp). Only the ring thread writes it.
inline std::atomic<uint64_t> g_ns_create_textures{0};
inline std::atomic<uint64_t> g_ns_waiting_bindings{0};
inline std::atomic<uint64_t> g_textures_bound_thread{0};
// New textures with the fingerprint, the detiling and the byte order done on the fingerprint thread
// (nfsc_native_texture_fingerprint_thread in nfsc_native_draws.cpp): what the ring spends copying guest memory
// (snapshots; counted inside "textures"), how many the thread prepares and in how long, how many the ring
// keeps for itself at submit time and in how long, and how long the ring waits for the thread. Only the ring
// thread writes it (when planning and when collecting).
inline std::atomic<uint64_t> g_ns_fingerprint_snapshot{0};
inline std::atomic<uint64_t> g_textures_fingerprint_thread{0};
inline std::atomic<uint64_t> g_ns_fingerprint_thread{0};
inline std::atomic<uint64_t> g_textures_fingerprint_ring{0};
inline std::atomic<uint64_t> g_ns_fingerprint_ring{0};
inline std::atomic<uint64_t> g_ns_waiting_fingerprints{0};

inline void Add(Type type, uint64_t ns) {
  g_ns[type].fetch_add(ns, std::memory_order_relaxed);
  g_times[type].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nfsc::waits
