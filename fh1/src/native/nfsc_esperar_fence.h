// nfsc-recomp: GPU fence waits of the native renderer that cannot hang silently.
//
// The renderer inherited from nfsmw-nx waited for its fences with no time limit. If the GPU stops finishing work (a
// driver reset, a power-source change on a handheld, a GPU hang), the ring thread then waits forever, the window stops
// answering and Windows closes the game with no message in the log (seen 2026-10-02). This waits in 1 s steps and
// says in the log what the GPU is doing: still busy after N s, finished late, or "device lost".
#pragma once

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>

#include <cstdint>

namespace nfsmw::nativo {

template <typename Funciones>
inline VkResult EsperarFenceVigilada(const Funciones& dfn, VkDevice device, VkFence fence, const char* donde) {
  constexpr uint64_t kPasoNs = 1'000'000'000ull;
  VkResult r = dfn.vkWaitForFences(device, 1, &fence, VK_TRUE, kPasoNs);
  for (uint32_t segundos = 1; r == VK_TIMEOUT; ++segundos) {
    REXLOG_WARN("[nfsc] GPU has not finished after {} s ({}); still waiting", segundos, donde);
    r = dfn.vkWaitForFences(device, 1, &fence, VK_TRUE, kPasoNs);
    if (r == VK_SUCCESS) {
      REXLOG_WARN("[nfsc] GPU finished after about {} s ({})", segundos + 1, donde);
    }
  }
  if (r == VK_ERROR_DEVICE_LOST) {
    REXLOG_ERROR("[nfsc] GPU DEVICE LOST while waiting ({}): the graphics driver reset or removed the GPU", donde);
  } else if (r != VK_SUCCESS) {
    REXLOG_ERROR("[nfsc] vkWaitForFences failed with {} ({})", int(r), donde);
  }
  return r;
}

}  // namespace nfsmw::nativo
