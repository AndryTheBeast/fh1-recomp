// LZX decompression with the same library and parameters as the ReXGlue runtime.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mspack.h>
#include <lzx.h>

extern "C" struct mspack_system* mspack_default_system;

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr, "use: nfsmw_lzx <entry> <output new_entry> <bits window> <bytes output>\n");
    return 1;
  }
  const int bits = std::atoi(argv[3]);
  const long bytes = std::atol(argv[4]);
  if (bits < 15 || bits > 21 || bytes <= 0 || bytes > 512*1024*1024 || std::filesystem::exists(argv[2])) return 1;
  auto* system = mspack_default_system;
  auto* entry = system->open(system, argv[1], MSPACK_SYS_OPEN_READ);
  if (!entry) return 1;
  auto* output = system->open(system, argv[2], MSPACK_SYS_OPEN_WRITE);
  if (!output) { system->close(entry); return 1; }
  auto* flow = lzxd_init(system, entry, output, bits, 0, 0x8000, bytes, 0);
  const int state = flow ? lzxd_decompress(flow, bytes) : -1;
  if (flow) lzxd_free(flow);
  system->close(entry);
  system->close(output);
  if (state || std::filesystem::file_size(argv[2]) != size_t(bytes)) {
    std::fprintf(stderr, "LZX failure: %d; the output is not valid\n", state);
    return 2;
  }
  std::printf("LZX: %ld bytes descomprimidos\n", bytes);
  return 0;
}
