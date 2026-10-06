// fh1 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "fh1_autoplay.h"
#include "fh1_crash_report.h"
#include "fh1_native_system.h"
#include "fh1_prepare_screen.h"

#include <atomic>
#include <chrono>
#include <thread>

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

// Native renderer: the "Preparing shaders" screen (fh1_prepare_screen.h) normally shows only while the pipeline
// list is being built, and not at all when that takes under half a second. This keeps it up for at least N
// seconds, to look at it on a PC whose driver already has every pipeline.
REXCVAR_DEFINE_INT32(fh1_native_prepare_screen_min_s, 0, "FH1",
                     "Native renderer: keep the 'Preparing shaders' screen for at least this many seconds (test)");

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
  // By default Forza Horizon draws through the SDK's Xbox 360 GPU emulation (the native renderer
  // is opt-in). The SDK defaults to none because nfsmw-nx renders natively.
  // --gpu_plugin=<name> on the command line still wins.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // --fh1_renderer=native: FH1's native Vulkan renderer (src/native, docs/native-renderer-fh1.md) goes in
    // config.graphics, and ReXApp then does not load the GPU plugin.
    if (fh1::native::Active()) {
      config.graphics = fh1::native::CreateSystemGraphics();
    }
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    // FH1 reads render-to-texture results on the CPU: its auto exposure (without it a wrong
    // exposure made car lights bloom into white streaks in the garage) and the car photos it
    // saves when you buy or repaint a car (they were saved as garbage). "fast" = the previous
    // frame's copy, every frame: the photo is rendered over several frames, and "some" only
    // copied a destination's first resolve (the Subaru repaint photo was saved half stale).
    // Same frame rate as "some" (27.7 vs 27.6 fps); "full" stalls. --readback_resolve still wins.
    // (SetFlagAppDefault, not SetFlagByName: the flag belongs to the GPU plugin, which is loaded
    // after this hook - SetFlagByName was silently rejected and readback stayed "none", so car
    // photos were saved as garbage again.)
    rex::cvar::SetFlagAppDefault("readback_resolve", "fast");
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
  /*
   * Native renderer: the pipeline list is built before the game's code starts.
   *
   * A pipeline the graphics driver has not compiled yet costs 40-170 ms, and compiled while the game runs that
   * is a hitch; one frame of about 3.2 s and the game stops sending commands for good. Before the game runs
   * there is nothing to freeze, so the wait happens here: the renderer starts its list (this PC's and the one
   * shipped with the port), a thread watches it, the window shows "Preparing shaders n / total" if it lasts more
   * than half a second, and the game is launched when the list has ended (0.2 s when the driver has everything,
   * about 20 s on a first start of the Legion Go). The screen is removed before launching: a registered dialog
   * makes the SDK present through the UI thread, which costs frames in play.
   */
  void LaunchModule() override {
    if (!fh1::native::Active() || !fh1::native::PrewarmBeforeLaunch()) {
      rex::ReXApp::LaunchModule();
      return;
    }
    prepare_thread_ = std::thread([this] {
      using Clock = std::chrono::steady_clock;
      const auto start = Clock::now();
      const double minimum_s = double(REXCVAR_GET(fh1_native_prepare_screen_min_s));
      bool shown = false;
      // At most ten minutes: whatever happens to the list, the game starts.
      while (!prepare_stop_.load(std::memory_order_acquire)) {
        uint32_t done = 0;
        uint32_t total = 0;
        bool finished = false;
        fh1::native::PrewarmProgress(done, total, finished);
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        if ((finished && seconds >= minimum_s) || seconds >= 600.0) {
          REXLOG_INFO("[prepare] pipeline list {} after {:.1f} s ({} of {}); screen shown: {}",
                      finished ? "ended" : "NOT ended, launching anyway", seconds, done, total, shown ? "yes" : "no");
          break;
        }
        if (!shown && seconds >= (minimum_s > 0.0 ? 0.0 : 0.5)) {
          shown = true;
          app_context().CallInUIThreadDeferred([this] {
            if (imgui_drawer() && !prepare_stop_.load(std::memory_order_acquire)) {
              prepare_screen_ = std::make_unique<Fh1PrepareScreen>(imgui_drawer());
            }
          });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      if (prepare_stop_.load(std::memory_order_acquire)) {
        return;  // the window was closed while preparing
      }
      app_context().CallInUIThreadDeferred([this] {
        prepare_screen_.reset();
        rex::ReXApp::LaunchModule();
      });
    });
  }
  void OnShutdown() override {
    prepare_stop_.store(true, std::memory_order_release);
    if (prepare_thread_.joinable()) {
      prepare_thread_.join();
    }
    prepare_screen_.reset();
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}

 private:
  std::thread prepare_thread_;
  std::atomic<bool> prepare_stop_{false};
  std::unique_ptr<Fh1PrepareScreen> prepare_screen_;  // UI thread only
};
