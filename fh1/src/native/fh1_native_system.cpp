// fh1 - native renderer, part C1: the app's own graphics system.
//
// What it does
//   Replaces the Xenos emulation plugin when fh1_renderer is
//   "native" (OnPreSetup in fh1_app.h decides it):
//     - Presentation: the SDK's VulkanProvider + VulkanPresenter, same as
//       the emulation. ReXApp sees presenter() and sets up F3, settings and
//       the console the usual way (rex_app.cpp:386-398).
//     - The game's D3D keeps running unchanged and fills its command ring. A
//       "sink" consumes it right away: it answers what the game expects from
//       the GPU and hands the draws, copies and Swaps to the rest of the
//       native renderer.
//
// What the game expects from the GPU, and why each thing is needed
//   1. MMIO registers at 0x7FC80000. Without a registered range REX_MM_LOAD_U32
//      reads uninitialized garbage (mmio_handler.cpp:105-124). The same fixed
//      values as the emulation are returned (graphics_system.cpp:232-262).
//   2. The ring read pointer: the game spins in sub_82597690 watching that
//      word to know how much room it has left.
//   3. Interrupts: vblank (sub_82597960 counts vblanks) and PM4_INTERRUPT
//      packets, which signal that the GPU reached that point of the ring.
//   4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM and
//      EVENT_WRITE_*), with the semantics of graphics/command_processor.cpp.
//      Occlusion queries return 1000 samples, like the emulation.
//
// Where the rest is
//   fh1_native_targets.cpp does the copies (resolve and clear) and presents
//   the resolved texture of each Swap, fh1_native_shaders.cpp identifies the
//   shaders that reach the ring, and fh1_native_draws.cpp draws the
//   geometry. See docs/nfsmw-nx/native-renderer.md.

#include "fh1_native_system.h"

#include "fh1_hitch_waits.h"
#include "fh1_fence_wait.h"
#include "fh1_short_wait.h"
#include "fh1_native_targets.h"
#include "fh1_native_hooks.h"
#include "fh1_native_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read keys[4] before writing it and the same texture was
 * created several times (see docs/nfsmw-nx/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would "
       "come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

// The summary of the scenery LOD hook (fh1_scenery_lod.cpp).
// Declared here instead of creating a header for a single function.
namespace fh1::scenery_lod {
std::string Summary();
}  // namespace fh1::scenery_lod

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriority. That header deliberately does not include switch.h.
#include "../../sdk/src/core/threading_switch.h"
#endif

REXCVAR_DEFINE_STRING(fh1_renderer, "xenos", "FH1",
                      "xenos = Xbox GPU translation layer; native = the native Vulkan renderer")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(fh1_native_vector_registers, true, "FH1",
                    "Native renderer (24/09, build 164): the ring's register blocks are copied 4 at a time with "
                    "NEON. The first 200,000 blocks are checked against the usual path and, if one differs, it "
                    "turns itself off. false = the usual path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(fh1_native_registers_in_block, true, "FH1",
                    "Native renderer: register blocks without side effects (almost always VS and PS constants) are "
                    "written at once instead of register by register (build 130). false: as before");

/*
 * The report is written from the ring thread, and that causes real stutters.
 *
 * Measured over 170 race stutters: the periodic dump explains 33 of them (19.4 %) and 1,267 ms of the 5,707
 * lost. It is not a coincidence: the report runs on a timer, not on what happens in the game, and still 13 of
 * the 30 reports of the race land inside a stuttering frame, when chance would give 1.53 %. That is 28 times the
 * background probability. Each block is 31-37 lines and 5.9-7.4 KB, and takes 14-40 ms to write.
 *
 * Interval: 20 s. A longer interval means fewer dumps, but each one costs twice as much and the damage
 * concentrates in clusters of 5-6 stutters in a row; even so, measured side by side, 10 s loses twice the time:
 *         20 s:  33 stutters from this cause, 211 ms of stutter per minute of racing
 *         10 s:  58 stutters,                 428 ms/min
 *     Half a millisecond per frame thrown away, of the same order as what the query pools gained.
 *
 * So 20. The clusters are ugly, but the total rules, and with 20 s there are still ~15 reports per session,
 * plenty to measure.
 *
 * The real fix is not spacing the dump out but keeping its cost off the ring thread, so that no frame pays for
 * the whole block: see the next comment.
 */
REXCVAR_DEFINE_INT32(fh1_native_report_s, 20, "FH1",
                     "Native renderer: seconds between dumps of the diagnostic report. Each dump is written by the "
                     "thread that feeds the GPU and causes hitches: 10 gives twice the detail and twice the "
                     "hitches from this cause")
    .range(5, 120);

/*
 * The dump is written on another thread.
 *
 * What the comment above asked for, with a new measurement: the block takes 29-32 ms on the ring thread. Of the
 * 6 stutters of 60 ms or more in a race, 3 end 40-70 ms after a dump starts and 1 within 0.12 s of the
 * "C6 substages" line, when chance would give 0.5; and 3 of the 7 dumps of the race produced a stutter of 60 ms
 * or more. It is not the formatting: the log is synchronous (log_async = false) and the 2-9 ms gaps between
 * lines fall every 2-4 lines, when the FILE is flushed to the SD.
 *
 * Lines are still formatted on the ring (they read its state, which is only coherent on its thread) and
 * enqueued; the "FH1 reports" thread writes them, sleeping on a condition variable. Both lessons of the
 * failed asynchronous logger (see log_async in sdk/src/core/logging.cpp) are covered: the queue never blocks
 * the producer (full = the line is dropped and counted) and the thread runs at 0x3B, the only time-sliced
 * priority on Horizon. One point of contention remains: if the ring writes a line of its own right while this
 * thread writes another, it waits for that line to finish (the log lock), not for the whole block.
 * false = as before: the ring writes every line itself.
 */
REXCVAR_DEFINE_BOOL(fh1_native_deferred_reports, true, "FH1",
                    "Native renderer (25/09, build 179): the periodic report and the ring thread's [hitch] lines "
                    "are written to the SD card from a separate thread; the ring only formats and queues them. "
                    "false = as before, the ring writes them itself (29-32 ms every 20 s in 176)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD without memcmp. On every IM_LOAD the ring compared the whole microcode with guest memory (~0.6 us,
 * ~41,000 per second in a race). Now the D3D constructors and the fetch patcher report what they write
 * (fh1_microcode_versions.h, with the cross-thread protocol) and the ring only compares if someone has
 * written that microcode since the last check. The self-checking guard is in LoadShaderCached.
 */
#include "fh1_microcode_versions.h"

REXCVAR_DEFINE_BOOL(fh1_native_im_load_without_memcmp, true, "FH1",
                    "Native renderer (25/09, build 184): an IM_LOAD whose microcode nobody has written since the "
                    "last check (the constructors and the D3D fetch patcher report it) is taken from the cache "
                    "without a full memcmp. It starts by checking every load against the memcmp and turns itself "
                    "off at the first disagreement. false = memcmp on every IM_LOAD, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD_IMMEDIATE with an exact cache. sub_825A37D8 (D3D) copies the VS into the ring itself with the
 * fetches patched and, if needed, with the outputs the PS does not read nulled: op 2B, 4,000-8,000 per second
 * in stretches of a race, at 1.5-2.8 us each ("times per packet"). Now a packet whose microcode is
 * byte-identical to one already seen (memcmp against the stored raw copy) reuses its swapped copy, its
 * identification, its fingerprint and its generation. The self-checking guard is in LoadImmediate.
 */
REXCVAR_DEFINE_BOOL(fh1_native_im_immediate_cache, true, "FH1",
                    "Native renderer (25/09, build 184): an IM_LOAD_IMMEDIATE (the VS the D3D copies into the ring "
                    "with the patched fetches) identical byte for byte to one already seen reuses its byte-swapped "
                    "copy, its identification, its fingerprint and its generation, without swapping or identifying "
                    "again. It starts by checking every hit against the usual path and turns itself off at the "
                    "first disagreement. false = no cache, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames, one per Swap (nfsmw-nx's D3D trace; stubbed in Carbon). Only for the NoteGameByAhead measurement.
extern std::atomic<uint64_t> g_fh1_frames_game;

namespace fh1::native {
namespace {

// The line queue and its thread. Created with the first line; it lives until exit.
class QueueReports {
 public:
  ~QueueReports() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    warning_.notify_one();
    if (thread_.joinable()) {
      thread_.join();  // writes whatever is left before exiting
    }
  }

  // false if there is no thread (it could not be created, or shutdown is under way): then the caller writes
  // the line.
  bool Enqueue(std::string&& line) {
    bool notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || without_thread_) {
        return false;
      }
      if (!thread_.joinable()) {
        try {
          thread_ = std::thread([this] { Loop(); });  // persistent: on Horizon detach() closes the game
        } catch (...) {
          without_thread_ = true;  // no thread (Horizon thread limit): everything as before, nothing breaks
          return false;
        }
      }
      if (queue_.size() >= kMaxLines) {
        ++lost_;  // never block the producer: the line is dropped and counted
        return true;
      }
      notify = queue_.empty();  // the thread only sleeps with the queue empty: one wake-up per burst, not per line
      queue_.push_back(std::move(line));
    }
    if (notify) {
      warning_.notify_one();
    }
    return true;
  }

 private:
  static constexpr size_t kMaxLines = 4096;

  void Loop() {
    rex::thread::set_current_thread_name("FH1 reports");
#if REX_PLATFORM_SWITCH
    const bool priority = RexSwitchSetCurrentThreadPriorityOk(REX_SWITCH_PRIO_GUEST);
#else
    const bool priority = true;
#endif
    REXLOG_INFO("[native] deferred reports (build 179): thread running{}",
                priority ? "" : " (the kernel did NOT accept priority 0x3B)");
    std::deque<std::string> batch;
    for (;;) {
      uint64_t lost = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        warning_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;  // stop_ and nothing pending
        }
        batch.swap(queue_);
        lost = lost_;
        lost_ = 0;
      }
      for (const std::string& line : batch) {
        REXLOG_INFO("{}", line);
      }
      batch.clear();
      if (lost) {
        REXLOG_WARN("[native] deferred reports: {} lines lost (the queue of {} was full)", lost,
                    kMaxLines);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::string> queue_;  // under mutex_
  std::thread thread_;              // created with mutex_ held
  bool stop_ = false;            // under mutex_
  bool without_thread_ = false;         // with mutex_: the thread could not be created
  uint64_t lost_ = 0;         // under mutex_
};

}  // namespace

void ReportDeferred(std::string line) {
  static const bool defer = REXCVAR_GET(fh1_native_deferred_reports);
  if (!defer) {
    REXLOG_INFO("{}", line);
    return;
  }
  static QueueReports queue;
  if (!queue.Enqueue(std::move(line))) {
    REXLOG_INFO("{}", line);  // no report thread: as before (Enqueue has not moved it)
  }
}

}  // namespace fh1::native

/*
 * See LoopRing. Lowered so the presentation thread (0x2C) can preempt it; at the same priority the
 * presenter went without a core 70 % of the time.
 */
REXCVAR_DEFINE_INT32(fh1_native_ring_priority, 0x2D, "FH1",
                     "Switch: Horizon priority of the ring thread (0x1C-0x3B). 0x2D by default, one step below the "
                     "presenter (0x2C) and two below the audio (0x2B). Raising it to 0x2C brings back the split of "
                     "build 113, in which only half the frames reached the screen");
/*
 * Which core the ring starts on.
 *
 * Pinning the Vulkan thread to a separate CPU core was suggested, and the data support looking at it: the ring
 * uses 94.7 % of a core and the game's main thread 88.2 %, and both run with "preferred core -1", which on
 * Horizon is the process default core: the same one for both. The kernel has to keep moving them, and every
 * migration throws away the thread's L1.
 *
 * That does not show in the mean (total CPU does not correlate with FPS: 258 % at 32 FPS and 280 % at 21) but in
 * the variance, which is what is noticeable. And the variance is exactly the problem: 54 % of frames fall in
 * 30-36 ms, straddling the 33.3 vblank, so a few tenths decide whether a frame shows at 33 ms or jumps to 50.
 *
 * It is not an exclusive affinity: it only changes the preferred core and leaves the mask as it was, so the
 * kernel can still move the thread if needed and this cannot cause starvation. That is why it can be on by
 * default.
 *
 * How it is checked: the "C6 ring" line of the log counts migrations per second and says which core the
 * thread is on. Measured: the migrations are the same with -1 and with 1 (see the next comment).
 */
REXCVAR_DEFINE_INT32(fh1_native_ring_core, 1, "FH1",
                     "Switch: preferred core of the ring thread (-1 = the default one, the same as the game "
                     "thread's; 0-2 = that core). Not exclusive: the mask is not touched, so the kernel can still "
                     "move it");
/*
 * The real pin.
 *
 * Measured: the preferred core does not help. The kernel accepted it ("preferred 1 (accepted)") and still the
 * thread showed up on cores 0, 1 and 2 with 0.24 migrations per loop, the same figure wherever it is set.
 * Horizon moves it anyway.
 *
 * It also became clear that migrations by themselves are not the problem: 44 per second, at about 12 us to
 * refill the L1 each, is 0.5 ms/s = 0.05 % of CPU. Negligible.
 *
 * What could matter is removing the competition, which is something else and which the migration counter does
 * not see: the ring (94.7 % of a core) and the game's main thread (88.2 %) want 183 % of a core that gives 100,
 * so they preempt each other. With the mask set to a single core the ring stops competing; the other two cores
 * (200 %) have to carry the remaining 183 %, which barely fits.
 *
 * Off by default: fixing a mask can leave a thread unable to run (unlike the preferred core), and measured on
 * the console, pinning the ring to one core made the peaks worse: the minimum dropped to 16.3 FPS against about
 * 21 without pinning (see nfsc-nx docs/nfsmw-nx/platform-notes.md, Threads). The A/B is done from the toml without recompiling,
 * and the log says whether migrations drop to zero.
 */
REXCVAR_DEFINE_BOOL(fh1_native_ring_core_exclusive, false, "FH1",
                    "Switch: besides preferring the core of fh1_native_ring_core, it FORBIDS the others "
                    "(exclusive mask). This is the real pin. Careful: if that core saturates, the ring stops "
                    "running; watch the migrations/s in the log, which must drop to 0");
REXCVAR_DEFINE_INT32(fh1_native_wait_regmem_us, 200, "FH1",
                     "Native renderer: pause between WAIT_REG_MEM polls in microseconds, after 8 quick yields "
                     "(NFSC: high-resolution timer on Windows, so it is accurate; was 1000)");
REXCVAR_DEFINE_BOOL(fh1_native_swap_test_color, false, "FH1",
                    "Native renderer: a Swap without a game image paints a pulsing blue-green test color instead of "
                    "black (tests only: tells a running game from a stalled one)");
REXCVAR_DEFINE_INT32(fh1_native_diag_frame_s, 0, "FH1",
                     "Native renderer: after this many seconds, logs every draw and every copy of one whole frame "
                     "(0 = no; tests only)");
REXCVAR_DEFINE_STRING(fh1_native_diag_vertices_ps, "", "FH1",
                      "Native renderer: in the traced frame, for the draws with these PS (comma-separated numbers) "
                      "logs the VS fetches and the bytes of their first vertices and texels (tests only)");
// The game measures with an occlusion query how much of the sun is visible (sub_82225438: GetData, Issue(BEGIN),
// a draw and Issue(END)) and uses it to turn off the flare when trees or terrain cover it. With the faked count of
// 1000 samples the flare was always drawn in full: sky burned to white and blue or purple halos in the trees.
REXCVAR_DEFINE_INT32(fh1_native_occlusion, 1, "FH1",
                     "Native renderer: the game's occlusion queries (the sun flare). 1 = measured on the GPU, like "
                     "the Xbox 360; 0 = faked count of 1000 samples (the behavior before build 146: the flare is "
                     "never hidden); 2 = faked count of 0 samples (tests only: never a flare)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(fh1_native_diag_constants_ps, "", "FH1",
                      "Native renderer (tests only): PS numbers (comma-separated) whose first 12 constants are "
                      "logged, at most every fh1_native_diag_constants_ms");
REXCVAR_DEFINE_INT32(fh1_native_diag_constants_ms, 250, "FH1",
                     "Native renderer (tests only): minimum interval between constant logs per PS")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(fh1_native_occlusion_toggle_s, 0, "FH1",
                     "Native renderer (tests only): with fh1_native_occlusion = 1 and N > 0, toggles every N "
                     "seconds between measured queries (even intervals) and the faked count of 1000 (odd "
                     "intervals), and logs each change, to compare the sun flare in the same race")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(fh1_native_occlusion_scale, 0, "FH1",
                     "Native renderer: samples per pixel used to count the occlusion queries (0 = those of the "
                     "antialiasing mode the game has chosen, like the Xbox 360; 1-16 = fixed, tests only)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in fh1_clip_shadows.cpp, which owns the switch.
namespace fh1::guard30 {
void Beat(double ms);
void Report();
}  // namespace fh1::guard30

namespace fh1::render_targets {
uint32_t SamplesOriginalModeCurrent(const uint8_t* base);  // stub in fh1_native_stubs.cpp (nfsmw-nx's render-target hooks)
}

namespace fh1::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Clock = std::chrono::steady_clock;
using ContextOutput = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

std::atomic<uint64_t> g_swaps_native{0};  // SwapsNative(), for the watchdog

/*
 * Adding without a lock in the counters only the ring writes.
 *
 * Stack sampling pointed here: 36 % of the ring thread's time is in LoopRing -> Packet -> PacketType3. And
 * every packet did a `fetch_add` on an atomic. On Cortex-A57 (ARMv8.0, without the ARMv8.1 atomic instructions)
 * a fetch_add is an ldxr/stxr loop with an exclusive reservation: ~15-20 cycles, and it also invalidates the line
 * on the other cores. At 83,000 packets per frame that is ~1.1 ms per frame spent counting packets.
 *
 * Why removing it is safe: these counters are written by a single thread, the ring thread (Packet and
 * PacketType3 are only called from LoopRing and from BufferIndirect, which hangs off it), and the only reader
 * is Report, which runs on that same thread (LoopRing calls it on every loop). The shutdown Report(true)
 * runs after thread_ring_->Wait(), that is, with the thread already dead. Without two writers there is no need
 * for an atomic read-modify-write.
 *
 * They stay std::atomic on purpose (the type does not change, nor do the readers); the only thing that changes
 * is how they are incremented: a relaxed load and store, which on 64-bit ARM64 are a plain ldr and str, without
 * exclusive reservation and without barriers. It is still correct for an outside reader (it never sees half a
 * value) and it stops costing.
 *
 * Do not use this for counters another thread touches: writes_wptr_ (written by the game thread through
 * MMIO), vblanks_ and counter_ (the vblank thread). Those keep fetch_add.
 */
template <typename T>
inline void AddOnlyRing(std::atomic<T>& counter, T how_much = 1) {
  counter.store(counter.load(std::memory_order_relaxed) + how_much, std::memory_order_relaxed);
}

constexpr uint32_t kBaseMmio = 0x7FC80000;
/*
 * From 16 to 128, because the packet count grew sixfold.
 *
 * 16 was chosen when there were ~14,000 packets per frame. Stack sampling gives the real count: 20.8 million
 * packets per interval, which at 18.1 packets per draw is ~83,000 per frame. At 1 in 16 that is 5,187 clock
 * reads per frame, and each Clock::now() is a CNTPCT_EL0 read plus the conversion.
 *
 * With 128 there are 648 samples per frame, still plenty for a mean: the relative error of a sample of 648 is
 * 4 %, and these times are read to split percentages, not for exact figures.
 */
constexpr uint64_t kStopwatchPacketsEvery = 128;  // packets per timed one (power of 2)
/*
 * From 8 to 64. At 1 in 8, this timer and above all the stage timer in fh1_native_draws.cpp (about 20 clock
 * reads per timed draw) added up to thousands of clock reads per frame in the alley, with 2,500-4,000 draws.
 * With 64 there are ~1,400 measured draws per second, plenty for a mean every 20 s. The "times" line keeps
 * its scale: the measured time is multiplied by this same factor and divided by all the draws.
 */
constexpr uint64_t kStopwatchDrawsEvery = 64;   // draws per timed one (power of 2)
constexpr uint32_t kMaskMmio = 0xFFFF0000;
constexpr uint32_t kSizeMmio = 0x0000FFFF;
constexpr uint32_t kNumRegisters = 0x5003;  // RegisterFile::kRegisterCount

// Registers with a fixed value or with an effect, as in graphics_system.cpp.
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;
// Gamma ramp, from DC_LUT_RW_MODE (0x1921) to DC_LUT_WRITE_EN_MASK (0x1927) (register_table.inc).
// The mask is 0x1927 (an earlier version stopped at 0x1926), and the output applies the ramp.
constexpr uint32_t kRegRampFirst = 0x1921;
constexpr uint32_t kRegRampIndex = 0x1922;
constexpr uint32_t kRegRampSequential = 0x1923;
constexpr uint32_t kRegRampPwl = 0x1924;  // DC_LUT_PWL_DATA
constexpr uint32_t kRegRamp30 = 0x1925;
constexpr uint32_t kRegRampMask = 0x1927;
constexpr uint32_t kRegRampLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

/*
 * Register generations, so that a draw does not redo work whose inputs did not change.
 *
 * Measured on the console (race): the ring thread is busy 42.5 ms of every 44.2 ms frame, so it is the
 * limit, not the GPU (37.1 ms of real work). Of those 42.5 ms, draw packets (op 22) take 29.5, and inside
 * the translator the most expensive stages are "textures" (4.1 ms) and half of "pipeline", which is
 * really viewport and scissor.
 *
 * Both are pure functions of registers that almost never change between consecutive draws: the fetch
 * constants (0x4800-0x48BF) and the viewport state (viewport, scissor, clip and window offset). This
 * tracks when they really change (same technique as generation_constants_vs_/ps_: compare the value
 * before writing it), and the number travels in RequestDraw so the translator can skip the work with
 * a single uint64 comparison.
 *
 * There are 32 fetch constants of 6 words each: the first 16 are used by textures (kRegFetch in
 * fh1_native_draws.cpp) and vertex fetches read theirs from the same block.
 */
constexpr uint32_t kRegFetchFirst = 0x4800;  // SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kRegFetchLast = 0x48BF;   // SHADER_CONSTANT_FETCH_31_5

// Viewport state registers: PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR, PA_CL_VPORT_[XYZ]SCALE/OFFSET,
// PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and PA_SU_VTX_CNTL. No VGT_* register is included:
// VGT_DRAW_INITIATOR (0x21FC) changes on every draw and would make the generation useless.
constexpr uint32_t kRegFramingFirst = 0x2080;
constexpr uint32_t kRegFramingLast = 0x2302;
inline bool IsRegisterOfFraming(uint32_t index) {
  // One subtraction and one comparison: rejects everything outside the range.
  if (index - kRegFramingFirst > kRegFramingLast - kRegFramingFirst) {
    return false;
  }
  return index <= 0x2082 ||                      // PA_SC_WINDOW_OFFSET .. SCISSOR_BR
         (index >= 0x210F && index <= 0x2114) ||  // PA_CL_VPORT_XSCALE .. ZOFFSET
         (index >= 0x2204 && index <= 0x2206) ||  // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL
         index == 0x2302;                          // PA_SU_VTX_CNTL
}

/*
 * Phase 2 of the Direct3D-level renderer: the ring-thread half.
 * Applying registers was measured at ~146 ms/s of the ring thread (~4 ms per frame, with the ring at
 * 95 %), almost all of it in WriteRegistersInBlock: 94.5 million words in blocks against 2.2 million
 * one at a time in 10 s. The regular loop (RangeScale) byte-swaps each word, compares it and classifies
 * it with several branches. RangeVector does the same 4 words at a time with NEON: it splits the span
 * at the boundaries of each class (VS, PS and fetch constants and the small viewport state pieces), copies
 * each piece byte-swapped and only checks whether anything changed in the piece, which is all that was
 * used from the classification.
 */
inline void RangeScale(uint32_t index, uint32_t* target, const uint8_t* source, uint32_t n, bool& vs,
                         bool& ps, bool& fetch, bool& framing) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t new_value = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
    if (target[i] != new_value) {
      const uint32_t abs = index + i;
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsRegisterOfFraming(abs)) {
        framing = true;
      }
      target[i] = new_value;
    }
  }
}

// Copies n big-endian words byte-swapped; true if any of them differed from the previous value.
inline bool CopyWithChange(uint32_t* target, const uint8_t* source, uint32_t n) {
  uint32_t i = 0;
  bool change = false;
#if defined(__aarch64__)
  uint32x4_t difference = vdupq_n_u32(0);
  for (; i + 4 <= n; i += 4) {
    const uint32x4_t new_value = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(source + size_t(i) * 4)));
    difference = vorrq_u32(difference, veorq_u32(new_value, vld1q_u32(target + i)));
    vst1q_u32(target + i, new_value);
  }
  change = vmaxvq_u32(difference) != 0;
#endif
  for (; i < n; ++i) {
    const uint32_t new_value = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
    change |= target[i] != new_value;
    target[i] = new_value;
  }
  return change;
}

inline void RangeVector(uint32_t index, uint32_t* target, const uint8_t* source, uint32_t n, bool& vs,
                           bool& ps, bool& fetch, bool& framing) {
  /*
   * Fast path. Almost every span is a block of VS or PS constants (or fetch constants) that lies entirely
   * inside its class: one CopyWithChange plus marking the class if anything changed is enough, with no
   * search for boundaries (the loop below walks all 12 for every piece). Same if the span touches no class
   * and not the viewport state range: it is only copied. Anything that crosses a boundary still takes the
   * regular loop.
   * Same result as RangeScale: tested on the PC with 3.5 million blocks (every start from 0x1F00 to the
   * end with lengths 1 to 40, plus 2 million random ones), 0 differences. VerifyBlock still compares it
   * with the regular path for the first blocks of each run, like the rest of the vector path.
   */
  if (n != 0) {
    const uint32_t last = index + n - 1;  // WriteRegistersInBlock already checked it stays inside the table
    if (index >= 0x4000) {
      if (last < 0x4400) {
        if (CopyWithChange(target, source, n)) {
          vs = true;
        }
        return;
      }
      if (index >= 0x4400 && last < 0x4800) {
        if (CopyWithChange(target, source, n)) {
          ps = true;
        }
        return;
      }
      if (index >= 0x4800 && last <= kRegFetchLast) {
        if (CopyWithChange(target, source, n)) {
          fetch = true;
        }
        return;
      }
      if (index > kRegFetchLast) {
        CopyWithChange(target, source, n);  // booleans and loops: no class
        return;
      }
    } else if (last < kRegFramingFirst || (index > kRegFramingLast && last < 0x4000)) {
      CopyWithChange(target, source, n);  // outside every class and the viewport state range
      return;
    }
  }
  // Where a class starts or ends (see IsRegisterOfFraming and RangeScale).
  static constexpr uint32_t kCuts[] = {kRegFramingFirst, 0x2083, 0x210F, 0x2115, 0x2204, 0x2207,
                                         0x2302, 0x2303, 0x4000, 0x4400, 0x4800, kRegFetchLast + 1};
  uint32_t i = 0;
  while (i < n) {
    const uint32_t abs = index + i;
    uint32_t fin = n;
    for (const uint32_t cut : kCuts) {
      if (cut > abs && cut - index < fin) {
        fin = cut - index;
      }
    }
    const bool change = CopyWithChange(target + i, source + size_t(i) * 4, fin - i);
    if (change) {
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsRegisterOfFraming(abs)) {
        framing = true;
      }
    }
    i = fin;
  }
}

constexpr uint32_t kSamplesOcclusion = 1000;  // query_occlusion_fake_sample_count
constexpr uint16_t kExtensionMaxima = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxDepthIndirect = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
}

// Big-endian words of a command buffer. In the ring the position wraps around (the size is a power
// of two); in an indirect buffer it does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t fin = 0;

  uint32_t Pending() const { return mask ? ((fin - pos) & mask) : (fin - pos); }
  uint32_t Look() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t input_value = Look();
    Advance(1);
    return input_value;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t input_value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return input_value < reference;
    case 0x2:
      return input_value <= reference;
    case 0x3:
      return input_value == reference;
    case 0x4:
      return input_value != reference;
    case 0x5:
      return input_value >= reference;
    case 0x6:
      return input_value > reference;
    default:
      return true;
  }
}

class SystemGraphicsNative final : public rex::system::IGraphicsSystem {
 public:
  SystemGraphicsNative() : registers_(kNumRegisters, 0), seen_(kNumRegisters) {}
  ~SystemGraphicsNative() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      // Steps C5c-C6: the XenosRecomp SPIR-V needs capabilities that the VkDevice only
      // enables if they are requested before it is created.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] Could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] Could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] Native graphics system (C1): SDK presenter, no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kBaseMmio, kMaskMmio, kSizeMmio, this,
                                                        &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] Could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
    // Step C5a: the NFSSPV library sits next to the executable (it is built from the game
    // data, so it is not distributed). Without it the game still runs, without identification.
    if (shaders_.Load(rex::filesystem::GetExecutableFolder() / "fh1_shaders.nfsp")) {
      ActivateHooks(&shaders_);  // step C5b: shader objects and Draw* records
    }
    last_report_ = Clock::now();
    last_time_report_ = last_report_;
    start_system_ = last_report_;
    thread_vblank_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return LoopVblank(); }));
    thread_vblank_->set_name("GPU native VSync");
    thread_vblank_->Create();
    thread_ring_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return LoopRing(); }));
    thread_ring_->set_name("GPU native ring");
    thread_ring_->Create();
    // From here on FlushState can write the composite marker (ProcessMarker).
    ActivateConsumerMarkers(true);
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] Interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    generation_ring_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] Ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_returned_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    ActivateHooks(nullptr);  // the game hooks stop touching the library
    ActivateConsumerMarkers(false);  // FlushState goes back to the game's own path
    if (active_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (thread_ring_) {
        thread_ring_->Wait(0, 0, 0, nullptr);
        thread_ring_.reset();
      }
      if (thread_vblank_) {
        thread_vblank_->Wait(0, 0, 0, nullptr);
        thread_vblank_.reset();
      }
      Report(true);
    }
    // The framebuffers point to presenter images: destroy them before it.
    DestroyVulkan();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO registers -------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* context_id, uint32_t address) {
    (void)ppc_context;
    return static_cast<SystemGraphicsNative*>(context_id)->ReadRegisterMmio(address);
  }

  static void WriteMmio(void* ppc_context, void* context_id, uint32_t address, uint32_t input_value) {
    (void)ppc_context;
    static_cast<SystemGraphicsNative*>(context_id)->WriteRegisterMmio(address, input_value);
  }

  uint32_t ReadRegisterMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        break;
    }
    NoteFirstTime("read", r, 0);
    return Register(r);
  }

  void WriteRegisterMmio(uint32_t address, uint32_t input_value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      pointer_write_.store(input_value, std::memory_order_release);
      writes_wptr_.fetch_add(1, std::memory_order_relaxed);
      // Taking the lock for an instant avoids losing the wakeup if the ring
      // thread is right between checking and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    } else if (r != kRegD1GrphPrimarySurface) {
      NoteFirstTime("write", r, input_value);
    }
    SaveRegister(r, input_value);  // no side effects through MMIO (graphics_system.cpp:265-281)
  }

  // The gamma ramp the game loads, with the same logic as the SDK's CommandProcessor::WriteRegister
  // (DC_LUT_SEQ_COLOR in red, green and blue; DC_LUT_30_COLOR with blue in bits 0-9). Called before the
  // value is stored. The write mask (blue at bit 0) and the index that only advances after DC_LUT_30_COLOR
  // match the SDK; every change bumps version_ramp_ and the output picks it up on the next Swap.
  void NoteRampGamma(uint32_t index, uint32_t input_value) {
    ++writes_ramp_[index - kRegRampFirst];
    if (index == kRegRampIndex) {
      component_ramp_ = 0;
    } else if (index == kRegRampMask) {
      mask_ramp_ = input_value & 0b111;
    } else if (index == kRegRampSequential) {
      const uint32_t i = registers_[kRegRampIndex] & 0xFF;
      if (mask_ramp_ & (UINT32_C(1) << (2 - component_ramp_))) {
        ramp_gamma_[i][component_ramp_] = uint16_t((input_value >> 6) & 0x3FF);
        ++version_ramp_;
      }
      if (++component_ramp_ >= 3) {
        component_ramp_ = 0;
        registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (index == kRegRamp30) {
      const uint32_t i = registers_[kRegRampIndex] & 0xFF;
      if (mask_ramp_ & 0b001) {
        ramp_gamma_[i][2] = uint16_t(input_value & 0x3FF);
      }
      if (mask_ramp_ & 0b010) {
        ramp_gamma_[i][1] = uint16_t((input_value >> 10) & 0x3FF);
      }
      if (mask_ramp_ & 0b100) {
        ramp_gamma_[i][0] = uint16_t((input_value >> 20) & 0x3FF);
      }
      if (mask_ramp_) {
        ++version_ramp_;
      }
      component_ramp_ = 0;
      registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    } else if (index == kRegRampPwl) {
      // FH1: the game loads the piecewise-linear ramp (DC_LUT_PWL_DATA: 128 segments of base and delta per color,
      // red, green, blue in turn), not the 256-entry table. Same logic as the SDK's CommandProcessor::WriteRegister;
      // the 256 entries the output uses are sampled from it: a 10-bit input picks the segment with its top 7 bits
      // and goes `base + delta * low 3 bits / 8` (16-bit values with the low 6 bits zero).
      const uint32_t i = registers_[kRegRampIndex] & 0x7F;  // bit 7 of the index is ignored for PWL
      if (mask_ramp_ & (UINT32_C(1) << (2 - component_ramp_))) {
        ramp_pwl_[i][component_ramp_] = {uint16_t(input_value & 0xFFC0), uint16_t((input_value >> 16) & 0xFFC0)};
        ramp_pwl_dirty_ = true;
      }
      if (++component_ramp_ >= 3) {
        component_ramp_ = 0;
        registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0x7F)) | ((i + 1) & 0x7F);
        // The table is rebuilt once per complete load (D3D writes all 128 segments in a row), and only if it changes.
        if (i == 0x7F && ramp_pwl_dirty_) {
          ramp_pwl_dirty_ = false;
          bool changed = false;
          for (uint32_t e = 0; e < 256; ++e) {
            const uint32_t x = (e * 0x3FF + 127) / 0xFF;
            for (uint32_t c = 0; c < 3; ++c) {
              const RampPwl& segment = ramp_pwl_[x >> 3][c];
              const uint32_t value_16 = uint32_t(segment.base) + ((uint32_t(segment.delta) * (x & 7)) >> 3);
              const uint16_t value = uint16_t(std::min<uint32_t>(value_16 >> 6, 0x3FF));
              changed |= ramp_gamma_[e][c] != value;
              ramp_gamma_[e][c] = value;
            }
          }
          if (changed) {
            ++version_ramp_;
          }
        }
      }
    }
  }

  // EVENT_WRITE_ZPD: the sample counters of an occlusion query.
  // How the game's D3D does it:
  //   - Issue(BEGIN) (sub_8258F810) marks the structure at base + 0 with 0xFFFFFEED and requests the
  //     counters at base + 32.
  //   - Issue(END) requests them at base + 0 (sub_8258EA28).
  //   - GetData (sub_8258F998) waits for base + 0 to lose the marks and returns
  //     ZPass(base + 0) - ZPass(base + 32).
  // Here (fh1_native_occlusion = 1): at the start, base + 32 is zeroed, and the draws up to the end are
  // counted on the GPU with host queries. At the end, base + 0 gets the last measured count of that
  // structure: one from an earlier frame, because host queries are read when their work finishes. Anything
  // that does not fit keeps the old behavior (a faked count).
  void QueryOcclusion(uint32_t address) {
    using Counts = xenos::xe_gpu_depth_sample_counts;
    constexpr uint32_t kMark = 0xEDFEFFFF;  // 0xFFFFFEED written big-endian by D3D, read as little-endian
    const auto marked = [](const Counts* c) {
      return uint32_t(c->ZPass_A) == kMark || uint32_t(c->ZPass_B) == kMark || uint32_t(c->ZFail_A) == kMark ||
             uint32_t(c->ZFail_B) == kMark;
    };
    auto* counts = memory_->TranslatePhysical<Counts*>(address);
    const bool marks_here = marked(counts);
    const bool marks_before = address >= 32 && marked(memory_->TranslatePhysical<Counts*>(address - 32));
    int32_t mode = REXCVAR_GET(fh1_native_occlusion);
    // Test cvar fh1_native_occlusion_toggle_s: odd intervals use the old faked count.
    if (const int32_t toggle_s = REXCVAR_GET(fh1_native_occlusion_toggle_s); mode == 1 && toggle_s > 0) {
      const auto now = Clock::now();
      if (!occlusion_test_started_) {
        occlusion_test_started_ = true;
        occlusion_test_start_ = now;
      }
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(now - occlusion_test_start_).count();
      const bool faked = (seconds / toggle_s) % 2 == 1;
      if (faked != occlusion_test_faked_) {
        occlusion_test_faked_ = faked;
        if (faked && occlusion_base_ != 0 && targets_) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(occlusion_base_, discarded);
          occlusion_base_ = 0;
        }
        REXLOG_INFO("[native] occlusion test: {}", faked ? "faked (1000)" : "measured");
      }
      if (faked) {
        mode = 0;
      }
    }
    const char* que = nullptr;
    uint32_t written = UINT32_MAX;
    uint64_t measurements = 0;
    bool measurement = false;
    uint32_t scale = 0;
    if (mode == 1 && targets_) {
      if (occlusion_base_ != 0 && address == occlusion_base_) {
        measurement = targets_->FinishOcclusion(address, measurements);
        occlusion_base_ = 0;
        scale = ScaleOcclusion();
        written = measurement ? uint32_t(std::min<uint64_t>(measurements * scale, 0xFFFFFF)) : kSamplesOcclusion;
        std::memset(counts, 0, sizeof(*counts));
        counts->ZPass_A = written;
        counts->Total_A = written;
        que = measurement ? "final" : "final without measurement";
        if (measurement) {
          ++occlusion_counters_[1];
          occlusion_counters_[5] += written;
          occlusion_counters_[6] = std::max<uint64_t>(occlusion_counters_[6], written);
        } else {
          ++occlusion_counters_[2];
        }
        // By render target: the 320 one (the sun) or a scene one (640 or more, scaled by the AA mode).
        const size_t group = scale > 1 ? 1 : 0;
        ++occlusion_by_target_[group];
        if (measurement) {
          occlusion_max_by_target_[group] = std::max(occlusion_max_by_target_[group], written);
        }
        if (group == 1 && warnings_occlusion_scene_ < 12) {
          ++warnings_occlusion_scene_;
          const uint32_t info = Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          REXLOG_INFO("[native] C2 occlusion on a scene target {}: structure {:08X}, pitch {}, MSAA {}, scale {}, "
                      "host samples {} ({}), written {}",
                      warnings_occlusion_scene_, address, info & 0x3FFF, (info >> 16) & 0x3, scale, measurements,
                      measurement ? "measured" : "not measured", written);
        }
      } else if (address >= 32 && (marks_before || bases_occlusion_.count(address - 32))) {
        const uint32_t base = address - 32;
        if (occlusion_base_ != 0) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(occlusion_base_, discarded);  // the previous one never reached its Issue(END)
          ++occlusion_counters_[3];
        }
        if (bases_occlusion_.size() < 64) {
          bases_occlusion_.insert(base);
        }
        targets_->BeginOcclusion(base);
        occlusion_base_ = base;
        std::memset(counts, 0, sizeof(*counts));
        que = "principio";
        ++occlusion_counters_[0];
      }
    }
    if (!que) {
      // The previous behavior (also the emulated path's): a faked count at the end.
      std::memset(counts, 0, sizeof(*counts));
      if (marks_here) {
        written = mode == 2 ? 0 : kSamplesOcclusion;
        counts->ZPass_A = written;
        counts->Total_A = written;
        ++occlusion_counters_[4];
      }
      que = mode == 1 ? "no partner" : "faked";
    }
    if (warnings_occlusion_ < 8) {  // was 48; these lines are long and the ring thread writes them
      ++warnings_occlusion_;
      const uint8_t* base_virtual = memory_->virtual_membase();
      const auto read_be = [base_virtual](uint32_t d) {
        uint32_t v = 0;
        std::memcpy(&v, base_virtual + d, sizeof(v));
        return __builtin_bswap32(v);
      };
      // FH1: 0x82A2D1AC / 0x82A2CEE4 are Most Wanted's AA settings; in FH1 they hold other data and following
      // the pointer crashed on the festival load (read of 2B0B0000). Not read here.
      (void)read_be;
      const uint32_t renderer = 0;
      const uint32_t info = Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
      REXLOG_INFO("[native] C2 occlusion {}: ZPD at {:08X} ({}), marks here {} and 32 before {}; host samples {} "
                  "({}), scale {}, written {}; bins {:016X}/{:016X}; RB_SURFACE_INFO {:08X} (pitch {}, MSAA {}), "
                  "RB_DEPTH_INFO {:08X}; game AA mode {}, quality {}",
                  warnings_occlusion_, address, que, marks_here, marks_before, measurements, measurement ? "measured" : "not "
                                                                                                                       "measured",
                  scale, written == UINT32_MAX ? -1 : int64_t(written), bin_select_, bin_mask_, info, info & 0x3FFF,
                  (info >> 16) & 0x3, Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  renderer ? int64_t(renderer) : -1, 0u);
    }
  }

  // Samples per pixel the Xbox 360 would count in the query's render target: the MSAA samples the target
  // requests and, for scene targets (pitch of 640 or more), those of the game's AA mode before it is
  // removed (fh1_render_without_tile). The sun flare is queried on a 320 target without MSAA: 1.
  uint32_t ScaleOcclusion() const {
    const int32_t fixed = REXCVAR_GET(fh1_native_occlusion_scale);
    if (fixed > 0) {
      return uint32_t(fixed);
    }
    const uint32_t info = Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t samples_target = UINT32_C(1) << std::min<uint32_t>((info >> 16) & 0x3, 2);
    const uint32_t samples_scene =
        (info & 0x3FFF) >= 640 ? fh1::render_targets::SamplesOriginalModeCurrent(memory_->virtual_membase()) : 1;
    return std::max(samples_target, samples_scene);
  }

  void NoteFirstTime(const char* que, uint32_t index, uint32_t input_value) {
    if (index >= kNumRegisters || seen_[index].exchange(1, std::memory_order_acq_rel)) {
      return;
    }
    REXLOG_INFO("[native] MMIO register {:04X}: first {} ({:08X})", index, que, input_value);
  }

  // The registers are touched by the game thread (MMIO) and by the ring thread. As in the
  // emulated path, races on integer values are harmless.
  void SaveRegister(uint32_t index, uint32_t input_value) {
    if (index < kNumRegisters) {
      /*
       * This entry point is used by MMIO from the game thread and by VIZ_QUERY, and it does not go
       * through WriteRegister. Neither touches the fetch constants or the viewport state today (the
       * 360's D3D writes GPU registers through the ring), but if one ever did and the generation were
       * not bumped, the translator would keep the stale viewport or textures. It is called very rarely
       * compared with the ring, so the safeguard costs nothing.
       */
      if (registers_[index] != input_value) {
        if (index >= kRegFetchFirst && index <= kRegFetchLast) {
          ++generation_fetch_;
        } else if (IsRegisterOfFraming(index)) {
          ++generation_framing_;
        }
      }
      registers_[index] = input_value;
    }
  }

  // A write from the ring, with the side effects it has when the GPU processes it
  // (CommandProcessor::WriteRegister). SCRATCH_REG values are copied to memory: the
  // D3D writes one and then waits with WAIT_REG_MEM until it shows up at SCRATCH_ADDR
  // (measured on the PC: without this the game stops within the first second).
  // COHER_STATUS_HOST stays marked as pending until the next poll.
  void WriteRegister(uint32_t index, uint32_t input_value) {
    if (index >= kNumRegisters) {
      return;
    }
    /*
     * Only when the value really changes.
     *
     * The Xbox 360 D3D dumps the constant block before every draw, and many constants are identical
     * between draws of the same material. Bumping the generation on every write made 60 % of the draws
     * re-upload their 7.7 KB of constants to the buffer (12.54 MB per frame, measured) into memory with
     * no CPU cache (2654 MB/s), on the thread that already uses 96 % of a core.
     *
     * Comparing before writing costs one read of a line that is about to be written anyway, so it is
     * already in cache. And the result is bit-for-bit identical: if the value does not change, the
     * shader reads the same thing.
     */
    if (index >= 0x4000) {
      if (index < 0x4800) {  // VS and PS constants (native draws)
        if (registers_[index] != input_value) {
          ++(index < 0x4400 ? generation_constants_vs_ : generation_constants_ps_);
        } else {
          ++constants_without_change_;
        }
        ++constants_written_;
      } else if (index <= kRegFetchLast) {  // fetch constants: textures and vertices
        if (registers_[index] != input_value) {
          ++generation_fetch_;
        } else {
          ++fetch_without_change_;
        }
        ++fetch_written_;
      }
    } else if (IsRegisterOfFraming(index)) {
      if (registers_[index] != input_value) {
        ++generation_framing_;
      } else {
        ++framing_without_change_;
      }
      ++framing_written_;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      input_value |= UINT32_C(0x80000000);
    }
    if (index >= kRegRampFirst && index <= kRegRampLast) {
      NoteRampGamma(index, input_value);
    }
    registers_[index] = input_value;
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), input_value);
        AddOnlyRing(writes_memory_);
      }
    }
  }

  // A block of consecutive registers without side effects, in one go. Returns false (and consumes nothing)
  // if it touches COHER_STATUS_HOST or the SCRATCH_REG registers, if it goes past the table or if it is
  // disabled.
  bool WriteRegistersInBlock(uint32_t index, uint32_t count, Reader& data) {
    if (registers_in_block_ < 0) {
      registers_in_block_ = REXCVAR_GET(fh1_native_registers_in_block) ? 1 : 0;
      REXLOG_INFO("[native] registers in block (fh1_native_registers_in_block) = {}",
                  registers_in_block_ ? "SI" : "no");
    }
    if (!registers_in_block_ || count < 2 || index >= kNumRegisters || count > kNumRegisters - index) {
      return false;
    }
    const uint32_t fin = index + count;
    const auto due = [index, fin](uint32_t since, uint32_t until) { return index < until && since < fin; };
    if (due(rex::graphics::XE_GPU_REG_COHER_STATUS_HOST, rex::graphics::XE_GPU_REG_COHER_STATUS_HOST + 1) ||
        due(rex::graphics::XE_GPU_REG_SCRATCH_REG0, rex::graphics::XE_GPU_REG_SCRATCH_REG7 + 1) ||
        due(kRegRampFirst, kRegRampLast + 1)) {
      return false;
    }
    // Same as above, but a block does not know whether it changed until it has been copied, so
    // the generation is bumped afterwards, and only if some word in the range differs.
    const bool looks_vs = due(0x4000, 0x4400);
    const bool looks_ps = due(0x4400, 0x4800);
    // The same for the fetch constants and the viewport state. The viewport state is four small
    // separate pieces, so it is enough to check whether the block crosses the range that holds them:
    // inside the loop each word is already classified, and a generation bumped too often only makes
    // the translator do extra work, it never draws wrong.
    const bool looks_fetch = due(kRegFetchFirst, kRegFetchLast + 1);
    const bool looks_framing = due(kRegFramingFirst, kRegFramingLast + 1);
    const bool looks_values = looks_vs || looks_ps || looks_fetch || looks_framing;
    bool change_vs = false;
    bool change_ps = false;
    bool change_fetch = false;
    bool change_framing = false;
    // The vector path, with a self-checking guard (see VerifyBlock).
    if (registers_vector_ < 0) {
      registers_vector_ = REXCVAR_GET(fh1_native_vector_registers) ? 1 : 0;
      REXLOG_INFO("[native] vector registers (fh1_native_vector_registers) = {}",
                  registers_vector_ ? "YES, checking the first blocks against the usual path"
                                         : "no");
    }
    const bool vectorized = registers_vector_ > 0;
    const bool verify = vectorized && blocks_verified_ < kBlocksAVerify;
    Reader data_verification;
    if (verify) {
      data_verification = data;
      copy_verification_.assign(registers_.begin() + index, registers_.begin() + index + count);
    }
    uint32_t index_current = index;
    uint32_t* target = registers_.data() + index;
    uint32_t remain = count;
    while (remain) {
      // Contiguous span: up to the end of the ring (in a linear buffer, everything left).
      const uint32_t range = data.mask ? std::min(remain, data.mask + 1 - data.pos) : remain;
      const uint8_t* source = data.base + size_t(data.pos) * 4;
      if (looks_values) {
        if (vectorized) {
          RangeVector(index_current, target, source, range, change_vs, change_ps, change_fetch,
                         change_framing);
        } else {
          RangeScale(index_current, target, source, range, change_vs, change_ps, change_fetch,
                       change_framing);
        }
        if (looks_vs || looks_ps) {
          constants_written_ += range;
        }
        if (looks_fetch) {
          fetch_written_ += range;
        }
        if (looks_framing) {
          framing_written_ += range;
        }
      } else if (vectorized) {
        CopyWithChange(target, source, range);
      } else {
        for (uint32_t i = 0; i < range; ++i) {
          target[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
      }
      index_current += range;
      target += range;
      data.Advance(range);
      remain -= range;
    }
    if (verify) {
      VerifyBlock(index, count, data_verification, looks_values, change_vs, change_ps, change_fetch,
                      change_framing);
    }
    if (change_vs) {
      ++generation_constants_vs_;
    }
    if (change_ps) {
      ++generation_constants_ps_;
    }
    if (change_fetch) {
      ++generation_fetch_;
    }
    if (change_framing) {
      ++generation_framing_;
    }
    if ((looks_vs && !change_vs) || (looks_ps && !change_ps)) {
      ++blocks_constants_without_change_;
    }
    if (looks_vs || looks_ps) {
      ++blocks_constants_;
    }
    return true;
  }

  /*
   * The vector path guard. For the first kBlocksAVerify blocks, the block is replayed through the
   * regular path (RangeScale) on a copy of the previous registers, and the results are compared register
   * by register and class by class. If anything differs, the regular path's result is written, the log
   * reports it and the vector path is turned off for the rest of the run.
   */
  void VerifyBlock(uint32_t index, uint32_t count, Reader data, bool looks_values, bool& vs, bool& ps,
                       bool& fetch, bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_framing = false;
    uint32_t done = 0;
    while (done < count) {
      const uint32_t remain = count - done;
      const uint32_t range = data.mask ? std::min(remain, data.mask + 1 - data.pos) : remain;
      RangeScale(index + done, copy_verification_.data() + done, data.base + size_t(data.pos) * 4, range,
                   ref_vs, ref_ps, ref_fetch, ref_framing);
      data.Advance(range);
      done += range;
    }
    ++blocks_verified_;
    const bool equal_values =
        std::equal(copy_verification_.begin(), copy_verification_.end(), registers_.begin() + index);
    const bool equal_classes =
        !looks_values || (ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_framing == framing);
    if (!equal_values || !equal_classes) {
      REXLOG_ERROR("[native] vector registers: block {:04X}+{} does NOT match the usual path (values {}, classes "
                   "VS {}/{} PS {}/{} fetch {}/{} framing {}/{}). The vector path is turned OFF.",
                   index, count, equal_values ? "equal" : "DIFFERENT", vs, ref_vs, ps, ref_ps, fetch,
                   ref_fetch, framing, ref_framing);
      std::copy(copy_verification_.begin(), copy_verification_.end(), registers_.begin() + index);
      if (looks_values) {
        vs = ref_vs;
        ps = ref_ps;
        fetch = ref_fetch;
        framing = ref_framing;
      }
      registers_vector_ = 0;
      return;
    }
    if (blocks_verified_ == kBlocksAVerify) {
      REXLOG_INFO("[native] vector registers: {} blocks checked against the usual path, all equal. Staying with "
                  "the vector path.",
                  blocks_verified_);
    }
  }

  /*
   * Phase 2 of the Direct3D-level renderer: the FlushState composite marker.
   * See kMarkerMagic in fh1_native_hooks.h and the end of fh1_d3d_registers_native.cpp. FlushState
   * writes one NOP with all its register spans instead of ~7 type-0 packets and ~6 padding words per draw.
   *   - Apply mode: the spans are written here with the same effect as a type-0 block (values, generations
   *     and counters of WriteRegistersInBlock).
   *   - Check mode: the type-0 packets of the same dump come right before it and have already been applied.
   *     It only checks that the registers hold what the marker says, and the result goes back to the game
   *     thread.
   * A NOP without the magic belongs to the game itself and is ignored, as before.
   */
  void ProcessMarker(Reader& data, uint32_t words) {
    if (words < 2 || (data.Look() & 0xFFFFFF00u) != kMarkerMagic) {
      return;
    }
    const uint32_t header = data.Read();
    const uint32_t mode = header & 0x0Fu;               // registers: 0 no ranges, 1 apply, 2 check
    const uint32_t mode_draw = (header >> 4) & 0x0Fu;  // phase 2b: 0 none, kDrawApply, kDrawCheck
    const uint32_t sequence = data.Read();
    uint32_t n = words - 2;  // draw record, span headers and values
    // The spans, contiguous in memory: D3D never splits a packet; if it wraps around the ring, it is copied
    // aside.
    const uint8_t* p = data.base + size_t(data.pos) * 4;
    if (data.mask && data.pos + n > data.mask + 1) {
      marker_flat_.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&marker_flat_[i], data.base + size_t((data.pos + i) & data.mask) * 4, 4);
      }
      p = reinterpret_cast<const uint8_t*>(marker_flat_.data());
    }
    // Phase 2b: the Draw* record of this FlushState, ahead of the spans. It is stored at the end, once the
    // whole marker has been validated.
    RegisterDraw draw;
    if (mode_draw != 0) {
      if ((mode_draw != kDrawApply && mode_draw != kDrawCheck) || n < kWordsDraw ||
          !ReadDrawOfMarker(p, draw)) {
        MarkerBad(mode, sequence, header);
        return;
      }
      p += size_t(kWordsDraw) * 4;
      n -= kWordsDraw;
    }
    // Validate the whole structure first: a broken marker writes nothing.
    uint32_t ranges = 0;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t count = t >> 16;
      if (count >= n - i || !RangeOfDump(t & 0xFFFFu, count)) {
        MarkerBad(mode, sequence, t);
        return;
      }
      i += 1 + count;
      ++ranges;
    }
    if (mode > kMarkerCheck || (mode == 0 && (n != 0 || mode_draw == 0))) {
      MarkerBad(mode, sequence, header);
      return;
    }
    // For the DRAW_INDX packets that follow (MatchDraw): until one accepts it or the next Draw* record
    // arrives.
    if (mode_draw != 0) {
      draw_marker_ = draw;
      draw_marker_mode_ = mode_draw;
    }
    if (mode == kMarkerCheck) {
      CheckMarker(p, n, sequence);
      return;
    }
    if (mode == 0) {
      return;  // only the draw record
    }
    // The fast path is decided once per marker: the block path and the vector path enabled and already
    // verified.
    const bool fast =
        registers_in_block_ > 0 && registers_vector_ > 0 && blocks_verified_ >= kBlocksAVerify;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t count = t >> 16;
      if (fast && count >= 2 && marker_fast_ > 0) {
        ApplyRangeFast(index, count, p + size_t(i + 1) * 4);
      } else {
        // The regular path, same as a type-0 packet with these registers.
        ++marker_ranges_slow_;
        Reader linear;
        linear.base = p;
        linear.pos = i + 1;
        linear.fin = n;
        if (count < 2 || !WriteRegistersInBlock(index, count, linear)) {
          for (uint32_t k = 0; k < count; ++k) {
            WriteRegister(index + k, rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4));
          }
        }
      }
      i += 1 + count;
    }
    ++markers_applied_;
    marker_ranges_ += ranges;
    marker_words_ += n - ranges;
  }

  void MarkerBad(uint32_t mode, uint32_t sequence, uint32_t word) {
    ++markers_bad_;
    if (warnings_marker_ < 8) {
      ++warnings_marker_;
      REXLOG_ERROR("[native] D3D marker {} MALFORMED (mode {}, word {:08X}): not applied, and the game thread "
                   "turns off the marker path",
                   sequence, mode, word);
    }
    NoteCheckMarker(false, sequence, 0, 0, word);
  }

  void CheckMarker(const uint8_t* p, uint32_t n, uint32_t sequence) {
    ++markers_checked_;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t count = t >> 16;
      for (uint32_t k = 0; k < count; ++k) {
        const uint32_t input_value = rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4);
        if (registers_[index + k] != input_value) {
          ++markers_different_;
          if (warnings_marker_ < 8) {
            ++warnings_marker_;
            REXLOG_ERROR("[native] D3D marker {}: register {:04X} is {:08X} after its packets and {:08X} in the "
                         "marker. The game thread turns off the marker path",
                         sequence, index + k, registers_[index + k], input_value);
          }
          NoteCheckMarker(false, sequence, index + k, registers_[index + k], input_value);
          return;
        }
      }
      i += 1 + count;
    }
    NoteCheckMarker(true, sequence, 0, 0, 0);
  }

  // A span of 2 or more registers from a single dump group (RangeOfDump). Same effect as
  // WriteRegistersInBlock with those registers (values, generations and counters), without its checks
  // and without searching for boundaries: in each group, the classes lie in a single known piece.
  void ApplyRangeFast(uint32_t index, uint32_t count, const uint8_t* source) {
    uint32_t* target = registers_.data() + index;
    const bool verify = ranges_fast_verified_ < kRangesFastAVerify;
    if (verify) {
      copy_marker_.assign(target, target + count);
    }
    bool vs = false;
    bool ps = false;
    bool fetch = false;
    bool framing = false;
    const uint32_t fin = index + count;
    if (index >= 0x4000) {
      // VS, PS, fetch or boolean constants: the whole span belongs to a single class.
      const bool change = CopyWithChange(target, source, count);
      if (index < 0x4400) {
        vs = change;
      } else if (index < 0x4800) {
        ps = change;
      } else if (index <= kRegFetchLast) {
        fetch = change;
      }
    } else {
      // 0x2000-0x23FF: the viewport state of each group is a single piece (0x210F-0x2114,
      // 0x2204-0x2206 and 0x2302).
      uint32_t e0 = fin;
      uint32_t e1 = fin;
      if (index >= 0x2100 && index < 0x2180) {
        e0 = 0x210F;
        e1 = 0x2115;
      } else if (index >= 0x2200 && index < 0x2280) {
        e0 = 0x2204;
        e1 = 0x2207;
      } else if (index >= 0x2300 && index < 0x2380) {
        e0 = 0x2302;
        e1 = 0x2303;
      }
      e0 = std::min(std::max(e0, index), fin);
      e1 = std::min(std::max(e1, e0), fin);
      CopyWithChange(target, source, e0 - index);
      framing = CopyWithChange(target + (e0 - index), source + size_t(e0 - index) * 4, e1 - e0);
      CopyWithChange(target + (e1 - index), source + size_t(e1 - index) * 4, fin - e1);
    }
    if (verify) {
      VerifyRangeMarker(index, count, source, vs, ps, fetch, framing);
    }
    // The same counters and generations as WriteRegistersInBlock for a block with these registers.
    if (index >= 0x4000 && index < 0x4800) {
      constants_written_ += count;
      ++blocks_constants_;
      if (!(index < 0x4400 ? vs : ps)) {
        ++blocks_constants_without_change_;
      }
    } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
      fetch_written_ += count;
    } else if (index <= kRegFramingLast && fin > kRegFramingFirst) {
      framing_written_ += count;
    }
    if (vs) {
      ++generation_constants_vs_;
    }
    if (ps) {
      ++generation_constants_ps_;
    }
    if (fetch) {
      ++generation_fetch_;
    }
    if (framing) {
      ++generation_framing_;
    }
  }

  // The fast path guard: the first kRangesFastAVerify spans are replayed through RangeScale (the
  // regular path, word by word) on a copy of the previous registers, and values and classes are compared.
  // If anything differs, the regular path's result is kept and the fast path is turned off for the rest
  // of the run.
  void VerifyRangeMarker(uint32_t index, uint32_t count, const uint8_t* source, bool& vs, bool& ps, bool& fetch,
                              bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_framing = false;
    RangeScale(index, copy_marker_.data(), source, count, ref_vs, ref_ps, ref_fetch, ref_framing);
    ++ranges_fast_verified_;
    const bool equal = std::equal(copy_marker_.begin(), copy_marker_.end(), registers_.begin() + index) &&
                         ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_framing == framing;
    if (!equal) {
      REXLOG_ERROR("[native] D3D marker: the fast path of range {:04X}+{} does NOT match the usual one (classes VS "
                   "{}/{} PS {}/{} fetch {}/{} framing {}/{}). The fast path is turned OFF.",
                   index, count, vs, ref_vs, ps, ref_ps, fetch, ref_fetch, framing, ref_framing);
      std::copy(copy_marker_.begin(), copy_marker_.end(), registers_.begin() + index);
      vs = ref_vs;
      ps = ref_ps;
      fetch = ref_fetch;
      framing = ref_framing;
      marker_fast_ = 0;
      return;
    }
    if (ranges_fast_verified_ == kRangesFastAVerify) {
      REXLOG_INFO("[native] D3D marker: {} ranges of the fast path checked against the usual one, all equal. "
                  "Staying with the fast path.",
                  ranges_fast_verified_);
    }
  }

  uint32_t Register(uint32_t index) const { return index < kNumRegisters ? registers_[index] : 0; }

  // --- GPU memory (the two low bits of the address are the byte order) ----

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t input_value;
    std::memcpy(&input_value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(input_value));
    return xenos::GpuSwap(input_value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t input_value) {
    const uint32_t sorted = xenos::GpuSwap(input_value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &sorted, sizeof(sorted));
    AddOnlyRing(writes_memory_);
  }

  // --- Threads ------------------------------------------------------------------

  void Interrupt(uint32_t source_2, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread_value = rex::system::XThread::GetCurrentThread();
    if (!thread_value) {
      return;
    }
    thread_value->SetActiveCpu(uint8_t(cpu));
    uint64_t arguments[] = {source_2, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread_value->thread_state(), callback, arguments, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);  // NOTE: also called from LoopVblank (another thread)
    NotifyProgressRing();  // some D3D waits also check the interrupt counter
  }

  int LoopVblank() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      // After a long pause (debugger, loading) hundreds of vblanks are not replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int LoopRing() {
    /*
     * The ring thread drops to 0x2D, because of the presenter.
     *
     * Measured: our presentation thread took 54.26 ms per iteration, 53.38 of them in "record"
     * (twenty Vulkan calls that should cost less than a millisecond). It was not work: the thread
     * spent 998 ms of wall-clock time per second but only 300 ms of CPU. The other 700 ms it was
     * ready with no core, preempted by this thread, which runs at 81 % and shared its priority
     * (0x2C). On Horizon, priorities below 0x3B are not time-sliced, so at equal priority the
     * presenter only ran when the ring thread yielded... and the ring thread had just lost exactly
     * what used to make it yield.
     *
     * Result: only 13.37 of the 26.25 frames per second reached the window.
     *
     * This thread is lowered instead of raising the presenter, to stay out of the audio priority
     * (0x2B), which was hard to get right and is settled. The presenter needs ~2 ms about 27 times
     * per second (5 % of a core) and sleeps on a condition variable as soon as it is done, so what
     * it takes from this thread is negligible.
     */
#if REX_PLATFORM_SWITCH
    {
      const int32_t priority = REXCVAR_GET(fh1_native_ring_priority);
      if (priority >= 0x1C && priority <= 0x3B) {
        RexSwitchSetCurrentThreadPriority(int(priority));
        REXLOG_INFO("[native] ring thread at priority {:#x} (the presenter runs at 0x2C and must be able to "
                    "preempt it)", uint32_t(priority));
      }
      /*
       * And which core it starts on. See fh1_native_ring_core.
       *
       * The core is logged before and after. Without the "before" the line cannot be interpreted: if
       * the process's default core were already 1, asking for core 1 would separate nothing at all and
       * the log would look like a success.
       */
      const int core_before = RexSwitchCurrentCore();
      const int32_t core = REXCVAR_GET(fh1_native_ring_core);
      const bool exclusive = core >= 0 && REXCVAR_GET(fh1_native_ring_core_exclusive);
      if (core >= -1 && core <= 2) {
        const bool set_2 = exclusive ? RexSwitchPinCurrentThreadToCore(int(core))
                                      : RexSwitchSetCurrentThreadCore(int(core));
        REXLOG_INFO("[native] ring thread: core {} {} ({}); it was on {} and now runs on {}",
                    core, exclusive ? "EXCLUSIVE (reduced mask)" : "preferred",
                    set_2 ? "accepted" : "REJECTED by the kernel", core_before,
                    RexSwitchCurrentCore());
      }
      core_ring_ = RexSwitchCurrentCore();
    }
#endif
    uint32_t read_2 = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        const auto before_without_work = Clock::now();  // for the "[hitch]" line
        const bool with_signal = ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 pointer_write_.load(std::memory_order_acquire) != read_2 ||
                 generation_ring_.load(std::memory_order_acquire) != generation;
        });
        fh1::waits::Add(fh1::waits::kRingWithoutWork, NanosecondsSince(before_without_work));
        // Wakeup counters (only this thread uses them; Report runs on it).
        ++laps_ring_;
        if (!with_signal) {
          ++waits_ring_exhausted_;
        }
#if REX_PLATFORM_SWITCH
        /*
         * Counts how many times the kernel has moved this thread to another core. It is one
         * register read per loop iteration (not a real syscall), and it is the only way to know
         * whether the preferred core does anything: if -1 and 1 give the same count, it has no
         * effect here.
         */
        {
          const int now = RexSwitchCurrentCore();
          if (now != core_ring_) {
            core_ring_ = now;
            ++migrations_ring_;
          }
        }
#endif
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report(false);
      const uint32_t gen = generation_ring_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read_2 = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t write_pos = pointer_write_.load(std::memory_order_acquire) & (words - 1);
      if (write_pos == read_2) {
        continue;
      }
      ++laps_ring_with_data_;
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read_2;
      reader.fin = write_pos;
      const auto start_ring = Clock::now();
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          // Partial packet: the rest will arrive with the next CP_RB_WPTR.
          break;
        }
      }
      {
        const uint64_t ns_ring = NanosecondsSince(start_ring);
        time_ring_ns_ += ns_ring;
        AddOnlyRing(fh1::waits::g_ns_ring_working, ns_ring);  // without fetch_add
      }
      read_2 = reader.pos;
      // Deferred vertex copies must be done before telling the game that the GPU has read up to here:
      // after that it may reuse those buffers (fh1_native_uploads_thread).
      if (targets_) {
        targets_->WaitUploads();
      }
      const uint32_t to_return = read_returned_.load(std::memory_order_acquire);
      if (to_return) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(to_return), read_2);
      }
      // After returning it: the game's D3D waits sleep until this notification (fh1_d3d_wait.cpp).
      NotifyProgressRing();
    }
    return 0;
  }

  // --- PM4 packets ------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Look();
    uint32_t load = 0;
    switch (packet >> 30) {
      case 0:
        load = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        load = 2;
        break;
      case 2:
        load = 0;
        break;
      default:
        load = ((packet >> 16) & 0x3FFF) + 1;
        break;
    }
    if (reader.Pending() < load + 1) {
      return false;
    }
    reader.Advance(1);
    AddOnlyRing(packets_);
    Reader data = reader;
    reader.Advance(load);
    if (!packet) {
      return true;
    }
    // Timer on 1 of every kStopwatchPacketsEvery packets, with the time scaled up: there are ~14,000 per
    // frame, and two clock reads per packet cost more than writing the register. WAIT_REG_MEM, long and
    // infrequent, is always timed.
    const bool time = (++stopwatch_packets_ & (kStopwatchPacketsEvery - 1)) == 0;
    const auto start = time ? Clock::now() : Clock::time_point{};
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool un_register = (packet >> 15) & 0x1;
        if (un_register || !WriteRegistersInBlock(index, load, data)) {
          for (uint32_t i = 0; i < load; ++i) {
            WriteRegister(un_register ? index : index + i, data.Read());
          }
          words_loose_ += load;  // Measurement only
        } else {
          words_block_ += load;
        }
        if (time) {
          time_registers_ns_ += NanosecondsSince(start) * kStopwatchPacketsEvery;
        }
        ++count_registers_;
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        if (time) {
          time_registers_ns_ += NanosecondsSince(start) * kStopwatchPacketsEvery;
        }
        ++count_registers_;
        break;
      }
      case 2:
        break;
      default: {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const bool wait = opcode == xenos::PM4_WAIT_REG_MEM;
        const auto start_op = wait && !time ? Clock::now() : start;
        PacketType3(data, packet, load, depth);
        // An indirect buffer is timed through its packets, so they are not counted twice.
        if (opcode != xenos::PM4_INDIRECT_BUFFER && opcode != xenos::PM4_INDIRECT_BUFFER_PFD) {
          if (wait) {
            time_opcode_ns_[opcode] += NanosecondsSince(start_op);
          } else if (time) {
            time_opcode_ns_[opcode] += NanosecondsSince(start) * kStopwatchPacketsEvery;
          }
          ++count_opcode_[opcode];
        }
        break;
      }
    }
    return true;
  }

  void PacketType3(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    AddOnlyRing(opcodes_[opcode]);
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        NoteFenceCopies();  // Measurement only
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {
        // VdSwap writes "SWAP", the physical address of the frontbuffer and its size.
        if (words >= 4) {
          data.Read();
          swap_frontbuffer_.store(data.Read(), std::memory_order_relaxed);
          swap_width_.store(data.Read(), std::memory_order_relaxed);
          swap_height_.store(data.Read(), std::memory_order_relaxed);
        }
        TraceSwap();
        Present();
        NoteGameByAhead();  // Measurement only
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxDepthIndirect) {
          BufferIndirect(address, length, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto start_regmem = Clock::now();  // for the "[hitch]" line
        const auto limit = start_regmem + kWaitRegMemMax;
        uint32_t laps = 0;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            SaveRegister(poll, 0);  // MakeCoherent: there is no shared memory to synchronize
          }
          const uint32_t input_value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, input_value & mask, reference)) {
            break;
          }
          // The real GPU waits; without a GPU some conditions will never be
          // met. The wait is cut short so the ring does not hang.
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_exhausted_.fetch_add(1, std::memory_order_relaxed) == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          // NFSC: a few quick yields first (the value is often written within microseconds), then short accurate
          // sleeps (fh1_short_wait.cpp). A plain 1 ms sleep noticed the value late several times per frame.
          const int32_t pause_us = REXCVAR_GET(fh1_native_wait_regmem_us);
          if (laps < 8) {
            std::this_thread::yield();
          } else {
            fh1::WaitShort(uint32_t(pause_us > 50 ? pause_us : 50));
          }
          ++laps;
        }
        /*
         * The guest has just waited for the GPU. It is the only point where it may legally rewrite
         * a vertex range it already referenced in this frame, so from here on upload deduplication
         * cannot reuse anything recorded so far. See fh1_native_vertex_dedupe.h. Relaxed is
         * enough: the same thread reads it.
         */
        g_synchronizations_ring.fetch_add(1, std::memory_order_relaxed);
        fh1::waits::Add(fh1::waits::kRingRegMem, NanosecondsSince(start_regmem));
        const uint64_t key_wait = (uint64_t(info & 0x10) << 32) | poll;
        if (waits_.size() < 64 || waits_.count(key_wait)) {
          Wait& wait = waits_[key_wait];
          ++wait.times;
          wait.laps += laps;
          wait.reference = reference;
          wait.mask = mask;
          wait.info = info;
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read();
        const uint32_t y = data.Read();
        const uint32_t o = data.Read();
        uint32_t input_value = Register(info & 0x1FFF);
        input_value &= ((info >> 31) & 0x1) ? Register(y & 0x1FFF) : y;
        input_value |= ((info >> 30) & 0x1) ? Register(o & 0x1FFF) : o;
        WriteRegister(info & 0x1FFF, input_value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg_entry = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(reg_entry));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        NoteFenceCopies();  // Measurement only
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t datum = data.Read();
        const uint32_t input_value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, input_value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, datum);
          } else {
            WriteRegister(target, datum);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        NoteFenceCopies();  // Measurement only
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t input_value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1)
                                       ? counter_.load(std::memory_order_relaxed)
                                       : input_value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum is faked.
        const uint16_t extension[] = {0, kExtensionMaxima, 0, kExtensionMaxima, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extension); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extension[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (!address) break;
        QueryOcclusion(address);
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registers
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) {
            WriteRegister(index + i - 1, data.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (words < 2) break;
        const uint64_t height = data.Read();
        const uint64_t low = data.Read();
        bin_mask_ = (height << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (words < 2) break;
        const uint64_t height = data.Read();
        const uint64_t low = data.Read();
        bin_select_ = (height << 32) | low;
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: first value = register index, the rest is written consecutively.
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < words; ++i) {
          WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t amount = data.Read() & 0xFFF;
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registers
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < amount; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // Like the emulated path: when the query ends it is reported as visible,
        // in case the game reads the result.
        if (words < 1) break;
        const uint32_t datum = data.Read();
        const uint32_t id = datum & 0x3F;
        if (!(datum & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg_entry = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                            : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          const uint32_t bit = UINT32_C(1) << (id < 32 ? id : id - 32);
          SaveRegister(reg_entry, Register(reg_entry) | bit);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        // DRAW_INDX is preceded by the visibility query word.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) {
          data.Read();
        }
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        // Indices in a buffer (DMA): the base and size come in the packet and are
        // stored in their registers, like ExecutePacketType3Draw
        // (graphics/command_processor.cpp:1379-1390). Without this, indexed draws
        // read VGT_DMA_SIZE as zero.
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (words < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        AddOnlyRing(draws_);
        // No fetch_add, which on the A57 is an ldxr/stxr loop on every draw. Only this thread writes
        // g_draws, and the "[hitch]" line reads it from this same thread (Present): AddOnlyRing is
        // enough.
        AddOnlyRing(fh1::waits::g_draws);
        // A draw with RB_MODECONTROL in copy mode is a resolve (and/or a
        // clear): the emulated path treats it that way (vulkan/command_processor.cpp:3653).
        if ((Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          AddOnlyRing(copies_);
          last_copy_target_.store(Register(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE),
                                      std::memory_order_relaxed);
          last_copy_control_.store(Register(rex::graphics::XE_GPU_REG_RB_COPY_CONTROL),
                                      std::memory_order_relaxed);
          last_copy_info_.store(Register(rex::graphics::XE_GPU_REG_RB_COPY_DEST_INFO),
                                   std::memory_order_relaxed);
          last_copy_pitch_.store(Register(rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH),
                                    std::memory_order_relaxed);
          DoCopy();
        } else {
          CountDrawShaders();
          MatchDraw();
          TraceDraw();
          // In mode 5 (depth only) the VS is enough: Xenos does not run the PS, and D3D leaves
          // the PS object at 0 in some of the race shadow draws.
          if (vs_draw_ &&
              (ps_draw_ || (Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5)) {
            DrawNative();
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type is in the two low bits.
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        const uint8_t* source = memory_->TranslatePhysical(address_type & ~uint32_t(0x3));
        if (size && (address_type & 0x3) <= 1) {
          LoadShaderCached(address_type, size, source);
          break;
        }
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
        IdentifyShader(address_type & 0x3);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet itself.
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        if (size > words - 2) break;
        LoadImmediate(data, type, size);  // with an exact cache (fh1_native_im_immediate_cache)
        break;
      }
      case xenos::PM4_NOP:
        // The FlushState composite marker (phase 2 of the Direct3D-level renderer).
        // A NOP without its magic belongs to the game itself and does nothing, as before.
        ProcessMarker(data, words);
        break;
      default:
        // Invalidations and the rest: nothing is drawn yet.
        break;
    }
  }

  // Step C2: a draw in copy mode is a resolve and/or a clear.
  bool EnsureTargets() {
    if (!targets_ && !targets_failed_) {
      targets_ = TargetsNative::Create(provider_ ? provider_->vulkan_device() : nullptr, memory_);
      if (!targets_) {
        targets_failed_ = true;
        REXLOG_ERROR("[native] Could not create the native targets (C2)");
      }
    }
    return targets_ != nullptr;
  }

  // Diagnostic fh1_native_diag_constants_ps: the first constants of the requested PS, at most once
  // every fh1_native_diag_constants_ms per PS (exposure and brightness of the visual treatment).
  void NoteConstantsPs() {
    if (!ps_draw_) {
      return;
    }
    const std::string& list = REXCVAR_GET(fh1_native_diag_constants_ps);
    if (list.empty()) {
      return;
    }
    if (list != constants_list_text_) {
      constants_list_text_ = list;
      constants_list_.clear();
      size_t start = 0;
      while (start <= list.size()) {
        const size_t fin = std::min(list.find(',', start), list.size());
        if (fin > start) {
          constants_list_.insert(uint32_t(std::strtoul(list.substr(start, fin - start).c_str(), nullptr, 10)));
        }
        start = fin + 1;
      }
    }
    if (!constants_list_.count(ps_draw_->number)) {
      return;
    }
    const auto now = Clock::now();
    auto& last = constants_last_[ps_draw_->number];
    if (now - last < std::chrono::milliseconds(REXCVAR_GET(fh1_native_diag_constants_ms))) {
      return;
    }
    last = now;
    std::string values;
    for (uint32_t k = 0; k < 12; ++k) {
      values += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = registers_[0x4400 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        values += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      values += ")";
    }
    // FH1: the boolean constants too (0x4900-0x4903 vertex, 0x4904-0x4907 pixel).
    for (uint32_t i = 0; i < 8; ++i) {
      values += fmt::format(" bool{}={:08X}", i, Register(0x4900 + i));
    }
    REXLOG_INFO("[native] PS constants n{} (Swap {}):{}", ps_draw_->number, swaps_.load(), values);
    {  // NFSC: how many of the 224 pixel-shader constants (and 256 vertex-shader ones) are non-zero
      auto count = [&](uint32_t base, uint32_t total, std::string* list) {
        uint32_t nz = 0;
        for (uint32_t k = 0; k < total; ++k) {
          bool any = false;
          float v[4];
          for (uint32_t c = 0; c < 4; ++c) {
            const uint32_t bits = registers_[base + k * 4 + c];
            std::memcpy(&v[c], &bits, sizeof(float));
            any |= bits != 0;
          }
          if (any) {
            if (list && nz < 8) *list += fmt::format(" c{}=({:.3g},{:.3g},{:.3g},{:.3g})", k, v[0], v[1], v[2], v[3]);
            ++nz;
          }
        }
        return nz;
      };
      std::string list_ps;
      const uint32_t nz_ps = count(0x4400, 224, &list_ps);
      const uint32_t nz_vs = count(0x4000, 256, nullptr);
      REXLOG_INFO("[fh1] PS n{}: {} of 224 PS constants non-zero ({} of 256 VS constants):{}", ps_draw_->number, nz_ps, nz_vs, list_ps);
      std::string blur;  // NFSC: the blur taps live at c174-c188 (offsets) and c190-c204 (weights)
      for (uint32_t k : {174u, 190u, 207u, 236u, 239u, 240u, 253u, 254u, 255u}) {
        float v[4];
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t bits = registers_[0x4400 + k * 4 + c];
          std::memcpy(&v[c], &bits, sizeof(float));
        }
        blur += fmt::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", k, v[0], v[1], v[2], v[3]);
      }
      REXLOG_INFO("[fh1] PS n{} blur constants:{}", ps_draw_->number, blur);
    }
  }

  // Steps C3-C6: the draw, with its VS and PS identified, goes to Vulkan.
  /*
   * Measurement only. A fence or an interrupt the game can see: how many vertex copies are still
   * pending at that moment (the ring only waits for them before returning the read pointer). Ring
   * thread only.
   */
  /*
   * Measurement only. When the ring finishes a frame (its Swap, already counted in swaps_), how many
   * more Swaps the game has written: 0 or less = the ring is caught up; 1 = the next frame is already
   * entirely in the ring; 2 or more = two or more. The game counts after writing the Swap, so a
   * momentary -1 is normal. The first difference is recorded separately in case the two counters do
   * not start together. Ring thread only.
   */
  void NoteGameByAhead() {
    const int64_t difference = int64_t(::g_fh1_frames_game.load(std::memory_order_relaxed)) -
                               int64_t(swaps_.load(std::memory_order_relaxed));
    if (!game_ahead_first_valid_) {
      game_ahead_first_valid_ = true;
      game_ahead_first_ = difference;
    }
    ++game_ahead_[difference <= 0 ? 0 : difference == 1 ? 1 : 2];
    game_ahead_max_ = std::max(game_ahead_max_, difference);
  }

  void NoteFenceCopies() {
    ++fences_total_;
    if (targets_) {
      const size_t pending_2 = targets_->CopiesPending();
      if (pending_2) {
        ++fences_with_copies_;
        fences_copies_max_ = std::max(fences_copies_max_, pending_2);
      }
    }
  }

  void DrawNative() {
    if (!EnsureTargets()) {
      return;
    }
    // NoteConstantsPs used to read the text cvar on every draw, and REXCVAR_GET of a string
    // is a call with an initialization guard (a load with acquire) that cannot be folded. The
    // flag is refreshed once per Swap: the diagnostic takes at most one frame to turn on.
    if (diag_constants_active_) {
      NoteConstantsPs();
    }
    RequestDraw request;
    request.register_values = registers_.data();
    request.vs = vs_draw_;
    request.ps = ps_draw_;
    request.vs_microcode = vs_microcode_;
    request.generation_vs = generation_vs_;
    request.generation_constants_vs = generation_constants_vs_;
    request.generation_constants_ps = generation_constants_ps_;
    request.generation_fetch = generation_fetch_;
    request.generation_framing = generation_framing_;
    request.vegetation_game = vegetation_game_;  // fh1_d3d_game_vegetation
    // Timer on 1 of every kStopwatchDrawsEvery draws, with the time scaled up (as in Packet).
    // The phase comes from a counter that is not reset on each report (draws_measured_ is), offset to
    // the middle of the batch: counting from 1, draws 33, 97, 161... are timed here, and the stage timer
    // of DrawsVulkanImpl::Draw runs on 64, 128, 192... Those pay ~20 extra clock reads; previously,
    // after each report, 1 time in 8 they landed exactly on the draws timed here and inflated
    // "draws us/draw".
    if ((phase_stopwatch_draws_++ & (kStopwatchDrawsEvery - 1)) == kStopwatchDrawsEvery / 2) {
      const auto start = Clock::now();
      targets_->Draw(request);
      time_draws_ns_ += NanosecondsSince(start) * kStopwatchDrawsEvery;
    } else {
      targets_->Draw(request);
    }
    ++draws_measured_;
  }

  void DoCopy() {
    if (!EnsureTargets()) {
      return;
    }
    namespace g = rex::graphics;
    RegistersCopy r;
    r.rb_surface_info = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = Register(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = Register(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = Register(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = Register(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = Register(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = Register(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = Register(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = Register(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    TraceCopy(r);
    const auto start = Clock::now();
    targets_->Copy(r);
    time_copies_ns_ += NanosecondsSince(start);
    ++copies_measurements_;
  }

  // Diagnostic (fh1_native_diag_frame_s): every draw and every copy of one whole
  // frame, from the first Swap after that many seconds until the next one.
  void TraceSwap() {
    // Once per Swap, not once per draw (see DrawNative).
    diag_constants_active_ = !REXCVAR_GET(fh1_native_diag_constants_ps).empty();
    if (tracing_) {
      tracing_ = false;
      trace_done_ = true;
      REXLOG_INFO("[trace] end of frame: {} lines", traces_);
      return;
    }
    const int32_t seconds = REXCVAR_GET(fh1_native_diag_frame_s);
    // NFSC: -1 = when a file named trace_now appears in the working folder (removed when seen).
    std::error_code ec_trace;
    if (!trace_done_ && ((seconds > 0 && Clock::now() - start_system_ >= std::chrono::seconds(seconds)) ||
                          (seconds == -1 && std::filesystem::remove("trace_now", ec_trace)))) {
      tracing_ = true;
      traces_ = 0;
      REXLOG_INFO("[trace] whole frame after Swap {}", swaps_.load());
    }
  }

  // One line per draw: shaders, render targets, state and the textures of the PS samplers
  // (address / format / dimension; ! if the fetch constant is not a texture).
  void TraceDraw() {
    if (!tracing_ || traces_ >= 20000) {
      return;
    }
    ++traces_;
    namespace g = rex::graphics;
    std::string textures;
    if (ps_draw_) {
      for (const SamplerShader& s : ps_draw_->samplers) {
        if (s.reg_entry >= 16) {
          continue;
        }
        const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_entry) * 6;
        const uint32_t d0 = Register(base);
        const uint32_t d1 = Register(base + 1);
        const uint32_t d3 = Register(base + 3);
        const uint32_t d5 = Register(base + 5);
        textures += fmt::format(" t{}={:08X}/f{}/d{}/s{:03X}/e{}/sign{:02X}/exp{}{}", s.reg_entry, d1 & 0xFFFFF000,
                                d1 & 0x3F, (d5 >> 9) & 0x3, (d3 >> 1) & 0xFFF, (d1 >> 6) & 0x3, (d0 >> 2) & 0xFF,
                                int32_t(d3 << 13) >> 26,
                                (d0 & 0x3) == 2 ? "" : "!");
      }
    }
    if (!vs_draw_ || !ps_draw_) {
      static constexpr const char* kReasons[] = {"registration with unknown shaders",
                                                 "no registration",
                                                 "registration with shaders different from the IM_LOAD"};
      textures += fmt::format(" | {}: objects VS {:08X} PS {:08X}; IM_LOAD PS n{} VS n{}; pending:",
                              kReasons[reason_matched_ % 3], object_vs_, object_ps_,
                              ps_actual_ ? int(ps_actual_->number) : -1,
                              vs_actual_ ? int(vs_actual_->number) : -1);
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        const RegisterDraw& pending = pending_[i];
        const EntryShader* vs = ShaderOfObjectCached(pending.vs);
        const EntryShader* ps = ShaderOfObjectCached(pending.ps);
        textures += fmt::format(" [function {} type {} count {} VS n{} PS n{}]",
                                int(pending.function), pending.args[0],
                                CountOfRegister(pending), vs ? int(vs->number) : -1,
                                ps ? int(ps->number) : -1);
      }
    }
    const uint32_t initiator = Register(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    REXLOG_INFO("[trace] draw VS n{} PS n{} type {} count {} surf {:08X} rt0 {:08X} rt1 {:08X} mask {:08X} blend "
                "{:08X} colorctl {:08X} depth {:08X} stencil {:08X} mode {:08X} window {:08X} tl {:08X} br {:08X} "
                "vte {:08X} yoff {:.0f}{}",
                vs_draw_ ? int(vs_draw_->number) : -1, ps_draw_ ? int(ps_draw_->number) : -1,
                initiator & 0x3F, initiator >> 16, Register(g::XE_GPU_REG_RB_SURFACE_INFO),
                Register(g::XE_GPU_REG_RB_COLOR_INFO), Register(g::XE_GPU_REG_RB_COLOR1_INFO),
                Register(g::XE_GPU_REG_RB_COLOR_MASK), Register(g::XE_GPU_REG_RB_BLENDCONTROL0),
                Register(g::XE_GPU_REG_RB_COLORCONTROL), Register(g::XE_GPU_REG_RB_DEPTHCONTROL),
                Register(g::XE_GPU_REG_RB_STENCILREFMASK), Register(g::XE_GPU_REG_RB_MODECONTROL),
                Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET), Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL),
                Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR), Register(g::XE_GPU_REG_PA_CL_VTE_CNTL),
                std::bit_cast<float>(Register(g::XE_GPU_REG_PA_CL_VPORT_YOFFSET)), textures);
    TraceVertices(initiator);
  }

  // Diagnostic (fh1_native_diag_vertices_ps): the fetch of each VS element, original and
  // patched, the bytes of the draw's first vertices and the center texels of its 8888
  // textures, as they are in guest memory.
  void TraceVertices(uint32_t initiator) {
    namespace g = rex::graphics;
    if (!vs_draw_ || !ps_draw_ || traces_ >= 20000 ||
        vs_microcode_.size() != vs_draw_->microcode.size()) {
      return;
    }
    const std::string list = REXCVAR_GET(fh1_native_diag_vertices_ps);
    bool in_list = false;
    uint32_t input_value = 0;
    bool there_is = false;
    for (char c : list + ",") {
      if (c >= '0' && c <= '9') {
        input_value = input_value * 10 + uint32_t(c - '0');
        there_is = true;
      } else if (there_is) {
        in_list = in_list || input_value == ps_draw_->number;
        input_value = 0;
        there_is = false;
      }
    }
    if (!in_list) {
      return;
    }
    ++traces_;
    std::string detail;
    uint32_t slot = UINT32_MAX;
    uint32_t stride = 0;
    for (const ElementVertex& element : vs_draw_->elements) {
      const size_t i = size_t(element.instruction) * 3;
      const uint32_t o0 = vs_draw_->microcode[i], o1 = vs_draw_->microcode[i + 1];
      const uint32_t p0 = vs_microcode_[i], p1 = vs_microcode_[i + 1], p2 = vs_microcode_[i + 2];
      detail += fmt::format(" {}{}@{}: original r{} s{:03X} fmt{}; patched r{} op{} s{:03X} fmt{} f{} z{} o{} "
                            "({:08X} {:08X} {:08X});",
                             NameUse(element.use), element.index_use, element.instruction,
                             (o0 >> 12) & 0x3F, o1 & 0xFFF, (o1 >> 16) & 0x3F, (p0 >> 12) & 0x3F,
                             p0 & 0x1F, p1 & 0xFFF, (p1 >> 16) & 0x3F,
                             ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3), p2 & 0xFF,
                             int32_t(p2 << 1) >> 9, p0, p1, p2);
      if (slot == UINT32_MAX && (p0 & 0x1F) == 0 && !((p1 >> 30) & 0x1)) {
        slot = ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3);
        stride = (p2 & 0xFF) * 4;
      }
    }
    if (slot < 96 && stride && stride <= 64) {
      const uint32_t f0 = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 2);
      const uint32_t f1 = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 2 + 1);
      const uint32_t source_2 = (initiator >> 6) & 0x3;
      uint32_t first_2 = Register(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
      if (source_2 == uint32_t(xenos::SourceSelect::kDMA)) {
        const bool de_32 = (initiator >> 11) & 0x1;
        const uint32_t base_indices =
            Register(g::XE_GPU_REG_VGT_DMA_BASE) & (de_32 ? 0x1FFFFFFC : 0x1FFFFFFE);
        const auto order_indices =
            static_cast<xenos::Endian>(Register(g::XE_GPU_REG_VGT_DMA_SIZE) >> 30);
        if (de_32) {
          uint32_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 4);
          first_2 = (first_2 + (xenos::GpuSwap(raw, order_indices) & 0xFFFFFF)) & 0xFFFFFF;
        } else {
          uint16_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 2);
          first_2 = (first_2 + xenos::GpuSwap(raw, order_indices)) & 0xFFFFFF;
        }
      }
      const uint64_t address = uint64_t(f0 & 0x1FFFFFFC) + uint64_t(first_2) * stride;
      std::string bytes;
      if (address + uint64_t(4) * stride <= 0x20000000) {
        const uint8_t* data = memory_->TranslatePhysical(uint32_t(address));
        for (uint32_t b = 0; b < 4 * stride; ++b) {
          bytes += fmt::format("{}{:02X}", b % stride == 0 ? " | " : (b % 4 == 0 ? " " : ""),
                               data[b]);
        }
      }
      detail += fmt::format(" fetch f{} {:08X} {:08X} (order {}) source {} first {} stride {}:{}",
                             slot, f0, f1, f1 & 0x3, source_2, first_2, stride, bytes);
    }
    for (const SamplerShader& s : ps_draw_->samplers) {
      if (s.reg_entry >= 16) {
        continue;
      }
      const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_entry) * 6;
      const uint32_t t0 = Register(base), t1 = Register(base + 1), t2 = Register(base + 2);
      if ((t0 & 0x3) != 2 || (t1 & 0x3F) != 6) {
        continue;
      }
      const uint32_t width = (t2 & 0x1FFF) + 1, height = ((t2 >> 13) & 0x1FFF) + 1;
      const uint32_t pitch = std::max<uint32_t>(((t0 >> 22) & 0x1FF) << 5, 1);
      const bool tile = (t0 >> 31) & 0x1;
      const uint32_t y = height / 2;
      std::string texels;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t x = std::min(width - 1, (width > 8 ? width / 2 - 4 : 0) + k);
        const int64_t displacement = tile ? OffsetTileDiag(int32_t(x), int32_t(y), pitch, 2)
                                               : int64_t(y) * pitch * 4 + int64_t(x) * 4;
        const uint64_t d = uint64_t(t1 & 0x1FFFF000) + uint64_t(displacement);
        if (displacement < 0 || d + 4 > 0x20000000) {
          break;
        }
        const uint8_t* texel = memory_->TranslatePhysical(uint32_t(d));
        texels += fmt::format(" {:02X}{:02X}{:02X}{:02X}", texel[0], texel[1], texel[2], texel[3]);
      }
      detail += fmt::format("; t{} {}x{} {} pitch {} swizzle {:03X} order {} signs {:02X} row {}:{}",
                             s.reg_entry, width, height, tile ? "tiled" : "linear", pitch,
                             (Register(base + 3) >> 1) & 0xFFF, (t1 >> 6) & 0x3, (t0 >> 2) & 0xFF,
                             y, texels);
      const uint32_t address_texture = t1 & 0x1FFFF000;
      if (textures_dumped_.size() < 8 && textures_dumped_.insert(address_texture).second) {
        std::vector<uint8_t> dump(size_t(width) * height * 4);
        bool complete = true;
        for (uint32_t yy = 0; yy < height && complete; ++yy) {
          for (uint32_t xx = 0; xx < width; ++xx) {
            const int64_t dt = tile ? OffsetTileDiag(int32_t(xx), int32_t(yy), pitch, 2)
                                       : int64_t(yy) * pitch * 4 + int64_t(xx) * 4;
            const uint64_t texel = uint64_t(address_texture) + uint64_t(dt);
            if (dt < 0 || texel + 4 > 0x20000000) {
              complete = false;
              break;
            }
            std::memcpy(dump.data() + (size_t(yy) * width + xx) * 4,
                        memory_->TranslatePhysical(uint32_t(texel)), 4);
          }
        }
        const auto path = rex::filesystem::GetExecutableFolder() /
                          fmt::format("diag_texture_{:08X}_{}x{}.bin", address_texture, width, height);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(dump.data()), std::streamsize(dump.size()));
        detail += fmt::format(" (dumped to {}{})", path.filename().string(),
                               complete ? "" : ", incompleta");
      }
    }
    detail += "; VS constants";
    for (uint32_t c = 0; c < 16; ++c) {
      const uint32_t b = 0x4000 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(Register(b)),
                             std::bit_cast<float>(Register(b + 1)), std::bit_cast<float>(Register(b + 2)),
                             std::bit_cast<float>(Register(b + 3)));
    }
    detail += "; PS constants";
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t b = 0x4400 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(Register(b)),
                             std::bit_cast<float>(Register(b + 1)), std::bit_cast<float>(Register(b + 2)),
                             std::bit_cast<float>(Register(b + 3)));
    }
    REXLOG_INFO("[trace] vertices PS n{} VS n{}:{}", ps_draw_->number, vs_draw_->number,
                detail);
  }

  // GetTiledOffset2D (graphics/pipeline/texture/util.cpp), same as OffsetTile2D in
  // fh1_native_draws.cpp; here only for the diagnostic. pitch in blocks.
  static int32_t OffsetTileDiag(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
    pitch = (pitch + 31) & ~uint32_t(31);
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }

  void TraceCopy(const RegistersCopy& r) {
    if (!tracing_ || traces_ >= 20000) {
      return;
    }
    ++traces_;
    REXLOG_INFO("[trace] copy control {:08X} target {:08X} info {:08X} pitch {:08X} surf {:08X} rt0 {:08X} depth "
                "{:08X} clear {:08X}/{:08X} depth_clear {:08X}",
                r.rb_copy_control, r.rb_copy_dest_base, r.rb_copy_dest_info, r.rb_copy_dest_pitch,
                r.rb_surface_info, r.rb_color_info[0], r.rb_depth_info, r.rb_color_clear,
                r.rb_color_clear_lo, r.rb_depth_clear);
  }

  // Step C5a: the last VS (type 0) or PS (type 1) uploaded to the ring.
  // IM_LOAD with a cache. The game reloads the same shaders on almost every draw: in a race on the PC,
  // 70,000-97,000 loads per second of about 82 distinct microcodes. Each load byte-swapped the data,
  // computed the XXH3 to identify it and copied the VS, and each new VS then repeated the XXH3 in
  // FingerprintVs and in NoteEntryVertices. If the address, size and bytes match an earlier load, its
  // swapped copy, identification, fingerprint and generation are reused. The bytes must be compared:
  // D3D patches VS microcode in place, so the address alone is not enough. Same microcode, same
  // generation: whatever is cached per generation (vertex input, fingerprint, coherence) stays valid.
  //
  // IM_LOAD without memcmp (fh1_native_im_load_without_memcmp). The memcmp above cost ~0.6 us per IM_LOAD
  // (~41,000 per second in a race). Whoever writes microcode announces it (the D3D constructors and the
  // fetch patcher; the list and the cross-thread protocol are in fh1_microcode_versions.h). Each way
  // records the versions of its slot it was checked against guest memory with; if they are unchanged,
  // nobody has written that microcode since, and it is used without reading it.
  // Self-checking guard:
  //   - watching: always the regular memcmp and, if the shortcut had a candidate, it is compared with the
  //     memcmp result. After kImShortcutALook agreements, no disagreement and at least one in-place patch
  //     seen (proof that the sub_825A2FB8 hook runs: without it, a patch would not bump any version), it
  //     moves to applying;
  //   - applying: the candidate is used without memcmp, except 1 in every kImShortcutCheckEvery, which is
  //     still compared;
  //   - a disagreement (the candidate no longer matches memory) or a patcher call that is neither of the
  //     two known ones: "DIFFERENCE" in the log and the shortcut turned off for the run. That load and
  //     the following ones use memcmp.
  void LoadShaderCached(uint32_t address_type, uint32_t size, const uint8_t* source) {
    const uint32_t type = address_type & 0x3;
    const size_t bytes = size_t(size) * sizeof(uint32_t);
    auto& vias = loads_cache_[type][size_t((uint64_t(address_type) * 0x9E3779B97F4A7C15ull) >> 57)];
    if (im_shortcut_phase_ == kImShortcutWithoutBegin) {
      BeginImShortcut();
    }
    const uint32_t slot = microcode::SlotOf(address_type & ~uint32_t(0x3));
    // 1. The shortcut: a way with the same contents, checked with the current versions.
    LoadShader* candidate = nullptr;
    if (im_shortcut_phase_ != kImShortcutOff) {
      const uint32_t global = microcode::GlobalRelaxed();
      for (LoadShader& via : vias) {
        if (!via.validated || via.address_type != address_type || via.size != size) {
          continue;
        }
        if (via.version_global != global) {
          // Someone has written microcode since it was checked: if not in its slot, it is still valid.
          if (via.version_slot != microcode::StartAfterGlobal(slot)) {
            via.validated = false;
            ++im_i_version_changed_;
            continue;
          }
          if (microcode::g_patches_others.load(std::memory_order_relaxed) != 0) {
            TurnOffImShortcutByOthers();
            break;
          }
          via.version_global = global;
        }
        candidate = &via;
        break;
      }
    }
    LoadShader* load = nullptr;
    if (candidate && im_shortcut_phase_ == kImShortcutApplying &&
        (++im_shortcut_turn_ & (kImShortcutCheckEvery - 1)) != 0) {
      // 2. Without reading the guest microcode.
      load = candidate;
      ++loads_cached_;
      ++im_i_sin_memcmp_;
    } else {
      // 3. The regular path, with the versions read before and after reading guest memory.
      const bool validate = im_shortcut_phase_ != kImShortcutOff;
      microcode::ReadAccess read;
      if (validate) {
        read = microcode::BeforeOfRead(slot);
      }
      for (LoadShader& via : vias) {
        if (via.address_type == address_type && via.size == size &&
            std::memcmp(via.raw.data(), source, bytes) == 0) {
          load = &via;
          break;
        }
      }
      if (validate && candidate) {
        if (load == candidate) {
          ++im_shortcut_checked_;
          ++im_i_checked_;
          if (im_shortcut_phase_ == kImShortcutWatching && im_shortcut_checked_ >= kImShortcutALook &&
              microcode::g_patches_in_its_room.load(std::memory_order_relaxed) != 0) {
            im_shortcut_phase_ = kImShortcutApplying;
            FH1_REPORT_RING("[native] C5a IM_LOAD without memcmp: {} loads checked against the memcmp, 0 "
                             "disagreements and {} in-place patches seen: APPLYING, the memcmp is skipped if the "
                             "version has not changed (1 in {} is still checked)",
                                 im_shortcut_checked_,
                                 microcode::g_patches_in_its_room.load(std::memory_order_relaxed),
                                 kImShortcutCheckEvery);
          }
        } else {
          TurnOffImShortcut(address_type, size, candidate->raw, candidate->version_slot,
                        candidate->version_global, load != nullptr, source, read);
        }
      }
      if (load) {
        ++loads_cached_;
      } else {
        load = vias[0].use <= vias[1].use ? &vias[0] : &vias[1];
        load->address_type = address_type;
        load->size = size;
        load->raw.resize(size);
        std::memcpy(load->raw.data(), source, bytes);
        load->host.resize(size);
        // The fresh copy is byte-swapped without reading game memory again: raw and host hold the same
        // contents, the ones the versions certify (with the guest idle, the same as before).
        const uint8_t* copy = reinterpret_cast<const uint8_t*>(load->raw.data());
        for (uint32_t i = 0; i < size; ++i) {
          load->host[i] = rex::memory::load_and_swap<uint32_t>(copy + size_t(i) * 4);
        }
        load->entry = shaders_.loaded() ? shaders_.Identify(type == 0, load->host) : nullptr;
        load->fingerprint = XXH3_64bits(load->host.data(), bytes);
        load->generation = ++generations_microcode_;
        load->validated = false;
      }
      if (validate && im_shortcut_phase_ != kImShortcutOff) {
        ++im_i_con_memcmp_;
        if (microcode::AfterOfRead(slot, read)) {
          load->validated = true;
          load->version_slot = read.start;
          load->version_global = read.global;
        } else {
          load->validated = false;  // someone wrote to its slot during the read: it will be compared again
          ++im_i_cancelled_;
        }
      }
    }
    load->use = ++loads_tic_;
    (type == 0 ? vs_actual_ : ps_actual_) = load->entry;
    if (tracing_ && traces_ < 4000) {
      ++traces_;
      REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, fingerprint {:016X})", type == 0 ? "VS" : "PS",
                  load->entry ? int(load->entry->number) : -1, load->host.size(), load->fingerprint);
    }
    if (type == 0) {
      vs_microcode_ = load->host;  // patched: the vertex input comes from here
      generation_vs_ = load->generation;
      fingerprint_vs_generation_ = load->generation;
      fingerprint_vs_ = load->fingerprint;
    }
  }

  // IM_LOAD without memcmp. The first load reads the cvar.
  void BeginImShortcut() {
    const bool requested = REXCVAR_GET(fh1_native_im_load_without_memcmp);
    im_shortcut_phase_ = requested && microcode::kCreationsWatched ? kImShortcutWatching : kImShortcutOff;
    FH1_REPORT_RING("[native] C5a IM_LOAD without memcmp (fh1_native_im_load_without_memcmp) = {}",
                         !requested ? "no (off with the cvar): memcmp on every IM_LOAD"
                         : !microcode::kCreationsWatched
                             ? "no: with NFSMW_NATIVE_SHADER_LIBRARY the constructors do not report"
                             : "YES, watching: every load is also compared with memcmp until there is proof");
  }

  const char* NamePhaseImShortcut() const {
    switch (im_shortcut_phase_) {
      case kImShortcutWatching:
        return "watching";
      case kImShortcutApplying:
        return "applying";
      case kImShortcutOff:
        return "OFF";
      default:
        return "not started";
    }
  }

  // The shortcut's candidate does not match guest memory: someone wrote microcode without announcing it.
  // (Its fields are passed separately: LoadShader is declared further down and cannot appear in the
  // signature.)
  void TurnOffImShortcut(uint32_t address_type, uint32_t size, const std::vector<uint32_t>& raw,
                     uint32_t version_slot, uint32_t version_global, bool other_via, const uint8_t* source,
                     const microcode::ReadAccess& read) {
    uint32_t first = 0;
    while (first < size && std::memcmp(raw.data() + first, source + size_t(first) * 4, 4) == 0) {
      ++first;
    }
    REXLOG_ERROR("[native] C5a IM_LOAD without memcmp: DIFFERENCE. The shortcut gave the copy of {} {:08X} ({} "
                 "words) checked with versions slot {} and global {}, and the guest memory is already different "
                 "(first differing word: {}; the memcmp {}). Current versions: start {} end {} global {}; phase {} "
                 "with {} loads checked. Writes reported: {} in-place patches, {} in the ring copy, {} others, {} "
                 "creations. Someone writes microcode without reporting it: shortcut OFF for the rest of the "
                 "session (every load with memcmp, as before build 184)",
                 (address_type & 0x3) == 0 ? "VS" : "PS", address_type & ~uint32_t(0x3), size,
                 version_slot, version_global, first,
                 other_via ? "finds another path" : "has to copy it again", read.start, read.fin,
                 read.global, NamePhaseImShortcut(), im_shortcut_checked_,
                 microcode::g_patches_in_its_room.load(std::memory_order_relaxed),
                 microcode::g_patches_in_copy.load(std::memory_order_relaxed),
                 microcode::g_patches_others.load(std::memory_order_relaxed),
                 microcode::g_creations.load(std::memory_order_relaxed));
    im_shortcut_phase_ = kImShortcutOff;
  }

  // The fetch patcher was called from outside its two known call sites.
  void TurnOffImShortcutByOthers() {
    REXLOG_ERROR("[native] C5a IM_LOAD without memcmp: DIFFERENCE. The fetch patcher (sub_825A2FB8) was called {} "
                 "times from outside its two known calls (return {:08X}, target {:08X}): it may write microcode "
                 "some other way. Shortcut OFF for the rest of the session (every load with memcmp, as before "
                 "build 184)",
                 microcode::g_patches_others.load(std::memory_order_relaxed),
                 microcode::g_other_return.load(std::memory_order_relaxed),
                 microcode::g_other_target.load(std::memory_order_relaxed));
    im_shortcut_phase_ = kImShortcutOff;
  }

  // IM_LOAD_IMMEDIATE (and unusual IM_LOAD loads): no cache.
  void IdentifyShader(uint32_t type) {
    if (type > 1) {
      return;
    }
    const EntryShader* entry =
        shaders_.loaded() ? shaders_.Identify(type == 0, microcode_) : nullptr;
    (type == 0 ? vs_actual_ : ps_actual_) = entry;
    if (tracing_ && traces_ < 4000) {
      ++traces_;
      REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, fingerprint {:016X})", type == 0 ? "VS" : "PS",
                  entry ? int(entry->number) : -1, microcode_.size(),
                  XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t)));
    }
    if (type == 0) {
      vs_immediate_ = microcode_;  // patched: the vertex input comes from here
      vs_microcode_ = vs_immediate_;
      generation_vs_ = ++generations_microcode_;
    }
  }

  // IM_LOAD_IMMEDIATE with an exact cache (fh1_native_im_immediate_cache).
  //
  // Who sends them: sub_825A37D8, the D3D copy path. If the GPU has not yet passed the fence of a VS's
  // last IM_LOAD, or outputs the PS does not read must be disabled (sub_825A36A8), it copies the microcode
  // at [VS+40] ([VS+600] bytes) into the ring itself and patches it there (sub_825A2FB8 on the copy).
  // Always type 0 (VS).
  // In a race there are stretches with 4,000-8,000 per second at 1.5-2.8 us each ("times per
  // packet", op 2B): the regular path byte-swaps word by word, computes the XXH3 in Identify, copies
  // the VS to vs_immediate_ and gives it a new generation, so the next draw redoes the fingerprint
  // (another XXH3 in FingerprintVs), the fetch coherence and the vertex input key. And the contents repeat:
  // over a whole run there are 82 distinct microcodes between IM_LOAD and immediate loads.
  //   - Set chosen by a cheap signature (size and four words, not swapped). Within the set, the way with
  //     the same signature, the same size and the same bytes (memcmp against its raw copy): equality is
  //     exact; the signature only chooses where to look.
  //   - Hit: the way's swapped copy, identification, fingerprint and generation. Same microcode, same
  //     generation, as in the IM_LOAD cache (LoadShaderCached): whatever is cached per generation
  //     stays valid.
  //   - Miss, or a packet that wraps around the ring (not contiguous): the regular path; the miss is
  //     stored.
  // Self-checking guard:
  //   - watching: every load takes the regular path and, if the cache had a way, it is compared with what
  //     the cache would give (swapped copy and identification). After kImmALook agreements and no
  //     disagreement it moves to applying;
  //   - applying: hits neither swap nor identify; 1 in every kImmCheckEvery is still compared;
  //   - a disagreement, or a new way whose swapped raw copy is not what the regular path read (the ring
  //     position arithmetic): "DIFFERENCE" in the log and the cache turned off for the run. That load
  //     already uses the regular result.
  struct ImmediateLoad {
    uint64_t signature = 0;  // 0 = via empty
    uint32_t size = 0;
    std::vector<uint32_t> raw;  // as it is in the ring (big-endian)
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const EntryShader* entry = nullptr;
    uint64_t fingerprint = 0;      // host XXH3, the same one FingerprintVs would compute
    uint64_t generation = 0;  // from generations_microcode_, when this content was stored
    uint64_t use = 0;         // the least recently used way is the one replaced
  };

  void LoadImmediate(Reader data, uint32_t type, uint32_t size) {
    if (imm_phase_ == kImmWithoutBegin) {
      BeginImmediate();
    }
    const bool lap = data.mask != 0 && size_t(data.pos) + size > size_t(data.mask) + 1;
    if (imm_phase_ == kImmOff || type > 1 || size == 0 || lap) {
      if (lap) {
        ++imm_i_lap_;
      } else {
        ++imm_i_without_cache_;
      }
      LoadImmediateWithoutCache(data, type, size);
      return;
    }
    const uint8_t* raw = data.base + size_t(data.pos) * 4;
    const size_t bytes = size_t(size) * sizeof(uint32_t);
    const uint64_t signature = SignatureImmediate(raw, size);
    const size_t index = size_t(signature >> (64 - kImmBitsSet));
    auto& set = imm_cache_[type][index];
    uint8_t& last_2 = imm_last_[type][index];
    ImmediateLoad* via = nullptr;
    for (uint32_t k = 0; k < kImmVias; ++k) {  // first the way of the set's last hit
      const uint32_t v = (last_2 + k) % kImmVias;
      ImmediateLoad& candidate_2 = set[v];
      if (candidate_2.signature == signature && candidate_2.size == size &&
          std::memcmp(candidate_2.raw.data(), raw, bytes) == 0) {
        via = &candidate_2;
        last_2 = uint8_t(v);
        break;
      }
    }
    if (via && imm_phase_ == kImmApplying && (++imm_turn_ & (kImmCheckEvery - 1)) != 0) {
      // Hit: no byte swap, no identification, no copy, no new generation.
      via->use = ++imm_tic_;
      UseImmediate(type, *via);
      ++imm_i_hits_;
      ++loads_cached_;
      if (tracing_ && traces_ < 4000) {
        ++traces_;
        REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, fingerprint {:016X}; IM_LOAD_IMMEDIATE from the cache)",
                    type == 0 ? "VS" : "PS", via->entry ? int(via->entry->number) : -1, via->host.size(),
                    via->fingerprint);
      }
      return;
    }
    // The regular path: swaps, identifies and leaves its result in use
    // (vs_actual_ or ps_actual_, vs_immediate_...).
    LoadImmediateWithoutCache(data, type, size);
    const EntryShader* entry = type == 0 ? vs_actual_ : ps_actual_;
    if (via) {
      // Check: what the cache would have given against what the regular path just gave.
      uint32_t first = 0;
      while (first < size && first < via->host.size() && via->host[first] == microcode_[first]) {
        ++first;
      }
      if (first != size || via->host.size() != size || via->entry != entry) {
        TurnOffImmediate("a cache hit does not give the same as the usual path", type, size, signature, first,
                        via->entry, entry);
        return;
      }
      ++imm_checked_;
      ++imm_i_checked_;
      via->use = ++imm_tic_;
      if (imm_phase_ == kImmWatching && imm_checked_ >= kImmALook) {
        imm_phase_ = kImmApplying;
        FH1_REPORT_RING("[native] C5a IM_LOAD_IMMEDIATE with cache: {} hits checked against the usual path and 0 "
                         "disagreements: APPLYING, hits are no longer swapped nor identified (1 in {} is still "
                         "checked)",
                             imm_checked_, kImmCheckEvery);
      }
      if (imm_phase_ == kImmApplying) {
        UseImmediate(type, *via);  // the same generation as the unchecked hits
      }
      return;
    }
    // Miss: into the least recently used way of the set.
    ++imm_i_misses_;
    uint32_t room = 0;
    for (uint32_t k = 1; k < kImmVias; ++k) {
      if (set[k].use < set[room].use) {
        room = k;
      }
    }
    ImmediateLoad& new_entry = set[room];
    new_entry.signature = signature;
    new_entry.size = size;
    new_entry.raw.resize(size);
    std::memcpy(new_entry.raw.data(), raw, bytes);
    new_entry.host.assign(microcode_.begin(), microcode_.end());
    new_entry.entry = entry;
    new_entry.fingerprint = XXH3_64bits(new_entry.host.data(), bytes);
    new_entry.generation = ++generations_microcode_;
    new_entry.use = ++imm_tic_;
    last_2 = uint8_t(room);
    // The raw copy comes from the packet's position in the ring: once swapped it must equal what the
    // regular path read word by word. Only on misses (a few dozen per run).
    const uint8_t* copy = reinterpret_cast<const uint8_t*>(new_entry.raw.data());
    for (uint32_t i = 0; i < size; ++i) {
      if (rex::memory::load_and_swap<uint32_t>(copy + size_t(i) * 4) != new_entry.host[i]) {
        new_entry.signature = 0;  // empty way: cannot come out again
        new_entry.use = 0;
        TurnOffImmediate("the raw copy of a new path is not what the usual path read", type, size, signature,
                        i, entry, entry);
        return;
      }
    }
    if (imm_phase_ == kImmApplying) {
      UseImmediate(type, new_entry);
    }
  }

  // The regular IM_LOAD_IMMEDIATE path (formerly inline in PacketType3).
  void LoadImmediateWithoutCache(Reader data, uint32_t type, uint32_t size) {
    microcode_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
      microcode_[i] = data.Read();
    }
    IdentifyShader(type);
  }

  // The same state IdentifyShader leaves, taken from the way (without swapping, identifying or
  // hashing).
  void UseImmediate(uint32_t type, const ImmediateLoad& via) {
    (type == 0 ? vs_actual_ : ps_actual_) = via.entry;
    if (type == 0) {
      vs_microcode_ = via.host;  // patched: the vertex input comes from here
      generation_vs_ = via.generation;
      fingerprint_vs_generation_ = via.generation;
      fingerprint_vs_ = via.fingerprint;
    }
  }

  // Cheap signature of an unswapped packet: picks the set and rejects other shaders' ways before the
  // memcmp.
  static uint64_t SignatureImmediate(const uint8_t* raw, uint32_t size) {
    const size_t positions[4] = {0, size_t(size) / 3, size_t(size) * 2 / 3, size_t(size) - 1};
    uint64_t h = uint64_t(size) * 0x9E3779B97F4A7C15ull;
    for (const size_t p : positions) {
      uint32_t word;
      std::memcpy(&word, raw + p * 4, sizeof(word));
      h = (h ^ word) * 0xBF58476D1CE4E5B9ull;
      h ^= h >> 29;
    }
    return h | 1;  // never 0: 0 means an empty way
  }

  void BeginImmediate() {
    const bool requested = REXCVAR_GET(fh1_native_im_immediate_cache);
    imm_phase_ = requested ? kImmWatching : kImmOff;
    FH1_REPORT_RING("[native] C5a IM_LOAD_IMMEDIATE with cache (fh1_native_im_immediate_cache) = {}",
                         requested ? "YES, watching: every hit is compared with the usual path until there is proof"
                                : "no (off with the cvar): every IM_LOAD_IMMEDIATE is swapped and identified, as "
                                  "before");
  }

  const char* NamePhaseImmediate() const {
    switch (imm_phase_) {
      case kImmWatching:
        return "watching";
      case kImmApplying:
        return "applying";
      case kImmOff:
        return "OFF";
      default:
        return "not started";
    }
  }

  // The cache does not match the regular path: it is turned off for the rest of the run.
  void TurnOffImmediate(const char* reason, uint32_t type, uint32_t size, uint64_t signature, uint32_t first,
                       const EntryShader* de_la_cache, const EntryShader* of_always) {
    REXLOG_ERROR("[native] C5a IM_LOAD_IMMEDIATE with cache: DIFFERENCE, {}: {} of {} words (signature {:016X}), "
                 "first differing word {}, cache shader n{} and usual-path shader n{}; phase {} with {} hits "
                 "checked. Cache OFF for the rest of the session (everything by the usual path, as before build "
                 "184); this load already goes by the usual path",
                 reason, type == 0 ? "VS" : "PS", size, signature, first,
                 de_la_cache ? int(de_la_cache->number) : -1, of_always ? int(of_always->number) : -1,
                 NamePhaseImmediate(), imm_checked_);
    imm_phase_ = kImmOff;
  }

  void CountDrawShaders() {
    if (!shaders_.loaded()) {
      return;
    }
    if (!vs_actual_) {
      ++draws_without_vs_;
    } else if (!ps_actual_) {
      ++draws_without_ps_;
    } else {
      ++draws_identified_;
    }
    const uint64_t par = (uint64_t(vs_actual_ ? vs_actual_->number + 1 : 0) << 32) |
                         (ps_actual_ ? ps_actual_->number + 1 : 0);
    // In a race there are 40 distinct pairs and ~2,200 draws per frame, mostly runs of the same
    // material. Remembering the previous pair avoids the set insert (a hash and a possible cache
    // miss) on every draw.
    if (par != last_pair_shaders_) {
      last_pair_shaders_ = par;
      if (pairs_.size() < 4096) {
        pairs_.insert(par);
      }
    }
  }

  // Step C5b: the game thread's Draw* record that corresponds to this ring draw.
  // They arrive in the same order; the primitive type and count are checked, and
  // at most 8 records without a draw are skipped.
  // Phase 2b: if the last marker carries its Draw* record and this draw accepts it (the usual acceptance:
  // type, count and shaders consistent with the IM_LOAD packets), apply mode uses it without the queue or
  // the search. If it is not accepted, it is kept for the next draw (like the head of the queue) and the
  // usual search runs. In check mode the usual search runs and the result of each path is compared
  // (CompareDrawMarker).
  void MatchDraw() {
    vs_draw_ = nullptr;
    ps_draw_ = nullptr;
    reason_matched_ = 0;
    object_vs_ = object_ps_ = 0;
    vegetation_game_ = 0;  // without a record there is no game verdict to compare
    if (!shaders_.loaded()) {
      return;
    }
    RefreshObjects();  // a single acquire load per draw (formerly four)
    const uint32_t initiator = Register(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t type = initiator & 0x3F;
    const uint32_t count = initiator >> 16;
    const uint32_t mode_marker = draw_marker_mode_;
    bool marker_valid = false;
    if (mode_marker != 0) {
      marker_valid = draw_marker_.args[0] == type && CountOfRegister(draw_marker_) == count &&
                      ShadersCoherent(draw_marker_);
      if (!marker_valid) {
        ++registers_of_marker_rejected_;
      } else {
        draw_marker_mode_ = 0;  // accepted: valid for this draw and no other
        if (mode_marker == kDrawApply) {
          ++draws_with_register_of_marker_;
          UseRegisterOfDraw(draw_marker_);
          return;
        }
      }
    }
    RegisterDraw reg_entry;
    while (pending_.size() < 64 && TakeDraw(reg_entry)) {
      pending_.push_back(reg_entry);
    }
    size_t found = SIZE_MAX;
    bool incoherent = false;
    // The size of a deque is not a field, it is computed by subtracting iterators. It was
    // queried on every loop iteration, and there are ~2,200 draws per frame.
    const size_t candidates = std::min<size_t>(pending_.size(), 8);
    for (size_t i = 0; i < candidates; ++i) {
      if (pending_[i].args[0] != type || CountOfRegister(pending_[i]) != count) {
        continue;
      }
      // Type and count are not enough: in a series of identical draws, a record
      // without a draw in the ring shifts the matching. The record's VS and PS
      // must be those of the last IM_LOAD packets (observed: VS n15 matched with
      // another shader's microcode).
      if (!ShadersCoherent(pending_[i])) {
        ++candidates_incoherent_;
        incoherent = true;
        continue;
      }
      found = i;
      break;
    }
    if (mode_marker == kDrawCheck && marker_valid) {
      CompareDrawMarker(found == SIZE_MAX ? nullptr : &pending_[found], type, count);
    }
    if (found == SIZE_MAX) {
      ++draws_without_register_;
      draws_incoherent_ += incoherent;
      reason_matched_ = incoherent ? 2 : 1;
      if (warnings_c5b_ < 16) {
        ++warnings_c5b_;
        std::string first_2;
        if (!pending_.empty()) {
          const RegisterDraw& r = pending_.front();
          first_2 = fmt::format("; the first pending is function {} type {} count {}",
                                int(r.function), r.args[0], CountOfRegister(r));
        }
        REXLOG_WARN("[native] C5b: ring draw without a Draw* registration: type {} count {} ({} pending{})",
                    type, count, pending_.size(), first_2);
      }
      UseIdentityOfRing();
      return;
    }
    registers_skipped_ += found;
    const RegisterDraw r = pending_[found];
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(found + 1));
    UseRegisterOfDraw(r);
  }

  // The usual work with the matched record, whether it came from the queue or from the marker (phase 2b:
  // moved out of MatchDraw unchanged).
  void UseRegisterOfDraw(const RegisterDraw& r) {
    if (r.shadow) {
      CompareShadow(r.shadow);  // phase 1 of the Direct3D-level renderer
    }
    object_vs_ = r.vs;
    object_ps_ = r.ps;
    ++draws_matched_;
    vs_draw_ = ShaderOfObjectCached(r.vs);
    ps_draw_ = ShaderOfObjectCached(r.ps);
    // The game's vegetation verdict is only compared when drawing with this record's shaders; not with
    // the ring identity (UseIdentityOfRing, below) (fh1_d3d_game_vegetation).
    vegetation_game_ = IdentityForVegetation(r.vegetation, vs_draw_ != nullptr && ps_draw_ != nullptr);
    if (vs_draw_ && ps_draw_) {
      // Only when the VS or its microcode changes: it used to be one map write per draw.
      if ((vs_draw_ != mapped_vs_ || generation_vs_ != mapped_generation_) &&
          vs_by_microcode_.size() < 4096) {
        vs_by_microcode_[FingerprintVs()] = vs_draw_;
        mapped_vs_ = vs_draw_;
        mapped_generation_ = generation_vs_;
      }
    } else {
      UseIdentityOfRing();
    }
    draws_with_vs_ += vs_draw_ != nullptr;
    draws_with_ps_ += ps_draw_ != nullptr;
    if (vs_draw_ && (vs_draw_ != noted_vs_ || generation_vs_ != noted_generation_)) {
      noted_vs_ = vs_draw_;
      noted_generation_ = generation_vs_;
      NoteEntryVertices(*vs_draw_);
    }
  }

  // Phase 2b: the VS and PS that would be used to draw with this record (null: no record), without
  // changing anything: what UseRegisterOfDraw and UseIdentityOfRing would do.
  std::pair<const EntryShader*, const EntryShader*> ShadersWithRegister(const RegisterDraw* r) {
    const EntryShader* vs = r ? ShaderOfObjectCached(r->vs) : nullptr;
    const EntryShader* ps = r ? ShaderOfObjectCached(r->ps) : nullptr;
    if ((vs && ps) || !ps_actual_) {
      return {vs, ps};
    }
    const EntryShader* identity = vs_actual_;
    if (!identity) {
      const auto it = vs_by_microcode_.find(FingerprintVs());
      if (it == vs_by_microcode_.end()) {
        return {vs, ps};
      }
      identity = it->second;
    }
    return {identity, ps_actual_};
  }

  /*
   * Phase 2b: the comparison of the watching phase (and of 1 in every 1,024 afterwards). Only when the
   * draw accepts the marker's record: if it does not, the marker path ends in the same search and gives
   * the same result by construction. What decides the draw is compared: the VS and PS it would be drawn
   * with using the record from the search (or the ring identity if none is found) and using the marker's
   * record. A different record with the same shaders is only counted.
   */
  void CompareDrawMarker(const RegisterDraw* lookup, uint32_t type, uint32_t count) {
    const auto with_lookup = ShadersWithRegister(lookup);
    const auto with_marker = ShadersWithRegister(&draw_marker_);
    const RegisterDraw& m = draw_marker_;
    uint32_t que = 0;  // 0 equal; 1 different shaders; 3 search finds no record and the identity is not valid
    if (with_lookup != with_marker) {
      que = lookup ? 1u : 3u;
    } else if (!lookup || lookup->function != m.function || lookup->vs != m.vs || lookup->ps != m.ps ||
               lookup->args[1] != m.args[1] || lookup->args[2] != m.args[2] || lookup->args[3] != m.args[3]) {
      ++checks_draw_other_register_;  // another record (or the identity), same shaders
    }
    ++checks_draw_;
    if (que != 0) {
      ++checks_draw_different_;
      if (warnings_draw_marker_ < 8) {
        ++warnings_draw_marker_;
        const auto number = [](const EntryShader* e) { return e ? int(e->number) : -1; };
        REXLOG_ERROR("[native] draw registration (phase 2b): the lookup and the marker do not agree (case {}) on a "
                     "draw of type {} count {}: lookup {} (VS {:08X} PS {:08X}) -> VS n{} PS n{}; marker (VS "
                     "{:08X} PS {:08X}) -> VS n{} PS n{}. The game thread turns off phase 2b",
                     que, type, count, lookup ? "with registration" : "without registration", lookup ? lookup->vs : 0u,
                     lookup ? lookup->ps : 0u, number(with_lookup.first), number(with_lookup.second), m.vs,
                     m.ps, number(with_marker.first), number(with_marker.second));
      }
    }
    NoteCheckDraw(que == 0, que, lookup, &m);
  }

  // Phase 2b: the kWordsDraw words of the Draw* record (WriteDraw in
  // fh1_d3d_registers_native.cpp). false if the function is not a Draw* one.
  static bool ReadDrawOfMarker(const uint8_t* p, RegisterDraw& r) {
    // The low byte is the function and the 16 bits above it the game's vegetation verdict
    // (RegisterDraw::vegetation, fh1_d3d_game_vegetation); the high byte must be 0.
    const uint32_t word = rex::memory::load_and_swap<uint32_t>(p);
    const uint32_t function = word & 0xFFu;
    if (function > uint32_t(FunctionDraw::kIndexedUP) || (word >> 24) != 0) {
      return false;
    }
    r.function = FunctionDraw(function);
    r.vegetation = uint16_t(word >> 8);
    r.vs = rex::memory::load_and_swap<uint32_t>(p + 4);
    r.ps = rex::memory::load_and_swap<uint32_t>(p + 8);
    for (uint32_t i = 0; i < 4; ++i) {
      r.args[i] = rex::memory::load_and_swap<uint32_t>(p + 12 + 4 * i);
    }
    r.shadow = (uint64_t(rex::memory::load_and_swap<uint32_t>(p + 28)) << 32) |
               rex::memory::load_and_swap<uint32_t>(p + 32);
    return true;
  }

  /*
   * Phase 1 of the Direct3D-level renderer (docs/nfsmw-nx/native-renderer.md).
   * The snapshot of the device mirror taken in this draw's Draw* call, against the state the ring has
   * read from the packets right when it reaches this draw. Everything that matches is state the renderer
   * can read from the mirror without packets; anything else has to come through another path.
   */
  void CompareShadow(uint64_t sequence) {
    if (!ReadSnapshot(sequence, shadow_photo_)) {
      ++shadow_lost_;
      return;
    }
    ++shadow_draws_;
    const SnapshotMirror& f = shadow_photo_;
    for (uint32_t g = 0; g < kGroupsMirror; ++g) {
      const uint64_t m = f.mask[g];
      for (uint32_t i = 0; m && i < 64; ++i) {
        if (!((m >> (63 - i)) & 1)) {
          continue;
        }
        const uint32_t reg = f.base_register[g] + i;
        ++shadow_state_compared_;
        if (Register(reg) != f.state[g][i]) {
          ++shadow_state_different_;
          ++shadow_by_register_[reg];
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      ++shadow_fetch_compared_;
      if (Register(0x4800 + i) != f.fetch[i]) {
        ++shadow_fetch_different_;
        ++shadow_by_register_[0x4800 + i];
      }
    }
    for (uint32_t i = 0; i < 2048; ++i) {
      ++shadow_constants_compared_;
      if (Register(0x4000 + i) != f.constants[i]) {
        ++shadow_constants_different_;
        ++shadow_by_register_[0x4000 + i];
      }
    }
  }

  // Whether the VS and PS of a Draw* record are the ones the ring has loaded.
  // The PS arrives unpatched: its microcode is compared. For the patched VS, the only
  // possible check is that its fetches write to the original's registers.
  bool ShadersCoherent(const RegisterDraw& reg_entry) {
    const EntryShader* ps = ShaderOfObjectCached(reg_entry.ps);
    if (ps && ps_actual_ && ps->fingerprint != ps_actual_->fingerprint) {
      return false;
    }
    const EntryShader* vs = ShaderOfObjectCached(reg_entry.vs);
    if (!vs) {
      return true;
    }
    if (vs != coherence_vs_ || generation_vs_ != coherence_generation_) {
      coherence_vs_ = vs;
      coherence_generation_ = generation_vs_;
      coherence_ = FetchCoherent(*vs, vs_microcode_);
    }
    return coherence_;
  }

  // No usable Draw* record (in the menu, the final composite has no record of its own
  // and the ring carries VS n111 and PS n19): the PS of the last IM_LOAD, which arrives
  // unpatched; the VS of the IM_LOAD if it was fully identified, and otherwise the one
  // already seen matched with this same patched microcode.
  void UseIdentityOfRing() {
    if (!ps_actual_) {
      return;
    }
    const EntryShader* vs = vs_actual_;
    if (!vs) {
      const auto it = vs_by_microcode_.find(FingerprintVs());
      if (it == vs_by_microcode_.end()) {
        return;
      }
      vs = it->second;
    }
    vs_draw_ = vs;
    ps_draw_ = ps_actual_;
    ++draws_by_im_load_;
  }

  uint64_t FingerprintVs() {
    if (fingerprint_vs_generation_ != generation_vs_) {
      fingerprint_vs_generation_ = generation_vs_;
      fingerprint_vs_ = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    }
    return fingerprint_vs_;
  }

  static uint32_t CountOfRegister(const RegisterDraw& r) {
    switch (r.function) {
      case FunctionDraw::kVertices:
        return r.args[2];  // r6: vertex count
      case FunctionDraw::kIndexed:
        return r.args[3];  // r7: index count
      case FunctionDraw::kVerticesUP:
        return r.args[1];  // r5: vertex count
      case FunctionDraw::kIndexedUP:
      default:
        return r.args[3];  // r7: index count (unconfirmed)
    }
  }

  /*
   * The object generation used to be read four times per draw (twice in ShadersCoherent and
   * twice in MatchDraw), and it is an atomic load with acquire: on the Switch's A57 that
   * is an ldar, which drains the load buffer. It is now read once, on entering the matching;
   * everything after that in the draw (including the trace) already has it.
   */
  void RefreshObjects() {
    const uint64_t generation = GenerationObjects();
    if (generation != generation_objects_) {
      objects_.clear();
      memo_objects_.fill(MemoObject{});
      generation_objects_ = generation;
    }
  }

  const EntryShader* ShaderOfObjectCached(uint32_t object) {
    /*
     * A 16-slot direct-mapped cache in front of the map: the draw's VS and PS are looked up several
     * times (candidate coherence and matching), and between consecutive draws they are almost
     * always the same. It saves the hash and the cache miss of the unordered_map, which with
     * ~2,200 draws per frame were ~9,000 lookups. RefreshObjects has already validated the
     * generation: when it changes, both caches are cleared together.
     */
    MemoObject& memo = memo_objects_[size_t((object * UINT32_C(2654435761)) >> 28)];
    if (memo.object == object) {
      return memo.entry;
    }
    const EntryShader* entry;
    if (const auto it = objects_.find(object); it != objects_.end()) {
      entry = it->second;
    } else {
      entry = ShaderOfObject(object);
      objects_.emplace(object, entry);
    }
    memo.object = object;
    memo.entry = entry;
    return entry;
  }

  // Vertex input of the patched VS. Each element of the container fetches into
  // a temporary register; the ring microcode is searched, among the fetch
  // instructions, for the one that writes to that register (D3D reorders them),
  // and it yields the fetch constant, format, stride and offset. Logged once per
  // variant, for validation.
  void NoteEntryVertices(const EntryShader& vs) {
    if (vs_microcode_.size() != vs.microcode.size()) {
      ++variants_length_different_;
      return;
    }
    if (variants_views_.size() >= 256) {
      return;
    }
    const uint64_t key = (uint64_t(vs.number) << 48) ^ FingerprintVs();  // the same XXH3, already computed
    if (!variants_views_.insert(key).second) {
      return;
    }
    std::string detail;
    for (const ElementVertex& element : vs.elements) {
      const uint32_t reg_entry = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      size_t p = SIZE_MAX;
      for (const ElementVertex& other : vs.elements) {
        const size_t q = size_t(other.instruction) * 3;
        if (((vs_microcode_[q] >> 12) & 0x3F) == reg_entry && (vs_microcode_[q] & 0x1F) == 0) {
          p = q;
          break;
        }
      }
      if (p == SIZE_MAX) {
        detail += fmt::format(" {}{}:?", NameUse(element.use), element.index_use);
        ++elements_without_fetch_;
        continue;
      }
      const uint32_t d0 = vs_microcode_[p], d1 = vs_microcode_[p + 1], d2 = vs_microcode_[p + 2];
      // exp_adjust (signed bits 24-29) and sign mode (bit 14): the emulated path multiplies the
      // value by 2^exp_adjust (spirv_translator_fetch.cpp:445-449); this path does not.
      const int32_t exponent = int32_t(d1 << 2) >> 26;
      detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}/s{:03X}>{:03X}{}{}{}{}", NameUse(element.use),
                             element.index_use, ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3),
                             (d1 >> 16) & 0x3F, d2 & 0xFF, int32_t(d2 << 1) >> 9,
                             vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF,
                             d1 & 0xFFF, ((d1 >> 30) & 0x1) ? "/mini" : "",
                             ((d1 >> 12) & 0x1) ? "/sign" : "",
                             exponent ? fmt::format("/exp{}", exponent) : std::string(),
                             ((d1 >> 14) & 0x1) ? "/rf1" : "");
    }
    if (variants_views_.size() <= 24) {  // was 200; the ring thread writes it
      REXLOG_INFO("[native] C5b: VS n{} (variant {:016X}): vertex input{}", vs.number,
                  key, detail);
    }
  }

  // Fetch constant 0, which VdSwap writes right before the Swap packet.
  TextureSwap TextureOfSwap() const {
    TextureSwap texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = Register(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    return texture;
  }

  void BufferIndirect(uint32_t address, uint32_t words, int depth) {
    AddOnlyRing(indirect_);
    Reader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.fin = words;
    while (reader.Pending()) {
      if (!Packet(reader, depth)) {
        break;
      }
    }
  }

  // --- Test presentation -----------------------------------------------------

  void Present() {
    const uint64_t swap = swaps_.fetch_add(1, std::memory_order_relaxed) + 1;
    g_swaps_native.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) {
      return;
    }
    // C2: the game image, if this Swap has a resolved texture.
    if (targets_) {
      // The gamma ramp, if the game has changed it (same thread as NoteRampGamma).
      if (version_ramp_sent_ != version_ramp_) {
        version_ramp_sent_ = version_ramp_;
        targets_->RampGamma(ramp_gamma_);
      }
      const auto start = Clock::now();
      const bool presented_2 = targets_->Present(presenter_.get(), TextureOfSwap(),
                                                   swap_width_.load(std::memory_order_relaxed),
                                                   swap_height_.load(std::memory_order_relaxed));
      time_present_ns_ += NanosecondsSince(start);
      ++presentations_measurements_;
      if (presented_2) {
        return;
      }
    }
    presenter_->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
        [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& context_id) {
          return CleanOutput(static_cast<ContextOutput&>(context_id), swap);
        });
  }

  bool CleanOutput(ContextOutput& context_id, uint64_t swap) {
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const auto& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      reserve.commandPool = pool_;
      reserve.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      reserve.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &reserve, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo info_fence{};
      info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &info_fence, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // A single command buffer: wait for the previous one before reusing it.
    if (fence_pending_) {
      WaitFenceWatched(dfn, device, fence_, "single command buffer");
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context_id.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context_id.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context_id.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &start) != VK_SUCCESS) {
      return false;
    }
    // A Swap without a game image (the first frames of boot): black, as the console shows. With
    // fh1_native_swap_test_color the old test color is painted instead: green goes up and down with each Swap,
    // so a capture tells a running game from a stalled one.
    VkClearValue color{};
    color.color.float32[3] = 1.0f;
    if (REXCVAR_GET(fh1_native_swap_test_color)) {
      const float phase = float(swap % 240) / 239.0f;
      color.color.float32[0] = 0.05f;
      color.color.float32[1] = 0.10f + 0.40f * phase;
      color.color.float32[2] = 0.35f;
    }
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submission{};
    submission.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands_;
    {
      const auto queue = vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submission, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pending_ = true;
    context_id.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    targets_.reset();  // uses the device: before everything else
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const auto& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) {
        WaitFenceWatched(dfn, device, fence_, "single command buffer");
        fence_pending_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // Report lines go to the report thread (FH1_REPORT_RING, see ReportDeferred), except the
  // shutdown ones (force), which are written right here so they are not lost.
#define FH1_REPORT_BY(defer, ...) do { if (defer) { FH1_REPORT_RING(__VA_ARGS__); } else { REXLOG_INFO(__VA_ARGS__); } } while (0)
  void Report(bool force) {
    const auto now = Clock::now();
    if (!force &&
        now - last_report_ < std::chrono::seconds(REXCVAR_GET(fh1_native_report_s))) {
      return;
    }
    const bool defer_report = !force;
    last_report_ = now;
    FH1_REPORT_BY(defer_report,
        "[native] swaps={} vblanks={} packets={} interrupts={} indirect={} writes_memory={} waits_exhausted={} "
        "draws={} copies={}",
        swaps_.load(), vblanks_.load(), packets_.load(), interrupts_.load(),
        indirect_.load(), writes_memory_.load(), waits_exhausted_.load(), draws_.load(),
        copies_.load());
    // Ring thread cost since the previous report. "ring" includes the
    // WAIT_REG_MEM waits and the wait for the GPU when presenting.
    const double seconds = std::chrono::duration<double>(now - last_time_report_).count();
    last_time_report_ = now;
    if (seconds > 0.0) {
      FH1_REPORT_BY(defer_report,
          "[native] times: ring {:.1f} ms/s; draws {:.1f} us/draw ({}); copies {:.1f} us/copy; present {:.2f} "
          "ms/Swap including the GPU wait",
          double(time_ring_ns_) / 1e6 / seconds,
          draws_measured_ ? double(time_draws_ns_) / 1e3 / double(draws_measured_) : 0.0,
          draws_measured_,
          copies_measurements_ ? double(time_copies_ns_) / 1e3 / double(copies_measurements_) : 0.0,
          presentations_measurements_
              ? double(time_present_ns_) / 1e6 / double(presentations_measurements_)
              : 0.0);
      // Ring thread wakeups: how many CP_RB_WPTR writes the game makes and how many iterations this
      // thread runs to serve them.
      {
        const uint64_t writes = writes_wptr_.load(std::memory_order_relaxed);
        const uint64_t d_writes = writes - writes_wptr_previous_;
        const uint64_t d_laps = laps_ring_ - laps_ring_previous_;
        const uint64_t d_with_data = laps_ring_with_data_ - laps_ring_with_data_previous_;
        const uint64_t d_exhausted = waits_ring_exhausted_ - waits_ring_exhausted_previous_;
        /*
         * Core migrations go on this same line on purpose, next to the timed-out waits: both
         * measure the same thing (the ring not working when it should) and are best read
         * together. If fh1_native_ring_core at -1 and at 1 gives the same migrations/s,
         * the preferred core has no effect here.
         */
        const uint64_t d_migrations = migrations_ring_ - migrations_ring_previous_;
        FH1_REPORT_BY(defer_report, "[native] ring wakeups: {:.0f} CP_RB_WPTR writes/s, {:.0f} laps/s, {:.0f} "
                                     "with data/s ({:.2f} writes per lap with data), {:.0f} 4 ms waits "
                                     "exhausted/s; core {} with {:.0f} migrations/s ({:.2f} per lap)",
                    double(d_writes) / seconds, double(d_laps) / seconds, double(d_with_data) / seconds,
                    d_with_data ? double(d_writes) / double(d_with_data) : 0.0, double(d_exhausted) / seconds,
                    core_ring_, double(d_migrations) / seconds,
                    d_laps ? double(d_migrations) / double(d_laps) : 0.0);
        migrations_ring_previous_ = migrations_ring_;
        writes_wptr_previous_ = writes;
        laps_ring_previous_ = laps_ring_;
        laps_ring_with_data_previous_ = laps_ring_with_data_;
        waits_ring_exhausted_previous_ = waits_ring_exhausted_;
      }
      // Breakdown per packet, largest first (top 8).
      std::vector<std::tuple<uint64_t, int, uint64_t>> parts;
      if (time_registers_ns_) {
        parts.emplace_back(time_registers_ns_, -1, count_registers_);
      }
      for (int op = 0; op < int(time_opcode_ns_.size()); ++op) {
        if (time_opcode_ns_[op]) {
          parts.emplace_back(time_opcode_ns_[op], op, count_opcode_[op]);
        }
      }
      std::sort(parts.begin(), parts.end(), std::greater<>());
      std::string split;
      for (size_t i = 0; i < parts.size() && i < 8; ++i) {
        const auto& [ns, op, n] = parts[i];
        split += op < 0 ? fmt::format(" registers {:.1f} ms/s ({})", double(ns) / 1e6 / seconds, n)
                          : fmt::format(" op {:02X} {:.1f} ms/s ({})", op,
                                        double(ns) / 1e6 / seconds, n);
      }
      FH1_REPORT_BY(defer_report, "[native] times per packet:{}", split);
      // How many register words take the block path and how many go one at a time.
      FH1_REPORT_BY(defer_report, "[native] registers by path: {} words in blocks, {} one by one ({:.1f} words "
                                   "per type 0/1 packet)",
                  words_block_, words_loose_,
                  count_registers_ ? double(words_block_ + words_loose_) / double(count_registers_) : 0.0);
      words_block_ = words_loose_ = 0;
      // Phase 2 of the Direct3D-level renderer (ProcessMarker). Its time shows up as "op 10".
      if (markers_applied_ || markers_checked_ || markers_bad_) {
        FH1_REPORT_BY(defer_report, "[native] D3D markers (phase 2): {} applied ({:.1f} ranges and {:.1f} words "
                                     "each; {} ranges by the usual path), {} checked against their packets ({} "
                                     "different), {} malformed; fast path {} ({} ranges checked)",
                    markers_applied_,
                    markers_applied_ ? double(marker_ranges_) / double(markers_applied_) : 0.0,
                    markers_applied_ ? double(marker_words_) / double(markers_applied_) : 0.0,
                    marker_ranges_slow_, markers_checked_, markers_different_, markers_bad_,
                    marker_fast_ > 0 ? "on" : "OFF", ranges_fast_verified_);
        markers_applied_ = markers_checked_ = markers_different_ = markers_bad_ = 0;
        marker_ranges_ = marker_ranges_slow_ = marker_words_ = 0;
      }
      // Phase 2b: the draw record in the marker (MatchDraw).
      if (draws_with_register_of_marker_ || registers_of_marker_rejected_ || checks_draw_) {
        FH1_REPORT_BY(defer_report, "[native] draw registration in the marker (phase 2b): {} draws without "
                                     "lookup, {} marker registrations not accepted (type, count or shaders: looked "
                                     "up as always), {} checked against the lookup ({} different, {} with another "
                                     "registration and the same shaders)",
                    draws_with_register_of_marker_, registers_of_marker_rejected_, checks_draw_,
                    checks_draw_different_, checks_draw_other_register_);
        draws_with_register_of_marker_ = registers_of_marker_rejected_ = 0;
        checks_draw_ = checks_draw_different_ = checks_draw_other_register_ = 0;
      }
      // Phase 1 of the Direct3D-level renderer.
      if (shadow_draws_ || shadow_lost_) {
        std::vector<std::pair<uint64_t, uint32_t>> worst;
        for (const auto& [reg, n] : shadow_by_register_) {
          worst.emplace_back(n, reg);
        }
        std::sort(worst.begin(), worst.end(), std::greater<>());
        std::string list;
        for (size_t i = 0; i < worst.size() && i < 12; ++i) {
          list += fmt::format(" 0x{:04X}x{}", worst[i].second, worst[i].first);
        }
        size_t groups_seen = 0;
        for (uint32_t g = 0; g < kGroupsMirror; ++g) {
          groups_seen += shadow_photo_.mask[g] != 0;
        }
        FH1_REPORT_BY(defer_report, "[native] D3D shadow: {} draws compared ({} snapshots lost; {} of {} mirror "
                                     "groups learned) | state {} different of {} | fetch {} of {} | constants {} "
                                     "of {} | registers with differences: {}{}",
                    shadow_draws_, shadow_lost_, groups_seen, kGroupsMirror, shadow_state_different_,
                    shadow_state_compared_, shadow_fetch_different_, shadow_fetch_compared_,
                    shadow_constants_different_, shadow_constants_compared_, worst.size(),
                    list.empty() ? "" : " (the most:" + list + ")");
        shadow_draws_ = shadow_lost_ = 0;
        shadow_state_compared_ = shadow_state_different_ = 0;
        shadow_fetch_compared_ = shadow_fetch_different_ = 0;
        shadow_constants_compared_ = shadow_constants_different_ = 0;
        shadow_by_register_.clear();
      }
      // What WAIT_REG_MEM waits for: the 4 with the most 1 ms iterations.
      std::vector<std::pair<uint64_t, Wait>> waits(waits_.begin(), waits_.end());
      std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) {
        return a.second.laps > b.second.laps;
      });
      std::string detail_waits;
      for (size_t i = 0; i < waits.size() && i < 4; ++i) {
        const auto& [key, e] = waits[i];
        detail_waits += fmt::format(
            " {} {:08X} (function {} ref {:08X} mask {:08X}): {} waits, {} laps;",
            (key >> 32) ? "memory" : "register", uint32_t(key), e.info & 0x7, e.reference,
            e.mask, e.times, e.laps);
      }
      if (!detail_waits.empty()) {
        FH1_REPORT_BY(defer_report, "[native] WAIT_REG_MEM:{}", detail_waits);
      }
    }
    time_ring_ns_ = time_draws_ns_ = time_copies_ns_ = time_present_ns_ = 0;
    draws_measured_ = copies_measurements_ = presentations_measurements_ = 0;
    time_registers_ns_ = count_registers_ = 0;
    time_opcode_ns_.fill(0);
    count_opcode_.fill(0);
    waits_.clear();
    const uint32_t frontbuffer = swap_frontbuffer_.load();
    const uint32_t target = last_copy_target_.load();
    FH1_REPORT_BY(defer_report,
        "[native] last swap: frontbuffer={:08X} {}x{}; last copy: target={:08X} control={:08X} info={:08X} "
        "pitch={:08X}; match={}",
        frontbuffer, swap_width_.load(), swap_height_.load(), target, last_copy_control_.load(),
        last_copy_info_.load(), last_copy_pitch_.load(),
        (frontbuffer & 0x1FFFFFFF) == (target & 0x1FFFFFFF));
    if (targets_) {
      uint64_t copies = 0, clears = 0, presented = 0, rejections = 0;
      targets_->Statistics(copies, clears, presented, rejections);
      uint64_t gpu_ns = 0, gpu_jobs = 0;
      targets_->TimeGpu(gpu_ns, gpu_jobs);
      const uint64_t presented_interval = presented - presented_previous_gpu_;
      FH1_REPORT_BY(defer_report, "[native] C2: copies={} (depth {}) clears={} (depth {}) presented={} "
                                   "rejections={}; GPU {:.2f} ms per Swap ({} jobs measured)",
                  copies, targets_->CopiesDepth(), clears,
                  targets_->ClearsDepth(), presented, rejections,
                  presented_interval
                      ? double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval)
                      : 0.0,
                  gpu_jobs - gpu_jobs_previous_);
      // Occlusion queries since the previous report.
      {
        const auto d = [&](size_t i) { return occlusion_counters_[i] - occlusion_counters_previous_[i]; };
        uint64_t oc[5] = {};
        targets_->StatisticsOcclusion(oc);
        if (d(0) || d(1) || d(2) || d(4)) {
          FH1_REPORT_BY(defer_report, "[native] C2 occlusion: mode {}; game queries started {}, finished with a "
                                       "measurement {} and not measured yet {}, without their end {}, faked "
                                       "without a partner {}; measured written: average {:.1f}, historic maximum "
                                       "{}; finished by target: small (scale 1) {} (maximum {}), scene (scale of "
                                       "the AA mode) {} (maximum {}); host: ranges {} (no room {}), queries "
                                       "published {}, samples {} (historic maximum per query {})",
                      REXCVAR_GET(fh1_native_occlusion), d(0), d(1), d(2), d(3), d(4),
                      d(1) ? double(d(5)) / double(d(1)) : 0.0, occlusion_counters_[6], occlusion_by_target_[0],
                      occlusion_max_by_target_[0], occlusion_by_target_[1], occlusion_max_by_target_[1],
                      oc[0] - occlusion_host_previous_[0], oc[1] - occlusion_host_previous_[1],
                      oc[2] - occlusion_host_previous_[2], oc[3] - occlusion_host_previous_[3], oc[4]);
        }
        occlusion_by_target_ = {};
        occlusion_counters_previous_ = occlusion_counters_;
        std::copy(std::begin(oc), std::end(oc), occlusion_host_previous_);
      }
      {
        uint64_t waits = 0, ns_waits = 0;
        targets_->WaitsGpu(waits, ns_waits);
        const double ms_waits = double(ns_waits - ns_waits_gpu_previous_) / 1e6;
        FH1_REPORT_BY(defer_report, "[native] C2: ring waits for the GPU: {} ({:.1f} ms; {:.2f} ms per Swap)",
                    waits - waits_gpu_previous_, ms_waits,
                    presented_interval ? ms_waits / double(presented_interval) : 0.0);
        waits_gpu_previous_ = waits;
        ns_waits_gpu_previous_ = ns_waits;
        // And the real duration of the jobs, to check it against the sum of the timestamps.
        uint64_t jobs = 0, ns_jobs = 0;
        targets_->DurationJobsGpu(jobs, ns_jobs);
        const uint64_t d_jobs = jobs - jobs_gpu_previous_;
        const double ms_jobs = double(ns_jobs - ns_work_gpu_previous_) / 1e6;
        FH1_REPORT_BY(defer_report, "[native] C2: GPU jobs by wall clock: {} ({:.1f} ms; {:.2f} ms each; {:.2f} "
                                     "ms per Swap)",
                    d_jobs, ms_jobs, d_jobs ? ms_jobs / double(d_jobs) : 0.0,
                    presented_interval ? ms_jobs / double(presented_interval) : 0.0);
        jobs_gpu_previous_ = jobs;
        ns_work_gpu_previous_ = ns_jobs;
      }
      {
        uint64_t cost[6] = {};
        targets_->CostRecord(cost);
        uint64_t dc[6] = {};
        for (size_t i = 0; i < 6; ++i) {
          dc[i] = cost[i] - cost_record_previous_[i];
          cost_record_previous_[i] = cost[i];
        }
        FH1_REPORT_BY(defer_report, "[native] C2: command buffers started {} ({:.1f} ms in Record, {:.1f} ms "
                                     "resetting pools); readbacks {} ({:.2f} M texels, {:.1f} ms writing them)",
                    dc[0], double(dc[1]) / 1e6, double(dc[2]) / 1e6, dc[3], double(dc[4]) / 1e6,
                    double(dc[5]) / 1e6);
      }
      std::array<uint64_t, kGpuCategories> categories{};
      targets_->TimeGpuByCategory(categories);
      if (presented_interval) {
        const auto ms = [&](uint32_t c) {
          return double(categories[c] - gpu_categories_previous_[c]) / 1e6 /
                 double(presented_interval);
        };
        FH1_REPORT_BY(defer_report, "[native] C2: GPU per Swap: shadows {:.2f} ms, scene {:.2f}, reflection "
                                     "{:.2f}, 320 (cube and blur) {:.2f}, small {:.2f}, copies {:.2f}, clears "
                                     "{:.2f}, rest {:.2f}, scene without depth {:.2f}, gap between jobs {:.2f}",
                    ms(kGpuShadows), ms(kGpuScene), ms(kGpuReflection), ms(kGpu320),
                    ms(kGpuSmaller), ms(kGpuCopies), ms(kGpuClears), ms(kGpuOthers),
                    ms(kGpuSceneWithoutDepth), ms(kGpuGapBetweenJobs));
      }
      gpu_categories_previous_ = categories;
      // The size of the remaining copies.
      if (presented_interval) {
        std::array<uint64_t, 4> copies_t{}, pixels_t{};
        targets_->CopiesBySize(copies_t, pixels_t);
        static constexpr const char* kBuckets[] = {"<=64x64", "<=320x320", "<=1024x1024", "larger"};
        std::string line;
        for (uint32_t c = 0; c < 4; ++c) {
          const uint64_t n = copies_t[c] - copies_bucket_previous_[c];
          const uint64_t px = pixels_t[c] - pixels_bucket_previous_[c];
          if (n) {
            line += fmt::format(" {} {:.1f} copies and {:.2f} Mpixels", kBuckets[c],
                                 double(n) / double(presented_interval),
                                 double(px) / 1e6 / double(presented_interval));
          }
        }
        if (!line.empty()) {
          FH1_REPORT_BY(defer_report, "[native] C2 copies per frame and size:{}", line);
        }
        copies_bucket_previous_ = copies_t;
        pixels_bucket_previous_ = pixels_t;
      }
      // Actual fragments and vertices of each pass, to separate per-pixel cost from geometry cost.
      // The counts do not depend on the machine: what is measured on the PC holds for the console.
      {
        std::array<uint64_t, kGpuCategories> frag{}, vert{}, prim{};
        targets_->StatisticsPipeline(frag, vert, prim);
        static constexpr const char* kTypesEstad[] = {"rest",  "shadows", "scene",  "reflection",
                                                      "cube",   "small", "copies",  "clears",
                                                      "scene without depth", "gap"};
        std::string line;
        for (uint32_t c = 0; presented_interval && c < kGpuCategories; ++c) {
          const uint64_t f = frag[c] - fragments_category_previous_[c];
          const uint64_t v = vert[c] - vertices_category_previous_[c];
          const uint64_t p = prim[c] - primitives_category_previous_[c];
          if (f || v) {
            line += fmt::format(" {} {:.2f} M fragments, {:.0f} k vertices, {:.0f} k primitives;",
                                 kTypesEstad[c], double(f) / 1e6 / double(presented_interval),
                                 double(v) / 1e3 / double(presented_interval),
                                 double(p) / 1e3 / double(presented_interval));
          }
        }
        if (!line.empty()) {
          FH1_REPORT_BY(defer_report, "[native] C2 per frame and pass type:{}", line);
        }
        fragments_category_previous_ = frag;
        vertices_category_previous_ = vert;
        primitives_category_previous_ = prim;
      }
      // Which pixel shaders fill the screen, from the diagnostic frames
      // (fh1_native_per_draw_statistics_s). Accumulated from the start, not per interval.
      {
        std::vector<uint64_t> frag_ps, dib_ps;
        uint64_t frames = 0;
        targets_->StatisticsByShader(frag_ps, dib_ps, frames);
        if (frames > frames_diagnostic_previous_) {
          frames_diagnostic_previous_ = frames;
          static constexpr const char* kTypesPs[] = {"rest",  "shadows", "scene",  "reflection",
                                                     "cube",   "small", "copies",  "clears",
                                                     "scene without depth", "gap"};
          const size_t by_category = frag_ps.size() / kGpuCategories;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            uint64_t total = 0;
            std::vector<uint32_t> order;
            for (size_t i = 0; i < by_category; ++i) {
              const size_t e = size_t(c) * by_category + i;
              total += frag_ps[e];
              if (frag_ps[e]) {
                order.push_back(uint32_t(e));
              }
            }
            uint64_t draws = 0;
            for (size_t i = 0; i < by_category; ++i) {
              draws += dib_ps[size_t(c) * by_category + i];
            }
            if (!total && !draws) {
              continue;  // that category was not measured
            }
            std::sort(order.begin(), order.end(),
                      [&](uint32_t a, uint32_t b) { return frag_ps[a] > frag_ps[b]; });
            std::string line;
            for (size_t i = 0; i < order.size() && i < 8; ++i) {
              const uint32_t e = order[i];
              const uint32_t ps = uint32_t(e % by_category);
              line += fmt::format(" PS {} {:.2f} M ({:.0f} %, {:.1f} draws)",
                                   ps ? fmt::format("n{}", ps - 1) : std::string("none"),
                                   double(frag_ps[e]) / 1e6 / double(frames),
                                   100.0 * double(frag_ps[e]) / double(total),
                                   double(dib_ps[e]) / double(frames));
            }
            FH1_REPORT_BY(defer_report, "[native] C2 fragments of {} per pixel shader ({} frames, {:.2f} M and "
                                         "{:.0f} draws per frame):{}",
                        kTypesPs[c], frames, double(total) / 1e6 / double(frames),
                        double(draws) / double(frames), line);
          }
        }
      }
      // Real scale of the GPU timestamps. Between two reports, the end timestamp of the last timed job
      // must advance as much as the clock (with a lag of one or two frames at each end). On the console
      // NVK declares 1 ns per unit and the ratio comes out at 1.628 in every interval measured, menus
      // included: the GPU ms in the reports must be multiplied by the accumulated scale.
      {
        const uint64_t mark = targets_->MarkGpuFinalNs();
        const Clock::time_point now = Clock::now();
        if (mark != 0 && mark != mark_gpu_previous_ns_) {
          if (mark_gpu_previous_ns_ != 0 && mark > mark_gpu_previous_ns_) {
            const double real_ns = double(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - clock_mark_previous_).count());
            const double advance_ns = double(mark - mark_gpu_previous_ns_);
            scale_real_ns_ += real_ns;
            scale_marks_ns_ += advance_ns;
            const double scale = scale_real_ns_ / scale_marks_ns_;
            FH1_REPORT_BY(defer_report, "[native] C2: GPU timestamp scale {:.3f} in this interval ({:.3f} "
                                         "accumulated; real time between reports / timestamp advance); real GPU "
                                         "per Swap {:.2f} ms",
                        real_ns / advance_ns, scale,
                        presented_interval
                            ? double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval) * scale
                            : 0.0);
          }
          mark_gpu_previous_ns_ = mark;
          clock_mark_previous_ = now;
        }
      }
      // Mode of the intermediate timestamps of the jobs timed in the interval.
      {
        const uint64_t precise = targets_->JobsMarksPrecise();
        FH1_REPORT_BY(defer_report, "[native] C2: precise timestamps in {} of {} jobs measured", precise - jobs_precise_previous_,
                    gpu_jobs - gpu_jobs_previous_);
        jobs_precise_previous_ = precise;
      }
      // Intervals between Swaps and GPU overlaps.
      {
        std::array<uint64_t, kBucketsSwap> buckets{};
        uint64_t overlaps = 0;
        double worst_ms = 0.0;
        targets_->IntervalsBetweenSwaps(buckets, overlaps, worst_ms);
        uint64_t total = 0;
        for (uint32_t i = 0; i < kBucketsSwap; ++i) {
          total += buckets[i] - buckets_swap_previous_[i];
        }
        if (total) {
          const auto c = [&](uint32_t i) { return buckets[i] - buckets_swap_previous_[i]; };
          FH1_REPORT_BY(defer_report, "[native] C2: intervals between Swaps: <15 ms {} | 15-18 {} | 18-25 {} | "
                                       "25-30 {} | 30-36 {} | 36-50 {} | 50-60 {} | 60-75 {} | 75-100 {} | 100-150 "
                                       "{} | >=150 {}; worst frame {:.1f} ms; overlapping jobs on the GPU {}",
                      c(0), c(1), c(2), c(3), c(4), c(5), c(6), c(7), c(8), c(9), c(10), worst_ms,
                      overlaps - overlaps_gpu_previous_);
        }
        buckets_swap_previous_ = buckets;
        fh1::guard30::Report();  // which step the 30 FPS guard was on
        {
          // How much of the game's constant dump is redundant.
          static uint64_t esc_prev = 0, sin_prev = 0, blo_prev = 0, blosin_prev = 0;
          const uint64_t esc = constants_written_ - esc_prev;
          const uint64_t sin_c = constants_without_change_ - sin_prev;
          const uint64_t blo = blocks_constants_ - blo_prev;
          const uint64_t blosin = blocks_constants_without_change_ - blosin_prev;
          esc_prev = constants_written_; sin_prev = constants_without_change_;
          blo_prev = blocks_constants_; blosin_prev = blocks_constants_without_change_;
          if (esc > 0 || blo > 0) {
            FH1_REPORT_BY(defer_report, "[native] C6 game constants: {} single writes ({} unchanged, {:.1f} %) | "
                                         "{} blocks ({} unchanged, {:.1f} %)",
                        esc, sin_c, esc ? 100.0 * double(sin_c) / double(esc) : 0.0,
                        blo, blosin, blo ? 100.0 * double(blosin) / double(blo) : 0.0);
          }
        }
        {
          /*
           * How often the fetch constants and the viewport state really change. This
           * measure tells how much the translator can skip: if changes per frame are far
           * fewer than draws per frame, the "textures" stage and the viewport/scissor
           * are almost entirely repeated work.
           */
          static uint64_t fe_prev = 0, fs_prev = 0, fg_prev = 0;
          static uint64_t ee_prev = 0, es_prev = 0, eg_prev = 0, dib_prev = 0;
          const uint64_t fe = fetch_written_ - fe_prev, fs = fetch_without_change_ - fs_prev;
          const uint64_t fg = generation_fetch_ - fg_prev;
          const uint64_t ee = framing_written_ - ee_prev, es = framing_without_change_ - es_prev;
          const uint64_t eg = generation_framing_ - eg_prev;
          // draws_measured_ was already reset above in this same report: the right count is
          // the ring's accumulated counter.
          const uint64_t dib_total = draws_.load(std::memory_order_relaxed);
          const uint64_t dib = dib_total - dib_prev;
          fe_prev = fetch_written_; fs_prev = fetch_without_change_; fg_prev = generation_fetch_;
          ee_prev = framing_written_; es_prev = framing_without_change_; eg_prev = generation_framing_;
          dib_prev = dib_total;
          if (fe > 0 || ee > 0) {
            FH1_REPORT_BY(defer_report, "[native] C6 generations: fetch {} writes ({} unchanged), {} changes "
                                         "({:.2f} per draw) | framing {} writes ({} unchanged), {} changes ({:.2f} "
                                         "per draw); draws {}",
                        fe, fs, fg, dib ? double(fg) / double(dib) : 0.0,
                        ee, es, eg, dib ? double(eg) / double(dib) : 0.0, dib);
          }
        }
        overlaps_gpu_previous_ = overlaps;
      }
      // Breakdown of the time spent in Present.
      {
        uint64_t c[12] = {};
        targets_->CostPresent(c);
        const auto d = [&](int i) { return c[i] - cost_present_previous_[i]; };
        const auto average = [&](int ns, int times) { return d(times) ? double(d(ns)) / 1e6 / double(d(times)) : 0.0; };
        if (d(0) || d(2) || d(5) || d(8)) {
          FH1_REPORT_BY(defer_report, "[native] C2: present (ms each time): wait for the previous output {:.2f} "
                                       "({}); work submission: queue lock {:.2f}, vkQueueSubmit {:.2f} ({}); "
                                       "output submission: lock {:.2f}, vkQueueSubmit {:.2f} ({}); "
                                       "RefreshGuestOutput: before the call {:.2f}, inside {:.2f}, after {:.2f} "
                                       "({})",
                      average(1, 0), d(0), average(3, 2), average(4, 2), d(2), average(6, 5), average(7, 5), d(5),
                      average(9, 8), average(10, 8), average(11, 8), d(8));
        }
        std::copy(c, c + 12, cost_present_previous_);
      }
      gpu_ns_previous_ = gpu_ns;
      gpu_jobs_previous_ = gpu_jobs;
      presented_previous_gpu_ = presented;
      const StatisticsDraws d = targets_->StatisticsOfDraws();
      std::string causes;
      for (const auto& [cause, n] : d.causes) {
        causes += fmt::format(" {}={}", cause, n);
      }
      // Per-stage cost of the draws recorded since the previous report, measured on the timed ones.
      const uint64_t recorded = d.drawn - drawn_previous_;
      const uint64_t timed = d.drawn_timed - timed_previous_;
      if (recorded && timed) {
        static constexpr const char* kStages[] = {"state",  "indices",  "textures",
                                                  "pass",    "uploads",  "pipeline",
                                                  "record",  "(of the pass stage, the pass change)",
                                                  "[of the pass change, closing the previous one]",
                                                  "[targets]", "[render pass y framebuffer]",
                                                  "[opening the pass]"};
        std::string stages;
        for (size_t i = 0; i < d.stages_ns.size(); ++i) {
          stages += fmt::format(" {} {:.1f}", kStages[i],
                                double(d.stages_ns[i] - stages_previous_[i]) / 1e3 / double(timed));
        }
        /*
         * The divisor is the number of recorded draws, not the number of incoming draws.
         * `drawn_timed_` is incremented in fh1_native_draws.cpp:2440, right after
         * vkCmdDraw/vkCmdDrawIndexed: it only counts recorded draws, never rejected ones.
         * Arithmetic check on a log (1 in 8 timed): sample 107,457 and recorded 859,614, and
         * 859,614/8 = 107,452 (exact). With the incoming draws it would have been 147,852.
         *
         * In practice: to turn these us/draw into ms per frame, multiply by the recorded draws
         * (~1,395/frame), not by the incoming ones (~1,957/frame). Getting it wrong inflates every
         * stage by 40 %. Also, these samples pay for 12 clock reads, so they come out ~3 us above
         * "draws us/draw".
         */
        // The ratio comes from the counts themselves (it used to be a hard-coded 8; the stage timer
        // now runs on 1 of every 64 draws).
        FH1_REPORT_BY(defer_report, "[native] C6 stages (us per RECORDED draw, sample of {} = 1 in {:.0f} "
                                     "recorded; recorded {}):{}",
                    timed, double(recorded) / double(timed), recorded, stages);
      }
      // The scenery LOD hook. If "forced" does not resemble "that the game draws", the hook is
      // not reaching the objects and the popping will remain (this has happened before).
      FH1_REPORT_BY(defer_report, "[native] C6 {}", fh1::scenery_lod::Summary());
      // The early vegetation rejection has its own counter. Without this line there is no way to
      // check whether the shortcut works (the number would have to be deduced by subtracting
      // recorded draws from incoming ones).
      {
        const uint64_t soon = d.vegetation_soon - vegetation_soon_previous_;
        vegetation_soon_previous_ = d.vegetation_soon;
        if (presented_interval) {
          FH1_REPORT_BY(defer_report, "[native] C6 shadow vegetation dropped early: {:.0f} per frame ({} in the "
                                       "interval); saves the indices, textures, upload and pass of each one",
                      double(soon) / double(presented_interval), soon);
        }
      }
      {
        // How much of the scene forces shading before the depth test.
        const uint64_t con = d.scene_with_discard, sin = d.scene_without_discard;
        const uint64_t dcon = con - discard_with_previous_, dsin = sin - discard_without_previous_;
        discard_with_previous_ = con;
        discard_without_previous_ = sin;
        if (dcon + dsin) {
          FH1_REPORT_BY(defer_report, "[native] C6 early discard in the scene: {} draws allow it, {} prevent it "
                                       "(alpha test, kill or depth) = {:.0f} %",
                      dsin, dcon, 100.0 * double(dcon) / double(dcon + dsin));
        }
      }
      drawn_previous_ = d.drawn;
      timed_previous_ = d.drawn_timed;
      stages_previous_ = d.stages_ns;
      {
        const std::array<uint64_t, 20> counters = {d.passes, d.submissions_full,
                                                      d.ns_submissions_full, d.bytes_vertices,
                                                      d.bytes_indices, d.samplers,
                                                      d.samplers_cache, d.ns_passes,
                                                      d.ns_vertices, d.entries_computed,
                                                      d.ns_entries, d.entries_reused,
                                                      d.passes_by_generation, d.passes_by_target,
                                                      d.passes_resumed, d.ns_render_pass,
                                                      d.bytes_repeated_frame,
                                                      d.bytes_equal_previous, d.ns_hash_vertices,
                                                      d.texels_passes};
        // Deduplication of vertex uploads within the frame.
        const std::array<uint64_t, 3> dedupe = {d.dedupe_hits, d.dedupe_bytes,
                                                d.dedupe_collisions};
        const auto ddelta = [&](size_t i) { return dedupe[i] - dedupe_previous_[i]; };
        const auto delta = [&](size_t i) { return counters[i] - counters_previous_[i]; };
        FH1_REPORT_BY(defer_report, "[native] C6 counters: passes {} submissions because the upload was full {} "
                                     "({:.1f} ms) vertices {:.1f} MB indices {:.1f} MB samplers {} (cache {}); "
                                     "{:.1f} ms in pass changes ({:.1f} us each), {:.1f} ms copying vertices",
                    delta(0), delta(1), double(delta(2)) / 1e6, double(delta(3)) / 1048576.0,
                    double(delta(4)) / 1048576.0, delta(5), delta(6), double(delta(7)) / 1e6,
                    delta(0) ? double(delta(7)) / 1e3 / double(delta(0)) : 0.0,
                    timed ? double(delta(8)) / 1e6 * double(recorded) / double(timed) : 0.0);
        if (presented_interval) {
          FH1_REPORT_BY(defer_report, "[native] C6 unrepeated vertices: {} bindings reused, {:.2f} MB NOT copied "
                                       "({:.2f} MB per frame, {:.0f} % of the uploaded){}",
                      ddelta(0), double(ddelta(1)) / 1048576.0,
                      double(ddelta(1)) / 1048576.0 / double(presented_interval),
                      delta(3) + ddelta(1)
                          ? 100.0 * double(ddelta(1)) / double(delta(3) + ddelta(1))
                          : 0.0,
                      ddelta(2) ? fmt::format("; *** {} COLLISIONS: the table is too small ***",
                                              ddelta(2))
                                : "");
        }
        dedupe_previous_ = dedupe;
        FH1_REPORT_BY(defer_report, "[native] C6 counters: vertex inputs computed {} ({:.1f} ms, {:.1f} us each), "
                                     "reused {}",
                    delta(9), double(delta(10)) / 1e6,
                    delta(9) ? double(delta(10)) / 1e3 / double(delta(9)) : 0.0, delta(11));
        FH1_REPORT_BY(defer_report, "[native] C6 passes: per new command buffer {}, per targets {}, resumed after "
                                     "a copy or clear {}; {:.1f} ms in vkCmdBegin/EndRenderPass; open area {:.1f} "
                                     "Mtexels ({:.2f} Mtexels per frame)",
                    delta(12), delta(13), delta(14), double(delta(15)) / 1e6,
                    double(delta(19)) / 1e6,
                    presented_interval ? double(delta(19)) / 1e6 / double(presented_interval)
                                          : 0.0);
        if (presented_interval) {
          static constexpr const char* kTypes[] = {"rest",  "shadows", "scene",  "reflection",
                                                   "cube",   "small", "copies",  "clears",
                                                   "scene without depth", "gap"};
          std::string area;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            const uint64_t t = d.texels_by_category[c] - texels_category_previous_[c];
            const uint64_t n = d.passes_by_category[c] - passes_category_previous_[c];
            if (!t && !n) {
              continue;
            }
            const uint64_t dib = d.draws_by_category[c] - draws_category_previous_[c];
            const uint64_t tri = d.triangles_by_category[c] - triangles_category_previous_[c];
            area += fmt::format(" {} {:.2f} Mtexels in {:.1f} passes ({:.0f} draws, {:.0f} k triangles)",
                                kTypes[c], double(t) / 1e6 / double(presented_interval),
                                double(n) / double(presented_interval),
                                double(dib) / double(presented_interval),
                                double(tri) / 1e3 / double(presented_interval));
          }
          FH1_REPORT_BY(defer_report, "[native] C6 open area per frame and target type:{}", area);
        }
        if (presented_interval) {
          // Draws without color, with and without a required pixel shader.
          FH1_REPORT_BY(defer_report, "[native] C6 colorless draws per frame: {:.0f} with an unneeded pixel "
                                       "shader, {:.0f} that need it (alpha test, kill or depth); in the shadow map "
                                       "{:.0f} with the alpha test set and {:.0f} without it",
                      double(d.draws_ps_useless - draws_ps_useless_previous_) /
                          double(presented_interval),
                      double(d.draws_ps_needed - draws_ps_needed_previous_) /
                          double(presented_interval),
                      double(d.shadows_alpha_active - shadows_alpha_active_previous_) /
                          double(presented_interval),
                      double(d.shadows_alpha_off - shadows_alpha_off_previous_) /
                          double(presented_interval));
        }
        shadows_alpha_active_previous_ = d.shadows_alpha_active;
        shadows_alpha_off_previous_ = d.shadows_alpha_off;
        draws_ps_useless_previous_ = d.draws_ps_useless;
        draws_ps_needed_previous_ = d.draws_ps_needed;
        texels_category_previous_ = d.texels_by_category;
        passes_category_previous_ = d.passes_by_category;
        draws_category_previous_ = d.draws_by_category;
        triangles_category_previous_ = d.triangles_by_category;
        // Which constants mode each interval used (test cvar fh1_native_constants_ubo_toggle_s).
        FH1_REPORT_BY(defer_report, "[native] C6 constants by UBO: {} of {} submissions", d.submissions_ubo - submissions_ubo_previous_,
                    d.submissions - submissions_previous_);
        {
          const uint64_t looked = d.shared_looked - shared_looked_previous_;
          const uint64_t changed = d.shared_changed - shared_changed_previous_;
          shared_looked_previous_ = d.shared_looked;
          shared_changed_previous_ = d.shared_changed;
          if (looked > 0) {
            FH1_REPORT_BY(defer_report, "[native] C6 shared constants: {} of {} draws change the 488 bytes "
                                         "({:.1f} %); the others only pay the memcmp",
                        changed, looked, 100.0 * double(changed) / double(looked));
          }
        }
        submissions_ubo_previous_ = d.submissions_ubo;
        submissions_previous_ = d.submissions;
        // Diagnostic: whether the game has touched the gamma ramp since the previous report.
        {
          uint64_t total = 0;
          for (uint64_t n : writes_ramp_) {
            total += n;
          }
          if (total != writes_ramp_previous_) {
            writes_ramp_previous_ = total;
            uint32_t different_2 = 0;
            for (uint32_t i = 0; i < 256; ++i) {
              const uint16_t identity = uint16_t(i * 0x3FF / 0xFF);
              if (ramp_gamma_[i][0] != identity || ramp_gamma_[i][1] != identity ||
                  ramp_gamma_[i][2] != identity) {
                ++different_2;
              }
            }
            const auto sample = [&](uint32_t i) {
              return fmt::format("[{}]={}/{}/{}", i, ramp_gamma_[i][0], ramp_gamma_[i][1], ramp_gamma_[i][2]);
            };
            FH1_REPORT_BY(defer_report, "[native] C2 gamma ramp: writes mode {} index {} sequential {} PWL {} 30 "
                                         "bits {} 1926 {} mask {} (value {}); entries of the 256 table different "
                                         "from identity {}; {} {} {} {} {} (identity: i*1023/255); version {}",
                        writes_ramp_[0], writes_ramp_[1], writes_ramp_[2], writes_ramp_[3],
                        writes_ramp_[4], writes_ramp_[5], writes_ramp_[6], mask_ramp_, different_2,
                        sample(0), sample(64), sample(128), sample(192), sample(255), version_ramp_);
            // The whole table, to compare the image with the Xbox 360 one outside the game.
            bool channels_equal = true;
            for (const auto& entry : ramp_gamma_) {
              channels_equal = channels_equal && entry[0] == entry[1] && entry[0] == entry[2];
            }
            static constexpr const char* kNamesChannel[3] = {"roja", "verde", "blue"};
            for (uint32_t c = 0; c < (channels_equal ? 1u : 3u); ++c) {
              std::string list;
              list.reserve(256 * 5);
              for (uint32_t i = 0; i < 256; ++i) {
                list += fmt::format("{}{}", i ? " " : "", ramp_gamma_[i][c]);
              }
              FH1_REPORT_BY(defer_report, "[native] C2 gamma ramp, table {}: {}",
                          channels_equal ? "of the three channels" : kNamesChannel[c], list);
            }
          }
        }
        if (delta(16) || delta(17)) {
          FH1_REPORT_BY(defer_report, "[native] C6 repeated vertices: {:.1f} MB copied; {:.1f} MB repeated in the "
                                       "same frame and {:.1f} MB equal to an earlier frame; {:.1f} ms in hashes",
                      double(delta(3)) / 1048576.0, double(delta(16)) / 1048576.0,
                      double(delta(17)) / 1048576.0, double(delta(18)) / 1e6);
        }
        counters_previous_ = counters;
      }
      FH1_REPORT_BY(defer_report, "[native] C6: drawn={} rejected={} pipelines={} ({} ms creating) textures={} "
                                   "({} MB) texture uploads={} MB uploaded={}; rejections by cause:{}",
                  d.drawn, d.rejected, d.pipelines, d.ms_pipelines, d.textures,
                  d.megabytes_textures, d.uploads_texture, d.megabytes_uploaded, causes);
    }
    if (shaders_.loaded()) {
      const StatisticsShaders e = shaders_.Statistics();
      FH1_REPORT_BY(defer_report, "[native] C5a: loads={} different={} identified={} without_identify={} "
                                   "ambiguous={} (FH1: {} by the tolerant pass); draws with VS and PS={} without VS={} "
                                   "without PS={}; VS/PS pairs={}",
                  e.loads + loads_cached_, e.different, e.identified, e.without_identify, e.ambiguous, e.tolerant,
                  draws_identified_, draws_without_vs_, draws_without_ps_, pairs_.size());
      // IM_LOAD without memcmp (LoadShaderCached).
      {
        const uint64_t sin_memcmp = im_i_sin_memcmp_;
        const uint64_t con_memcmp = im_i_con_memcmp_;
        FH1_REPORT_BY(defer_report,
                            "[native] C5a IM_LOAD without memcmp (build 184): phase {}; in the interval {} loads "
                            "without reading the microcode ({:.1f} %) and {} with memcmp ({} compared with the "
                            "shortcut and equal, {} because of a write to their slot, {} not noted because of a "
                            "half-done write); checked before applying {} of {}{} | writes reported since startup: "
                            "{} in-place patches, {} in the ring copy, {} others, {} creations",
                            NamePhaseImShortcut(), sin_memcmp,
                            sin_memcmp + con_memcmp
                                ? 100.0 * double(sin_memcmp) / double(sin_memcmp + con_memcmp)
                                : 0.0,
                            con_memcmp, im_i_checked_, im_i_version_changed_, im_i_cancelled_,
                            std::min(im_shortcut_checked_, kImShortcutALook), kImShortcutALook,
                            im_shortcut_phase_ == kImShortcutWatching && im_shortcut_checked_ >= kImShortcutALook
                                ? " (no in-place patch seen at all: the sub_825A2FB8 hook does not run)"
                                : "",
                            microcode::g_patches_in_its_room.load(std::memory_order_relaxed),
                            microcode::g_patches_in_copy.load(std::memory_order_relaxed),
                            microcode::g_patches_others.load(std::memory_order_relaxed),
                            microcode::g_creations.load(std::memory_order_relaxed));
        im_i_sin_memcmp_ = im_i_con_memcmp_ = im_i_checked_ = im_i_version_changed_ = im_i_cancelled_ = 0;
      }
      // IM_LOAD_IMMEDIATE with an exact cache (LoadImmediate).
      if (imm_phase_ != kImmWithoutBegin) {
        const uint64_t total =
            imm_i_hits_ + imm_i_checked_ + imm_i_misses_ + imm_i_lap_ + imm_i_without_cache_;
        uint32_t busy = 0;
        for (const auto& by_type : imm_cache_) {
          for (const auto& set : by_type) {
            for (const ImmediateLoad& via : set) {
              busy += via.signature != 0 ? 1 : 0;
            }
          }
        }
        FH1_REPORT_BY(defer_report,
                            "[native] C5a IM_LOAD_IMMEDIATE with cache (build 184): phase {}; in the interval {} "
                            "loads: {} hits without swapping or identifying ({:.1f} %), {} hits checked against "
                            "the usual path, {} misses (new paths), {} because the ring wrapped around and {} "
                            "without cache; paths in use {} of {}; hits checked since startup {}",
                            NamePhaseImmediate(), total, imm_i_hits_,
                            total ? 100.0 * double(imm_i_hits_) / double(total) : 0.0, imm_i_checked_,
                            imm_i_misses_, imm_i_lap_, imm_i_without_cache_, busy,
                            2 * kImmVias * (uint32_t(1) << kImmBitsSet), imm_checked_);
        imm_i_hits_ = imm_i_checked_ = imm_i_misses_ = imm_i_lap_ = imm_i_without_cache_ = 0;
      }
      // Measurement only: fences with pending vertex copies (NoteFenceCopies).
      FH1_REPORT_BY(defer_report,
                          "[native] C6 fences with pending vertex copies (build 185, measurement only): {} of {} "
                          "fences and interrupts in the interval (at most {} pending copies)",
                          fences_with_copies_, fences_total_, fences_copies_max_);
      fences_with_copies_ = fences_total_ = 0;
      fences_copies_max_ = 0;
      // Measurement only (NoteGameByAhead).
      FH1_REPORT_BY(defer_report,
                          "[native] C6 game ahead of the ring (build 186, measurement only): when the ring "
                          "finished a frame, the game was 0 Swaps ahead in {}, 1 in {} and 2 or more in {} (at "
                          "most {}; difference at the first frame {})",
                          game_ahead_[0], game_ahead_[1], game_ahead_[2], game_ahead_max_,
                          game_ahead_first_);
      game_ahead_[0] = game_ahead_[1] = game_ahead_[2] = 0;
      game_ahead_max_ = 0;
      const StatisticsHooks g = StatisticsOfHooks();
      FH1_REPORT_BY(defer_report, "[native] C5b: shaders created VS={} (known {}) PS={} (known {}); draws "
                                   "matched={} without registration={} registrations skipped={} lost={}; with "
                                   "VS={} with PS={}; registrations with shaders different from the IM_LOADs={} "
                                   "(draws left without registration because of that={}); draws with the ring "
                                   "identity={}; VS variants={} (different length {}, elements without fetch {})",
                  g.created_vs, g.known_vs, g.created_ps, g.known_ps,
                  draws_matched_, draws_without_register_, registers_skipped_, g.lost_count,
                  draws_with_vs_, draws_with_ps_, candidates_incoherent_,
                  draws_incoherent_, draws_by_im_load_, variants_views_.size(),
                  variants_length_different_, elements_without_fetch_);
    }
    if (!histogram_noted_ && swaps_.load() >= 60) {
      histogram_noted_ = true;
      std::string lines;
      for (uint32_t op = 0; op < opcodes_.size(); ++op) {
        const uint64_t n = opcodes_[op].load();
        if (n) {
          lines += fmt::format(" {:02X}={}", op, n);
        }
      }
      FH1_REPORT_BY(defer_report, "[native] type 3 packets by opcode (hex=count):{}", lines);
    }
  }

#undef FH1_REPORT_BY

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  bool mmio_registered_ = false;

  std::vector<uint32_t> registers_;
  std::vector<std::atomic<uint8_t>> seen_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> generation_ring_{0};
  std::atomic<uint32_t> read_returned_{0};
  std::atomic<uint32_t> pointer_write_{0};
  // Ring wakeups: the game thread counts the writes; the ring thread counts the rest.
  std::atomic<uint64_t> writes_wptr_{0};
  uint64_t writes_wptr_previous_ = 0;
  // Which core the ring thread runs on and how many times it has been moved to another one.
  int core_ring_ = -1;
  uint64_t migrations_ring_ = 0;
  uint64_t migrations_ring_previous_ = 0;
  uint64_t laps_ring_ = 0;
  uint64_t laps_ring_previous_ = 0;
  uint64_t laps_ring_with_data_ = 0;
  uint64_t laps_ring_with_data_previous_ = 0;
  uint64_t waits_ring_exhausted_ = 0;
  uint64_t waits_ring_exhausted_previous_ = 0;
  std::atomic<uint32_t> counter_{0};
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;

  std::atomic<bool> active_{false};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> thread_vblank_;
  rex::system::object_ref<rex::system::XHostThread> thread_ring_;

  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> packets_{0};
  std::atomic<uint64_t> interrupts_{0};
  uint64_t fences_total_ = 0;       // NoteFenceCopies, ring thread only
  uint64_t fences_with_copies_ = 0;
  size_t fences_copies_max_ = 0;
  uint64_t game_ahead_[3] = {};  // NoteGameByAhead, ring thread only
  int64_t game_ahead_max_ = 0;
  int64_t game_ahead_first_ = 0;
  bool game_ahead_first_valid_ = false;
  std::atomic<uint64_t> indirect_{0};
  std::atomic<uint64_t> writes_memory_{0};
  std::atomic<uint64_t> waits_exhausted_{0};
  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> copies_{0};
  std::atomic<uint32_t> last_copy_target_{0};
  std::atomic<uint32_t> last_copy_control_{0};
  std::atomic<uint32_t> last_copy_info_{0};
  std::atomic<uint32_t> last_copy_pitch_{0};
  std::atomic<uint32_t> swap_frontbuffer_{0};
  std::atomic<uint32_t> swap_width_{0};
  std::atomic<uint32_t> swap_height_{0};
  std::array<std::atomic<uint64_t>, 128> opcodes_{};
  bool histogram_noted_ = false;  // only the ring thread touches it
  // Step C2: render targets, copies and presentation (ring thread only).
  std::unique_ptr<TargetsNative> targets_;
  bool targets_failed_ = false;
  // Step C5a: identified shaders (ring thread only).
  ShadersNative shaders_;
  const EntryShader* vs_actual_ = nullptr;
  const EntryShader* ps_actual_ = nullptr;
  std::vector<uint32_t> microcode_;
  uint64_t draws_identified_ = 0;
  uint64_t draws_without_vs_ = 0;
  uint64_t draws_without_ps_ = 0;
  std::unordered_set<uint64_t> pairs_;
  // Step C5b: matching with the game's Draw* calls (ring thread only).
  std::deque<RegisterDraw> pending_;
  std::unordered_map<uint32_t, const EntryShader*> objects_;
  uint64_t generation_objects_ = UINT64_MAX;
  // direct cache in front of objects_ (ShaderOfObjectCached).
  struct MemoObject {
    uint32_t object = UINT32_MAX;  // UINT32_MAX = empty slot: not a guest object
    const EntryShader* entry = nullptr;
  };
  std::array<MemoObject, 16> memo_objects_{};
  uint64_t last_pair_shaders_ = UINT64_MAX;  // CountDrawShaders: the previous draw's pair
  std::span<const uint32_t> vs_microcode_;  // from the IM_LOAD cache or from vs_immediate_
  const EntryShader* vs_draw_ = nullptr;
  const EntryShader* ps_draw_ = nullptr;
  uint64_t draws_matched_ = 0;
  uint64_t draws_without_register_ = 0;
  uint64_t registers_skipped_ = 0;
  uint64_t draws_with_vs_ = 0;
  uint64_t draws_with_ps_ = 0;
  uint64_t candidates_incoherent_ = 0;
  uint64_t draws_incoherent_ = 0;
  const EntryShader* coherence_vs_ = nullptr;
  uint64_t coherence_generation_ = UINT64_MAX;
  bool coherence_ = false;
  const EntryShader* noted_vs_ = nullptr;
  uint64_t noted_generation_ = UINT64_MAX;
  uint32_t warnings_c5b_ = 0;
  uint64_t variants_length_different_ = 0;
  uint64_t elements_without_fetch_ = 0;
  std::unordered_set<uint64_t> variants_views_;
  // Steps C3-C6: generations, so unchanged data is not uploaded again.
  uint64_t generation_vs_ = 0;  // one per distinct VS microcode (LoadShaderCached)
  // IM_LOAD cache (LoadShaderCached): 2 types x 128 sets x 2 ways.
  struct LoadShader {
    uint32_t address_type = 0;
    uint32_t size = 0;
    std::vector<uint32_t> raw;  // as it is in game memory
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const EntryShader* entry = nullptr;
    uint64_t fingerprint = 0;      // host XXH3
    uint64_t generation = 0;  // from generations_microcode_, when this content was loaded
    uint64_t use = 0;         // the least recently used way is the one replaced
    // The versions of its slot this content was checked against guest memory with
    // (microcode::BeforeOfRead and AfterOfRead). validated = false: it must be compared with memcmp again.
    bool validated = false;
    uint32_t version_slot = 0;
    uint32_t version_global = 0;
  };
  std::array<std::array<std::array<LoadShader, 2>, 128>, 2> loads_cache_{};
  uint64_t loads_tic_ = 0;
  uint64_t loads_cached_ = 0;
  uint64_t generations_microcode_ = 0;
  // IM_LOAD without memcmp (LoadShaderCached). Ring thread only.
  static constexpr int kImShortcutWithoutBegin = -1;
  static constexpr int kImShortcutWatching = 0;
  static constexpr int kImShortcutApplying = 1;
  static constexpr int kImShortcutOff = 2;
  static constexpr uint64_t kImShortcutALook = 200000;
  static constexpr uint64_t kImShortcutCheckEvery = 4096;  // power of 2
  int im_shortcut_phase_ = kImShortcutWithoutBegin;
  uint64_t im_shortcut_turn_ = 0;
  uint64_t im_shortcut_checked_ = 0;  // agreements with the memcmp since startup
  // Since the last report.
  uint64_t im_i_sin_memcmp_ = 0;
  uint64_t im_i_con_memcmp_ = 0;
  uint64_t im_i_checked_ = 0;
  uint64_t im_i_version_changed_ = 0;
  uint64_t im_i_cancelled_ = 0;
  std::vector<uint32_t> vs_immediate_;  // VS from IM_LOAD_IMMEDIATE, uncached
  // Exact IM_LOAD_IMMEDIATE cache (LoadImmediate): 2 types x 32 sets x 8 ways. Ring thread only.
  static constexpr uint32_t kImmBitsSet = 5;
  static constexpr uint32_t kImmVias = 8;
  static constexpr int kImmWithoutBegin = -1;
  static constexpr int kImmWatching = 0;
  static constexpr int kImmApplying = 1;
  static constexpr int kImmOff = 2;
  static constexpr uint64_t kImmALook = 20000;
  static constexpr uint64_t kImmCheckEvery = 1024;  // power of 2
  std::array<std::array<std::array<ImmediateLoad, kImmVias>, (size_t(1) << kImmBitsSet)>, 2> imm_cache_{};
  std::array<std::array<uint8_t, (size_t(1) << kImmBitsSet)>, 2> imm_last_{};  // way of the last hit
  int imm_phase_ = kImmWithoutBegin;
  uint64_t imm_turn_ = 0;
  uint64_t imm_tic_ = 0;
  uint64_t imm_checked_ = 0;  // hits checked and equal since startup
  // Since the last report.
  uint64_t imm_i_hits_ = 0;
  uint64_t imm_i_checked_ = 0;
  uint64_t imm_i_misses_ = 0;
  uint64_t imm_i_lap_ = 0;
  uint64_t imm_i_without_cache_ = 0;
  const EntryShader* mapped_vs_ = nullptr;
  uint64_t mapped_generation_ = UINT64_MAX;
  // How many constant writes do not change the value. Tells whether comparing before bumping
  // the generation pays off, and by how much.
  uint64_t constants_written_ = 0;
  uint64_t constants_without_change_ = 0;
  uint64_t blocks_constants_ = 0;
  uint64_t blocks_constants_without_change_ = 0;
  uint64_t generation_constants_vs_ = 0;
  int registers_in_block_ = -1;  // -1 = cvar not read yet
  uint64_t generation_constants_ps_ = 0;
  // Generations of the fetch constants and the viewport state (viewport, scissor, clip). They
  // travel in RequestDraw so the translator does not redo per draw what has not changed.
  uint64_t generation_fetch_ = 0;
  uint64_t generation_framing_ = 0;
  uint64_t fetch_written_ = 0;
  uint64_t fetch_without_change_ = 0;
  uint64_t framing_written_ = 0;
  uint64_t framing_without_change_ = 0;
  // Ring thread times since the last report (that thread only).
  uint64_t time_ring_ns_ = 0;
  uint64_t time_draws_ns_ = 0;
  uint64_t time_copies_ns_ = 0;
  uint64_t time_present_ns_ = 0;
  uint64_t draws_measured_ = 0;
  uint64_t phase_stopwatch_draws_ = 0;  // phase of the DrawNative timer, never reset
  uint64_t copies_measurements_ = 0;
  uint64_t presentations_measurements_ = 0;
  // Ring breakdown per packet: registers (types 0 and 1) and each type-3 opcode.
  uint64_t time_registers_ns_ = 0;
  uint64_t count_registers_ = 0;
  uint64_t words_block_ = 0;   // Measurement only
  // Vector path of WriteRegistersInBlock and its guard.
  static constexpr uint64_t kBlocksAVerify = 200000;
  int registers_vector_ = -1;
  uint64_t blocks_verified_ = 0;
  std::vector<uint32_t> copy_verification_;
  // Phase 1 of the Direct3D-level renderer (CompareShadow).
  SnapshotMirror shadow_photo_;
  uint64_t shadow_draws_ = 0;
  uint64_t shadow_lost_ = 0;
  uint64_t shadow_state_compared_ = 0;
  uint64_t shadow_state_different_ = 0;
  uint64_t shadow_fetch_compared_ = 0;
  uint64_t shadow_fetch_different_ = 0;
  uint64_t shadow_constants_compared_ = 0;
  uint64_t shadow_constants_different_ = 0;
  std::unordered_map<uint32_t, uint64_t> shadow_by_register_;
  // Phase 2 of the Direct3D-level renderer (ProcessMarker). Ring thread only.
  static constexpr uint64_t kRangesFastAVerify = 200000;
  int marker_fast_ = 1;  // 0: the fast path was turned off by a mismatch (VerifyRangeMarker)
  uint64_t ranges_fast_verified_ = 0;
  std::vector<uint32_t> copy_marker_;
  std::vector<uint32_t> marker_flat_;  // a marker that wrapped around the ring, made contiguous (should not happen)
  uint64_t markers_applied_ = 0;
  uint64_t markers_checked_ = 0;
  uint64_t markers_different_ = 0;
  uint64_t markers_bad_ = 0;
  uint64_t marker_ranges_ = 0;
  uint64_t marker_ranges_slow_ = 0;  // through the regular path (WriteRegistersInBlock or one at a time)
  uint64_t marker_words_ = 0;
  uint32_t warnings_marker_ = 0;
  // Phase 2b: the Draw* record carried by the last marker, for the DRAW_INDX that follows. It stays
  // until a draw accepts it or the next Draw* record arrives, like the head of the queue.
  RegisterDraw draw_marker_;
  uint32_t draw_marker_mode_ = 0;  // 0 none, kDrawApply or kDrawCheck
  uint16_t vegetation_game_ = 0;  // kVeg* flags of the record used to draw (0: none)
  uint64_t draws_with_register_of_marker_ = 0;
  uint64_t registers_of_marker_rejected_ = 0;
  uint64_t checks_draw_ = 0;
  uint64_t checks_draw_different_ = 0;
  uint64_t checks_draw_other_register_ = 0;
  uint32_t warnings_draw_marker_ = 0;
  uint64_t words_loose_ = 0;
  uint64_t stopwatch_packets_ = 0;
  std::array<uint64_t, 128> time_opcode_ns_{};
  std::array<uint64_t, 128> count_opcode_{};
  // WAIT_REG_MEM per polled register or address since the last report.
  struct Wait {
    uint64_t times = 0;
    uint64_t laps = 0;  // failed polls, each with a 1 ms wait
    uint32_t reference = 0;
    uint32_t mask = 0;
    uint32_t info = 0;
  };
  std::unordered_map<uint64_t, Wait> waits_;
  // Draw stages at the previous report (for the deltas).
  uint64_t drawn_previous_ = 0;
  uint64_t timed_previous_ = 0;
  uint64_t vegetation_soon_previous_ = 0;
  std::array<uint64_t, kStagesDraw> stages_previous_{};
  uint64_t discard_with_previous_ = 0;
  uint64_t discard_without_previous_ = 0;
  std::array<uint64_t, 20> counters_previous_{};
  std::array<uint64_t, 3> dedupe_previous_{};  // "C6 counters"
  std::array<uint64_t, kGpuCategories> texels_category_previous_{};  // "C6 area open"
  std::array<uint64_t, kGpuCategories> passes_category_previous_{};
  uint64_t draws_ps_useless_previous_ = 0;
  uint64_t shadows_alpha_active_previous_ = 0;
  uint64_t shadows_alpha_off_previous_ = 0;
  uint64_t draws_ps_needed_previous_ = 0;
  std::array<uint64_t, kGpuCategories> draws_category_previous_{};
  std::array<uint64_t, kGpuCategories> triangles_category_previous_{};
  uint64_t submissions_ubo_previous_ = 0;  // "C6 constants by UBO"
  uint64_t shared_looked_previous_ = 0;  // "C6 shared constants"
  uint64_t shared_changed_previous_ = 0;
  std::array<uint64_t, kBucketsSwap> buckets_swap_previous_{};  // "C2: intervals between Swaps"
  uint64_t overlaps_gpu_previous_ = 0;
  uint64_t cost_present_previous_[12] = {};  // "C2: present"
  uint64_t submissions_previous_ = 0;
  // Gamma ramp loaded by the game (NoteRampGamma), identity at startup. The output applies it.
  std::array<std::array<uint16_t, 3>, 256> ramp_gamma_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  // FH1: the piecewise-linear ramp as the game writes it (see NoteRampGamma).
  struct RampPwl {
    uint16_t base = 0;
    uint16_t delta = 0;
  };
  std::array<std::array<RampPwl, 3>, 128> ramp_pwl_{};
  bool ramp_pwl_dirty_ = false;
  uint32_t component_ramp_ = 0;
  uint32_t mask_ramp_ = 0b111;  // default DC_LUT_WRITE_EN_MASK (register_table.inc)
  uint64_t version_ramp_ = 0;
  uint64_t version_ramp_sent_ = 0;  // the identity is already in the output
  std::array<uint64_t, kRegRampLast - kRegRampFirst + 1> writes_ramp_{};
  uint64_t writes_ramp_previous_ = 0;
  // Occlusion queries (QueryOcclusion), ring thread only.
  uint32_t occlusion_base_ = 0;                    // structure of the open query (0 = none)
  std::unordered_set<uint32_t> bases_occlusion_;   // structures seen in an Issue(BEGIN)
  uint32_t warnings_occlusion_ = 0;                  // details of the first ones in the log
  // Finished queries per render target kind (0: under 640 or without MSAA on the Xbox 360; 1: scene,
  // scaled) in the interval, and the largest count written for each since the start.
  std::array<uint64_t, 2> occlusion_by_target_{};
  std::array<uint32_t, 2> occlusion_max_by_target_{};
  uint32_t warnings_occlusion_scene_ = 0;
  // Diagnostic fh1_native_diag_constants_ps.
  std::string constants_list_text_;
  std::unordered_set<uint32_t> constants_list_;
  std::unordered_map<uint32_t, Clock::time_point> constants_last_;
  // Test fh1_native_occlusion_toggle_s.
  bool occlusion_test_started_ = false;
  bool occlusion_test_faked_ = false;
  Clock::time_point occlusion_test_start_{};
  // Started, finished with a measurement, finished without a measurement yet, started without their
  // end, written with the faked count outside a pair, sum of the written measurements and the
  // largest one written.
  std::array<uint64_t, 7> occlusion_counters_{};
  std::array<uint64_t, 7> occlusion_counters_previous_{};
  uint64_t occlusion_host_previous_[5] = {};  // TargetsNative::StatisticsOcclusion at the previous report
  // GPU time at the previous report (C2).
  uint64_t gpu_ns_previous_ = 0;
  uint64_t waits_gpu_previous_ = 0;     // C2 report: ring waits for the GPU
  uint64_t ns_waits_gpu_previous_ = 0;
  uint64_t jobs_gpu_previous_ = 0;
  uint64_t ns_work_gpu_previous_ = 0;
  uint64_t cost_record_previous_[6] = {};  // C2 report: Record and readbacks
  uint64_t gpu_jobs_previous_ = 0;
  uint64_t presented_previous_gpu_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_categories_previous_{};
  std::array<uint64_t, kGpuCategories> fragments_category_previous_{};
  std::array<uint64_t, kGpuCategories> vertices_category_previous_{};
  std::array<uint64_t, kGpuCategories> primitives_category_previous_{};
  uint64_t frames_diagnostic_previous_ = 0;
  std::array<uint64_t, 4> copies_bucket_previous_{};
  std::array<uint64_t, 4> pixels_bucket_previous_{};
  // Real scale of the GPU timestamps (C2 report).
  uint64_t mark_gpu_previous_ns_ = 0;
  uint64_t jobs_precise_previous_ = 0;
  Clock::time_point clock_mark_previous_{};
  double scale_real_ns_ = 0.0;
  double scale_marks_ns_ = 0.0;
  Clock::time_point last_time_report_{};
  Clock::time_point last_report_{};
  // Trace of one frame (fh1_native_diag_frame_s).
  Clock::time_point start_system_{};
  bool tracing_ = false;
  bool trace_done_ = false;
  // fh1_native_diag_constants_ps is not empty. Refreshed on every Swap.
  bool diag_constants_active_ = false;
  std::unordered_set<uint32_t> textures_dumped_;  // fh1_native_diag_vertices_ps
  uint32_t traces_ = 0;
  uint32_t reason_matched_ = 0;  // 0 matched, 1 no registration, 2 inconsistent registration
  uint32_t object_vs_ = 0;           // objects of the paired record
  uint32_t object_ps_ = 0;
  // Ring identity for draws without a usable record (UseIdentityOfRing).
  std::unordered_map<uint64_t, const EntryShader*> vs_by_microcode_;
  uint64_t fingerprint_vs_ = 0;
  uint64_t fingerprint_vs_generation_ = UINT64_MAX;
  uint64_t draws_by_im_load_ = 0;

  // Vulkan objects for the test image: only the ring thread touches them.
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  std::array<Framebuffer, rex::ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      framebuffers_{};
  size_t next_framebuffer_ = 0;
};

}  // namespace

uint64_t SwapsNative() {
  return g_swaps_native.load(std::memory_order_relaxed);
}

bool Active() {
  const std::string& renderer = REXCVAR_GET(fh1_renderer);
  return renderer == "native";
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics() {
  return std::make_unique<SystemGraphicsNative>();
}

}  // namespace fh1::native
