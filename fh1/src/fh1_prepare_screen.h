// fh1 - "Preparing shaders": the progress display of the native renderer's pipeline list.
//
// The list (this PC's and the one shipped with the port, fh1_pipelines.nfpl) is built before the game's code
// starts (Fh1App::LaunchModule). On a PC whose graphics driver already has the pipelines that takes 0.2 s and
// this screen is never created; on a first start, or after a driver update, it takes tens of seconds and this
// is what the window shows meanwhile. It exists only for that time: while any dialog is registered the SDK
// presents through the UI thread, which costs frames once the game runs.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "fh1_native_system.h"

class Fh1PrepareScreen final : public rex::ui::ImGuiDialog {
 public:
  explicit Fh1PrepareScreen(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    uint32_t done = 0;
    uint32_t total = 0;
    bool finished = false;
    fh1::native::PrewarmProgress(done, total, finished);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::clamp(io.DisplaySize.x * 0.5f, 320.0f, 900.0f), 0.0f), ImGuiCond_Always);
    if (ImGui::Begin("##fh1_prepare", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImGui::TextUnformatted("Preparing shaders");
      char text[48];
      std::snprintf(text, sizeof(text), "%u / %u", done, total);
      ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f, ImVec2(-1.0f, 0.0f), text);
      ImGui::TextUnformatted("Only on the first start and after a graphics driver update.");
    }
    ImGui::End();
  }
};
