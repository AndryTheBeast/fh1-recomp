// fh1 - CPU-saving hooks on the game's own code.
//
// The game's render thread waits for the GPU command processor (ring space, read pointer) in a
// busy loop: sub_829F04A8 is one poll of that wait (a ~128-cycle db16cyc delay, then it reads the
// ring read pointer and a timeout clock, returns 1 = keep waiting, 0 = done) and is called ~465,000
// times per frame - about half of the render thread's time (docs/performance-review.md 2.2). On the
// PC that half core is free; on the Switch (3 usable cores) it starves the GPU command thread. With
// --fh1_yield_ring_wait, every "keep waiting" poll gives the core away (yield), and after a long
// streak of polls sleeps briefly, so the waiting costs almost no CPU.

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(fh1_yield_ring_wait, true, "FH1",
                    "Yield the CPU in the game's GPU ring busy-wait (sub_829F04A8) instead of "
                    "spinning");

REXCVAR_DEFINE_INT32(fh1_ring_wait_sleep_after, 0, "FH1",
                     "With fh1_yield_ring_wait: after this many polls in a row, sleep 50 us per "
                     "poll instead of yielding (0 = never sleep; Windows sleeps overshoot to ~1 ms "
                     "and cost frame pacing)");

namespace {
thread_local uint32_t g_ring_wait_streak = 0;
}  // namespace

REX_EXTERN(__imp__sub_829F04A8);
void Fh1ShaderDumpTick(const uint8_t* base);  // fh1_shader_dump.cpp

REX_HOOK_RAW(sub_829F04A8) {
  __imp__sub_829F04A8(ctx, base);
  Fh1ShaderDumpTick(base);
  if (!REXCVAR_GET(fh1_yield_ring_wait)) {
    return;
  }
  if (ctx.r3.u32 != 1) {
    g_ring_wait_streak = 0;
    return;
  }
  // Still waiting: yield (no latency if nothing else wants the core). Optionally, after a long
  // streak of polls, sleep (for platforms with fine-grained sleeps).
  int32_t sleep_after = REXCVAR_GET(fh1_ring_wait_sleep_after);
  if (sleep_after <= 0 || ++g_ring_wait_streak < uint32_t(sleep_after)) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}
