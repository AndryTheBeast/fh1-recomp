// nfsmw - graphics settings in the menu (F4) that work with the native renderer (see nfsmw_graphics_settings.h).

#include "nfsmw_graphics_settings.h"

#include "nfsmw_native_system.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/settings_overlay.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(nfsmw_internal_resolution, "automatico", "Graphics",
                      "Resolution the game draws at. auto: 1920x1080 docked and 1280x720 handheld, switching on "
                      "the fly when the console goes in and out of the dock AND also following Reverse-NX. "
                      "Careful: with Reverse-NX the clocks do not go up, so there 1080p costs a third of the FPS. "
                      "1280x720 is the Xbox 360's. 1920x1080 always forces it. 1024x576 is the game's own "
                      "lowest-resolution mode: 36 % fewer pixels (this one needs a restart). 640x360 and 640x480 "
                      "go lower, but CAREFUL: measured on 20/09, this does NOT change the resolution the game "
                      "DRAWS at -it stays at 1280x720- only the size it is shrunk to when resolving. What it saves "
                      "is the post-processing, the copies and the cube; the scene does not move. From 1280x720 to "
                      "1024x576 is 2.14 real ms and the post-processing already drops to 0.03, so below that there "
                      "is little left to gain and the scaling does show")
    .allowed({"automatico", "1280x720", "1920x1080", "1024x576", "640x360", "640x480"});

/*
 * GPU MHz in handheld mode.
 *
 * The console starts games at 307.2 MHz. Nintendo later made a 460.8 MHz profile available for
 * the games that need it, and this one does: 25 FPS with the GPU at its limit. This is not an
 * overclock (no KIP or sysmodule is touched): it is requested through the official API (apm), the
 * same one any commercial game uses. Not every game requests it because the ones that do not
 * need it get longer battery life.
 *
 * 307.2 -> 460.8 is +50 % GPU. It costs battery and heat; with 0 the console stays
 * exactly as it was.
 *
 * How to check that it worked: the profiler reports the real frequency in every report
 * ("clocks: CPU 1020.0 MHz, GPU 307.2 MHz, ..."). If it still says 307.2, it did not take effect.
 */
REXCVAR_DEFINE_INT32(nfsmw_switch_gpu_mhz, 460, "Graphics",
                     "Switch: GPU MHz requested from the system in handheld mode (0 = touch nothing, 384, 460). "
                     "460.8 MHz is an official Horizon profile, not an overclock; it uses more battery and runs "
                     "hotter")
    .allowed({"0", "384", "460"});

/*
 * The trade-off, just in case.
 *
 * The high GPU configurations come in two flavors: memory at 1331.2 (0x92220008) and memory at
 * 1600 (0x92220007). We want the first, because raising the RAM clock costs battery and heat
 * without giving anything back: our measurements put bandwidth at 27 %, and what saturates is the
 * fragment ALU. If this firmware lacks it, the default is to leave the GPU as it was rather than
 * raise the RAM clock. With this set to true the trade-off is accepted: high GPU with RAM at 1600.
 */
REXCVAR_DEFINE_BOOL(nfsmw_switch_ram_1600, false, "Graphics",
                    "Switch: accept the high GPU profile even if it raises the memory to 1600 MHz. NO by default: "
                    "if none leaves the RAM where it was, the GPU stays as it was. The [apm] log says which one "
                    "was applied");

REXCVAR_DEFINE_BOOL(nfsmw_switch_saltynx, true, "Graphics",
                    "Switch: publishes the FPS and the resolution where the console overlays read them (SaltyNX) "
                    "and follows the handheld/docked mode of Reverse-NX. Without SaltyNX it does nothing");

REXCVAR_DEFINE_BOOL(nfsmw_switch_reverse_nx_clocks, false, "Graphics",
                    "Switch: make the clocks follow Reverse-NX when it does not match the hardware, in BOTH "
                    "directions, by asking the sysmodule (Horizon OC and other sys-clk forks). If it fakes DOCKED "
                    "with the console in your hands, YOUR docked column is applied (the game profile's or the "
                    "global one): that is an overclock and is no good for measuring. If it fakes HANDHELD with the "
                    "console in the dock, it drops to the handheld column and, if that is empty, to the stock "
                    "clocks (GPU 307.2 MHz and memory 1331.2). It is released when they match again and on exit");
/*
 * The rate it really ticks at, measured on the console (race, no overclock).
 *
 * nfsmw_fps_limit ends up in video_mode_refresh_rate, and LoopVblank (nfsmw_native_system.cpp) reads
 * it from there to know how often to fire the guest interrupt. With "60" the vblank thread ticks at
 * exactly 60.1 Hz: 601 vblanks per 10 s of log, spot on in every interval.
 *
 * It neither throttles nor drops work, and both are measured, not assumed:
 *   - It does not throttle: the game runs at 24.5 swaps/s, far below 60. The vblanks/swaps ratio is
 *     2.45, which is not an integer; if the game limited itself by counting vblanks it would be 2.00
 *     or 3.00. The pace is set by the work, not by the vblank.
 *   - Nothing is dropped: "presented=6260 rejections=0" in the log, and the profiler checks that the
 *     game presents 25.35/s and 25.35/s reach the window (nwindowQueueBuffer), deviation +0.0 %. Not
 *     a single frame is lost in the output mailbox.
 * Conclusion: at 60 there is nothing to gain or lose here. Setting "30" would only lower the guest
 * interrupt to 30 Hz (0.25 % of a core) and tell the game that the panel runs at 30; as long as a
 * frame takes 39 ms nothing changes. The time missing to reach 33.3 ms is not in this setting: it is
 * in the PM4 ring thread spending 6.4-7.5 ms per frame inside presentation without recording the next
 * frame.
 */
REXCVAR_DEFINE_STRING(nfsmw_fps_limit, "60", "Graphics",
                      "Maximum game FPS. 60: no limit of its own (the game runs at the pace of a 60 Hz vblank). "
                      "30: a fixed 30 FPS pace, without ups and downs, with the game at its normal speed. Applied "
                      "on restart")
    .allowed({"60", "30"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * Pace fixed by hardware, not by software.
 *
 * nfsmw_fps_limit does not limit presentation: it tells the native renderer's vblank thread at
 * what rate to fire the guest interrupt, and the game limits itself by counting vblanks. Those are two
 * different free-running 30 Hz clocks (the process timer and the panel), and they beat against each
 * other: most frames at 33.3 ms and every few seconds one at 50. Micro-stutters with the counter
 * showing 30.
 *
 * This is the other approach: it asks the compositor for one image every N vblanks. With 2 there is a
 * single clock, the panel's, and its cadence propagates back through backpressure to the PM4 ring
 * thread. The WSI sets it to 1 when the swapchain is created, so it is reapplied on every presentation
 * (switch_perf.cpp).
 */
/*
 * The default is 0, and this matters.
 *
 * The console swapchain is created in IMMEDIATE mode, which in the Horizon WSI means
 * nwindowSetSwapInterval(nw, 0): the queue stops blocking the producer. That is what closes the
 * 6.3-7.8 ms gap in which the GPU sits idle while the PM4 ring thread is inside presentation
 * (measured on the console: frame = GPU work + gap, exactly).
 *
 * But NoteRitmo reapplies the interval on every presentation (on purpose, because the swapchain is
 * recreated when switching from docked to handheld mode). With the default at 2, or with a 1 in the
 * toml, the WSI's 0 was overwritten on the first frame and IMMEDIATE did nothing. With 0 it is left
 * alone and the swapchain mode applies, which is what we want.
 *
 * If the panel ever has to be locked again: 2 still works, but it only makes sense when the work
 * really fits in 33.3 ms. Because of the gap it does not fit, and with the gap closed interval 0
 * gives the same result without the risk of dropping to the next step.
 */
REXCVAR_DEFINE_UINT32(nfsmw_swap_interval, 0, "Graphics",
                      "Switch: vblanks between images. 0 = leave it alone, the swapchain mode decides (the normal "
                      "case since build 113: IMMEDIATE, that is interval 0, which removes the gap). 2 = 30 FPS "
                      "locked by the panel, only useful if the work fits in 33.3 ms. 1 = up to 60");

// Optional antialiasing on the output, applied live. The Xbox 360 draws the scene with 4x MSAA; the Switch
// has none (it costs GPU time and hung NVK in another port), and FXAA smooths the edges with little work.
REXCVAR_DEFINE_STRING(nfsmw_antialiasing, "off", "Graphics",
                      "Image antialiasing. off: as until now. fxaa: smooths jagged edges (the Xbox 360 uses 4x "
                      "MSAA, which is not available here); costs some GPU")
    .allowed({"off", "fxaa"});

// Sky glow, applied live. The game's bright pass (PS n137) subtracts the threshold per channel: with an
// intense blue sky only the blue channel passes and leaves blue edges over the trees. natural: the same
// glow energy with the hue of the source color. soft: the threshold is applied to luminance, with much
// less halo.
// natural is the default: compared on the console, original still showed a noticeable blue halo.
REXCVAR_DEFINE_STRING(nfsmw_sky_glow, "natural", "Graphics",
                      "Sky glow. natural (default): the game's glow without the saturated blue that leaves fringes "
                      "on trees and roofs; colored lights unchanged. original: like the Xbox 360, with those blue "
                      "fringes. soft: almost no sky halo; single-color lights (brakes, police) glow less. No cost")
    .allowed({"original", "natural", "soft"});

// Optional post-processing, applied live (visible when changed with the menu open). Option lists for the
// gamepad. The presets use the values from GoldenEye-Recomp (ge_postfx.cpp, public domain).
REXCVAR_DEFINE_STRING(nfsmw_postprocess, "off", "Graphics/Postprocess",
                      "Image post-processing. off: like the Xbox 360. Presets: cinema, sepia, noir, cold, warm, "
                      "vivid, matrix and crt. custom: the values below. Single-channel grading costs nothing; "
                      "saturation, vibrance, vignette and scanlines do some per-pixel work")
    .allowed({"off", "cine", "sepia", "noir", "frio", "calido", "live", "matrix", "crt", "personalizado"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_brightness, "0.00", "Graphics/Postprocess",
                      "Custom: brightness added (0.00 = no change)")
    .allowed({"-0.20", "-0.15", "-0.10", "-0.05", "0.00", "+0.05", "+0.10", "+0.15", "+0.20"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_contrast, "1.00", "Graphics/Postprocess",
                      "Custom: contrast around middle gray (1.00 = no change)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30", "1.40"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_saturation, "1.00", "Graphics/Postprocess",
                      "Custom: saturation (0.00 = black and white, 1.00 = no change)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00", "1.10", "1.20", "1.35", "1.50", "2.00"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_vibrance, "0.00", "Graphics/Postprocess",
                      "Custom: vibrance, raises the least saturated colors more (0.00 = no change)")
    .allowed({"-0.50", "-0.25", "0.00", "+0.15", "+0.25", "+0.50", "+0.75", "+1.00"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_temperature, "0.00", "Graphics/Postprocess",
                      "Custom: temperature, warm (+) or cold (-) (0.00 = no change)")
    .allowed({"-1.00", "-0.75", "-0.50", "-0.25", "0.00", "+0.25", "+0.50", "+0.75", "+1.00"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_gamma, "1.00", "Graphics/Postprocess",
                      "Custom: gamma, above 1 brightens the midtones (1.00 = no change)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_vignette, "0.00", "Graphics/Postprocess",
                      "Custom: vignette, darkens the edges (0.00 = no vignette)")
    .allowed({"0.00", "0.20", "0.30", "0.40", "0.55", "0.70", "1.00"});
REXCVAR_DEFINE_STRING(nfsmw_postprocess_scanlines, "0.00", "Graphics/Postprocess",
                      "Custom: old TV scanlines, one row in three (0.00 = no lines)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00"});

namespace nfsmw::settings {
namespace {

std::atomic<uint64_t> g_version_postprocess{1};
std::atomic<bool> g_fxaa{false};
std::atomic<int> g_glow_sky{0};  // 0 original, 1 natural, 2 soft

constexpr const char* kCvarsPostprocess[] = {
    "nfsmw_postprocess",           "nfsmw_postprocess_brightness",      "nfsmw_postprocess_contrast",
    "nfsmw_postprocess_saturation", "nfsmw_postprocess_vibrance",  "nfsmw_postprocess_temperature",
    "nfsmw_postprocess_gamma",     "nfsmw_postprocess_vignette",      "nfsmw_postprocess_scanlines",
};

// nfsmw_sky_glow: 0 original, 1 natural, 2 soft.
int ModeGlow(std::string_view input_value) {
  return input_value == "natural" ? 1 : input_value == "soft" ? 2 : 0;
}

float Number(const char* name, float by_default) {
  const std::string text = rex::cvar::GetFlagByName(name);
  char* fin = nullptr;
  const double input_value = std::strtod(text.c_str(), &fin);
  return fin && fin != text.c_str() ? float(input_value) : by_default;
}

// The command line takes precedence (PC tests with --video_mode_width=...). Whatever comes from the
// configuration file is replaced: SaveConfig saves the cvars that differ from their default value, and an
// old video mode must not win over the new settings.
void Set(const char* name, const std::string& input_value) {
  if (rex::cvar::GetFlagInfo(name) == nullptr) {
    REXLOG_WARN("[settings] the cvar {} does not exist: {} is not applied", name, input_value);
    return;
  }
  if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine) {
    REXLOG_INFO("[settings] {} comes from the command line ({}): left alone", name,
                rex::cvar::GetFlagByName(name));
    return;
  }
  if (!rex::cvar::SetFlagByName(name, input_value)) {
    REXLOG_WARN("[settings] could not set {} = {}", name, input_value);
  }
}

}  // namespace

#if REX_PLATFORM_SWITCH
// Provided by the SaltyNX module so that the console overlays can show the resolution.
extern "C" void RexSwitchPerfResolution(unsigned width, unsigned height);
extern "C" void RexSwitchPerfIntervalSwap(unsigned vblanks);
extern "C" void RexSwitchSaltyNxEnable(int enabled);
extern "C" void RexSwitchClocksReverseEnable(int enabled);
extern "C" void RexSwitchApmRequestGpuMhz(int mhz);
extern "C" void RexSwitchApmAllowRam1600(int allow);
extern "C" void RexSwitchApmApply(void);
#endif

void ApplySettingsGraphics() {
  const std::string resolution = REXCVAR_GET(nfsmw_internal_resolution);
  // 1920x1080 is not requested through the video mode. The game has its own table and above 1280x720 it
  // stays at 1280x720, so asking for it does nothing. What does work is writing the size into the mode
  // table of its renderer: nfsmw_render_targets.cpp does that, reading this same cvar, and also switches
  // it on the fly when the console enters or leaves the dock (automatic mode). The video mode stays at
  // 720p, which is what is presented. Setting the video mode to 1920x1080 so that the game's front buffer
  // would grow with it was tried and does not work: the front buffer is registered earlier and stays at
  // 1280x720.
  const bool internal_1080p = resolution == "1920x1080" || resolution == "automatico";
  const std::string mode_video = internal_1080p ? std::string("1280x720") : resolution;
  const size_t x = mode_video.find('x');
  if (x != std::string::npos) {
    Set("video_mode_width", mode_video.substr(0, x));
    Set("video_mode_height", mode_video.substr(x + 1));
  }
#if REX_PLATFORM_SWITCH
  // FPS and resolution for the console overlays, and honoring Reverse-NX. Can be turned off if it gets in the way.
  RexSwitchSaltyNxEnable(REXCVAR_GET(nfsmw_switch_saltynx) ? 1 : 0);
  RexSwitchClocksReverseEnable(REXCVAR_GET(nfsmw_switch_reverse_nx_clocks) ? 1 : 0);
  // GPU profile in handheld mode. See nfsmw_switch_gpu_mhz.
  RexSwitchApmRequestGpuMhz(REXCVAR_GET(nfsmw_switch_gpu_mhz));
  RexSwitchApmAllowRam1600(REXCVAR_GET(nfsmw_switch_ram_1600) ? 1 : 0);
  RexSwitchApmApply();
  RexSwitchPerfIntervalSwap(unsigned(REXCVAR_GET(nfsmw_swap_interval)));
#endif
  Set("resolution", "");  // empty preset: the width and height above apply
  const std::string limit = REXCVAR_GET(nfsmw_fps_limit);
  Set("video_mode_refresh_rate", limit == "30" ? "30" : "60");
  REXLOG_INFO("[settings] internal resolution {} and FPS limit {}: video mode {}x{} at {} Hz", resolution, limit,
              rex::cvar::GetFlagByName("video_mode_width"), rex::cvar::GetFlagByName("video_mode_height"),
              rex::cvar::GetFlagByName("video_mode_refresh_rate"));
#if REX_PLATFORM_SWITCH
  {
    // The overlay shows the resolution the game draws at, so with the 1080p internal mode it has to be
    // told 1920x1088, which is the real scene render target, not the video mode.
    unsigned width = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_width").c_str(), nullptr, 10));
    unsigned height = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_height").c_str(), nullptr, 10));
    // nfsmw_render_targets.cpp publishes the real size every time the mode changes, because with
    // "automatico" it changes on the fly. Only the startup value is set here.
    if (resolution == "1920x1080") {
      width = 1920;
      height = 1080;
    }
    if (width && height) {
      RexSwitchPerfResolution(width, height);  // for the console overlay
    }
  }
#endif
}

void HideSettingsWithoutEffect() {
  if (!nfsmw::native::Active()) {
    return;  // with emulation, all of them do something
  }
  const std::vector<std::string> hidden = {
      // From the emulated path: they do nothing with the native renderer.
      "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y", "vsync", "anisotropic_override",
      "swap_post_effect",
      // Replaced by nfsmw_internal_resolution and nfsmw_fps_limit.
      "video_mode_width", "video_mode_height", "resolution", "video_mode_refresh_rate",
  };
  // On PC, the emulated path settings are registered by the GPU plugin, which is not loaded with the
  // native renderer: they do not exist there. On the Switch they are built into the executable and do show
  // up in the menu. They are hidden by name in both cases.
  std::string registered;
  std::string sin_registrar;
  for (const std::string& name : hidden) {
    std::string& list = rex::cvar::GetFlagInfo(name) ? registered : sin_registrar;
    list += (list.empty() ? "" : ", ") + name;
  }
  rex::ui::HideSettingsInMenu(hidden);
  REXLOG_INFO("[settings] menu: left out, because they do nothing with the native renderer or the Graphics "
              "category replaces them: {}; not registered in this build: {}",
              registered.empty() ? "ninguno" : registered, sin_registrar.empty() ? "ninguno" : sin_registrar);
}

Postprocess ReadPostprocess() {
  struct Preset {
    const char* name;
    Postprocess values;
  };
  // The GoldenEye-Recomp ones (ge_postfx.cpp): brightness, contrast, saturation, vibrance, temperature,
  // gamma, tint (red, green, blue and strength), vignette and scanlines.
  static const Preset kPresets[] = {
      {"cine", {true, -0.04f, 1.15f, 1.05f, 0.15f, -0.10f, 1.0f, 1.0f, 0.95f, 0.85f, 0.12f, 0.40f, 0.0f}},
      {"sepia", {true, -0.02f, 1.05f, 0.20f, 0.0f, 0.25f, 1.0f, 1.0f, 0.82f, 0.55f, 0.50f, 0.30f, 0.0f}},
      {"noir", {true, -0.03f, 1.35f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.55f, 0.0f}},
      {"frio", {true, 0.0f, 1.05f, 1.0f, 0.10f, -0.55f, 1.0f, 0.70f, 0.85f, 1.0f, 0.15f, 0.18f, 0.0f}},
      {"calido", {true, 0.02f, 1.05f, 1.05f, 0.15f, 0.55f, 1.0f, 1.0f, 0.85f, 0.60f, 0.12f, 0.18f, 0.0f}},
      {"live", {true, 0.0f, 1.10f, 1.20f, 0.50f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}},
      {"matrix", {true, -0.03f, 1.10f, 0.85f, 0.20f, 0.0f, 1.0f, 0.55f, 1.0f, 0.60f, 0.30f, 0.30f, 0.25f}},
      {"crt", {true, -0.02f, 1.05f, 1.10f, 0.10f, 0.0f, 1.0f, 0.90f, 1.0f, 0.90f, 0.10f, 0.20f, 0.50f}},
  };
  const std::string mode = rex::cvar::GetFlagByName("nfsmw_postprocess");
  for (const Preset& p : kPresets) {
    if (mode == p.name) {
      return p.values;
    }
  }
  Postprocess result;
  if (mode != "personalizado") {
    return result;  // off
  }
  result.active = true;
  result.brightness = Number("nfsmw_postprocess_brightness", 0.0f);
  result.contrast = Number("nfsmw_postprocess_contrast", 1.0f);
  result.saturation = Number("nfsmw_postprocess_saturation", 1.0f);
  result.vibrance = Number("nfsmw_postprocess_vibrance", 0.0f);
  result.temperature = Number("nfsmw_postprocess_temperature", 0.0f);
  result.gamma = Number("nfsmw_postprocess_gamma", 1.0f);
  result.vignette = Number("nfsmw_postprocess_vignette", 0.0f);
  result.lines = Number("nfsmw_postprocess_scanlines", 0.0f);
  return result;
}

uint64_t VersionPostprocess() {
  return g_version_postprocess.load(std::memory_order_relaxed);
}

bool AntialiasingFxaa() {
  return g_fxaa.load(std::memory_order_relaxed);
}

int GlowSky() {
  return g_glow_sky.load(std::memory_order_relaxed);
}

void WatchSettingsInLive() {
  // The notification arrives with the registry lock held (SetFlagFromSource): only atomics are touched.
  for (const char* name : kCvarsPostprocess) {
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
      g_version_postprocess.fetch_add(1, std::memory_order_relaxed);
    });
  }
  g_fxaa.store(rex::cvar::GetFlagByName("nfsmw_antialiasing") == "fxaa", std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_antialiasing", [](std::string_view, std::string_view input_value) {
    g_fxaa.store(input_value == "fxaa", std::memory_order_relaxed);
  });
  g_glow_sky.store(ModeGlow(rex::cvar::GetFlagByName("nfsmw_sky_glow")),
                          std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_sky_glow", [](std::string_view, std::string_view input_value) {
    g_glow_sky.store(ModeGlow(input_value), std::memory_order_relaxed);
  });
}

}  // namespace nfsmw::settings
