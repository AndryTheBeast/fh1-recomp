#include "nfsmw_video_vulkan.h"
#include <stdexcept>
#include <string>

namespace nfsmw::native {
namespace {
void Check(VkResult r, const char* step) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::string(step) + ": " + std::to_string(r));
}
}
VideoVulkan::VideoVulkan(VkDevice vulkan_device, PFN_vkGetDeviceProcAddr proc,
                         ModulesShaders& modules, const Shader& vs,
                         const Shader& ps0, const Shader& ps1, VkFormat target)
    : vulkan_device_(vulkan_device) {
  if (!vulkan_device || !proc || !vs.vertices || ps0.vertices || ps1.vertices)
    throw std::invalid_argument("Device o stages incorrectos para video");
#define NFSMW_LOAD_VIDEO(n) \
  n = reinterpret_cast<PFN_vk##n>(proc(vulkan_device, "vk" #n)); \
  if (!n) throw std::runtime_error("Missing vk" #n);
  NFSMW_FUNCTIONS_VIDEO(NFSMW_LOAD_VIDEO)
#undef NFSMW_LOAD_VIDEO
  try {
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      binding.descriptorCount = i == 3 ? 1 : 3;
      binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.bindingCount = i == 0 || i == 3 ? 1 : 0;
      info.pBindings = &binding;
      Check(CreateDescriptorSetLayout(vulkan_device, &info, nullptr, &layouts_[i]), "Layout de descriptores de video");
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 4; pl.pSetLayouts = layouts_.data();
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &range;
    Check(CreatePipelineLayout(vulkan_device, &pl, nullptr, &pipelineLayout_), "Layout de pipeline de video");
    VkAttachmentDescription color{};
    color.format = target; color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo rp{}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1; rp.pAttachments = &color; rp.subpassCount = 1; rp.pSubpasses = &subpass;
    Check(CreateRenderPass(vulkan_device, &rp, nullptr, &renderPass_), "Render pass de video");
    VkShaderModule moduleVS, modulePS[2];
    Check(modules.Get(vs, moduleVS), "VS de video");
    Check(modules.Get(ps0, modulePS[0]), "PS de video 0");
    Check(modules.Get(ps1, modulePS[1]), "PS de video 1");
    const VkVertexInputBindingDescription vb{0, sizeof(VertexVideo), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {4, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{}; vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &blend;
    const VkDynamicState dynamic_2[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{}; ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dynamic_2;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = moduleVS;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkGraphicsPipelineCreateInfo gp{}; gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = pipelineLayout_; gp.renderPass = renderPass_;
    for (size_t i = 0; i < 2; ++i) {
      stages[1].module = modulePS[i];
      Check(CreateGraphicsPipelines(vulkan_device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelines_[i]), "Pipeline de video");
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 4; dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
    Check(CreateDescriptorPool(vulkan_device, &dp, nullptr, &pool_), "Pool de video");
    VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = pool_; da.descriptorSetCount = 4; da.pSetLayouts = layouts_.data();
    Check(AllocateDescriptorSets(vulkan_device, &da, sets_.data()), "Descriptores de video");
    VkSamplerCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sm.magFilter = sm.minFilter = VK_FILTER_LINEAR; sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sm.maxLod = 0;
    Check(CreateSampler(vulkan_device, &sm, nullptr, &sampler_), "Sampler de video");
  } catch (...) { Free(); throw; }
}
VideoVulkan::~VideoVulkan() { Free(); }
void VideoVulkan::Free() {
  if (pool_) DestroyDescriptorPool(vulkan_device_, pool_, nullptr);
  if (sampler_) DestroySampler(vulkan_device_, sampler_, nullptr);
  for (auto p : pipelines_) if (p) DestroyPipeline(vulkan_device_, p, nullptr);
  if (renderPass_) DestroyRenderPass(vulkan_device_, renderPass_, nullptr);
  if (pipelineLayout_) DestroyPipelineLayout(vulkan_device_, pipelineLayout_, nullptr);
  for (auto l : layouts_) if (l) DestroyDescriptorSetLayout(vulkan_device_, l, nullptr);
}
void VideoVulkan::ConfigureTextures(const std::array<VkImageView, 3>& planes) {
  std::array<VkDescriptorImageInfo, 3> images{};
  for (size_t i = 0; i < 3; ++i) {
    if (!planes[i]) throw std::invalid_argument("Flat de video ausente");
    images[i].imageView = planes[i]; images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  VkDescriptorImageInfo sampler{}; sampler.sampler = sampler_;
  VkWriteDescriptorSet writes[2]{};
  for (auto& e : writes) e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = sets_[0]; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].descriptorCount = 3; writes[0].pImageInfo = images.data();
  writes[1].dstSet = sets_[3]; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[1].descriptorCount = 1; writes[1].pImageInfo = &sampler;
  UpdateDescriptorSets(vulkan_device_, 2, writes, 0, nullptr);
  texturesLists_ = true;
}
void VideoVulkan::Draw(VkCommandBuffer cmd, VkFramebuffer target, uint32_t width, uint32_t height,
                          VkBuffer vertices, VkDeviceSize offset, VkDeviceAddress constants, uint32_t variant, bool backgroundBlack) {
  if (!texturesLists_ || variant >= 2 || !cmd || !target || !width || !height || !vertices || !constants || constants % 16)
    throw std::invalid_argument("Dibujado de video incompleto o constants desalineadas");
  VkRenderPassBeginInfo start{}; start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  start.renderPass = renderPass_; start.framebuffer = target; start.renderArea.extent = {width, height};
  CmdBeginRenderPass(cmd, &start, VK_SUBPASS_CONTENTS_INLINE);
  if (backgroundBlack) {
    VkClearAttachment black{}; black.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    black.clearValue.color.float32[3] = 1;
    const VkClearRect zone{{{0,0},{width,height}},0,1};
    CmdClearAttachments(cmd,1,&black,1,&zone);
  }
  CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[variant]);
  CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 4, sets_.data(), 0, nullptr);
  const uint64_t push[] = {0, 0, constants};
  CmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24, push);
  const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
  const VkRect2D scissor{{0, 0}, {width, height}};
  CmdSetViewport(cmd, 0, 1, &viewport); CmdSetScissor(cmd, 0, 1, &scissor);
  CmdBindVertexBuffers(cmd, 0, 1, &vertices, &offset);
  CmdDraw(cmd, 6, 1, 0, 0);
  CmdEndRenderPass(cmd);
}
}  // namespace nfsmw::native
