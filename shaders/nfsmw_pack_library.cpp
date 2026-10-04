#include "../app/src/nfsc_shader_library.h"
#include "nfsmw_container_2005.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<uint8_t> Read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || f.tellg() > 64 * 1024 * 1024)
    throw std::runtime_error("Input file unreadable or too large: " + path.string());
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) throw std::runtime_error("ReadAccess incompleta");
  return d;
}

int main(int argc, char** argv) try {
  if (argc != 4) throw std::runtime_error("Use: nfsmw_pack_library <containers> <spirv validados> <output new_entry>");
  const fs::path original = argv[1], compiled = argv[2], output = argv[3];
  if (fs::exists(output)) throw std::runtime_error("The output already exists");
  std::vector<fs::path> paths;
  for (const auto& e : fs::directory_iterator(original))
    if (e.is_regular_file() && e.path().extension() == ".bin") paths.push_back(e.path());
  std::sort(paths.begin(), paths.end());
  std::vector<nfsmw::native::Shader> shaders;
  for (const auto& path : paths) {
    nfsmw::native::Shader s;
    s.original = Read(path);
    nfsmw::Flow flow;
    (void)nfsmw::Convert2005(s.original, flow);
    auto spv = Read(compiled / (path.stem().string() + ".spv"));
    if (spv.size() % 4) throw std::runtime_error("SPIR-V desalineado");
    for (size_t i = 0; i < spv.size(); i += 4)
      s.spirv.push_back(uint32_t(spv[i]) | uint32_t(spv[i+1]) << 8 |
                       uint32_t(spv[i+2]) << 16 | uint32_t(spv[i+3]) << 24);
    shaders.push_back(std::move(s));
  }
  auto packet = nfsmw::native::PackShaders(std::move(shaders));
  nfsmw::native::LibraryShaders library;
  library.Load(packet);
  for (const auto& path : paths)
    if (!library.Find(Read(path))) throw std::runtime_error("A shader is not found after packing");
  std::ofstream f(output, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(packet.data()), packet.size()) || !f.flush())
    throw std::runtime_error("Could not write the whole package");
  std::printf("%zu containers -> %zu shaders unicos, %zu bytes; all recuperables\n",
              paths.size(), library.shaders().size(), packet.size());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "%s\n", e.what());
  return 1;
}
