// nfsc - native renderer, part C5a: identify in the NFSSPV library the
// shaders the game's D3D uploads to the ring (PM4_IM_LOAD and
// IM_LOAD_IMMEDIATE).
//
// The ring only carries the microcode. The library stores the original
// compiled container (header, constant table, definitions and microcode)
// with its SPIR-V translation made by XenosRecomp.
//
// Pixel shaders arrive as is and are identified by their microcode. Vertex
// shaders do not: D3D reorders the fetches, changes their swizzles and
// nulls the outputs the pixel shader does not read (measured), so their
// identity comes from the device objects (nfsc_native_hooks.h) and only
// the vertex input is read from the patched microcode.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace nfsc::native {
struct Shader;
}

namespace nfsc::native {

// Vertex declaration element of the container (XenosRecomp shader.h).
struct ElementVertex {
  uint16_t instruction = 0;  // index of the fetch instruction in the microcode
  uint8_t use = 0;           // DeclUsage: 0 position, 3 normal, 5 texcoord, 10 color...
  uint8_t index_use = 0;
};

// Sampler from the constant table: the register is the texture's fetch constant
// in pixel shaders.
struct SamplerShader {
  uint16_t reg_entry = 0;
  uint16_t type = 0;  // D3DXPARAMETER_TYPE: 10-12 = 2D, 13 = 3D, 14 = cube
  // It is called SHADOWMAP_SAMPLER in the constant table: it is the shadow map sampling the library rewrites
  // (tfetch2DShadow and tfetch2DShadowMin). nfsc_native_shadow_minimum.
  bool map_shadows = false;
};

struct EntryShader {
  const nfsc::native::Shader* shader = nullptr;  // container original y SPIR-V
  bool vertices = false;
  uint32_t number = 0;  // position in the library, for the reports
  std::vector<uint32_t> microcode;  // in host byte order, with the fetches masked
  uint64_t fingerprint = 0;                // XXH3 of that masked microcode
  // NFSC: Carbon's Direct3D also rewrites the destination swizzle (low 12 bits of word 1) of the vertex fetches, so
  // vertex shaders are matched with those bits cleared in the fetch words of both sides.
  uint64_t fingerprint_compare = 0;
  std::vector<uint32_t> microcode_compare;
  // FH1: the microcode as in the container (no masks), for the tolerant pass (MatchesFh1).
  std::vector<uint32_t> microcode_shader;
  std::vector<ElementVertex> elements;
  std::vector<SamplerShader> samplers;
  uint32_t outputs = 0;  // pixel shader: bits COLOR0..3 y DEPTH (PixelShaderOutputs)
  // OpKill in the SPIR-V. XenosRecomp puts one in every pixel shader for the alpha test (guarded by
  // SPEC_CONSTANT_ALPHA_TEST), so it only really discards if there is more than one.
  uint32_t kills = 0;
  bool discards = false;
  // Its SPIR-V has tfetch2DShadowMin (the NFSC_MARK_SHADOW_MINIMUM constant from shader_common.h): with
  // kSpecShadowMinimum it takes the minimum of the shadow map and its pair. nfsc_native_shadow_minimum.
  bool shadow_minimum = false;
  // Bytes of its float constant buffer that the SPIR-V reads (16 per register).
  uint32_t constants_bytes = 256 * 16;
};

struct StatisticsShaders {
  uint64_t loads = 0;           // IM_LOAD received
  uint64_t different = 0;        // distinct microcodes (with their patches)
  uint64_t identified = 0;    // of the distinct ones
  uint64_t without_identify = 0;  // of the distinct ones
  uint64_t ambiguous = 0;         // distinct ones with more than one possible container
  uint64_t tolerant = 0;          // FH1: identified by the tolerant pass (MatchesFh1)
};

class ShadersNative {
 public:
  ShadersNative();
  ~ShadersNative();

  // false, with a warning in the log, if the library is missing or damaged.
  bool Load(const std::filesystem::path& file);
  bool loaded() const;

  // Full original container (the one the D3D constructors receive).
  // nullptr if it is not in the library. Can be called from any thread
  // once the library is loaded.
  const EntryShader* IdentifyContainer(std::span<const uint8_t> container) const;

  // The entry with that number (its position in the library), or nullptr. Like IdentifyContainer, from any
  // thread once loaded: entries do not change after Load. Pipeline prewarming (nfsc_native_draws.cpp) uses
  // it to recreate the pipelines of the previous session.
  const EntryShader* ByNumber(uint32_t number) const;

  // Microcode already in host byte order. nullptr if it is not in the library.
  // Only the ring thread uses it.
  const EntryShader* Identify(bool vertices, std::span<const uint32_t> microcode);

  StatisticsShaders Statistics() const;

 private:
  struct Data;
  std::unique_ptr<Data> data_;
};

// Short name of a DeclUsage for the reports.
const char* NameUse(uint8_t use);

// Whether a patched microcode from the ring can belong to that vertex shader: every
// declaration element has, at one of the element positions, a fetch that writes the
// same temporary register as the original. D3D reorders the fetches and changes
// formats and swizzles, but not the registers.
bool FetchCoherent(const EntryShader& vs, std::span<const uint32_t> patched);

}  // namespace nfsc::native
