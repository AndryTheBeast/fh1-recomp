// nfsmw - priority, CPU time and core of the threads on the Switch (and the PC equivalent, for testing)
//
// Horizon only time-slices threads at the preemption priority (0x3B), which is the one of the game threads.
// A thread with a lower number takes the CPU from them as soon as it is ready. The game's audio server
// thread uses it (nfsmw_audio_server_priority), since it is almost always waiting for its event.
//
// CPU time used by a thread and the core the current one runs on, for the audio server's packet ring
// summaries (nfsmw_audio_diag_ring). On the Switch, from the CPU time the kernel keeps for each thread
// (InfoType_ThreadTickCount); on PC, with GetThreadTimes, which has a ~15.6 ms granularity.

#include <cstdint>

#ifdef __SWITCH__
#include <switch.h>
#endif

#if defined(_WIN32) && !defined(__SWITCH__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nfsmw::threads {

bool PriorityThreadCurrent(int priority) {
#ifdef __SWITCH__
  return R_SUCCEEDED(svcSetThreadPriority(threadGetCurHandle(), static_cast<u32>(priority)));
#elif defined(_WIN32)
  // PC (testing): there are no Horizon priorities; any requested priority raises the thread to
  // THREAD_PRIORITY_HIGHEST, above the other game threads.
  (void)priority;
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
#else
  (void)priority;
  return false;
#endif
}

// Handle of the current thread usable to read its CPU time from another thread (0 = not available). On PC it
// is a duplicate with query access: it is requested once per thread and never closed.
uint64_t HandlerThreadCurrent() {
#ifdef __SWITCH__
  return threadGetCurHandle();
#elif defined(_WIN32)
  HANDLE copy = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &copy,
                       THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
    return 0;
  }
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(copy));
#else
  return 0;
#endif
}

// Microseconds of CPU time used by the thread of that handle; -1 if it cannot be known.
int64_t CpuThreadUs(uint64_t handler) {
  if (handler == 0) {
    return -1;
  }
#ifdef __SWITCH__
  u64 ticks = 0;
  if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, static_cast<Handle>(handler), UINT64_MAX))) {
    return -1;
  }
  const u64 frequency = armGetSystemTickFreq();
  return frequency != 0 ? static_cast<int64_t>(double(ticks) * 1e6 / double(frequency)) : -1;
#elif defined(_WIN32)
  FILETIME creation{}, output{}, core{}, user{};
  if (!GetThreadTimes(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handler)), &creation, &output, &core,
                      &user)) {
    return -1;
  }
  const auto hundred_ns = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return static_cast<int64_t>((hundred_ns(core) + hundred_ns(user)) / 10);
#else
  return -1;
#endif
}

// Core the current thread is running on right now; -1 if it cannot be known.
int CoreCurrent() {
#ifdef __SWITCH__
  return static_cast<int>(svcGetCurrentProcessorNumber());
#elif defined(_WIN32)
  return static_cast<int>(GetCurrentProcessorNumber());
#else
  return -1;
#endif
}

}  // namespace nfsmw::threads
