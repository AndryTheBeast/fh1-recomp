// fh1 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "fh1_crash_report.h"

// Forza Horizon is single player. By default every controller drives player 1: tools like
// DSX / DS4Windows show one pad twice (the real one and a virtual Xbox 360 pad), and with
// the SDK's one-device-per-slot routing Start could land on player 2, who has no profile
// ("No Gamer Profiles"). --fh1_merge_controllers=false restores one controller per player.
REXCVAR_DEFINE_BOOL(fh1_merge_controllers, true, "FH1",
                    "Every controller drives player 1 (single player)");

class Fh1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fh1App>(new Fh1App(ctx, "fh1",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override { fh1::InstallCrashReport(); }
  // Forza Horizon has no native renderer (yet): draw through the SDK's Xbox 360
  // GPU emulation. The SDK defaults to none because nfsmw-nx renders natively.
  // --gpu_plugin=<name> on the command line still wins.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
    if (!REXCVAR_GET(fh1_merge_controllers)) return;
    auto* input = dynamic_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (input) {
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
    }
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
