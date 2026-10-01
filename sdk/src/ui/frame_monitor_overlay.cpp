/**
 * @file        ui/frame_monitor_overlay.cpp
 * @brief       Lightweight frame monitor (see frame_monitor_overlay.h).
 */
#include <rex/ui/frame_monitor_overlay.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

namespace rex::ui {

namespace {

std::mutex g_provider_mutex;
std::function<bool(FrameMonitorContent&)> g_provider;
std::atomic<bool> g_enabled{false};

// 5x7 font, one byte per row, bit 4 = leftmost column. Lowercase letters use the uppercase glyphs.
struct Glyph {
  char c;
  uint8_t rows[7];
};
constexpr Glyph kGlyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
};

const uint8_t* FindGlyph(char c) {
  c = char(std::toupper(static_cast<unsigned char>(c)));
  for (const Glyph& g : kGlyphs) {
    if (g.c == c) return g.rows;
  }
  return nullptr;  // space and unknown characters: blank
}

}  // namespace

void SetFrameMonitorProvider(std::function<bool(FrameMonitorContent&)> provider) {
  std::lock_guard<std::mutex> lock(g_provider_mutex);
  g_provider = std::move(provider);
}

void SetFrameMonitorEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }

void ToggleFrameMonitor() { g_enabled.store(!g_enabled.load(), std::memory_order_relaxed); }

bool IsFrameMonitorEnabled() { return g_enabled.load(std::memory_order_relaxed); }

bool BuildFrameMonitorRects(uint32_t width, uint32_t height, FrameMonitorRects& out) {
  out.background.clear();
  out.text.clear();
  out.graph.clear();
  out.graph_slow.clear();
  if (!IsFrameMonitorEnabled() || !width || !height) return false;
  FrameMonitorContent content;
  {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    if (!g_provider || !g_provider(content)) return false;
  }
  // Font pixel size from the screen height: 2 px at 720p-1080p, 3 px at 1600p.
  const int32_t px = std::max<int32_t>(1, int32_t(height) / 540);
  const int32_t cell_w = 6 * px, line_h = 11 * px;
  const int32_t margin = 8 * px / 2, pad = 4 * px;
  size_t max_chars = 0;
  for (const std::string& line : content.lines) max_chars = std::max(max_chars, line.size());
  const int32_t graph_w = std::max<int32_t>(int32_t(max_chars) * cell_w, 240 * px / 2);
  const int32_t graph_h = content.history_ms.empty() ? 0 : 24 * px;
  const int32_t box_w = graph_w + 2 * pad;
  const int32_t box_h =
      int32_t(content.lines.size()) * line_h + (graph_h ? graph_h + pad : 0) + 2 * pad - px * 2;
  out.background.push_back({margin, margin, margin + box_w, margin + box_h});

  int32_t y = margin + pad;
  for (const std::string& line : content.lines) {
    int32_t x = margin + pad;
    for (char c : line) {
      if (const uint8_t* rows = FindGlyph(c)) {
        for (int32_t r = 0; r < 7; ++r) {
          // Horizontal runs of lit pixels as one rectangle each.
          int32_t run_start = -1;
          for (int32_t col = 0; col <= 5; ++col) {
            bool lit = col < 5 && (rows[r] >> (4 - col)) & 1;
            if (lit && run_start < 0) run_start = col;
            if (!lit && run_start >= 0) {
              out.text.push_back({x + run_start * px, y + r * px, x + col * px, y + (r + 1) * px});
              run_start = -1;
            }
          }
        }
      }
      x += cell_w;
    }
    y += line_h;
  }

  if (graph_h) {
    const int32_t gx = margin + pad, gy = y;
    const size_t n = content.history_ms.size();
    double sum = 0;
    for (float v : content.history_ms) sum += v;
    const float slow = float(sum / double(n)) * 1.5f;
    for (size_t i = 0; i < n; ++i) {
      int32_t x0 = gx + int32_t(i * size_t(graph_w) / n);
      int32_t x1 = std::max(x0 + 1, gx + int32_t((i + 1) * size_t(graph_w) / n) - 1);
      float v = std::clamp(content.history_ms[i] / content.graph_max_ms, 0.0f, 1.0f);
      int32_t bar = std::max<int32_t>(1, int32_t(v * float(graph_h)));
      FrameMonitorRects::Rect rect{x0, gy + graph_h - bar, x1, gy + graph_h};
      (content.history_ms[i] > slow ? out.graph_slow : out.graph).push_back(rect);
    }
  }
  return true;
}

}  // namespace rex::ui
