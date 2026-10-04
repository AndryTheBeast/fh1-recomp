#pragma once
#include "nfsmw_shader_library.h"

namespace nfsmw::native {
// Initialize once before starting the guest threads. It is not reloaded.
void StartLibraryShaders();
// Only recognizes objects from the two verified creation paths. nullptr
// means unknown: it does not allow skipping the original draw.
const Shader* ShaderOfObject(uint32_t object);
const Shader* ShaderOriginal(std::span<const uint8_t> container);
}  // namespace nfsmw::native
