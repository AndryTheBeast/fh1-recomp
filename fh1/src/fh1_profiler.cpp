// fh1 - built-in sampling profiler (Windows). --fh1_profile=N: about 1000 times a second, note
// which function every thread of the process is executing; every N seconds log, for the busiest
// threads, the functions that took the most samples (translated guest code shows up as
// sub_XXXXXXXX, the SDK as its C++ names). For finding where frame time goes without external
// tools. The sampler suspends each thread only for GetThreadContext and allocates nothing while a
// thread is suspended.

#if defined(_WIN32)

#include <windows.h>
// dbghelp.h and tlhelp32.h need windows.h first.
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(fh1_profile, 0, "FH1",
                     "Debug: N > 0 = sample every thread ~1000x/s and log the top functions of "
                     "the busiest threads every N seconds");
REXCVAR_DEFINE_INT32(fh1_profile_top, 25, "FH1", "Debug: functions listed per thread");

namespace {

struct Sample {
  DWORD tid;
  DWORD64 pc;
};

std::string ThreadName(DWORD tid) {
  std::string name;
  if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      for (PWSTR p = desc; *p; ++p) name += char(*p < 128 ? *p : '?');
      LocalFree(desc);
    }
    CloseHandle(h);
  }
  return name.empty() ? fmt::format("thread {}", tid) : name;
}

void ProfilerMain(int32_t interval_s) {
  HANDLE process = GetCurrentProcess();
  DWORD self = GetCurrentThreadId();
  DWORD pid = GetCurrentProcessId();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);

  std::vector<Sample> samples;
  samples.reserve(1 << 20);
  std::unordered_map<DWORD, HANDLE> handles;
  auto last_report = std::chrono::steady_clock::now();
  auto last_enum = last_report - std::chrono::seconds(10);
  std::vector<std::pair<DWORD, HANDLE>> targets;

  for (;;) {
    auto now = std::chrono::steady_clock::now();
    // Refresh the thread list once a second.
    if (now - last_enum >= std::chrono::seconds(1)) {
      last_enum = now;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
          if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
          if (!handles.count(te.th32ThreadID)) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (h) handles[te.th32ThreadID] = h;
          }
        }
        CloseHandle(snap);
      }
      targets.assign(handles.begin(), handles.end());
    }

    for (auto& [tid, h] : targets) {
      CONTEXT c;
      c.ContextFlags = CONTEXT_CONTROL;
      if (SuspendThread(h) == DWORD(-1)) continue;
      BOOL got = GetThreadContext(h, &c);
      ResumeThread(h);
      if (got && samples.size() < samples.capacity()) samples.push_back({tid, c.Rip});
    }

    if (now - last_report >= std::chrono::seconds(interval_s)) {
      last_report = now;
      // Modules loaded after start (the GPU plugin DLL) need their symbols too.
      SymRefreshModuleList(process);
      // Per thread: total samples and per-function counts.
      std::unordered_map<DWORD, std::unordered_map<std::string, uint32_t>> per_thread;
      std::unordered_map<DWORD, uint32_t> totals;
      std::unordered_map<DWORD64, std::string> names;
      alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
      for (const Sample& s : samples) {
        auto it = names.find(s.pc);
        if (it == names.end()) {
          auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
          sym->SizeOfStruct = sizeof(SYMBOL_INFO);
          sym->MaxNameLen = 255;
          DWORD64 disp = 0;
          std::string n = SymFromAddr(process, s.pc, &disp, sym)
                              ? std::string(sym->Name)
                              : fmt::format("?{:X}", s.pc >> 12 << 12);
          it = names.emplace(s.pc, std::move(n)).first;
        }
        per_thread[s.tid][it->second]++;
        totals[s.tid]++;
      }
      // Rank threads by samples spent working (not blocked in a kernel wait).
      auto is_wait = [](const std::string& name) {
        return name.find("Wait") != std::string::npos || name.find("Sleep") != std::string::npos ||
               name.find("DelayExecution") != std::string::npos ||
               name.find("GetMessage") != std::string::npos ||
               name.find("RemoveIoCompletion") != std::string::npos;
      };
      std::vector<std::pair<uint32_t, DWORD>> order;
      for (auto& [tid, funcs] : per_thread) {
        uint32_t busy = 0;
        for (auto& [name, count] : funcs)
          if (!is_wait(name)) busy += count;
        order.push_back({busy, tid});
      }
      std::sort(order.rbegin(), order.rend());
      int top = REXCVAR_GET(fh1_profile_top);
      int shown = 0;
      for (auto& [busy, tid] : order) {
        if (shown++ >= 5 || busy == 0) break;
        uint32_t n = totals[tid];
        auto& funcs = per_thread[tid];
        std::vector<std::pair<uint32_t, std::string>> list;
        for (auto& [name, count] : funcs) list.push_back({count, name});
        std::sort(list.rbegin(), list.rend());
        std::string line;
        for (int i = 0; i < int(list.size()) && i < top; ++i) {
          line += fmt::format("\n    {:5.1f}%  {}", 100.0 * list[i].first / n, list[i].second);
        }
        REXLOG_INFO("[profile] {}: working {:.0f}% of {} samples (% of wall time):{}",
                    ThreadName(tid), 100.0 * busy / n, n, line);
      }
      samples.clear();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

void Fh1StartProfiler() {
  int32_t interval = REXCVAR_GET(fh1_profile);
  if (interval <= 0) return;
  std::thread(ProfilerMain, interval).detach();
  REXLOG_INFO("[profile] sampling every thread, report every {} s", interval);
}

#else

void Fh1StartProfiler() {}

#endif
