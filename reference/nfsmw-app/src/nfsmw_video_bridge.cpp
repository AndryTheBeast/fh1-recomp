#include "nfsmw_video_bridge.h"
#include "nfsmw_shader_hooks.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

// Testing on the Switch showed a regression: planes were copied and then rejected because of
// the vertices. Only enable explicitly for development.
REXCVAR_DEFINE_BOOL(nfsmw_native_video, false, "NFSMW",
                    "Present cinematica por Vulkan native con fallback Xenos")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nfsmw::native {
namespace {
std::atomic<bool> g_disabled{false};
std::mutex g_mutex;
std::shared_ptr<FrameVideo> g_captured;
std::shared_ptr<const FrameVideo> g_ready;
QueueVideo g_queue;
std::atomic<unsigned> g_rejections{0};
void Rejection(unsigned bit,const char* reason) {
  if (!(g_rejections.fetch_or(bit)&bit)) REXLOG_WARN("[video native] capture rechazada: {}",reason);
}
bool Active() { return !g_disabled.load(std::memory_order_relaxed) && REXCVAR_GET(nfsmw_native_video); }
uint32_t BE(const uint8_t* base, uint32_t p) {
  const volatile uint8_t* d = base + p;
  return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
}
bool Range(uint32_t p, uint64_t n) { return p && uint64_t(p) + n <= (uint64_t(1) << 32); }
}
void DisableVideo(const char* reason) {
  if (!g_disabled.exchange(true)) REXLOG_WARN("[video native] Xenos sigue active: {}", reason);
  { std::lock_guard lock(g_mutex); g_captured.reset(); g_ready.reset(); }
  g_queue.Empty();
}
void InvalidateVideo() {
  if (!Active()) return;
  std::lock_guard lock(g_mutex); g_ready.reset();
}
void CapturePlanesVideo(const uint8_t* base, uint32_t object, uint32_t data) {
  if (!Active() || !Range(object, 376)) return;
  try {
    { std::lock_guard lock(g_mutex); g_captured.reset(); g_ready.reset(); }
    const auto* vs = ShaderOriginal({base + 0x8200FCF8u, 260});
    const auto* ps0 = ShaderOriginal({base + 0x8200FE00u, 444});
    const auto* ps1 = ShaderOriginal({base + 0x8200FFC0u, 444});
    // The original shader's identity already includes its stage. Do not repeat it with
    // a boolean: swapping VS/PS here rejected every frame.
    if (!vs || !ps0 || !ps1 || ShaderOfObject(BE(base,object+72)) != vs) {
      Rejection(1,"VS del object o containers original no reconocidos"); return;
    }
    const uint32_t width = BE(base,object+124), height = BE(base,object+128);
    if (!width || !height || width > 1920 || height > 1080 || ((width|height)&1)) {
      Rejection(2,"dimensiones YUV no admitidas"); return;
    }
    const auto* pixel = ShaderOfObject(BE(base,object+76));
    if (pixel != ps0 && pixel != ps1) { Rejection(4,"PS del object no reconocido"); return; }
    auto f = std::make_shared<FrameVideo>();
    f->object = object; f->width = width; f->height = height;
    f->vs = vs; f->ps[0] = ps0; f->ps[1] = ps1; f->variant = pixel == ps1;
    uint64_t offset = 0;
    for (unsigned flat = 0; flat < 3; ++flat) {
      const uint32_t w = flat ? width/2 : width, h = flat ? height/2 : height;
      const uint32_t pitch = BE(base,object+344+flat*4);
      if (BE(base,object+332+flat*4) != w || BE(base,object+356+flat*4) != h ||
          pitch < w || pitch > 4096 || !Range(data, offset+uint64_t(pitch)*h)) {
        Rejection(8,"disposicion de planes o pitch no accepted"); return;
      }
      f->planes[flat].resize(size_t(w)*h);
      for (uint32_t y = 0; y < h; ++y) {
        // Plain loads: the Horizon handler is not required to emulate a SIMD memcpy
        // when a plane crosses a watched page of guest memory.
        const volatile uint8_t* source = base + data + offset + uint64_t(y)*pitch;
        auto* target = f->planes[flat].data() + size_t(y)*w;
        for (uint32_t x = 0; x < w; ++x) target[x] = source[x];
      }
      offset += uint64_t(pitch)*h;
    }
    static std::atomic<bool> first{true};
    if (first.exchange(false)) REXLOG_INFO("[video native] first capture YUV {}x{}; shaders verified",width,height);
    std::lock_guard lock(g_mutex); g_captured = std::move(f);
  } catch (const std::exception& e) { DisableVideo(e.what()); }
}
void NoteDrawVideo(const uint8_t* base, bool esVideo, uint32_t object) {
  if (!Active()) return;
  try {
    std::lock_guard lock(g_mutex);
    g_ready.reset();
    if (!esVideo || !g_captured || g_captured->object != object || !Range(object,264)) return;
    // sub_826D8408 copies these same 120 bytes to the vertex buffer.
    for (size_t i = 0; i < 6; ++i) {
      auto read = [&](unsigned j) { return std::bit_cast<float>(BE(base,object+144+uint32_t(i)*20+j*4)); };
      auto& v = g_captured->vertices[i]; v = {read(0),read(1),read(2),read(3),read(4)};
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.u) || !std::isfinite(v.v) ||
          std::abs(v.x)>1.1f || std::abs(v.y)>1.1f || v.z<0 || v.z>1 || v.u<0 || v.u>1 || v.v<0 || v.v>1) {
        Rejection(16,"vertices outside del range expected"); return;
      }
    }
    g_ready = std::move(g_captured);
  } catch (const std::exception& e) { DisableVideo(e.what()); }
}
void NoteSwapVideo() {
  if (!Active()) return;
  try {
    std::shared_ptr<const FrameVideo> f;
    { std::lock_guard lock(g_mutex); f = std::move(g_ready); g_captured.reset(); }
    if (!g_queue.Enqueue(std::move(f))) DisableVideo("queue de Swap full; se evita desincronizar frames");
  } catch (const std::exception& e) { DisableVideo(e.what()); }
}
std::shared_ptr<const FrameVideo> ConsumeVideo() {
  if (!Active()) return {};
  return g_queue.Consume();
}
}

REX_EXTERN(__imp__sub_82589DF0);
REX_HOOK_RAW(sub_82589DF0) {
  const bool video = ctx.lr == 0x826DB5C4 && ctx.r27.u32 == 0;
  const uint32_t object = ctx.r31.u32, data = ctx.r25.u32;
  __imp__sub_82589DF0(ctx, base);
  if (video) nfsmw::native::CapturePlanesVideo(base,object,data);
}
