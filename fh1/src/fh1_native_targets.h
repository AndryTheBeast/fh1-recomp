// fh1 - native renderer, step N3a: render targets, copies (resolve and clear) and presentation of the
// game's image, without EDRAM (docs/native-renderer-fh1.md). Lean FH1 version of nfsmw-nx's
// nfsmw_nativo_destinos.* (stage C2): a render target is one Vulkan image per (EDRAM base, format,
// pitch); a resolved texture is one image per destination address, where the Swap looks it up.
// Everything runs on the native system's ring thread: no locks.

#pragma once

#include <cstdint>
#include <memory>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace fh1::native {

// Registers a copy needs, from the ring's register mirror when a draw runs in copy mode.
struct CopyRegisters {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info[4] = {};
  uint32_t rb_depth_info = 0;
  uint32_t rb_copy_control = 0;
  uint32_t rb_copy_dest_base = 0;
  uint32_t rb_copy_dest_pitch = 0;
  uint32_t rb_copy_dest_info = 0;
  uint32_t rb_color_clear = 0;
  uint32_t rb_color_clear_lo = 0;
  uint32_t rb_depth_clear = 0;
  uint32_t pa_sc_window_offset = 0;
  uint32_t pa_sc_window_scissor_tl = 0;
  uint32_t pa_sc_window_scissor_br = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_su_vtx_cntl = 0;
  uint32_t fetch_vertices[2] = {};  // fetch constant 0 as vertices: the copy rectangle (D3D puts it there)
};

struct TargetStats {
  uint64_t copies = 0, clears = 0, presented = 0, rejected = 0, depth_copies = 0;
  uint32_t render_targets = 0, resolved = 0;
};

class Targets {
 public:
  // nullptr if the device is missing or setup failed.
  static std::unique_ptr<Targets> Create(const rex::ui::vulkan::VulkanDevice* device, rex::memory::Memory* memory);
  virtual ~Targets() = default;
  // Resolve and/or clear. false if it could not be done (each cause is logged once).
  virtual bool Copy(const CopyRegisters& registers) = 0;
  // Submits the frame's work and draws the resolved texture at guest address `frontbuffer` into the
  // presenter's output. false if there is no such resolved texture (the caller shows something else).
  virtual bool Present(rex::ui::Presenter* presenter, uint32_t frontbuffer, uint32_t width, uint32_t height) = 0;
  virtual TargetStats Stats() const = 0;
};

}  // namespace fh1::native
