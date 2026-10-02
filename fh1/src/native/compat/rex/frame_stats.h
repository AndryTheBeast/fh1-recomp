/**
 * @file        frame_stats.h
 * @brief       Presented-frame statistics shared between the GPU plugin (writer) and the app's on-screen monitor
 *              (reader). nfsc-recomp addition.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace rex {

constexpr int kFrameHistory = 120;

struct FrameStatsSnapshot {
  std::string renderer;   // "D3D12" / "Vulkan"
  uint64_t frames = 0;    // frames presented since launch
  double last_ms = 0;     // the last presented frame
  // Over the last 10 seconds (refreshed 4 times a second):
  double avg_fps = 0;
  double avg_ms = 0;
  double low1_fps = 0;    // "1% low": speed of the slowest 1% of the frames
  double low1_ms = 0;
  double worst_ms = 0;    // the single slowest frame
  // The last kFrameHistory frame times, oldest first, for the graph.
  std::array<float, kFrameHistory> history{};
};

/// Called by the command processor once per presented frame with the time since the previous one.
void RecordPresentedFrame(double frame_ms);
void SetRendererName(const std::string& name);
FrameStatsSnapshot GetFrameStats();

}  // namespace rex
