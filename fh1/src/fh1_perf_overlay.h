// fh1 - on-screen performance monitor (F3): renderer, FPS, frame time, 1% low and an FPS graph. Taken from
// nfsc-recomp (GoatHonks, GPL-3.0; his nfsc_perf_overlay, on F2 there). It shows rex::GetFrameStats(): the
// native renderer records its frames there itself; for the emulated GPU (Direct3D 12 and Vulkan) the app
// passes a function that copies the plugin's frame times into it before each draw.
#pragma once

#include <functional>
#include <utility>

#include <rex/ui/imgui_dialog.h>

namespace fh1 {

class PerfOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlay(rex::ui::ImGuiDrawer* drawer, std::function<void()> feed = {})
      : ImGuiDialog(drawer), feed_(std::move(feed)) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::function<void()> feed_;  // emulated GPU: brings the statistics up to date
};

}  // namespace fh1
