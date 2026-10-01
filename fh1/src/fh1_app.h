// fh1 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "fh1_autoplay.h"
#include "fh1_crash_report.h"

void Fh1StartProfiler();  // fh1_profiler.cpp
namespace fh1::census { void Start(); }  // fh1_d3d_census_report.cpp

// Forza Horizon is single player. By default every controller drives player 1: tools like
// DSX / DS4Windows show one pad twice (the real one and a virtual Xbox 360 pad), and with
// the SDK's one-device-per-slot routing Start could land on player 2, who has no profile
// ("No Gamer Profiles"). --fh1_merge_controllers=false restores one controller per player.
REXCVAR_DEFINE_BOOL(fh1_merge_controllers, true, "FH1",
                    "Every controller drives player 1 (single player)");

// Scripted virtual controller for unattended tests (tools/auto_test.ps1); format in
// fh1_autoplay.h. Empty = off. Needs fh1_merge_controllers so it drives player 1.
REXCVAR_DEFINE_STRING(fh1_autoplay, "", "FH1",
                      "Scripted input: START+DURATION=CONTROLS;... (seconds from launch)");

class Fh1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fh1App>(new Fh1App(ctx, "fh1",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override {
    fh1::InstallCrashReport();
    Fh1StartProfiler();  // --fh1_profile=N (fh1_profiler.cpp)
    fh1::census::Start();  // --fh1_d3d_census (Direct3D mapping)
  }
  // Forza Horizon has no native renderer (yet): draw through the SDK's Xbox 360
  // GPU emulation. The SDK defaults to none because nfsmw-nx renders natively.
  // --gpu_plugin=<name> on the command line still wins.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    // FH1 reads render-to-texture results on the CPU: its auto exposure (without it a wrong
    // exposure made car lights bloom into white streaks in the garage) and the car photos it
    // saves when you buy or repaint a car (they were saved as garbage). "fast" = the previous
    // frame's copy, every frame: the photo is rendered over several frames, and "some" only
    // copied a destination's first resolve (the Subaru repaint photo was saved half stale).
    // Same frame rate as "some" (27.7 vs 27.6 fps); "full" stalls. --readback_resolve still wins.
    if (!rex::cvar::HasNonDefaultValue("readback_resolve")) {
      rex::cvar::SetFlagByName("readback_resolve", "fast");
    }
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
    auto* input = dynamic_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (!input) return;
    if (auto autoplay = fh1::CreateAutoplayDriver(REXCVAR_GET(fh1_autoplay))) {
      input->AddDriver(std::move(autoplay));
    }
    if (!REXCVAR_GET(fh1_merge_controllers)) return;
    input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
