// fh1 - Direct3D census report: with --fh1_d3d_census, every 10 s (on the Swap call) log the
// functions of the game's D3D ranges called most per frame, with their last caller. Used to map
// the game's Direct3D for the native renderer (ROADMAP Stage 3, phase A).

#include "fh1_d3d_census.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fh1_d3d_census, false, "FH1",
                    "Debug: count the calls of every function in the game's Direct3D ranges and "
                    "log the most called per frame every 10 s (native renderer mapping)");
REXCVAR_DEFINE_INT32(fh1_d3d_census_top, 80, "FH1", "Debug: functions listed per census report");

namespace fh1::census {

std::atomic<bool> g_on{false};

namespace {
std::atomic<bool> g_started{false};
std::atomic<uint32_t> g_last_caller[4096];
uint32_t g_frames = 0;  // touched only by the Swap caller (the game's render thread)
std::chrono::steady_clock::time_point g_window_start;
std::atomic<bool> g_reporting{false};

void Report() {
  uint32_t frames = std::max<uint32_t>(1, g_frames);
  std::vector<std::pair<uint32_t, uint32_t>> rows;  // calls, index
  for (uint32_t i = 0; i < kCount; ++i) {
    uint32_t n = g_calls[i].exchange(0, std::memory_order_relaxed);
    if (n) rows.push_back({n, i});
  }
  std::sort(rows.rbegin(), rows.rend());
  int top = REXCVAR_GET(fh1_d3d_census_top);
  std::string s;
  for (int i = 0; i < int(rows.size()) && i < top; ++i) {
    uint32_t idx = rows[i].second;
    s += fmt::format("\n    {:9.2f}/frame  sub_{:08X}  (last caller {:08X})",
                     double(rows[i].first) / frames, kAddresses[idx],
                     idx < 4096 ? g_last_caller[idx].load(std::memory_order_relaxed) : 0);
  }
  REXLOG_INFO("[census] {} frames, {} functions called:{}", g_frames, rows.size(), s);
  g_frames = 0;
}
}  // namespace

void Count(uint32_t index, PPCContext& ctx) {
  g_calls[index].fetch_add(1, std::memory_order_relaxed);
  if (index < 4096) g_last_caller[index].store(uint32_t(ctx.lr), std::memory_order_relaxed);
  if (int32_t(index) != kSwapIndex) return;
  auto now = std::chrono::steady_clock::now();
  if (!g_started.exchange(true)) g_window_start = now;
  ++g_frames;
  if (now - g_window_start >= std::chrono::seconds(10)) {
    g_window_start = now;
    Report();
  }
}

// Turned on from the app once the cvars are parsed (fh1_app.h).
void Start() {
  if (REXCVAR_GET(fh1_d3d_census)) {
    g_on.store(true);
    REXLOG_INFO("[census] counting {} Direct3D-range functions", kCount);
  }
}

}  // namespace fh1::census
