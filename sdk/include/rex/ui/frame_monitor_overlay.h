/**
 * @file        ui/frame_monitor_overlay.h
 * @brief       Lightweight frame monitor drawn by the presenter on top of the guest output.
 *
 * Unlike an ImGui dialog (whose mere existence moves presentation to the UI thread and caused
 * 100+ ms hitches in Forza Horizon), this is drawn inside the normal guest output paint as a list of
 * filled rectangles: a 5x7 pixel font, a background box and a frame time graph. F3 toggles it,
 * --show_frame_monitor opens it at startup.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::ui {

struct FrameMonitorContent {
  std::vector<std::string> lines;
  std::vector<float> history_ms;  // oldest first
  float graph_max_ms = 66.7f;
};

struct FrameMonitorRects {
  struct Rect {
    int32_t left, top, right, bottom;
  };
  std::vector<Rect> background;
  std::vector<Rect> text;
  std::vector<Rect> graph;
  std::vector<Rect> graph_slow;  // frames slower than 1.5x the average
};

// Content source, called from the presenting thread.
void SetFrameMonitorProvider(std::function<bool(FrameMonitorContent&)> provider);
void SetFrameMonitorEnabled(bool enabled);
void ToggleFrameMonitor();
bool IsFrameMonitorEnabled();
// Builds the rectangles for a back buffer of the given size; false if disabled or no content.
bool BuildFrameMonitorRects(uint32_t width, uint32_t height, FrameMonitorRects& out);

}  // namespace rex::ui
