// fh1 - scripted controller for unattended test runs (see fh1_autoplay.h).

#include "fh1_autoplay.h"
#include "fh1_memscan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/kernel/file_events.h>
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

// The pad as a script leaves it at a moment.
struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;
};

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
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
  if (axis == "lt") return step.lt = uint8_t(std::lround(std::max(v, 0.0f) * 255.0f)), true;
  if (axis == "rt") return step.rt = uint8_t(std::lround(std::max(v, 0.0f) * 255.0f)), true;
  if (axis == "lx") return step.lx = v, step.has_lx = true;
  if (axis == "ly") return step.ly = v, step.has_ly = true;
  if (axis == "rx") return step.rx = v, step.has_rx = true;
  if (axis == "ry") return step.ry = v, step.has_ry = true;
  return false;
}

bool ParseControls(std::string_view controls, Step& step) {
  while (!controls.empty()) {
    auto comma = controls.find(',');
    std::string_view c = Trim(controls.substr(0, comma));
    controls = comma == std::string_view::npos ? std::string_view() : controls.substr(comma + 1);
    if (!c.empty() && !ParseControl(c, step)) return false;
  }
  return true;
}

int16_t Stick(float v) { return static_cast<int16_t>(std::lround(v * 32767.0f)); }

class AutoplayDriver final : public InputDriver {
 public:
  // Timetable mode (--fh1_autoplay).
  explicit AutoplayDriver(std::vector<Step> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)),
        start_(std::chrono::steady_clock::now()) {}
  // Script mode (--fh1_autoplay_file): the pad state comes from the script thread.
  AutoplayDriver(std::function<PadState()> provider, std::shared_ptr<void> keep_alive)
      : InputDriver(nullptr, 0), provider_(std::move(provider)),
        keep_alive_(std::move(keep_alive)), start_(std::chrono::steady_clock::now()) {}

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
    PadState pad;
    if (provider_) pad = provider_();
    for (const auto& s : steps_) {
      if (t < s.start || t >= s.end) continue;
      pad.buttons |= s.buttons;
      pad.lt = std::max(pad.lt, s.lt);
      pad.rt = std::max(pad.rt, s.rt);
      if (s.has_lx) pad.lx = s.lx;
      if (s.has_ly) pad.ly = s.ly;
      if (s.has_rx) pad.rx = s.rx;
      if (s.has_ry) pad.ry = s.ry;
    }
    // Always-rising packet number (milliseconds), so the merged state counts as new input.
    out_state->packet_number =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    out_state->gamepad.buttons = pad.buttons;
    out_state->gamepad.left_trigger = pad.lt;
    out_state->gamepad.right_trigger = pad.rt;
    out_state->gamepad.thumb_lx = Stick(pad.lx);
    out_state->gamepad.thumb_ly = Stick(pad.ly);
    out_state->gamepad.thumb_rx = Stick(pad.rx);
    out_state->gamepad.thumb_ry = Stick(pad.ry);
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
  std::function<PadState()> provider_;
  std::shared_ptr<void> keep_alive_;
  std::chrono::steady_clock::time_point start_;
};

// --fh1_autoplay_file: a script run line by line on its own thread (format in fh1_autoplay.h).
struct Instr {
  std::string op;
  Step controls;  // tap / hold / set
  double seconds = 0;
  int repeat = 1;
  double gap = 0.25;
  std::string text;  // shot name / log text / waitfile pattern
  // waitdraws
  bool greater = true;
  uint32_t draws = 0;
  double hold_for = 0;
  double timeout = 120;
  float lo = 0, hi = 0;  // memscan_start
};

// Files the game opened, for 'waitfile' (lower-case paths with a running number).
struct FileLog {
  std::mutex mutex;
  std::vector<std::pair<uint64_t, std::string>> recent;  // last 256
  uint64_t count = 0;
};
FileLog g_file_log;

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

class ScriptRunner {
 public:
  ScriptRunner(std::vector<Instr> program, std::string dir,
               std::function<uint32_t()> draws_provider)
      : program_(std::move(program)), dir_(std::move(dir)),
        draws_provider_(std::move(draws_provider)) {}
  ~ScriptRunner() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  void Start() { thread_ = std::thread([this] { Run(); }); }

  PadState Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    PadState pad = held_;
    pad.buttons |= set_.buttons;
    pad.lt = std::max(pad.lt, set_.lt);
    pad.rt = std::max(pad.rt, set_.rt);
    if (set_.has_lx) pad.lx = set_.lx;
    if (set_.has_ly) pad.ly = set_.ly;
    if (set_.has_rx) pad.rx = set_.rx;
    if (set_.has_ry) pad.ry = set_.ry;
    return pad;
  }

 private:
  static PadState FromStep(const Step& s) {
    PadState p;
    p.buttons = s.buttons;
    p.lt = s.lt;
    p.rt = s.rt;
    p.lx = s.lx;
    p.ly = s.ly;
    p.rx = s.rx;
    p.ry = s.ry;
    return p;
  }

  void Sleep(double seconds) {
    auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!stop_ && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void Hold(const Step& s, double seconds) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      held_ = FromStep(s);
    }
    Sleep(seconds);
    std::lock_guard<std::mutex> lock(mutex_);
    held_ = PadState();
  }

  // Writes <dir>/<name>.req and waits (up to 5 s) for tools/auto_test.ps1 to delete it.
  void Request(const std::string& name) {
    if (dir_.empty()) {
      REXLOG_WARN("[autoplay] '{}' needs --fh1_autoplay_dir (run through tools/auto_test.ps1)",
                  name);
      return;
    }
    std::filesystem::path req = std::filesystem::path(dir_) / (name + ".req");
    { std::ofstream(req) << name; }
    for (int i = 0; i < 500 && !stop_ && std::filesystem::exists(req); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void Run() {
    auto start = std::chrono::steady_clock::now();
    for (const Instr& in : program_) {
      if (stop_) return;
      double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (in.op == "wait") {
        Sleep(in.seconds);
      } else if (in.op == "tap") {
        for (int i = 0; i < in.repeat && !stop_; ++i) {
          Hold(in.controls, 0.12);
          Sleep(in.gap);
        }
      } else if (in.op == "hold") {
        Hold(in.controls, in.seconds);
      } else if (in.op == "set") {
        std::lock_guard<std::mutex> lock(mutex_);
        const Step& c = in.controls;
        set_.buttons |= c.buttons;
        set_.lt = std::max(set_.lt, c.lt);
        set_.rt = std::max(set_.rt, c.rt);
        if (c.has_lx) set_.lx = c.lx, set_.has_lx = true;
        if (c.has_ly) set_.ly = c.ly, set_.has_ly = true;
        if (c.has_rx) set_.rx = c.rx, set_.has_rx = true;
        if (c.has_ry) set_.ry = c.ry, set_.has_ry = true;
      } else if (in.op == "clear") {
        std::lock_guard<std::mutex> lock(mutex_);
        set_ = Step();
      } else if (in.op == "shot") {
        REXLOG_INFO("[autoplay] {:.1f} s: screenshot '{}'", t, in.text);
        Request("shot-" + in.text);
      } else if (in.op == "log") {
        REXLOG_INFO("[autoplay] {:.1f} s: {}", t, in.text);
      } else if (in.op == "memscan_start") {
        memscan::Start(in.lo, in.hi, dir_);
      } else if (in.op == "memscan_sample") {
        memscan::Sample(in.text, dir_);
      } else if (in.op == "memscan_filter") {
        memscan::Filter(in.text, in.lo);
      } else if (in.op == "memscan_list") {
        memscan::List(size_t(in.lo));
      } else if (in.op == "memscan_ptrs") {
        memscan::Pointers(uint32_t(in.lo), int(in.hi));
      } else if (in.op == "memscan_pick") {
        memscan::Pick(in.text, in.lo);
      } else if (in.op == "logvar") {
        float v = 0;
        if (memscan::Variable(in.text, v)) {
          REXLOG_INFO("[autoplay] {:.1f} s: {} = {:.3f}", t, in.text, v);
        } else {
          REXLOG_WARN("[autoplay] {:.1f} s: variable {} not picked", t, in.text);
        }
      } else if (in.op == "waitvar") {
        WaitVar(in, t);
      } else if (in.op == "waitfile") {
        WaitFile(in, t);
      } else if (in.op == "waitdraws") {
        WaitDraws(in, t);
      } else if (in.op == "quit") {
        REXLOG_INFO("[autoplay] {:.1f} s: quit", t);
        Sleep(2.0);  // let the log reach the file before auto_test kills the game
        Request("quit");
        return;
      }
    }
    REXLOG_INFO("[autoplay] script finished");
  }

  // Waits until a file whose path contains the pattern is opened after this point.
  void WaitFile(const Instr& in, double t) {
    uint64_t since;
    {
      std::lock_guard<std::mutex> lock(g_file_log.mutex);
      since = g_file_log.count;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(in.timeout);
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(g_file_log.mutex);
        for (const auto& [n, path] : g_file_log.recent) {
          if (n >= since && path.find(in.text) != std::string::npos) {
            REXLOG_INFO("[autoplay] {:.1f} s: file '{}' opened", t, path);
            return;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REXLOG_WARN("[autoplay] waitfile '{}' timed out after {:.0f} s", in.text, in.timeout);
  }

  // Waits until a picked variable is above/below a value (for hold_for seconds).
  void WaitVar(const Instr& in, double t) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(in.timeout);
    auto ok_since = std::chrono::steady_clock::time_point();
    bool ok_run = false;
    float v = 0;
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
      if (!memscan::Variable(in.text, v)) {
        REXLOG_WARN("[autoplay] waitvar: variable {} not picked", in.text);
        return;
      }
      bool ok = in.greater ? v > in.lo : v < in.lo;
      auto now = std::chrono::steady_clock::now();
      if (ok && !ok_run) ok_since = now;
      ok_run = ok;
      if (ok && std::chrono::duration<double>(now - ok_since).count() >= in.hold_for) {
        REXLOG_INFO("[autoplay] {:.1f} s: {} {} {} (now {:.3f})", t, in.text,
                    in.greater ? ">" : "<", in.lo, v);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REXLOG_WARN("[autoplay] waitvar {} {}{} timed out after {:.0f} s (now {:.3f})", in.text,
                in.greater ? ">" : "<", in.lo, in.timeout, v);
  }

  // Waits until every frame for hold_for seconds has more (or fewer) draws than the threshold.
  void WaitDraws(const Instr& in, double t) {
    if (!draws_provider_) {
      REXLOG_WARN("[autoplay] waitdraws: no draw counter available");
      return;
    }
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::duration<double>(in.timeout);
    auto ok_since = std::chrono::steady_clock::time_point();
    bool ok_run = false;
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
      uint32_t d = draws_provider_();
      bool ok = in.greater ? d > in.draws : d < in.draws;
      auto now = std::chrono::steady_clock::now();
      if (ok && !ok_run) ok_since = now;
      ok_run = ok;
      if (ok && std::chrono::duration<double>(now - ok_since).count() >= in.hold_for) {
        REXLOG_INFO("[autoplay] {:.1f} s: draws {} {} {} (now {})", t, in.greater ? ">" : "<",
                    in.draws, in.hold_for > 0 ? "held" : "reached", d);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REXLOG_WARN("[autoplay] waitdraws {}{} timed out after {:.0f} s", in.greater ? ">" : "<",
                in.draws, in.timeout);
  }

  std::vector<Instr> program_;
  std::string dir_;
  std::function<uint32_t()> draws_provider_;
  std::mutex mutex_;
  PadState held_;
  Step set_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

std::vector<std::string> Words(std::string_view line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    if (j > i) out.emplace_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

// "xN" repeat count, or 0 if the word is not one.
int RepeatCount(const std::string& w) {
  if (w.size() < 2 || w[0] != 'x') return 0;
  return std::max(1, std::atoi(w.c_str() + 1));
}

using MacroMap = std::unordered_map<std::string, std::vector<Instr>>;

bool ParseScript(const std::filesystem::path& path, MacroMap& macros, std::vector<Instr>& out,
                 int depth) {
  if (depth > 8) {
    REXLOG_ERROR("fh1_autoplay_file: includes nested too deep at {}", path.string());
    return false;
  }
  std::ifstream file(path);
  if (!file) {
    REXLOG_ERROR("fh1_autoplay_file: cannot read {}", path.string());
    return false;
  }
  std::string raw;
  int line_no = 0;
  std::string defining;
  while (std::getline(file, raw)) {
    ++line_no;
    std::string_view line = raw;
    if (auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    line = Trim(line);
    auto w = Words(line);
    if (w.empty()) continue;
    auto fail = [&](const char* why) {
      REXLOG_ERROR("fh1_autoplay_file: {}:{}: {} ('{}')", path.filename().string(), line_no, why,
                   std::string(line));
      return false;
    };
    std::vector<Instr>& target = defining.empty() ? out : macros[defining];
    Instr in;
    in.op = w[0];
    if (in.op == "def") {
      if (w.size() != 2 || !defining.empty()) return fail("def NAME (no nesting)");
      defining = w[1];
      macros[defining].clear();
      continue;
    }
    if (in.op == "end") {
      if (defining.empty()) return fail("end without def");
      defining.clear();
      continue;
    }
    if (in.op == "do") {
      if (w.size() < 2 || w.size() > 3) return fail("do NAME [xN]");
      auto it = macros.find(w[1]);
      if (it == macros.end()) return fail("unknown macro");
      int n = w.size() == 3 ? RepeatCount(w[2]) : 1;
      if (n == 0) return fail("do NAME [xN]");
      std::vector<Instr> body = it->second;
      for (int i = 0; i < n; ++i) target.insert(target.end(), body.begin(), body.end());
      continue;
    }
    if (in.op == "include") {
      if (w.size() != 2) return fail("include FILE");
      if (!ParseScript(path.parent_path() / w[1], macros, target, depth + 1)) return false;
      continue;
    }
    if (in.op == "wait") {
      if (w.size() != 2) return fail("wait SECONDS");
      in.seconds = std::strtod(w[1].c_str(), nullptr);
    } else if (in.op == "tap") {
      if (w.size() < 2 || !ParseControls(w[1], in.controls)) {
        return fail("tap CONTROLS [xN] [gap S]");
      }
      for (size_t i = 2; i < w.size(); ++i) {
        if (int n = RepeatCount(w[i])) {
          in.repeat = n;
        } else if (w[i] == "gap" && i + 1 < w.size()) {
          in.gap = std::strtod(w[++i].c_str(), nullptr);
        } else {
          return fail("tap CONTROLS [xN] [gap S]");
        }
      }
    } else if (in.op == "hold") {
      if (w.size() != 3 || !ParseControls(w[1], in.controls)) return fail("hold CONTROLS SECONDS");
      in.seconds = std::strtod(w[2].c_str(), nullptr);
    } else if (in.op == "set") {
      if (w.size() != 2 || !ParseControls(w[1], in.controls)) return fail("set CONTROLS");
    } else if (in.op == "memscan_start") {
      if (w.size() != 3) return fail("memscan_start LO HI");
      in.lo = std::strtof(w[1].c_str(), nullptr);
      in.hi = std::strtof(w[2].c_str(), nullptr);
    } else if (in.op == "memscan_sample") {
      if (w.size() != 2) return fail("memscan_sample NAME");
      in.text = w[1];
    } else if (in.op == "memscan_filter") {
      if (w.size() < 2 || w.size() > 3) return fail("memscan_filter OP [V]");
      in.text = w[1];
      if (w.size() == 3) in.lo = std::strtof(w[2].c_str(), nullptr);
    } else if (in.op == "memscan_list") {
      if (w.size() != 2) return fail("memscan_list N");
      in.lo = std::strtof(w[1].c_str(), nullptr);
    } else if (in.op == "memscan_ptrs") {
      if (w.size() != 3) return fail("memscan_ptrs MAXOFF DEPTH");
      in.lo = float(std::strtoul(w[1].c_str(), nullptr, 0));
      in.hi = std::strtof(w[2].c_str(), nullptr);
    } else if (in.op == "memscan_pick") {
      // memscan_pick NAME [MIN]
      if (w.size() < 2 || w.size() > 3) return fail("memscan_pick NAME [MIN]");
      in.text = w[1];
      in.lo = w.size() == 3 ? std::strtof(w[2].c_str(), nullptr) : 1.0f;
    } else if (in.op == "logvar") {
      if (w.size() != 2) return fail("logvar NAME");
      in.text = w[1];
    } else if (in.op == "waitvar") {
      // waitvar NAME >V|<V [for S] [timeout T]
      if (w.size() < 3 || (w[2][0] != '>' && w[2][0] != '<')) {
        return fail("waitvar NAME >V|<V [for S] [timeout T]");
      }
      in.text = w[1];
      in.greater = w[2][0] == '>';
      in.lo = std::strtof(w[2].c_str() + 1, nullptr);
      for (size_t i = 3; i + 1 < w.size(); i += 2) {
        if (w[i] == "for") {
          in.hold_for = std::strtod(w[i + 1].c_str(), nullptr);
        } else if (w[i] == "timeout") {
          in.timeout = std::strtod(w[i + 1].c_str(), nullptr);
        } else {
          return fail("waitvar NAME >V|<V [for S] [timeout T]");
        }
      }
    } else if (in.op == "waitfile") {
      // waitfile PATTERN [TIMEOUT]
      if (w.size() < 2 || w.size() > 3) return fail("waitfile PATTERN [TIMEOUT]");
      in.text = Lower(w[1]);
      if (w.size() == 3) in.timeout = std::strtod(w[2].c_str(), nullptr);
    } else if (in.op == "waitdraws") {
      // waitdraws >N|<N [for S] [timeout S]
      if (w.size() < 2 || (w[1][0] != '>' && w[1][0] != '<')) {
        return fail("waitdraws >N|<N [for S] [timeout S]");
      }
      in.greater = w[1][0] == '>';
      in.draws = uint32_t(std::strtoul(w[1].c_str() + 1, nullptr, 10));
      for (size_t i = 2; i + 1 < w.size(); i += 2) {
        if (w[i] == "for") {
          in.hold_for = std::strtod(w[i + 1].c_str(), nullptr);
        } else if (w[i] == "timeout") {
          in.timeout = std::strtod(w[i + 1].c_str(), nullptr);
        } else {
          return fail("waitdraws >N|<N [for S] [timeout S]");
        }
      }
    } else if (in.op == "clear" || in.op == "quit") {
    } else if (in.op == "shot") {
      if (w.size() != 2) return fail("shot NAME");
      in.text = w[1];
    } else if (in.op == "log") {
      in.text = std::string(Trim(line.substr(3)));
    } else {
      return fail("unknown command");
    }
    target.push_back(std::move(in));
  }
  if (!defining.empty()) {
    REXLOG_ERROR("fh1_autoplay_file: {}: def {} without end", path.filename().string(), defining);
    return false;
  }
  return true;
}

}  // namespace

void InstallFileObserver(bool log_opens) {
  rex::kernel::SetFileOpenObserver([log_opens](std::string_view path) {
    std::string lower = Lower(path);
    if (log_opens) REXLOG_INFO("[file] {}", path);
    std::lock_guard<std::mutex> lock(g_file_log.mutex);
    g_file_log.recent.emplace_back(g_file_log.count++, std::move(lower));
    if (g_file_log.recent.size() > 256) g_file_log.recent.erase(g_file_log.recent.begin());
  });
}

std::unique_ptr<InputDriver> CreateAutoplayScriptDriver(const std::string& path,
                                                        const std::string& dir,
                                                        std::function<uint32_t()> draws) {
  MacroMap macros;
  std::vector<Instr> program;
  if (!ParseScript(std::filesystem::path(path), macros, program, 0)) return nullptr;
  if (program.empty()) return nullptr;
  REXLOG_INFO("fh1_autoplay_file: {} ({} steps, {} macros)", path, program.size(), macros.size());
  InstallFileObserver(false);
  auto runner = std::make_shared<ScriptRunner>(std::move(program), dir, std::move(draws));
  runner->Start();
  ScriptRunner* raw = runner.get();
  return std::make_unique<AutoplayDriver>([raw] { return raw->Current(); }, runner);
}

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
    if (!ParseControls(entry.substr(eq + 1), step)) {
      REXLOG_ERROR("fh1_autoplay: unknown control in '{}'", entry);
      return nullptr;
    }
    steps.push_back(step);
  }
  if (steps.empty()) return nullptr;
  REXLOG_INFO("fh1_autoplay: {} scripted input steps", steps.size());
  return std::make_unique<AutoplayDriver>(std::move(steps));
}

}  // namespace fh1
