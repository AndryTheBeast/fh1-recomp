#pragma once
#include "nfsmw_video_vulkan.h"
#include <deque>
#include <memory>
#include <mutex>

namespace nfsmw::native {
struct FrameVideo {
  uint32_t object = 0;
  uint32_t width = 0, height = 0, variant = 0;
  std::array<std::vector<uint8_t>, 3> planes;
  std::array<VertexVideo, 6> vertices{};
  const Shader* vs = nullptr;
  const Shader* ps[2]{};
};

// One marker per guest Swap, even if that frame uses Xenos.
// That way a race never consumes a cutscene's last frame by mistake.
class QueueVideo {
 public:
  bool Enqueue(std::shared_ptr<const FrameVideo> frame) {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= 8) { queue_.clear(); return false; }
    queue_.push_back(std::move(frame)); return true;
  }
  std::shared_ptr<const FrameVideo> Consume() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return {};
    auto f = std::move(queue_.front()); queue_.pop_front(); return f;
  }
  void Empty() { std::lock_guard lock(mutex_); queue_.clear(); }
 private:
  std::mutex mutex_;
  std::deque<std::shared_ptr<const FrameVideo>> queue_;
};

void CapturePlanesVideo(const uint8_t* base, uint32_t object, uint32_t data);
void NoteDrawVideo(const uint8_t* base, bool esVideo, uint32_t object);
void InvalidateVideo();
void NoteSwapVideo();
void DisableVideo(const char* reason);
std::shared_ptr<const FrameVideo> ConsumeVideo();
}  // namespace nfsmw::native
