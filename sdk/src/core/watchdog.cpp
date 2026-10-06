/**
 * @file        watchdog.cpp
 * @brief       See rex/watchdog.h.
 */

#include <rex/watchdog.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if REX_PLATFORM_SWITCH
#define REX_WATCHDOG_DEFAULT_SECONDS 40.0
#else
#define REX_WATCHDOG_DEFAULT_SECONDS 0.0
#endif

REXCVAR_DEFINE_DOUBLE(watchdog_seconds, REX_WATCHDOG_DEFAULT_SECONDS, "Watchdog",
                      "Freeze watchdog: if no frame is presented for this many seconds while the game is in the "
                      "foreground, write what everything is waiting for to watchdog.log and close the game. "
                      "0 = off (default on the Switch: 40; on the PC: off)");
REXCVAR_DEFINE_DOUBLE(watchdog_boot_seconds, 120.0, "Watchdog",
                      "Freeze watchdog: seconds to wait for the very first frame after the game starts (loading "
                      "the game can take long)");
#if REX_PLATFORM_SWITCH
#define REX_WATCHDOG_DEFAULT_HEAP_MB 2480
#else
#define REX_WATCHDOG_DEFAULT_HEAP_MB 0
#endif
REXCVAR_DEFINE_INT32(watchdog_heap_mb, REX_WATCHDOG_DEFAULT_HEAP_MB, "Watchdog",
                     "Memory guard: when the program has taken this many MB from the system heap, write the state to "
                     "watchdog.log and close the game before the console hangs (it froze with the heap at 2.4-2.5 GB). "
                     "0 = off (default on the Switch: 2480; on the PC: off)");
REXCVAR_DEFINE_BOOL(watchdog_close, true, "Watchdog",
                    "Freeze watchdog: after writing the state, close the game (false = only write the state)");
REXCVAR_DEFINE_DOUBLE(watchdog_grace_seconds, 8.0, "Watchdog",
                      "Freeze watchdog: seconds the normal close gets before the process is ended by force");
REXCVAR_DEFINE_DOUBLE(watchdog_selftest_after_s, 0.0, "Watchdog",
                      "Freeze watchdog self-test: after this many seconds pretend that frames stopped, to check "
                      "the dump and the close (0 = off). Needs watchdog_seconds above 0");

namespace rex::watchdog {
namespace {

using Clock = std::chrono::steady_clock;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---- Wait registry -----------------------------------------------------------------------------------------

constexpr size_t kMaxSlots = 256;

struct Slot {
  std::atomic<uint32_t> seq{0};  // odd while the owner writes
  std::atomic<bool> used{false};
  std::atomic<bool> waiting{false};
  std::atomic<int64_t> since_ms{0};
  WaitInfo info;
};
Slot g_slots[kMaxSlots];

struct SlotOwner {
  Slot* slot = nullptr;
  ~SlotOwner() {
    if (slot) {
      slot->waiting.store(false, std::memory_order_release);
      slot->used.store(false, std::memory_order_release);
    }
  }
};
thread_local SlotOwner t_owner;

Slot* MySlot() {
  if (t_owner.slot) return t_owner.slot;
  for (Slot& s : g_slots) {
    bool expected = false;
    if (s.used.compare_exchange_strong(expected, true)) {
      t_owner.slot = &s;
      return &s;
    }
  }
  return nullptr;  // more than kMaxSlots live threads: not tracked
}

// ---- Sections, hooks, state ----------------------------------------------------------------------------------

constexpr size_t kMaxSections = 16;
struct Section {
  const char* name;
  void (*fn)(std::FILE*);
};
Section g_sections[kMaxSections];
std::atomic<size_t> g_section_count{0};

Hooks g_hooks;
std::mutex g_hooks_mutex;

std::atomic<uint64_t> g_progress{0};
std::once_flag g_started;
std::atomic<int64_t> g_reap_at_ms{0};  // forced exit time, 0 = not armed
std::atomic<bool> g_fired{false};
const int64_t g_start_ms = NowMs();

Hooks CurrentHooks() {
  std::lock_guard<std::mutex> lock(g_hooks_mutex);
  return g_hooks;
}

void PrintWaits(std::FILE* f, size_t max_lines, bool to_log) {
  struct Row {
    int64_t since;
    WaitInfo info;
  };
  std::vector<Row> rows;
  for (Slot& s : g_slots) {
    if (!s.used.load(std::memory_order_acquire) || !s.waiting.load(std::memory_order_acquire)) continue;
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint32_t a = s.seq.load(std::memory_order_acquire);
      if (a & 1) continue;
      Row row{s.since_ms.load(std::memory_order_relaxed), s.info};
      std::atomic_thread_fence(std::memory_order_acquire);
      if (s.seq.load(std::memory_order_relaxed) == a && s.waiting.load(std::memory_order_acquire)) {
        rows.push_back(row);
        break;
      }
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.since < b.since; });
  const int64_t now = NowMs();
  size_t printed = 0;
  for (const Row& r : rows) {
    if (printed++ >= max_lines) break;
    char line[512];
    char timeout[32] = "no limit";
    if (r.info.timeout_ms >= 0) std::snprintf(timeout, sizeof(timeout), "%" PRId64 " ms", r.info.timeout_ms);
    int n = std::snprintf(line, sizeof(line), "thread %u \"%s\" waits %.1f s: %s%s, timeout %s, called from guest %08X:",
                          r.info.thread_id, r.info.thread_name, double(now - r.since) / 1000.0, r.info.api,
                          r.info.count > 1 ? (r.info.wait_all ? " (all)" : " (any)") : "", timeout, r.info.guest_lr);
    const uint32_t shown = std::min<uint32_t>(r.info.count, kMaxWaitObjects);
    for (uint32_t i = 0; i < shown && n > 0 && size_t(n) < sizeof(line) - 80; ++i) {
      const WaitObject& o = r.info.objects[i];
      n += std::snprintf(line + n, sizeof(line) - size_t(n), " [%s handle %08X guest %08X \"%s\"]", o.type, o.handle,
                         o.guest_object, o.name);
    }
    if (r.info.count > shown && size_t(n) < sizeof(line)) {
      std::snprintf(line + n, sizeof(line) - size_t(n), " +%u more", r.info.count - shown);
    }
    if (f) std::fprintf(f, "  %s\n", line);
    if (to_log) REXLOG_ERROR("[watchdog] {}", line);
  }
  if (rows.empty()) {
    if (f) std::fprintf(f, "  (no guest thread is inside a kernel wait)\n");
    if (to_log) REXLOG_ERROR("[watchdog] no guest thread is inside a kernel wait");
  } else if (rows.size() > max_lines && f) {
    std::fprintf(f, "  ... and %zu more\n", rows.size() - max_lines);
  }
}

void Fire(const char* reason_text) {
  const Hooks hooks = CurrentHooks();
  const int64_t grace_ms = int64_t(std::max(1.0, REXCVAR_GET(watchdog_grace_seconds)) * 1000.0);
  // The dump gets 12 s; if it hangs (a thread paused holding a lock), the forced exit still happens.
  g_reap_at_ms.store(NowMs() + 12000 + grace_ms, std::memory_order_release);

  const char* reason = reason_text;
  REXLOG_ERROR("[watchdog] FREEZE: {}. Writing the state to watchdog.log", reason);
  PrintWaits(nullptr, 6, true);  // the longest waits go to the normal log too

  std::string path = hooks.log_dir ? hooks.log_dir() : "";
  path += "watchdog.log";
  if (std::FILE* f = std::fopen(path.c_str(), "a")) {
    DumpState(f, reason);
    std::fclose(f);
  } else {
    REXLOG_ERROR("[watchdog] cannot open {}", path);
  }
  REXLOG_ERROR("[watchdog] state written ({})", path);

  if (!REXCVAR_GET(watchdog_close)) {
    REXLOG_ERROR("[watchdog] watchdog_close is off: leaving the game running");
    g_reap_at_ms.store(0, std::memory_order_release);
    return;
  }
  REXLOG_ERROR("[watchdog] closing the game ({} s for a clean close, then by force)", grace_ms / 1000);
  rex::FlushLogging();
  g_reap_at_ms.store(NowMs() + grace_ms, std::memory_order_release);
  if (hooks.request_quit) hooks.request_quit();
}

void DetectorLoop() {
  int64_t last_tick = NowMs();
  int64_t last_heap_check = 0;
  int64_t last_change = last_tick;
  uint64_t last_count = g_progress.load(std::memory_order_acquire);
  bool started = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int64_t now = NowMs();
    const bool gap = now - last_tick > 5000;  // the process was suspended (console asleep): start counting again
    last_tick = now;
    const double seconds = REXCVAR_GET(watchdog_seconds);
    const Hooks hooks = CurrentHooks();
    const uint64_t count = g_progress.load(std::memory_order_acquire);
    const bool focus = !hooks.in_focus || hooks.in_focus();
    if (gap || !focus || count != last_count) {
      if (count != last_count) started = true;
      last_count = count;
      last_change = now;
    }
    if (g_fired.load(std::memory_order_acquire)) continue;
    // Memory guard, once per second (the platform call may walk the heap).
    const int32_t heap_limit = REXCVAR_GET(watchdog_heap_mb);
    if (heap_limit > 0 && hooks.heap_mb && now - last_heap_check >= 1000) {
      last_heap_check = now;
      const uint32_t heap = hooks.heap_mb();
      if (heap >= uint32_t(heap_limit) && !g_fired.exchange(true)) {
        char reason[160];
        std::snprintf(reason, sizeof(reason), "heap %u MB reached watchdog_heap_mb (%d MB): closing before the console hangs", heap,
                      heap_limit);
        Fire(reason);
        continue;
      }
    }
    if (seconds <= 0.0) continue;
    const double selftest = REXCVAR_GET(watchdog_selftest_after_s);
    double idle = double(now - last_change) / 1000.0;
    if (selftest > 0.0 && double(now - g_start_ms) / 1000.0 >= selftest) {
      idle = std::max(idle, seconds + 0.1);  // pretend frames stopped
    }
    const double limit = started ? seconds : std::max(seconds, REXCVAR_GET(watchdog_boot_seconds));
    if (idle >= limit && !g_fired.exchange(true)) {
      char reason[160];
      std::snprintf(reason, sizeof(reason), "no frame presented for %.1f s%s", idle, started ? "" : " (since the game started)");
      Fire(reason);
    }
  }
}

void ReaperLoop() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int64_t at = g_reap_at_ms.load(std::memory_order_acquire);
    if (at != 0 && NowMs() >= at) {
      REXLOG_ERROR("[watchdog] the game did not close in time: ending the process");
      rex::FlushLogging();
      std::fflush(nullptr);
      const Hooks hooks = CurrentHooks();
      if (hooks.force_exit) hooks.force_exit(2);
      std::_Exit(2);
    }
  }
}

}  // namespace

void BeginWait(const WaitInfo& info) {
  Start();
  Slot* s = MySlot();
  if (!s) return;
  s->seq.fetch_add(1, std::memory_order_acq_rel);  // odd: being written
  s->info = info;
  s->since_ms.store(NowMs(), std::memory_order_relaxed);
  s->waiting.store(true, std::memory_order_relaxed);
  s->seq.fetch_add(1, std::memory_order_release);  // even: complete
}

void EndWait() {
  if (t_owner.slot) t_owner.slot->waiting.store(false, std::memory_order_release);
}

void NoteProgress() {
  g_progress.fetch_add(1, std::memory_order_relaxed);
  if (g_progress.load(std::memory_order_relaxed) == 1) Start();
}

void RegisterSection(const char* name, void (*fn)(std::FILE*)) {
  const size_t i = g_section_count.fetch_add(1);
  if (i >= kMaxSections) return;
  g_sections[i] = {name, fn};
}

void SetHooks(const Hooks& hooks) {
  std::lock_guard<std::mutex> lock(g_hooks_mutex);
  g_hooks = hooks;
}

void Start() {
  // std::thread throws on the console (std::system_error, crash at startup 2026-10-05): the SDK's own
  // thread creation is what the game's threads use. A failure here must never take the game down.
  std::call_once(g_started, [] {
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 256 * 1024;
    auto detector = rex::thread::Thread::Create(params, DetectorLoop);
    auto reaper = rex::thread::Thread::Create(params, ReaperLoop);
    if (!detector || !reaper) {
      REXLOG_WARN("[watchdog] could not start its threads: the watchdog is off");
    }
    detector.release();  // run for the life of the process
    reaper.release();
  });
}

void DumpState(std::FILE* f, const char* reason) {
  const Hooks hooks = CurrentHooks();
  std::fprintf(f, "==== watchdog: %s | game up %.1f s ====\n", reason, double(NowMs() - g_start_ms) / 1000.0);
  std::fprintf(f, "frames presented since start: %" PRIu64 "\n\n", g_progress.load(std::memory_order_relaxed));
  std::fprintf(f, "-- guest threads inside a kernel wait (longest first) --\n");
  PrintWaits(f, 64, false);
  std::fflush(f);
  if (hooks.dump_memory) {
    std::fprintf(f, "\n-- memory --\n");
    hooks.dump_memory(f);
    std::fflush(f);
  }
  const size_t n = std::min(g_section_count.load(), kMaxSections);
  for (size_t i = 0; i < n; ++i) {
    std::fprintf(f, "\n-- %s --\n", g_sections[i].name);
    g_sections[i].fn(f);
    std::fflush(f);
  }
  if (hooks.dump_threads) {
    std::fprintf(f, "\n-- host threads --\n");
    hooks.dump_threads(f);
  }
  std::fprintf(f, "\n==== end of watchdog dump ====\n\n");
  std::fflush(f);
}

}  // namespace rex::watchdog
