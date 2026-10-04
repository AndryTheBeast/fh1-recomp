#include "nfsmw_shader_hooks.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>

namespace nfsmw::native {
namespace {
LibraryShaders g_library;
std::atomic<bool> g_list{false};
std::mutex g_mutex;
std::unordered_map<uint32_t, const Shader*> g_objects;

uint32_t ReadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

const Shader* Identify(const uint8_t* base, uint32_t address, bool vertices) {
  if (!g_list.load(std::memory_order_acquire) || !address || address > UINT32_MAX - 24)
    return nullptr;
  const uint8_t* p = base + address;
  if (ReadBE(p) != (vertices ? 0x102A0E01u : 0x102A0E00u)) return nullptr;
  const uint32_t virtuales = ReadBE(p + 4), physical = ReadBE(p + 8);
  const uint64_t total = uint64_t(virtuales) + physical;
  if (virtuales < 24 || !physical || total > 65536 ||
      uint64_t(address) + total > (uint64_t(1) << 32)) return nullptr;
  return g_library.Find(std::span<const uint8_t>(p, size_t(total)));
}

void Remember(uint32_t object, const Shader* shader) {
  if (!object || !g_list.load(std::memory_order_acquire)) return;
  try {
    std::lock_guard lock(g_mutex);
    // The address can be reused after Release. An unknown shader invalidates any
    // previous association with that same address.
    if (shader) g_objects.insert_or_assign(object, shader);
    else g_objects.erase(object);
  } catch (const std::exception& e) {
    // Never let an exception from the experimental registry reach the guest.
    // Disabling all lookups avoids using stale associations.
    g_list.store(false, std::memory_order_release);
    REXLOG_WARN("Register de shaders native disabled: {}", e.what());
  }
}
}  // namespace

void StartLibraryShaders() {
  static std::once_flag start;
  std::call_once(start, [] {
    try {
      g_library.Load(rex::filesystem::GetExecutableFolder() / "nfsmw_shaders.nfsp");
      g_list.store(true, std::memory_order_release);
      REXLOG_INFO("Library experimental: {} shaders native; video available, rest de drawn Xenos",
                  g_library.shaders().size());
    } catch (const std::exception& e) {
      REXLOG_WARN("Library de shaders native no available: {}", e.what());
    }
  });
}

const Shader* ShaderOfObject(uint32_t object) {
  if (!g_list.load(std::memory_order_acquire) || !object) return nullptr;
  std::lock_guard lock(g_mutex);
  auto it = g_objects.find(object);
  return it != g_objects.end() ? it->second : nullptr;
}
const Shader* ShaderOriginal(std::span<const uint8_t> container) {
  if (!g_list.load(std::memory_order_acquire)) return nullptr;
  return g_library.Find(container);
}
}  // namespace nfsmw::native

// These constructors receive the original contiguous container in r3 and return
// the object in r3 (or zero). The hash is computed before the driver's copies.
// The original function always runs and no PPC register is modified.
REX_EXTERN(__imp__sub_8259BC90);
REX_HOOK_RAW(sub_8259BC90) {
  const auto* shader = nfsmw::native::Identify(base, ctx.r3.u32, false);
  __imp__sub_8259BC90(ctx, base);
  nfsmw::native::Remember(ctx.r3.u32, shader);
}

REX_EXTERN(__imp__sub_8259C038);
REX_HOOK_RAW(sub_8259C038) {
  const auto* shader = nfsmw::native::Identify(base, ctx.r3.u32, true);
  __imp__sub_8259C038(ctx, base);
  nfsmw::native::Remember(ctx.r3.u32, shader);
}
