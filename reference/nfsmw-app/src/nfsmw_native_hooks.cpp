// nfsmw - native renderer, step C5b (see nfsmw_native_hooks.h).

#include "nfsmw_native_hooks.h"

#include "nfsmw_native_shaders.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <unordered_map>
#include <array>  // nfsmw_d3d_game_vegetation
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_native_shadow_d3d, false, "NFSMW",
                    "Native renderer (24/09, phase 1 of the Direct3D-level renderer): in 1 of every 64 draws it "
                    "snapshots the device's register mirror and the ring compares it with what it reads from the "
                    "packets ('D3D shadow' line). Does not change what is drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Phase 2b of the Direct3D-level renderer: see g_draw_in_progress and NoteDraw below.
REXCVAR_DEFINE_BOOL(nfsmw_d3d_marker_registration, true, "NFSMW",
                    "Native renderer (25/09, phase 2b of the Direct3D-level renderer): the FlushState marker of "
                    "each Draw* carries its registration (VS, PS, arguments) and the ring uses it without the "
                    "queue or the MatchDraw lookup. It starts by checking against the lookup and turns itself off "
                    "at the first disagreement. false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Shadow map vegetation filtered on the game thread (see DecideVegetation).
REXCVAR_DEFINE_BOOL(nfsmw_d3d_game_vegetation, true, "NFSMW",
                    "Native renderer (25/09, build 184): the DrawVertices and DrawIndexedVertices the ring would "
                    "drop as shadow-map vegetation (colorless, with alpha test or discard) are skipped entirely on "
                    "the game thread: no FlushState, DRAW_INDX or registration. It starts by watching (the ring "
                    "checks the verdict of each draw) and turns itself off at the first disagreement. false = as "
                    "before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The settings of the ring's vegetation discard (nfsmw_native_draws.cpp): the game side checks the
// same ones.
REXCVAR_DECLARE(bool, nfsmw_native_alpha_only_ps);
REXCVAR_DECLARE(int32_t, nfsmw_native_alpha_only_ps_toggle_s);
REXCVAR_DECLARE(bool, nfsmw_shadows_without_vegetation);

namespace nfsmw::native {
namespace {

// Where SetVertexShader (sub_8259C2A8) and SetPixelShader (sub_8259BDC0) store the bound shader inside
// the device object (from the recompiled code).
constexpr uint32_t kDeviceVs = 0x4FE8;
constexpr uint32_t kDevicePs = 0x3290;

// One producer (the thread using D3D) and one consumer (the ring thread).
constexpr uint32_t kSizeQueue = uint32_t(1) << 16;

std::atomic<ShadersNative*> g_shaders{nullptr};

std::mutex g_mutex;
std::unordered_map<uint32_t, const EntryShader*> g_objects;
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_created_vs{0};
std::atomic<uint64_t> g_known_vs{0};
std::atomic<uint64_t> g_created_ps{0};
std::atomic<uint64_t> g_known_ps{0};
std::atomic<uint32_t> g_warnings{0};

RegisterDraw g_queue[kSizeQueue];
std::atomic<uint32_t> g_write{0};
std::atomic<uint32_t> g_read{0};
std::atomic<uint64_t> g_lost{0};

uint32_t ReadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Phase 1 of the Direct3D-level renderer (see SnapshotMirror in the header).
constexpr uint32_t kPhotoEvery = 64;            // 1 in 64 draws
constexpr uint32_t kPhotos = 256;              // snapshots in flight between the game thread and the ring thread
constexpr uint32_t kDeviceFetch = 0x480;
constexpr uint32_t kDeviceConstantsVs = 0x780;
constexpr uint32_t kDeviceConstantsPs = 0x1780;

std::atomic<uint64_t> g_mask_group[kGroupsMirror];
std::atomic<uint32_t> g_offset_group[kGroupsMirror];  // 0 = aun no se ha seen

struct Photo {
  std::atomic<uint64_t> sequence{0};  // 0 while being written
  SnapshotMirror data;
};
Photo g_photos[kPhotos];
uint64_t g_next_photo = 1;  // game thread only
uint32_t g_counter_photos = 0;

int GroupOf(uint32_t register_base) {
  if (register_base >= 0x2000 && register_base < 0x2400 && (register_base & 0x7F) == 0) {
    return int((register_base - 0x2000) >> 7);
  }
  return register_base == 0x4900 ? 8 : -1;
}

/*
 * Phase 2b: the draw record inside the marker.
 * Each Draw* stores its record before calling the original, and its FlushState runs inside the
 * original. With 2b the record does not go straight to the queue: it stays in g_draw_in_progress, that
 * FlushState takes it (TakeDrawInProgress) and puts it in its marker, and the DRAW_INDX that follows in
 * the ring uses it without a lookup if it accepts it. If the marker cannot carry it (no room, phase 2 off)
 * or nobody takes it, it goes to the queue as usual (DeliverDraw, FinishDraw).
 * Self-checking guard: in the observing phase the record goes both ways (queue and marker in check
 * mode); the draw uses the usual lookup and the ring compares the shaders from the lookup with those from
 * the marker's record (CompareDrawMarker). After kChecksDraw matches, marker only; even so
 * 1 in kCheckDrawEvery is still checked. At the first difference it is off for the whole session
 * ("[d3d_marker] reg_entry de draw: DIFFERENCE" in the log) and records go back to the queue.
 */
constexpr uint64_t kChecksDraw = 20000;
constexpr uint64_t kCheckDrawEvery = 1024;  // potencia de 2
RegisterDraw g_draw_in_progress;  // only the thread using D3D (the Draw* calls and their FlushState)
bool g_draw_pending = false;
uint64_t g_turn_draw = 0;
std::atomic<bool> g_draw_off{false};
std::atomic<bool> g_draw_applying{false};
std::atomic<uint64_t> g_draw_equal{0};  // written only by the ring thread
std::atomic<bool> g_draw_different{false};
// The first disagreement, written by the ring thread before g_draw_different (release).
uint32_t g_draw_that = 0;
RegisterDraw g_draw_lookup;
RegisterDraw g_draw_marker;
bool g_draw_there_is_lookup = false;
bool g_draw_there_is_marker = false;
// Report every 10 s (only the thread using D3D).
uint64_t g_i_draws = 0;
uint64_t g_i_in_marker = 0;
uint64_t g_i_checking = 0;
uint64_t g_i_by_queue = 0;
uint64_t g_i_sin_flushstate = 0;
int64_t g_i_next_ms = 0;

bool DrawInMarker() {
  static const bool active = REXCVAR_GET(nfsmw_d3d_marker_registration);
  return active && !g_draw_off.load(std::memory_order_relaxed);
}

// The usual path: to the ring's queue (if it is full, the record is dropped and counted, as before).
void EnqueueDraw(const RegisterDraw& reg_entry) {
  const uint32_t write = g_write.load(std::memory_order_relaxed);
  const uint32_t next = (write + 1) & (kSizeQueue - 1);
  if (next == g_read.load(std::memory_order_acquire)) {
    g_lost.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_queue[write] = reg_entry;
  g_write.store(next, std::memory_order_release);
}

void TurnOffDraw() {
  if (g_draw_off.load(std::memory_order_relaxed)) {
    return;
  }
  g_draw_off.store(true, std::memory_order_relaxed);
  static constexpr const char* kQue[] = {
      "?", "the registrations give different shaders", "?",
      "the lookup finds no registration and the ring identity gives other shaders"};
  const RegisterDraw& b = g_draw_lookup;
  const RegisterDraw& m = g_draw_marker;
  REXLOG_ERROR("[d3d_marker] draw registration: DIFFERENCE with the ring lookup ({}): lookup {} (function {} type "
               "{} VS {:08X} PS {:08X}), marker {} (function {} type {} VS {:08X} PS {:08X}). Phase 2b OFF for the "
               "rest of the session: registrations go back to the queue",
               kQue[g_draw_that < 4 ? g_draw_that : 0], g_draw_there_is_lookup ? "with registration" : "without "
                                                                                                  "registration",
               int(b.function), b.args[0], b.vs, b.ps, g_draw_there_is_marker ? "with registration" : "without "
                                                                                                 "registration",
               int(m.function), m.args[0], m.vs, m.ps);
}

void ReportDraw() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now < g_i_next_ms) {
    return;
  }
  const bool first = g_i_next_ms == 0;
  g_i_next_ms = now + 10000;
  if (!first) {
    NFSMW_REPORT_DEFERRED("[d3d_marker] reg_entry de draw (phase 2b), last 10 s: {} en el marker ({} checking), {} "
                "por la queue, {} sin FlushState | phase {} | checked en el ring {} de {}",
                g_i_in_marker, g_i_checking, g_i_by_queue, g_i_sin_flushstate,
                g_draw_off.load(std::memory_order_relaxed)     ? "OFF"
                : g_draw_applying.load(std::memory_order_relaxed) ? "applying"
                                                                     : "watching",
                g_draw_equal.load(std::memory_order_relaxed), kChecksDraw);
  }
  g_i_in_marker = g_i_checking = g_i_by_queue = g_i_sin_flushstate = 0;
}

}  // namespace


namespace hooks_detail {

const EntryShader* IdentifyCreation(const uint8_t* base, uint32_t address, bool vertices) {
  ShadersNative* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !address || address > UINT32_MAX - 24) {
    return nullptr;
  }
  const uint8_t* p = base + address;
  if (ReadBE(p) != (vertices ? 0x102A0E01u : 0x102A0E00u)) {
    return nullptr;
  }
  const uint64_t total = uint64_t(ReadBE(p + 4)) + ReadBE(p + 8);
  if (total < 24 || total > 65536 || uint64_t(address) + total > (uint64_t(1) << 32)) {
    return nullptr;
  }
  return shaders->IdentifyContainer(std::span<const uint8_t>(p, size_t(total)));
}

void RememberCreation(uint32_t object, const EntryShader* entry, bool vertices) {
  if (!g_shaders.load(std::memory_order_acquire)) {
    return;
  }
  (vertices ? g_created_vs : g_created_ps).fetch_add(1, std::memory_order_relaxed);
  if (entry) {
    (vertices ? g_known_vs : g_known_ps).fetch_add(1, std::memory_order_relaxed);
  } else if (g_warnings.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[native] C5b: {} shader created at {:08X} that is not in the library",
                vertices ? "vertex" : "pixel", object);
  }
  if (!object) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  // The address of a freed object gets reused: an unknown shader clears any earlier association with it.
  if (entry) {
    g_objects.insert_or_assign(object, entry);
  } else {
    g_objects.erase(object);
  }
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace hooks_detail

// see nfsmw_native_hooks.h.
const ShadersNative* LibraryActive() {
  return g_shaders.load(std::memory_order_acquire);
}

void ActivateHooks(ShadersNative* shaders) {
  g_shaders.store(shaders, std::memory_order_release);
  if (!shaders) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_objects.clear();
    g_generation.fetch_add(1, std::memory_order_acq_rel);
  }
}

void NoteDraw(FunctionDraw function, const uint8_t* base, uint32_t vulkan_device, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation) {
  if (!vulkan_device || !g_shaders.load(std::memory_order_relaxed)) {
    return;
  }
  RegisterDraw reg_entry;
  reg_entry.vs = ReadBE(base + vulkan_device + kDeviceVs);
  reg_entry.ps = ReadBE(base + vulkan_device + kDevicePs);
  reg_entry.args[0] = r4;
  reg_entry.args[1] = r5;
  reg_entry.args[2] = r6;
  reg_entry.args[3] = r7;
  reg_entry.function = function;
  reg_entry.shadow = 0;
  reg_entry.vegetation = vegetation;  // nfsmw_d3d_game_vegetation
  static const bool shadow = REXCVAR_GET(nfsmw_native_shadow_d3d);
  if (shadow && ++g_counter_photos % kPhotoEvery == 0) {
    const uint64_t sequence = g_next_photo++;
    Photo& photo = g_photos[sequence % kPhotos];
    photo.sequence.store(0, std::memory_order_release);  // being written
    SnapshotMirror& d = photo.data;
    const uint8_t* disp = base + vulkan_device;
    for (uint32_t g = 0; g < kGroupsMirror; ++g) {
      const uint32_t desp = g_offset_group[g].load(std::memory_order_relaxed);
      d.mask[g] = desp ? g_mask_group[g].load(std::memory_order_relaxed) : 0;
      d.base_register[g] = g < 8 ? 0x2000 + g * 0x80 : 0x4900;
      if (!desp) {
        continue;
      }
      for (uint32_t i = 0; i < 64; ++i) {
        if ((d.mask[g] >> (63 - i)) & 1) {
          d.state[g][i] = ReadBE(disp + desp + i * 4);
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      d.fetch[i] = ReadBE(disp + kDeviceFetch + i * 4);
    }
    for (uint32_t i = 0; i < 1024; ++i) {
      d.constants[i] = ReadBE(disp + kDeviceConstantsVs + i * 4);
      d.constants[1024 + i] = ReadBE(disp + kDeviceConstantsPs + i * 4);
    }
    photo.sequence.store(sequence, std::memory_order_release);
    reg_entry.shadow = sequence;
  }
  // Phase 2b: with the record in the marker, this Draw*'s FlushState decides (TakeDrawInProgress);
  // otherwise, to the queue as usual.
  if (DrawInMarker()) {
    if (g_draw_pending) {  // should not happen: the previous Draw* reached neither its FlushState nor FinishDraw
      EnqueueDraw(g_draw_in_progress);
      ++g_i_sin_flushstate;
    }
    g_draw_in_progress = reg_entry;
    g_draw_pending = true;
    if ((++g_i_draws & 4095) == 0) {
      ReportDraw();
    }
    return;
  }
  EnqueueDraw(reg_entry);
}

bool TakeDrawInProgress(RegisterDraw& reg_entry) {
  if (!g_draw_pending) {
    return false;
  }
  g_draw_pending = false;
  reg_entry = g_draw_in_progress;
  return true;
}

uint32_t DecideModeDraw() {
  if (!DrawInMarker()) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_draw_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffDraw();
    return 0;
  }
  if (!g_draw_applying.load(std::memory_order_relaxed)) {
    const uint64_t equal = g_draw_equal.load(std::memory_order_relaxed);
    if (equal < kChecksDraw) {
      return kDrawCheck;
    }
    g_draw_applying.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marker] draw registration: {} draws checked against the ring lookup, 0 disagreements: the "
                "marker already carries the registration and the ring does not look it up (1 in {} is still "
                "checked)",
                equal, kCheckDrawEvery);
  }
  return (++g_turn_draw & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
}

void DeliverDraw(const RegisterDraw& reg_entry, uint32_t mode, bool in_marker) {
  if (!in_marker) {
    ++g_i_by_queue;
    EnqueueDraw(reg_entry);
    return;
  }
  ++g_i_in_marker;
  if (mode == kDrawCheck) {  // the ring's lookup has to be able to find it to compare
    ++g_i_checking;
    EnqueueDraw(reg_entry);
  }
}

void FinishDraw() {
  if (!g_draw_pending) {
    return;
  }
  g_draw_pending = false;
  ++g_i_sin_flushstate;
  EnqueueDraw(g_draw_in_progress);
}

void NoteCheckDraw(bool equal, uint32_t que, const RegisterDraw* lookup,
                              const RegisterDraw* marker) {
  if (equal) {  // written only by the ring thread
    g_draw_equal.store(g_draw_equal.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }
  if (g_draw_different.load(std::memory_order_relaxed)) {
    return;
  }
  g_draw_that = que;
  g_draw_there_is_lookup = lookup != nullptr;
  if (lookup) {
    g_draw_lookup = *lookup;
  }
  g_draw_there_is_marker = marker != nullptr;
  if (marker) {
    g_draw_marker = *marker;
  }
  g_draw_different.store(true, std::memory_order_release);
}

void LearnGroupMirror(uint32_t register_base, uint64_t mask, uint32_t displacement) {
  const int g = GroupOf(register_base);
  if (g < 0 || !displacement) {
    return;
  }
  // The dump copies from source + 4 * bit position: the mirror of register base_register + i is at
  // displacement + 4 * i. Always the same per group; the first one seen is stored.
  uint32_t expected = 0;
  g_offset_group[g].compare_exchange_strong(expected, displacement, std::memory_order_relaxed);
  const uint64_t before = g_mask_group[g].load(std::memory_order_relaxed);
  if ((before | mask) != before) {
    g_mask_group[g].store(before | mask, std::memory_order_relaxed);
  }
}

bool ReadSnapshot(uint64_t sequence, SnapshotMirror& output) {
  const Photo& photo = g_photos[sequence % kPhotos];
  if (photo.sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = photo.data;
  std::atomic_thread_fence(std::memory_order_acquire);
  return photo.sequence.load(std::memory_order_relaxed) == sequence;  // not rewritten while being copied
}

bool TakeDraw(RegisterDraw& reg_entry) {
  const uint32_t read = g_read.load(std::memory_order_relaxed);
  if (read == g_write.load(std::memory_order_acquire)) {
    return false;
  }
  reg_entry = g_queue[read];
  g_read.store((read + 1) & (kSizeQueue - 1), std::memory_order_release);
  return true;
}

const EntryShader* ShaderOfObject(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_objects.find(object);
  return it != g_objects.end() ? it->second : nullptr;
}

uint64_t GenerationObjects() {
  return g_generation.load(std::memory_order_acquire);
}

/*
 * Shadow map vegetation, filtered on the game thread (nfsmw_d3d_game_vegetation).
 *
 * The ring drops, right on entry to Draw, the colorless draws with an alpha test, a kill or PS-written
 * depth: the shadow map vegetation, ~480 per frame in a race (267-760). But each one still costs ~2.2-2.6 us
 * of ring time outside Draw (its FlushState marker, the DRAW_INDX, the pairing and its share of IM_LOAD)
 * plus its whole Draw* on the game thread (FlushState with streams, shaders and IM_LOAD, and the EDRAM mode
 * changes 5 and 4 with their packets). Here the same decision is taken in the D3D Draw*, before the
 * original, using the device's register mirror, and if it is vegetation the original is not called: the
 * Draw* never exists for the ring.
 *
 * Why the whole Draw* can be removed (from reading the PowerPC code)
 *   - DrawVertices (82593A10) and DrawIndexedVertices (82593C50), before FlushState, only touch
 *     VGT_INDX_OFFSET in the mirror (dev+0x2D14, with its dirty bit 1<<61 in dev+0x28): the same is done
 *     here. After FlushState they only write their DRAW_INDX (normal path: bit 0x04 of dev+0x28C0 clear and a
 *     single-segment count).
 *   - What the skipped FlushState would have dumped stays marked dirty and the next one dumps it (or the
 *     partial dump 825A42E0 of Resolve and ClearF) with the mirror's values, which are the same: the ring's
 *     state at the next draw is the usual one. The same goes for the shaders: their dirty bits stay set and
 *     the next FlushState patches and loads them for whatever shader pair is bound then.
 *   - What gets written to the ring without a dump first: the occlusion query (8258F810 and 8258EA28)
 *     writes RB_MODECONTROL, RB_COLOR_INFO and RB_SAMPLE_COUNT_ADDR directly and leaves them dirty to be
 *     restored. In the ring the only lazy register read without a dump before it is RB_SURFACE_INFO
 *     (ScaleOcclusion). That is why nothing is skipped with group 0x2000 dirty (render targets pending a
 *     dump) or with an occlusion query open.
 *   - Predicated tiling and ZPass: inside a D3D block (dev+0x28C0 & 0x3F: 0x10 tiling, 0x04 extension path
 *     with SET_BIN_MASK and EVENT_WRITE_EXT, 0x01, 0x02 and 0x20 ZPass, 0x08 the one from 825AB9A0) nothing
 *     is skipped: there the packets are recorded to be replayed per tile.
 *
 * The criterion: Draw's up to its early discard, with the state the ring will see for that draw
 *   - PS and VS bound (dev+0x3290 and dev+0x4FE8) and present in the library (ShaderOfObject: the identity
 *     the ring takes from the record).
 *   - Effective EDRAM mode 4. It is the mirror's (RB_MODECONTROL, dev+0x2D94) except for one change
 *     FlushState makes before dumping it: with a PS bound and its dirty bit 0x100 in dev+0x30, the shader
 *     loader (825A3AF0) calls 825A3A58, which turns mode 5 into 4. The switch to 5 (825A3968) only happens
 *     without a PS, and the streams (825A2D80) only set bit 0x80. No other function in the FlushState tree
 *     writes RB_MODECONTROL, RB_COLOR_MASK, RB_COLORCONTROL or the PS.
 *   - No color: no target i with RB_COLOR_MASK (dev+0x2D1C) nonzero and the PS writing it.
 *   - Alpha test (RB_COLORCONTROL, dev+0x2D7C: enabled and a function other than ALWAYS), the PS's own
 *     kill, or depth written by the PS.
 *   - And to skip it: nfsmw_native_alpha_only_ps without alternation, nfsmw_shadows_without_vegetation, no
 *     occlusion query open, outside blocks, no render targets pending a dump, and a single segment
 *     (count 1..65535).
 *
 * Self-checking guard, in two phases
 *   1. Observing: nothing is skipped. Each Draw* carries its verdict in the record and the ring computes
 *      its own in Draw, with its own registers and PS, and counts game yes/no against ring yes/no.
 *      After kChecks checked draws the game would skip, with no disagreement, it moves to applying.
 *   2. Applying: they are skipped. 1 in kSampleEvery is still sent, flagged kVegSample, for the ring to
 *      check, and the ones not skipped still carry their negative verdict, which the ring checks on all of
 *      them.
 *   The filter is switched off for the rest of the session, with DIFFERENCE in the log, on: a differing
 *   verdict in either direction, an occlusion query open in the ring that the game does not see, a draw the
 *   game sees as vegetation that the ring does not draw with its record's shaders, or a ring verdict that
 *   differs from Draw's early discard.
 *
 * Memory ordering
 *   The ring writes the disagreement details and then g_different with release. The game reads g_different
 *   relaxed on every Draw* and, if it is true, issues an acquire fence before reading the details: the fence
 *   synchronizes with the release and the details are seen in full (the same pairing as phase 2b in
 *   DecideModeDraw). Nobody waits or sleeps on this flag: the game only polls it, so no wake-up can be
 *   lost and nothing can hang (the earlier hangs were a thread asleep waiting for a wake-up). The worst case
 *   is seeing it a few microseconds late. The ring's counters have a single writer (the ring) and the game
 *   reads them relaxed: they only count. The verdict travels from the game to the ring inside the draw
 *   record, over the same already synchronized path as phase 2b (marker) or the queue (release and acquire
 *   on its indices).
 */
namespace {
namespace vegetation {

constexpr uint64_t kChecks = 200000;  // observing phase: draws the game would skip, checked in the ring
constexpr uint64_t kSampleEvery = 4096;       // applying phase: 1 in kSampleEvery is sent anyway (power of 2)
// The device's register mirror (FlushState groups: 0x2100 from +0x2D0C, 0x2200 from +0x2D74).
constexpr uint32_t kModeEdram = 0x2D94;     // RB_MODECONTROL (0x2208)
constexpr uint32_t kMaskColor = 0x2D1C;  // RB_COLOR_MASK (0x2104)
constexpr uint32_t kControlColor = 0x2D7C;  // RB_COLORCONTROL (0x2202)
constexpr uint32_t kIndexBase = 0x2D14;    // VGT_INDX_OFFSET (0x2102)
// Its dirty masks (64-bit big-endian): +0x20 fetch, booleans and group 0x2000 (bits 0x3FFFC000 of the low
// word); +0x28 groups 0x2100 (bit 63 = 0x2100), 0x2180, 0x2200 and 0x2280; +0x30 streams (0x400), shaders
// (0x1E0; 0x100 is the PS) and groups 0x2300 and 0x2380.
constexpr uint32_t kDirty20 = 0x20;
constexpr uint32_t kDirty28 = 0x28;
constexpr uint32_t kDirty30 = 0x30;
constexpr uint32_t kBlocks = 0x28C0;      // D3D block byte (BeginTiling 825992F0, ZPass 825999D8...)
constexpr uint8_t kBlocksMask = 0x3F;  // 0x40 and 0x80 are set at device creation: not blocks
constexpr uint32_t kTypeOcclusion = 9;      // D3DQUERYTYPE_OCCLUSION, en query+4 (8258F810)
constexpr uint32_t kMaxQueries = 16;

enum What : uint32_t { kThatNothing, kThatGameYes, kThatRingYes, kThatOcclusion, kThatWithoutIdentity, kThatModel, kWhats };

// ---- Game thread (the one using D3D: the Draw* calls and the queries' Issue) ----
int8_t g_active = -1;  // nfsmw_d3d_game_vegetation, read the first time (without a local static guard)
bool g_applying = false;
bool g_off = false;
bool g_settings = false;  // refreshed every 1024 Draw* calls checked
uint64_t g_turn = 0;
uint64_t g_looked_total = 0;
struct Memo {
  uint32_t object = 0;
  const EntryShader* entry = nullptr;
};
std::array<Memo, 32> g_memo{};
uint64_t g_memo_generation = UINT64_MAX;
// Report every 10 s.
int64_t g_i_next_ms = 0;
uint64_t g_i_looked = 0;
uint64_t g_i_si = 0;
uint64_t g_i_skipped = 0;
uint64_t g_i_samples = 0;
uint64_t g_i_block = 0;
uint64_t g_i_targets = 0;
uint64_t g_i_occlusion = 0;
uint64_t g_i_settings = 0;
uint64_t g_i_count = 0;
std::array<uint32_t, 64> g_i_blocks{};  // the block ones, by value of dev+0x28C0 & 0x3F

// ---- Open D3D occlusion queries (Issue BEGIN without its END), per query object ----
// The D3D thread touches them; the lock only prevents a race should they ever be called from another
// thread.
std::mutex g_queries_lock;
std::array<uint32_t, kMaxQueries> g_queries{};
uint32_t g_n_queries = 0;
std::atomic<uint32_t> g_queries_open{0};  // UINT32_MAX: count lost, nothing is skipped any more

// ---- Shared with the ring thread ----
// Counters: only the ring writes them, without atomic read-modify-write (the A57 has no LSE). The game
// reads them relaxed for the report and to decide when to move to applying.
enum Counter : uint32_t {
  kSiSi,
  kNoNo,
  kSiNo,
  kNoSi,
  kNoSiBlock,
  kNoSiTargets,
  kNoSiOcclusion,
  kNoSiSettings,
  kNoSiCount,
  kSettingsDifferent,
  kWithoutCompare,
  kAgreements,
  kSamplesOk,
  kCounters
};
// Own cache line: the ring writes them on every draw and they must not share a line with anything the
// game thread reads on every Draw* (g_different), or every write would steal it from the other core.
alignas(64) std::atomic<uint64_t> g_counters[kCounters];
alignas(64) std::array<uint64_t, kCounters> g_previous{};  // own cache line; the previous report's values (game thread only)
// The first disagreement: the ring writes the details and then g_different with release (see Memory
// ordering). Own cache line (the game reads it on every Draw*; see g_counters).
alignas(64) std::atomic<bool> g_different{false};
uint32_t g_what = kThatNothing;
uint16_t g_flags = 0;
DetailVegetation g_detail;

uint64_t ReadBE64(const uint8_t* p) {
  return (uint64_t(ReadBE(p)) << 32) | ReadBE(p + 4);
}

void WriteBE(uint8_t* p, uint32_t input_value) {
  p[0] = uint8_t(input_value >> 24);
  p[1] = uint8_t(input_value >> 16);
  p[2] = uint8_t(input_value >> 8);
  p[3] = uint8_t(input_value);
}

const char* YesNo(bool input_value) {
  return input_value ? "yes" : "no";
}

inline void Count(Counter c) {  // ring thread only
  g_counters[c].store(g_counters[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Ring thread only: the details before the flag (release); the game reads them after its acquire fence.
void Disagreement(What que, uint16_t flags, const DetailVegetation& ring) {
  if (g_different.load(std::memory_order_relaxed)) {
    return;
  }
  g_what = que;
  g_flags = flags;
  g_detail = ring;
  g_different.store(true, std::memory_order_release);
}

// The object of a bound shader, with the same identity the ring takes from the record (ShaderOfObject).
// The generation is read relaxed: a shader created on another thread only reaches a Draw* on this thread
// through some synchronization in the game, and then this read already sees the new generation
// (write-read coherence).
const EntryShader* Entry(uint32_t object) {
  const uint64_t generation = g_generation.load(std::memory_order_relaxed);
  if (generation != g_memo_generation) {
    g_memo.fill(Memo{});
    g_memo_generation = generation;
  }
  Memo& memo = g_memo[size_t((object * UINT32_C(2654435761)) >> 27)];
  if (memo.object != object) {
    memo.object = object;
    memo.entry = ShaderOfObject(object);
  }
  return memo.entry;
}

// Draw's criterion up to its early discard (nfsmw_native_draws.cpp), with the state the ring will
// see.
bool Structure(const uint8_t* d, uint32_t& reason) {
  const uint32_t object_ps = ReadBE(d + kDevicePs);
  const uint32_t object_vs = ReadBE(d + kDeviceVs);
  if (!object_ps || !object_vs) {
    reason = kVegReasonWithoutShaders;
    return false;
  }
  // The EDRAM mode the ring will see: the mirror's, with the switch from 5 to 4 FlushState makes before
  // dumping it if a PS is bound and its dirty bit is set (825A3AF0 and 825A3A58). Without a PS it already
  // returned above.
  uint32_t mode = ReadBE(d + kModeEdram) & 0x7;
  if (mode == 5 && (ReadBE64(d + kDirty30) & 0x100)) {
    mode = 4;
  }
  if (mode != 4) {
    reason = kVegReasonMode;
    return false;
  }
  const EntryShader* ps = Entry(object_ps);
  const EntryShader* vs = Entry(object_vs);
  if (!ps || !vs) {
    reason = kVegReasonUnknown;
    return false;
  }
  const uint32_t mask = ReadBE(d + kMaskColor);
  for (uint32_t i = 0; i < 4; ++i) {
    if (((mask >> (i * 4)) & 0xF) && ((ps->outputs >> i) & 0x1)) {
      reason = kVegReasonColor;
      return false;
    }
  }
  const uint32_t control = ReadBE(d + kControlColor);
  const bool test_alpha = ((control >> 3) & 0x1) && (control & 0x7) != 7;
  if (!test_alpha && !ps->discards && !(ps->outputs & 0x10)) {
    reason = kVegReasonWithoutDiscard;
    return false;
  }
  reason = 0;
  return true;
}

void RefreshSettingsVegetation() {
  g_settings = REXCVAR_GET(nfsmw_native_alpha_only_ps) && REXCVAR_GET(nfsmw_native_alpha_only_ps_toggle_s) <= 0 &&
              REXCVAR_GET(nfsmw_shadows_without_vegetation);
}

// Game thread only, after the acquire fence.
void TurnOffVegetation() {
  if (g_off) {
    return;
  }
  g_off = true;
  static constexpr const char* kThatText[kWhats] = {
      "?",
      "the game sees vegetation and the ring does not",
      "the ring sees vegetation and the game does not",
      "occlusion query open on the ring and not in the game",
      "the game sees vegetation and the ring does not draw with the shaders of its registration",
      "the ring's verdict is not the early discard of Draw"};
  const DetailVegetation& a = g_detail;
  const uint16_t b = g_flags;
  REXLOG_ERROR("[vegetation] DIFFERENCE with the ring ({}): game {} (reason {}, settings {}, occlusion {}, block "
               "{}, targets {}, would skip {}, sample {}; phase {}); ring: structure {}, settings {}, occlusion "
               "{}, early discard in Draw {}, RB_MODECONTROL {:08X}, RB_COLOR_MASK {:08X}, RB_COLORCONTROL {:08X}, "
               "PS n{} (outputs {:X}, discards {}), VS n{}. Vegetation filter in the game OFF for the rest of the "
               "session: every Draw* goes back to the ring",
               kThatText[g_what < kWhats ? g_what : 0], YesNo((b & kVegYes) != 0), (b >> kVegReason) & 0xF,
               YesNo((b & kVegSettings) != 0), YesNo((b & kVegOcclusion) != 0), YesNo((b & kVegBlock) != 0),
               YesNo((b & kVegTargets) != 0), YesNo((b & kVegWouldSkip) != 0), YesNo((b & kVegSample) != 0),
               g_applying ? "applying" : "watching", YesNo(a.structure), YesNo(a.settings), YesNo(a.occlusion),
               YesNo(a.early), a.mode, a.mask, a.control, a.ps, a.outputs, YesNo(a.discards), a.vs);
}

// Every 10 s, from the game thread (NFSMW_REPORT_DEFERRED). The ring's counters, as differences.
void ReportVegetation() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now < g_i_next_ms) {
    return;
  }
  const bool first = g_i_next_ms == 0;
  g_i_next_ms = now + 10000;
  std::array<uint64_t, kCounters> ring{};
  for (uint32_t i = 0; i < kCounters; ++i) {
    ring[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (first) {
    REXLOG_INFO("[vegetation] shadow-map vegetation filtered in the game (build 184): starts by watching; it skips "
                "once the ring has checked {} draws it would skip without any disagreement",
                kChecks);
  } else {
    std::array<uint64_t, kCounters> d{};
    for (uint32_t i = 0; i < kCounters; ++i) {
      d[i] = ring[i] - g_previous[i];
    }
    std::string blocks;
    for (uint32_t v = 1; v < 64; ++v) {
      if (g_i_blocks[v]) {
        blocks += fmt::format(" {:02X}:{}", v, g_i_blocks[v]);
      }
    }
    if (!blocks.empty()) {
      blocks = " (values" + blocks + ")";
    }
    const bool lost = g_queries_open.load(std::memory_order_relaxed) == UINT32_MAX;
    NFSMW_REPORT_DEFERRED(
        "[vegetation] Draw* (build 184), last 10 s: {} looked at, {} vegetation for the game: {} skipped and {} "
        "sent for checking; {} are not skipped in D3D blocks{}, {} with targets to dump, {} with an occlusion "
        "query open, {} with the settings off or toggling, {} because of the count | ring: yes and yes {}, no and "
        "no {}, game yes and ring no {}, game no and ring yes {} ({} by block, {} by targets, {} by occlusion only "
        "in the game, {} by settings, {} by the count), different settings {}, not compared {}, samples ok {} | "
        "phase {} | checked {} of {}{}",
        g_i_looked, g_i_si, g_i_skipped, g_i_samples, g_i_block, blocks, g_i_targets, g_i_occlusion, g_i_settings,
        g_i_count, d[kSiSi], d[kNoNo], d[kSiNo], d[kNoSi], d[kNoSiBlock], d[kNoSiTargets], d[kNoSiOcclusion],
        d[kNoSiSettings], d[kNoSiCount], d[kSettingsDifferent], d[kWithoutCompare], d[kSamplesOk],
        g_applying ? "applying" : "watching", ring[kAgreements], kChecks,
        lost ? " | occlusion queries: THE COUNT WAS LOST, nothing is skipped" : "");
  }
  g_previous = ring;
  g_i_looked = g_i_si = g_i_skipped = g_i_samples = g_i_block = g_i_targets = g_i_occlusion = g_i_settings =
      g_i_count = 0;
  g_i_blocks.fill(0);
}

}  // namespace vegetation
}  // namespace

uint16_t DecideVegetation(FunctionDraw function, uint8_t* base, uint32_t vulkan_device, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip) {
  using namespace vegetation;
  skip = false;
  if (g_active < 0) {
    g_active = REXCVAR_GET(nfsmw_d3d_game_vegetation) ? 1 : 0;
  }
  if (!g_active || g_off || !vulkan_device || !g_shaders.load(std::memory_order_relaxed) ||
      (function != FunctionDraw::kVertices && function != FunctionDraw::kIndexed)) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffVegetation();
    return 0;
  }
  if ((g_looked_total++ & 1023) == 0) {
    RefreshSettingsVegetation();
    ReportVegetation();
  }
  ++g_i_looked;
  const uint8_t* d = base + vulkan_device;
  uint32_t reason = 0;
  const bool si = Structure(d, reason);
  const uint32_t open = g_queries_open.load(std::memory_order_relaxed);
  const uint8_t blocks = uint8_t(d[kBlocks] & kBlocksMask);
  const bool targets = (uint32_t(ReadBE64(d + kDirty20)) & 0x3FFFC000u) != 0;
  const uint32_t count = function == FunctionDraw::kVertices ? r6 : r7;
  uint16_t flags = uint16_t(kVegThereIs | (reason << kVegReason));
  if (si) {
    flags = uint16_t(flags | kVegYes);
  }
  if (open) {
    flags = uint16_t(flags | kVegOcclusion);
  }
  if (g_settings) {
    flags = uint16_t(flags | kVegSettings);
  }
  if (blocks) {
    flags = uint16_t(flags | kVegBlock);
  }
  if (targets) {
    flags = uint16_t(flags | kVegTargets);
  }
  if (!si) {
    return flags;
  }
  ++g_i_si;
  // Why it would not be skipped, in this order (each draw counts under a single reason).
  if (blocks) {
    ++g_i_block;
    ++g_i_blocks[blocks];
    return flags;
  }
  if (targets) {
    ++g_i_targets;
    return flags;
  }
  if (open) {
    ++g_i_occlusion;
    return flags;
  }
  if (!g_settings) {
    ++g_i_settings;
    return flags;
  }
  if (count == 0 || count > 0xFFFF) {
    ++g_i_count;
    return flags;
  }
  flags = uint16_t(flags | kVegWouldSkip);
  if (!g_applying) {
    const uint64_t checked = g_counters[kAgreements].load(std::memory_order_relaxed);
    if (checked < kChecks) {
      return flags;  // observing phase: sent, and the ring compares it
    }
    g_applying = true;
    REXLOG_INFO("[vegetation] {} vegetation draws the game would skip checked on the ring, 0 disagreements: they "
                "are now skipped in the D3D Draw* (1 in {} is still sent for checking)",
                checked, kSampleEvery);
  }
  if ((++g_turn & (kSampleEvery - 1)) == 0) {
    ++g_i_samples;
    return uint16_t(flags | kVegSample);
  }
  // The whole Draw* is skipped. The only thing it does to the mirror before FlushState: VGT_INDX_OFFSET and
  // its dirty bit (DrawVertices sets it to its start vertex, r5; DrawIndexedVertices to 0). Everything else
  // stays dirty for the next dump, with the same mirror values.
  uint8_t* mirror = base + vulkan_device;
  const uint32_t index = function == FunctionDraw::kVertices ? r5 : 0u;
  if (ReadBE(mirror + kIndexBase) != index) {
    WriteBE(mirror + kIndexBase, index);
    const uint64_t dirty = ReadBE64(mirror + kDirty28) | (uint64_t(1) << 61);
    WriteBE(mirror + kDirty28, uint32_t(dirty >> 32));
    WriteBE(mirror + kDirty28 + 4, uint32_t(dirty));
  }
  ++g_i_skipped;
  skip = true;
  return flags;
}

void NoteQueryD3D(const uint8_t* base, uint32_t query, uint32_t flags) {
  using namespace vegetation;
  if (!query || ReadBE(base + query + 4) != kTypeOcclusion) {
    return;
  }
  const bool begins = (flags & 2) != 0;
  if (!begins && !(flags & 1)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_queries_lock);
  uint32_t i = 0;
  while (i < g_n_queries && g_queries[i] != query) {
    ++i;
  }
  if (begins) {  // BEGIN takes precedence over END, as in the original
    if (i == g_n_queries) {
      if (g_n_queries == kMaxQueries) {
        g_queries_open.store(UINT32_MAX, std::memory_order_relaxed);
        return;
      }
      g_queries[g_n_queries++] = query;
    }
  } else if (i < g_n_queries) {
    g_queries[i] = g_queries[--g_n_queries];
  }
  if (g_queries_open.load(std::memory_order_relaxed) != UINT32_MAX) {
    g_queries_open.store(g_n_queries, std::memory_order_relaxed);
  }
}

uint16_t IdentityForVegetation(uint16_t flags, bool con_sus_shaders) {
  using namespace vegetation;
  if (!(flags & kVegThereIs)) {
    return 0;
  }
  if (con_sus_shaders) {
    return flags;
  }
  Count(kWithoutCompare);
  if (flags & kVegYes) {
    Disagreement(kThatWithoutIdentity, flags, DetailVegetation{});
  }
  return 0;
}

void NoteVegetationRing(uint16_t flags, const DetailVegetation& ring) {
  using namespace vegetation;
  const bool si = (flags & kVegYes) != 0;
  const bool would_skip = (flags & kVegWouldSkip) != 0;
  const bool complete = ring.structure && ring.settings && !ring.occlusion;
  Count(would_skip ? (complete ? kSiSi : kSiNo) : (complete ? kNoSi : kNoNo));
  if (si != ring.structure) {
    Disagreement(si ? kThatGameYes : kThatRingYes, flags, ring);
    return;
  }
  if (would_skip) {
    if (!ring.settings) {
      Count(kSettingsDifferent);  // a setting just changed and each thread reads it at its own time: not an error
    } else if (ring.occlusion) {
      Disagreement(kThatOcclusion, flags, ring);
    } else {
      Count(kAgreements);
      if (flags & kVegSample) {
        Count(kSamplesOk);
      }
    }
    return;
  }
  if (complete) {  // same structure, but the game would not skip it: why
    Count((flags & kVegBlock)       ? kNoSiBlock
           : (flags & kVegTargets)   ? kNoSiTargets
           : (flags & kVegOcclusion)   ? kNoSiOcclusion
           : !(flags & kVegSettings)   ? kNoSiSettings
                                         : kNoSiCount);
  }
}

void NoteVegetationModel(uint16_t flags, const DetailVegetation& ring) {
  vegetation::Disagreement(vegetation::kThatModel, flags, ring);
}

StatisticsHooks StatisticsOfHooks() {
  StatisticsHooks e;
  e.created_vs = g_created_vs.load(std::memory_order_relaxed);
  e.known_vs = g_known_vs.load(std::memory_order_relaxed);
  e.created_ps = g_created_ps.load(std::memory_order_relaxed);
  e.known_ps = g_known_ps.load(std::memory_order_relaxed);
  e.lost_count = g_lost.load(std::memory_order_relaxed);
  return e;
}

}  // namespace nfsmw::native

// The constructors and the fetch patcher report what they write into the microcode (IM_LOAD without
// memcmp, see nfsmw_microcode_versions.h).
#include "nfsmw_microcode_versions.h"

// The experimental library (nfsmw_shader_hooks.cpp) hooks the same constructors; with it enabled these
// are not compiled.
#if !defined(NFSMW_NATIVE_SHADER_LIBRARY)
// r3 = original container; they return the object in r3. The original is always called and no PPC
// register is touched.
REX_EXTERN(__imp__sub_8259BC90);
REX_HOOK_RAW(sub_8259BC90) {  // pixel shader
  const auto* entry =
      nfsmw::native::hooks_detail::IdentifyCreation(base, ctx.r3.u32, false);
  __imp__sub_8259BC90(ctx, base);
  nfsmw::native::hooks_detail::RememberCreation(ctx.r3.u32, entry, false);
  nfsmw::native::microcode::NotifyCreation(base, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_8259C038);
REX_HOOK_RAW(sub_8259C038) {  // vertex shader
  const auto* entry = nfsmw::native::hooks_detail::IdentifyCreation(base, ctx.r3.u32, true);
  __imp__sub_8259C038(ctx, base);
  nfsmw::native::hooks_detail::RememberCreation(ctx.r3.u32, entry, true);
  nfsmw::native::microcode::NotifyCreation(base, ctx.r3.u32, true);
}
#endif

/*
 * The VS fetch patcher reports what it writes (IM_LOAD without memcmp, see nfsmw_microcode_versions.h).
 * sub_825A2FB8(r3 device, r4 VS, r5 destination, r6 declaration) writes the patched fetches to r5.
 *   - In place (r5 = [VS+40], from sub_825A3AF0): bumps the versions of that microcode's slot before and
 *     after writing.
 *   - On the ring's copy (from sub_825A37D8, return 0x825A38D0): not memory of any IM_LOAD; only counted.
 *   - Any other call: also bumps the destination's versions, but is counted separately and the ring turns
 *     off the shortcut.
 * Careful: the two calls in the generated code must go to sub_825A2FB8 and not to __imp__sub_825A2FB8
 * (nfsmw_recomp.58.cpp and nfsmw_recomp.124.cpp, changed by a patch; tools/direct_calls.py already
 * handles them because this address appears here). If the one from sub_825A3AF0 went to __imp__, this
 * hook would not see the in-place patches and the ring's guard would stay in the observing phase (or
 * switch off with DIFFERENCE).
 * It changes no PPC register.
 */
REX_EXTERN(__imp__sub_825A2FB8);
REX_HOOK_RAW(sub_825A2FB8) {
  namespace mc = nfsmw::native::microcode;
  const uint32_t target = ctx.r5.u32;
  const uint32_t vs = ctx.r4.u32;
  const uint32_t return_value = uint32_t(ctx.lr);
  const bool in_its_room = vs != 0 && mc::ReadGuest32(base, vs + 40) == target;
  if (!in_its_room && return_value == 0x825A38D0u) {
    mc::Count(mc::g_patches_in_copy);
    __imp__sub_825A2FB8(ctx, base);
    return;
  }
  const uint32_t slot = mc::SlotOf(mc::Physical(target) & ~uint32_t(3));
  mc::BeginWrite(slot);
  __imp__sub_825A2FB8(ctx, base);
  // The counts, before FinishWrite's global++: a ring that sees the new global value already sees
  // them.
  if (in_its_room) {
    mc::Count(mc::g_patches_in_its_room);
  } else {
    mc::g_other_return.store(return_value, std::memory_order_relaxed);
    mc::g_other_target.store(target, std::memory_order_relaxed);
    mc::Count(mc::g_patches_others);
  }
  mc::FinishWrite(slot);
}
