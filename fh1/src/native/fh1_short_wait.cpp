// nfsc-recomp: short, accurate waits for the native renderer's ring thread (WAIT_REG_MEM polling).
//
// WHY (2026-10-02, Free Roam on the ROG Ally X): the ring waits about 4 times per frame for a value the game writes
// (memory 1FCA3006). It polled every 1 ms with a normal sleep, so it noticed the value up to 1 ms (often more) late,
// several times per frame; those delays turned heavy frames into 30-40 ms frames. Polling without sleeping fixed the
// frames (11 -> 3 slow frames in 4 min) but kept a whole core at 99%. A high-resolution waitable timer sleeps
// accurately for a fraction of a millisecond, so the thread both notices quickly and stays mostly idle.
#include "fh1_short_wait.h"

#include <rex/thread.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>

namespace fh1 {

void WaitShort(uint32_t microseconds) {
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  // One timer per thread (only the ring thread uses it). Windows 10 1803 and later; if creating it fails, a
  // normal sleep is used, as before.
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER expires;
    expires.QuadPart = -int64_t(microseconds) * 10;  // relative, in 100 ns units
    if (SetWaitableTimerEx(timer, &expires, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  rex::thread::Sleep(std::chrono::microseconds(microseconds));
}

}  // namespace fh1
