// nfsmw - sampling CPU profiler and stack dumper for the PC build (see nfsmw_profile_pc.h).
//
// Careful with the pause (as in switch_perf.cpp): while a thread is suspended, nothing may be done that could
// take a lock that thread holds (no allocation, no stdio, no logger). Between SuspendThread and ResumeThread
// there are only system calls; samples go to a vector whose capacity was reserved beforehand.

#include "nfsmw_profile_pc.h"

#if defined(_WIN32)

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#define PSAPI_VERSION 2  // K32EnumProcessModules and friends, in kernel32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(nfsmw_profile_pc_since_s, 0, "NFSMW",
                     "PC: muestrear la CPU de los threads ocupados since este second de life del process "
                     "(0 = never; solo tests). Leaves logs/profile_pc.csv")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_profile_pc_duration_s, 20, "NFSMW",
                     "PC: seconds de sampling de nfsmw_profile_pc_since_s")
    .range(1, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_profile_pc_stacks_s, 0, "NFSMW",
                     "PC: dump the stacks_value de all los threads en este second de life del process (0 = never; "
                     "solo tests). Leaves logs/stacks_N.txt; el watcher tambien vuelca al ver el game stopped")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::profile_pc {
namespace {

constexpr size_t kCandidates = 6;     // stack addresses that fall inside a known module
constexpr size_t kWordsStack = 64;  // how many stack words are inspected per sample

struct Modulo {
  uint64_t base = 0;
  uint64_t tam = 0;
  std::string name;
  std::string path;
};

struct Thread {
  DWORD id = 0;
  HANDLE h = nullptr;
  std::string name;
  uint64_t time_prev = 0;  // 100 ns de CPU (user + core)
  bool busy_3 = false;
  uint64_t sample_total = 0;
  double cpu_max = 0.0;
};

struct Sample {
  uint32_t thread_value = 0;
  uint64_t pc = 0;
  uint64_t cand[kCandidates] = {};
};

std::atomic<bool> g_stop{false};
std::thread g_thread;
std::thread g_thread_stacks;
std::mutex g_dump_mutex;
int g_dumps = 0;  // con g_dump_mutex

std::string Utf8(const wchar_t* w) {
  if (!w || !*w) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return {};
  }
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string NameThread(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const Fn read =
      reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!read) {
    return {};
  }
  PWSTR desc = nullptr;
  std::string name;
  if (SUCCEEDED(read(h, &desc)) && desc) {
    name = Utf8(desc);
    LocalFree(desc);
  }
  return name;
}

double SecondsOfLife() {
  FILETIME creation{}, output{}, core{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &output, &core, &user)) {
    return 0.0;
  }
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  const uint64_t c = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  const uint64_t a = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  return a > c ? double(a - c) / 1e7 : 0.0;
}

std::vector<Modulo> ListModules() {
  std::vector<Modulo> modules;
  HMODULE list[1024];
  DWORD bytes = 0;
  if (!EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &bytes)) {
    return modules;
  }
  for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
    MODULEINFO info{};
    char name[MAX_PATH] = {};
    char path[MAX_PATH * 2] = {};
    if (!GetModuleInformation(GetCurrentProcess(), list[i], &info, sizeof(info))) {
      continue;
    }
    GetModuleBaseNameA(GetCurrentProcess(), list[i], name, sizeof(name));
    GetModuleFileNameExA(GetCurrentProcess(), list[i], path, sizeof(path));
    modules.push_back({uint64_t(uintptr_t(info.lpBaseOfDll)), uint64_t(info.SizeOfImage), name, path});
  }
  std::sort(modules.begin(), modules.end(), [](const Modulo& a, const Modulo& b) { return a.base < b.base; });
  return modules;
}

// Index of the module that contains the address, or -1.
int ModuleOf(const std::vector<Modulo>& modules, uint64_t address) {
  auto it = std::upper_bound(modules.begin(), modules.end(), address,
                             [](uint64_t d, const Modulo& m) { return d < m.base; });
  if (it == modules.begin()) {
    return -1;
  }
  --it;
  return address - it->base < it->tam ? int(it - modules.begin()) : -1;
}

std::vector<Thread> ListThreads() {
  std::vector<Thread> threads;
  const HANDLE photo = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (photo == INVALID_HANDLE_VALUE) {
    return threads;
  }
  THREADENTRY32 e{};
  e.dwSize = sizeof(e);
  const DWORD process = GetCurrentProcessId();
  const DWORD yo = GetCurrentThreadId();
  for (BOOL ok = Thread32First(photo, &e); ok; ok = Thread32Next(photo, &e)) {
    if (e.th32OwnerProcessID != process || e.th32ThreadID == yo) {
      continue;
    }
    const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                FALSE, e.th32ThreadID);
    if (!h) {
      continue;
    }
    Thread thread_value;
    thread_value.id = e.th32ThreadID;
    thread_value.h = h;
    thread_value.name = NameThread(h);
    threads.push_back(std::move(thread_value));
  }
  CloseHandle(photo);
  return threads;
}

uint64_t TimeCpu(HANDLE h) {
  FILETIME c{}, s{}, n{}, u{};
  if (!GetThreadTimes(h, &c, &s, &n, &u)) {
    return 0;
  }
  return ((uint64_t(n.dwHighDateTime) << 32) | n.dwLowDateTime) +
         ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}

void Loop(int since_s, int duration_s) {
  while (!g_stop.load() && SecondsOfLife() < double(since_s)) {
    Sleep(200);
  }
  if (g_stop.load()) {
    return;
  }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
  const std::vector<Modulo> modules = ListModules();
  std::vector<Thread> threads = ListThreads();
  std::vector<Sample> sample_total;
  sample_total.reserve(size_t(duration_s) * 1000 * 8);

  const double start = SecondsOfLife();
  LARGE_INTEGER frequency{}, t0{}, t{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&t0);
  LONGLONG last_revision = t0.QuadPart - 2 * frequency.QuadPart;
  const LONGLONG fin = t0.QuadPart + LONGLONG(duration_s) * frequency.QuadPart;
  uint64_t misses_pause = 0;
  uint64_t lost = 0;

  for (;;) {
    QueryPerformanceCounter(&t);
    if (g_stop.load() || t.QuadPart >= fin) {
      break;
    }
    // Every second: which threads are busy (more than 5 % of a core since the previous check).
    if (t.QuadPart - last_revision >= frequency.QuadPart) {
      const double seconds = double(t.QuadPart - last_revision) / double(frequency.QuadPart);
      for (Thread& h : threads) {
        const uint64_t cpu = TimeCpu(h.h);
        const double fraction = h.time_prev && seconds < 10.0
                                    ? double(cpu - h.time_prev) / 1e7 / seconds
                                    : 0.0;
        h.busy_3 = fraction > 0.05;
        h.cpu_max = std::max(h.cpu_max, fraction);
        h.time_prev = cpu;
      }
      last_revision = t.QuadPart;
    }
    for (size_t i = 0; i < threads.size(); ++i) {
      Thread& h = threads[i];
      if (!h.busy_3) {
        continue;
      }
      if (sample_total.size() == sample_total.capacity()) {
        ++lost;  // no growth: no allocation inside the loop
        continue;
      }
      if (SuspendThread(h.h) == DWORD(-1)) {
        ++misses_pause;
        continue;
      }
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      const bool ok = GetThreadContext(h.h, &ctx) != 0;
      uint64_t stack[kWordsStack] = {};
      SIZE_T read = 0;
      if (ok) {
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(uintptr_t(ctx.Rsp)), stack,
                          sizeof(stack), &read);
      }
      ResumeThread(h.h);
      if (!ok) {
        continue;
      }
      Sample m;
      m.thread_value = uint32_t(i);
      m.pc = ctx.Rip;
      size_t n = 0;
      for (size_t k = 0; k < read / sizeof(uint64_t) && n < kCandidates; ++k) {
        if (ModuleOf(modules, stack[k]) >= 0) {
          m.cand[n++] = stack[k];
        }
      }
      sample_total.push_back(m);
      ++h.sample_total;
    }
    if (timer) {
      LARGE_INTEGER term{};
      term.QuadPart = -10000;  // 1 ms, relativo
      SetWaitableTimer(timer, &term, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 20);
    } else {
      Sleep(1);
    }
  }
  const double fin_s = SecondsOfLife();
  if (timer) {
    CloseHandle(timer);
  }

  // Everything that allocates or writes comes after sampling.
  const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "logs" / "profile_pc.csv";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::FILE* f = _wfopen(path.c_str(), L"wb");
  if (f) {
    std::fprintf(f, "#sampling;%.1f;%.1f;%zu\n", start, fin_s, sample_total.size());
    for (size_t i = 0; i < modules.size(); ++i) {
      std::fprintf(f, "#modulo;%zu;%s;%llX;%llX\n", i, modules[i].name.c_str(),
                   static_cast<unsigned long long>(modules[i].base),
                   static_cast<unsigned long long>(modules[i].tam));
    }
    for (size_t i = 0; i < threads.size(); ++i) {
      if (threads[i].sample_total) {
        std::fprintf(f, "#thread_value;%zu;%lu;%s;%llu;%.3f\n", i, static_cast<unsigned long>(threads[i].id),
                     threads[i].name.c_str(), static_cast<unsigned long long>(threads[i].sample_total),
                     threads[i].cpu_max);
      }
    }
    const auto write = [&](uint64_t d) {
      const int mod = ModuleOf(modules, d);
      if (mod < 0) {
        std::fprintf(f, ";-;%llX", static_cast<unsigned long long>(d));
      } else {
        std::fprintf(f, ";%d;%llX", mod, static_cast<unsigned long long>(d - modules[size_t(mod)].base));
      }
    };
    for (const Sample& m : sample_total) {
      std::fprintf(f, "%u", m.thread_value);
      write(m.pc);
      for (size_t k = 0; k < kCandidates && m.cand[k]; ++k) {
        write(m.cand[k]);
      }
      std::fputc('\n', f);
    }
    std::fclose(f);
  }
  REXLOG_INFO("[profile_pc] {} sample_total en {:.1f} s (de {:.1f} a {:.1f} s de life), {} pauses fallidas, {} "
              "lost; {}",
              sample_total.size(), fin_s - start, start, fin_s, misses_pause, lost,
              f ? path.string() : std::string("no se pudo write el file"));
  for (const Thread& h : threads) {
    if (h.sample_total) {
      REXLOG_INFO("[profile_pc] thread_value {} \"{}\": {} sample_total, CPU maxima {:.0f} %", h.id, h.name, h.sample_total,
                  h.cpu_max * 100.0);
    }
  }
  for (Thread& h : threads) {
    CloseHandle(h.h);
  }
}

// --- Stack dump ------------------------------------------------------------------------------------------------
// For hangs (the ring thread can stop while the profiler above only sees busy threads). It suspends threads
// one at a time, copies their context and the top of their stack, and resumes them; with nobody suspended
// any more, it unwinds each copy with its module's unwind table (.pdata) and RtlVirtualUnwind, with the
// registers that point into the stack relocated to the copy. A blocked thread does not move, so its copy is
// its real stack; that of a thread that was running may come out truncated.

constexpr size_t kBytesStack = 256 * 1024;
constexpr size_t kMarginStack = 32 * 1024;  // RtlVirtualUnwind reads a bit above the frame it unwinds
constexpr size_t kFrames = 48;

struct StackCopied {
  CONTEXT ctx{};
  std::vector<uint8_t> data;
  size_t bytes = 0;
  bool ok = false;
};

// The module's .pdata entry for that address, read from the loaded image (without the locks of
// RtlLookupFunctionEntry, in case some thread held them).
const RUNTIME_FUNCTION* EntryUnrolled(const Modulo& m, uint64_t pc) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(uintptr_t(m.base));
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return nullptr;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(uintptr_t(m.base + uint64_t(dos->e_lfanew)));
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return nullptr;
  }
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) {
    return nullptr;
  }
  const auto* table = reinterpret_cast<const RUNTIME_FUNCTION*>(uintptr_t(m.base + dir.VirtualAddress));
  const uint32_t rva = uint32_t(pc - m.base);
  size_t lo = 0;
  size_t hi = dir.Size / sizeof(RUNTIME_FUNCTION);
  while (lo < hi) {
    const size_t half = lo + (hi - lo) / 2;
    if (rva < table[half].BeginAddress) {
      hi = half;
    } else if (rva >= table[half].EndAddress) {
      lo = half + 1;
    } else {
      const RUNTIME_FUNCTION* f = &table[half];
      if (f->UnwindData & 1) {  // indirect entry: points to another RUNTIME_FUNCTION
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(uintptr_t(m.base + (f->UnwindData & ~DWORD(1))));
      }
      return f;
    }
  }
  return nullptr;
}

// One protected RtlVirtualUnwind step: an odd table or stack does not bring the game down. No objects with
// destructors (__try does not allow them).
bool StepUnrolled(uint64_t base, const RUNTIME_FUNCTION* f, CONTEXT* ctx) {
  __try {
    PVOID data_handler = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, const_cast<PRUNTIME_FUNCTION>(f), ctx,
                     &data_handler, &frame, nullptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::vector<uint64_t> Unroll(const StackCopied& p, const std::vector<Modulo>& modules) {
  std::vector<uint64_t> frames;
  if (!p.ok) {
    return frames;
  }
  CONTEXT ctx = p.ctx;
  const uint64_t original = p.ctx.Rsp;
  const uint64_t copy = uint64_t(uintptr_t(p.data.data()));
  const uint64_t bytes = p.bytes;
  const auto in_copy = [&](uint64_t d, uint64_t tam) { return d >= copy && d + tam <= copy + bytes; };
  // In UNWIND_INFO register number order (0 = RAX ... 15 = R15).
  const std::array<DWORD64*, 16> register_values = {&ctx.Rax, &ctx.Rcx, &ctx.Rdx, &ctx.Rbx, &ctx.Rsp, &ctx.Rbp,
                                              &ctx.Rsi, &ctx.Rdi, &ctx.R8,  &ctx.R9,  &ctx.R10, &ctx.R11,
                                              &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15};
  const auto translate = [&] {
    for (DWORD64* r : register_values) {
      if (*r >= original && *r < original + bytes) {
        *r = *r - original + copy;
      }
    }
  };
  translate();
  frames.push_back(ctx.Rip);
  for (size_t k = 1; k < kFrames && in_copy(ctx.Rsp, 8); ++k) {
    const int mod = ModuleOf(modules, ctx.Rip);
    if (mod < 0) {
      break;
    }
    const Modulo& m = modules[size_t(mod)];
    const RUNTIME_FUNCTION* f = EntryUnrolled(m, ctx.Rip);
    if (!f) {
      if (k != 1) {
        break;  // only the top frame can be a leaf function without .pdata
      }
      std::memcpy(&ctx.Rip, reinterpret_cast<const void*>(uintptr_t(ctx.Rsp)), 8);
      ctx.Rsp += 8;
    } else {
      const auto* info = reinterpret_cast<const uint8_t*>(uintptr_t(m.base + f->UnwindData));
      const uint8_t register_frame = info[3] & 0x0F;
      const bool past_prologue = uint32_t(ctx.Rip - m.base) - f->BeginAddress >= info[1];
      if (register_frame && past_prologue && !in_copy(*register_values[register_frame], 1)) {
        break;  // the frame is based on a register that does not point into the copy
      }
      if (!StepUnrolled(m.base, f, &ctx)) {
        break;
      }
    }
    if (!ctx.Rip) {
      break;
    }
    frames.push_back(ctx.Rip);
    translate();  // non-volatile registers restored from the stack hold values from the original stack
  }
  return frames;
}

}  // namespace

void DumpStacks(const char* reason) {
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  const int number = ++g_dumps;
  const std::vector<Modulo> modules = ListModules();
  std::vector<Thread> threads = ListThreads();
  std::vector<StackCopied> stacks_value(threads.size());
  for (StackCopied& p : stacks_value) {
    p.data.assign(kBytesStack + kMarginStack, 0);
  }
  const double life = SecondsOfLife();
  for (size_t i = 0; i < threads.size(); ++i) {
    StackCopied& p = stacks_value[i];
    if (SuspendThread(threads[i].h) == DWORD(-1)) {
      continue;
    }
    p.ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(threads[i].h, &p.ctx)) {
      p.ok = true;
      MEMORY_BASIC_INFORMATION region{};
      if (VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p.ctx.Rsp)), &region, sizeof(region))) {
        const uint64_t fin = uint64_t(uintptr_t(region.BaseAddress)) + region.RegionSize;
        const uint64_t request_2 = fin > p.ctx.Rsp ? std::min<uint64_t>(fin - p.ctx.Rsp, kBytesStack) : 0;
        SIZE_T read = 0;
        if (request_2 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(uintptr_t(p.ctx.Rsp)),
                                       p.data.data(), SIZE_T(request_2), &read)) {
          p.bytes = size_t(read);
        }
      }
    }
    ResumeThread(threads[i].h);
  }

  // Nobody is suspended any more.
  const std::filesystem::path path =
      rex::filesystem::GetExecutableFolder() / "logs" / ("stacks_" + std::to_string(number) + ".txt");
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::FILE* f = _wfopen(path.c_str(), L"wb");
  if (f) {
    std::fprintf(f, "#dump;%s;%.1f;%zu\n", reason, life, threads.size());
    for (size_t i = 0; i < modules.size(); ++i) {
      std::fprintf(f, "#modulo;%zu;%s;%llX;%llX;%s\n", i, modules[i].name.c_str(),
                   static_cast<unsigned long long>(modules[i].base),
                   static_cast<unsigned long long>(modules[i].tam), modules[i].path.c_str());
    }
    for (size_t i = 0; i < threads.size(); ++i) {
      const std::vector<uint64_t> frames = Unroll(stacks_value[i], modules);
      const unsigned long id = static_cast<unsigned long>(threads[i].id);
      std::fprintf(f, "#thread_value;%lu;%s;%d;%zu;%zu\n", id, threads[i].name.c_str(), stacks_value[i].ok ? 1 : 0,
                   stacks_value[i].bytes, frames.size());
      for (size_t k = 0; k < frames.size(); ++k) {
        const int mod = ModuleOf(modules, frames[k]);
        if (mod < 0) {
          std::fprintf(f, "%lu;%zu;-;%llX\n", id, k, static_cast<unsigned long long>(frames[k]));
        } else {
          std::fprintf(f, "%lu;%zu;%d;%llX\n", id, k, mod,
                       static_cast<unsigned long long>(frames[k] - modules[size_t(mod)].base));
        }
      }
    }
    std::fclose(f);
  }
  REXLOG_WARN("[profile_pc] stacks_value de {} threads dumped ({}): {}", threads.size(), reason,
              f ? path.string() : std::string("no se pudo write el file"));
  for (Thread& h : threads) {
    CloseHandle(h.h);
  }
}

void Start() {
  g_stop.store(false);
  const int stacks_s = REXCVAR_GET(nfsmw_profile_pc_stacks_s);
  if (stacks_s > 0 && !g_thread_stacks.joinable()) {
    g_thread_stacks = std::thread([stacks_s] {
      while (!g_stop.load() && SecondsOfLife() < double(stacks_s)) {
        Sleep(200);
      }
      if (!g_stop.load()) {
        DumpStacks("nfsmw_profile_pc_stacks_s");
      }
    });
  }
  const int since_s = REXCVAR_GET(nfsmw_profile_pc_since_s);
  if (since_s <= 0 || g_thread.joinable()) {
    return;
  }
  g_thread = std::thread(Loop, since_s, int(REXCVAR_GET(nfsmw_profile_pc_duration_s)));
  REXLOG_INFO("[profile_pc] sampling de CPU since {} s de life del process, {} s", since_s,
              int(REXCVAR_GET(nfsmw_profile_pc_duration_s)));
}

void Stop() {
  g_stop.store(true);
  if (g_thread.joinable()) {
    g_thread.join();
  }
  if (g_thread_stacks.joinable()) {
    g_thread_stacks.join();
  }
}

}  // namespace nfsmw::profile_pc

#endif  // _WIN32
