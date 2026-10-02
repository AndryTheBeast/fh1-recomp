// fh1 - records what the player does, for writing test autoplay scripts and recording routes.
//
// --fh1_record=<file> writes every change of the controller state the game reads (buttons,
// triggers, sticks; sticks rounded to 0.05) with the time since launch, and - when the autoplay
// script has named the car position variables posx/posy/posz (calibrate_position) - the car
// position ten times a second:
//   12.345 in a,start lt=0.00 rt=1.00 lx=-0.25 ly=0.00 rx=0.00 ry=0.00
//   12.400 pos -1066.77 -10.18 -226.74
// tools/record_to_script.py turns the input lines into an autoplay script.

#include "fh1_recorder.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/kernel/input_events.h>
#include <rex/logging.h>

#include "fh1_memscan.h"

namespace fh1 {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::chrono::steady_clock::time_point g_start;
std::string g_last;

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

std::string Describe(const rex::kernel::InputSnapshot& s) {
  static constexpr struct {
    const char* name;
    uint16_t mask;
  } kButtons[] = {{"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
                  {"start", 0x0010}, {"back", 0x0020}, {"ls", 0x0040},   {"rs", 0x0080},
                  {"lb", 0x0100},    {"rb", 0x0200},   {"a", 0x1000},    {"b", 0x2000},
                  {"x", 0x4000},     {"y", 0x8000}};
  std::string buttons;
  for (const auto& b : kButtons) {
    if (s.buttons & b.mask) buttons += (buttons.empty() ? "" : ",") + std::string(b.name);
  }
  if (buttons.empty()) buttons = "-";
  auto stick = [](int16_t v) {
    double f = std::round(double(v) / 32767.0 / 0.05) * 0.05;
    return std::fabs(f) < 0.075 ? 0.0 : f;  // dead zone
  };
  auto trig = [](uint8_t v) { return std::round(double(v) / 255.0 / 0.05) * 0.05; };
  return fmt::format("in {} lt={:.2f} rt={:.2f} lx={:.2f} ly={:.2f} rx={:.2f} ry={:.2f}", buttons,
                     trig(s.left_trigger), trig(s.right_trigger), stick(s.thumb_lx),
                     stick(s.thumb_ly), stick(s.thumb_rx), stick(s.thumb_ry));
}

}  // namespace

void StartRecorder(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file) return;
  g_file = std::fopen(path.c_str(), "w");
  if (!g_file) {
    REXLOG_ERROR("[record] cannot write {}", path);
    return;
  }
  g_start = std::chrono::steady_clock::now();
  std::fprintf(g_file, "# fh1 input/position recording (fh1/src/fh1_recorder.cpp)\n");
  std::fflush(g_file);
  REXLOG_INFO("[record] recording the controller to {}", path);
  rex::kernel::SetInputObserver([](const rex::kernel::InputSnapshot& s) {
    if (s.user != 0) return;
    std::string line = Describe(s);
    std::lock_guard<std::mutex> lock2(g_mutex);
    if (!g_file || line == g_last) return;
    g_last = line;
    std::fprintf(g_file, "%.3f %s\n", Now(), line.c_str());
    std::fflush(g_file);
  });
  std::thread([] {
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      float x, y, z;
      if (!memscan::Variable("posx", x) || !memscan::Variable("posy", y) ||
          !memscan::Variable("posz", z)) {
        continue;
      }
      std::lock_guard<std::mutex> lock2(g_mutex);
      if (!g_file) return;
      std::fprintf(g_file, "%.3f pos %.2f %.2f %.2f\n", Now(), x, y, z);
    }
  }).detach();
}

void RecordMarker(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_file) return;
  std::fprintf(g_file, "%.3f mark %s\n", Now(), text.c_str());
  std::fflush(g_file);
}

}  // namespace fh1
