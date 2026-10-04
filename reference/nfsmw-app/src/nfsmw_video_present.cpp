#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/instance.h>
// The SDK defines the beta extensions and the Horizon surface first.
#include "nfsmw_video_session.h"

namespace {
std::shared_ptr<const nfsmw::native::FrameVideo> g_frame;
std::unique_ptr<nfsmw::native::SessionVideo> g_session;
uint64_t g_presented=0;
}

// Only the GPU command thread calls these. The guest capture arrives through
// a marker queue, and the resources are never destroyed from the UI thread.
extern "C" bool RexNativeVideoBeginSwap() {
  g_frame=nfsmw::native::ConsumeVideo();
  return bool(g_frame);
}
extern "C" bool RexNativeVideoPresent(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                      rex::ui::Presenter* presenter,uint32_t width,uint32_t height) {
  if (!g_frame) return false;
  auto f=std::move(g_frame);
  try {
    const auto& p=vulkan_device->properties();
    if (!p.shaderInt64 || !p.shaderSampledImageArrayDynamicIndexing || !p.bufferDeviceAddress ||
        !p.runtimeDescriptorArray || !p.scalarBlockLayout) {
      nfsmw::native::DisableVideo("missing capacidades Vulkan habilitadas"); return false;
    }
    if (!width || !height || width>1920 || height>1080) return false;
    if (!g_session) {
      VkPhysicalDeviceMemoryProperties memory_block{};
      const auto& ifn=vulkan_device->vulkan_instance()->functions();
      ifn.vkGetPhysicalDeviceMemoryProperties(vulkan_device->physical_device(),&memory_block);
      g_session=std::make_unique<nfsmw::native::SessionVideo>(vulkan_device->device(),ifn.vkGetDeviceProcAddr,
          memory_block,vulkan_device->queue_family_graphics_compute());
    }
    const bool ok=presenter->RefreshGuestOutput(width,height,width,height,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& base) {
          auto& c=static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(base);
          if (!g_session->Prepare(*f,c.image(),c.image_view(),c.image_version(),c.image_ever_written_previously(),width,height)) return false;
          auto queue=vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(),0);
          g_session->Send(queue.queue());
          c.SetIs8bpc(true);
          return true;
        });
    if (ok) {
      ++g_presented;
      if (g_presented==1 || g_presented%300==0)
        REXLOG_INFO("[video native] presented {}: YUV {}x{}, output {}x{}, variant {}",g_presented,f->width,f->height,width,height,f->variant);
    }
    return ok;
  } catch (const std::exception& e) {
    nfsmw::native::DisableVideo(e.what()); return false;
  }
}
extern "C" void RexNativeVideoShutdown() {
  nfsmw::native::DisableVideo("close del vulkan_device");
  g_frame.reset(); g_session.reset();
}
