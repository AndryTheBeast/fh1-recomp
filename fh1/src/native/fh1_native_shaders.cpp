// fh1 - native renderer, part C5a (see fh1_native_shaders.h).
//
// 2005 container format: see Read(). All fields are big-endian.

#include "fh1_native_shaders.h"

#include "fh1_extra_shaders.h"
#include "fh1_shader_library.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <filesystem>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read keys[4] before writing it and the same texture was
 * created several times (see docs/nfsmw-nx/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would "
       "come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_STRING(fh1_dump_ring_shaders, "", "FH1",
                      "Tools: folder where every shader the library does not know is written (p_/v_<hash>.mc: type byte, "
                      "big-endian word count, big-endian microcode words), to build containers from the microcode alone")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fh1::native {
namespace {

// NFSC: writes the microcode of a shader the library does not know (once per hash).
void DumpMicrocodeUnknown(bool vertices, uint64_t fingerprint, std::span<const uint32_t> microcode) {
  const std::string dir = REXCVAR_GET(fh1_dump_ring_shaders);
  if (dir.empty()) return;
  static std::unordered_map<uint64_t, bool> done;
  if (!done.emplace(fingerprint, true).second) return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%c_%016llX.mc", vertices ? 'v' : 'p', static_cast<unsigned long long>(fingerprint));
  std::vector<uint8_t> data;
  data.push_back(vertices ? 'v' : 'p');
  const uint32_t n = uint32_t(microcode.size());
  for (int sh = 24; sh >= 0; sh -= 8) data.push_back(uint8_t(n >> sh));
  for (uint32_t w : microcode) {
    for (int sh = 24; sh >= 0; sh -= 8) data.push_back(uint8_t(w >> sh));
  }
  if (FILE* f = std::fopen((std::filesystem::path(dir) / name).string().c_str(), "wb")) {
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
}

// Bits of a vertex fetch instruction that D3D does not touch
// (VertexFetchInstruction in XenosRecomp shader_code.h): opcode, source and
// destination registers, destination swizzle and predicate. The rest (fetch
// constant, format, stride, offset and numeric modes) comes from the declaration.
constexpr uint32_t kFetchKeeps[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};

constexpr uint32_t kMaxWarnings = 120;

constexpr const char* kUses[] = {"position", "peso",     "indices",    "normal",
                                 "tam_punto", "texcoord", "tangente",   "binormal",
                                 "tessfactor",  "position_t", "color",    "fog",
                                 "depth", "sample", "usage14",     "usage15"};

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

// fh1_native_shadow_minimum. The zero-terminated string starting at `position` is `name`.
bool NameIs(const Container& c, size_t position, const char* name) {
  const size_t n = std::strlen(name);
  return c.ThereIs(position, n + 1) && std::memcmp(c.o.data() + position, name, n) == 0 && c.o[position + n] == 0;
}

// Reads what is needed from the 2005 container, which is not the one in XenosRecomp
// shader.h (that is the 2008 one): 24-byte header with signature, virtual part,
// physical part, definitions (+12), CTAB (+16) and shader header (+20). The
// microcode is the whole physical part. Reference: docs/nfsmw-nx/shaders.md and
// shaders/fh1_container_2005.h (Convert2005). Returns the reason if it cannot be
// read.
// NFSC: Carbon's Direct3D also REORDERS the vertex fetches of a vertex shader. For identification the fetch
// instructions (the library's element instructions) are compared as a sorted set: their three words are put in
// sorted order within the slots the elements occupy. The attribute mapping later finds each fetch by its destination
// register, so it does not depend on the order.
void CanonicalizeFetches(std::vector<uint32_t>& m, std::vector<uint32_t> slots) {
  std::sort(slots.begin(), slots.end());
  std::vector<std::array<uint32_t, 3>> triples;
  triples.reserve(slots.size());
  for (uint32_t i : slots) triples.push_back({m[size_t(i) * 3], m[size_t(i) * 3 + 1], m[size_t(i) * 3 + 2]});
  std::sort(triples.begin(), triples.end());
  for (size_t k = 0; k < slots.size(); ++k) {
    for (size_t j = 0; j < 3; ++j) m[size_t(slots[k]) * 3 + j] = triples[k][j];
  }
}

// FH1 (measured 2026-10-02, from the FH1-only renderer fh1_native_shaders.cpp): its Direct3D also nulls the exports
// of outputs the pixel shader does not read (this instruction), and patches fetches the declaration does not list
// (the cars' extra streams) like the declared ones. Tolerant pass: fetches compared only by opcode and registers.
constexpr uint32_t kFetchKeepsTolerant[3] = {0x0007FFFF, 0x80000000, 0x80000000};
constexpr uint32_t kInstructionNull[3] = {0xC8000000, 0x00000000, 0x02000000};

bool MatchesFh1(const EntryShader& e, std::span<const uint32_t> uploaded) {
  const std::vector<uint32_t>& original = e.microcode_shader;
  if (uploaded.size() != original.size()) return false;
  std::vector<uint8_t> fetch(uploaded.size() / 3 + 1, 0);
  for (const ElementVertex& element : e.elements) fetch[element.instruction] = 1;
  for (size_t i = 0; i < uploaded.size(); i += 3) {
    const size_t n = std::min<size_t>(3, uploaded.size() - i);
    const bool fetch_undeclared = n == 3 && !fetch[i / 3] && (original[i] & 0x1F) == 0 && ((original[i] >> 19) & 1) &&
                                  (uploaded[i] & kFetchKeepsTolerant[0]) == (original[i] & kFetchKeepsTolerant[0]);
    bool equal = true;
    for (size_t j = 0; j < n; ++j) {
      uint32_t a = uploaded[i + j], b = original[i + j];
      if (fetch[i / 3] || fetch_undeclared) {
        a &= kFetchKeepsTolerant[j];
        b &= kFetchKeepsTolerant[j];
      }
      equal = equal && a == b;
    }
    if (equal) continue;
    if (e.vertices && n == 3 && uploaded[i] == kInstructionNull[0] && uploaded[i + 1] == kInstructionNull[1] &&
        uploaded[i + 2] == kInstructionNull[2]) {
      continue;
    }
    return false;
  }
  return true;
}

const char* Read(const fh1::native::Shader& shader, EntryShader& e) {
  const Container c{shader.original};
  if (!c.ThereIs(0, 36)) return "container too short";
  // NFSC: Carbon's containers are the 2008 layout (signature 0x102A11xx): the shader header offset is at +24
  // (not +20) and the vertex elements follow the 0x24-byte vertex header directly. The rest is the same.
  const bool es2008 = (c.U32(0) & 0xFFFFFFFEu) == 0x102A1100u;
  const uint32_t virtuales = c.U32(4);
  const uint32_t physical = c.U32(8);
  const uint32_t table = c.U32(16);
  const uint32_t header = c.U32(es2008 ? 24 : 20);
  if (!physical || (physical % 4) || !c.ThereIs(virtuales, physical)) {
    return "microcode outside the container";
  }
  if (header >= virtuales || size_t(header) + (shader.vertices ? 40 : 32) > virtuales) {
    return "shader header outside the virtual part";
  }

  e.vertices = shader.vertices;
  // FH1: in its 2008 containers the physical part also holds other data; the shader header says where the
  // microcode is (+0 offset in the physical part, +4 size in bytes). NFSC's containers start with it.
  size_t code_start = 0, code_bytes = physical;
  if (es2008) {
    code_start = c.U32(header);
    code_bytes = c.U32(header + 4);
    if (!code_bytes || (code_bytes % 4) || code_start + code_bytes > physical) {
      return "microcode outside the physical part (2008 header)";
    }
  }
  e.microcode.resize(code_bytes / 4);
  for (size_t i = 0; i < e.microcode.size(); ++i) {
    e.microcode[i] = c.U32(size_t(virtuales) + code_start + i * 4);
  }
  e.microcode_shader = e.microcode;

  if (e.vertices) {
    // List at +0x28, after skipping the words at +0x18; +0x1C = element count.
    const uint32_t previous = c.U32(header + 24);  // field18: words before the element list (both layouts)
    const uint32_t amount = c.U32(header + 28);
    const size_t start_offset = size_t(header) + (es2008 ? 36 : 40) + size_t(previous) * 4;
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
      registers_float = std::max(registers_float, how_many_2 > 1 ? 256u
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
    // fh1_native_shadow_minimum. The name is at that distance from the start of the table, like the type
    // (XenosRecomp shader_recompiler.cpp: constantTableData + constantInfo->name).
    sampler.map_shadows = NameIs(c, base + size_t(c.U32(p)), "SHADOWMAP_SAMPLER");
    e.samplers.push_back(sampler);
  }
  // FH1: some shaders fetch from texture registers their constant table does not list (the translator names them
  // s<N>): the UI's two-layer shader reads its second layer that way, and unbound it read as transparent black,
  // which made every box behind the menu and loading-screen text invisible. The microcode says which registers
  // are fetched: walk the control flow (pairs of 48-bit instructions in three words) and, in each exec block,
  // the instructions its sequence bits mark as fetches.
  {
    const std::vector<uint32_t>& m = e.microcode_shader;
    size_t first_instruction = m.size() / 3;
    for (size_t pair = 0; pair < first_instruction && pair * 3 + 2 < m.size(); ++pair) {
      const uint32_t w0 = m[pair * 3], w1 = m[pair * 3 + 1], w2 = m[pair * 3 + 2];
      const uint32_t low[2] = {w0, (w1 >> 16) | (w2 << 16)};
      const uint32_t high[2] = {w1 & 0xFFFF, w2 >> 16};
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t opcode = (high[k] >> 12) & 0xF;
        // kExec 1, kExecEnd 2, kCondExec 3, kCondExecEnd 4, kCondExecPred 5, kCondExecPredEnd 6,
        // kCondExecPredClean 13, kCondExecPredCleanEnd 14.
        if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
        const uint32_t address = low[k] & 0xFFF, count = (low[k] >> 12) & 0x7, sequence = (low[k] >> 16) & 0xFFF;
        if (count) first_instruction = std::min<size_t>(first_instruction, address);
        for (uint32_t i = 0; i < count; ++i) {
          const size_t p = (size_t(address) + i) * 3;
          if (!((sequence >> (i * 2)) & 0x1) || p + 2 >= m.size()) continue;
          if ((m[p] & 0x1F) != 1) continue;  // kTextureFetch
          const uint16_t reg = uint16_t((m[p] >> 20) & 0x1F);
          const uint32_t dimension = (m[p + 2] >> 14) & 0x3;
          bool listed = false;
          for (const SamplerShader& s : e.samplers) listed = listed || s.reg_entry == reg;
          if (listed) continue;
          SamplerShader sampler;
          sampler.reg_entry = reg;
          sampler.type = dimension == 3 ? 14 : dimension == 2 ? 13 : dimension == 0 ? 11 : 12;
          e.samplers.push_back(sampler);
        }
      }
    }
  }

  e.fingerprint = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
  e.microcode_compare = e.microcode;
  for (const ElementVertex& element : e.elements) {
    e.microcode_compare[size_t(element.instruction) * 3 + 1] &= ~uint32_t(0xFFF);
  }
  {
    std::vector<uint32_t> slots_value;
    for (const ElementVertex& element : e.elements) slots_value.push_back(element.instruction);
    if (!slots_value.empty()) CanonicalizeFetches(e.microcode_compare, slots_value);
  }
  e.fingerprint_compare = XXH3_64bits(e.microcode_compare.data(), e.microcode_compare.size() * sizeof(uint32_t));
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

// Shaders made on this PC while the game runs (fh1_extra_shaders.h) are added to the entries by the ring
// thread. Room for them is reserved at Load, so an entry never moves: pointers to entries stay good.
constexpr size_t kMaxAddedEntries = 1024;

struct ShadersNative::Data {
  fh1::native::LibraryShaders library;
  // The shaders made on this PC: the saved ones (read at Load) and this session's. A deque: adding one does
  // not move the others, and the entries point at them.
  std::deque<fh1::native::Shader> extras;
  uint32_t next_number = 0;
  // Other threads read the entries (ByNumber, IdentifyContainer) while the ring thread may add one.
  mutable std::shared_mutex mutex;
  std::vector<EntryShader> entries;
  std::unordered_map<const fh1::native::Shader*, uint32_t> por_shader;
  // (vertices, words) -> candidate entries.
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

// fh1_native_shadow_minimum. The library has tfetch2DShadowMin if the pixel shader's SPIR-V contains the
// FH1_MARK_SHADOW_MINIMUM constant from shader_common.h (a 32-bit OpConstant): nothing else uses it.
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
  // The library's shaders, then the ones earlier sessions made on this PC.
  d.extras.clear();
  for (fh1::native::Shader& s : extra_shaders::LoadSaved()) {
    // The container's fingerprint, as the library computes it for its own (fh1_shader_library.cpp). Without it
    // (0 until 2026-10-06) no pipeline record that uses such a shader could be matched in another session.
    s.fingerprint = XXH3_64bits(s.original.data(), s.original.size());
    d.extras.push_back(std::move(s));
  }
  std::vector<const fh1::native::Shader*> shaders;
  for (const fh1::native::Shader& s : d.library.shaders()) shaders.push_back(&s);
  for (const fh1::native::Shader& s : d.extras) shaders.push_back(&s);
  const size_t from_library = d.library.shaders().size();
  d.next_number = uint32_t(shaders.size());
  std::unique_lock<std::shared_mutex> lock(d.mutex);
  uint32_t con_kill = 0, sin_kill = 0;
  uint32_t with_map_shadows = 0, with_minimum = 0;  // fh1_native_shadow_minimum
  d.entries.clear();
  d.entries.reserve(shaders.size() + kMaxAddedEntries);
  uint32_t vertex = 0, pixel = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    EntryShader e;
    e.shader = shaders[i];
    e.number = i;
    if (const char* reason = Read(*shaders[i], e)) {
      REXLOG_WARN("[native] C5a: library container {} ignored: {}", i, reason);
      continue;
    }
    if (!e.vertices) {
      e.kills = CountKills(shaders[i]->spirv);
      e.discards = e.kills != 1;                // 1 = only the alpha test one
      (e.discards ? con_kill : sin_kill) += 1;
      // fh1_native_shadow_minimum. Whether its SPIR-V can take the shadow map minimum.
      e.shadow_minimum = HasMarkShadowMinimum(shaders[i]->spirv);
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
  REXLOG_INFO("[native] C5c: {} of those shaders were made on this PC by earlier sessions (shaders_extra); the "
              "library file has {}",
              shaders.size() - from_library, from_library);
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
  Adopt(d);
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
      // NFSC: ignore the destination swizzle D3D patches into the fetches (see EntryShader::fingerprint_compare).
      for (const ElementVertex& element : e.elements) {
        d.temporal[size_t(element.instruction) * 3 + 1] &= ~uint32_t(0xFFF);
      }
      if (!e.elements.empty()) {
        std::vector<uint32_t> slots_value;
        for (const ElementVertex& element : e.elements) slots_value.push_back(element.instruction);
        CanonicalizeFetches(d.temporal, slots_value);
      }
      if (XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t)) != e.fingerprint_compare ||
          d.temporal != e.microcode_compare) {
        continue;
      }
      if (!chosen) {
        chosen = &e;
      }
      ++matches;
    }
  }

  // FH1: tolerant pass (MatchesFh1) when the exact one finds nothing.
  if (!chosen) {
    if (auto c = d.candidates.find({vertices, uint32_t(microcode.size())}); c != d.candidates.end()) {
      for (uint32_t index : c->second) {
        const EntryShader& e = d.entries[index];
        if (!MatchesFh1(e, microcode)) continue;
        if (!chosen) chosen = &e;
        ++matches;
      }
      if (chosen) ++d.statistics.tolerant;
    }
  }

  // Vertex shaders arrive patched: a mismatch is normal.
  const bool notify = d.warnings < kMaxWarnings;  // NFSC: also vertex shaders (diagnostic, to see how Carbon patches them)
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
            (void)virtuales;
            const uint32_t original = e.microcode_shader[i];  // FH1: the microcode may not start the physical part
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
  if (!chosen) {
    DumpMicrocodeUnknown(vertices, key.fingerprint, microcode);
    // A helper thread makes it on this PC; Adopt takes it in when it is ready.
    extra_shaders::Request(vertices, key.fingerprint, microcode);
    // The game waits for it (a short pause, once per shader): some things are painted only once, and a stretch of
    // ground painted while its shader was being made stayed one flat color for the whole session.
    static thread_local bool waiting = false;
    if (!waiting && extra_shaders::Wait(vertices, key.fingerprint)) {
      waiting = true;
      const EntryShader* made = Identify(vertices, microcode);  // Adopt takes it in, then it is matched again
      waiting = false;
      return made;
    }
  }
  d.cache.emplace(key, chosen);
  return chosen;
}

// Ring thread: takes in the shaders the helper thread has finished (fh1_extra_shaders.h). Each becomes an
// entry like the library's; the unknown microcodes are forgotten so that the next upload is matched again.
void ShadersNative::Adopt(Data& d) {
  fh1::native::Shader made;
  while (d.loaded && extra_shaders::TakeFinished(made)) {
    if (d.entries.size() >= d.entries.capacity()) {
      REXLOG_WARN("[native] C5c: no room left for a shader made in this session: it is used from the next start");
      continue;
    }
    made.fingerprint = XXH3_64bits(made.original.data(), made.original.size());
    d.extras.push_back(std::move(made));
    EntryShader e;
    e.shader = &d.extras.back();
    e.number = d.next_number++;
    if (const char* reason = Read(*e.shader, e)) {
      REXLOG_WARN("[native] C5c: shader made on this PC ignored: {}", reason);
      continue;
    }
    if (!e.vertices) {
      e.kills = CountKills(e.shader->spirv);
      e.discards = e.kills != 1;
      e.shadow_minimum = HasMarkShadowMinimum(e.shader->spirv);
    }
    const bool vertices = e.vertices;
    const uint32_t words = uint32_t(e.microcode.size()), number = e.number;
    {
      std::unique_lock<std::shared_mutex> lock(d.mutex);
      const uint32_t index = uint32_t(d.entries.size());
      d.entries.push_back(std::move(e));
      d.candidates[{vertices, words}].push_back(index);
      d.por_shader[d.entries[index].shader] = index;
    }
    std::erase_if(d.cache, [](const auto& item) { return item.second == nullptr; });
    REXLOG_INFO("[native] C5c: {} shader n{} ({} words) taken in: what it draws appears from now on",
                vertices ? "vertex" : "pixel", number, words);
  }
}

const EntryShader* ShadersNative::IdentifyContainer(
    std::span<const uint8_t> container) const {
  const Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }
  std::shared_lock<std::shared_mutex> lock(d.mutex);
  const fh1::native::Shader* shader = d.library.Find(container);
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
  std::shared_lock<std::shared_mutex> lock(d.mutex);
  const auto it = std::lower_bound(d.entries.begin(), d.entries.end(), number,
                                   [](const EntryShader& e, uint32_t n) { return e.number < n; });
  return it != d.entries.end() && it->number == number ? &*it : nullptr;
}

const EntryShader* ShadersNative::ByContainerFingerprint(uint64_t fingerprint, bool vertices) const {
  const Data& d = *data_;
  if (!d.loaded) {
    return nullptr;
  }
  std::shared_lock<std::shared_mutex> lock(d.mutex);
  for (const EntryShader& e : d.entries) {
    if (e.vertices == vertices && e.shader && e.shader->fingerprint == fingerprint) {
      return &e;
    }
  }
  return nullptr;
}

const char* NameUse(uint8_t use) {
  return kUses[use & 0xF];
}

StatisticsShaders ShadersNative::Statistics() const {
  return data_->statistics;
}

}  // namespace fh1::native
