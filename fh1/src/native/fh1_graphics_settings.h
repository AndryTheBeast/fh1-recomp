// fh1 - graphics settings in the menu (F4) that work with the native renderer.
//
// The FPS and resolution settings that do nothing are removed from the menu and the ones that work are kept.
// Measured on PC:
//   - NFSMW draws at the size of the video mode that VdQueryVideoMode returns, but it only has two:
//     1280x720 and, below that, 1024x576 (tried 854x480, 960x540, 1024x576 and 1152x648). Above
//     1280x720 it stays at 1280x720.
//   - Its pace is set by the vblank, which follows video_mode_refresh_rate: at 30 Hz it gives exactly
//     30 FPS and the game runs at normal speed (the race timer advances in real time).
//   - resolution_scale, draw_resolution_scale_x/y, vsync, anisotropic_override and swap_post_effect are
//     only read by the emulated path: with the native renderer they do nothing.
// With the gamepad, the menu makes it awkward to change free text (resolution), an integer one step at a
// time (video_mode_width/height) or a decimal (video_mode_refresh_rate). That is why the ones that work
// are offered as lists (fh1_internal_resolution and fh1_fps_limit) and passed to the video mode at
// startup.

#pragma once

#include <cstdint>

namespace fh1::settings {

// Optional post-processing in the Graphics/Postprocess category, like the one in GoldenEye-Recomp (which only
// exists in D3D12): color grading (temperature, brightness, contrast, tint, saturation, vibrance and gamma),
// vignette and scanlines. Off by default: the output stays bit-identical. Applied by the output pass of the
// native renderer.
struct Postprocess {
  bool active = false;
  float brightness = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float vibrance = 0.0f;
  float temperature = 0.0f;
  float gamma = 1.0f;
  float tint_r = 1.0f;
  float tint_g = 1.0f;
  float tint_b = 1.0f;
  float tint = 0.0f;
  float vignette = 0.0f;
  float lines = 0.0f;
};

// Values in effect (the chosen preset or the custom ones). Reads the cvars with the registry lock: call
// only when VersionPostprocess changes.
Postprocess ReadPostprocess();

// Increments every time the menu changes a post-processing cvar (lock-free: it can be checked every frame).
uint64_t VersionPostprocess();

// fh1_antialiasing = fxaa (lock-free: it can be checked every frame).
bool AntialiasingFxaa();

// fh1_sky_glow: 0 original, 1 natural, 2 soft (lock-free: it can be checked on every draw).
int GlowSky();

// fh1_color_filter: 0 original, 1 soft, 2 off (lock-free: it can be checked on every draw).
int TreatmentVisual();

// Registers the change notifications for the live Graphics settings (post-processing, antialiasing, sky glow
// and the color filter), once at startup.
void WatchSettingsInLive();

// Passes fh1_internal_resolution and fh1_fps_limit to the video mode (video_mode_width/height,
// resolution and video_mode_refresh_rate), except the ones that come from the command line. Must run
// before the game requests the video mode.
void ApplySettingsGraphics();

// Nintendo Switch only: asks the console for the official 460.8 MHz handheld GPU profile and starts the SaltyNX FPS
// publishing (the same lines ApplySettingsGraphics has, without touching the video mode). Does nothing elsewhere.
void ApplySwitchClocks();

// With the native renderer, removes from the menu the FPS, resolution and graphics settings that do
// nothing, and the video mode settings that the ones above replace.
void HideSettingsWithoutEffect();

}  // namespace fh1::settings
