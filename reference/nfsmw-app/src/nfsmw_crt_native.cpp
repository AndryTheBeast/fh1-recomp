// nfsmw - native memset of the game's CRT (sub_826BE610)
//
// sub_826BE610 is the memset of the game's CRT, as seen in the generated code:
//  - writes byte by byte until the destination is 4-byte aligned;
//  - replicates the byte into a word with two rlwimi;
//  - writes 16-byte blocks with four stw, then the remaining words and the leftover bytes;
//  - leaves r3 untouched.
// It is the sibling of the memcpy at 0x826BE1B0, which already goes to the host's through [rexcrt] in
// overrides.toml. Putting it there too would require regenerating and rebuilding all the code; a hook in the
// app has the same effect. The recompiled code writes with volatile stores and byte swapping and std::memset
// writes the same thing in one go: memory ends up identical. Only volatile registers change (r0, r4-r6, ctr
// and cr0), which the caller does not read after the call.
// The audio calls it for every source and packet (sub_825DCED8), but the whole game uses it.
//
// On the Switch, if the write lands on a watched page, the exception handler unwatches it and replays the
// instruction without decoding it (exception_handler_switch.cpp), just like with the host memcpy.
//
// nfsmw_crt_memset_native: 0 = recompiled; 1 = native (default); 2 = validate: runs the recompiled code, saves
// what it wrote, undoes it, runs the native code, compares and keeps the recompiled result (up to 1 MB per
// call). With mode 2 or with nfsmw_crt_diag, calls and bytes are counted and a summary is logged every 10 s.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(nfsmw_crt_memset_native, 1, "NFSMW",
                     "memset del CRT del game (sub_826BE610): 0 = code recompiled, 1 = memset del host (la memory_block "
                     "queda equal; por default), 2 = validate el native contra el recompiled");
REXCVAR_DEFINE_BOOL(nfsmw_crt_diag, false, "NFSMW",
                    "Diagnostic: count the calls y los bytes del memset del game (sub_826BE610) y anota un "
                    "summary every 10 s");

REX_EXTERN(__imp__sub_826BE610);

namespace nfsmw::crt {
namespace {

constexpr uint32_t kMaxValidate = uint32_t(1) << 20;

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_validated{0};
std::atomic<uint64_t> g_without_validate{0};
std::atomic<uint64_t> g_differences{0};
std::atomic<int64_t> g_last_report_ms{0};
std::atomic<bool> g_difference_noted{false};

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Report(int32_t mode) {
  const int64_t now = NowMs();
  int64_t last = g_last_report_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10000 || !g_last_report_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  REXLOG_INFO("[crt] memset del game (mode {}) en {:.1f} s: {} calls y {} KB; validated {}, sin validate (mas de "
              "1 MB) {}, differences con el recompiled {}",
              mode, double(now - last) / 1000.0, g_calls.exchange(0), g_bytes.exchange(0) >> 10,
              g_validated.exchange(0), g_without_validate.exchange(0), g_differences.exchange(0));
}

// Same arguments as the PPC: 32-bit destination and count, 8-bit value. r3 stays the same, as in the
// recompiled code.
inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r5.u32;
  if (n != 0) {
    std::memset(rex::memory::GuestPtr<uint8_t*>(base, ctx.r3.u32), ctx.r4.u8, n);
  }
}

void Validate(PPCContext& ctx, uint8_t* base) {
  const uint32_t target = ctx.r3.u32;
  const uint32_t n = ctx.r5.u32;
  const uint8_t input_value = ctx.r4.u8;
  if (n == 0 || n > kMaxValidate) {
    if (n != 0) {
      g_without_validate.fetch_add(1, std::memory_order_relaxed);
    }
    __imp__sub_826BE610(ctx, base);
    return;
  }
  thread_local std::vector<uint8_t> before;
  thread_local std::vector<uint8_t> recompiled;
  uint8_t* p = rex::memory::GuestPtr<uint8_t*>(base, target);
  before.assign(p, p + n);
  __imp__sub_826BE610(ctx, base);
  const uint32_t r3_recompiled = ctx.r3.u32;
  recompiled.assign(p, p + n);
  std::memcpy(p, before.data(), n);
  std::memset(p, input_value, n);
  if (std::memcmp(p, recompiled.data(), n) != 0 || r3_recompiled != target) {
    g_differences.fetch_add(1, std::memory_order_relaxed);
    if (!g_difference_noted.exchange(true, std::memory_order_relaxed)) {
      REXLOG_WARN("[crt] memset del game: first difference en 0x{:08X}, {} bytes, input_value 0x{:02X}, r3 del "
                  "recompiled 0x{:08X}",
                  target, n, input_value, r3_recompiled);
    }
  }
  std::memcpy(p, recompiled.data(), n);  // the game continues with the recompiled result
  g_validated.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void Memset826BE610(PPCContext& ctx, uint8_t* base) {
  const int32_t mode = REXCVAR_GET(nfsmw_crt_memset_native);
  if (mode == 1 && !REXCVAR_GET(nfsmw_crt_diag)) {
    Native(ctx, base);
    return;
  }
  if (mode != 1 && mode != 2) {
    __imp__sub_826BE610(ctx, base);
    return;
  }
  const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_bytes.fetch_add(ctx.r5.u32, std::memory_order_relaxed);
  if ((calls & 0x3FF) == 0) {
    Report(mode);
  }
  if (mode == 2) {
    Validate(ctx, base);
  } else {
    Native(ctx, base);
  }
}

}  // namespace nfsmw::crt

// A single hook per function: on the Switch the linker accepts duplicate definitions and would silently keep one.
REX_HOOK_RAW(sub_826BE610) {
  nfsmw::crt::Memset826BE610(ctx, base);
}
