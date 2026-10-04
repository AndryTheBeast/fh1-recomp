// nfsmw - frame handoff between the game's two threads without a Sleep(0) loop.
//
// WHY (PC profile of a race)
//   With the D3D wait already sleeping (nfsmw_wait_ring.cpp), the thread that prepares the frames
//   (XThread F800002C) is still at 101 %: 41 % in yields and 12 % in the game's Sleep (sub_8262F258).
//   The game splits each frame between two threads with a flag at 0x82A2CF40:
//     - That thread, in sub_824411B8, waits in sub_82441F18 for the flag to go back to 0 (the previous
//       frame has been executed) by calling Sleep(0) (sub_8262D988) in a loop. Then sub_82442058 sets it
//       to 1 and fills the command list 0x82909650 inside sub_82445660.
//     - The "Main XThread", in sub_82441CC8, waits with another Sleep(0) loop for the flag to be 1, runs
//       the commands (sub_823C83F8), sets it to 0 and immediately calls sub_8262DE18 (a game trace).
//   On the console both threads run at 44-83 % in a race.
//
// WHAT IT DOES
//   The Sleep(0) calls of those two loops (return addresses 0x82441FB4 and 0x82441D24) sleep until the
//   flag changes, or at most nfsmw_wait_frame_max_us, instead of yielding. The change is signaled by
//   the entry to sub_82445660 (right after setting it to 1) and by the call to sub_8262DE18 (right after
//   setting it to 0). The game's loop checks the flag again, so an extra notification changes nothing and
//   a lost one only costs the maximum time. The game's other Sleep calls are not touched.
//
// MEASURED ON THE CONSOLE (race, no overclock), AND WHAT IS NOT KNOWN
//   Per 10 s window: the preparer between 736 and 4888 waits (1.0 to 6.1 s asleep), the executor between
//   260 and 717 (0.1 to 1.1 s). In the worst window that is 434 waits/s for the preparer, 17.7 per frame,
//   and 21.7 ms of each frame with that thread asleep. That is not a problem by itself: the preparer
//   waits for the executor to finish the command list, and while it waits it has nothing to do.
//   What the earlier counters could not tell: the average per wait was 1.23 ms with the timeout at
//   1.00 ms. That fits both "almost all use up the timeout" (the notification does not work) and "the
//   ones in between use it up and the last one is woken by the notification" (the notification works),
//   because on Horizon, with 3 cores and the ring thread at 95 %, getting back on the CPU after the
//   timeout already costs those tenths. That is why the waits ended by a notification and the worst one
//   are counted separately. Mostly by notification means the timeout can go up to 4-8 ms, removing 15
//   wakeups per frame at no cost; almost none means the notification is broken and the handoff pays up to
//   1 ms of delay on every delivery.

#include "nfsmw_hitch_waits.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriorityOk. switch.h is deliberately not included.
#include "../../sdk/src/core/threading_switch.h"
#endif

/*
 * The two handoff threads, one step above the rest of the game.
 *
 * What the logs showed. This file's log reports the worst wait of each 10 s window, and with a
 * 1 ms timeout it comes out again and again between 7 and 17 ms (executor, worst 16.94; preparer, worst
 * 17.20). A wait with a 1 ms timeout can only last 17 ms if the thread, already awake, does not get a
 * core. And that is exactly what to expect: the game's threads all run at 0x3B, the only priority that
 * Horizon shares in 10 ms time slices, with the CPU at 280 % out of 300. The thread that sets the frame
 * pace (Main XThread, the executor) wakes up and sits in the queue behind another game thread until
 * that one's slice runs out. A 33 ms frame plus a lost slice is the jump to 50 ms that is seen as a
 * stutter.
 *
 * And it rules out the other explanation: the ring dropped from 80.8 to 75.7 % CPU and the FPS did not
 * move, while the Main XThread stayed at 77-80 % over several builds. The game thread is now the
 * limiting factor, not the ring.
 *
 * 0x3A (lower number = higher priority) preempts the other game threads on wakeup, and stays below
 * the ring (0x2D), the presenter (0x2C) and the audio (0x2B), which are not touched.
 *
 * The risk, and why it is acceptable: below 0x3B Horizon does not time-slice, so a thread at 0x3A
 * that never yields would starve the 0x3B threads on that core. These two yield: they block in the
 * handoff hundreds of times per second (this same log counts it), the executor in the D3D wait
 * (nfsmw_wait_ring.cpp), and neither goes above 80 % of a core; and with mask 0x7 the others have
 * two more cores. If something still got stuck, 0 restores the previous behavior.
 *
 * It is applied on each thread's first wait, which is when we know which is which.
 */
REXCVAR_DEFINE_BOOL(nfsmw_executor_without_laps, true, "NFSMW",
                    "24/09 (build 169): el Main XThread wait the commands del preparer SLEEPING en pauses short "
                    "en time de dar laps sin stop en sub_82441CC8 (12,6 % de un core en la 162). false = as "
                    "before");
REXCVAR_DEFINE_INT32(nfsmw_executor_pause_us, 100, "NFSMW",
                     "Pause del executor mientras wait commands (us). Minus = responde before y gasta mas CPU");
REXCVAR_DEFINE_INT32(nfsmw_executor_wait_max_us, 2000, "NFSMW",
                     "As mucho esto (us) por wait del executor; luego el loop del game vuelve a look");

REXCVAR_DEFINE_INT32(nfsmw_handoff_priority, 0x3A, "NFSMW",
                     "Switch: priority de Horizon de los dos threads del handoff de frames (el que "
                     "prepara y el que ejecuta, Main XThread). 0 = no touch (0x3B, as el rest del "
                     "game). 0x3A = un step por encima de los demas threads del game, para que al "
                     "wake no esperen un turn de 10 ms")
    .range(0, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_wait_frame_blocking, true, "NFSMW",
                    "Handoff de frames between los dos threads del game (flag 0x82A2CF40): duermen until el "
                    "change en time de call a Sleep(0) en loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_wait_frame_max_us, 1000, "NFSMW",
                     "Wait maxima por lap del handoff de frames, en microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kFlag = 0x82A2CF40;
constexpr uint32_t kReturnPreparer = 0x82441FB4;  // Sleep(0) of sub_82441F18: waits for the flag to stop being 1
constexpr uint32_t kReturnExecutor = 0x82441D24;    // Sleep(0) of sub_82441CC8: waits for the flag to stop being 0

std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<uint32_t> g_changes{0};
std::atomic<int> g_waiting{0};

std::atomic<uint64_t> g_waits_preparer{0};
std::atomic<uint64_t> g_waits_executor{0};
std::atomic<uint64_t> g_ns_preparer{0};
std::atomic<uint64_t> g_ns_executor{0};
// How many waits end because the notification arrives and how many use up the timeout, as in
// nfsmw_wait_ring.cpp ("N terminadas por advance del ring"). Without this, "the handoff wakes up
// immediately" cannot be told apart from "the notification never arrives and the whole timeout is always
// paid", and in a console measurement the average per wait (1.23 ms with a cap of 1.00) fit both. The
// answer tells whether nfsmw_wait_frame_max_us can be raised (fewer wakeups, same delay) or whether
// raising it would add up to that timeout of delay to each handoff.
std::atomic<uint64_t> g_warnings_preparer{0};
std::atomic<uint64_t> g_warnings_executor{0};
std::atomic<uint64_t> g_ns_max_preparer{0};
std::atomic<uint64_t> g_ns_max_executor{0};
std::atomic<int64_t> g_next_report_ms{0};
// The executor with no commands (see the sub_823C83F8 hook).
std::atomic<uint64_t> g_without_commands_waits{0};
std::atomic<uint64_t> g_without_commands_pauses{0};
std::atomic<uint64_t> g_without_commands_ns{0};

/*
 * Wakeup delay, measured rather than inferred from the "worst".
 *
 * It is the time from when the other thread notifies (or the timeout expires) until the sleeping one
 * really runs again. With spare CPU it is tenths of a millisecond; if the scheduler holds it back, it
 * is milliseconds. What counts are the ones above 3 ms: each one is a frame that can jump from the 33 ms
 * vblank to the 50 ms one. This is the measurement that says whether nfsmw_handoff_priority helps:
 * going from 0 to 0x3A must lower the number of "late" ones.
 */
std::atomic<int64_t> g_last_warning_ns{0};
std::atomic<uint64_t> g_wake_ns{0};
std::atomic<uint64_t> g_wake_n{0};
std::atomic<uint64_t> g_wake_late{0};  // more than 3 ms
std::atomic<uint64_t> g_wake_max_ns{0};
constexpr int64_t kWakeLateNs = 3000000;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void NoteWake(int64_t delay_ns) {
  if (delay_ns < 0) {
    delay_ns = 0;
  }
  g_wake_ns.fetch_add(uint64_t(delay_ns), std::memory_order_relaxed);
  g_wake_n.fetch_add(1, std::memory_order_relaxed);
  if (delay_ns > kWakeLateNs) {
    g_wake_late.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t previous = g_wake_max_ns.load(std::memory_order_relaxed);
  while (uint64_t(delay_ns) > previous &&
         !g_wake_max_ns.compare_exchange_weak(previous, uint64_t(delay_ns),
                                                   std::memory_order_relaxed)) {
  }
}

// Once per thread: raises its priority the first time it enters the handoff. The role is given by the
// return address, so there is no need to know in advance which thread is which.
void UploadPriorityOneTime(bool& done, const char* role) {
  if (done) {
    return;
  }
  done = true;
#if REX_PLATFORM_SWITCH
  const int32_t priority = REXCVAR_GET(nfsmw_handoff_priority);
  if (priority >= 0x1C && priority <= 0x3B) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
    REXLOG_INFO("[wait_frame] thread_value {} del handoff a priority {:#x} ({})", role,
                uint32_t(priority), ok ? "aceptada" : "RECHAZADA por el kernel");
  } else {
    REXLOG_INFO("[wait_frame] thread_value {} del handoff: priority sin touch (0x3B)", role);
  }
#else
  (void)role;
#endif
}

void Maximum(std::atomic<uint64_t>& target, uint64_t input_value) {
  uint64_t previous = target.load(std::memory_order_relaxed);
  while (input_value > previous && !target.compare_exchange_weak(previous, input_value, std::memory_order_relaxed)) {
  }
}

uint32_t Read32(const uint8_t* base, uint32_t address) {  // address < 0xE0000000: sin displacement physical
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

// Whether someone is waiting is checked with the lock held (as in nfsmw_wait_ring.cpp): without the
// lock, each side may not yet see what the other wrote and the notification is lost; here it would cost
// the maximum time, a stutter of up to 1 ms.
void Notify() {
  g_last_warning_ns.store(NowNs(), std::memory_order_relaxed);
  g_changes.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) {
    g_cv.notify_all();
  }
}

// Sleeps until the flag stops being 'input_value', until a notification or at most the maximum time. Returns
// the nanoseconds slept; 'by_warning' says whether it ended because the condition was met (notification
// or flag already changed) rather than by using up the timeout.
uint64_t Wait(const uint8_t* base, uint32_t input_value, bool& by_warning) {
  by_warning = true;
  const uint32_t seen = g_changes.load(std::memory_order_acquire);
  if (Read32(base, kFlag) != input_value) {
    return 0;
  }
  const auto before = std::chrono::steady_clock::now();
  const int64_t deadline_us = REXCVAR_GET(nfsmw_wait_frame_max_us);
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_waiting.fetch_add(1, std::memory_order_acq_rel);
    // wait_for with a predicate returns false only if the timeout expired with the predicate still unmet.
    by_warning = g_cv.wait_for(lock, std::chrono::microseconds(deadline_us),
                              [base, input_value, seen] {
                                return g_changes.load(std::memory_order_acquire) != seen ||
                                       Read32(base, kFlag) != input_value;
                              });
    g_waiting.fetch_sub(1, std::memory_order_acq_rel);
  }
  const auto after = std::chrono::steady_clock::now();
  const uint64_t asleep =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
  // Wakeup delay: from the notification if there was one; if the timeout expired, how far past it.
  const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               after.time_since_epoch())
                               .count();
  if (by_warning && g_changes.load(std::memory_order_acquire) != seen) {
    NoteWake(now_ns - g_last_warning_ns.load(std::memory_order_relaxed));
  } else if (!by_warning) {
    NoteWake(int64_t(asleep) - deadline_us * 1000);
  }
  return asleep;
}

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
  const uint64_t waits_p = g_waits_preparer.exchange(0);
  const uint64_t waits_e = g_waits_executor.exchange(0);
  NFSMW_REPORT_DEFERRED("[wait_frame] last 10 s: preparer {} waits ({} por warning, {:.1f} ms sleeping, "
              "worst {:.2f} ms), executor {} waits ({} por warning, {:.1f} ms sleeping, worst {:.2f} ms); "
              "term {} us",
              waits_p, g_warnings_preparer.exchange(0), double(g_ns_preparer.exchange(0)) / 1e6,
              double(g_ns_max_preparer.exchange(0)) / 1e6, waits_e, g_warnings_executor.exchange(0),
              double(g_ns_executor.exchange(0)) / 1e6, double(g_ns_max_executor.exchange(0)) / 1e6,
              REXCVAR_GET(nfsmw_wait_frame_max_us));
  // The executor sleeps while waiting for commands instead of spinning.
  NFSMW_REPORT_DEFERRED("[wait_frame] executor sin commands: {} waits, {} pauses de {} us, {:.1f} ms sleeping "
              "(before, dando laps)",
              g_without_commands_waits.exchange(0), g_without_commands_pauses.exchange(0),
              REXCVAR_GET(nfsmw_executor_pause_us), double(g_without_commands_ns.exchange(0)) / 1e6);
  // What decides whether nfsmw_handoff_priority helps. See g_wake_*.
  const uint64_t n = g_wake_n.exchange(0);
  const uint64_t ns = g_wake_ns.exchange(0);
  NFSMW_REPORT_DEFERRED("[wait_frame] wake: middle {:.3f} ms, worst {:.2f} ms, {} LATE (mas de 3 ms) de {}; "
              "priority del handoff {:#x}",
              n ? double(ns) / 1e6 / double(n) : 0.0, double(g_wake_max_ns.exchange(0)) / 1e6,
              g_wake_late.exchange(0), n, uint32_t(REXCVAR_GET(nfsmw_handoff_priority)));
}

}  // namespace

// The game's Sleep(ms): li r4,0 and a jump to sub_8262F258. The two handoff loops only check the flag on
// return.
REX_EXTERN(__imp__sub_8262D988);
REX_HOOK_RAW(sub_8262D988) {
  static const bool active = REXCVAR_GET(nfsmw_wait_frame_blocking);
  if (active && ctx.r3.u32 == 0) {
    const uint32_t return_value = uint32_t(ctx.lr);
    if (return_value == kReturnPreparer) {
      thread_local bool priority_set = false;
      UploadPriorityOneTime(priority_set, "preparer");
      bool by_warning = false;
      const uint64_t ns = Wait(base, 1, by_warning);
      g_waits_preparer.fetch_add(1, std::memory_order_relaxed);
      g_ns_preparer.fetch_add(ns, std::memory_order_relaxed);
      if (by_warning) {
        g_warnings_preparer.fetch_add(1, std::memory_order_relaxed);
      }
      Maximum(g_ns_max_preparer, ns);
      nfsmw::waits::Add(nfsmw::waits::kHandoffPreparer, ns);
      Report();
      ctx.r3.u64 = 0;
      return;
    }
    if (return_value == kReturnExecutor) {
      thread_local bool priority_set = false;
      UploadPriorityOneTime(priority_set, "executor (Main XThread)");
      bool by_warning = false;
      const uint64_t ns = Wait(base, 0, by_warning);
      g_waits_executor.fetch_add(1, std::memory_order_relaxed);
      g_ns_executor.fetch_add(ns, std::memory_order_relaxed);
      if (by_warning) {
        g_warnings_executor.fetch_add(1, std::memory_order_relaxed);
      }
      Maximum(g_ns_max_executor, ns);
      nfsmw::waits::Add(nfsmw::waits::kHandoffExecutor, ns);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_8262D988(ctx, base);
}

/*
 * During stutters the preparer slept almost half of the time in here (sub_826E8EE8 <- sub_8245DE88 <-
 * sub_82285F78 <- sub_823AFF80), in a loop of KeWaitForSingleObject and Sleep on an object with virtual
 * calls (states 1 and 7). Measurement only: how long it lasts, the object's vtable and the caller, for
 * the "[hitch]" line. It changes nothing of what it does.
 */
REX_EXTERN(__imp__sub_826E8EE8);
REX_HOOK_RAW(sub_826E8EE8) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const auto before = std::chrono::steady_clock::now();
  __imp__sub_826E8EE8(ctx, base);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - before).count());
  nfsmw::waits::Add(nfsmw::waits::kGameMiddle, ns);
  if (object) {
    nfsmw::waits::g_vtable_middle.store(Read32(base, object), std::memory_order_relaxed);
  }
  nfsmw::waits::g_caller_middle.store(caller, std::memory_order_relaxed);
}

/*
 * The executor spun without sleeping while waiting for commands.
 *
 * Stack sampling in a race: the Main XThread spent 12.6 % of a core in the code of sub_82441CC8 itself (with LTO
 * it has sub_823C83F8 inlined). It is this loop (loc_82441D6C):
 *
 *     done = list[+12] == 0 || (list[+8] != 0 && list[+0] == list[+4]);
 *     while (!done) { sub_823C83F8(list, list[+0] - list[+4]); ...recompute done... }
 *
 * list = 0x82909650: +0 commands written by the preparer, +4 commands executed, +8 "the preparer has closed
 * the list", +12 list open. While the preparer is still writing (+8 == 0) and there are no new commands
 * (+0 == +4), sub_823C83F8 is called with 0 commands, returns at once, and is called again, without sleeping.
 * On the Xbox 360 that was a hardware thread; here it is one core out of 3, and the executor runs at 0x3A,
 * where Horizon does not time-slice: it does not give the core up to the game threads (0x3B) until it moves
 * elsewhere.
 *
 * Here, only in that call (return address 0x82441DC4) and only with 0 commands: it sleeps in short pauses
 * while the list is still open, not closed and without new commands, for at most
 * nfsmw_executor_wait_max_us. Then the original is called as before (with 0 commands it does nothing), and
 * the game's loop checks the conditions again.
 */
constexpr uint32_t kReturnCommands = 0x82441DC4;
REX_EXTERN(__imp__sub_823C83F8);
REX_HOOK_RAW(sub_823C83F8) {
  static const bool active = REXCVAR_GET(nfsmw_executor_without_laps);
  if (active && uint32_t(ctx.lr) == kReturnCommands && ctx.r4.u32 == 0) {
    const uint32_t list = ctx.r3.u32;
    const auto without_commands = [base, list] {
      return Read32(base, list + 12) != 0 && base[list + 8] == 0 && Read32(base, list) == Read32(base, list + 4);
    };
    if (without_commands()) {
      const auto before = std::chrono::steady_clock::now();
      const auto pause = std::chrono::microseconds(std::max(REXCVAR_GET(nfsmw_executor_pause_us), 10));
      const auto limit = before + std::chrono::microseconds(std::max(REXCVAR_GET(nfsmw_executor_wait_max_us), 50));
      uint64_t pauses = 0;
      do {
        rex::thread::Sleep(pause);
        ++pauses;
      } while (without_commands() && std::chrono::steady_clock::now() < limit);
      g_without_commands_waits.fetch_add(1, std::memory_order_relaxed);
      g_without_commands_pauses.fetch_add(pauses, std::memory_order_relaxed);
      const uint64_t ns_without_commands = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - before)
                                                   .count());
      g_without_commands_ns.fetch_add(ns_without_commands, std::memory_order_relaxed);
      // Also in the stutter frame ("[hitch] game" line, nfsmw_hitch_waits.h).
      nfsmw::waits::Add(nfsmw::waits::kExecutorWithoutCommands, ns_without_commands);
    }
  }
  __imp__sub_823C83F8(ctx, base);
}

// Right after the flag is set to 1 (sub_82442058).
// Also how long the preparer takes to fill the list (the whole call) and the time spent outside it between two
// fills (its simulation plus its handoff wait), for the "[hitch] game" line. Once per frame.
REX_EXTERN(__imp__sub_82445660);
REX_HOOK_RAW(sub_82445660) {
  Notify();
  static int64_t end_previous_ns = 0;  // only called by the thread that prepares the frames
  const int64_t start_ns = NowNs();
  if (end_previous_ns != 0) {
    nfsmw::waits::Add(nfsmw::waits::kPreparerOutside, uint64_t(start_ns - end_previous_ns));
  }
  __imp__sub_82445660(ctx, base);
  end_previous_ns = NowNs();
  nfsmw::waits::Add(nfsmw::waits::kPreparerList, uint64_t(end_previous_ns - start_ns));
}

// Right after it is set to 0 (sub_82441CC8); sub_82441F18 also calls it, and an extra notification does not matter.
REX_EXTERN(__imp__sub_8262DE18);
REX_HOOK_RAW(sub_8262DE18) {
  Notify();
  __imp__sub_8262DE18(ctx, base);
}
