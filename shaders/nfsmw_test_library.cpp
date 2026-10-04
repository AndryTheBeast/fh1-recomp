#include "../app/src/nfsmw_shader_vulkan.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#define XXH_INLINE_ALL
#include <xxhash.h>

using namespace nfsmw::native;
namespace {
void Check(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
std::vector<uint8_t> Read(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  Check(bool(f) && f.tellg() >= 0, "Could not read the input");
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg())); f.seekg(0);
  Check(bool(f.read(reinterpret_cast<char*>(d.data()), d.size())), "ReadAccess incompleta");
  return d;
}
uint32_t U32(const std::vector<uint8_t>& d, size_t p) {
  return uint32_t(d[p]) | uint32_t(d[p+1]) << 8 | uint32_t(d[p+2]) << 16 | uint32_t(d[p+3]) << 24;
}
void Set(std::vector<uint8_t>& d, size_t p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) d.at(p+i) = uint8_t(v >> (8*i));
}
void Seal(std::vector<uint8_t>& d) {
  const uint64_t h = XXH3_64bits(d.data() + 24, d.size() - 24);
  Set(d, 16, uint32_t(h)); Set(d, 20, uint32_t(h >> 32));
}
size_t created = 0, destroyed = 0;
bool fail = false;
VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice, const VkShaderModuleCreateInfo* i,
                                     const VkAllocationCallbacks*, VkShaderModule* m) {
  Check(i->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO &&
      i->codeSize >= 20 && i->codeSize % 4 == 0 &&
      uintptr_t(i->pCode) % alignof(uint32_t) == 0 && i->pCode[0] == 0x07230203,
      "Wrong vkCreateShaderModule parameters");
  if (fail) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  *m = (VkShaderModule)(uintptr_t(++created));
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice, VkShaderModule m, const VkAllocationCallbacks*) {
  Check(m != VK_NULL_HANDLE, "A null module is destroyed");
  ++destroyed;
}
}
int main(int argc, char** argv) try {
  Check(argc == 3, "Use: nfsmw_test_library <packet> <containers>");
  const auto original = Read(argv[1]);
  LibraryShaders b; b.Load(original);
  const size_t amount = b.shaders().size();
  Check(amount > 1, "Corpus insuficiente");
  size_t rejected = 0, matches = 0;
  auto reject = [&](std::vector<uint8_t> d, bool seal = false) {
    if (seal) Seal(d);
    bool miss = false;
    try { b.Load(d); } catch (const std::runtime_error&) { miss = true; }
    Check(miss, "An altered package was accepted");
    Check(b.shaders().size() == amount, "The failed load replaced the library");
    ++rejected;
  };
  for (size_t n = 0; n < 64; ++n) reject({original.begin(), original.begin() + n});
  reject({original.begin(), original.end() - 1});
  auto d = original; d[0] ^= 1; reject(d);
  d = original; Set(d, 8, 2); reject(d);
  d = original; Set(d, 12, 4097); reject(d);
  d = original; Set(d, 12, U32(d, 12) + 1); reject(d);
  d = original; d.back() ^= 1; reject(d);
  d = original; d.push_back(0); reject(d, true);
  d = original; Set(d, 24, 0xffffffff); reject(d, true);
  d = original; Set(d, 28, 0xffffffff); reject(d, true);
  d = original; d[32] ^= 1; reject(d, true);
  const size_t code = 40 + U32(original, 24);
  d = original; Set(d, code, 0); reject(d, true);
  d = original; Set(d, code + 20, 0); reject(d, true);
  size_t entry = code + 20;
  while ((U32(original, entry) & 65535) != 15) entry += 4 * (U32(original, entry) >> 16);
  d = original; Set(d, entry + 4, 5); reject(d, true);
  d = original; Set(d, entry + 12, 0); reject(d, true);
  for (const auto& e : std::filesystem::directory_iterator(argv[2])) {
    if (e.path().extension() != ".bin") continue;
    auto data = Read(e.path());
    const Shader* s = b.Find(data);
    Check(s && s->original == data, "An original container is not recovered");
    data.back() ^= 1;
    Check(!b.Find(data), "Un container alterado selecciona un shader");
    ++matches;
  }
  auto copy = b.shaders();
  copy.push_back(copy.front()); std::reverse(copy.begin(), copy.end());
  Check(PackShaders(copy) == original, "The package is not deterministic or does not remove duplicates");
  copy.back().spirv.back() ^= 1;
  bool miss = false;
  try { (void)PackShaders(copy); } catch (const std::runtime_error&) { miss = true; }
  Check(miss, "Different translations of the same container are accepted");
  {
    ModulesShaders m((VkDevice)(uintptr_t(1)), Create, Destroy);
    VkShaderModule modulo;
    fail = true;
    Check(m.Get(b.shaders().front(), modulo) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
        modulo == VK_NULL_HANDLE, "The Vulkan error is hidden");
    fail = false;
    for (const auto& s : b.shaders()) {
      VkShaderModule repeated;
      Check(m.Get(s, modulo) == VK_SUCCESS && modulo != VK_NULL_HANDLE, "Creation failure");
      Check(m.Get(s, repeated) == VK_SUCCESS && repeated == modulo, "Cache failure");
    }
    Check(created == amount, "Repeated modules are created");
  }
  Check(destroyed == created, "Vulkan module leak");
  std::printf("%zu originals found; %zu unique; %zu wrong packages rejected; cache, failure and destruction checked with a simulated Vulkan\n",
      matches, amount, rejected);
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
