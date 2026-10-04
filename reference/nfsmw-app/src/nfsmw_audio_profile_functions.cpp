// nfsmw - diagnostic: timing of the sound engine functions on the audio server thread
//
// On the console, the remaining robotic audio shows up in hard crashes, with the game's audio server thread at
// 70-94 % of a core: its mixing costs almost a whole core. To make it cheaper we need to know which sound
// engine functions take that time, on PC and on the console.
//
// With nfsmw_audio_diag_functions, only on the server thread (marked by its wait, in nfsmw_audio_server.cpp),
// each function in the list measures:
//  - its self time: its duration minus that of the other measured functions it calls. These can be added up;
//  - its inclusive time, counted only in the outermost call. The functions of the sound graph call themselves
//    recursively, and an earlier version that added up every level reached 391 % of the thread's CPU;
//  - its calls and its maximum self time in a 500 ms window (to catch the crashes).
// Every 10 s a summary sorted by self time is logged, with the thread's CPU. These are wall-clock times: they
// include the time the thread is blocked inside the function.
// When off, which is the normal case, each hook only checks the cvar and calls the original function.
//
// The large per-call self times did not come from the bodies of those functions but from what they call
// without being measured, often through pointers. That is why those calls are measured and their targets
// are logged:
//  - the voice's virtual call (sub_825EB0E8, method +8 of the object r3 + (byte [r5 + 73] + 16) * 8) goes to
//    sub_825E1CD0, which reads what the XMA decoder leaves;
//  - the effects pointer of sub_825DCED8 (0x82A2B1C8) points to sub_825FDFB0, the gain-scaled sum (native in
//    nfsmw_audio_suma.cpp);
//  - sub_825D24E0 walks the list at 0x82C5E214 (+0 next, +8 function, +12 argument) and calls each function;
//  - sub_825DA1E0 drains the command queue at 0x82A2AD38 (count at +0, 8-byte entries from +8 with type, index
//    and argument) and calls method +84 or +80 of the object in the table at +772.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_audio_native.h"

REXCVAR_DEFINE_BOOL(nfsmw_audio_diag_functions, false, "NFSMW",
                    "Diagnostic: time own, time inclusive y calls de the functions del motor de sonido del "
                    "game en el thread_value server de audio, con un summary every 10 s en el log");

namespace nfsmw::threads {
// nfsmw_threads_switch.cpp
uint64_t HandlerThreadCurrent();
int64_t CpuThreadUs(uint64_t handler);
}  // namespace nfsmw::threads

namespace nfsmw::audio_profile {

// Order of the kFunctions table.
enum Index : size_t {
  kCommands,
  kLoop825E4160,
  kLoop825D07C8,
  kGraph825CFCC8,
  kBlendPacket,
  k825DD288,
  k825DCED8,
  k825D2538,
  k825ED568,
  k825DBD68,
  k825DCCB0,
  k825DC0C0,
  kVoice,
  kResampling,
  kResamplingLinear,
  kResampling826033A0,
  k826022C8,
  k826047B0,
  k8262E220,
  k82619820,
  k82612270,
  k825E1370,
  k825CD088,
  kHelp826BDD90,
  k82601A08,
  k825D05F8,
  k825DE890,
  k826027F0,
  k825F2F60,
  k825D24E0,
  k825ED268,
  k825DA1E0,
  k825FDFB0,
  k825E1CD0,
  kNumber
};

namespace {

struct Function {
  const char* name;
  const char* role;
};

// Call tree seen in the recompiled code and in a PC profiling run: the server loop (sub_825E3E28) calls the
// commands and sub_825E4160 and sub_825D07C8; sub_825ED350 runs once per packet; sub_825EB0E8, once per voice.
constexpr std::array<Function, kNumber> kFunctions = {{
    {"sub_825CF780", "commands de audio"},
    {"sub_825E4160", "del loop"},
    {"sub_825D07C8", "del loop y del graph"},
    {"sub_825CFCC8", "graph de sonido"},
    {"sub_825ED350", "one time por packet"},
    {"sub_825DD288", "de 825ED350"},
    {"sub_825DCED8", "de 825DD288"},
    {"sub_825D2538", "de 825ED350"},
    {"sub_825ED568", "de 825ED350"},
    {"sub_825DBD68", "de 825DCED8, hoja"},
    {"sub_825DCCB0", "de 825DCED8"},
    {"sub_825DC0C0", "de 825DCED8"},
    {"sub_825EB0E8", "one time por voice"},
    {"sub_82602BE0", "llama a los resamplers"},
    {"sub_826031C0", "remuestreador linear"},
    {"sub_826033A0", "other remuestreador"},
    {"sub_826022C8", "de 825EB0E8"},
    {"sub_826047B0", "de 826022C8, 9 times"},
    {"sub_8262E220", "de 825EB0E8"},
    {"sub_82619820", "hoja"},
    {"sub_82612270", "call indirect"},
    {"sub_825E1370", "change de bytes"},
    {"sub_825CD088", "call since 1 room"},
    {"sub_826BDD90", "help con 950 llamadores"},
    {"sub_82601A08", "de 825E4160, hoja"},
    {"sub_825D05F8", "de 825D07C8, hoja"},
    {"sub_825DE890", "memory_block de work de la voice y del packet"},
    {"sub_826027F0", "de 825EB0E8"},
    {"sub_825F2F60", "de 825ED350"},
    {"sub_825D24E0", "list de 0x82C5E214, por range"},
    {"sub_825ED268", "de 825ED350, por range"},
    {"sub_825DA1E0", "queue de commands de 0x82A2AD38, por range"},
    {"sub_825FDFB0", "sum con gain, de 825DCED8"},
    {"sub_825E1CD0", "read de la voice (XMA)"},
}};

struct Count {
  uint64_t calls = 0;
  int64_t own_ns = 0;
  int64_t inclusive_ns = 0;
  int64_t own_window_ns = 0;
  int64_t max_own_window_ns = 0;
};

// Targets of a call through a pointer.
struct Target {
  uint32_t address = 0;
  uint64_t times = 0;
};

struct Targets {
  std::array<Target, 8> table{};
  uint64_t others = 0;     // did not fit in the table
  uint64_t without_read = 0;  // pointers outside the expected ranges

  void Note(uint32_t target) {
    if (target < 0x82000000 || target >= 0x83000000) {
      ++without_read;
      return;
    }
    for (Target& d : table) {
      if (d.address == target || d.times == 0) {
        d.address = target;
        ++d.times;
        return;
      }
    }
    ++others;
  }

  std::string Text() const {
    std::string text;
    for (const Target& d : table) {
      if (d.times != 0) {
        text += fmt::format("{}sub_{:08X} {}", text.empty() ? "" : ", ", d.address, d.times);
      }
    }
    return fmt::format("{}; others targets {}, sin read {}", text.empty() ? std::string("ninguna") : text, others,
                       without_read);
  }
};

class Stack;

// Only the server thread touches all of this.
std::array<Count, kNumber> g_counts{};
std::array<int, kNumber> g_active{};  // calls in progress for each function: detects recursion
int64_t g_measured_ns = 0;               // time of the outermost measured calls, = sum of the self times
int64_t g_since_ns = 0;
int64_t g_window_since_ns = 0;
int64_t g_cpu_before_us = -1;
uint64_t g_handler = 0;
thread_local bool t_server = false;
Targets g_targets_voice;
Targets g_targets_effect;
Targets g_targets_list;
Targets g_targets_queue;

constexpr int64_t kWindowNs = 500'000'000;
constexpr int64_t kSummaryNs = 10'000'000'000;
// sub_825DCED8: lis r9,-32093; addi r31,r9,-20288 (0x82A2B0C0) and lwz r4,264(r31) before its bctrl.
constexpr uint32_t kPointerEffect = 0x82A2B0C0 + 264;
// sub_825D24E0: lis r11,-32058; lwz r11,-7660(r11).
constexpr uint32_t kHeadList = 0x82C5E214;
// sub_825DA1E0: lis r11,-32093; addi r30,r11,-21192.
constexpr uint32_t kQueueCommands = 0x82A2AD38;

int64_t NowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void CloseWindow() {
  for (Count& c : g_counts) {
    c.max_own_window_ns = std::max(c.max_own_window_ns, c.own_window_ns);
    c.own_window_ns = 0;
  }
}

// The same reads sub_825EB0E8 does before its bctrl (0x825EB16C-0x825EB190), done on entry. Only pointers
// that land where expected are followed: the object in the heap (above 64 KB) and the virtual table in the
// XEX image.
void NoteTargetVoice(uint8_t* base, uint32_t r3, uint32_t r5) {
  using nfsmw::audio_native::Dir;
  using nfsmw::audio_native::Read32;
  const uint32_t index = *Dir(base, r5 + 73);
  const uint32_t source_2 = r3 >= 0x10000 ? Read32(base, r3 + ((index + 16) << 3)) : 0;
  const uint32_t table = source_2 >= 0x10000 ? Read32(base, source_2) : 0;
  const bool table_valid = table >= 0x82000000 && table < 0x83000000;
  g_targets_voice.Note(table_valid ? Read32(base, table + 8) : 0);
}

void NoteTargetEffect(uint8_t* base) {
  g_targets_effect.Note(nfsmw::audio_native::Read32(base, kPointerEffect));
}

// The functions of the list that sub_825D24E0 is about to walk (at most 32 nodes).
void NoteTargetsList(uint8_t* base) {
  using nfsmw::audio_native::Read32;
  uint32_t node = Read32(base, kHeadList);
  for (int i = 0; i < 32 && node >= 0x10000; ++i) {
    g_targets_list.Note(Read32(base, node + 8));
    node = Read32(base, node + 0);
  }
}

// The methods sub_825DA1E0 is about to call when draining its queue (at most 64 entries).
void NoteTargetsQueue(uint8_t* base) {
  using nfsmw::audio_native::Read16;
  using nfsmw::audio_native::Read32;
  const int32_t count = int32_t(Read32(base, kQueueCommands));
  for (int32_t k = 0; k < std::min(count, 64); ++k) {
    const uint32_t entry = kQueueCommands + 8 + uint32_t(k) * 8;
    const uint32_t type = Read16(base, entry - 4);
    const uint32_t index = Read16(base, entry - 2);
    const uint32_t object = Read32(base, kQueueCommands + 772 + index * 4);
    g_targets_queue.Note(object >= 0x10000 ? Read32(base, object + (type == 0 ? 84 : 80)) : 0);
  }
}

void Report(int64_t now) {
  CloseWindow();
  const int64_t cpu_us = nfsmw::threads::CpuThreadUs(g_handler);
  const int64_t cpu_ms = cpu_us >= 0 && g_cpu_before_us >= 0 ? (cpu_us - g_cpu_before_us) / 1000 : -1;
  std::array<size_t, kNumber> order{};
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(),
            [](size_t a, size_t b) { return g_counts[a].own_ns > g_counts[b].own_ns; });
  std::string text;
  for (size_t i : order) {
    const Count& c = g_counts[i];
    if (c.calls == 0) {
      continue;
    }
    text += fmt::format("{} ({}) {} calls, own {:.1f} ms, inclusive {:.1f} ms, maximum own {:.1f} ms en "
                         "500 ms; ",
                         kFunctions[i].name, kFunctions[i].role, c.calls, double(c.own_ns) / 1e6,
                         double(c.inclusive_ns) / 1e6, double(c.max_own_window_ns) / 1e6);
  }
  const double seconds = double(now - g_since_ns) / 1e9;
  REXLOG_INFO("[audio] functions del server en {:.1f} s, CPU del thread_value {} ms, time own measured {:.1f} ms: {}",
              seconds, cpu_ms, double(g_measured_ns) / 1e6, text);
  REXLOG_INFO("[audio] call virtual de sub_825EB0E8 en {:.1f} s: {}", seconds, g_targets_voice.Text());
  REXLOG_INFO("[audio] effect de sub_825DCED8 (pointer en 0x{:08X}) en {:.1f} s: {}", kPointerEffect, seconds,
              g_targets_effect.Text());
  REXLOG_INFO("[audio] list de sub_825D24E0 (0x{:08X}) en {:.1f} s: {}", kHeadList, seconds,
              g_targets_list.Text());
  REXLOG_INFO("[audio] queue de sub_825DA1E0 (0x{:08X}) en {:.1f} s: {}", kQueueCommands, seconds,
              g_targets_queue.Text());
  g_counts = {};
  g_targets_voice = {};
  g_targets_effect = {};
  g_targets_list = {};
  g_targets_queue = {};
  g_measured_ns = 0;
  g_since_ns = now;
  g_window_since_ns = now;
  g_cpu_before_us = cpu_us;
}

// When a measured call that is not nested in another one ends: 500 ms windows and 10 s summary.
void ToExit(int64_t now, int64_t total_ns) {
  g_measured_ns += total_ns;
  if (g_since_ns == 0) {
    g_since_ns = now;
    g_window_since_ns = now;
    g_cpu_before_us = nfsmw::threads::CpuThreadUs(g_handler);
    return;
  }
  if (now - g_window_since_ns >= kWindowNs) {
    CloseWindow();
    g_window_since_ns = now;
  }
  if (now - g_since_ns >= kSummaryNs) {
    Report(now);
  }
}

}  // namespace

// Measures a call if the diagnostic is on and this is the server thread. The measurements in progress form a
// stack through the pointer to the outer one: each passes its total time to its parent, which subtracts it
// from its own self time.
class Measurement {
 public:
  explicit Measurement(size_t index)
      : index_(index), active_(t_server && REXCVAR_GET(nfsmw_audio_diag_functions)) {
    if (!active_) {
      return;
    }
    parent_ = actual_;
    actual_ = this;
    external_ = g_active[index_]++ == 0;
    start_ns_ = NowNs();
  }
  ~Measurement() {
    if (!active_) {
      return;
    }
    const int64_t fin = NowNs();
    const int64_t total = fin - start_ns_;
    const int64_t own = total - children_ns_;
    Count& c = g_counts[index_];
    ++c.calls;
    c.own_ns += own;
    c.own_window_ns += own;
    if (external_) {
      c.inclusive_ns += total;
    }
    --g_active[index_];
    actual_ = parent_;
    if (parent_ != nullptr) {
      parent_->children_ns_ += total;
    } else {
      ToExit(fin, total);
    }
  }
  Measurement(const Measurement&) = delete;
  Measurement& operator=(const Measurement&) = delete;

  bool active() const { return active_; }

 private:
  static inline Measurement* actual_ = nullptr;  // only the server thread touches it
  size_t index_;
  bool active_;
  bool external_ = false;
  Measurement* parent_ = nullptr;
  int64_t start_ns_ = 0;
  int64_t children_ns_ = 0;
};

// Called by the audio server thread's wait, once, from that thread.
void MarkThreadServer() {
  t_server = true;
  g_handler = nfsmw::threads::HandlerThreadCurrent();
}

}  // namespace nfsmw::audio_profile

#define NFSMW_MEASURE_FUNCTION(name, index)                          \
  REX_EXTERN(__imp__##name);                                       \
  REX_HOOK_RAW(name) {                                             \
    nfsmw::audio_profile::Measurement measurement(nfsmw::audio_profile::index); \
    __imp__##name(ctx, base);                                      \
  }

NFSMW_MEASURE_FUNCTION(sub_825CF780, kCommands)
NFSMW_MEASURE_FUNCTION(sub_825E4160, kLoop825E4160)
NFSMW_MEASURE_FUNCTION(sub_825D07C8, kLoop825D07C8)
NFSMW_MEASURE_FUNCTION(sub_825CFCC8, kGraph825CFCC8)
NFSMW_MEASURE_FUNCTION(sub_825ED350, kBlendPacket)
NFSMW_MEASURE_FUNCTION(sub_825DD288, k825DD288)
// sub_825DCED8 also records where the effects pointer points.
REX_EXTERN(__imp__sub_825DCED8);
REX_HOOK_RAW(sub_825DCED8) {
  nfsmw::audio_profile::Measurement measurement(nfsmw::audio_profile::k825DCED8);
  if (measurement.active()) {
    nfsmw::audio_profile::NoteTargetEffect(base);
  }
  __imp__sub_825DCED8(ctx, base);
}
NFSMW_MEASURE_FUNCTION(sub_825D2538, k825D2538)
NFSMW_MEASURE_FUNCTION(sub_825ED568, k825ED568)
NFSMW_MEASURE_FUNCTION(sub_825DBD68, k825DBD68)
NFSMW_MEASURE_FUNCTION(sub_825DCCB0, k825DCCB0)
NFSMW_MEASURE_FUNCTION(sub_825DC0C0, k825DC0C0)
// The voice also records where its virtual call goes.
REX_EXTERN(__imp__sub_825EB0E8);
REX_HOOK_RAW(sub_825EB0E8) {
  nfsmw::audio_profile::Measurement measurement(nfsmw::audio_profile::kVoice);
  if (measurement.active()) {
    nfsmw::audio_profile::NoteTargetVoice(base, ctx.r3.u32, ctx.r5.u32);
  }
  __imp__sub_825EB0E8(ctx, base);
}
NFSMW_MEASURE_FUNCTION(sub_82602BE0, kResampling)
// The two linear resamplers can run in native code (nfsmw_audio_resampling_native): their hook calls the
// selection in nfsmw_audio_remuestreo.cpp instead of going straight to the recompiled function. A single hook
// per function: on the Switch the linker accepts duplicate definitions (--allow-multiple-definition) and would
// silently keep one of them.
namespace nfsmw::audio_resampling {
void Resampling826031C0(PPCContext& ctx, uint8_t* base);
void Resampling82619820(PPCContext& ctx, uint8_t* base);
}  // namespace nfsmw::audio_resampling

#define NFSMW_MEASURE_FUNCTION_BY(name, index, call)            \
  REX_HOOK_RAW(name) {                                             \
    nfsmw::audio_profile::Measurement measurement(nfsmw::audio_profile::index); \
    call(ctx, base);                                              \
  }

NFSMW_MEASURE_FUNCTION_BY(sub_826031C0, kResamplingLinear, nfsmw::audio_resampling::Resampling826031C0)
NFSMW_MEASURE_FUNCTION(sub_826033A0, kResampling826033A0)
NFSMW_MEASURE_FUNCTION(sub_826022C8, k826022C8)
NFSMW_MEASURE_FUNCTION(sub_826047B0, k826047B0)
NFSMW_MEASURE_FUNCTION(sub_8262E220, k8262E220)
NFSMW_MEASURE_FUNCTION_BY(sub_82619820, k82619820, nfsmw::audio_resampling::Resampling82619820)
NFSMW_MEASURE_FUNCTION(sub_82612270, k82612270)
NFSMW_MEASURE_FUNCTION(sub_825E1370, k825E1370)
namespace nfsmw::audio_filter {
void Filter825CD088(PPCContext& ctx, uint8_t* base);  // nfsmw_audio_filter.cpp (nfsmw_audio_filter_native)
}  // namespace nfsmw::audio_filter
NFSMW_MEASURE_FUNCTION_BY(sub_825CD088, k825CD088, nfsmw::audio_filter::Filter825CD088)
// sub_826BDD90 (the real memcpy of the CRT, 950 callers) now goes through [rexcrt] memmove in overrides.toml
// and no longer exists in the generated code: its measurement has nothing to hook.
// NFSMW_MEASURE_FUNCTION(sub_826BDD90, kHelp826BDD90)
NFSMW_MEASURE_FUNCTION(sub_82601A08, k82601A08)
NFSMW_MEASURE_FUNCTION(sub_825D05F8, k825D05F8)
// What the voice and the per-packet mix call without being measured.
NFSMW_MEASURE_FUNCTION(sub_825DE890, k825DE890)
NFSMW_MEASURE_FUNCTION(sub_826027F0, k826027F0)
NFSMW_MEASURE_FUNCTION(sub_825F2F60, k825F2F60)
NFSMW_MEASURE_FUNCTION(sub_825ED268, k825ED268)
// sub_825D24E0 and sub_825DA1E0 call through pointers; their targets are recorded.
REX_EXTERN(__imp__sub_825D24E0);
REX_HOOK_RAW(sub_825D24E0) {
  nfsmw::audio_profile::Measurement measurement(nfsmw::audio_profile::k825D24E0);
  if (measurement.active()) {
    nfsmw::audio_profile::NoteTargetsList(base);
  }
  __imp__sub_825D24E0(ctx, base);
}
REX_EXTERN(__imp__sub_825DA1E0);
REX_HOOK_RAW(sub_825DA1E0) {
  nfsmw::audio_profile::Measurement measurement(nfsmw::audio_profile::k825DA1E0);
  if (measurement.active()) {
    nfsmw::audio_profile::NoteTargetsQueue(base);
  }
  __imp__sub_825DA1E0(ctx, base);
}
// The gain-scaled sum can run in native code (nfsmw_audio_sum_native, nfsmw_audio_suma.cpp).
namespace nfsmw::audio_sum {
void Sum825FDFB0(PPCContext& ctx, uint8_t* base);
}  // namespace nfsmw::audio_sum
NFSMW_MEASURE_FUNCTION_BY(sub_825FDFB0, k825FDFB0, nfsmw::audio_sum::Sum825FDFB0)
NFSMW_MEASURE_FUNCTION(sub_825E1CD0, k825E1CD0)
