// nfsmw - automated tests on the PC (see nfsmw_test_entry.h).

#include "nfsmw_test_entry.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <atomic>
#include <vector>

REXCVAR_DEFINE_STRING(nfsmw_test_buttons, "", "NFSMW",
                      "Tests: dash de buttons de un controller virtual, en seconds since el "
                      "arranque. \"92:start,98:a,130-160:rt\" pulsa START a los 92 s, A a los 98 s y "
                      "mantiene el gatillo right de 130 a 160 s. Buttons: a b x y start back up "
                      "down left right lb rb lt rt, y de la stick left "
                      "stick_up stick_down stick_left stick_right")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Race tests: the car has to move forward following the track without leaving it. The button script only
// holds the trigger, and the car ended up against a wall. The game has a script command "ForceAIControl"
// (registered with sub_8237B858 at 0x8235ED28; handler sub_82367128): with r3 = 0 it takes player 1 from the
// list 0x82C74BA0 (sub_82366FB0), looks up its AI interface (sub_8231AF38) and, unless it is already active,
// enables AI control (virtual function +16 with 1). It is what the game does when crossing the finish line:
// the AI drives the player's car along the racing line.
REXCVAR_DEFINE_BOOL(nfsmw_test_ia_drives, false, "NFSMW",
                    "Tests: en every race la IA del game drives el car del player since el principio "
                    "(command ForceAIControl), para que advance siguiendo el circuito")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Enabled after the countdown: when it was enabled 30 frames after entering the race (during the intro, with
// the script's A presses already inside the race), the car ended up flipped and off the track.
REXCVAR_DEFINE_DOUBLE(nfsmw_test_ia_drives_delay_s, 12.0, "NFSMW",
                      "Tests: seconds since que begins la race (state 6) until que la IA take el car "
                      "del player (nfsmw_test_ia_drives)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Alley shortcut in Ironwood Estates: the robotic-audio tests have to go through it, and the game's AI does
// not take it. The command registered right before ForceAIControl (handler sub_823671C0) does the opposite:
// with r3 = 0 it takes player 1, looks up its AI interface and, if the AI is driving, calls virtual function
// +16 with 0 and gives the car back to the player.
REXCVAR_DEFINE_DOUBLE(nfsmw_test_ia_loose_s, 0.0, "NFSMW",
                      "Tests: seconds since que begins la race (state 6) until que se le quita el car a la "
                      "IA (nfsmw_test_ia_drives); 0 = never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nfsmw_test_after_release, "", "NFSMW",
                      "Tests: buttons del controller virtual since que se loose la IA, en seconds counted since ese "
                      "momento, con el format de nfsmw_test_buttons (\"0-15:rt\" acelera 15 s)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Player car position: the AI does not repeat the race identically (in one run it passed the checkpoint at
// 41.5 s and in another at 53 s, with traffic in between), so releasing it at a fixed time does not bring
// the car to the alley. Steering it requires knowing where it is: these dumps store the interfaces of
// player 1's car (the same ones ForceAIControl uses) and what they point to, to find the position and
// speed offline.
REXCVAR_DEFINE_STRING(nfsmw_test_dump_car, "", "NFSMW",
                      "Tests: seconds since que begins la race (state 6) en los que se vuelca la memory_block del "
                      "car del player a logs/car_N.bin, separados por comas (\"30,30.25,30.5,31\"); empty = "
                      "never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Alley shortcut autopilot: on the home straight the car is taken from the AI and the left stick drives it
// along a route of points (world meters, taken from the trace) with the throttle floored. After the last
// point, or after nfsmw_test_pilot_max_s seconds, the AI drives again (ForceAIControl). It rearms when
// the car is 300 m away from the first point, so in a 2-lap race it runs twice. The alley turns left about
// 225 m from the home straight: with a single straight segment the autopilot dragged the car along the
// right-hand wall.
REXCVAR_DEFINE_BOOL(nfsmw_test_trace_car, false, "NFSMW",
                    "Tests: guarda en logs/trace_car.csv la position, la speed y el rumbo del car del "
                    "player en every frame de race")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nfsmw_test_pilot, "", "NFSMW",
                      "Tests: path \"x1,y1,x2,y2[,x3,y3...]\" (meters del world) por la que el pilot lleva el "
                      "car del player cuando pasa a minus de nfsmw_test_pilot_radio del first punto; empty = "
                      "sin pilot")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_test_pilot_radio, 20.0, "NFSMW",
                      "Tests: distance a A (m) a la que el pilot le quita el car a la IA")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_test_pilot_lead, 25.0, "NFSMW",
                      "Tests: meters por ahead, over la line, a los que apunta el pilot")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_test_pilot_angle, 25.0, "NFSMW",
                      "Tests: degrees de error de rumbo con los que el pilot gira el stick del todo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_test_pilot_sign, 1.0, "NFSMW",
                      "Tests: 1 o -1, direction del stick del pilot")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_test_pilot_max_s, 12.0, "NFSMW",
                      "Tests: seconds as mucho que drives el pilot before de devolverle el car a la IA")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(nfsmw_test_pilot_zone_dead, 0.24, "NFSMW",
                      "Tests: fraction del stick por debajo de la which el game no gira; the corrections del "
                      "pilot empiezan ahi (0,24 es la zone dead_2 habitual de XInput)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824411B8);
REX_EXTERN(__imp__sub_82366FB0);
REX_EXTERN(__imp__sub_82367128);
REX_EXTERN(__imp__sub_823671C0);

namespace nfsmw::test {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
namespace in = rex::input;

constexpr in::DeviceId kControllerDash = in::DeviceId(0x4E46534D57475549ull);  // "NFSMWGUI"
constexpr double kPress = 0.25;  // seconds of a single press

// X_INPUT_GAMEPAD bits; the triggers go in bits 16 and 17. The key is the
// XInputGetKeystroke VK_PAD code (ui/virtual_key.h).
struct Button {
  std::string_view name;
  uint32_t bits;
  uint16_t key;
};
constexpr Button kButtons[] = {
    {"a", 0x1000, 0x5800},          {"b", 0x2000, 0x5801},
    {"x", 0x4000, 0x5802},          {"y", 0x8000, 0x5803},
    {"rb", 0x0200, 0x5804},         {"lb", 0x0100, 0x5805},
    {"lt", 1u << 16, 0x5806},       {"rt", 1u << 17, 0x5807},
    {"up", 0x0001, 0x5810},     {"down", 0x0002, 0x5811},
    {"left", 0x0004, 0x5812},  {"right", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814},      {"back", 0x0020, 0x5815},
    // The left stick. The "create a profile?" dialog ignores the D-pad and only responds to the stick, so
    // without this there was no way to reach a save from the script. The codes are the real ones
    // (VK_PAD_LTHUMB_UP and following), for the menus that read keystrokes instead of state.
    {"stick_up", 1u << 18, 0x5820},     {"stick_down", 1u << 19, 0x5821},
    {"stick_right", 1u << 20, 0x5822},    {"stick_left", 1u << 21, 0x5823},
};

struct Step {
  double start;
  double fin;
  uint32_t bits;
};

bool Number(std::string_view text, double& input_value) {
  const std::string copy(text);
  char* fin = nullptr;
  input_value = std::strtod(copy.c_str(), &fin);
  return !copy.empty() && fin == copy.c_str() + copy.size() && input_value >= 0.0;
}

// "T:button" presses briefly; "T1-T2:button" holds. Separated by commas,
// spaces or semicolons.
std::vector<Step> Read(std::string_view text) {
  std::vector<Step> steps;
  size_t i = 0;
  const auto separator = [](char c) { return c == ' ' || c == ',' || c == ';'; };
  while (i < text.size()) {
    while (i < text.size() && separator(text[i])) {
      ++i;
    }
    size_t j = i;
    while (j < text.size() && !separator(text[j])) {
      ++j;
    }
    if (j == i) {
      break;
    }
    const std::string_view step = text.substr(i, j - i);
    i = j;
    const size_t two_points = step.find(':');
    const std::string_view times = step.substr(0, two_points);
    const std::string_view name =
        two_points == std::string_view::npos ? std::string_view() : step.substr(two_points + 1);
    const Button* button = nullptr;
    for (const Button& b : kButtons) {
      if (b.name == name) {
        button = &b;
      }
    }
    const size_t dash = times.find('-');
    double start = 0.0;
    double fin = 0.0;
    const bool ok = button && Number(times.substr(0, dash), start) &&
                      (dash == std::string_view::npos
                           ? (fin = start + kPress, true)
                           : (Number(times.substr(dash + 1), fin) && fin > start));
    if (!ok) {
      REXLOG_WARN("[test] controller del dash: step no valid \"{}\": se ignora", step);
      continue;
    }
    steps.push_back({start, fin, button->bits});
  }
  return steps;
}

// Buttons of nfsmw_test_after_release at this instant: the game's main thread writes them (IaDrives) and
// the virtual pad adds them. A value that changes a few times per race, with no waiting between threads.
std::atomic<uint32_t> g_bits_after_release{0};
// Left stick, horizontal axis (negative = left): set by the shortcut autopilot from the main thread.
std::atomic<int16_t> g_stick_lx{0};

class ControllerDash final : public in::InputDriver {
 public:
  explicit ControllerDash(std::vector<Step> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)), start_(Clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kControllerDash;
    info.name = "Controller del dash de tests";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kControllerDash) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint32_t packet = 0;
    int16_t stick = 0;
    const uint32_t bits = Update(packet, stick);
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      // The script's stick overrides the autopilot while held, which is what the menus need.
      int16_t stick_y = 0;
      if ((bits >> 18) & 0x1) stick_y = 30000;
      if ((bits >> 19) & 0x1) stick_y = -30000;
      if ((bits >> 20) & 0x1) stick = 30000;
      if ((bits >> 21) & 0x1) stick = -30000;
      out_state->gamepad.thumb_lx = stick;
      out_state->gamepad.thumb_ly = stick_y;
      out_state->gamepad.left_trigger = ((bits >> 16) & 0x1) ? 0xFF : 0;
      out_state->gamepad.right_trigger = ((bits >> 17) & 0x1) ? 0xFF : 0;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags,
                                 in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kControllerDash) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      // Like the SDK's empty pad (nop_input_driver.cpp:47-69).
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      // The sticks too, like the empty pad: with 0 the game did not steer with the autopilot's stick.
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kControllerDash ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags,
                              in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != kControllerDash) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    uint32_t packet = 0;
    int16_t stick = 0;
    Update(packet, stick);
    std::lock_guard<std::mutex> lock(mutex_);
    if (keys_.empty()) {
      return X_ERROR_EMPTY;
    }
    if (out_keystroke) {
      *out_keystroke = keys_.front();
    }
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  // Script buttons at this instant and the autopilot's stick. Each change bumps packet_number and queues
  // the pressed and released keys, for the menus that read keys.
  uint32_t Update(uint32_t& packet, int16_t& stick) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double t = std::chrono::duration<double>(Clock::now() - start_).count();
    uint32_t bits = g_bits_after_release.load(std::memory_order_relaxed);
    stick = g_stick_lx.load(std::memory_order_relaxed);
    for (const Step& p : steps_) {
      if (t >= p.start && t < p.fin) {
        bits |= p.bits;
      }
    }
    if (bits != current_ || stick != stick_) {
      for (const Button& b : kButtons) {
        const bool before = (current_ & b.bits) != 0;
        const bool now = (bits & b.bits) != 0;
        if (before == now) {
          continue;
        }
        in::X_INPUT_KEYSTROKE key{};
        key.virtual_key = b.key;
        key.flags = uint16_t(now ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
        if (keys_.size() < 64) {
          keys_.push_back(key);
        }
        REXLOG_INFO("[test] controller del dash: {} {} a los {:.1f} s", b.name,
                    now ? "pulsado" : "suelto", t);
      }
      current_ = bits;
      stick_ = stick;
      ++packet_;
    }
    packet = packet_;
    return bits;
  }

  const std::vector<Step> steps_;
  const Clock::time_point start_;
  std::mutex mutex_;
  uint32_t current_ = 0;
  int16_t stick_ = 0;
  uint32_t packet_ = 1;
  std::deque<in::X_INPUT_KEYSTROKE> keys_;
};

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

constexpr uint32_t kStateGame = 0x82A39AD8;
constexpr uint32_t kStateRace = 6;
// Only from the game's main thread.
bool g_in_race = false;
Clock::time_point g_start_race;
bool g_ia_enabled = false;
bool g_ia_released = false;
Clock::time_point g_released;
std::vector<Step> g_steps_after_release;
bool g_steps_after_release_read = false;

// Calls a game function for player 1 in the middle of another one: saves and restores the volatile
// registers. Returns the r3 the function returns with.
uint32_t CommandPlayer1(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*)) {
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  const uint64_t r7 = ctx.r7.u64, r8 = ctx.r8.u64, r9 = ctx.r9.u64, r10 = ctx.r10.u64;
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = 0;  // player 1
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r7.u64 = r7;
  ctx.r8.u64 = r8;
  ctx.r9.u64 = r9;
  ctx.r10.u64 = r10;
  ctx.lr = lr;
  return result;
}

// After releasing the AI: the nfsmw_test_after_release buttons due now.
void ButtonsAfterRelease() {
  if (!g_steps_after_release_read) {
    g_steps_after_release_read = true;
    g_steps_after_release = Read(REXCVAR_GET(nfsmw_test_after_release));
  }
  const double t = std::chrono::duration<double>(Clock::now() - g_released).count();
  uint32_t bits = 0;
  for (const Step& p : g_steps_after_release) {
    if (t >= p.start && t < p.fin) {
      bits |= p.bits;
    }
  }
  g_bits_after_release.store(bits, std::memory_order_relaxed);
}

// Dumps for nfsmw_test_dump_car. Only from the game's main thread.
std::vector<double> g_times_dump;
bool g_times_dump_read = false;
size_t g_next_dump = 0;
int g_dumps = 0;

// Guest memory with pages readable on the host, to follow pointers without going out of bounds. Excludes
// the high physical range (from 0xE0000000 it carries the REX_PHYS_HOST_OFFSET offset).
bool Readable(uint8_t* base, uint32_t address, uint32_t size) {
  if (size == 0 || address < 0x40000000u || uint64_t(address) + size > 0xE0000000ull) {
    return false;
  }
  const uint64_t page = rex::memory::page_size();
  uint64_t actual = address;
  const uint64_t fin = uint64_t(address) + size;
  while (actual < fin) {
    size_t is_long = size_t(fin - actual);
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    if (!rex::memory::QueryProtect(base + actual, is_long, access) ||
        (uint32_t(access) & uint32_t(rex::memory::PageAccess::kReadOnly)) == 0) {
      return false;
    }
    // On Windows the length counts from the start of the page.
    const uint64_t next = (actual & ~(page - 1)) + is_long;
    if (next <= actual) {
      return false;
    }
    actual = next;
  }
  return true;
}

// Dumps to logs/car_N.bin the memory of player 1's car interfaces and of what they point to.
// sub_82366FB0(0) returns the player's car interface; at +4 is its COM object, with the sorted list of
// {identifier, interface} pairs between +4 and +8 (walked by sub_8231AF38). Format: "NFSCOCHE", u32
// version (1), f64 race seconds and blocks {u32 address, u32 size, u32 source (where the pointer was;
// 0 = direct), guest bytes}; the headers in little endian.
void DumpCar(PPCContext& ctx, uint8_t* base, double seconds) {
  const uint32_t interfaz = CommandPlayer1(ctx, base, __imp__sub_82366FB0);
  const uint32_t object = Readable(base, interfaz, 8) ? Read32(base, interfaz + 4) : 0;
  if (!Readable(base, object, 16)) {
    REXLOG_WARN("[test] car del player a los {:.2f} s: sin object (interfaz {:08X}, object {:08X})", seconds,
                interfaz, object);
    return;
  }
  const uint32_t list = Read32(base, object + 4);
  const uint32_t list_end = Read32(base, object + 8);
  uint32_t interfaces = 0;
  if (list_end > list && (list_end - list) % 8 == 0 && Readable(base, list, list_end - list)) {
    interfaces = std::min<uint32_t>((list_end - list) / 8, 64);
  }

  struct Block {
    uint32_t address;
    uint32_t size;
    uint32_t source;
    bool explore;
  };
  std::vector<Block> blocks;
  std::set<uint32_t> views;
  const auto add = [&](uint32_t address, uint32_t size, uint32_t source, bool explore) {
    if (views.insert(address).second && Readable(base, address, size)) {
      blocks.push_back({address, size, source, explore});
    }
  };
  add(interfaz - 0x100, 0x1000, 0, true);
  add(object, 0x40, 0, false);
  add(list, interfaces * 8, 0, false);
  std::string summary;
  for (uint32_t i = 0; i < interfaces; ++i) {
    const uint32_t identifier = Read32(base, list + i * 8);
    const uint32_t pointer = Read32(base, list + i * 8 + 4);
    const uint32_t vtable = Readable(base, pointer, 4) ? Read32(base, pointer) : 0;
    char text[32];
    std::snprintf(text, sizeof(text), " %08X=%08X/%08X", identifier, pointer, vtable);
    summary += text;
    add(pointer - 0x100, 0x1000, 0, true);
  }
  // One level of pointers from the first 0x500 bytes of each interface, excluding the executable image
  // (vtables and globals).
  const size_t direct = blocks.size();
  for (size_t b = 0; b < direct; ++b) {
    if (!blocks[b].explore) {
      continue;
    }
    for (uint32_t displacement = 0x100; displacement + 4 <= 0x600 && blocks.size() < 3000;
         displacement += 4) {
      const uint32_t input_value = Read32(base, blocks[b].address + displacement);
      if ((input_value & 3) != 0 || (input_value >= 0x80000000u && input_value < 0x90000000u)) {
        continue;
      }
      add(input_value, 0x400, blocks[b].address + displacement, false);
    }
  }

  char path[48];
  std::snprintf(path, sizeof(path), "logs/car_%d.bin", g_dumps++);
  std::FILE* file = std::fopen(path, "wb");
  if (!file) {
    REXLOG_WARN("[test] car del player: no se can create {}", std::string_view(path));
    return;
  }
  const uint32_t version = 1;
  std::fwrite("NFSCOCHE", 1, 8, file);
  std::fwrite(&version, sizeof(version), 1, file);
  std::fwrite(&seconds, sizeof(seconds), 1, file);
  uint64_t bytes = 0;
  for (const Block& b : blocks) {
    std::fwrite(&b.address, sizeof(b.address), 1, file);
    std::fwrite(&b.size, sizeof(b.size), 1, file);
    std::fwrite(&b.source, sizeof(b.source), 1, file);
    std::fwrite(base + b.address, 1, b.size, file);
    bytes += b.size;
  }
  std::fclose(file);
  REXLOG_INFO("[test] car del player a los {:.2f} s: interfaz {:08X}, object {:08X}, {} interfaces:{}; {} "
              "blocks ({} bytes) en {}",
              seconds, interfaz, object, interfaces, summary, blocks.size(), bytes, std::string_view(path));
}

// nfsmw_test_dump_car: one dump per time in the list, in order.
void DumpsCar(PPCContext& ctx, uint8_t* base, double seconds) {
  if (!g_times_dump_read) {
    g_times_dump_read = true;
    const std::string text = REXCVAR_GET(nfsmw_test_dump_car);
    size_t i = 0;
    while (i < text.size()) {
      size_t j = text.find(',', i);
      if (j == std::string::npos) {
        j = text.size();
      }
      double input_value = 0.0;
      if (Number(std::string_view(text).substr(i, j - i), input_value)) {
        g_times_dump.push_back(input_value);
      }
      i = j + 1;
    }
    std::sort(g_times_dump.begin(), g_times_dump.end());
  }
  if (g_next_dump < g_times_dump.size() && seconds >= g_times_dump[g_next_dump]) {
    ++g_next_dump;
    DumpCar(ctx, base, seconds);
  }
}

// Player car state in world coordinates (Z up), from the car's main interface.
struct StateCar {
  double x, y, z;
  double vx, vy, vz;
  double fx, fy;  // "forward" row of the world matrix
};
// Only from the game's main thread; looked up again in every race.
uint32_t g_interfaz_car = 0;

double ReadF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float input_value = 0.0f;
  std::memcpy(&input_value, &bits, sizeof(input_value));
  return input_value;
}

bool ReadCar(PPCContext& ctx, uint8_t* base, StateCar& e) {
  if (g_interfaz_car == 0) {
    const uint32_t interfaz = CommandPlayer1(ctx, base, __imp__sub_82366FB0);
    if (!Readable(base, interfaz, 0x900)) {
      return false;
    }
    g_interfaz_car = interfaz;
    REXLOG_INFO("[test] car del player: interfaz {:08X}", interfaz);
  }
  const uint32_t i = g_interfaz_car;
  e.fx = ReadF32(base, i + 0x854);
  e.fy = ReadF32(base, i + 0x858);
  e.x = ReadF32(base, i + 0x884);
  e.y = ReadF32(base, i + 0x888);
  e.z = ReadF32(base, i + 0x88C);
  e.vx = ReadF32(base, i + 0x894);
  e.vy = ReadF32(base, i + 0x898);
  e.vz = ReadF32(base, i + 0x89C);
  return std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && std::isfinite(e.vx) &&
         std::isfinite(e.vy) && std::isfinite(e.fx) && std::isfinite(e.fy);
}

// logs/trace_car.csv (nfsmw_test_trace_car). Flushed every half second: the tests end by killing the
// process. mode: 0 = no AI, 1 = AI, 2 = AI released by time, 3 = autopilot.
std::FILE* g_trace = nullptr;
bool g_trace_tried = false;
int g_trace_lines = 0;

void Trace(double seconds, const StateCar& e, int mode, int16_t stick) {
  if (!g_trace_tried) {
    g_trace_tried = true;
    g_trace = std::fopen("logs/trace_car.csv", "w");
    if (!g_trace) {
      REXLOG_WARN("[test] trace del car: no se can create logs/trace_car.csv");
      return;
    }
    std::fprintf(g_trace, "clock_ms,seconds,x,y,z,vx,vy,vz,fx,fy,mode,stick\n");
  }
  if (!g_trace) {
    return;
  }
  const long long clock = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
  std::fprintf(g_trace, "%lld,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%d,%d\n", clock, seconds, e.x, e.y, e.z,
               e.vx, e.vy, e.vz, e.fx, e.fy, mode, int(stick));
  if (++g_trace_lines % 30 == 0) {
    std::fflush(g_trace);
  }
}

// Autopilot route (nfsmw_test_pilot): signed world x,y points; the first is where the AI is released.
// g_accumulated: meters from the first point to each point. g_range: the segment from g_range - 1 to g_range.
enum class Pilot { kWaiting, kDriving, kFinished };
Pilot g_pilot = Pilot::kWaiting;
bool g_line_read = false;
bool g_there_is_line = false;
std::vector<double> g_px, g_py, g_accumulated;
size_t g_range = 1;
Clock::time_point g_pilot_start;
double g_pilot_report = 0.0;
constexpr uint32_t kBitRt = 1u << 17;
constexpr double kPi = 3.14159265358979323846;

bool ReadLine() {
  if (g_line_read) {
    return g_there_is_line;
  }
  g_line_read = true;
  const std::string text = REXCVAR_GET(nfsmw_test_pilot);
  if (text.empty()) {
    return false;
  }
  std::vector<double> numbers;
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = text.find(',', i);
    if (j == std::string::npos) {
      j = text.size();
    }
    const std::string chunk = text.substr(i, j - i);
    char* fin = nullptr;
    const double input_value = std::strtod(chunk.c_str(), &fin);
    if (chunk.empty() || fin != chunk.c_str() + chunk.size() || !std::isfinite(input_value)) {
      numbers.clear();
      break;
    }
    numbers.push_back(input_value);
    i = j + 1;
  }
  if (numbers.size() < 4 || numbers.size() % 2 != 0) {
    REXLOG_WARN("[test] pilot: path no valid \"{}\" (hacen missing al minus 2 points x,y)", text);
    return false;
  }
  for (size_t k = 0; k < numbers.size(); k += 2) {
    g_px.push_back(numbers[k]);
    g_py.push_back(numbers[k + 1]);
  }
  g_accumulated.assign(g_px.size(), 0.0);
  for (size_t k = 1; k < g_px.size(); ++k) {
    const double is_long = std::hypot(g_px[k] - g_px[k - 1], g_py[k] - g_py[k - 1]);
    if (is_long < 1.0) {
      REXLOG_WARN("[test] pilot: path no valid \"{}\" (los points {} y {} estan a minus de 1 m)", text, k,
                  k + 1);
      g_px.clear();
      g_py.clear();
      g_accumulated.clear();
      return false;
    }
    g_accumulated[k] = g_accumulated[k - 1] + is_long;
  }
  g_there_is_line = true;
  REXLOG_INFO("[test] pilot: path de {} points since ({:.1f}, {:.1f}), {:.1f} m", g_px.size(), g_px[0], g_py[0],
              g_accumulated.back());
  return true;
}

// Route point 'meters' from the first point; before the first and after the last it extends the end
// segment.
void PuntoOfPath(double meters, double& x, double& y) {
  size_t k = 1;
  while (k + 1 < g_px.size() && g_accumulated[k] < meters) {
    ++k;
  }
  const double f = (meters - g_accumulated[k - 1]) / (g_accumulated[k] - g_accumulated[k - 1]);
  x = g_px[k - 1] + (g_px[k] - g_px[k - 1]) * f;
  y = g_py[k - 1] + (g_py[k] - g_py[k - 1]) * f;
}

// Shortcut autopilot: returns true while it drives the car. Pure pursuit: it aims at a route point
// nfsmw_test_pilot_lead meters ahead of the car's projection onto its segment.
bool Pilot(PPCContext& ctx, uint8_t* base, double seconds, const StateCar& e) {
  const double distance_a = std::hypot(e.x - g_px[0], e.y - g_py[0]);
  const double speed = std::hypot(e.vx, e.vy);
  if (g_pilot == Pilot::kFinished) {
    if (distance_a > 300.0) {
      g_pilot = Pilot::kWaiting;
    }
    return false;
  }
  if (g_pilot == Pilot::kWaiting) {
    if (!g_ia_enabled || g_ia_released || distance_a > REXCVAR_GET(nfsmw_test_pilot_radio)) {
      return false;
    }
    CommandPlayer1(ctx, base, __imp__sub_823671C0);
    g_pilot = Pilot::kDriving;
    g_pilot_start = Clock::now();
    g_pilot_report = 0.0;
    g_range = 1;
    REXLOG_INFO("[test] pilot: loose la IA a {:.1f} m del first punto, en ({:.1f}, {:.1f}), {:.1f} m/s, a los "
                "{:.1f} s de race",
                distance_a, e.x, e.y, speed, seconds);
  }
  // Projection onto the current segment; moves to the next one when the projection passes the segment's end.
  double route = 0.0;
  double lateral = 0.0;  // > 0: left of the segment
  for (;;) {
    const size_t k = g_range;
    const double is_long = g_accumulated[k] - g_accumulated[k - 1];
    const double ux = (g_px[k] - g_px[k - 1]) / is_long;
    const double uy = (g_py[k] - g_py[k - 1]) / is_long;
    const double rx = e.x - g_px[k - 1];
    const double ry = e.y - g_py[k - 1];
    const double a_lo_long = rx * ux + ry * uy;
    if (a_lo_long > is_long && k + 1 < g_px.size()) {
      ++g_range;
      continue;
    }
    route = g_accumulated[k - 1] + a_lo_long;
    lateral = -rx * uy + ry * ux;
    break;
  }
  const double total = g_accumulated.back();
  const double t = std::chrono::duration<double>(Clock::now() - g_pilot_start).count();
  if (route >= total || t >= REXCVAR_GET(nfsmw_test_pilot_max_s)) {
    g_stick_lx.store(0, std::memory_order_relaxed);
    g_bits_after_release.store(0, std::memory_order_relaxed);
    CommandPlayer1(ctx, base, __imp__sub_82367128);
    g_pilot = Pilot::kFinished;
    REXLOG_INFO("[test] pilot: la IA vuelve a conducir en ({:.1f}, {:.1f}) after {:.1f} s, range {}, route "
                "{:.1f} de {:.1f} m, lateral {:+.1f} m, {:.1f} m/s",
                e.x, e.y, t, g_range, route, total, lateral, speed);
    return true;
  }
  double tx = 0.0;
  double ty = 0.0;
  PuntoOfPath(route + REXCVAR_GET(nfsmw_test_pilot_lead), tx, ty);
  tx -= e.x;
  ty -= e.y;
  const double hx = speed > 5.0 ? e.vx : e.fx;
  const double hy = speed > 5.0 ? e.vy : e.fy;
  const double error = std::atan2(hx * ty - hy * tx, hx * tx + hy * ty);  // > 0: the target is to the left
  const double complete = REXCVAR_GET(nfsmw_test_pilot_angle) * kPi / 180.0;
  double controller = std::clamp(-error / complete, -1.0, 1.0) * REXCVAR_GET(nfsmw_test_pilot_sign);
  // Below the dead zone the game does not steer: corrections start at its edge, except errors under 2 % of
  // full lock, which stay centered to avoid swerving.
  const double zone = std::clamp(REXCVAR_GET(nfsmw_test_pilot_zone_dead), 0.0, 0.9);
  controller = std::abs(controller) < 0.02 ? 0.0 : std::copysign(zone + (1.0 - zone) * std::abs(controller), controller);
  const int16_t stick = int16_t(std::lround(std::clamp(controller, -1.0, 1.0) * 32767.0));
  g_stick_lx.store(stick, std::memory_order_relaxed);
  g_bits_after_release.store(kBitRt, std::memory_order_relaxed);
  if (t >= g_pilot_report) {
    g_pilot_report += 0.5;
    REXLOG_INFO("[test] pilot: {:.1f} s, ({:.1f}, {:.1f}), range {}, route {:.1f} m, lateral {:+.1f} m, "
                "error {:+.1f} degrees, stick {}, {:.1f} m/s",
                t, e.x, e.y, g_range, route, lateral, error * 180.0 / kPi, stick, speed);
  }
  return true;
}

}  // namespace

// Once per race, nfsmw_test_ia_drives_delay_s after entering the race state (after the intro and the
// countdown), the game's ForceAIControl command for player 1. It rearms when leaving the race.
void IaDrives(PPCContext& ctx, uint8_t* base) {
  if (Read32(base, kStateGame) != kStateRace) {
    g_in_race = false;
    g_ia_enabled = false;
    g_ia_released = false;
    g_bits_after_release.store(0, std::memory_order_relaxed);
    g_next_dump = 0;
    g_interfaz_car = 0;
    if (g_pilot == Pilot::kDriving) {
      g_stick_lx.store(0, std::memory_order_relaxed);
    }
    g_pilot = Pilot::kWaiting;
    if (g_trace) {
      std::fflush(g_trace);
    }
    return;
  }
  if (!g_in_race) {
    g_in_race = true;
    g_start_race = Clock::now();
    return;
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - g_start_race).count();
  DumpsCar(ctx, base, seconds);
  const bool line = ReadLine();
  const bool trace = REXCVAR_GET(nfsmw_test_trace_car);
  if (line || trace) {
    StateCar car{};
    if (ReadCar(ctx, base, car)) {
      const bool piloting = line && Pilot(ctx, base, seconds, car);
      if (trace) {
        const int mode = g_pilot == Pilot::kDriving ? 3 : g_ia_released ? 2 : g_ia_enabled ? 1 : 0;
        Trace(seconds, car, mode, g_stick_lx.load(std::memory_order_relaxed));
      }
      if (piloting) {
        return;
      }
    }
  }
  if (g_ia_released) {
    ButtonsAfterRelease();
    return;
  }
  if (g_ia_enabled) {
    // Alley shortcut: after nfsmw_test_ia_loose_s seconds of racing the car is taken from the AI.
    const double loose_s = REXCVAR_GET(nfsmw_test_ia_loose_s);
    if (loose_s > 0.0 && seconds >= loose_s) {
      CommandPlayer1(ctx, base, __imp__sub_823671C0);
      g_ia_released = true;
      g_released = Clock::now();
      REXLOG_INFO("[test] race: la IA loose el car del player ({:.1f} s after de enter en la race); "
                  "buttons since now: \"{}\"",
                  seconds, REXCVAR_GET(nfsmw_test_after_release));
      ButtonsAfterRelease();
    }
    return;
  }
  if (seconds < REXCVAR_GET(nfsmw_test_ia_drives_delay_s)) {
    return;
  }
  CommandPlayer1(ctx, base, __imp__sub_82367128);
  g_ia_enabled = true;
  REXLOG_INFO("[test] race: la IA del game drives el car del player (ForceAIControl, {:.1f} s "
              "after de enter en la race)",
              seconds);
}

void WrapEntry(rex::RuntimeConfig& config) {
  const std::string dash = REXCVAR_GET(nfsmw_test_buttons);
  if (dash.empty() || !config.input_factory) {
    return;
  }
  std::vector<Step> steps = Read(dash);
  if (steps.empty()) {
    REXLOG_WARN("[test] controller del dash: \"{}\" no has steps validos", dash);
    return;
  }
  auto base = config.input_factory;
  config.input_factory =
      [base, steps](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      // The default factory is CreateDefaultInputSystem (ui/rex_app.cpp:337), which
      // returns a rex::input::InputSystem.
      static_cast<in::InputSystem*>(system.get())->AddDriver(std::make_unique<ControllerDash>(steps));
      REXLOG_INFO("[test] controller del dash con {} steps", steps.size());
    }
    return system;
  };
}

}  // namespace nfsmw::test

// Render of each game frame (main thread): used as the clock to enable the AI in a race.
REX_HOOK_RAW(sub_824411B8) {
  if (REXCVAR_GET(nfsmw_test_ia_drives)) {
    nfsmw::test::IaDrives(ctx, base);
  }
  __imp__sub_824411B8(ctx, base);
}
