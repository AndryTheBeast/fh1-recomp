// nfsmw - native renderer, part C5a (see nfsmw_native_shaders.h).
//
// 2005 container format: see Read(). All fields are big-endian.

#include "nfsmw_native_shaders.h"

#include "nfsmw_shader_library.h"

#include <rex/logging.h>

#include <algorithm>
#include <exception>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read keys[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would "
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::native {
namespace {

// Bits of a vertex fetch instruction that D3D does not touch
// (VertexFetchInstruction in XenosRecomp shader_code.h): opcode, source and
// destination registers, destination swizzle and predicate. The rest (fetch
// constant, format, stride, offset and numeric modes) comes from the declaration.
constexpr uint32_t kFetchKeeps[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};

constexpr uint32_t kMaxWarnings = 48;

constexpr const char* kUses[] = {"position", "peso",     "indices",    "normal",
                                 "tam_punto", "texcoord", "tangente",   "binormal",
                                 "teselado",  "position_t", "color",    "niebla",
                                 "depth", "sample", "use14",     "use15"};

struct Container {
  const std::vector<uint8_t>& o;

  bool ThereIs(size_t position, size_t bytes) const {
    return position <= o.size() && bytes <= o.size() - position;
  }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// nfsmw_native_shadow_minimum. The zero-terminated string starting at `position` is `name`.
bool NameIs(const Container& c, size_t position, const char* name) {
  const size_t n = std::strlen(name);
  return c.ThereIs(position, n + 1) && std::memcmp(c.o.data() + position, name, n) == 0 && c.o[position + n] == 0;
}

// Reads what is needed from the 2005 container, which is not the one in XenosRecomp
// shader.h (that is the 2008 one): 24-byte header with signature, virtual part,
// physical part, definitions (+12), CTAB (+16) and shader header (+20). The
// microcode is the whole physical part. Reference: docs/shaders.md and
// shaders/nfsmw_container_2005.h (Convert2005). Returns the reason if it cannot be
// read.
const char* Read(const nfsmw::native::Shader& shader, EntryShader& e) {
  const Container c{shader.original};
  if (!c.ThereIs(0, 24)) return "container demasiado short";
  const uint32_t virtuales = c.U32(4);
  const uint32_t physical = c.U32(8);
  const uint32_t table = c.U32(16);
  const uint32_t header = c.U32(20);
  if (!physical || (physical % 4) || !c.ThereIs(virtuales, physical)) {
    return "microcode outside the container";
  }
  if (header >= virtuales || size_t(header) + (shader.vertices ? 40 : 32) > virtuales) {
    return "shader header outside the virtual part";
  }

  e.vertices = shader.vertices;
  e.microcode.resize(physical / 4);
  for (size_t i = 0; i < e.microcode.size(); ++i) {
    e.microcode[i] = c.U32(size_t(virtuales) + i * 4);
  }

  if (e.vertices) {
    // List at +0x28, after skipping the words at +0x18; +0x1C = element count.
    const uint32_t previous = c.U32(header + 24);
    const uint32_t amount = c.U32(header + 28);
    const size_t start_offset = size_t(header) + 40 + size_t(previous) * 4;
    if (amount > 64 || previous > 1024 || start_offset + size_t(amount) * 4 > virtuales) {
      return "vertex elements outside the virtual part";
    }
    for (uint32_t i = 0; i < amount; ++i) {
      const uint32_t input_value = c.U32(start_offset + size_t(i) * 4);
      ElementVertex element;
      element.instruction = uint16_t(input_value & 0xFFF);
      element.use = uint8_t((input_value >> 12) & 0xF);
      element.index_use = uint8_t((input_value >> 16) & 0xF);
      if ((size_t(element.instruction) + 1) * 3 > e.microcode.size()) {
        return "vertex element points outside the microcode";
      }
      e.elements.push_back(element);
      for (size_t j = 0; j < 3; ++j) {
        e.microcode[size_t(element.instruction) * 3 + j] &= kFetchKeeps[j];
      }
    }
  } else {
    if (!c.ThereIs(header + 24, 8)) return "short pixel shader header";
    e.outputs = c.U32(header + 28);
  }

  // Constant table: only the samplers (register and type).
  if (!table || !c.ThereIs(table + 4, 28)) return "no constant table";
  const size_t base = size_t(table) + 4;
  const uint32_t constants = c.U32(base + 12);
  const uint32_t info = c.U32(base + 16);
  if (constants > 1024 || !c.ThereIs(base + info, size_t(constants) * 20)) {
    return "constant table outside the container";
  }
  // Float registers the SPIR-V reads: the highest in the table, or up to the end of the
  // buffer if there is an array with relative indexing (XenosRecomp shader_recompiler.cpp:
  // 1172-1183: tailCount = 256 in VS and 224 in PS).
  uint32_t registers_float = 0;
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) == 2) {  // RegisterSet::Float4
      const uint32_t index = c.U16(p + 6);
      const uint32_t how_many_2 = c.U16(p + 8);
      registers_float = std::max(registers_float, how_many_2 > 1 ? (e.vertices ? 256u : 224u)
                                                              : index + 1);
    }
  }
  e.constants_bytes = std::min<uint32_t>(std::max<uint32_t>(registers_float, 1), 256) * 16;
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    SamplerShader sampler;
    sampler.reg_entry = c.U16(p + 6);
    const uint32_t type = c.U32(p + 12);
    sampler.type = c.ThereIs(base + type, 4) ? c.U16(base + type + 2) : 0;
    // nfsmw_native_shadow_minimum. The name is at that distance from the start of the table, like the type
    // (XenosRecomp shader_recompiler.cpp: constantTableData + constantInfo->name).
    sampler.map_shadows = NameIs(c, base + size_t(c.U32(p)), "SHADOWMAP_SAMPLER");
    e.samplers.push_back(sampler);
  }

  e.fingerprint = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
  return nullptr;
}

struct KeyRaw {
  uint64_t fingerprint;
  uint32_t words;
  bool vertices;
  bool operator==(const KeyRaw&) const = default;
};

struct HashKeyRaw {
  size_t operator()(const KeyRaw& c) const {
    return size_t(c.fingerprint ^ (uint64_t(c.words) << 1) ^ uint64_t(c.vertices));
  }
};

}  // namespace

struct ShadersNative::Data {
  nfsmw::native::LibraryShaders library;
  std::vector<EntryShader> entries;
  std::unordered_map<const nfsmw::native::Shader*, uint32_t> por_shader;
  // (vertices, words) -> entries candidates_2.
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidates;
  std::unordered_map<KeyRaw, const EntryShader*, HashKeyRaw> cache;
  std::vector<uint32_t> temporal;
  StatisticsShaders statistics;
  uint32_t warnings = 0;
  bool loaded = false;
};

ShadersNative::ShadersNative() : data_(std::make_unique<Data>()) {}
ShadersNative::~ShadersNative() = default;

bool ShadersNative::loaded() const {
  return data_->loaded;
}

bool FetchCoherent(const EntryShader& vs, std::span<const uint32_t> patched) {
  if (!vs.vertices || patched.size() != vs.microcode.size()) {
    return false;
  }
  for (const ElementVertex& element : vs.elements) {
    const uint32_t reg_entry = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
    bool found = false;
    for (const ElementVertex& other : vs.elements) {
      const uint32_t d0 = patched[size_t(other.instruction) * 3];
      if (((d0 >> 12) & 0x3F) == reg_entry && (d0 & 0x1F) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

// nfsmw_native_shadow_minimum. The library has tfetch2DShadowMin if the pixel shader's SPIR-V contains the
// NFSMW_MARK_SHADOW_MINIMUM constant from shader_common.h (a 32-bit OpConstant): nothing else uses it.
constexpr uint32_t kMarkShadowMinimum = 0x5E3B1A84u;
static bool HasMarkShadowMinimum(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return false;
  }
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    if (words == 0 || i + words > spirv.size()) {
      return false;
    }
    if ((spirv[i] & 0xFFFF) == 43 && words == 4 && spirv[i + 3] == kMarkShadowMinimum) {
      return true;
    }
    i += words;
  }
  return false;
}

// OpKill, OpTerminateInvocation and OpDemoteToHelperInvocation in the module. If the SPIR-V cannot be walked
// it returns 2, the conservative answer (the fragment stage is kept).
static uint32_t CountKills(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return 2;
  }
  uint32_t kills = 0;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return 2;
    }
    if (code == 252 || code == 4416 || code == 5380) {
      ++kills;
    }
    i += words;
  }
  return kills;
}

bool ShadersNative::Load(const std::filesystem::path& file) {
  Data& d = *data_;
  try {
    d.library.Load(file);
  } catch (const std::exception& e) {
    REXLOG_WARN("[native] C5a: shader library not available ({}): {}", file.string(),
                e.what());
    return false;
  }
  const auto& shaders = d.library.shaders();
  uint32_t con_kill = 0, sin_kill = 0;
  uint32_t with_map_shadows = 0, with_minimum = 0;  // nfsmw_native_shadow_minimum
  d.entries.clear();
  d.entries.reserve(shaders.size());
  uint32_t vertex = 0, pixel = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    EntryShader e;
    e.shader = &shaders[i];
    e.number = i;
    if (const char* reason = Read(shaders[i], e)) {
      REXLOG_WARN("[native] C5a: library container {} ignored: {}", i, reason);
      continue;
    }
    if (!e.vertices) {
      e.kills = CountKills(shaders[i].spirv);
      e.discards = e.kills != 1;                // 1 = only the alpha test one
      (e.discards ? con_kill : sin_kill) += 1;
      // nfsmw_native_shadow_minimum. Whether its SPIR-V can take the shadow map minimum.
      e.shadow_minimum = HasMarkShadowMinimum(shaders[i].spirv);
      for (const SamplerShader& s : e.samplers) {
        if (s.map_shadows) {
          ++with_map_shadows;
          with_minimum += e.shadow_minimum ? 1 : 0;
          break;
        }
      }
    }
    (e.vertices ? vertex : pixel) += 1;
    d.entries.push_back(std::move(e));
  }
  d.candidates.clear();
  d.por_shader.clear();
  std::map<std::tuple<bool, uint32_t, uint64_t>, std::vector<uint32_t>> groups;
  for (uint32_t i = 0; i < d.entries.size(); ++i) {
    const EntryShader& e = d.entries[i];
    const uint32_t words = uint32_t(e.microcode.size());
    d.candidates[{e.vertices, words}].push_back(i);
    d.por_shader[e.shader] = i;
    groups[{e.vertices, words, e.fingerprint}].push_back(i);
  }
  uint32_t repeated = 0, repeated_different = 0;
  for (const auto& [key, members] : groups) {
    if (members.size() < 2) continue;
    ++repeated;
    const auto& spirv = d.entries[members[0]].shader->spirv;
    for (uint32_t m : members) {
      if (d.entries[m].shader->spirv != spirv) {
        ++repeated_different;
        break;
      }
    }
  }
  d.cache.clear();
  d.loaded = !d.entries.empty();
  REXLOG_INFO("[native] C5a: library with {} shaders ({} vertex, {} pixel); {} groups with the same microcode, {} "
              "of them with different SPIR-V",
              d.entries.size(), vertex, pixel, repeated, repeated_different);
  REXLOG_INFO("[native] C5a: pixel shaders that can discard pixels {} of {} (the rest only have the alpha-test "
              "kill: with no color to write, their stage can be removed)",
              con_kill, con_kill + sin_kill);
  REXLOG_INFO("[native] C5a: shadow by minimum (build 184): {} pixel shaders sample the shadow map "
              "(SHADOWMAP_SAMPLER) and {} have tfetch2DShadowMin{}",
              with_map_shadows, with_minimum,
              with_minimum && with_minimum == with_map_shadows
                  ? " (library with the minimum)"
                  : " (without the minimum: the shadow map is still copied as always)");
  return d.loaded;
}

const EntryShader* ShadersNative::Identify(bool vertices,
                                                 std::span<const uint32_t> microcode) {
  Data& d = *data_;
  ++d.statistics.loads;
  const KeyRaw key{XXH3_64bits(microcode.data(), microcode.size_bytes()),
                         uint32_t(microcode.size()), vertices};
  if (auto it = d.cache.find(key); it != d.cache.end()) {
    return it->second;
  }
  ++d.statistics.different;

  const EntryShader* chosen = nullptr;
  uint32_t matches = 0;
  if (auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      c != d.candidates.end()) {
    for (uint32_t index : c->second) {
      const EntryShader& e = d.entries[index];
      d.temporal.assign(microcode.begin(), microcode.end());
      for (const ElementVertex& element : e.elements) {
        for (size_t j = 0; j < 3; ++j) {
          d.temporal[size_t(element.instruction) * 3 + j] &= kFetchKeeps[j];
        }
      }
      if (XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t)) != e.fingerprint ||
          d.temporal != e.microcode) {
        continue;
      }
      if (!chosen) {
        chosen = &e;
      }
      ++matches;
    }
  }

  // Vertex shaders arrive patched: a mismatch is normal.
  const bool notify = d.warnings < kMaxWarnings && !vertices;
  if (chosen) {
    ++d.statistics.identified;
    if (matches > 1) {
      ++d.statistics.ambiguous;
    }
    if (notify && vertices) {
      // What D3D has patched in each fetch: it becomes the vertex input.
      ++d.warnings;
      std::string detail;
      for (const ElementVertex& element : chosen->elements) {
        const size_t p = size_t(element.instruction) * 3;
        const uint32_t d0 = microcode[p], d1 = microcode[p + 1], d2 = microcode[p + 2];
        const uint32_t fetch = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
        const int32_t offset = int32_t(d2 << 1) >> 9;
        detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}{}", kUses[element.use & 0xF],
                               element.index_use, fetch, (d1 >> 16) & 0x3F, d2 & 0xFF, offset,
                               ((d1 >> 30) & 0x1) ? "/mini" : "");
      }
      REXLOG_INFO("[native] C5a: VS n{} ({} words, {} matches):{}", chosen->number,
                  microcode.size(), matches, detail);
    }
  } else {
    ++d.statistics.without_identify;
    if (notify) {
      ++d.warnings;
      const auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      REXLOG_WARN("[native] C5a: {} shader not identified: {} words, fingerprint {:016X} ({} containers of that "
                  "type and length)",
                  vertices ? "vertex" : "pixel", microcode.size(), key.fingerprint,
                  c != d.candidates.end() ? c->second.size() : 0);
      // Diagnostics: which words change against the containers of the same
      // length (incoming / container original, without the mask).
      for (size_t k = 0; c != d.candidates.end() && k < c->second.size() && k < 2; ++k) {
        const EntryShader& e = d.entries[c->second[k]];
        const auto& o = e.shader->original;
        const size_t virtuales = size_t(o[4]) << 24 | size_t(o[5]) << 16 | size_t(o[6]) << 8 | o[7];
        std::string differences;
        uint32_t how_many = 0;
        for (size_t i = 0; i < microcode.size(); ++i) {
          uint32_t incoming = microcode[i];
          for (const ElementVertex& element : e.elements) {
            const size_t p = size_t(element.instruction) * 3;
            if (i >= p && i < p + 3) {
              incoming &= kFetchKeeps[i - p];
            }
          }
          if (incoming == e.microcode[i]) {
            continue;
          }
          if (++how_many <= 24) {
            const size_t b = virtuales + i * 4;
            const uint32_t original = uint32_t(o[b]) << 24 | uint32_t(o[b + 1]) << 16 |
                                      uint32_t(o[b + 2]) << 8 | o[b + 3];
            differences += fmt::format(" {}:{:08X}/{:08X}", i, microcode[i], original);
          }
        }
        std::string fetch;
        for (const ElementVertex& element : e.elements) {
          fetch += fmt::format(" {}", element.instruction);
        }
        REXLOG_WARN("[native] C5a:   against n{}: {} different words; fetch in the instructions{}:{}",
                    e.number, how_many, fetch, differences);
      }
    }
  }
  d.cache.emplace(key, chosen);
  return chosen;
}

const EntryShader* ShadersNative::IdentifyContainer(
    std::span<const uint8_t> container) const {
  const Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }
  const nfsmw::native::Shader* shader = d.library.Find(container);
  if (!shader) {
    return nullptr;
  }
  const auto it = d.por_shader.find(shader);
  return it != d.por_shader.end() ? &d.entries[it->second] : nullptr;
}

// Entries are in library order (Load), with gaps where a container was skipped.
const EntryShader* ShadersNative::ByNumber(uint32_t number) const {
  const Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }
  const auto it = std::lower_bound(d.entries.begin(), d.entries.end(), number,
                                   [](const EntryShader& e, uint32_t n) { return e.number < n; });
  return it != d.entries.end() && it->number == number ? &*it : nullptr;
}

const char* NameUse(uint8_t use) {
  return kUses[use & 0xF];
}

StatisticsShaders ShadersNative::Statistics() const {
  return data_->statistics;
}

}  // namespace nfsmw::native
