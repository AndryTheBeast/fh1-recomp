// nfsmw - linear resamplers of the sound engine in native code
//
// On the console, the remaining robotic audio shows up in hard crashes, with the audio server thread at
// 70-94 % of a core. On PC the sound engine's two linear resamplers take 16.6 % of that thread:
// sub_82619820 (9.9 %) and sub_826031C0 (6.7 %). The recompiled code handles each sample with volatile reads
// and writes of guest memory (with byte swapping) and converts the integers to floating point through the
// stack; sub_82619820 also stores the position and the fraction to memory on every sample.
//
// Here they do the same floating-point operations in the same order as the recompiled code (subtraction,
// product and fma in double precision, rounded to single), so the result is bit-identical, without the extra
// accesses. Each sample i, with the position in 16.16 (integer part in pos, fraction in frac < 65536), source
// source and step step:
//   s0 = source[pos], s1 = source[pos + 1]
//   output[i] = s0 + (s1 - s0) * (frac * K)       K = the float at 0x820AFD18
//   acc = frac + step; pos += acc >> 16; frac = acc & 0xFFFF
//
// nfsmw_audio_resampling_native: 0 = recompiled code; 1 = native; 2 = validate: computes the native result
// separately, runs the recompiled code and compares each sample and the final state, bit by bit (the game uses
// the recompiled result). Every 10 s it logs how many calls and samples it has done and how many differences it
// saw.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_deferred_report.h"  // deferred reports

#include "nfsmw_audio_native.h"

// Native by default. On PC, in the alley test run, it gave 0 differences against the recompiled code over
// 130 million samples and runs 1.7 and 3.7 times faster.
REXCVAR_DEFINE_INT32(nfsmw_audio_resampling_native, 1, "NFSMW",
                     "Resamplers lineales del motor de sonido (sub_826031C0 y sub_82619820): 0 = code "
                     "recompiled, 1 = native (same result bit a bit; por default), 2 = validate el native contra "
                     "el recompiled");

REX_EXTERN(__imp__sub_826031C0);
REX_EXTERN(__imp__sub_82619820);

namespace nfsmw::audio_resampling {
namespace {

using namespace nfsmw::audio_native;

constexpr uint32_t kDirConstant = 0x820AFD18;  // lis r11,-32245; lfs f0,-744(r11)

// One sample, with the operations of the recompiled code (fsubs, fcfid, frsp, fmuls and fmadds from XenonRecomp).
inline float Interpolate(float s0, float s1, uint32_t frac, float k) {
  const double subtract = double(float(double(s1) - double(s0)));
  const double t = double(float(double(float(double(int64_t(frac)))) * double(k)));
  return float(double(float(std::fma(subtract, t, double(s0)))));
}

struct State {
  uint32_t pos;
  uint32_t frac;
};

// The common loop. write(i, sample) stores each sample as soon as it is computed, in the same order as the
// recompiled code (this matters if the destination overlaps the source).
template <typename Write>
State Loop(uint8_t* base, int32_t n, uint32_t source, uint32_t pos, uint32_t frac, uint32_t step, float k,
             Write write) {
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t dir = (pos << 2) + source;
    const float s0 = ReadFloat(base, dir);
    const float s1 = ReadFloat(base, dir + 4);
    write(i, Interpolate(s0, s1, frac, k));
    const uint32_t acc = frac + step;
    pos += (acc >> 16) & 0xFFFF;
    frac = acc & 0xFFFF;
  }
  return {pos, frac};
}

struct Counter {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> sample_total{0};
  std::atomic<uint64_t> differences{0};
  std::atomic<uint64_t> overwritten{0};  // sub_82619820 with the state inside the destination: use the recompiled code
};
Counter g_826031C0;
Counter g_82619820;
std::atomic<int64_t> g_last_report_ms{0};
std::atomic<bool> g_difference_noted{false};
thread_local std::vector<float> t_local;

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Count(Counter& c, int32_t n) {
  c.calls.fetch_add(1, std::memory_order_relaxed);
  c.sample_total.fetch_add(uint64_t(std::max(n, 0)), std::memory_order_relaxed);
  const int64_t now = NowMs();
  int64_t last = g_last_report_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10000 || !g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  NFSMW_REPORT_DEFERRED("[audio] resampling linear (mode {}): sub_826031C0 {} calls y {} sample_total, sub_82619820 {} calls y {} "
              "sample_total ({} con el state inside del target), differences con el recompiled {} y {}",
              REXCVAR_GET(nfsmw_audio_resampling_native), g_826031C0.calls.exchange(0),
              g_826031C0.sample_total.exchange(0), g_82619820.calls.exchange(0), g_82619820.sample_total.exchange(0),
              g_82619820.overwritten.exchange(0), g_826031C0.differences.exchange(0), g_82619820.differences.exchange(0));
}

void NoteDifference(const char* function, uint32_t target, int32_t index, uint32_t native, uint32_t recompiled) {
  if (!g_difference_noted.exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[audio] resampling linear: first difference en {}: target 0x{:08X}, sample {}, native 0x{:08X}, "
                "recompiled 0x{:08X}",
                function, target, index, native, recompiled);
  }
}

// Compares the destination and state left by the recompiled code with the separately computed result.
uint64_t Compare(const char* function, uint8_t* base, uint32_t target, int32_t n, const std::vector<float>& local,
                  uint32_t dir_pos, uint32_t dir_frac, State state) {
  uint64_t differences = 0;
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t recompiled = Read32(base, target + uint32_t(i) * 4);
    const uint32_t native = Bits(local[size_t(i)]);
    if (recompiled != native) {
      NoteDifference(function, target, i, native, recompiled);
      ++differences;
    }
  }
  if (Read32(base, dir_pos) != state.pos || Read32(base, dir_frac) != (state.frac << 16)) {
    NoteDifference(function, target, -1, state.pos, Read32(base, dir_pos));
    ++differences;
  }
  return differences;
}

}  // namespace

// sub_826031C0: r4 = samples, r5 = source, r6 = destination, r7 = &pos, r8 = &frac (in the upper half), r9 =
// integer part of the step, r10 = fraction of the step (in its upper half). Reads the state on entry and
// writes it on exit.
void Resampling826031C0(PPCContext& ctx, uint8_t* base) {
  const int32_t mode = REXCVAR_GET(nfsmw_audio_resampling_native);
  if (mode != 1 && mode != 2) {
    __imp__sub_826031C0(ctx, base);
    return;
  }
  const int32_t n = ctx.r4.s32;
  const uint32_t source = ctx.r5.u32;
  const uint32_t target = ctx.r6.u32;
  const uint32_t dir_pos = ctx.r7.u32;
  const uint32_t dir_frac = ctx.r8.u32;
  const uint32_t step = (ctx.r9.u32 << 16) | ((ctx.r10.u32 >> 16) & 0xFFFF);
  ctx.fpscr.disableFlushMode();
  const float k = ReadFloat(base, kDirConstant);
  const uint32_t pos = Read32(base, dir_pos);
  const uint32_t frac = Read16(base, dir_frac);
  Count(g_826031C0, n);
  if (mode == 2) {
    std::vector<float>& local = t_local;
    local.resize(size_t(std::max(n, 0)));
    const State state =
        Loop(base, n, source, pos, frac, step, k, [&](int32_t i, float v) { local[size_t(i)] = v; });
    __imp__sub_826031C0(ctx, base);
    g_826031C0.differences.fetch_add(Compare("sub_826031C0", base, target, n, local, dir_pos, dir_frac, state),
                                     std::memory_order_relaxed);
    return;
  }
  const State state = Loop(base, n, source, pos, frac, step, k, [&](int32_t i, float v) {
    WriteFloat(base, target + uint32_t(i) * 4, v);
  });
  Write32(base, dir_pos, state.pos);
  Write32(base, dir_frac, state.frac << 16);
  // Volatile registers as the recompiled code leaves them (just in case): r3 = pos, r9 = frac << 16,
  // r10 = step, f0 = K.
  ctx.r3.u64 = state.pos;
  ctx.r9.u64 = uint64_t(state.frac) << 16;
  ctx.r10.u64 = step;
  ctx.f0.f64 = double(k);
}

// sub_82619820: r3 = samples, r4 = source, r5 = destination, r6 = &pos, r7 = &frac (in the upper half), r8 =
// integer part of the step, r9 = fraction of the step (in its upper half). The recompiled code reads and writes
// the state in memory on every sample; if the state lies inside the destination, that changes the result, so
// that case is left to the recompiled code.
void Resampling82619820(PPCContext& ctx, uint8_t* base) {
  const int32_t mode = REXCVAR_GET(nfsmw_audio_resampling_native);
  if (mode != 1 && mode != 2) {
    __imp__sub_82619820(ctx, base);
    return;
  }
  const int32_t n = ctx.r3.s32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t target = ctx.r5.u32;
  const uint32_t dir_pos = ctx.r6.u32;
  const uint32_t dir_frac = ctx.r7.u32;
  const uint32_t step = (ctx.r8.u32 << 16) | ((ctx.r9.u32 >> 16) & 0xFFFF);
  const uint64_t bytes = uint64_t(std::max(n, 0)) * 4;
  if (Overlap(dir_pos, 4, target, bytes) || Overlap(dir_frac, 4, target, bytes) || Overlap(dir_pos, 4, dir_frac, 4)) {
    g_82619820.overwritten.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82619820(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  const float k = ReadFloat(base, kDirConstant);
  const uint32_t frac = Read16(base, dir_frac);
  const uint32_t pos = Read32(base, dir_pos);
  Count(g_82619820, n);
  if (mode == 2) {
    std::vector<float>& local = t_local;
    local.resize(size_t(std::max(n, 0)));
    const State state =
        Loop(base, n, source, pos, frac, step, k, [&](int32_t i, float v) { local[size_t(i)] = v; });
    __imp__sub_82619820(ctx, base);
    g_82619820.differences.fetch_add(Compare("sub_82619820", base, target, n, local, dir_pos, dir_frac, state),
                                     std::memory_order_relaxed);
    return;
  }
  const State state = Loop(base, n, source, pos, frac, step, k, [&](int32_t i, float v) {
    WriteFloat(base, target + uint32_t(i) * 4, v);
  });
  Write32(base, dir_pos, state.pos);
  Write32(base, dir_frac, state.frac << 16);
  // Volatile registers as the recompiled code leaves them: r3 = frac << 16, r4 = frac, r9 = 0, r10 = step, f0 = K.
  ctx.r3.u64 = uint64_t(state.frac) << 16;
  ctx.r4.u64 = state.frac;
  ctx.r9.u64 = 0;
  ctx.r10.u64 = step;
  ctx.f0.f64 = double(k);
}

}  // namespace nfsmw::audio_resampling
