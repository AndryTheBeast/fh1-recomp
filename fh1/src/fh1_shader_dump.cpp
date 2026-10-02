// fh1 - shader container dump for the native renderer's library (docs/native-renderer-fh1.md, N0).
//
// Some of FH1's shaders live in the tracks' bin.zip archives, compressed with zip method 21 (not
// read offline yet). Once the game has loaded an area, their decompressed containers sit in guest
// memory: --fh1_dump_shaders=SECONDS scans all committed guest memory at that time after the first
// frame and writes every plausible 2008 container (102A1100 pixel, 102A1101 vertex) to
// --fh1_dump_shaders_dir as <address>.bin. tools/fh1_extract_shaders.py --merge DIR adds them to the
// library's container folder (named and deduplicated like the disc ones).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(fh1_dump_shaders, 0, "FH1",
                     "Debug (native renderer): N seconds after the first frame, write every shader "
                     "container found in guest memory to fh1_dump_shaders_dir (0 = off)");
REXCVAR_DEFINE_STRING(fh1_dump_shaders_dir, "", "FH1",
                      "Folder for fh1_dump_shaders (created if needed)");

namespace {

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void ScanAndWrite(const uint8_t* base) {
  std::filesystem::path dir = REXCVAR_GET(fh1_dump_shaders_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  uint32_t found = 0, written = 0;
  std::unordered_set<std::string> seen;
#ifdef _WIN32
  // Walk the host mapping of the 4 GB guest space, committed readable regions only.
  const uint8_t* p = base;
  const uint8_t* end = base + 0x100000000ull;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info))) break;
    const uint8_t* region = static_cast<const uint8_t*>(info.BaseAddress);
    const uint8_t* region_end = region + info.RegionSize;
    bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
    if (readable) {
      const uint8_t* q = std::max(region, base);
      const uint8_t* q_end = std::min(region_end, end);
      for (; q + 36 <= q_end; q += 4) {
        if (q[0] != 0x10 || q[1] != 0x2A || q[2] != 0x11 || q[3] > 1) continue;
        uint32_t virtual_size = Be32(q + 4), physical_size = Be32(q + 8);
        uint32_t const_off = Be32(q + 16), def_off = Be32(q + 20), shader_off = Be32(q + 24);
        uint64_t size = uint64_t(virtual_size) + physical_size;
        if (virtual_size < 36 || virtual_size >= 0x40000 || !physical_size || physical_size >= 0x100000 ||
            physical_size % 4 || shader_off >= virtual_size || const_off >= virtual_size ||
            def_off >= virtual_size || q + size > q_end) {
          continue;
        }
        ++found;
        std::string blob(reinterpret_cast<const char*>(q), size_t(size));
        if (!seen.insert(blob).second) continue;
        char name[32];
        std::snprintf(name, sizeof(name), "%08X.bin", uint32_t(q - base));
        if (FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
          std::fwrite(blob.data(), 1, blob.size(), f);
          std::fclose(f);
          ++written;
        }
      }
    }
    p = region_end;
  }
#endif
  REXLOG_INFO("[shaders] dump: {} containers found in guest memory, {} distinct written to {}", found,
              written, dir.string());
}

}  // namespace

// Called every ring wait (fh1_perf_hooks.cpp); starts the timer on the first call.
void Fh1ShaderDumpTick(const uint8_t* base) {
  static std::atomic<bool> started{false};
  int32_t seconds = REXCVAR_GET(fh1_dump_shaders);
  if (seconds <= 0 || started.exchange(true)) return;
  std::thread([base, seconds] {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    ScanAndWrite(base);
  }).detach();
}
