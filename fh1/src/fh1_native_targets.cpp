// fh1 - native renderer, step N3a (see fh1_native_targets.h). From nfsmw-nx's nfsc_native_targets.cpp,
// without its NFS-specific optimisations and diagnostics.
//
// Covered: colour render targets in the formats FH1 uses (8888, 2_10_10_10, 2_10_10_10_FLOAT, 16-bit
// and 32-bit float), used with 1 sample (MSAA comes with the draws); clears with RB_COLOR_CLEAR
// converted like the SDK (vulkan/render_target_cache.cpp); copies of the rectangle into the resolved
// texture at RB_COPY_DEST_BASE (rectangle and tiled base like draw_util::GetResolveInfo), converting
// formats with a blit; presentation of the resolved texture named by the Swap with the SDK's own output
// shaders. Not yet: depth copies and clears, 3D destinations, writing resolves back to guest memory.
// All images stay in VK_IMAGE_LAYOUT_GENERAL, with a full barrier between operations (simple first).

#include "fh1_native_targets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

namespace fh1::native {
namespace shaders {
// The SPIR-V the SDK presenter draws the game image with (sdk/src/ui/shaders/vulkan_spirv).
#include "vulkan_spirv/guest_output_bilinear_ps.h"
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;
constexpr uint32_t kMaxTargetHeight = 2048;
constexpr uint32_t kSlots = 3;
constexpr VkImageSubresourceRange kColorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fixed16p8(float value) {
  if (!(std::abs(value) >= 1.0f / 512.0f)) return 0;
  const double scaled = std::clamp(double(value) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(scaled));
}

// Address of a texel in a 32x32-tiled texture (pipeline/texture/util.cpp).
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t SignExtend15(uint32_t value) { return int32_t(value << 17) >> 17; }

// Host format of a colour render target (ColorRenderTargetFormat) and of a resolve destination
// (ColorFormat). VK_FORMAT_UNDEFINED = not handled yet.
VkFormat TargetFormat(uint32_t format) {
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat TextureFormat(uint32_t format) {
  switch (xenos::ColorFormat(format)) {
    case xenos::ColorFormat::k_8_8_8_8:
    case xenos::ColorFormat::k_8_8_8_8_A:
    case xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorFormat::k_2_10_10_10:
    case xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorFormat::k_16_16:
      return VK_FORMAT_R16G16_SNORM;
    case xenos::ColorFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// RB_COLOR_CLEAR in the render target's format, as the SDK converts it.
VkClearColorValue ClearColor(uint32_t format, uint64_t value) {
  VkClearColorValue c{};
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      for (uint32_t j = 0; j < 4; ++j) c.float32[j] = float((value >> (j * 8)) & 0xFF) / 255.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      for (uint32_t j = 0; j < 3; ++j) c.float32[j] = float((value >> (j * 10)) & 0x3FF) / 1023.0f;
      c.float32[3] = float((value >> 30) & 0x3) / 3.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (uint32_t j = 0; j < 3; ++j) c.float32[j] = xenos::Float7e3To32(uint32_t(value >> (j * 10)) & 0x3FF);
      c.float32[3] = float((value >> 30) & 0x3) / 3.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      for (uint32_t j = 0; j < 4; ++j) c.float32[j] = rex::xenos_half_to_float(uint16_t(value >> (j * 16)));
      break;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      for (uint32_t j = 0; j < 4; ++j) c.float32[j] = std::max(float(int16_t(value >> (j * 16))) / 32767.0f, -1.0f);
      break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
      const uint32_t lo = uint32_t(value), hi = uint32_t(value >> 32);
      std::memcpy(&c.float32[0], &lo, 4);
      std::memcpy(&c.float32[1], &hi, 4);
      break;
    }
    default:
      break;
  }
  return c;
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool initialized = false;  // moved out of UNDEFINED into GENERAL
};

class VulkanTargets final : public Targets {
 public:
  VulkanTargets(const VulkanDevice* device, rex::memory::Memory* memory)
      : device_info_(device), dfn_(device->functions()), device_(device->device()), memory_(memory) {}

  ~VulkanTargets() override {
    WaitIdle();
    for (auto& retired : retired_) {
      for (Image& image : retired) Destroy(image);
    }
    for (auto& [key, image] : targets_) Destroy(image);
    for (auto& [key, image] : resolved_) Destroy(image);
    for (uint32_t i = 0; i < kSlots; ++i) {
      if (fences_[i]) dfn_.vkDestroyFence(device_, fences_[i], nullptr);
      if (pools_[i]) dfn_.vkDestroyCommandPool(device_, pools_[i], nullptr);
    }
    for (auto& f : framebuffers_) {
      if (f.framebuffer) dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
    }
    if (pipeline_) dfn_.vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) dfn_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (descriptor_pool_) dfn_.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (set_layout_) dfn_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (sampler_) dfn_.vkDestroySampler(device_, sampler_, nullptr);
    if (render_pass_) dfn_.vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (vs_) dfn_.vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_) dfn_.vkDestroyShaderModule(device_, fs_, nullptr);
  }

  bool Initialize() {
    // Not in the SDK's device function table (as nfsmw does).
    const auto get = device_info_->vulkan_instance()->functions().vkGetDeviceProcAddr;
    blit_ = reinterpret_cast<PFN_vkCmdBlitImage>(get(device_, "vkCmdBlitImage"));
    queue_wait_idle_ = reinterpret_cast<PFN_vkQueueWaitIdle>(get(device_, "vkQueueWaitIdle"));
    if (!blit_ || !queue_wait_idle_) return false;
    for (uint32_t i = 0; i < kSlots; ++i) {
      VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool.queueFamilyIndex = device_info_->queue_family_graphics_compute();
      if (dfn_.vkCreateCommandPool(device_, &pool, nullptr, &pools_[i]) != VK_SUCCESS) return false;
      VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      allocate.commandPool = pools_[i];
      allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate.commandBufferCount = 2;
      VkCommandBuffer buffers[2];
      if (dfn_.vkAllocateCommandBuffers(device_, &allocate, buffers) != VK_SUCCESS) return false;
      work_[i] = buffers[0];
      output_[i] = buffers[1];
      VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (dfn_.vkCreateFence(device_, &fence, nullptr, &fences_[i]) != VK_SUCCESS) return false;
    }
    // Output: the SDK presenter's shaders, a sampled image + immutable sampler, push constants for the
    // rectangle (vertex) and the bilinear constants (fragment), as nfsmw does.
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
    module.pCode = shaders::guest_output_triangle_strip_rect_vs;
    if (dfn_.vkCreateShaderModule(device_, &module, nullptr, &vs_) != VK_SUCCESS) return false;
    module.codeSize = sizeof(shaders::guest_output_bilinear_ps);
    module.pCode = shaders::guest_output_bilinear_ps;
    if (dfn_.vkCreateShaderModule(device_, &module, nullptr, &fs_) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_};
    VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 2;
    set_layout.pBindings = bindings;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &set_layout, nullptr, &set_layout_) != VK_SUCCESS) return false;
    VkPushConstantRange ranges[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16}, {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 16}};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout_;
    layout.pushConstantRangeCount = 2;
    layout.pPushConstantRanges = ranges;
    if (dfn_.vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots}, {VK_DESCRIPTOR_TYPE_SAMPLER, kSlots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kSlots;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < kSlots; ++i) {
      VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocate.descriptorPool = descriptor_pool_;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &set_layout_;
      if (dfn_.vkAllocateDescriptorSets(device_, &allocate, &sets_[i]) != VK_SUCCESS) return false;
    }
    VkAttachmentDescription attachment{};
    attachment.format = VulkanPresenter::kGuestOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo render_pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass.attachmentCount = 1;
    render_pass.pAttachments = &attachment;
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    if (dfn_.vkCreateRenderPass(device_, &render_pass, nullptr, &render_pass_) != VK_SUCCESS) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs_, "main"};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = pipeline_layout_;
    pipeline.renderPass = render_pass_;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_) != VK_SUCCESS) {
      return false;
    }
    return true;
  }

  bool Copy(const CopyRegisters& reg) override {
    const uint32_t control = reg.rb_copy_control;
    const uint32_t source = control & 0x7;
    const bool clear_color = (control >> 8) & 0x1;
    const uint32_t command = (control >> 20) & 0x3;
    const bool copy = command == uint32_t(xenos::CopyCommand::kRaw) ||
                      command == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
    if (source >= xenos::kMaxColorRenderTargets) {
      if (copy) ++stats_.depth_copies;  // depth comes with the draws
      return true;
    }
    const uint32_t color_info = reg.rb_color_info[source];
    const uint32_t target_format = (color_info >> 16) & 0xF;
    const VkFormat host_target_format = TargetFormat(target_format);
    if (host_target_format == VK_FORMAT_UNDEFINED) return Reject(100 + target_format, "render target format not handled yet");
    int32_t x0, y0, x1, y1;
    if (!Rectangle(reg, pitch, x0, y0, x1, y1)) return false;
    Image* target = GetTarget(color_info & 0xFFF, target_format, host_target_format, pitch);
    if (!target) return false;
    if (!BeginWork()) return false;
    Prepare(*target);
    if (copy) {
      const uint32_t dest_info = reg.rb_copy_dest_info;
      const uint32_t dest_format = (dest_info >> 7) & 0x3F;
      const VkFormat host_dest_format = TextureFormat(dest_format);
      if ((dest_info >> 3) & 0x1) {
        Reject(3, "copy to a 3D texture or array: not yet");
      } else if (host_dest_format == VK_FORMAT_UNDEFINED) {
        Reject(200 + dest_format, "copy destination format not handled yet");
      } else {
        const uint32_t dest_pitch = reg.rb_copy_dest_pitch & 0x3FFF;
        const uint32_t dest_height = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
        const uint32_t base_x = uint32_t(x0) & ~uint32_t(31), base_y = uint32_t(y0) & ~uint32_t(31);
        const uint32_t bytes_log2 = BytesLog2(host_dest_format);
        const uint32_t base = (reg.rb_copy_dest_base +
                               uint32_t(TiledOffset2D(int32_t(base_x), int32_t(base_y), dest_pitch, bytes_log2))) &
                              0x1FFFFFFF;
        const uint32_t dx = uint32_t(x0) - base_x, dy = uint32_t(y0) - base_y;
        Image* resolved = GetResolved(base, dest_pitch, dest_height, host_dest_format);
        if (resolved) {
          Prepare(*resolved);
          const uint32_t width = std::min({uint32_t(x1 - x0), target->width - uint32_t(std::min<int32_t>(x0, target->width)),
                                           resolved->width > dx ? resolved->width - dx : 0u});
          const uint32_t height = std::min({uint32_t(y1 - y0), target->height - uint32_t(std::min<int32_t>(y0, target->height)),
                                            resolved->height > dy ? resolved->height - dy : 0u});
          if (width && height) {
            Barrier();
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {x0, y0, 0};
            blit.srcOffsets[1] = {x0 + int32_t(width), y0 + int32_t(height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
            blit.dstOffsets[1] = {int32_t(dx + width), int32_t(dy + height), 1};
            blit_(work_[slot_], target->image, VK_IMAGE_LAYOUT_GENERAL, resolved->image,
                                VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
            ++stats_.copies;
            last_resolved_ = base;
          }
        }
      }
    }
    if (clear_color) {
      Barrier();
      const VkClearColorValue color =
          ClearColor(target_format, uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32));
      dfn_.vkCmdClearColorImage(work_[slot_], target->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &kColorRange);
      ++stats_.clears;
    }
    return true;
  }

  bool Present(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height) override {
    if (!presenter) return false;
    auto it = resolved_.find(frontbuffer & 0x1FFFFFFF);
    if (it == resolved_.end() && last_resolved_) {
      // The Swap's address did not match a resolve: fall back to the frame's last resolve.
      if (!warned_fallback_) {
        warned_fallback_ = true;
        REXLOG_INFO("[native] N3a: Swap frontbuffer {:08X} is not a resolved texture; showing the last resolve "
                    "{:08X} instead",
                    frontbuffer, last_resolved_);
      }
      it = resolved_.find(last_resolved_);
    }
    if (!SubmitWork()) return false;
    bool painted = false;
    if (it != resolved_.end() && it->second.initialized) {
      Image& source = it->second;
      const uint32_t w = std::min(width ? width : source.width, source.width);
      const uint32_t h = std::min(height ? height : source.height, source.height);
      presenter->RefreshGuestOutput(w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
        painted = Paint(static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context), source, w, h);
        return painted;
      });
    }
    if (painted) {
      ++stats_.presented;
    } else if (work_submitted_) {
      // The frame's work went out without a fence: signal this slot's fence so it is not reused early.
      const auto queue = device_info_->AcquireQueue(device_info_->queue_family_graphics_compute(), 0);
      if (dfn_.vkQueueSubmit(queue.queue(), 0, nullptr, fences_[slot_]) == VK_SUCCESS) pending_[slot_] = true;
      work_submitted_ = false;
      slot_ = (slot_ + 1) % kSlots;
    }
    return painted;
  }

  TargetStats Stats() const override {
    TargetStats s = stats_;
    s.render_targets = uint32_t(targets_.size());
    s.resolved = uint32_t(resolved_.size());
    return s;
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++stats_.rejected;
    if (warned_.insert(cause).second) REXLOG_INFO("[native] N3a: {} (cause {})", text, cause);
    return false;
  }

  static uint32_t BytesLog2(VkFormat format) {
    switch (format) {
      case VK_FORMAT_R16G16B16A16_SFLOAT:
      case VK_FORMAT_R16G16B16A16_SNORM:
      case VK_FORMAT_R32G32_SFLOAT:
        return 3;
      default:
        return 2;
    }
  }

  // The copy rectangle, like draw_util::GetResolveInfo: fetch constant 0 holds 3 vertices (x, y).
  bool Rectangle(const CopyRegisters& reg, uint32_t pitch, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) {
    const uint32_t type = reg.fetch_vertices[0] & 0x3;
    const uint32_t address = reg.fetch_vertices[0] >> 2;
    const auto endian = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
    const uint32_t size = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
    if (type != uint32_t(xenos::FetchConstantType::kVertex) || size != 3 * 2) {
      return Reject(5, "copy vertices in an unsupported layout");
    }
    const uint8_t* vertices = memory_->TranslatePhysical(address * 4);
    const float half_pixel = (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
    int32_t fixed[6];
    for (int i = 0; i < 6; ++i) {
      float value;
      std::memcpy(&value, vertices + i * 4, sizeof(value));
      fixed[i] = Fixed16p8(xenos::GpuSwap(value, endian) + half_pixel);
    }
    x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    const int32_t offset_x = SignExtend15(reg.pa_sc_window_offset & 0x7FFF);
    const int32_t offset_y = SignExtend15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
    if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
      x0 += offset_x;
      y0 += offset_y;
      x1 += offset_x;
      y1 += offset_y;
    }
    int32_t left = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
    int32_t top = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
    int32_t right = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
    int32_t bottom = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
    if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
      left += offset_x;
      top += offset_y;
      right += offset_x;
      bottom += offset_y;
    }
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::max(right, left);
    bottom = std::max(bottom, top);
    x0 = std::clamp(x0, left, right);
    y0 = std::clamp(y0, top, bottom);
    x1 = std::clamp(x1, left, right);
    y1 = std::clamp(y1, top, bottom);
    x0 &= ~int32_t(7);
    y0 &= ~int32_t(7);
    x1 = (x1 + 7) & ~int32_t(7);
    y1 = (y1 + 7) & ~int32_t(7);
    const int32_t aligned_pitch = int32_t(pitch & ~uint32_t(7));
    x0 = std::min(x0, aligned_pitch);
    x1 = std::min(x1, aligned_pitch);
    if (x0 >= x1 || y0 >= y1) return Reject(6, "empty copy rectangle");
    return true;
  }

  bool CreateImage(Image& image, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(device_info_, info,
                                                               rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                               image.image, image.memory)) {
      return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = kColorRange;
    if (dfn_.vkCreateImageView(device_, &view, nullptr, &image.view) != VK_SUCCESS) {
      Destroy(image);
      return false;
    }
    image.width = width;
    image.height = height;
    image.format = format;
    return true;
  }

  void Destroy(Image& image) {
    if (image.view) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image) dfn_.vkDestroyImage(device_, image.image, nullptr);
    if (image.memory) dfn_.vkFreeMemory(device_, image.memory, nullptr);
    image = Image{};
  }

  Image* GetTarget(uint32_t base, uint32_t format, VkFormat host_format, uint32_t pitch) {
    if (!pitch) {
      Reject(7, "render target with pitch 0");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    if (auto it = targets_.find(key); it != targets_.end()) return &it->second;
    const uint32_t height = std::min(kMaxTargetHeight, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    Image image;
    if (!CreateImage(image, pitch, height, host_format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      Reject(8, "could not create a render target");
      return nullptr;
    }
    REXLOG_INFO("[native] N3a: render target base {:03X}, format {}, {}x{}", base, format, pitch, height);
    return &targets_.emplace(key, image).first->second;
  }

  Image* GetResolved(uint32_t base, uint32_t width, uint32_t height, VkFormat host_format) {
    if (!width || !height) {
      Reject(9, "copy to a texture of size 0");
      return nullptr;
    }
    if (auto it = resolved_.find(base); it != resolved_.end()) {
      if (it->second.width == width && it->second.height == height && it->second.format == host_format) {
        return &it->second;
      }
      // The old image may still be used by recorded or in-flight work: destroyed once this slot's
      // fence has passed (BeginWork). Waiting here would end the command buffer being recorded.
      retired_[slot_].push_back(it->second);
      resolved_.erase(it);
    }
    Image image;
    if (!CreateImage(image, width, height, host_format,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      Reject(10, "could not create a resolved texture");
      return nullptr;
    }
    if (++logged_resolved_ <= 64) {
      REXLOG_INFO("[native] N3a: resolved texture at {:08X}, {}x{}, format {}", base, width, height,
                  uint32_t(host_format));
    }
    return &resolved_.emplace(base, image).first->second;
  }

  // Command buffer of the frame being recorded (one per slot, reused after its fence).
  bool BeginWork() {
    if (recording_) return true;
    if (pending_[slot_]) {
      dfn_.vkWaitForFences(device_, 1, &fences_[slot_], VK_TRUE, UINT64_MAX);
      dfn_.vkResetFences(device_, 1, &fences_[slot_]);
      pending_[slot_] = false;
    }
    for (Image& image : retired_[slot_]) Destroy(image);
    retired_[slot_].clear();
    dfn_.vkResetCommandPool(device_, pools_[slot_], 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(work_[slot_], &begin) != VK_SUCCESS) return false;
    recording_ = true;
    return true;
  }

  bool SubmitWork() {
    if (!recording_) return true;
    recording_ = false;
    if (dfn_.vkEndCommandBuffer(work_[slot_]) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &work_[slot_];
    {
      const auto queue = device_info_->AcquireQueue(device_info_->queue_family_graphics_compute(), 0);
      if (dfn_.vkQueueSubmit(queue.queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    }
    work_submitted_ = true;
    return true;
  }

  // First use: UNDEFINED -> GENERAL.
  void Prepare(Image& image) {
    if (image.initialized) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = kColorRange;
    dfn_.vkCmdPipelineBarrier(work_[slot_], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &barrier);
    image.initialized = true;
  }

  void Barrier() {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(work_[slot_], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                              &barrier, 0, nullptr, 0, nullptr);
  }

  // Draws `source` into the presenter's output; its fence also covers the frame's work (same queue).
  bool Paint(VulkanPresenter::VulkanGuestOutputRefreshContext& context, Image& source, uint32_t width, uint32_t height) {
    const uint32_t s = slot_;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const auto& f : framebuffers_) {
      if (f.framebuffer && f.version == context.image_version() && f.width == width && f.height == height) {
        framebuffer = f.framebuffer;
      }
    }
    if (!framebuffer) {
      auto& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer) {
        WaitIdle();
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = width;
      info.height = height;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      f.width = width;
      f.height = height;
      framebuffer = f.framebuffer;
    }
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[s];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_info;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    VkCommandBuffer cb = output_[s];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) return false;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dfn_.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier,
                              0, nullptr, 0, nullptr);
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = {width, height};
    dfn_.vkCmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(cb, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {width, height}};
    dfn_.vkCmdSetScissor(cb, 0, 1, &scissor);
    dfn_.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    dfn_.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &sets_[s], 0, nullptr);
    const float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};  // x, y, width, height in NDC
    dfn_.vkCmdPushConstants(cb, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rect), rect);
    struct {
      int32_t offset[2];
      float inverse_size[2];
    } bilinear = {{0, 0}, {1.0f / float(source.width), 1.0f / float(source.height)}};
    dfn_.vkCmdPushConstants(cb, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 16, sizeof(bilinear), &bilinear);
    dfn_.vkCmdDraw(cb, 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(cb);
    if (dfn_.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    {
      const auto queue = device_info_->AcquireQueue(device_info_->queue_family_graphics_compute(), 0);
      if (dfn_.vkQueueSubmit(queue.queue(), 1, &submit, fences_[s]) != VK_SUCCESS) return false;
    }
    pending_[s] = true;
    work_submitted_ = false;
    slot_ = (slot_ + 1) % kSlots;  // the next frame records into the next slot
    context.SetIs8bpc(source.format == VK_FORMAT_R8G8B8A8_UNORM);
    return true;
  }

  void WaitIdle() {
    if (recording_) SubmitWork();
    const auto queue = device_info_->AcquireQueue(device_info_->queue_family_graphics_compute(), 0);
    queue_wait_idle_(queue.queue());
    for (uint32_t i = 0; i < kSlots; ++i) {
      if (pending_[i]) {
        dfn_.vkResetFences(device_, 1, &fences_[i]);
        pending_[i] = false;
      }
    }
  }

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
    uint32_t width = 0, height = 0;
  };

  const VulkanDevice* device_info_;
  PFN_vkCmdBlitImage blit_ = nullptr;
  PFN_vkQueueWaitIdle queue_wait_idle_ = nullptr;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  std::unordered_map<uint64_t, Image> targets_;
  std::unordered_map<uint32_t, Image> resolved_;
  std::array<VkCommandPool, kSlots> pools_{};
  std::array<VkCommandBuffer, kSlots> work_{}, output_{};
  std::array<VkFence, kSlots> fences_{};
  std::array<bool, kSlots> pending_{};
  std::array<std::vector<Image>, kSlots> retired_;  // replaced images, destroyed after the slot's fence
  uint32_t slot_ = 0;
  bool recording_ = false, work_submitted_ = false;
  VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlots> sets_{};
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  std::array<Framebuffer, 4> framebuffers_{};
  size_t next_framebuffer_ = 0;
  uint32_t last_resolved_ = 0;
  bool warned_fallback_ = false;
  uint32_t logged_resolved_ = 0;
  std::unordered_set<uint32_t> warned_;
  TargetStats stats_;
};

}  // namespace

std::unique_ptr<Targets> Targets::Create(const VulkanDevice* device, rex::memory::Memory* memory) {
  if (!device || !memory) return nullptr;
  auto targets = std::make_unique<VulkanTargets>(device, memory);
  if (!targets->Initialize()) {
    REXLOG_ERROR("[native] N3a: could not set up render targets and presentation");
    return nullptr;
  }
  return targets;
}

}  // namespace fh1::native
