// nfsmw - profile dump of the instrumented build for PGO.
//
// Only compiled with NFSMW_PGO=generate (CMakeLists.txt). For each object, GCC embeds in the executable the
// path NFSMW_PGO_DIR\CMakeFiles#...#file.gcda (NFSMW_PGO_DIR is the profile folder pgo/<edition> as a
// Windows path on the PC that compiles). That cannot be opened on the Switch, so the fopen --wrap redirects
// it to sdmc:/switch/nfsmw/pgo/CMakeFiles#...#file.gcda.
//
// The Switch does not always close the program cleanly (the HOME menu kills it), so the profile cannot be
// left for exit: a thread dumps it every 3 minutes and resets the counters. libgcov adds to whatever each
// .gcda already holds, so several dumps give the total for the run.
#if defined(NFSMW_PGO_GENERATE) && defined(__SWITCH__)

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

extern "C" {
FILE* __real_fopen(const char* path, const char* mode);
void __gcov_dump(void);
void __gcov_reset(void);

FILE* __wrap_fopen(const char* path, const char* mode) {
  static constexpr char kPrefix[] = NFSMW_PGO_DIR;
  if (path && std::strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) {
    const char* rest = path + sizeof(kPrefix) - 1;
    while (*rest == '\\' || *rest == '/') {
      ++rest;
    }
    std::string new_entry = "sdmc:/switch/nfsmw/pgo/";
    for (; *rest; ++rest) {
      new_entry += (*rest == '\\') ? '/' : *rest;
    }
    return __real_fopen(new_entry.c_str(), mode);
  }
  return __real_fopen(path, mode);
}
}

namespace {
Thread g_thread_pgo;

void ThreadPgo(void*) {
  for (int dump = 1;; ++dump) {
    svcSleepThread(180LL * 1000 * 1000 * 1000);
    mkdir("sdmc:/switch/nfsmw/pgo", 0777);
    const u64 before = armGetSystemTick();
    __gcov_dump();
    __gcov_reset();
    const double ms = double(armTicksToNs(armGetSystemTick() - before)) / 1e6;
    if (FILE* f = __real_fopen("sdmc:/switch/nfsmw/pgo/dumps.txt", "a")) {
      std::fprintf(f, "dump %d: %.0f ms\n", dump, ms);
      std::fclose(f);
    }
  }
}

__attribute__((constructor)) void StartPgo() {
  // The game's normal priority (0x3B) on any core: it only works once every 3 minutes.
  if (R_SUCCEEDED(threadCreate(&g_thread_pgo, ThreadPgo, nullptr, nullptr, 64 * 1024, 0x3B, -2))) {
    threadStart(&g_thread_pgo);
  }
}
}  // namespace

#endif
