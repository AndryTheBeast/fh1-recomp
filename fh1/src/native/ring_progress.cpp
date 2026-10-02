/**
 * @file        ring_progress.cpp
 * @brief       See rex/ring_progress.h. fh1: copied from nfsc-recomp's SDK (sdk/src/core), gpu_timing removed.
 *               Based on nfsmw-nx's app/src/nfsmw_espera_anillo.cpp (StevensND, GPL-3.0).
 */

#include <rex/ring_progress.h>
#include <rex/frame_stats.h>

#include <algorithm>
#include <functional>

#include <atomic>
#include <condition_variable>
#include <array>
#include <mutex>
#include <string>

namespace rex {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

uint32_t RingProgress() {
  return g_progress.load(std::memory_order_acquire);
}

// Order: the producer writes the read pointer, bumps the counter and only then checks whether anyone is
// waiting. The waiter reads the counter before looking at the pointer, registers itself and checks the counter
// under the lock: no notification is lost between checking and falling asleep. "Is anyone waiting" is read
// with the lock held: without it the two sides may not see each other's writes (Dekker handshake), which on
// ARM and on x86 without lock-prefixed stores can lose a wakeup.
void NotifyRingProgress() {
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) {
    g_cv.notify_all();
  }
}

bool WaitRingProgress(uint32_t seen, std::chrono::microseconds limit) {
  bool advanced = false;
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_waiting.fetch_add(1, std::memory_order_acq_rel);
    advanced = g_cv.wait_for(lock, limit,
                             [seen] { return g_progress.load(std::memory_order_acquire) != seen; });
    g_waiting.fetch_sub(1, std::memory_order_acq_rel);
  }
  return advanced;
}


// ---- presented-frame statistics (frame_stats.h) ----
namespace {
std::mutex g_stats_mutex;
constexpr int kLong = 1500;  // enough frames for 10 s even at 144 FPS
std::array<float, kLong> g_long{};  // ring of the last frame times
int g_long_pos = 0;
uint64_t g_frames = 0;
std::string g_renderer = "?";
// The 10-second statistics need a sort; refresh them 4 times a second, not on every UI frame.
FrameStatsSnapshot g_cached;
std::chrono::steady_clock::time_point g_cached_at;
}  // namespace

void RecordPresentedFrame(double frame_ms) {
  std::lock_guard<std::mutex> lock(g_stats_mutex);
  g_long[g_long_pos] = float(frame_ms);
  g_long_pos = (g_long_pos + 1) % kLong;
  ++g_frames;
}

void SetRendererName(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_stats_mutex);
  g_renderer = name;
}

FrameStatsSnapshot GetFrameStats() {
  std::lock_guard<std::mutex> lock(g_stats_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (g_frames == 0) {
    FrameStatsSnapshot empty;
    empty.renderer = g_renderer;
    return empty;
  }
  const bool refresh = now - g_cached_at > std::chrono::milliseconds(250);
  if (refresh) {
    g_cached_at = now;
    // The last 10 seconds: walk back through the frame times until 10 s are covered.
    std::array<float, kLong> window;
    int n = 0;
    double covered = 0;
    for (int i = 1; i <= kLong && uint64_t(i) <= g_frames; ++i) {
      const float v = g_long[(g_long_pos + kLong - i) % kLong];
      window[n++] = v;
      covered += v;
      if (covered >= 10000.0) break;
    }
    g_cached.avg_ms = n ? covered / n : 0.0;
    g_cached.avg_fps = g_cached.avg_ms > 0 ? 1000.0 / g_cached.avg_ms : 0.0;
    const int slow = std::max(1, n / 100);
    std::nth_element(window.begin(), window.begin() + (slow - 1), window.begin() + n, std::greater<float>());
    double slow_sum = 0;
    float worst = 0;
    for (int i = 0; i < slow; ++i) {
      slow_sum += window[i];
      worst = std::max(worst, window[i]);
    }
    g_cached.low1_ms = slow_sum / slow;
    g_cached.low1_fps = g_cached.low1_ms > 0 ? 1000.0 / g_cached.low1_ms : 0.0;
    g_cached.worst_ms = worst;
  }
  FrameStatsSnapshot out = g_cached;
  out.renderer = g_renderer;
  out.frames = g_frames;
  out.last_ms = g_long[(g_long_pos + kLong - 1) % kLong];
  for (int i = 0; i < kFrameHistory; ++i) {  // oldest first
    out.history[i] = g_long[(g_long_pos + kLong - kFrameHistory + i) % kLong];
  }
  return out;
}


}  // namespace rex
