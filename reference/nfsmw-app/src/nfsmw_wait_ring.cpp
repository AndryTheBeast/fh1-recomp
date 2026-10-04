// nfsmw - the game's D3D waits without spinning (native renderer).
//
// WHY (PC profile of a race)
//   The "Main XThread" runs at 100 % of a core in a race and 84 % of that is a busy-wait. sub_82597690
//   (wait for the GPU to read the ring up to the write pointer) calls sub_825A5D18 in a loop, without
//   sleeping. That function only checks whether the read pointer the GPU reports back ([[device+10384]])
//   has changed since the last iteration and, if 2 s pass without changes, declares the GPU hung. Seven
//   other D3D waits also call it. On the console that thread runs at 80 % in a race and takes CPU away
//   from the PM4 ring thread, which is the one that sets the FPS.
//
// WHAT IT DOES
//   If the read pointer is the same as in the previous iteration, it waits at most
//   nfsmw_wait_ring_max_us for the ring thread to report it again or deliver an interrupt
//   (ProgressRing), and then calls the original function, which decides as before. It does not change
//   what the game sees, only when it looks. With bit 0x04 of [device+10433] (GPU already declared hung)
//   or with Xenos emulation, it does not wait.
//
// MEASURED ON THE CONSOLE (race, no overclock)
//   697 D3D iterations in 10 s, 697 waits, 1414.4 ms asleep and only 123 ended by the ring advancing.
//   That is: 70 waits/s (2.8 per frame), 5.8 ms per frame with the "Main XThread" stopped, and 82 % use
//   up the 2 ms timeout. It is not a notification failure: the ring thread runs at 95 % and its batches
//   are long (3.5 wakeups with data per frame for 23 writes of CP_RB_WPTR), plus 6.8 ms per frame spent
//   in presentation without advancing the read pointer. It is real backpressure, not a busy-wait.
//   That is why the timeout is left alone: lowering it only multiplies the wakeups on a 3-core machine
//   where the ring thread is the bottleneck; raising it saves nothing because the notification already
//   wakes up the waiter.
//
// MIND THE ADDRESSES: the read pointer word is in the physical area (0xE0000000 and above). On PC
//   the recompiled code adds 0x1000 to it (REX_PHYS_HOST_OFFSET in nfsmw_pch.h); on the Switch, nothing.
//   Without that offset an earlier version read another word, never saw the pointer stopped and never
//   slept once.

#include "nfsmw_hitch_waits.h"
#include "nfsmw_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_wait_ring_blocking, true, "NFSMW",
                    "Renderer native: el D3D del game duerme mientras wait a que el thread_value del ring "
                    "advance, en time de dar laps en sub_825A5D18")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_wait_ring_max_us, 2000, "NFSMW",
                     "Renderer native: wait maxima por lap de the waits del D3D del game, en "
                     "microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::native {
namespace {

std::mutex g_progress_mutex;
std::condition_variable g_progress_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

uint32_t ProgressRing() {
  return g_progress.load(std::memory_order_acquire);
}

// Order: the ring thread writes the read pointer, bumps the counter and only then checks whether anyone
// is waiting. The waiter reads the counter before looking at the pointer, registers itself and checks the
// counter under the lock: no notification is lost between checking and falling asleep.
//
// Whether someone is waiting is checked with the lock held. Checking it without the lock is a Dekker
// handshake, and on ARM (and on x86 with stores without a lock prefix) each side may not yet see what the
// other wrote: that is how the vertex copy thread hung in earlier versions. Here the wait is bounded and
// would only cost that bound, but the lock is a few tens of nanoseconds per ring pass.
void NotifyProgressRing() {
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) {
    g_progress_cv.notify_all();
  }
}

bool WaitProgressRing(uint32_t seen, std::chrono::microseconds limit) {
  bool advanced = false;
  {
    std::unique_lock<std::mutex> lock(g_progress_mutex);
    g_waiting.fetch_add(1, std::memory_order_acq_rel);
    advanced = g_progress_cv.wait_for(lock, limit, [seen] {
      return g_progress.load(std::memory_order_acquire) != seen;
    });
    g_waiting.fetch_sub(1, std::memory_order_acq_rel);
  }
  return advanced;
}

}  // namespace nfsmw::native

namespace {

constexpr uint32_t kOffReadReturned = 10384;  // pointer to the word where the GPU reports the read pointer
constexpr uint32_t kOffState = 10433;           // bit 0x04: GPU considered hung

// The same as REX_LOAD_U32 / REX_LOAD_U8 in the recompiled code (nfsmw_pch.h).
inline uint32_t OffsetPhysical(uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return address >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)address;
  return 0u;
#endif
}

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address + OffsetPhysical(address), sizeof(v));
  return __builtin_bswap32(v);
}

uint8_t Read8(const uint8_t* base, uint32_t address) {
  return base[uint64_t(address) + OffsetPhysical(address)];
}

std::atomic<uint64_t> g_laps{0};
std::atomic<uint64_t> g_waits{0};
std::atomic<uint64_t> g_advances{0};
std::atomic<uint64_t> g_ns_waiting{0};
// The worst wait of the interval. The average says nothing here: in a console measurement there were 697
// waits and 1414 ms, i.e. 2.03 ms on average with a cap of 2.00, and only 123 ended because the ring
// advanced. So 82 % use up the whole timeout. What needs to be known is whether any goes far beyond the
// cap (the ring thread can spend 7 ms inside presentation without advancing the read pointer, and then
// the game's D3D pays two or three timeouts in a row for nothing).
std::atomic<uint64_t> g_ns_maxima{0};
std::atomic<int64_t> g_next_report_ms{0};

void ReportWaitsComplete();  // Defined below

// Only called when sleeping (and every 2^16 iterations without sleeping): reading the clock on every
// iteration cost more than the check itself (an earlier version: ~20 million iterations per second).
void Report() {
  using namespace std::chrono;
  const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t next = g_next_report_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  if (next == 0) {
    g_next_report_ms.store(now + 10000, std::memory_order_relaxed);
    return;
  }
  if (!g_next_report_ms.compare_exchange_strong(next, now + 10000)) {
    return;
  }
  const uint64_t laps = g_laps.exchange(0);
  const uint64_t waits = g_waits.exchange(0);
  const uint64_t advances = g_advances.exchange(0);
  const uint64_t ns = g_ns_waiting.exchange(0);
  const uint64_t ns_max = g_ns_maxima.exchange(0);
  REXLOG_INFO("[wait_ring] last 10 s: {} laps del D3D, {} waits ({} terminadas por advance del "
              "ring, {} agotaron el term de {} us), {:.1f} ms sleeping, worst {:.2f} ms",
              laps, waits, advances, waits > advances ? waits - advances : uint64_t(0),
              REXCVAR_GET(nfsmw_wait_ring_max_us), double(ns) / 1e6, double(ns_max) / 1e6);
  ReportWaitsComplete();
}

/*
 * The complete wait, and who asks for it.
 *
 * The code above measures each polling iteration (capped at 2 ms), so a 60 ms block of the game shows up
 * as thirty expired iterations and never as "worst 60 ms". That is why several builds did not show where
 * the game blocks in the corners and the alley. This measures the whole of sub_82597690 ("wait for the
 * GPU to read the ring up to here", called by the 13 D3D waits, eWaitUntilRenderingDone included) from
 * entry to exit, and breaks it down by return address (who called it).
 *
 * In our port the GPU memory writes (MEM_WRITE, EVENT_WRITE_SHD) are done when the packet is processed
 * on the PM4 ring thread, not when the real GPU finishes. So "waiting for the GPU" here means waiting
 * for the ring to consume the queue: if this comes out large, the ring is lagging behind.
 *
 * (Still inside the anonymous namespace above; the hook is at the end of the file, outside it.)
 */
constexpr size_t kCallers = 12;
struct Caller {
  std::atomic<uint32_t> lr{0};
  std::atomic<uint64_t> n{0};
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> ns_max{0};
  std::atomic<uint64_t> long{0};  // waits over 8 ms: a quarter of a frame, already a stutter
};
Caller g_callers[kCallers];
std::atomic<uint64_t> g_complete_n{0};
std::atomic<uint64_t> g_complete_ns{0};
std::atomic<uint64_t> g_complete_long{0};
std::atomic<uint64_t> g_complete_ns_max{0};

void NoteWaitComplete(uint32_t lr, uint64_t ns) {
  g_complete_n.fetch_add(1, std::memory_order_relaxed);
  g_complete_ns.fetch_add(ns, std::memory_order_relaxed);
  const bool long = ns > 8000000;
  if (long) g_complete_long.fetch_add(1, std::memory_order_relaxed);
  {
    uint64_t previous = g_complete_ns_max.load(std::memory_order_relaxed);
    while (ns > previous && !g_complete_ns_max.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {
    }
  }
  // Slot per caller: the first one with that lr, or the first empty one. With 13 possible callers and 12
  // slots, in the worst case one is left without a slot and shows up as "others" in the total.
  for (size_t i = 0; i < kCallers; ++i) {
    uint32_t expected = 0;
    if (g_callers[i].lr.load(std::memory_order_relaxed) == lr ||
        g_callers[i].lr.compare_exchange_strong(expected, lr, std::memory_order_relaxed) ||
        expected == lr) {
      g_callers[i].n.fetch_add(1, std::memory_order_relaxed);
      g_callers[i].ns.fetch_add(ns, std::memory_order_relaxed);
      if (long) g_callers[i].long.fetch_add(1, std::memory_order_relaxed);
      uint64_t previous = g_callers[i].ns_max.load(std::memory_order_relaxed);
      while (ns > previous &&
             !g_callers[i].ns_max.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {
      }
      return;
    }
  }
}

void ReportWaitsComplete() {
  const uint64_t n = g_complete_n.exchange(0);
  if (!n) {
    return;
  }
  const uint64_t ns = g_complete_ns.exchange(0);
  const uint64_t long = g_complete_long.exchange(0);
  const uint64_t ns_max = g_complete_ns_max.exchange(0);
  std::string by_caller;
  for (size_t i = 0; i < kCallers; ++i) {
    const uint32_t lr = g_callers[i].lr.load(std::memory_order_relaxed);
    if (!lr) continue;
    const uint64_t ln = g_callers[i].n.exchange(0);
    if (!ln) continue;
    const uint64_t lns = g_callers[i].ns.exchange(0);
    const uint64_t lmax = g_callers[i].ns_max.exchange(0);
    const uint64_t l_long = g_callers[i].long.exchange(0);
    char buf[96];
    std::snprintf(buf, sizeof(buf), " %08X:%llu/%.1fms/worst%.1f/long%llu", lr, (unsigned long long)ln,
                  double(lns) / 1e6, double(lmax) / 1e6, (unsigned long long)l_long);
    by_caller += buf;
  }
  REXLOG_INFO("[wait_ring] waits COMPLETE del D3D (sub_82597690) en 10 s: {} en {:.1f} ms, {} de mas "
              "de 8 ms, worst {:.1f} ms; por caller (lr:n/ms/worst/long):{}",
              n, double(ns) / 1e6, long, double(ns_max) / 1e6, by_caller);
}

}  // namespace

REX_EXTERN(__imp__sub_825A5D18);
REX_HOOK_RAW(sub_825A5D18) {
  static const bool active = nfsmw::native::Active() && REXCVAR_GET(nfsmw_wait_ring_blocking);
  if (active) {
    const uint32_t structure = ctx.r3.u32;
    const uint32_t vulkan_device = structure ? Read32(base, structure) : 0;
    if (vulkan_device && !(Read8(base, vulkan_device + kOffState) & 0x04)) {
      const uint32_t word = Read32(base, vulkan_device + kOffReadReturned);
      if (word) {
        const uint64_t laps = g_laps.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t seen = nfsmw::native::ProgressRing();
        if (Read32(base, word) == Read32(base, structure + 8)) {
          const auto before = std::chrono::steady_clock::now();
          g_waits.fetch_add(1, std::memory_order_relaxed);
          if (nfsmw::native::WaitProgressRing(
                  seen, std::chrono::microseconds(REXCVAR_GET(nfsmw_wait_ring_max_us)))) {
            g_advances.fetch_add(1, std::memory_order_relaxed);
          }
          const uint64_t ns_wait = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - before)
                  .count());
          g_ns_waiting.fetch_add(ns_wait, std::memory_order_relaxed);
          nfsmw::waits::Add(nfsmw::waits::kRoomRing, ns_wait);
          {
            uint64_t previous = g_ns_maxima.load(std::memory_order_relaxed);
            while (ns_wait > previous &&
                   !g_ns_maxima.compare_exchange_weak(previous, ns_wait, std::memory_order_relaxed)) {
            }
          }
          Report();
        } else if ((laps & 0xFFFF) == 0) {
          Report();
        }
      }
    }
  }
  __imp__sub_825A5D18(ctx, base);
}

// The complete D3D wait for the ring to reach a point, with who asks for it.
// See the comment of NoteWaitComplete. Always measured: two clock reads per wait.
REX_EXTERN(__imp__sub_82597690);
REX_HOOK_RAW(sub_82597690) {
  const uint32_t lr = uint32_t(ctx.lr);
  const auto before = std::chrono::steady_clock::now();
  __imp__sub_82597690(ctx, base);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - before).count());
  NoteWaitComplete(lr, ns);
}
