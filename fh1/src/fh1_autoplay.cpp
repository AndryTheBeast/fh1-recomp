// fh1 - scripted controller for unattended test runs (see fh1_autoplay.h).

#include "fh1_autoplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging.h>

namespace fh1 {
namespace {

using namespace rex;
using namespace rex::input;

constexpr DeviceId kAutoplayDevice = static_cast<DeviceId>(0x46483141);  // "FH1A"

struct Step {
  double start = 0, end = 0;
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;
  bool has_lx = false, has_ly = false, has_rx = false, has_ry = false;
};

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool ParseControl(std::string_view c, Step& step) {
  static constexpr struct {
    const char* name;
    uint16_t mask;
  } kButtons[] = {{"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
                  {"start", 0x0010}, {"back", 0x0020}, {"ls", 0x0040},   {"rs", 0x0080},
                  {"lb", 0x0100},    {"rb", 0x0200},   {"a", 0x1000},    {"b", 0x2000},
                  {"x", 0x4000},     {"y", 0x8000}};
  for (const auto& b : kButtons) {
    if (c == b.name) {
      step.buttons |= b.mask;
      return true;
    }
  }
  if (c == "lt") return step.lt = 255, true;
  if (c == "rt") return step.rt = 255, true;
  auto eq = c.find('=');
  if (eq == std::string_view::npos) return false;
  std::string_view axis = c.substr(0, eq);
  float v = std::clamp(std::strtof(std::string(c.substr(eq + 1)).c_str(), nullptr), -1.0f, 1.0f);
  if (axis == "lx") return step.lx = v, step.has_lx = true;
  if (axis == "ly") return step.ly = v, step.has_ly = true;
  if (axis == "rx") return step.rx = v, step.has_rx = true;
  if (axis == "ry") return step.ry = v, step.has_ry = true;
  return false;
}

int16_t Stick(float v) { return static_cast<int16_t>(std::lround(v * 32767.0f)); }

class AutoplayDriver final : public InputDriver {
 public:
  explicit AutoplayDriver(std::vector<Step> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)),
        start_(std::chrono::steady_clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    DeviceInfo info;
    info.id = kAutoplayDevice;
    info.name = "fh1 autoplay";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kAutoplayDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out_state) return X_ERROR_SUCCESS;
    auto elapsed = std::chrono::steady_clock::now() - start_;
    double t = std::chrono::duration<double>(elapsed).count();
    std::memset(out_state, 0, sizeof(*out_state));
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    for (const auto& s : steps_) {
      if (t < s.start || t >= s.end) continue;
      buttons |= s.buttons;
      lt = std::max(lt, s.lt);
      rt = std::max(rt, s.rt);
      if (s.has_lx) lx = s.lx;
      if (s.has_ly) ly = s.ly;
      if (s.has_rx) rx = s.rx;
      if (s.has_ry) ry = s.ry;
    }
    // Always-rising packet number (milliseconds), so the merged state counts as new input.
    out_state->packet_number =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = Stick(lx);
    out_state->gamepad.thumb_ly = Stick(ly);
    out_state->gamepad.thumb_rx = Stick(rx);
    out_state->gamepad.thumb_ry = Stick(ry);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kAutoplayDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kAutoplayDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return id == kAutoplayDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::vector<Step> steps_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace

std::unique_ptr<InputDriver> CreateAutoplayDriver(const std::string& script) {
  std::vector<Step> steps;
  std::string_view rest = script;
  while (!rest.empty()) {
    auto semi = rest.find(';');
    std::string_view entry = Trim(rest.substr(0, semi));
    rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
    if (entry.empty()) continue;
    auto plus = entry.find('+');
    auto eq = entry.find('=');
    if (plus == std::string_view::npos || eq == std::string_view::npos || eq < plus) {
      REXLOG_ERROR("fh1_autoplay: bad entry '{}' (expected START+DURATION=CONTROLS)", entry);
      return nullptr;
    }
    Step step;
    step.start = std::strtod(std::string(entry.substr(0, plus)).c_str(), nullptr);
    step.end = step.start +
               std::strtod(std::string(entry.substr(plus + 1, eq - plus - 1)).c_str(), nullptr);
    std::string_view controls = entry.substr(eq + 1);
    while (!controls.empty()) {
      auto comma = controls.find(',');
      std::string_view c = Trim(controls.substr(0, comma));
      controls = comma == std::string_view::npos ? std::string_view() : controls.substr(comma + 1);
      if (!c.empty() && !ParseControl(c, step)) {
        REXLOG_ERROR("fh1_autoplay: unknown control '{}' in '{}'", c, entry);
        return nullptr;
      }
    }
    steps.push_back(step);
  }
  if (steps.empty()) return nullptr;
  REXLOG_INFO("fh1_autoplay: {} scripted input steps", steps.size());
  return std::make_unique<AutoplayDriver>(std::move(steps));
}

}  // namespace fh1
