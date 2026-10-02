// FH1 version of nfsmw_empaquetar.cpp: packs FH1's 2008 shader containers and their SPIR-V into
// fh1_shaders.nfsp (fh1/src/fh1_shader_library.*). No 2005 conversion. Native renderer step N0.
#include "../fh1/src/fh1_shader_library.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<uint8_t> Leer(const fs::path& ruta) {
  std::ifstream f(ruta, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || f.tellg() > 64 * 1024 * 1024)
    throw std::runtime_error("Archivo de entrada ilegible o demasiado grande: " + ruta.string());
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) throw std::runtime_error("Lectura incompleta");
  return d;
}

int main(int argc, char** argv) try {
  if (argc != 4) throw std::runtime_error("Uso: fh1_empaquetar <contenedores> <spirv validados> <salida nueva>");
  const fs::path originales = argv[1], compilados = argv[2], salida = argv[3];
  if (fs::exists(salida)) throw std::runtime_error("La salida ya existe");
  std::vector<fs::path> rutas;
  for (const auto& e : fs::directory_iterator(originales))
    if (e.is_regular_file() && e.path().extension() == ".bin") rutas.push_back(e.path());
  std::sort(rutas.begin(), rutas.end());
  std::vector<fh1::native::Shader> shaders;
  for (const auto& ruta : rutas) {
    fh1::native::Shader s;
    s.original = Leer(ruta);
    auto spv = Leer(compilados / (ruta.stem().string() + ".spv"));
    if (spv.size() % 4) throw std::runtime_error("SPIR-V desalineado");
    for (size_t i = 0; i < spv.size(); i += 4)
      s.spirv.push_back(uint32_t(spv[i]) | uint32_t(spv[i+1]) << 8 |
                       uint32_t(spv[i+2]) << 16 | uint32_t(spv[i+3]) << 24);
    shaders.push_back(std::move(s));
  }
  auto paquete = fh1::native::EmpaquetarShaders(std::move(shaders));
  fh1::native::BibliotecaShaders biblioteca;
  biblioteca.Cargar(paquete);
  for (const auto& ruta : rutas)
    if (!biblioteca.Buscar(Leer(ruta))) throw std::runtime_error("Un shader no se encuentra tras empaquetar");
  std::ofstream f(salida, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(paquete.data()), paquete.size()) || !f.flush())
    throw std::runtime_error("No se pudo escribir el paquete completo");
  std::printf("%zu contenedores -> %zu shaders unicos, %zu bytes; todos recuperables\n",
              rutas.size(), biblioteca.shaders().size(), paquete.size());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "%s\n", e.what());
  return 1;
}
