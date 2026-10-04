#pragma once
#include "nfsmw_video_bridge.h"

namespace nfsmw::native {
// Three resource sets, without waiting to reuse a busy one. The graphics
// thread decides whether to present natively or keep the Xenos frame.
class SessionVideo {
 public:
  SessionVideo(VkDevice device, PFN_vkGetDeviceProcAddr proc,
               const VkPhysicalDeviceMemoryProperties& memory_block, uint32_t family);
  ~SessionVideo();
  SessionVideo(const SessionVideo&) = delete;
  SessionVideo& operator=(const SessionVideo&) = delete;
  bool Prepare(const FrameVideo& f, VkImage image, VkImageView vista, uint64_t version,
                bool written, uint32_t width, uint32_t height);
  void Send(VkQueue queue);
 private:
  struct Resources;
  VkDevice device_;
  PFN_vkGetDeviceProcAddr proc_;
  VkPhysicalDeviceMemoryProperties memory_;
  uint32_t family_;
  std::unique_ptr<ModulesShaders> modules_;
  std::array<std::unique_ptr<Resources>, 3> resources_;
  Resources* prepared_ = nullptr;
  unsigned next_ = 0;
};
}
