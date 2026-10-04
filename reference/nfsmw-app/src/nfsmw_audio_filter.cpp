// nfsmw - recursive filter of the sound engine in native code (sub_825CD088)
//
// After the resamplers, it is the heaviest leaf function of the audio server thread on PC: 7.9 % of its
// CPU, 7.4 us per call, about 4 calls per packet. It is a two-stage filter over a delay line of the
// object at r3 (+4 a, +8 b, +12 c, +16 buffer, +24 write position), with the input in r8, the output in
// r9, n = r4 and three delay taps in r5, r6 and r7. For each sample j:
//   t = x[j] - buf[r5 + j] * a
//   y = t - buf[r6 + j] * b                buf[pos + j] = y
//   z = buf[r7 + j] * c + buf[r6 + j]       (read after storing y)
//   output[j] = z * g        if r10 == 1   (g = the float at 0x820AFD68)
//   output[j] += z * g       otherwise
// Returns r3 = 1.
//
// The floating-point operations are those of the recompiled code (fnmsubs, fmadds and fmuls in double
// precision, rounded to single) in the same order, and so are the reads and writes of each sample, so the
// result is bit-identical even if the taps land on the write position. The object's fields are read once
// if nothing that is written lands on them; if something does, they are reread on every sample, as the
// recompiled code does.
//
// nfsmw_audio_filter_native: 0 = recompiled; 1 = native; 2 = validate: runs the recompiled code, saves
// what it wrote, undoes it, runs the native code, compares byte by byte and keeps the recompiled result.
// A summary is logged every 10 s.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports

#include "nfsmw_audio_native.h"

// Native by default. On PC, in the alley test run, it gave 0 differences against the recompiled code
// over 29 million samples and runs 1.6 times faster.
REXCVAR_DEFINE_INT32(nfsmw_audio_filter_native, 1, "NFSMW",
                     "Filter recursivo del motor de sonido (sub_825CD088): 0 = code recompiled, 1 = native (same "
                     "result bit a bit; por default), 2 = validate el native contra el recompiled");

REX_EXTERN(__imp__sub_825CD088);

namespace nfsmw::audio_filter {
namespace {

using namespace nfsmw::audio_native;

constexpr uint32_t kDirGain = 0x820AFD68;  // lis r11,-32245; lfs f0,-664(r11)
constexpr uint32_t kFields = 4;                 // bytes +4 to +27 of the object are read
constexpr uint32_t kTamFields = 24;

struct Arguments {
  uint32_t obj;
  int32_t n;
  uint32_t take5;
  uint32_t take6;
  uint32_t take7;
  uint32_t entry;
  uint32_t output;
  int32_t mode;
};

Arguments ReadArguments(const PPCContext& ctx) {
  return {ctx.r3.u32, ctx.r4.s32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.s32};
}

inline uint32_t DirTake(uint32_t buf, uint32_t take, int32_t j) {
  return ((take + uint32_t(j)) << 2) + buf;
}

// Output part of each sample, the same in both paths.
inline void Output(uint8_t* base, const Arguments& a, int32_t j, float z, float g) {
  const uint32_t dir = a.output + uint32_t(j) * 4;
  if (a.mode == 1) {
    WriteFloat(base, dir, float(double(float(double(z) * double(g)))));
  } else {
    const float previous = ReadFloat(base, dir);
    WriteFloat(base, dir, float(double(float(std::fma(double(z), double(g), double(previous))))));
  }
}

// The filter in native code. Returns whether it could read the object's fields only once.
bool Native(uint8_t* base, const Arguments& a) {
  const float g = ReadFloat(base, kDirGain);
  if (a.n <= 0) {
    return true;
  }
  const uint64_t bytes = uint64_t(a.n) * 4;
  const uint32_t buf0 = Read32(base, a.obj + 16);
  const uint32_t pos0 = Read32(base, a.obj + 24);
  const bool fixed = !Overlap((pos0 << 2) + buf0, bytes, a.obj + kFields, kTamFields) &&
                     !Overlap(a.output, bytes, a.obj + kFields, kTamFields);
  if (fixed) {
    const float ca = ReadFloat(base, a.obj + 4);
    const float cb = ReadFloat(base, a.obj + 8);
    const float cc = ReadFloat(base, a.obj + 12);
    for (int32_t j = 0; j < a.n; ++j) {
      const float x = ReadFloat(base, a.entry + uint32_t(j) * 4);
      const float p1 = ReadFloat(base, DirTake(buf0, a.take5, j));
      const float t = float(-std::fma(double(p1), double(ca), -double(x)));
      const float p2 = ReadFloat(base, DirTake(buf0, a.take6, j));
      const float y = float(-std::fma(double(p2), double(cb), -double(t)));
      WriteFloat(base, DirTake(buf0, pos0, j), y);
      const float q = ReadFloat(base, DirTake(buf0, a.take7, j));
      const float p = ReadFloat(base, DirTake(buf0, a.take6, j));
      const float z = float(std::fma(double(q), double(cc), double(p)));
      Output(base, a, j, z, g);
    }
    return true;
  }
  // Something that is written lands on the object's fields: they are reread in the same order as the
  // recompiled code.
  for (int32_t j = 0; j < a.n; ++j) {
    const uint32_t buf = Read32(base, a.obj + 16);
    const float ca = ReadFloat(base, a.obj + 4);
    const float x = ReadFloat(base, a.entry + uint32_t(j) * 4);
    const uint32_t pos = Read32(base, a.obj + 24);
    const float cb = ReadFloat(base, a.obj + 8);
    const float p1 = ReadFloat(base, DirTake(buf, a.take5, j));
    const float t = float(-std::fma(double(p1), double(ca), -double(x)));
    const float p2 = ReadFloat(base, DirTake(buf, a.take6, j));
    const float y = float(-std::fma(double(p2), double(cb), -double(t)));
    WriteFloat(base, DirTake(buf, pos, j), y);
    const uint32_t buf2 = Read32(base, a.obj + 16);
    const float cc = ReadFloat(base, a.obj + 12);
    const float q = ReadFloat(base, DirTake(buf2, a.take7, j));
    const float p = ReadFloat(base, DirTake(buf2, a.take6, j));
    const float z = float(std::fma(double(q), double(cc), double(p)));
    Output(base, a, j, z, g);
  }
  return false;
}

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_samples{0};
std::atomic<uint64_t> g_reread{0};
std::atomic<uint64_t> g_differences{0};
std::atomic<int64_t> g_last_report_ms{0};
std::atomic<bool> g_difference_noted{false};

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Report() {
  const int64_t now = NowMs();
  int64_t last = g_last_report_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10000 || !g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  NFSMW_REPORT_DEFERRED("[audio] filter native (mode {}): sub_825CD088 {} calls y {} sample_total, {} releyendo el object, "
              "differences con el recompiled {}",
              REXCVAR_GET(nfsmw_audio_filter_native), g_calls.exchange(0), g_samples.exchange(0),
              g_reread.exchange(0), g_differences.exchange(0));
}

struct Range {
  uint32_t dir;
  uint32_t bytes;
  std::vector<uint8_t> data;
};

void Save(uint8_t* base, Range& r) {
  r.data.resize(r.bytes);
  std::memcpy(r.data.data(), Dir(base, r.dir), r.bytes);
}

void Set(uint8_t* base, const Range& r) {
  std::memcpy(Dir(base, r.dir), r.data.data(), r.bytes);
}

// Mode 2. What the filter writes: the delay line from the write position, the output and, in case they
// overlap, the object's fields. They are compared as 4-byte words.
void Validate(PPCContext& ctx, uint8_t* base) {
  const Arguments a = ReadArguments(ctx);
  if (a.n <= 0) {
    __imp__sub_825CD088(ctx, base);
    return;
  }
  const uint32_t bytes = uint32_t(a.n) * 4;
  const uint32_t buf = Read32(base, a.obj + 16);
  const uint32_t pos = Read32(base, a.obj + 24);
  thread_local std::vector<Range> before(3);
  thread_local std::vector<Range> recompiled(3);
  const uint32_t dirs[3] = {(pos << 2) + buf, a.output, a.obj + kFields};
  const uint32_t sizes[3] = {bytes, bytes, kTamFields};
  for (size_t i = 0; i < 3; ++i) {
    before[i].dir = recompiled[i].dir = dirs[i];
    before[i].bytes = recompiled[i].bytes = sizes[i];
    Save(base, before[i]);
  }
  __imp__sub_825CD088(ctx, base);
  for (size_t i = 0; i < 3; ++i) {
    Save(base, recompiled[i]);
  }
  for (size_t i = 0; i < 3; ++i) {
    Set(base, before[i]);
  }
  // The arguments were read before calling the recompiled code, which changes the volatile registers.
  Native(base, a);
  uint64_t differences = 0;
  for (size_t i = 0; i < 3; ++i) {
    const Range& r = recompiled[i];
    for (uint32_t off = 0; off + 4 <= r.bytes; off += 4) {
      if (std::memcmp(Dir(base, r.dir + off), r.data.data() + off, 4) != 0) {
        if (!g_difference_noted.exchange(true, std::memory_order_relaxed)) {
          REXLOG_WARN("[audio] filter native: first difference en 0x{:08X} (range {}), native 0x{:08X}, recompiled "
                      "0x{:08X}",
                      r.dir + off, i, Read32(base, r.dir + off),
                      (uint32_t(r.data[off]) << 24) | (uint32_t(r.data[off + 1]) << 16) |
                          (uint32_t(r.data[off + 2]) << 8) | uint32_t(r.data[off + 3]));
        }
        ++differences;
      }
    }
  }
  // The game continues with the recompiled code's result.
  for (size_t i = 0; i < 3; ++i) {
    Set(base, recompiled[i]);
  }
  g_differences.fetch_add(differences, std::memory_order_relaxed);
}

}  // namespace

void Filter825CD088(PPCContext& ctx, uint8_t* base) {
  const int32_t mode = REXCVAR_GET(nfsmw_audio_filter_native);
  if (mode != 1 && mode != 2) {
    __imp__sub_825CD088(ctx, base);
    return;
  }
  const int32_t n = ctx.r4.s32;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_samples.fetch_add(uint64_t(std::max(n, 0)), std::memory_order_relaxed);
  Report();
  if (mode == 2) {
    Validate(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  if (!Native(base, ReadArguments(ctx))) {
    g_reread.fetch_add(1, std::memory_order_relaxed);
  }
  ctx.r3.u64 = 1;
}

}  // namespace nfsmw::audio_filter
