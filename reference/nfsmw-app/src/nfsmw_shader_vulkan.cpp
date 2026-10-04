#include "nfsmw_shader_vulkan.h"
#include <stdexcept>

namespace nfsmw::native {
ModulesShaders::ModulesShaders(VkDevice vulkan_device, PFN_vkCreateShaderModule create,
                               PFN_vkDestroyShaderModule destroy)
    : vulkan_device_(vulkan_device), create_(create), destroy_(destroy) {
  if (!vulkan_device || !create || !destroy)
    throw std::invalid_argument("Device o functions Vulkan ausentes");
}

ModulesShaders::~ModulesShaders() {
  for (auto [shader, modulo] : modules_) destroy_(vulkan_device_, modulo, nullptr);
}

VkResult ModulesShaders::Get(const Shader& shader, VkShaderModule& modulo) {
  modulo = VK_NULL_HANDLE;
  // Reserving before creating the resource avoids leaking it if the reservation fails.
  auto [it, new_value] = modules_.try_emplace(&shader, VK_NULL_HANDLE);
  if (new_value) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = shader.spirv.size() * sizeof(uint32_t);
    info.pCode = shader.spirv.data();
    const VkResult result = create_(vulkan_device_, &info, nullptr, &it->second);
    if (result != VK_SUCCESS) {
      modules_.erase(it);
      return result;
    }
  }
  modulo = it->second;
  return VK_SUCCESS;
}
}  // namespace nfsmw::native
