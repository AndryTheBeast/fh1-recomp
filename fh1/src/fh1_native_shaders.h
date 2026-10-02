// fh1 - native renderer, step N2: identify the shaders the game's D3D uploads to the ring
// (PM4_IM_LOAD / IM_LOAD_IMMEDIATE) in the shader library (fh1_shaders.nfsp, built by
// tools/build_shader_library.ps1). FH1 version of nfsmw-nx's nfsmw_nativo_shaders.* for the 2008
// container layout.
//
// Pixel shaders arrive as they are and are found by their microcode. Vertex shaders arrive patched
// by D3D (fetch constant, format, stride and offset of each vertex fetch come from the vertex
// declaration), so the lookup masks those fields in every declared fetch, as nfsmw does.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace fh1::native {

struct Shader;  // fh1_shader_library.h

struct VertexElement {
  uint16_t instruction = 0;  // index of the fetch instruction in the microcode
  uint8_t usage = 0;         // DeclUsage: 0 position, 3 normal, 5 texcoord, 10 color...
  uint8_t usage_index = 0;
};

struct ShaderSampler {
  uint16_t reg = 0;   // fetch constant (pixel shaders) / vertex sampler (vertex shaders)
  uint16_t type = 0;  // D3DXPARAMETER_TYPE: 10-12 = 2D, 13 = 3D, 14 = cube
};

struct ShaderEntry {
  const Shader* shader = nullptr;  // original container and SPIR-V
  bool vertex = false;
  uint32_t number = 0;             // position in the library
  std::vector<uint32_t> microcode; // host byte order, declared fetches masked
  std::vector<uint32_t> shader_microcode;  // the same without the mask
  uint64_t hash = 0;               // XXH3 of that masked microcode
  std::vector<VertexElement> elements;
  std::vector<ShaderSampler> samplers;
  uint32_t outputs = 0;            // pixel shader: COLOR0..3 and DEPTH bits
};

struct ShaderStats {
  uint64_t loads = 0;         // IM_LOADs looked up
  uint64_t distinct = 0;      // distinct microcodes (with their patches)
  uint64_t identified = 0;    // of the distinct ones
  uint64_t unidentified = 0;  // of the distinct ones
  uint64_t ambiguous = 0;     // distinct ones with more than one possible container
  uint64_t identified_vertex = 0, identified_pixel = 0;
  uint64_t unidentified_vertex = 0, unidentified_pixel = 0;
  uint64_t loose = 0;  // identified by the tolerant pass (FH1 D3D patches)
};

class Shaders {
 public:
  Shaders();
  ~Shaders();

  // false (with a log line) if the library is missing or damaged.
  bool Load(const std::filesystem::path& file);
  bool loaded() const;
  size_t size() const;

  // Microcode in host byte order, as uploaded. nullptr if not in the library. Ring thread only.
  const ShaderEntry* Identify(bool vertex, std::span<const uint32_t> microcode);

  ShaderStats Stats() const;

 private:
  struct Data;
  std::unique_ptr<Data> data_;
};

}  // namespace fh1::native
