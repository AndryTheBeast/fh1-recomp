// nfsmw - measurement probe for the engine sound (Ginsu): the acceleration sound that is slow to come back
// after braking
//
// On the console, in one particular corner, after braking hard and accelerating again, the engine's acceleration
// sound takes about 2.5 s to come back; the deceleration sound does play. It happens on both laps in the same
// corner and coincides with a zone load. This probe only measures: each hook calls the original function without
// touching its registers and then reads guest memory. It does not change the audio.
//
// How the engine sounds, as seen in the PowerPC of the recompiled code:
//  - Each .gin (signature "Gnsu2", checked byte by byte in sub_8220A3A0) is entirely in RAM: sub_821F6800 looks
//    it up by name in the resource table at 0x82A31370 (104-byte entries) and returns a pointer. Synthesis
//    does not read the disc.
//  - Each .gin has a 68-byte synthesizer (vtable 0x82072490) with two buffers of rate x 0.011 samples
//    (11 ms) in a 2620-byte workspace (sub_8220B110). It registers with the game's sound engine as a
//    user stream with the callback sub_8220ABF8 (sub_825D9768) and starts its voice with priority 101
//    (sub_8220B378 -> sub_825D98B0).
//  - When the voice consumes a buffer, the audio server thread (sub_825ED350, per packet) drains the queue
//    sub_825DA1E0, which calls sub_8220ABF8(r3 = buffer, r4 = synthesizer) -> sub_8220AC08 through a pointer.
//    That function refills the buffer by decoding the .gin from RAM (sub_8220AB10 and sub_8220A138, 32-sample
//    blocks) and queues it again (sub_825D9C58).
//  - If the voice asks for more samples than are queued, sub_825FDC38 gives it nothing for that packet: with
//    no consumption there is no callback, and with no callback there is no new buffer.
//  - Per frame, CARSFX_DualGinsuEng (vtable 0x82072498) runs sub_821F69E0 and, through its method +64,
//    sub_821F74B0: Ac voice volume = (gain [ctl+60] x DMX) >> 23 and Dc voice volume = ([ctl+64] x DMX) >> 23,
//    from 0 to 127; and it sets the rpm of both synthesizers (sub_8220B558). ctl = [this+64] is the controller
//    computed by sub_821CF0D8 -> sub_821CF770: rpm at +184, rpm delta per update at +172 and its moving average
//    at +144, mix weights at +208 and +212, and the AEMS, Ac and Dc gains at +56, +60 and +64.
//
// Hooks. All three are only called through pointers: the indirect call table in nfsmw_init.cpp sees the hook
// even though tools/direct_calls.py has put __imp__ on the direct calls (the patch checks this).
//  - sub_8220ABF8, on the audio server thread: per synthesizer it counts buffers, samples, silent buffers, the
//    peak of the last buffer, the time of the last one and gaps longer than 50 ms. Atomics only; it does not log.
//  - sub_821F74B0 (dual engine) and sub_821F7118 (single engine), on the thread that updates the car sound:
//    they read the state, watch for gaps and write the log. [motor] lines:
//      SIN SAMPLES  a voice has gone over 50 ms without refilling a buffer while other voices keep refilling;
//      VUELVE        the voice refills again, with the exact duration of the gap;
//      Ac off    a stretch of 300 ms or more with the Ac voice below a quarter of the Dc, with a trace every
//                    100 ms starting 1 s before (to compare the bad corner with the good braking events);
//      la update no corrio   more than 100 ms between two engine updates;
//      summary       every 10 s.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports

#include "nfsmw_audio_native.h"

REXCVAR_DEFINE_BOOL(nfsmw_audio_probe_engine, true, "NFSMW",
                    "Probe de measurement del sonido del motor (Ginsu): buffers que rellena every synthesizer, volumenes, "
                    "rpm y state de the voces; anota en el log los gaps de mas de 50 ms y los ranges con la "
                    "aceleracion off. Solo mide: no cambia el audio");

namespace nfsmw::audio_engine_probe {
namespace {

using nfsmw::audio_native::Dir;
using nfsmw::audio_native::Read16;
using nfsmw::audio_native::Read32;
using nfsmw::audio_native::ReadFloat;

// The game's sound engine (sub_825D9768, sub_825D9C58, sub_825DD8B0, sub_825FBC68, sub_825FDC38).
constexpr uint32_t kTableFlows = 0x82A2AD38 + 772;  // pointers to the user streams, by index
constexpr uint32_t kSnd = 0x82A2B1D0;  // +14 number of streams (byte), +48 number of voices (16 bits), +136 voices
constexpr uint32_t kTamVoice = 132;      // voice: +0 handle, +4 first physical voice, +56 volume, +105 in use,
                                       // +128 tone

constexpr int64_t kGapNs = 50'000'000;               // 50 ms without refilling a buffer...
constexpr uint64_t kOthersMinimums = 3;                  // ...while other voices refill at least 3
constexpr int64_t kUpdateStoppedNs = 100'000'000;
constexpr int64_t kSummaryNs = 10'000'000'000;
constexpr int64_t kTraceEveryNs = 100'000'000;          // one trace sample every 100 ms
constexpr size_t kTrace = 64;                          // 6,4 s de trace
constexpr int64_t kTraceBeforeNs = 1'000'000'000;       // the trace of a stretch starts 1 s before
constexpr int64_t kOffMinimumNs = 300'000'000;      // stretches with the Ac voice off that get logged
constexpr int64_t kOffLongNs = 4'000'000'000;     // warning en marcha si no vuelve
constexpr int64_t kForgetNs = 60'000'000'000;         // an inactive synthesizer or engine is forgotten
constexpr int32_t kPicoMute = 16;                      // silent buffer: peak below 16 out of 32767
constexpr uint32_t kMaxSamples = 552;                 // buffer limit (sub_8220B0E0)

int64_t NowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// Guest pointers that can be followed (heap or image objects, non-null).
bool Valid(uint32_t dir) {
  return dir >= 0x10000 && dir < 0xF0000000u;
}

size_t IdThread() {
  thread_local const size_t id = std::hash<std::thread::id>{}(std::this_thread::get_id());
  return id;
}

// What the server thread records on each synthesizer callback. Atomics only: it is read by the thread that
// updates the car sound.
struct Synthesizer {
  std::atomic<uint32_t> address{0};
  std::atomic<int64_t> last_ns{0};
  std::atomic<uint64_t> sequence{0};  // value of g_sequence at its last buffer
  std::atomic<uint64_t> buffers{0};
  std::atomic<uint64_t> sample_total{0};
  std::atomic<uint64_t> mute{0};
  std::atomic<int32_t> pico{0};  // of the last buffer
  std::atomic<int64_t> gap_max_ns{0};
  // The last gap longer than 50 ms, measured when the voice refills again.
  std::atomic<uint64_t> gaps{0};
  std::atomic<int64_t> gap_ns{0};
  std::atomic<uint64_t> gap_others{0};  // buffers from other voices during the gap
};

std::array<Synthesizer, 16> g_sint;
std::atomic<uint64_t> g_sequence{0};
std::atomic<size_t> g_thread_callbacks{0};

Synthesizer* Find(uint32_t s, bool create) {
  if (!Valid(s)) {
    return nullptr;
  }
  for (Synthesizer& e : g_sint) {
    if (e.address.load(std::memory_order_acquire) == s) {
      return &e;
    }
  }
  if (!create) {
    return nullptr;
  }
  for (Synthesizer& e : g_sint) {
    uint32_t free = 0;
    if (e.address.compare_exchange_strong(free, s, std::memory_order_acq_rel)) {
      return &e;
    }
  }
  return nullptr;  // table full: that synthesizer is not measured
}

// Audio server thread, after the original callback has refilled the buffer.
void ToFill(uint8_t* base, uint32_t buffer, uint32_t s) {
  Synthesizer* e = Find(s, true);
  if (e == nullptr) {
    return;
  }
  size_t without_thread = 0;
  g_thread_callbacks.compare_exchange_strong(without_thread, IdThread());
  const int64_t now = NowNs();
  const uint64_t sequence = g_sequence.fetch_add(1) + 1;
  const int64_t before = e->last_ns.exchange(now);
  const uint64_t sequence_before = e->sequence.exchange(sequence);
  if (before != 0) {
    const int64_t gap = now - before;
    if (gap > e->gap_max_ns.load(std::memory_order_relaxed)) {
      e->gap_max_ns.store(gap, std::memory_order_relaxed);
    }
    if (gap > kGapNs) {
      e->gap_ns.store(gap);
      e->gap_others.store(sequence - sequence_before - 1);
      e->gaps.fetch_add(1);
    }
  }
  // Peak of the freshly refilled buffer: [s+20] 16-bit big-endian samples.
  const uint32_t n = Read32(base, s + 20);
  int32_t pico = 0;
  if (n > 0 && n <= kMaxSamples && Valid(buffer)) {
    const uint8_t* p = Dir(base, buffer);
    for (uint32_t i = 0; i < n; ++i) {
      const int16_t v = static_cast<int16_t>(static_cast<uint16_t>((p[2 * i] << 8) | p[2 * i + 1]));
      pico = std::max<int32_t>(pico, std::abs(static_cast<int32_t>(v)));
    }
  }
  e->pico.store(pico, std::memory_order_relaxed);
  e->buffers.fetch_add(1, std::memory_order_relaxed);
  e->sample_total.fetch_add(n, std::memory_order_relaxed);
  if (pico < kPicoMute) {
    e->mute.fetch_add(1, std::memory_order_relaxed);
  }
}

// One engine voice (slot 0 = Ac, slot 1 = Dc), read from the engine object, its synthesizer, its user
// stream and the sound engine voice.
struct Voice {
  uint32_t sint = 0;
  uint32_t handler = 0;    // [this+104] o [this+128]
  int32_t volume = 0;       // [this+136] o [this+140], 0..127
  bool valid = false;       // the same check as sub_825DD8B0
  int32_t tone = 0;          // [voice+128]
  float volume_snd = -1.0f; // [voice physical+56] (inferido de sub_825D93D0)
  uint32_t rate = 0;         // [s+36]; 0 = the synthesizer has the voice stopped (sub_8220B6A0)
  uint32_t n = 0;            // [s+20], samples per buffer
  uint32_t pos = 0;          // [s+44], read pointer in the .gin
  uint32_t total = 0;        // [[s+40]+20], samples of the .gin
  int32_t actual = 0;        // [s+48]
  int32_t target = 0;       // [s+52], set by sub_8220B558 from the rpm
  int32_t steps = 0;         // [s+56]
  int32_t flow = -1;        // [s+28]
  uint32_t voice_flow = 0;    // [flow+0]
  int32_t in_queue = 0;       // [flow+72], sample_total queued
  int32_t drag = 0;      // [flow+76]
  int32_t buffers_queue = 0;  // [flow+52]
};

struct State {
  bool double = false;
  uint32_t ready = 0;   // [this+76]
  uint32_t active = 0;  // [this+8]
  int32_t dmx = 0;      // [[this+16]+4] & 0x7FFF
  uint32_t ctl = 0;     // [this+64]
  float rpm = 0.0f, delta = 0.0f, delta_media = 0.0f, peso_a = 0.0f, peso_b = 0.0f;
  int32_t g_aems = 0, g_ac = 0, g_dc = 0;
  std::array<Voice, 2> voice{};
};

Voice ReadVoice(uint8_t* base, uint32_t s, uint32_t handler, int32_t volume) {
  Voice v;
  v.sint = s;
  v.handler = handler;
  v.volume = volume;
  if (static_cast<int32_t>(handler) >= 0) {
    const uint32_t index = handler & 0xFF;
    const uint32_t how_many = Read16(base, kSnd + 48);
    const uint32_t table = Read32(base, kSnd + 136);
    if (index < how_many && Valid(table)) {
      const uint32_t voice = table + index * kTamVoice;
      v.valid = *Dir(base, voice + 105) != 0 && Read32(base, voice) == handler;
      v.tone = static_cast<int16_t>(Read16(base, voice + 128));
      const uint32_t physical = Read16(base, voice + 4);
      if (physical < how_many) {
        v.volume_snd = ReadFloat(base, table + physical * kTamVoice + 56);
      }
    }
  }
  if (!Valid(s)) {
    return v;
  }
  v.rate = Read32(base, s + 36);
  v.n = Read32(base, s + 20);
  v.pos = Read32(base, s + 44);
  v.actual = static_cast<int32_t>(Read32(base, s + 48));
  v.target = static_cast<int32_t>(Read32(base, s + 52));
  v.steps = static_cast<int32_t>(Read32(base, s + 56));
  const uint32_t data = Read32(base, s + 40);
  if (Valid(data)) {
    v.total = Read32(base, data + 20);
  }
  v.flow = static_cast<int32_t>(Read32(base, s + 28));
  if (v.flow >= 0 && v.flow < static_cast<int32_t>(*Dir(base, kSnd + 14))) {
    const uint32_t f = Read32(base, kTableFlows + static_cast<uint32_t>(v.flow) * 4);
    if (Valid(f)) {
      v.voice_flow = Read32(base, f + 0);
      v.in_queue = static_cast<int32_t>(Read32(base, f + 72));
      v.drag = static_cast<int32_t>(Read32(base, f + 76));
      v.buffers_queue = static_cast<int16_t>(Read16(base, f + 52));
    }
  }
  return v;
}

State ReadState(uint8_t* base, uint32_t object, bool double) {
  State e;
  e.double = double;
  e.ready = *Dir(base, object + 76);
  e.active = *Dir(base, object + 8);
  const uint32_t entries = Read32(base, object + 16);
  if (Valid(entries)) {
    e.dmx = static_cast<int32_t>(Read32(base, entries + 4) & 0x7FFF);
  }
  e.ctl = Read32(base, object + 64);
  if (Valid(e.ctl)) {
    e.rpm = ReadFloat(base, e.ctl + 184);
    e.delta = ReadFloat(base, e.ctl + 172);
    e.delta_media = ReadFloat(base, e.ctl + 144);
    e.peso_a = ReadFloat(base, e.ctl + 208);
    e.peso_b = ReadFloat(base, e.ctl + 212);
    e.g_aems = static_cast<int32_t>(Read32(base, e.ctl + 56));
    e.g_ac = static_cast<int32_t>(Read32(base, e.ctl + 60));
    e.g_dc = static_cast<int32_t>(Read32(base, e.ctl + 64));
  }
  e.voice[0] = ReadVoice(base, Read32(base, object + 88), Read32(base, object + 104),
                     static_cast<int32_t>(Read32(base, object + 136)));
  if (double) {
    e.voice[1] = ReadVoice(base, Read32(base, object + 112), Read32(base, object + 128),
                       static_cast<int32_t>(Read32(base, object + 140)));
  }
  return e;
}

std::string TextVoice(const char* name, const Voice& v, int64_t now) {
  std::string fills = "sin callbacks views";
  if (const Synthesizer* s = Find(v.sint, false)) {
    const int64_t last_2 = s->last_ns.load();
    fills = fmt::format("{} buffers, last does {:.1f} ms con pico {}", s->buffers.load(),
                           last_2 != 0 ? double(now - last_2) / 1e6 : -1.0, s->pico.load());
  }
  return fmt::format("{}: vol {} (SND {:.3f}), sint 0x{:08X} {} (rate {} Hz, {} sample_total por buffer), {}; "
                     ".gin pos {} de {}, actual {} target {} steps {}; voice 0x{:08X} {} tone {}; flow {} "
                     "(voice 0x{:08X}) con {} sample_total en queue + {} y {} buffers",
                     name, v.volume, v.volume_snd, v.sint, v.rate != 0 ? "arrancado" : "STOPPED", v.rate, v.n,
                     fills, v.pos, v.total, v.actual, v.target, v.steps, v.handler,
                     v.valid ? "valid" : "NO VALID", v.tone, v.flow, v.voice_flow, v.in_queue, v.drag,
                     v.buffers_queue);
}

std::string TextState(const State& e, int64_t now) {
  std::string t = fmt::format("DMX {}, ready {}, active {}; ctl 0x{:08X}: rpm {:.0f}, delta {:.1f} (media {:.1f}), "
                              "weights {:.2f}/{:.2f}, ganancias AEMS {} Ac {} Dc {}; ",
                              e.dmx, e.ready, e.active, e.ctl, e.rpm, e.delta, e.delta_media, e.peso_a, e.peso_b,
                              e.g_aems, e.g_ac, e.g_dc);
  t += TextVoice("Ac (slot 0)", e.voice[0], now);
  if (e.double) {
    t += "; ";
    t += TextVoice("Dc (slot 1)", e.voice[1], now);
  }
  return t;
}

struct Sample {
  int64_t ns = 0;
  int32_t vol_ac = 0, vol_dc = 0;
  float rpm = 0.0f, delta_media = 0.0f;
  int32_t g_ac = 0, g_dc = 0;
  uint64_t buffers_ac = 0, buffers_dc = 0;
};

// State of one engine (normally the player's car). Only the thread that updates it touches it, under g_lock.
struct Engine {
  uint32_t object = 0;
  bool double = false;
  bool presented = false;
  int64_t seen_ns = 0;
  int64_t last_ns = 0;
  uint64_t updates = 0;
  int64_t gap_max_ns = 0;
  bool in_same_thread = false;
  std::array<uint32_t, 2> sint{};  // Ac and Dc synthesizers from the last update
  std::array<bool, 2> in_gap{};
  std::array<uint64_t, 2> gaps_seen{};
  // Counters of each synthesizer at the previous summary (reset if the synthesizer changes).
  std::array<uint32_t, 2> sint_before{};
  std::array<uint64_t, 2> buffers_before{}, samples_before{}, mute_before{}, gaps_before{};
  uint64_t stopped_server = 0;  // gaps in which the other voices did not refill either
  bool off = false;
  bool off_warned = false;
  int64_t off_since_ns = 0;
  float rpm_to_turn_off = 0.0f;
  int32_t vol_dc_max = 0;
  std::array<Sample, kTrace> trace{};
  size_t trace_n = 0;
  int64_t trace_last_ns = 0;
};

std::mutex g_lock;
std::array<Engine, 8> g_engines;
int64_t g_summary_since_ns = 0;

Engine* EngineOf(uint32_t object, bool double, int64_t now) {
  Engine* free = nullptr;
  for (Engine& m : g_engines) {
    if (m.object == object) {
      return &m;
    }
    if (free == nullptr && (m.object == 0 || now - m.seen_ns > kForgetNs)) {
      free = &m;
    }
  }
  if (free != nullptr) {
    *free = Engine{};
    free->object = object;
    free->double = double;
  }
  return free;
}

std::string TextTrace(const Engine& m, int64_t since_ns) {
  std::string t;
  const size_t n = std::min(m.trace_n, kTrace);
  for (size_t i = m.trace_n - n; i < m.trace_n; ++i) {
    const Sample& mu = m.trace[i % kTrace];
    if (mu.ns < since_ns) {
      continue;
    }
    t += fmt::format("{}{}:{}/{} {:.0f} {:.1f} {}/{} {}/{}", t.empty() ? "" : " | ",
                     (mu.ns - m.off_since_ns) / 1'000'000, mu.vol_ac, mu.vol_dc, mu.rpm, mu.delta_media, mu.g_ac,
                     mu.g_dc, mu.buffers_ac, mu.buffers_dc);
  }
  return t;
}

// One line per engine seen in the last 10 s, with what each synthesizer refilled in that time.
void Summarize(Engine& m, int64_t period_ns) {
  std::string text;
  const int slots = m.double ? 2 : 1;
  for (int k = 0; k < slots; ++k) {
    if (m.sint[k] != m.sint_before[k]) {
      m.sint_before[k] = m.sint[k];
      m.buffers_before[k] = m.samples_before[k] = m.mute_before[k] = m.gaps_before[k] = 0;
    }
    Synthesizer* s = Find(m.sint[k], false);
    if (s == nullptr) {
      text += fmt::format("; {}: sin callbacks views", k == 0 ? "Ac" : "Dc");
      continue;
    }
    const uint64_t buffers = s->buffers.load();
    const uint64_t sample_total = s->sample_total.load();
    const uint64_t mute = s->mute.load();
    const uint64_t gaps = s->gaps.load();
    text += fmt::format("; {} (sint 0x{:08X}): {} buffers, {} sample_total, {} mute, gap maximum {:.1f} ms, {} gaps "
                         "de mas de 50 ms",
                         k == 0 ? "Ac" : "Dc", m.sint[k], buffers - m.buffers_before[k], sample_total - m.samples_before[k],
                         mute - m.mute_before[k], double(s->gap_max_ns.exchange(0)) / 1e6,
                         gaps - m.gaps_before[k]);
    m.buffers_before[k] = buffers;
    m.samples_before[k] = sample_total;
    m.mute_before[k] = mute;
    m.gaps_before[k] = gaps;
  }
  NFSMW_REPORT_DEFERRED("[motor] summary de {:.1f} s: motor 0x{:08X} ({}) {} updates, gap maximum {:.1f} ms, {}en el "
              "thread_value de the callbacks, {} gaps con all the voces stopped{}",
              double(period_ns) / 1e9, m.object, m.double ? "double" : "simple", m.updates,
              double(m.gap_max_ns) / 1e6, m.in_same_thread ? "" : "no ", m.stopped_server, text);
  m.updates = 0;
  m.gap_max_ns = 0;
}

// Thread that updates the car sound, after the original engine update.
void ToUpdate(uint8_t* base, uint32_t object, bool double) {
  if (!Valid(object)) {
    return;
  }
  const int64_t now = NowNs();
  std::lock_guard<std::mutex> lock(g_lock);
  Engine* m = EngineOf(object, double, now);
  if (m == nullptr) {
    return;
  }
  m->seen_ns = now;
  ++m->updates;
  m->in_same_thread = IdThread() == g_thread_callbacks.load();
  const State e = ReadState(base, object, double);
  m->sint[0] = e.voice[0].sint;
  m->sint[1] = e.voice[1].sint;

  if (!m->presented && e.ready != 0) {
    m->presented = true;
    REXLOG_INFO("[motor] motor 0x{:08X} ({}) ready; {}", object, double ? "double" : "simple", TextState(e, now));
  }

  // 1. The engine update itself.
  if (m->last_ns != 0) {
    const int64_t gap = now - m->last_ns;
    m->gap_max_ns = std::max(m->gap_max_ns, gap);
    if (gap > kUpdateStoppedNs) {
      REXLOG_INFO("[motor] la update del motor 0x{:08X} no corrio en {:.1f} ms; {}", object,
                  double(gap) / 1e6, TextState(e, now));
    }
  }
  m->last_ns = now;

  // 2. Gaps of each voice: no buffer refill while the other voices refill.
  const int slots = double ? 2 : 1;
  for (int k = 0; k < slots; ++k) {
    const char* name = k == 0 ? "Ac (slot 0)" : "Dc (slot 1)";
    const Synthesizer* s = Find(e.voice[k].sint, false);
    if (s == nullptr) {
      continue;
    }
    const int64_t last_2 = s->last_ns.load();
    const uint64_t others = g_sequence.load() - s->sequence.load();
    if (!m->in_gap[k] && last_2 != 0 && now - last_2 > kGapNs && others >= kOthersMinimums) {
      m->in_gap[k] = true;
      REXLOG_INFO("[motor] SIN SAMPLES {}: {:.1f} ms sin fill buffer mientras other voces rellenaron {}; {}",
                  name, double(now - last_2) / 1e6, others, TextState(e, now));
    }
    const uint64_t gaps = s->gaps.load();
    if (gaps != m->gaps_seen[k]) {
      m->gaps_seen[k] = gaps;
      const uint64_t others_gap = s->gap_others.load();
      if (m->in_gap[k] || others_gap >= kOthersMinimums) {
        REXLOG_INFO("[motor] VUELVE {} after {:.1f} ms sin fill (other voces rellenaron {} buffers); {}", name,
                    double(s->gap_ns.load()) / 1e6, others_gap, TextState(e, now));
      } else {
        ++m->stopped_server;
      }
      m->in_gap[k] = false;
    }
  }

  // 3. Trace every 100 ms and stretches with the Ac voice off while the Dc plays (dual engine only).
  if (double) {
    if (now - m->trace_last_ns >= kTraceEveryNs) {
      m->trace_last_ns = now;
      Sample& mu = m->trace[m->trace_n % kTrace];
      mu.ns = now;
      mu.vol_ac = e.voice[0].volume;
      mu.vol_dc = e.voice[1].volume;
      mu.rpm = e.rpm;
      mu.delta_media = e.delta_media;
      mu.g_ac = e.g_ac;
      mu.g_dc = e.g_dc;
      const Synthesizer* ac = Find(e.voice[0].sint, false);
      const Synthesizer* dc = Find(e.voice[1].sint, false);
      mu.buffers_ac = ac != nullptr ? ac->buffers.load() : 0;
      mu.buffers_dc = dc != nullptr ? dc->buffers.load() : 0;
      ++m->trace_n;
    }
    const bool off = e.ready != 0 && e.dmx > 0 && e.voice[0].volume * 4 < e.voice[1].volume;
    if (off && !m->off) {
      m->off = true;
      m->off_warned = false;
      m->off_since_ns = now;
      m->rpm_to_turn_off = e.rpm;
      m->vol_dc_max = 0;
    }
    if (off) {
      m->vol_dc_max = std::max(m->vol_dc_max, e.voice[1].volume);
      if (!m->off_warned && now - m->off_since_ns > kOffLongNs) {
        m->off_warned = true;
        REXLOG_INFO("[motor] Ac lleva {:.0f} ms off con Dc sonando; {}",
                    double(now - m->off_since_ns) / 1e6, TextState(e, now));
      }
    } else if (m->off) {
      m->off = false;
      const int64_t duration = now - m->off_since_ns;
      if (duration >= kOffMinimumNs) {
        REXLOG_INFO("[motor] Ac off {:.0f} ms con Dc sonando (vol Dc maximum {}), rpm {:.0f} -> {:.0f}; al volver: "
                    "{}; trace every 100 ms since 1 s before (ms since el start:vol Ac/Dc rpm delta_media "
                    "gain Ac/Dc buffers Ac/Dc): {}",
                    double(duration) / 1e6, m->vol_dc_max, m->rpm_to_turn_off, e.rpm, TextState(e, now),
                    TextTrace(*m, m->off_since_ns - kTraceBeforeNs));
      }
    }
  }

  // 4. Summary every 10 s of the engines seen in that time, and forgetting dead synthesizers.
  if (g_summary_since_ns == 0) {
    g_summary_since_ns = now;
  } else if (now - g_summary_since_ns >= kSummaryNs) {
    for (Engine& other : g_engines) {
      if (other.object != 0 && other.updates != 0) {
        Summarize(other, now - g_summary_since_ns);
      }
    }
    g_summary_since_ns = now;
    for (Synthesizer& s : g_sint) {
      const int64_t last_2 = s.last_ns.load();
      if (s.address.load() != 0 && last_2 != 0 && now - last_2 > kForgetNs) {
        s.last_ns.store(0);
        s.buffers.store(0);
        s.sample_total.store(0);
        s.mute.store(0);
        s.gaps.store(0);
        s.address.store(0, std::memory_order_release);
      }
    }
  }
}

}  // namespace
}  // namespace nfsmw::audio_engine_probe

// Callback of the Ginsu synthesizer (r3 = buffer to refill, r4 = synthesizer). Called through a pointer by
// the queue sub_825DA1E0 on the audio server thread; the original only swaps r3 and r4 and jumps to
// sub_8220AC08.
REX_EXTERN(__imp__sub_8220ABF8);
REX_HOOK_RAW(sub_8220ABF8) {
  const bool measure = REXCVAR_GET(nfsmw_audio_probe_engine);
  const uint32_t buffer = ctx.r3.u32;
  const uint32_t synthesizer = ctx.r4.u32;
  __imp__sub_8220ABF8(ctx, base);
  if (measure) {
    nfsmw::audio_engine_probe::ToFill(base, buffer, synthesizer);
  }
}

// Per-frame update of CARSFX_DualGinsuEng (method +64 of its vtable, called by sub_821F69E0).
REX_EXTERN(__imp__sub_821F74B0);
REX_HOOK_RAW(sub_821F74B0) {
  const bool measure = REXCVAR_GET(nfsmw_audio_probe_engine);
  const uint32_t object = ctx.r3.u32;
  __imp__sub_821F74B0(ctx, base);
  if (measure) {
    nfsmw::audio_engine_probe::ToUpdate(base, object, true);
  }
}

// The same for CARSFX_SingleGinsuEng (a single .gin; method +64 of vtable 0x820724E0).
REX_EXTERN(__imp__sub_821F7118);
REX_HOOK_RAW(sub_821F7118) {
  const bool measure = REXCVAR_GET(nfsmw_audio_probe_engine);
  const uint32_t object = ctx.r3.u32;
  __imp__sub_821F7118(ctx, base);
  if (measure) {
    nfsmw::audio_engine_probe::ToUpdate(base, object, false);
  }
}
