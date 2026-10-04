// Searches for original containers without modifying the files of the game copy.
// A signature and dimension match is only a candidate, not a validation of the
// microcode. The output keeps the source and the exact offset.
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
#include <stdexcept>

static uint32_t ReadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

int main(int argc, char** argv) try {
  if (argc != 3) {
    std::fprintf(stderr, "use: nfsmw_find_containers <entry> <output new_entry>\n");
    return 1;
  }
  const std::filesystem::path output(argv[2]);
  if (std::filesystem::exists(output)) throw std::runtime_error("the output already exists");
  std::filesystem::create_directories(output);
  std::ofstream index(output / "procedencia.tsv");
  index << "file\tsource\toffset\tvirtual\tphysical\n";
  size_t total = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext != ".BIN" && ext != ".bin" && ext != ".xex" && ext != ".exe") continue;
    const size_t n = entry.file_size();
    if (n < 24 || n > 1024ULL * 1024 * 1024) continue;
    std::vector<uint8_t> data(n);
    std::ifstream file(entry.path(), std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(data.data()), n)) throw std::runtime_error("read failure");
    size_t found = 0;
    for (size_t i = 0; i + 24 <= n; ++i) {
      const auto* p = data.data() + i;
      if (p[0] != 0x10 || p[1] != 0x2A || p[2] != 0x0E || p[3] > 1) continue;
      const uint32_t v = ReadBE(p + 4), f = ReadBE(p + 8), c = ReadBE(p + 16), s = ReadBE(p + 20);
      if (v < 24 || uint64_t(v) + f > 65536 || uint64_t(v) + f > n - i ||
          c < 24 || c >= v || s < 24 || uint64_t(s) + 24 > v || !f) continue;
      char name[48];
      std::snprintf(name, sizeof(name), "%c_%06zu.bin", p[3] ? 'v' : 'p', total);
      std::ofstream copy(output / name, std::ios::binary);
      copy.write(reinterpret_cast<const char*>(p), size_t(v) + f);
      if (!copy) throw std::runtime_error("write failure");
      index << name << '\t' << entry.path().string() << '\t' << i << '\t' << v << '\t' << f << '\n';
      ++total;
      ++found;
    }
    std::printf("%s: %zu candidates\n", entry.path().filename().string().c_str(), found);
    std::fflush(stdout);
  }
  std::printf("Total: %zu candidates; they must be checked before translating.\n", total);
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 2;
}
