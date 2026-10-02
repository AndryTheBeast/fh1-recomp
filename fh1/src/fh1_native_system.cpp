// fh1 - native renderer, step N1: the app's own graphics system (docs/native-renderer-fh1.md).
//
// Replaces the xenos GPU emulation plugin with --fh1_renderer=native. Modelled on nfsmw-nx's
// nfsmw_nativo_sistema.cpp, stage C1 (reference/nfsmw-app/src): the game's Direct3D keeps running
// unchanged and fills its command ring; a ring thread consumes it right away and answers what the
// game expects from the GPU:
//   1. MMIO registers at 0x7FC80000 (fixed values, as the emulation's graphics_system.cpp);
//   2. the ring read pointer, written back to guest memory (the game watches it for free space);
//   3. interrupts: vblank (display rate) and PM4_INTERRUPT;
//   4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM, EVENT_WRITE_*, SCRATCH registers)
//      and WAIT_REG_MEM, with the semantics of the SDK's graphics/command_processor.cpp.
// Draws and resolves are ignored for now; every Swap presents a test colour whose green pulses, so
// a screenshot tells a running game from a stalled one.

#include "fh1_native_system.h"

#include "fh1_native_shaders.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(fh1_renderer, "xenos", "FH1",
                      "xenos = the SDK's Xbox 360 GPU emulation; native = the native renderer in "
                      "development (step N1: the game runs without the emulation, the screen shows a "
                      "test colour)")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(fh1_shader_library, "", "FH1",
                      "Native renderer: shader library (fh1_shaders.nfsp from "
                      "tools/build_shader_library.ps1); empty = next to the executable")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fh1::native {

bool Active() { return REXCVAR_GET(fh1_renderer) == "native"; }

namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
using OutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
constexpr uint32_t kOcclusionSamples = 1000;  // like query_occlusion_fake_sample_count
constexpr uint16_t kMaxExtent = 2048 >> 3;
constexpr int kMaxIndirectDepth = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

// Big-endian words of a command buffer. In the ring the position wraps (power-of-two size); in an
// indirect buffer it does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // ring words - 1; 0 for a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;
  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    uint32_t v = Peek();
    Advance(1);
    return v;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < reference;
    case 0x2: return value <= reference;
    case 0x3: return value == reference;
    case 0x4: return value != reference;
    case 0x5: return value >= reference;
    case 0x6: return value > reference;
    default: return true;
  }
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) return X_STATUS_SUCCESS;
    app_context_ = app_context;
    if (!provider_) {
      // XenosRecomp's SPIR-V needs device features that must be requested before the device exists.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] native graphics system (N1): SDK presenter, no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ =
          memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &MmioRead, &MmioWrite);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) return X_STATUS_SUCCESS;
    // Step N2: the shader library is built from the game data, so it is not distributed. Without it
    // the game still runs, without shader identification.
    {
      std::filesystem::path library = std::string(REXCVAR_GET(fh1_shader_library));
      if (library.empty()) library = rex::filesystem::GetExecutableFolder() / "fh1_shaders.nfsp";
      shaders_.Load(library);
    }
    last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync (native)");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring (native)");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_pointer_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    if (active_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
      Report(true);
    }
    DestroyVulkan();
    if (presenter_) {
      if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO registers ---------------------------------------------------------------------------

  static uint32_t MmioRead(void*, void* context, uint32_t address) {
    return static_cast<NativeGraphicsSystem*>(context)->ReadMmio(address);
  }
  static void MmioWrite(void*, void* context, uint32_t address, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(context)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default:
        return Register(r);
    }
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);  // no lost wakeup
      }
      ring_cv_.notify_one();
    }
    if (r < kRegisterCount) registers_[r] = value;  // MMIO writes have no other side effects
  }

  // --- Registers written by the ring, with their side effects (CommandProcessor::WriteRegister) --

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) value |= UINT32_C(0x80000000);
    registers_[index] = value;
    // SCRATCH_REG values are copied to memory: the game's D3D writes one and then waits with
    // WAIT_REG_MEM until it shows up at SCRATCH_ADDR.
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 && index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
      }
    }
  }

  // GPU memory: the two low address bits are the byte order.
  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }
  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t ordered = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &ordered, sizeof(ordered));
  }

  // --- Threads ----------------------------------------------------------------------------------

  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;  // no replay after a long pause
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0, generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
      }
      if (!active_.load(std::memory_order_acquire)) break;
      Report(false);
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) continue;
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) continue;
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) break;
      }
      read = reader.pos;
      if (const uint32_t writeback = read_pointer_writeback_.load(std::memory_order_acquire)) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
    }
    return 0;
  }

  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) return false;  // incomplete: wait for the rest
    reader.Advance(1);
    ++packets_;
    Reader data = reader;
    reader.Advance(count);
    if (!packet) return true;
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool one_register = (packet >> 15) & 0x1;
        for (uint32_t i = 0; i < count; ++i) WriteRegister(one_register ? index : index + i, data.Read());
        break;
      }
      case 1: {
        const uint32_t v1 = data.Read(), v2 = data.Read();
        WriteRegister(packet & 0x7FF, v1);
        WriteRegister((packet >> 11) & 0x7FF, v2);
        break;
      }
      case 2:
        break;
      default:
        Type3(data, packet, count, depth);
        break;
    }
    return true;
  }

  void Type3(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    ++opcodes_[opcode];
    // Predicated packets run only in the selected bins (FH1's tiling strips); predicated Swaps never.
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) return;
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) Interrupt(1, cpu);
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        Present();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          Reader indirect;
          indirect.base = memory_->TranslatePhysical(address);
          indirect.end = length;
          while (indirect.Pending()) {
            if (!Packet(indirect, depth + 1)) break;
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read(), poll = data.Read(), reference = data.Read(), mask = data.Read();
        const auto limit = Clock::now() + kWaitRegMemMax;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronise
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) break;
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (wait_timeouts_.fetch_add(1, std::memory_order_relaxed) < 8) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          rex::thread::Sleep(std::chrono::microseconds(100));
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read(), and_value = data.Read(), or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg = data.Read(), address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i, address += 4) WriteMemory(address, data.Read());
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read(), poll = data.Read(), reference = data.Read(), mask = data.Read();
        const uint32_t target = data.Read(), value = data.Read();
        const uint32_t current = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, current & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, value);
          } else {
            WriteRegister(target, value);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        const uint32_t initiator = data.Read(), address = data.Read(), value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed) : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read(), address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the objects between the extent events: the whole screen (as the SDK).
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = Register(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (!address) break;
        // Occlusion queries: every object visible (fixed sample count, as the emulation).
        auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
        std::memset(counts, 0, sizeof(*counts));
        counts->ZPass_A = kOcclusionSamples;
        counts->Total_A = kOcclusionSamples;
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registers
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < words; ++i) WriteRegister(index + i - 1, data.Read());
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t size = data.Read() & 0xFFF;
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;
          case 1: base = 0x4800; break;
          case 2: base = 0x4900; break;
          case 3: base = 0x4908; break;
          case 4: base = 0x2000; break;
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          const uint8_t* source = memory_->TranslatePhysical(address);
          for (uint32_t i = 0; i < size; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + i * 4));
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK:
        if (words >= 2) {
          const uint64_t high = data.Read();
          bin_mask_ = (high << 32) | data.Read();
        }
        break;
      case xenos::PM4_SET_BIN_SELECT:
        if (words >= 2) {
          const uint64_t high = data.Read();
          bin_select_ = (high << 32) | data.Read();
        }
        break;
      case xenos::PM4_IM_LOAD: {
        // Shader upload by address: type in bits 0-1 (0 vertex, 1 pixel), size in words.
        if (words < 2) break;
        const uint32_t address_type = data.Read(), start_size = data.Read();
        LoadShader((address_type & 0x3) == 0, memory_->TranslatePhysical(address_type & ~uint32_t(0x3)),
                   start_size & 0xFFFF);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Shader upload inline: type, start/size, then the microcode.
        if (words < 2) break;
        const uint32_t type = data.Read(), start_size = data.Read();
        const uint32_t size = std::min(start_size & 0xFFFF, words - 2);
        scratch_.resize(size);
        for (uint32_t i = 0; i < size; ++i) scratch_[i] = data.Read();
        IdentifyShader((type & 0x3) == 0);
        break;
      }
      default:
        // Draws, resolves (draws in copy mode), NOPs...: not handled yet.
        break;
    }
  }

  // --- Shaders (step N2) ---------------------------------------------------------------------------

  void LoadShader(bool vertex, const uint8_t* source, uint32_t size) {
    scratch_.resize(size);
    for (uint32_t i = 0; i < size; ++i) scratch_[i] = rex::memory::load_and_swap<uint32_t>(source + i * 4);
    IdentifyShader(vertex);
  }

  void IdentifyShader(bool vertex) {
    if (!shaders_.loaded()) return;
    const ShaderEntry* entry = shaders_.Identify(vertex, scratch_);
    (vertex ? current_vs_ : current_ps_) = entry;
  }

  // --- Test presentation --------------------------------------------------------------------------

  void Present() {
    const uint64_t swap = swaps_.fetch_add(1, std::memory_order_relaxed) + 1;
    counter_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) return;
    presenter_->RefreshGuestOutput(kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
                                   [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& context) {
                                     return ClearOutput(static_cast<OutputContext&>(context), swap);
                                   });
  }

  bool ClearOutput(OutputContext& context, uint64_t swap) {
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();
    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }
    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = device_info->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo allocate{};
      allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocate.commandPool = pool_;
      allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &allocate, &commands_) != VK_SUCCESS) commands_ = VK_NULL_HANDLE;
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) fence_ = VK_NULL_HANDLE;
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) return false;
    if (fence_pending_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) framebuffer = f.framebuffer;
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &begin) != VK_SUCCESS) return false;
    // Test colour: the green pulses with each Swap (a stalled game shows a fixed colour).
    const float phase = float(swap % 120) / 119.0f;
    VkClearValue colour{};
    colour.color.float32[0] = 0.05f;
    colour.color.float32[1] = 0.10f + 0.40f * phase;
    colour.color.float32[2] = 0.35f;
    colour.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &colour;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) return false;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    {
      const auto queue = device_info->AcquireQueue(device_info->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) return false;
    }
    fence_pending_ = true;
    context.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    if (!provider_ || !provider_->vulkan_device()) return;
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
      fence_pending_ = false;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
      f = Framebuffer{};
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // --- Report (every 10 s, ring thread) ----------------------------------------------------------

  void Report(bool final_report) {
    const auto now = Clock::now();
    if (!final_report && now - last_report_ < std::chrono::seconds(10)) return;
    const double seconds = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    const uint64_t swaps = swaps_.load(std::memory_order_relaxed);
    std::string top;
    std::array<std::pair<uint64_t, uint32_t>, 128> counts{};
    for (uint32_t i = 0; i < 128; ++i) counts[i] = {opcodes_[i] - reported_opcodes_[i], i};
    std::sort(counts.begin(), counts.end(), std::greater<>());
    for (size_t i = 0; i < 6 && counts[i].first; ++i) top += fmt::format(" {:02X}x{}", counts[i].second, counts[i].first);
    for (uint32_t i = 0; i < 128; ++i) reported_opcodes_[i] = opcodes_[i];
    REXLOG_INFO("[native] {:.1f} swaps/s ({} total), {} vblanks, {} interrupts, {} packets, {} WAIT_REG_MEM "
                "timeouts; type-3 opcodes:{}",
                double(swaps - reported_swaps_) / std::max(seconds, 0.001), swaps, vblanks_.load(),
                interrupts_.load(), packets_, wait_timeouts_.load(), top);
    if (shaders_.loaded()) {
      const ShaderStats st = shaders_.Stats();
      REXLOG_INFO("[native] shaders: {} uploads, {} distinct microcodes: {} identified ({} ambiguous), {} not "
                  "identified; vertex {}/{} ({} by the tolerant pass), pixel {}/{}",
                  st.loads, st.distinct, st.identified, st.ambiguous, st.unidentified, st.identified_vertex,
                  st.identified_vertex + st.unidentified_vertex, st.loose, st.identified_pixel,
                  st.identified_pixel + st.unidentified_pixel);
    }
    reported_swaps_ = swaps;
  }

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
  };

  Shaders shaders_;
  std::vector<uint32_t> scratch_;
  const ShaderEntry* current_vs_ = nullptr;
  const ShaderEntry* current_ps_ = nullptr;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  std::vector<uint32_t> registers_;
  std::atomic<uint32_t> callback_{0}, callback_data_{0};
  std::atomic<uint32_t> ring_base_{0}, ring_words_{0}, ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0}, read_pointer_writeback_{0};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  uint64_t bin_mask_ = ~uint64_t(0), bin_select_ = ~uint64_t(0);
  std::atomic<uint64_t> counter_{0}, vblanks_{0}, interrupts_{0}, swaps_{0};
  std::atomic<uint32_t> wait_timeouts_{0};
  uint64_t packets_ = 0;
  uint64_t opcodes_[128] = {}, reported_opcodes_[128] = {};
  uint64_t reported_swaps_ = 0;
  Clock::time_point last_report_;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  std::array<Framebuffer, 4> framebuffers_{};
  size_t next_framebuffer_ = 0;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace fh1::native
