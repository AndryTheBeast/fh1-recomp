// FH1 version of nfsmw_pack_library.cpp: packs FH1's 2008 shader containers and their SPIR-V into
// fh1_shaders.nfsp (fh1/src/native/fh1_shader_library.*). No 2005 conversion. Native renderer step N0.
#include "../fh1/src/native/fh1_shader_library.h"

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
  if (argc != 4) throw std::runtime_error("Use: fh1_pack_library <containers> <spirv validados> <output new_entry>");
  const fs::path original = argv[1], compiled = argv[2], output = argv[3];
  if (fs::exists(output)) throw std::runtime_error("The output already exists");
  std::vector<fs::path> paths;
  for (const auto& e : fs::directory_iterator(original))
    if (e.is_regular_file() && e.path().extension() == ".bin") paths.push_back(e.path());
  std::sort(paths.begin(), paths.end());
  // FH1: containers that did not translate or compile (damaged memory dumps) have no SPIR-V: left out.
  size_t sin_spirv = 0;
  std::erase_if(paths, [&](const fs::path& path) {
    bool missing = !fs::exists(compiled / (path.stem().string() + ".spv"));
    sin_spirv += missing;
    return missing;
  });
  if (sin_spirv) std::printf("%zu containers without SPIR-V, left out of the library\n", sin_spirv);
  std::vector<fh1::native::Shader> shaders;
  for (const auto& path : paths) {
    fh1::native::Shader s;
    s.original = Read(path);
    auto spv = Read(compiled / (path.stem().string() + ".spv"));
    if (spv.size() % 4) throw std::runtime_error("SPIR-V desalineado");
    for (size_t i = 0; i < spv.size(); i += 4)
      s.spirv.push_back(uint32_t(spv[i]) | uint32_t(spv[i+1]) << 8 |
                       uint32_t(spv[i+2]) << 16 | uint32_t(spv[i+3]) << 24);
    shaders.push_back(std::move(s));
  }
  auto packet = fh1::native::PackShaders(std::move(shaders));
  fh1::native::LibraryShaders library;
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
