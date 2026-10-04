#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace nfsc::native {

// The original container also identifies constants and interface. The physical
// code alone is not enough. It is looked up before the driver creates its object.
struct Shader {
  std::vector<uint8_t> original;
  std::vector<uint32_t> spirv;
  uint64_t fingerprint = 0;
  bool vertices = false;
};

class LibraryShaders {
 public:
  // Transactional load: an invalid file does not replace the library.
  // Pointers returned by Find last until the next successful load.
  void Load(std::span<const uint8_t> file);
  void Load(const std::filesystem::path& file);
  const Shader* Find(std::span<const uint8_t> original) const;
  const std::vector<Shader>& shaders() const { return shaders_; }

 private:
  std::vector<Shader> shaders_;
};

// Versioned local format, little-endian, with integrity checks.
// It contains private data derived from the game; it is not embedded in the executable.
std::vector<uint8_t> PackShaders(std::vector<Shader> shaders);

}  // namespace nfsc::native
