// fh1 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/frame_stats.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/keybinds.h>

#include "fh1_autoplay.h"
#include "fh1_crash_report.h"
#include "fh1_native_system.h"
#include "fh1_perf_overlay.h"
#include "fh1_prepare_screen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

void Fh1StartProfiler();  // fh1_profiler.cpp
void Fh1UnpackImageIfAsked(const uint8_t* image);  // fh1_unpack_image.cpp
void Fh1Fps60Apply();  // fh1_fps60.cpp
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

// The Windows mouse pointer over the game window: hidden after a second without moving, back as soon as the
// mouse moves (the SDK's auto-hide mode), so the F4 settings window can still be clicked.
REXCVAR_DEFINE_BOOL(fh1_hide_cursor, true, "FH1",
                    "Hide the mouse pointer over the game window while the mouse is not moving");

// F3: the FPS / frame time monitor taken from nfsc-recomp (fh1_perf_overlay.h), on the native renderer and the
// emulated GPU's Vulkan backend (the emulated Direct3D 12 keeps the SDK's monitor, --show_frame_monitor).
REXCVAR_DEFINE_BOOL(fh1_perf_overlay, false, "FH1",
                    "Native renderer and emulated Vulkan: show the FPS / frame time monitor from the start (F3 "
                    "toggles it in game)");

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
  // By default Forza Horizon draws with its native Vulkan renderer (since 2026-10-06); --fh1_renderer=xenos, or
  // a missing shader library, selects the SDK's Xbox 360 GPU emulation.
  // --gpu_plugin=<name> on the command line still wins.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // --fh1_renderer=native: FH1's native Vulkan renderer (src/native, docs/native-renderer-fh1.md) goes in
    // config.graphics, and ReXApp then does not load the GPU plugin.
    Fh1Fps60Apply();  // --fh1_fps60: before the graphics system reads the video mode
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
  // --fh1_unpack_image=<file> (the installer): the loaded image is written and the program ends here.
  void OnPostLoadXexImage() override {
    if (runtime() && runtime()->virtual_membase()) {
      Fh1UnpackImageIfAsked(runtime()->virtual_membase() + 0x82000000u);
    }
  }
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
  // The window exists from here on (OnPostSetup runs before it is created).
  void OnPreLaunchModule() override {
    if (window() && REXCVAR_GET(fh1_hide_cursor)) {
      window()->SetCursorAutoHideDelayMs(1000);
      window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
    }
  }
  // Runs right after the SDK has bound its own keys: F3 opens the monitor taken from nfsc-recomp instead of
  // the SDK's small one, on the native renderer and on the emulated GPU's Vulkan backend. The emulated
  // Direct3D 12 keeps the SDK's (user, 2026-10-06): an ImGui window makes that backend present through the UI
  // thread, and the festival fell from 30 to about 25.6 fps with this monitor open.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (!drawer) return;
    if (!fh1::native::Active() && rex::cvar::GetFlagByName("gpu_backend") != "vulkan") return;
    rex::ui::UnregisterBind("bind_debug_overlay");
    rex::ui::RegisterBind("bind_fh1_perf", "F3", "Toggle the FPS / frame time monitor", [this] {
      if (perf_overlay_) perf_overlay_.reset();
      else OpenPerfOverlay();
    });
    if (REXCVAR_GET(fh1_perf_overlay)) OpenPerfOverlay();
  }
  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_fh1_perf");
    perf_overlay_.reset();
    prepare_stop_.store(true, std::memory_order_release);
    if (prepare_thread_.joinable()) {
      prepare_thread_.join();
    }
    prepare_screen_.reset();
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // An installed game (the installer's layout): the disc's files are in the folder "game" next to fh1.exe, so
  // a double click on fh1.exe starts the game. --game_data_root still wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    const std::filesystem::path game = paths.config_path.parent_path() / "game";
    std::error_code ec;
    if (std::filesystem::is_regular_file(game / "default.xex", ec)) {
      paths.game_data_root = game;
    }
  }

 private:
  // The native renderer records its presented frames in rex::GetFrameStats() itself. The emulated GPU keeps its
  // frame times inside its plugin (the SDK's frame monitor): the frames presented since the last look are
  // copied over, at most the 120 the plugin hands out.
  void OpenPerfOverlay() {
    if (!imgui_drawer()) return;
    if (fh1::native::Active()) {
      perf_overlay_ = std::make_unique<fh1::PerfOverlay>(imgui_drawer());
      return;
    }
    perf_frames_seen_ = 0;
    perf_overlay_ = std::make_unique<fh1::PerfOverlay>(imgui_drawer(), [this] {
      auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
      rex::system::FrameMonitorStats stats;
      if (!graphics || !graphics->GetFrameMonitorStats(stats)) return;
      constexpr uint64_t kHistory = rex::system::FrameMonitorStats::kHistory;
      if (perf_frames_seen_ == 0) {
        rex::SetRendererName(stats.renderer);
        // The first look: only what the plugin has really measured (the first swap has no frame time).
        perf_frames_seen_ = stats.total_frames > kHistory ? stats.total_frames - kHistory : 1;
      }
      if (stats.total_frames <= perf_frames_seen_) return;
      const uint64_t fresh = std::min(stats.total_frames - perf_frames_seen_, kHistory);
      for (uint64_t i = kHistory - fresh; i < kHistory; ++i) {
        rex::RecordPresentedFrame(double(stats.history_ms[i]));
      }
      perf_frames_seen_ = stats.total_frames;
    });
  }

  uint64_t perf_frames_seen_ = 0;  // UI thread only
  std::thread prepare_thread_;
  std::atomic<bool> prepare_stop_{false};
  std::unique_ptr<Fh1PrepareScreen> prepare_screen_;  // UI thread only
  std::unique_ptr<fh1::PerfOverlay> perf_overlay_;  // UI thread only
};
