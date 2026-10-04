#pragma once
#include "nfsmw_shader_vulkan.h"
#include <array>

namespace nfsmw::native {
struct VertexVideo { float x, y, z, u, v; };
static_assert(sizeof(VertexVideo) == 20);

// Layout shared with shader_common.h and the original SPIR-V.
struct alignas(16) ConstantsVideo {
  uint32_t textures2D[16]{0, 1, 2};
  uint32_t textures3D[16]{};
  uint32_t texturesCube[16]{};
  uint32_t samplers[16]{};
  uint32_t booleans = 0;
  uint32_t swapUV = 0;
  float middlePixel[2]{};
  float thresholdAlpha = 0;
  uint32_t fill[3]{};
};
static_assert(offsetof(ConstantsVideo, booleans) == 256);
static_assert(offsetof(ConstantsVideo, swapUV) == 260);
static_assert(offsetof(ConstantsVideo, middlePixel) == 264);
static_assert(sizeof(ConstantsVideo) == 288);

#define NFSMW_FUNCTIONS_VIDEO(X) \
  X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) \
  X(CreateRenderPass) X(DestroyRenderPass) \
  X(CreateGraphicsPipelines) X(DestroyPipeline) \
  X(CreateDescriptorPool) X(DestroyDescriptorPool) \
  X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
  X(CreateSampler) X(DestroySampler) \
  X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) \
  X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdSetViewport) \
  X(CmdSetScissor) X(CmdBindVertexBuffers) X(CmdDraw) X(CmdClearAttachments)

// Video draw path; it creates no device, queues or waits. The caller enables
// shaderInt64, shaderSampledImageArrayDynamicIndexing, bufferDeviceAddress,
// runtimeDescriptorArray and scalarBlockLayout when creating the VkDevice.
// It must check the features actually enabled before using this path and
// keep textures, constants, vertices and framebuffer alive until their fence.
// Use from a single thread. ConfigureTextures and destruction require every
// earlier submission that uses this object to have finished.
class VideoVulkan {
 public:
  VideoVulkan(VkDevice vulkan_device, PFN_vkGetDeviceProcAddr proc,
              ModulesShaders& modules, const Shader& vs,
              const Shader& ps0, const Shader& ps1, VkFormat target);
  ~VideoVulkan();
  VideoVulkan(const VideoVulkan&) = delete;
  VideoVulkan& operator=(const VideoVulkan&) = delete;
  VkRenderPass render_pass() const { return renderPass_; }
  void ConfigureTextures(const std::array<VkImageView, 3>& planes);
  // The target image arrives in COLOR_ATTACHMENT_OPTIMAL. Contents outside the
  // triangles are preserved. The textures arrive in SHADER_READ_ONLY_OPTIMAL.
  void Draw(VkCommandBuffer cmd, VkFramebuffer target, uint32_t width, uint32_t height,
               VkBuffer vertices, VkDeviceSize offset, VkDeviceAddress constants,
               uint32_t variant, bool backgroundBlack = false);

 private:
  void Free();
  VkDevice vulkan_device_;
#define NFSMW_DECLARE_VIDEO(n) PFN_vk##n n = nullptr;
  NFSMW_FUNCTIONS_VIDEO(NFSMW_DECLARE_VIDEO)
#undef NFSMW_DECLARE_VIDEO
  std::array<VkDescriptorSetLayout, 4> layouts_{};
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  std::array<VkPipeline, 2> pipelines_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  bool texturesLists_ = false;
};
}  // namespace nfsmw::native
