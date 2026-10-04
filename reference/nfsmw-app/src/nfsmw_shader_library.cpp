#include "nfsmw_shader_library.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads
 * through 64- and 32-bit pointers without may_alias, and GCC may hoist that read above the write of the
 * data being hashed (strict aliasing). That made the texture key read keys[4] before writing it, and
 * the same texture was created several times (tools/check_texture_key.py checks a built ELF for
 * this). Same fingerprint values; on AArch64, the same LDR.
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
constexpr std::array<uint8_t, 8> kSignature{'N','F','S','S','P','V',0,0};
constexpr size_t kMaxFile = 64 * 1024 * 1024;
constexpr size_t kMaxShaders = 4096;
constexpr size_t kMaxOriginal = 64 * 1024;
constexpr size_t kMaxSpirv = 4 * 1024 * 1024;

void Require(bool condition, const char* error) {
  if (!condition) throw std::runtime_error(error);
}

uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}

struct Reader {
  std::span<const uint8_t> data;
  size_t position = 0;
  std::span<const uint8_t> Take(size_t n) {
    Require(n <= data.size() - position, "Truncated shader package");
    auto r = data.subspan(position, n);
    position += n;
    return r;
  }
  uint32_t U32() {
    auto p = Take(4);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }
  uint64_t U64() {
    const uint64_t low = U32();
    return low | uint64_t(U32()) << 32;
  }
};

void U32(std::vector<uint8_t>& d, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (i * 8)));
}
void U64(std::vector<uint8_t>& d, uint64_t v) {
  U32(d, uint32_t(v)); U32(d, uint32_t(v >> 32));
}

void Validate(Shader& s) {
  Require(s.original.size() >= 24 && s.original.size() <= kMaxOriginal,
         "Container length out of bounds");
  const uint32_t signature = BE(s.original.data());
  Require((signature & ~1u) == 0x102A0E00, "Signature de container unknown");
  const uint32_t virtuales = BE(s.original.data() + 4);
  const uint32_t physical = BE(s.original.data() + 8);
  Require(virtuales >= 24 && physical && !(physical % 12) &&
             uint64_t(virtuales) + physical == s.original.size(),
         "Inconsistent container lengths");
  s.vertices = (signature & 1) != 0;
  s.fingerprint = XXH3_64bits(s.original.data(), s.original.size());
  Require(s.spirv.size() >= 5 && s.spirv.size() <= kMaxSpirv / 4,
         "SPIR-V length out of bounds");
  Require(s.spirv[0] == 0x07230203 && s.spirv[4] == 0,
         "Header SPIR-V incorrecta");
  // This checks the wrapper; it does not replace spirv-val in the packager.
  bool entry = false;
  for (size_t i = 5; i < s.spirv.size();) {
    const uint32_t words = s.spirv[i] >> 16, op = s.spirv[i] & 65535;
    Require(words && words <= s.spirv.size() - i, "Instruction SPIR-V truncada");
    if (op == 15) {  // OpEntryPoint: model de ejecucion, id, name e interfaz.
      Require(!entry && words >= 5 && s.spirv[i+1] == (s.vertices ? 0u : 4u) &&
                 s.spirv[i+3] == 0x6e69616d && s.spirv[i+4] == 0,
             "The SPIR-V stage or main entry does not match the container");
      entry = true;
    }
    i += words;
  }
  Require(entry, "SPIR-V without a main entry");
}

bool Smaller(const Shader& a, const Shader& b) {
  if (a.fingerprint != b.fingerprint) return a.fingerprint < b.fingerprint;
  return a.original < b.original;
}
}  // namespace

void LibraryShaders::Load(std::span<const uint8_t> file) {
  Require(file.size() >= 24 && file.size() <= kMaxFile,
         "Shader package size out of bounds");
  Reader l{file};
  auto signature = l.Take(8);
  Require(std::equal(signature.begin(), signature.end(), kSignature.begin()), "Unknown package signature");
  Require(l.U32() == 1, "Package version not supported");
  const uint32_t amount = l.U32();
  Require(amount && amount <= kMaxShaders, "Shader count out of bounds");
  const uint64_t fingerprint = l.U64();
  Require(XXH3_64bits(file.data() + 24, file.size() - 24) == fingerprint,
         "Shader package altered");
  std::vector<Shader> new_items;
  new_items.reserve(amount);
  for (uint32_t i = 0; i < amount; ++i) {
    const uint32_t original = l.U32(), words = l.U32();
    const uint64_t expected = l.U64();
    Require(original >= 24 && original <= kMaxOriginal &&
               words >= 5 && words <= kMaxSpirv / 4, "Entry demasiado large");
    Shader s;
    auto data = l.Take(original);
    s.original.assign(data.begin(), data.end());
    // Reading the whole range first avoids allocations with a truncated file.
    Reader code{l.Take(size_t(words) * 4)};
    s.spirv.reserve(words);
    for (uint32_t j = 0; j < words; ++j) s.spirv.push_back(code.U32());
    Validate(s);
    Require(s.fingerprint == expected, "Wrong container fingerprint");
    if (!new_items.empty()) Require(Smaller(new_items.back(), s), "Entries repetidas o desordenadas");
    new_items.push_back(std::move(s));
  }
  Require(l.position == file.size(), "Leftover data in the shader package");
  shaders_ = std::move(new_items);
}

void LibraryShaders::Load(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary | std::ios::ate);
  Require(bool(f), "Could not open the shader package");
  const auto n = f.tellg();
  Require(n >= 24 && n <= std::streamoff(kMaxFile), "Package size out of bounds");
  std::vector<uint8_t> data(static_cast<size_t>(n));
  f.seekg(0);
  Require(bool(f.read(reinterpret_cast<char*>(data.data()), data.size())), "Incomplete package read");
  Load(data);
}

const Shader* LibraryShaders::Find(std::span<const uint8_t> original) const {
  if (original.size() < 24 || original.size() > kMaxOriginal) return nullptr;
  const uint64_t fingerprint = XXH3_64bits(original.data(), original.size());
  auto it = std::lower_bound(shaders_.begin(), shaders_.end(), fingerprint,
      [](const Shader& s, uint64_t h) { return s.fingerprint < h; });
  // A hash collision can never pick another shader: the bytes are compared.
  for (; it != shaders_.end() && it->fingerprint == fingerprint; ++it)
    if (it->original.size() == original.size() &&
        std::equal(original.begin(), original.end(), it->original.begin())) return &*it;
  return nullptr;
}

std::vector<uint8_t> PackShaders(std::vector<Shader> shaders) {
  Require(!shaders.empty() && shaders.size() <= kMaxShaders, "Shader count out of bounds");
  for (auto& s : shaders) Validate(s);
  std::sort(shaders.begin(), shaders.end(), Smaller);
  std::vector<uint8_t> body;
  uint32_t amount = 0;
  const Shader* previous = nullptr;
  for (const auto& s : shaders) {
    if (previous && previous->original == s.original) {
      Require(previous->spirv == s.spirv, "Un container has dos traducciones different_2");
      continue;
    }
    const size_t n = 16 + s.original.size() + s.spirv.size() * 4;
    Require(n <= kMaxFile - 24 - body.size(), "Packet demasiado large");
    U32(body, uint32_t(s.original.size())); U32(body, uint32_t(s.spirv.size()));
    U64(body, s.fingerprint);
    body.insert(body.end(), s.original.begin(), s.original.end());
    for (uint32_t word : s.spirv) U32(body, word);
    ++amount;
    previous = &s;
  }
  std::vector<uint8_t> file(kSignature.begin(), kSignature.end());
  U32(file, 1); U32(file, amount);
  U64(file, XXH3_64bits(body.data(), body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}
}  // namespace nfsmw::native
