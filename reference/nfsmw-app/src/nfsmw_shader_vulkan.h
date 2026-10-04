#pragma once

#include "nfsmw_shader_library.h"
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace nfsmw::native {

// Owned by the render thread. The library must stay immutable and the device
// alive until this cache is destroyed. It does not wait for the GPU or create
// pipelines: those steps belong to the renderer and its synchronization.
class ModulesShaders {
 public:
  ModulesShaders(VkDevice vulkan_device, PFN_vkCreateShaderModule create,
                 PFN_vkDestroyShaderModule destroy);
  ~ModulesShaders();
  ModulesShaders(const ModulesShaders&) = delete;
  ModulesShaders& operator=(const ModulesShaders&) = delete;
  VkResult Get(const Shader& shader, VkShaderModule& modulo);

 private:
  VkDevice vulkan_device_;
  PFN_vkCreateShaderModule create_;
  PFN_vkDestroyShaderModule destroy_;
  std::unordered_map<const Shader*, VkShaderModule> modules_;
};
}  // namespace nfsmw::native
