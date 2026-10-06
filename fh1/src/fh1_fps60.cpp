// fh1 - 60 fps while driving (--fh1_fps60, off by default).
//
// Where the 30 comes from: the game asks Direct3D to show each picture of the 3D world for two
// screen refreshes (the swap command carries a present interval of 2; sub_829EED78 adds it to the
// previous swap's target refresh), and its simulation thread also steps once per two refreshes.
// With the 60 Hz refresh the port reports, that is 30 pictures and 30 steps a second.
//
// What the option does: the refresh the game counts ticks 120 times a second
// (video_mode_refresh_rate, read by the native renderer's LoopVblank and by the emulated GPU's
// vblank). "Two refreshes" is then 16.7 ms, for the pictures and for the simulation, and the
// game's own clock (it measures real milliseconds between steps) keeps its speed.
//
// What has to be put right with it:
//   - Pictures the game shows every refresh (the logo videos) would run at 120: their interval of
//     1 becomes 2.
//   - A frame that misses its refresh waits for the next one. When it is already two or more
//     refreshes late it is shown at once instead (fh1_fps60_late_swaps).
//   - Some code advances by a constant step per update, written for 30 updates a second: the
//     trackside crowd's animation and the scripted menu cameras (buying a car). They get the real
//     time since their previous update instead (fh1_fps60_fixed_steps).
//
// The method (a guest refresh at twice the wanted frame rate) and the places of the three fixes
// (0x829EEE44, 0x826499C8, 0x82E08A30 / 0x82E09144 / 0x82E0945C, and the simulation's time step
// at 0x823EDB84) come from pinyon-shift by the Pinyon Shift contributors, BSD 3-Clause:
// https://github.com/arcanite24/pinyon-shift (src/pinyon_shift_runtime_hooks.cpp). Same game
// file as ours (default.xex, SHA-256 DB40DF60...). See THIRD_PARTY_NOTICES.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fh1_fps60, false, "FH1",
                    "60 frames per second while driving instead of the game's 30 (the refresh the "
                    "game counts runs at 120 Hz). Off by default: being tested");
REXCVAR_DEFINE_BOOL(fh1_fps60_late_swaps, true, "FH1",
                    "With fh1_fps60: a frame that is two or more refreshes late is shown at once "
                    "instead of at the next refresh");
REXCVAR_DEFINE_BOOL(fh1_fps60_fixed_steps, true, "FH1",
                    "With fh1_fps60: the crowd's animation and the scripted menu cameras advance "
                    "by real time instead of a constant step per frame (they ran at double speed)");

namespace {
using Clock = std::chrono::steady_clock;

bool On() { return REXCVAR_GET(fh1_fps60); }
bool FixedSteps() { return On() && REXCVAR_GET(fh1_fps60_fixed_steps); }

// Seconds since the previous call for the same object; below 0 when there is none to trust (the
// first call, or a pause of more than a quarter of a second).
struct Clocks {
  std::mutex mutex;
  std::unordered_map<uint32_t, Clock::time_point> last;
  double Elapsed(uint32_t object) {
    const Clock::time_point now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex);
    if (last.size() >= 1024) last.clear();
    Clock::time_point& then = last[object];
    double seconds = -1.0;
    if (then != Clock::time_point{}) {
      seconds = std::chrono::duration<double>(now - then).count();
      if (seconds >= 0.25) seconds = -1.0;
    }
    then = now;
    return seconds;
  }
};

Clocks g_camera_clocks;
Clocks g_crowd_clocks;
std::unordered_map<uint32_t, double> g_crowd_carry;  // guarded by g_crowd_mutex
std::mutex g_crowd_mutex;
thread_local uint32_t g_crowd_steps = 1;

std::atomic<uint64_t> g_late_released{0};
std::atomic<uint64_t> g_interval_raised{0};
}  // namespace

// Called from Fh1App::OnPreSetup, before the graphics system reads the video mode.
void Fh1Fps60Apply() {
  if (!On()) {
    return;
  }
  rex::cvar::SetFlagByName("video_mode_refresh_rate", "120");
  REXLOG_INFO("[fps60] on: the game's refresh runs at {} Hz (late swaps {}, fixed steps {})",
              rex::cvar::GetFlagByName("video_mode_refresh_rate"),
              REXCVAR_GET(fh1_fps60_late_swaps) ? "released" : "wait", FixedSteps() ? "real time" : "as is");
}

// Direct3D's swap callback. r3: bits 0-7 = how late (percent of a refresh) a swap may be and still
// go to the current refresh, bits 8-11 = the present interval in refreshes, the rest = flags.
// Called from the hook on sub_829EED78 in fh1_d3d_census.cpp (one hook per function).
void Fh1Fps60SwapCallback(uint32_t& r3) {
  if (On() && ((r3 >> 8) & 0xF) == 1) {
    r3 = (r3 & ~0xF00u) | 0x200u;
    g_interval_raised.fetch_add(1, std::memory_order_relaxed);
  }
}

// 0x829EEE44, inside the swap callback: r7 = how late this swap is (percent of a refresh), r8 = the
// previous swap's target refresh, r10 = the refresh count now. Two or more refreshes past the
// previous target, the frame is late whatever happens: with r7 = 0 the game shows it now.
void Fh1Fps60ReleaseLateSwap(PPCRegister& r7, PPCRegister& r8, PPCRegister& r10) {
  if (On() && REXCVAR_GET(fh1_fps60_late_swaps) && r10.u32 - r8.u32 >= 2) {
    r7.u64 = 0;
    g_late_released.fetch_add(1, std::memory_order_relaxed);
  }
}

// 0x826499C8, the scripted menu camera's update (sub_82649960): f1 = its step, 1/30 s (1/60 under
// a UI flag), r31 = the camera.
void Fh1Fps60ScaleCameraStep(PPCRegister& r31, PPCRegister& f1) {
  if (!FixedSteps()) {
    return;
  }
  const double seconds = g_camera_clocks.Elapsed(r31.u32);
  if (seconds >= 0.0) {
    f1.f64 = f1.f64 * std::min(seconds, 0.1) * 30.0;
  }
}

// 0x82E08A30, the crowd's animation update (sub_82E08A00) after its prologue: r29 = the models
// object. The game adds one animation frame per rendered frame; here it is the whole 1/30 s steps
// of real time since the same object's previous update, the fraction kept for the next one.
void Fh1Fps60CrowdBegin(PPCRegister& r29) {
  g_crowd_steps = 1;
  if (!FixedSteps()) {
    return;
  }
  const uint32_t object = r29.u32;
  const double seconds = g_crowd_clocks.Elapsed(object);
  std::lock_guard<std::mutex> lock(g_crowd_mutex);
  if (g_crowd_carry.size() >= 1024) g_crowd_carry.clear();
  double& carry = g_crowd_carry[object];
  if (seconds < 0.0) {
    carry = 0.0;
    return;
  }
  carry += seconds * 30.0;
  g_crowd_steps = uint32_t(carry);
  carry -= g_crowd_steps;
}

// 0x82E09144 and 0x82E0945C: r10 = the step about to be added to one crowd member's frame.
void Fh1Fps60CrowdStep(PPCRegister& r10) {
  if (FixedSteps() && r10.u32 != 0) {
    r10.u64 = g_crowd_steps;
  }
}

// 0x823EDB84, the simulation loop (sub_823ED888): f31 = the time step in seconds the game is about
// to store and hand to its systems. Nothing is changed: every 10 s the log gets the steps per
// second and the game seconds that passed per real second (1.00 = the right speed).
void Fh1Fps60ObserveStep(PPCRegister& f31) {
  static Clock::time_point window;
  static uint32_t steps = 0;
  static double game_seconds = 0.0;
  static double longest = 0.0;
  const double step = f31.f64;
  if (!std::isfinite(step) || step < 0.0 || step > 4.0) {
    return;
  }
  const Clock::time_point now = Clock::now();
  if (window == Clock::time_point{}) {
    window = now;
  }
  ++steps;
  game_seconds += step;
  longest = std::max(longest, step);
  const double real = std::chrono::duration<double>(now - window).count();
  if (real >= 10.0) {
    REXLOG_INFO("[fps60] {}: {:.1f} simulation steps/s, {:.3f} game s per real s, longest step {:.1f} ms; "
                "late swaps shown at once {}, intervals 1 -> 2 {}",
                On() ? "on" : "off", steps / real, game_seconds / real, longest * 1000.0,
                g_late_released.load(std::memory_order_relaxed), g_interval_raised.load(std::memory_order_relaxed));
    window = now;
    steps = 0;
    game_seconds = 0.0;
    longest = 0.0;
  }
}
