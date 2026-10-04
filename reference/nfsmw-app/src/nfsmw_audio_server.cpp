// nfsmw - rescue of the game's audio server thread
//
// ===========================================================================
//  THE FAILURE (observed on the console, in the menu with the gamepad)
//  The menu audio goes silent and the race never finishes loading. The
//  watchdog always dumps guest thread 0xD waiting forever in
//  KeWaitForSingleObject (lr 0x82853230) on the audio server's event,
//  with ctr = 0x8285D560: its last indirect call was the SetState of a
//  packet submission that succeeded.
//
//  THE SERVER LOOP (sub_825E3E28, recompiled code)
//  Object in r31 and a ring of 2 packets of 6144 bytes:
//    +146        active              +152        XAudio voice
//    +12444      R: next packet to submit
//    +12448      W: next packet to mix
//    +12452      event (auto-reset)
//    +12456+88*i packet i (its context, at +84, is &state[i])
//    +12632+4*i  state i: 0 free, 1 full, 2 submitted to the voice
//  1. Waits for the event at 0x825E3EF0 with sub_828531E8, with no timeout.
//  2. On wakeup, if slot W is not free, it waits again without attempting
//     any submission.
//  3. If it is free: sub_825CF780 (the game's audio commands), mixes into W
//     (state 1) and submits in order while the voice has a free node (bit 0x20
//     of sub_82851D98): sub_82851F00 and state 2.
//  4. The end of each packet arrives through sub_825E4358 (state 0 and signal)
//     from the SDK's audio thread; each pass of the voice calls sub_825E4338,
//     which only signals.
//  If the voice stops returning end-of-packet notifications, the thread keeps
//  waiting with slot W occupied: sub_825CF780 never runs again and the game's
//  audio commands, including those of the race load, go unserviced.
//
//  WHAT THIS FILE DOES
//  It replaces that wait with waits that have a timeout. While end-of-packet
//  notifications keep arriving (one every ~5 ms) the loop does exactly the
//  same. If the thread has spent 250 ms with slot W occupied and no end of
//  packet or submission:
//   - it retries the pending submissions with the same condition as the game;
//   - if slot W is still occupied, it frees it and returns, so that the loop
//     keeps mixing and servicing commands at ~5 ms per packet, albeit silent;
//   - as soon as a real end of packet arrives, it stops freeing.
//  Entering and leaving that mode is logged with the state of the packet
//  ring and of the voice.
//
//  HOW TO TURN IT OFF WITHOUT A NEW NRO
//  In nfsmw.toml:   nfsmw_audio_rescue = false
//
//  DIAGNOSTICS (off by default)
//  nfsmw_audio_diag_delay_server_us delays each wakeup of the thread, to
//  see on PC what happens if it is slow to run again, as can happen on
//  Horizon. nfsmw_audio_diag_ring logs every 10 s how many packets the
//  voice had on each pass.
//
//  ROBOTIC AUDIO IN CRASHES (measured on the console)
//  In the Ironwood Estates alley the server thread (XThread5F79080 on the
//  console) delivered 89 % of the packets: it ran at 0x3B, taking turns every
//  10 ms with Main and XThread59CC1C0, and 1,308 passes of the voice came out
//  without a packet. Defaults on the Switch:
//   - nfsmw_audio_server_priority = 0x2D: it takes the CPU from them as soon
//     as it wakes up;
//   - nfsmw_audio_wait_server_ms = 30: the Audio Worker waits for the late
//     packet instead of mixing the audio frame with the voice silent, and the
//     cost is absorbed by the audio_switch_frames_in_queue cushion (10 audio
//     frames, 53 ms).
//  On PC both stay at 0: with the whole CPU the packet ring never runs dry.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(nfsmw_audio_rescue, true, "NFSMW",
                    "Keep en marcha el thread_value server de audio del game si la voice de XAudio "
                    "leaves de to_return ends de packet");
REXCVAR_DEFINE_INT32(nfsmw_audio_diag_delay_server_us, 0, "NFSMW",
                     "Diagnostic: retrasa N microseconds every wake del thread_value server de "
                     "audio del game, para imitar en el PC lo que tarda en volver a correr en la "
                     "Switch; 0 = nothing");
REXCVAR_DEFINE_INT32(nfsmw_audio_server_priority, REX_PLATFORM_SWITCH != 0 ? 0x2D : 0, "NFSMW",
                     "Priority de Horizon del thread_value server de audio del game (0x1C-0x3A); 0 = la de los threads "
                     "del game (0x3B). Por default 45 (0x2D) en la Switch, para que no espere su turn behind de "
                     "ellos en los choques; en el PC cualquier input_value lo sube a THREAD_PRIORITY_HIGHEST");
REXCVAR_DEFINE_BOOL(nfsmw_audio_diag_ring, false, "NFSMW",
                    "Diagnostic: every 10 s anota how_many_2 packets tenia la voice del server de "
                    "audio en every pass, los ends de packet y the deliveries, y la CPU y los cores del "
                    "thread_value server");
REXCVAR_DEFINE_INT32(nfsmw_audio_diag_ring_ms, 10000, "NFSMW",
                     "Diagnostic: milisegundos between resumenes de nfsmw_audio_diag_ring (as little 100); "
                     "500 separa los choques del shortcut del callejon");
REXCVAR_DEFINE_DOUBLE(nfsmw_audio_diag_slowness_blend, 0.0, "NFSMW",
                      "Diagnostic: after every blend del thread_value server de audio wait activamente N times lo que "
                      "ha tardado, before de deliver el packet; 3 imita one blend 4 times mas lenta; 0 = nothing");
REXCVAR_DEFINE_INT32(nfsmw_audio_wait_server_ms, REX_PLATFORM_SWITCH != 0 ? 30 : 0, "NFSMW",
                     "Before de every frame, si la voice del server de audio del game no has packet y el "
                     "server esta en marcha, wait as mucho N ms a que lo entregue (en time de mezclar la "
                     "frame con esa voice en silence); 0 = no wait (por default 30 en la Switch y 0 en el PC). "
                     "Necesita colchon en el driver (audio_switch_frames_in_queue en la Switch, "
                     "audio_sdl_pump_queue en el PC)");

namespace nfsmw::threads {
// nfsmw_threads_switch.cpp
bool PriorityThreadCurrent(int priority);
uint64_t HandlerThreadCurrent();
int64_t CpuThreadUs(uint64_t handler);
int CoreCurrent();
}  // namespace nfsmw::threads

namespace nfsmw::audio_server {

int64_t ClockReal() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The test bench replaces it with a simulated clock.
int64_t (*g_clock_ms)() = &ClockReal;

namespace {

constexpr uint32_t kReturnWait = 0x825E3EF4;
constexpr uint32_t kReturnDelivery = 0x825E4068;
constexpr uint32_t kReturnArranque = 0x825E3A3C;
constexpr uint32_t kFlagServer = 0x82A2B2AC;
constexpr uint32_t kOffActive = 146;
constexpr uint32_t kOffVoice = 152;
constexpr uint32_t kOffStateVoice = 61;
constexpr uint32_t kOffR = 12444;
constexpr uint32_t kOffW = 12448;
constexpr uint32_t kOffEvent = 12452;
constexpr uint32_t kOffPackets = 12456;
constexpr uint32_t kBytesPacket = 88;
constexpr uint32_t kOffStates = 12632;
constexpr uint32_t kFree = 0;
constexpr uint32_t kFull = 1;
constexpr uint32_t kDelivered = 2;
constexpr uint8_t kBitNodeFree = 0x20;
constexpr uint32_t kStatusTimeout = 0x102;

constexpr int64_t kDeadlineMs = 50;
constexpr int64_t kDeadlineRescueMs = 5;
constexpr int64_t kWithoutAdvanceMs = 250;
constexpr int64_t kSummaryRescueMs = 10000;
constexpr uint64_t kMaxDumps = 20;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  input_value = __builtin_bswap32(input_value);
  std::memcpy(base + address, &input_value, sizeof(input_value));
}

uint32_t State(const uint8_t* base, uint32_t obj, uint32_t i) {
  return Read32(base, obj + kOffStates + 4 * i);
}

// Written by the hooks of the notifications (SDK audio thread) and of the
// submission; read by the server thread.
std::atomic<uint64_t> g_ends{0};
std::atomic<uint64_t> g_passes{0};
std::atomic<uint64_t> g_deliveries{0};
std::atomic<int64_t> g_last_end_ms{0};
std::atomic<int64_t> g_last_delivery_ms{0};

// nfsmw_audio_diag_ring: from wakeup to submission. g_wake_us is written and read by the server thread
// (its wait and its submission); the summary resets the sums, from the SDK audio thread.
std::atomic<int64_t> g_wake_us{0};
std::atomic<uint64_t> g_blends{0};
std::atomic<uint64_t> g_blend_sum_us{0};
std::atomic<int64_t> g_blend_max_us{0};

// nfsmw_audio_wait_server_ms: the guest memory base is recorded by the server's wait; the submission
// notifies the condition and the Audio Worker waits on it before each audio frame. The counters are reset by
// the packet ring summary.
std::atomic<uint8_t*> g_base_server{nullptr};
std::mutex g_delivery_mutex;
std::condition_variable g_delivery_cv;
std::atomic<uint64_t> g_waits{0};
std::atomic<uint64_t> g_waits_exhausted{0};
std::atomic<uint64_t> g_wait_sum_us{0};
std::atomic<int64_t> g_wait_max_us{0};
// With no submissions within this time the server is considered stopped (loads, videos): no waiting.
constexpr int64_t kServerStoppedMs = 300;

int64_t ClockUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// nfsmw_audio_diag_* diagnostics. The server object is recorded by its wait; the passes are
// recorded by the SDK audio thread, which is the only one that calls them.
std::atomic<uint32_t> g_obj_server{0};
// Handle of the server thread, to read its CPU time from the summary, and its wakeups per core (the core the
// system reports when the wait returns signaled; the last bucket counts any other, such as PC cores above 3).
std::atomic<uint64_t> g_handler_server{0};
std::atomic<uint64_t> g_wakes_core[5];
struct DiagRing {
  uint64_t passes_with[3] = {0, 0, 0};
  int64_t since_ms = 0;
  uint64_t ends_before = 0;
  uint64_t deliveries_before = 0;
  // The longest a pass has seen since the server's last submission: how long it takes to run again.
  int64_t max_without_delivery_ms = 0;
  // CPU time the server thread had used when the summary started (-1 = unknown).
  int64_t cpu_before_us = -1;
};
DiagRing g_diag_ring;

// Used only by the server thread: it is the only one that reaches the hooked wait.
struct Rescue {
  bool active = false;
  int64_t first_wait_ms = -1;
  int64_t since_ms = 0;
  int64_t last_summary_ms = 0;
  uint64_t ends_to_enter = 0;
  uint64_t released = 0;
  uint64_t redeliveries = 0;
  uint64_t times = 0;
};
Rescue g_rescue;

void Dump(const char* que, const uint8_t* base, uint32_t obj, int64_t now) {
  const uint32_t voice = Read32(base, obj + kOffVoice);
  REXLOG_WARN("[audio] {}: R={} W={} states={}/{} active={} voice=0x{:08X} state_voice=0x{:02X} "
              "ends={} passes={} deliveries={} ms_sin_fin={} ms_without_delivery={}",
              que, Read32(base, obj + kOffR), Read32(base, obj + kOffW), State(base, obj, 0),
              State(base, obj, 1), static_cast<unsigned>(base[obj + kOffActive]), voice,
              static_cast<unsigned>(voice ? base[voice + kOffStateVoice] : 0), g_ends.load(),
              g_passes.load(), g_deliveries.load(), now - g_last_end_ms.load(),
              now - g_last_delivery_ms.load());
}

// nfsmw_audio_diag_ring: packets submitted to the voice (state 2) on each pass, with a
// summary every 10 s.
void NotePass(const uint8_t* base) {
  const uint32_t obj = g_obj_server.load(std::memory_order_relaxed);
  if (obj == 0) {
    return;
  }
  DiagRing& d = g_diag_ring;
  const uint32_t delivered = uint32_t(State(base, obj, 0) == kDelivered) +
                              uint32_t(State(base, obj, 1) == kDelivered);
  ++d.passes_with[delivered];
  const int64_t now = g_clock_ms();
  const uint64_t ends = g_ends.load(std::memory_order_relaxed);
  const uint64_t deliveries = g_deliveries.load(std::memory_order_relaxed);
  const int64_t last_delivery = g_last_delivery_ms.load(std::memory_order_relaxed);
  if (last_delivery != 0) {
    d.max_without_delivery_ms = std::max(d.max_without_delivery_ms, now - last_delivery);
  }
  if (d.since_ms == 0) {
    d.since_ms = now;
    d.ends_before = ends;
    d.deliveries_before = deliveries;
    d.cpu_before_us = nfsmw::threads::CpuThreadUs(g_handler_server.load(std::memory_order_relaxed));
    return;
  }
  if (now - d.since_ms >= std::max<int64_t>(100, REXCVAR_GET(nfsmw_audio_diag_ring_ms))) {
    const uint64_t blends = g_blends.exchange(0, std::memory_order_relaxed);
    const uint64_t blend_sum_us = g_blend_sum_us.exchange(0, std::memory_order_relaxed);
    const int64_t blend_max_us = g_blend_max_us.exchange(0, std::memory_order_relaxed);
    const uint64_t waits = g_waits.exchange(0, std::memory_order_relaxed);
    const uint64_t waits_exhausted = g_waits_exhausted.exchange(0, std::memory_order_relaxed);
    const uint64_t wait_sum_us = g_wait_sum_us.exchange(0, std::memory_order_relaxed);
    const int64_t wait_max_us = g_wait_max_us.exchange(0, std::memory_order_relaxed);
    const int64_t cpu_us = nfsmw::threads::CpuThreadUs(g_handler_server.load(std::memory_order_relaxed));
    const int64_t cpu_window_us = cpu_us >= 0 && d.cpu_before_us >= 0 ? cpu_us - d.cpu_before_us : -1;
    const int64_t window_ms = std::max<int64_t>(1, now - d.since_ms);
    uint64_t cores[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < 5; ++i) {
      cores[i] = g_wakes_core[i].exchange(0, std::memory_order_relaxed);
    }
    REXLOG_INFO("[audio] ring del server en {} ms: passes con 0/1/2 packets en la voice "
                "{}/{}/{}, ends de packet {}, deliveries {}, maximum sin delivery {} ms, de wake a "
                "deliver media {} us y maximum {} us ({} times), waits before de la frame {} (exhausted {}), "
                "wait media {} us y maxima {} us, CPU del thread_value server {} ms ({} %), wakes por core "
                "0/1/2/3/others {}/{}/{}/{}/{}",
                now - d.since_ms, d.passes_with[0], d.passes_with[1], d.passes_with[2],
                ends - d.ends_before, deliveries - d.deliveries_before, d.max_without_delivery_ms,
                blends ? blend_sum_us / blends : 0, blend_max_us, blends, waits, waits_exhausted,
                waits ? wait_sum_us / waits : 0, wait_max_us,
                cpu_window_us >= 0 ? cpu_window_us / 1000 : -1,
                cpu_window_us >= 0 ? cpu_window_us / (window_ms * 10) : -1, cores[0], cores[1],
                cores[2], cores[3], cores[4]);
    d = DiagRing{};
    d.since_ms = now;
    d.ends_before = ends;
    d.deliveries_before = deliveries;
    d.cpu_before_us = cpu_us;
  }
}

// nfsmw_audio_diag_delay_server_us: busy-wait, so that the delay is the requested one even
// below a millisecond on Windows.
void DelayWake(int32_t us) {
  const auto fin = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
  while (std::chrono::steady_clock::now() < fin) {
    std::this_thread::yield();
  }
}

}  // namespace
}  // namespace nfsmw::audio_server

namespace nfsmw::audio_server {
namespace {

// SDK hook before each audio frame (rex::audio::SetHookBeforeOfFrame), on the Audio Worker thread. Waits on
// the condition with 1 ms timeouts: the submission notifies before the game marks the packet as submitted, so
// each notification is checked again and a lost notification only costs that millisecond. It never spins: on
// the Switch the Audio Worker has a higher priority than the server itself.
void WaitPacketServer(size_t) {
  const int32_t maximum_ms = REXCVAR_GET(nfsmw_audio_wait_server_ms);
  if (maximum_ms <= 0) {
    return;
  }
  const uint32_t obj = g_obj_server.load(std::memory_order_relaxed);
  const uint8_t* const base = g_base_server.load(std::memory_order_relaxed);
  if (obj == 0 || base == nullptr) {
    return;
  }
  const auto with_packet = [&] {
    return State(base, obj, 0) == kDelivered || State(base, obj, 1) == kDelivered;
  };
  if (with_packet()) {
    return;
  }
  if (base[kFlagServer] == 0 ||
      g_clock_ms() - g_last_delivery_ms.load(std::memory_order_relaxed) > kServerStoppedMs) {
    return;
  }
  const int64_t start_us = ClockUs();
  const int64_t limit_us = start_us + int64_t(maximum_ms) * 1000;
  bool exhausted = false;
  {
    std::unique_lock<std::mutex> lock(g_delivery_mutex);
    while (!with_packet()) {
      const int64_t now_us = ClockUs();
      if (now_us >= limit_us) {
        exhausted = true;
        break;
      }
      g_delivery_cv.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(1000, limit_us - now_us)));
    }
  }
  const int64_t wait_us = ClockUs() - start_us;
  g_waits.fetch_add(1, std::memory_order_relaxed);
  if (exhausted) {
    g_waits_exhausted.fetch_add(1, std::memory_order_relaxed);
  }
  g_wait_sum_us.fetch_add(static_cast<uint64_t>(wait_us), std::memory_order_relaxed);
  int64_t maximum = g_wait_max_us.load(std::memory_order_relaxed);
  while (wait_us > maximum &&
         !g_wait_max_us.compare_exchange_weak(maximum, wait_us, std::memory_order_relaxed)) {
  }
}

}  // namespace
}  // namespace nfsmw::audio_server

REX_EXTERN(__imp__sub_828531E8);
REX_EXTERN(__imp__sub_825E4358);
REX_EXTERN(__imp__sub_825E4338);
REX_EXTERN(__imp__sub_82851F00);
REX_EXTERN(__imp__sub_82851FB8);
REX_EXTERN(sub_82851D98);

namespace nfsmw::audio_server {
namespace {

// Submits the full packets in order while the voice has a free node, with
// the same condition and the same steps as 0x825E4030-0x825E4090.
uint64_t Redeliver(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  uint64_t delivered = 0;
  for (int lap = 0; lap < 2; ++lap) {
    const uint32_t r = Read32(base, obj + kOffR) % 2;
    if (State(base, obj, r) != kFull) {
      break;
    }
    const uint32_t output = ctx.r1.u32 + 80;
    ctx.r3.u64 = Read32(base, obj + kOffVoice);
    ctx.r4.u64 = output;
    sub_82851D98(ctx, base);
    if ((base[output] & kBitNodeFree) == 0) {
      break;
    }
    ctx.r3.u64 = Read32(base, obj + kOffVoice);
    ctx.r4.u64 = obj + kOffPackets + kBytesPacket * r;
    ctx.r5.u64 = 0;
    __imp__sub_82851F00(ctx, base);
    Write32(base, obj + kOffStates + 4 * r, kDelivered);
    Write32(base, obj + kOffR, (r + 1) % 2);
    ++delivered;
  }
  return delivered;
}

}  // namespace
}  // namespace nfsmw::audio_server

REX_HOOK_RAW(sub_825E4358) {
  using namespace nfsmw::audio_server;
  g_ends.fetch_add(1, std::memory_order_relaxed);
  g_last_end_ms.store(g_clock_ms(), std::memory_order_relaxed);
  __imp__sub_825E4358(ctx, base);
}

REX_HOOK_RAW(sub_825E4338) {
  using namespace nfsmw::audio_server;
  g_passes.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(nfsmw_audio_diag_ring)) {
    NotePass(base);
  }
  __imp__sub_825E4338(ctx, base);
}

REX_HOOK_RAW(sub_82851F00) {
  using namespace nfsmw::audio_server;
  const bool of_server = static_cast<uint32_t>(ctx.lr) == kReturnDelivery;
  const bool diag = REXCVAR_GET(nfsmw_audio_diag_ring);
  const double slowness = REXCVAR_GET(nfsmw_audio_diag_slowness_blend);
  // Submission time taken before the call: it counts the mix, not how long XAudio takes to accept the packet.
  // Only the first submission after each wakeup.
  const int64_t delivery_us = of_server && (diag || slowness > 0.0) ? ClockUs() : 0;
  const int64_t wake_us = delivery_us != 0 ? g_wake_us.exchange(0, std::memory_order_relaxed) : 0;
  int64_t blend_us = wake_us != 0 ? delivery_us - wake_us : -1;
  if (blend_us > 0 && slowness > 0.0) {
    // nfsmw_audio_diag_slowness_blend: the mix takes (1 + slowness) times the measured time; the logged value is
    // the simulated one.
    DelayWake(static_cast<int32_t>(std::min(double(blend_us) * slowness, 50000.0)));
    blend_us = ClockUs() - wake_us;
  }
  __imp__sub_82851F00(ctx, base);
  if (of_server) {
    g_deliveries.fetch_add(1, std::memory_order_relaxed);
    g_last_delivery_ms.store(g_clock_ms(), std::memory_order_relaxed);
    if (REXCVAR_GET(nfsmw_audio_wait_server_ms) > 0) {
      {
        std::lock_guard<std::mutex> lock(g_delivery_mutex);
      }
      g_delivery_cv.notify_all();
    }
    if (blend_us >= 0 && diag) {
      const int64_t us = blend_us;
      g_blends.fetch_add(1, std::memory_order_relaxed);
      g_blend_sum_us.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
      int64_t maximum = g_blend_max_us.load(std::memory_order_relaxed);
      while (us > maximum && !g_blend_max_us.compare_exchange_weak(maximum, us, std::memory_order_relaxed)) {
      }
    }
  }
}

REX_HOOK_RAW(sub_82851FB8) {
  using namespace nfsmw::audio_server;
  const bool of_server = static_cast<uint32_t>(ctx.lr) == kReturnArranque;
  __imp__sub_82851FB8(ctx, base);
  if (of_server) {
    REXLOG_INFO("[audio] voice del server de audio arrancada: result 0x{:08X}", ctx.r3.u32);
  }
}

namespace nfsmw::audio_profile {
void MarkThreadServer();  // nfsmw_audio_profile_functions.cpp (nfsmw_audio_diag_functions)
}  // namespace nfsmw::audio_profile

REX_HOOK_RAW(sub_828531E8) {
  using namespace nfsmw::audio_server;
  const uint32_t obj = ctx.r31.u32;
  if (static_cast<uint32_t>(ctx.lr) != kReturnWait || !REXCVAR_GET(nfsmw_audio_rescue) ||
      obj == 0 || Read32(base, obj + kOffEvent) != ctx.r3.u32) {
    __imp__sub_828531E8(ctx, base);
    return;
  }
  const uint32_t event = ctx.r3.u32;
  const uint32_t alertable = ctx.r5.u32;
  Rescue& r = g_rescue;
  g_obj_server.store(obj, std::memory_order_relaxed);
  g_base_server.store(base, std::memory_order_relaxed);
  // The SDK hook is registered once, from the server thread itself, once its object and the base are known.
  static std::atomic<bool> hook_registered{false};
  if (!hook_registered.exchange(true, std::memory_order_relaxed)) {
    rex::audio::SetHookBeforeOfFrame(&WaitPacketServer);
    REXLOG_INFO("[audio] wait al server before de every frame: as mucho {} ms (0 = off)",
                REXCVAR_GET(nfsmw_audio_wait_server_ms));
  }
  // Once per thread, from the server thread itself: its handle (for the CPU in the packet ring summaries), the
  // server thread mark for nfsmw_audio_diag_functions, and nfsmw_audio_server_priority.
  thread_local bool priority_applied = false;
  if (!priority_applied) {
    priority_applied = true;
    g_handler_server.store(nfsmw::threads::HandlerThreadCurrent(), std::memory_order_relaxed);
    nfsmw::audio_profile::MarkThreadServer();
    const int32_t priority = REXCVAR_GET(nfsmw_audio_server_priority);
    if (priority >= 0x1C && priority <= 0x3A) {
      const bool done = nfsmw::threads::PriorityThreadCurrent(priority);
      REXLOG_INFO("[audio] thread_value server de audio a priority 0x{:X}: {}", priority,
                  done ? "done" : "no available en esta platform");
    } else if (priority != 0) {
      REXLOG_WARN("[audio] nfsmw_audio_server_priority = {} outside de 0x1C-0x3A: se ignora", priority);
    }
  }
  for (;;) {
    if (r.first_wait_ms < 0) {
      r.first_wait_ms = g_clock_ms();
    }
    ctx.r3.u64 = event;
    ctx.r4.u64 = static_cast<uint32_t>(r.active ? kDeadlineRescueMs : kDeadlineMs);
    ctx.r5.u64 = alertable;
    __imp__sub_828531E8(ctx, base);
    const bool signal = ctx.r3.u32 != kStatusTimeout;
    // Diagnostic: simulates on PC the delay with which the thread runs again on the Switch.
    const int32_t delay_us = REXCVAR_GET(nfsmw_audio_diag_delay_server_us);
    if (signal && delay_us > 0) {
      DelayWake(delay_us);
    }
    if (signal && (REXCVAR_GET(nfsmw_audio_diag_ring) || REXCVAR_GET(nfsmw_audio_diag_slowness_blend) > 0.0)) {
      g_wake_us.store(ClockUs(), std::memory_order_relaxed);
    }
    if (signal && REXCVAR_GET(nfsmw_audio_diag_ring)) {
      const int core = nfsmw::threads::CoreCurrent();
      g_wakes_core[core >= 0 && core < 4 ? core : 4].fetch_add(1, std::memory_order_relaxed);
    }
    const int64_t now = g_clock_ms();

    if (r.active && g_ends.load(std::memory_order_relaxed) != r.ends_to_enter) {
      REXLOG_INFO("[audio] la voice vuelve a to_return ends de packet after {} ms de rescue "
                  "(packets released {}, redeliveries {})",
                  now - r.since_ms, r.released, r.redeliveries);
      r.active = false;
    }

    const bool w_busy = State(base, obj, Read32(base, obj + kOffW) % 2) != kFree;
    if (!r.active) {
      const int64_t reference =
          std::max({g_last_end_ms.load(std::memory_order_relaxed),
                    g_last_delivery_ms.load(std::memory_order_relaxed), r.first_wait_ms});
      const bool stuck =
          w_busy && base[kFlagServer] != 0 && now - reference >= kWithoutAdvanceMs;
      if (!stuck) {
        if (signal) {
          break;
        }
        continue;
      }
      r.active = true;
      r.since_ms = now;
      r.last_summary_ms = now;
      r.ends_to_enter = g_ends.load(std::memory_order_relaxed);
      if (++r.times <= kMaxDumps) {
        Dump("el thread_value server lleva 250 ms sin advance; entra en rescue", base, obj, now);
      }
    }

    // In rescue mode: first the same as the game would do, and if slot W is still
    // occupied it is freed so that the loop can mix the next packet.
    r.redeliveries += Redeliver(ctx, base, obj);
    const uint32_t w = Read32(base, obj + kOffW) % 2;
    if (State(base, obj, w) != kFree) {
      Write32(base, obj + kOffStates + 4 * w, kFree);
      ++r.released;
    }
    if (now - r.last_summary_ms >= kSummaryRescueMs && r.times <= kMaxDumps) {
      r.last_summary_ms = now;
      Dump("sigue el rescue", base, obj, now);
    }
    break;
  }
  ctx.lr = kReturnWait;
}
