// fh1 - native renderer, step N2 (see fh1_native_shaders.h). From nfsmw-nx's nfsc_native_shaders.cpp,
// with the 2008 container reader (XenosRecomp shader.h).

#include "fh1_native_shaders.h"

#include "fh1_shader_library.h"

#include <rex/logging.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace fh1::native {
namespace {

// Bits of a vertex fetch instruction D3D does not touch (VertexFetchInstruction in XenosRecomp
// shader_code.h): opcode, source and destination registers, destination swizzle and predicate.
// The rest (fetch constant, format, stride, offset, numeric modes) comes from the declaration.
constexpr uint32_t kFetchKeep[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};
constexpr uint32_t kMaxWarnings = 24;

// FH1 (measured 2026-10-02): its D3D also rewrites the destination swizzle of each declared fetch
// (word 1, bits 0-11) for the vertex layout, and replaces the exports of outputs the pixel shader
// does not read with this instruction. The tolerant pass (Identify) accepts both.
constexpr uint32_t kFetchKeepLoose[3] = {0x0007FFFF, 0x80000000, 0x80000000};
constexpr uint32_t kNullInstruction[3] = {0xC8000000, 0x00000000, 0x02000000};

// Whether the uploaded microcode is the library shader up to the FH1 D3D patches above.
bool LooseMatch(const ShaderEntry& e, std::span<const uint32_t> uploaded) {
  if (uploaded.size() != e.microcode.size()) return false;
  const std::vector<uint32_t>& original = e.shader_microcode;
  std::vector<uint8_t> fetch(uploaded.size() / 3 + 1, 0);
  for (const VertexElement& element : e.elements) fetch[element.instruction] = 1;
  for (size_t i = 0; i < uploaded.size(); i += 3) {
    const size_t n = std::min<size_t>(3, uploaded.size() - i);
    // Fetches the declaration does not list (the cars' extra streams, mini fetches) are patched the
    // same way: an instruction that decodes as a vertex fetch in the library copy (opcode 0 and the
    // must-be-one bit 19) with the same registers.
    const bool undeclared_fetch = n == 3 && !fetch[i / 3] && (original[i] & 0x1F) == 0 &&
                                  (original[i] >> 19) & 1 &&
                                  (uploaded[i] & kFetchKeepLoose[0]) == (original[i] & kFetchKeepLoose[0]);
    bool same = true;
    for (size_t j = 0; j < n; ++j) {
      uint32_t a = uploaded[i + j], b = original[i + j];
      if (fetch[i / 3] || undeclared_fetch) {
        a &= kFetchKeepLoose[j];
        b &= kFetchKeepLoose[j];
      }
      same = same && a == b;
    }
    if (same) continue;
    if (n == 3 && uploaded[i] == kNullInstruction[0] && uploaded[i + 1] == kNullInstruction[1] &&
        uploaded[i + 2] == kNullInstruction[2]) {
      continue;  // an export the pixel shader does not read, removed by D3D
    }
    return false;
  }
  return true;
}

struct Bytes {
  const std::vector<uint8_t>& o;
  bool Has(size_t pos, size_t n) const { return pos <= o.size() && n <= o.size() - pos; }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// The 2008 container (XenosRecomp shader.h): header +4 virtual size, +8 physical size, +16 constant
// table, +24 shader header. The shader header: +0 physical offset of the microcode, +4 its size in
// bytes; vertex shaders: +24 count of leading words, +28 vertex element count, elements from +36
// after the leading words; pixel shaders: +28 outputs. Returns the reason if it cannot be read.
const char* Read(const Shader& shader, ShaderEntry& e) {
  const Bytes c{shader.original};
  if (!c.Has(0, 36)) return "container too short";
  const uint32_t virtual_size = c.U32(4);
  const uint32_t table = c.U32(16);
  const uint32_t header = c.U32(24);
  if (!c.Has(header, shader.vertices ? 36 : 32) || header + 36 > virtual_size) return "shader header outside";
  const uint32_t code_offset = c.U32(header), code_size = c.U32(header + 4);
  if (!code_size || code_size % 4 || !c.Has(size_t(virtual_size) + code_offset, code_size)) {
    return "microcode outside the container";
  }
  e.vertex = shader.vertices;
  e.microcode.resize(code_size / 4);
  for (size_t i = 0; i < e.microcode.size(); ++i) {
    e.microcode[i] = c.U32(size_t(virtual_size) + code_offset + i * 4);
  }
  e.shader_microcode = e.microcode;
  if (e.vertex) {
    const uint32_t leading = c.U32(header + 24);
    const uint32_t count = c.U32(header + 28);
    const size_t start = size_t(header) + 36 + size_t(leading) * 4;
    if (count > 64 || leading > 1024 || start + size_t(count) * 4 > virtual_size) {
      return "vertex elements outside";
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = c.U32(start + size_t(i) * 4);
      VertexElement element;
      element.instruction = uint16_t(v & 0xFFF);
      element.usage = uint8_t((v >> 12) & 0xF);
      element.usage_index = uint8_t((v >> 16) & 0xF);
      if ((size_t(element.instruction) + 1) * 3 > e.microcode.size()) return "vertex element outside the microcode";
      e.elements.push_back(element);
      for (size_t j = 0; j < 3; ++j) e.microcode[size_t(element.instruction) * 3 + j] &= kFetchKeep[j];
    }
  } else {
    e.outputs = c.U32(header + 28);
  }
  // Constant table (after its 4-byte size): only the samplers.
  if (table && c.Has(size_t(table) + 4, 28)) {
    const size_t base = size_t(table) + 4;
    const uint32_t constants = c.U32(base + 12), info = c.U32(base + 16);
    if (constants <= 1024 && c.Has(base + info, size_t(constants) * 20)) {
      for (uint32_t i = 0; i < constants; ++i) {
        const size_t p = base + info + size_t(i) * 20;
        if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
        ShaderSampler sampler;
        sampler.reg = c.U16(p + 6);
        const uint32_t type = c.U32(p + 12);
        sampler.type = c.Has(base + type, 4) ? c.U16(base + type + 2) : 0;
        e.samplers.push_back(sampler);
      }
    }
  }
  e.hash = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
  return nullptr;
}

struct RawKey {
  uint64_t hash;
  uint32_t words;
  bool vertex;
  bool operator==(const RawKey&) const = default;
};
struct RawKeyHash {
  size_t operator()(const RawKey& k) const { return size_t(k.hash ^ (uint64_t(k.words) << 1) ^ uint64_t(k.vertex)); }
};

}  // namespace

struct Shaders::Data {
  LibraryShaders library;
  std::vector<ShaderEntry> entries;
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidates;  // (vertex, words) -> entries
  std::unordered_map<RawKey, const ShaderEntry*, RawKeyHash> cache;
  std::vector<uint32_t> scratch;
  ShaderStats stats;
  uint32_t warnings = 0;
  bool loaded = false;
};

Shaders::Shaders() : data_(std::make_unique<Data>()) {}
Shaders::~Shaders() = default;
bool Shaders::loaded() const { return data_->loaded; }
size_t Shaders::size() const { return data_->entries.size(); }
ShaderStats Shaders::Stats() const { return data_->stats; }

bool Shaders::Load(const std::filesystem::path& file) {
  Data& d = *data_;
  try {
    d.library.Load(file);
  } catch (const std::exception& e) {
    REXLOG_WARN("[native] shader library not available ({}): {}", file.string(), e.what());
    return false;
  }
  const auto& shaders = d.library.shaders();
  d.entries.clear();
  d.entries.reserve(shaders.size());
  uint32_t vertex = 0, pixel = 0, skipped = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    ShaderEntry e;
    e.shader = &shaders[i];
    e.number = i;
    if (const char* reason = Read(shaders[i], e)) {
      if (++skipped <= 8) REXLOG_WARN("[native] library container {} skipped: {}", i, reason);
      continue;
    }
    (e.vertex ? vertex : pixel) += 1;
    d.entries.push_back(std::move(e));
  }
  d.candidates.clear();
  for (uint32_t i = 0; i < d.entries.size(); ++i) {
    d.candidates[{d.entries[i].vertex, uint32_t(d.entries[i].microcode.size())}].push_back(i);
  }
  d.cache.clear();
  d.loaded = !d.entries.empty();
  REXLOG_INFO("[native] shader library {}: {} shaders ({} vertex, {} pixel), {} skipped", file.string(),
              d.entries.size(), vertex, pixel, skipped);
  return d.loaded;
}

const ShaderEntry* Shaders::Identify(bool vertex, std::span<const uint32_t> microcode) {
  Data& d = *data_;
  ++d.stats.loads;
  const RawKey key{XXH3_64bits(microcode.data(), microcode.size_bytes()), uint32_t(microcode.size()), vertex};
  if (auto it = d.cache.find(key); it != d.cache.end()) return it->second;
  ++d.stats.distinct;
  const ShaderEntry* chosen = nullptr;
  uint32_t matches = 0;
  if (auto c = d.candidates.find({vertex, uint32_t(microcode.size())}); c != d.candidates.end()) {
    for (uint32_t index : c->second) {
      const ShaderEntry& e = d.entries[index];
      d.scratch.assign(microcode.begin(), microcode.end());
      for (const VertexElement& element : e.elements) {
        for (size_t j = 0; j < 3; ++j) d.scratch[size_t(element.instruction) * 3 + j] &= kFetchKeep[j];
      }
      if (XXH3_64bits(d.scratch.data(), d.scratch.size() * sizeof(uint32_t)) != e.hash || d.scratch != e.microcode) {
        continue;
      }
      if (!chosen) chosen = &e;
      ++matches;
    }
  }
  // Tolerant pass for vertex shaders (FH1's D3D patches, see LooseMatch).
  if (!chosen && vertex) {
    if (auto c = d.candidates.find({vertex, uint32_t(microcode.size())}); c != d.candidates.end()) {
      for (uint32_t index : c->second) {
        if (!LooseMatch(d.entries[index], microcode)) continue;
        if (!chosen) chosen = &d.entries[index];
        ++matches;
      }
    }
    if (chosen) ++d.stats.loose;
  }
  if (chosen) {
    ++d.stats.identified;
    ++(vertex ? d.stats.identified_vertex : d.stats.identified_pixel);
    if (matches > 1) ++d.stats.ambiguous;
  } else {
    ++d.stats.unidentified;
    ++(vertex ? d.stats.unidentified_vertex : d.stats.unidentified_pixel);
    if (d.warnings < kMaxWarnings) {
      ++d.warnings;
      const auto c = d.candidates.find({vertex, uint32_t(microcode.size())});
      // Diagnostics: the closest library shader of that type and length, and the words that differ
      // (uploaded / library, after masking the declared fetches).
      std::string detail;
      if (c != d.candidates.end()) {
        size_t best_diff = SIZE_MAX;
        const ShaderEntry* best = nullptr;
        for (uint32_t index : c->second) {
          const ShaderEntry& e = d.entries[index];
          d.scratch.assign(microcode.begin(), microcode.end());
          for (const VertexElement& element : e.elements) {
            for (size_t j = 0; j < 3; ++j) d.scratch[size_t(element.instruction) * 3 + j] &= kFetchKeep[j];
          }
          size_t diff = 0;
          for (size_t i = 0; i < d.scratch.size(); ++i) diff += d.scratch[i] != e.microcode[i];
          if (diff < best_diff) {
            best_diff = diff;
            best = &e;
          }
        }
        if (best) {
          d.scratch.assign(microcode.begin(), microcode.end());
          for (const VertexElement& element : best->elements) {
            for (size_t j = 0; j < 3; ++j) d.scratch[size_t(element.instruction) * 3 + j] &= kFetchKeep[j];
          }
          detail = fmt::format("; closest n{}: {} words differ:", best->number, best_diff);
          uint32_t shown = 0;
          for (size_t i = 0; i < d.scratch.size() && shown < 10; ++i) {
            if (d.scratch[i] != best->microcode[i]) {
              detail += fmt::format(" {}:{:08X}/{:08X}", i, d.scratch[i], best->microcode[i]);
              ++shown;
            }
          }
          std::string fetches;
          for (const VertexElement& element : best->elements) fetches += fmt::format(" {}", element.instruction);
          detail += "; declared fetch instructions:" + fetches;
        }
      }
      REXLOG_WARN("[native] {} shader not identified: {} words ({} library shaders of that type and length){}",
                  vertex ? "vertex" : "pixel", microcode.size(), c != d.candidates.end() ? c->second.size() : 0,
                  detail);
    }
  }
  d.cache.emplace(key, chosen);
  return chosen;
}

}  // namespace fh1::native
