// nfsmw - cutscene diagnostics (WMV)
//
// With nfsmw_video_diag = true, sub_826DB4F8 is logged (presentation of a movie frame, main thread,
// docs/audio-and-video.md): its caller, the video object, its size, the widths, strides and heights of
// the planes, the pending frames and the texture group, and every 5 s the rate and the time spent
// inside. The decoding (sub_827312C0, sub_828C35D8 and sub_8278A518) is in nfsmw_video_native.cpp.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nfsmw_video_diag, false, "NFSMW",
                    "Diagnostic de the cinematicas: anota la presentacion de frames de the peliculas (quien "
                    "llama, thread_value, ritmo y buffers)");

namespace nfsmw::video_diag {
namespace {

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

int64_t NowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t IdThread() {
  return std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFFFF;
}

struct Counter {
  std::atomic<uint64_t> calls{0};
  std::atomic<int64_t> since_us{0};
  std::atomic<uint64_t> calls_since{0};
  std::atomic<int64_t> us_inside{0};
};
Counter g_present;

// A summary every 5 s: calls per second and average time inside.
void Summary(const char* que, Counter& c, int64_t now) {
  int64_t since = c.since_us.load(std::memory_order_relaxed);
  if (since == 0) {
    c.since_us.store(now, std::memory_order_relaxed);
    c.calls_since.store(c.calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return;
  }
  if (now - since < 5000000) {
    return;
  }
  if (!c.since_us.compare_exchange_strong(since, now)) {
    return;
  }
  const uint64_t total = c.calls.load(std::memory_order_relaxed);
  const uint64_t n = total - c.calls_since.exchange(total);
  const int64_t inside = c.us_inside.exchange(0);
  REXLOG_INFO("[video] {}: {:.1f} calls/s, {:.2f} ms inside de media ({} en total)", que,
              double(n) * 1e6 / double(now - since), n ? double(inside) / double(n) / 1000.0 : 0.0, total);
}

}  // namespace
}  // namespace nfsmw::video_diag

REX_EXTERN(__imp__sub_826DB4F8);

REX_HOOK_RAW(sub_826DB4F8) {
  using namespace nfsmw::video_diag;
  if (!REXCVAR_GET(nfsmw_video_diag)) {
    __imp__sub_826DB4F8(ctx, base);
    return;
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t obj = ctx.r3.u32;
  const uint64_t n = g_present.calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 6 || n % 300 == 0) {
    REXLOG_INFO("[video] present #{} lr={:08X} obj={:08X} thread_value={:06X} r4={:08X} r5={:08X} | {}x{} +44={:08X} "
                "Y {}x{} step {} U {}x{} step {} V {}x{} step {} pending_2={} group={}",
                n, lr, obj, IdThread(), ctx.r4.u32, ctx.r5.u32, Read32(base, obj + 124), Read32(base, obj + 128),
                Read32(base, obj + 44), Read32(base, obj + 332), Read32(base, obj + 356), Read32(base, obj + 344),
                Read32(base, obj + 336), Read32(base, obj + 360), Read32(base, obj + 348),
                Read32(base, obj + 340), Read32(base, obj + 364), Read32(base, obj + 352),
                Read32(base, obj + 368), Read32(base, obj + 372));
  }
  const int64_t before = NowUs();
  __imp__sub_826DB4F8(ctx, base);
  const int64_t after = NowUs();
  g_present.us_inside.fetch_add(after - before, std::memory_order_relaxed);
  Summary("present", g_present, after);
}
