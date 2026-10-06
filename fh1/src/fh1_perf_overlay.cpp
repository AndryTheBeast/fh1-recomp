// fh1 - on-screen performance monitor, see fh1_perf_overlay.h. Taken from nfsc-recomp (GoatHonks, GPL-3.0). Data
// comes from rex::GetFrameStats(), which the native renderer updates once per presented frame (the emulated GPU's
// frames are copied into it by feed_). It does no per-frame work beyond a small copy and 120 graph points.
// FH1: the game runs at 30 FPS (its own cap), so the graph goes from 15 (bottom) to 30 FPS (top) where Carbon's
// goes from 30 to 60; the colours are his (red at 0, blue at 30, green at 60).

#include "fh1_perf_overlay.h"

#include <algorithm>

#include <imgui.h>

#include <rex/frame_stats.h>

namespace fh1 {
namespace {

constexpr ImVec4 kMuted(0.62f, 0.66f, 0.72f, 1.0f);
constexpr ImVec4 kGood(0.40f, 1.00f, 0.45f, 1.0f);     // 60 FPS
constexpr ImVec4 kConsole(0.45f, 0.75f, 1.00f, 1.0f);  // 30 FPS: the Xbox 360's own rate and the game's cap
constexpr ImVec4 kBad(1.00f, 0.42f, 0.42f, 1.0f);      // 0 FPS

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}

// A smooth gradient: red at 0 FPS to blue at 30, then blue at 30 to green at 60 (and above).
ImVec4 FpsColor(double fps) {
  const float f = std::clamp(float(fps), 0.0f, 60.0f);
  return f < 30.0f ? Mix(kBad, kConsole, f / 30.0f) : Mix(kConsole, kGood, (f - 30.0f) / 30.0f);
}

// One row of the table: a muted label, then the value (and its frame time) in a colour that says how good it is.
void Row(const char* label, double fps, double ms) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextColored(kMuted, "%s", label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextColored(FpsColor(fps), "%5.1f FPS", fps);
  ImGui::TableSetColumnIndex(2);
  ImGui::TextColored(kMuted, "%5.1f ms", ms);
}

}  // namespace

void PerfOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  if (feed_) feed_();
  const rex::FrameStatsSnapshot st = rex::GetFrameStats();
  ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.60f);
  // Only shows data: no navigation, no focus, so it never takes the controller away from the game.
  if (!ImGui::Begin("##fh1_perf", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNavInputs |
                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  // Header: the renderer on the left, how many frames have been shown on the right.
  ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.00f, 1.0f), "%s", st.renderer.c_str());
  ImGui::SameLine(0, 24);
  ImGui::TextColored(kMuted, "frame %llu", (unsigned long long)st.frames);

  if (st.frames < 2) {
    ImGui::TextUnformatted("waiting for frames...");
    ImGui::End();
    return;
  }

  // Big number: what the last frame ran at.
  const double now_fps = st.last_ms > 0 ? 1000.0 / st.last_ms : 0.0;
  ImGui::SetWindowFontScale(1.5f);
  ImGui::TextColored(FpsColor(now_fps), "%.0f FPS", now_fps);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::SameLine(0, 12);
  ImGui::TextColored(kMuted, "%.1f ms", st.last_ms);

  ImGui::Separator();
  ImGui::TextColored(kMuted, "last 10 seconds");
  if (ImGui::BeginTable("##stats", 3, ImGuiTableFlags_SizingFixedFit)) {
    Row("average", st.avg_fps, st.avg_ms);
    Row("1% low", st.low1_fps, st.low1_ms);
    Row("worst", st.worst_ms > 0 ? 1000.0 / st.worst_ms : 0.0, st.worst_ms);
    ImGui::EndTable();
  }

  // Graph of the last 120 frames in FPS: the top edge is 30 FPS (the game's cap) and the bottom edge 15 FPS
  // (faster or slower frames stay on the edge).
  constexpr float kTop = 30.0f;
  constexpr float kBottom = 15.0f;
  float fps[rex::kFrameHistory];
  for (int i = 0; i < rex::kFrameHistory; ++i) {
    const float ms = st.history[size_t(i)];
    fps[i] = ms > 0.0f ? std::clamp(1000.0f / ms, kBottom, kTop) : kBottom;
  }
  const ImVec2 size(240, 52);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::PlotLines("##fps", fps, rex::kFrameHistory, 0, nullptr, kBottom, kTop, size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y), IM_COL32(115, 190, 255, 140));
  dl->AddLine(ImVec2(origin.x, origin.y + size.y - 1), ImVec2(origin.x + size.x, origin.y + size.y - 1),
              IM_COL32(255, 150, 130, 140));
  ImGui::TextColored(kMuted, "graph: last 120 frames,");
  ImGui::SameLine(0, 4);
  ImGui::TextColored(kConsole, "top 30 FPS");
  ImGui::SameLine(0, 4);
  ImGui::TextColored(kMuted, "/");
  ImGui::SameLine(0, 4);
  ImGui::TextColored(FpsColor(15.0), "bottom 15 FPS");
  ImGui::End();
}

}  // namespace fh1
