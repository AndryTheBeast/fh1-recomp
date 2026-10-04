/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_INT32(audio_dump_s, 0, "Audio",
                     "Diagnostic: seconds of the audio the game hands over (6 channels, before the driver) saved "
                     "to audio_dump_<client>.wav next to the executable; 0 = nothing");
REXCVAR_DEFINE_INT32(audio_dump_since_s, 0, "Audio",
                     "Diagnostic: seconds of each client's audio skipped before the dump");

namespace {

// Diagnostics for the robotic audio: what the game delivers, the same on the PC and on the Switch.
// Each frame is 6 channels x 256 samples in big-endian float, channel by channel; the WAV is
// interleaved little-endian float at 48 kHz. It is kept in memory and written in one go.
struct ClientDump {
  uint64_t skipped = 0;
  std::vector<float> sample_total;
  bool write_pos = false;
};

std::mutex dump_mutex;
std::unordered_map<uint32_t, ClientDump> dump_clients;

// Dumps are written by a single thread that lives until the end of the process: on the Switch SD a
// file can take seconds and the game's audio thread must not wait for that. No thread is created
// per file: on the Switch, std::thread::detach() threw std::system_error (the writer thread had
// already finished) and the game closed. If the thread cannot be created, the file is written on
// the spot.
class WriterDumps {
 public:
  static void Enqueue(std::function<void()> work) {
    static WriterDumps* const writer = Create();  // never destroyed
    if (!writer) {
      work();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(writer->mutex_);
      writer->queue_.push_back(std::move(work));
    }
    writer->warning_.notify_one();
  }

 private:
  WriterDumps() : thread_([this]() { Loop(); }) {}

  static WriterDumps* Create() {
    try {
      return new WriterDumps();
    } catch (const std::system_error&) {
      return nullptr;
    }
  }

  void Loop() {
    for (;;) {
      std::function<void()> work;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        warning_.wait(lock, [this]() { return !queue_.empty(); });
        work = std::move(queue_.front());
        queue_.pop_front();
      }
      work();
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::function<void()>> queue_;
  std::thread thread_;  // last: starts with the other members already constructed
};

void WriteDump(uint32_t client, std::vector<float> sample_total) {
  WriterDumps::Enqueue([client, sample_total = std::move(sample_total)]() {
    const auto path = rex::filesystem::GetExecutableFolder() /
                      ("audio_dump_" + std::to_string(client) + ".wav");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
      REXKRNL_WARN("[audio] could not create the dump {}", path.string());
      return;
    }
    const uint32_t bytes = uint32_t(sample_total.size() * sizeof(float));
    const auto u32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    u32(36 + bytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(3);  // float IEEE
    u16(6);
    u32(48000);
    u32(48000 * 6 * 4);
    u16(6 * 4);
    u16(32);
    file.write("data", 4);
    u32(bytes);
    file.write(reinterpret_cast<const char*>(sample_total.data()), bytes);
    REXKRNL_INFO("[audio] dump of client {}: {} samples per channel in {}", client,
                 sample_total.size() / 6, path.string());
  });
}

void DumpFrame(uint32_t client, const uint8_t* frame) {
  const int32_t seconds = REXCVAR_GET(audio_dump_s);
  if (seconds <= 0 || !frame) {
    return;
  }
  std::lock_guard<std::mutex> lock(dump_mutex);
  ClientDump& c = dump_clients[client];
  if (c.write_pos) {
    return;
  }
  const uint64_t skip = uint64_t(std::max(REXCVAR_GET(audio_dump_since_s), 0)) * 48000;
  if (c.skipped < skip) {
    c.skipped += 256;
    return;
  }
  const size_t target_2 = size_t(seconds) * 48000 * 6;
  if (c.sample_total.capacity() < target_2) {
    c.sample_total.reserve(target_2);
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (uint32_t canal = 0; canal < 6; ++canal) {
      const uint8_t* b = frame + (size_t(canal) * 256 + i) * 4;
      const uint32_t bits = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
                            (uint32_t(b[2]) << 8) | uint32_t(b[3]);
      float input_value;
      std::memcpy(&input_value, &bits, sizeof(input_value));
      c.sample_total.push_back(input_value);
    }
  }
  if (c.sample_total.size() >= target_2) {
    c.write_pos = true;
    WriteDump(client, std::move(c.sample_total));
    c.sample_total = std::vector<float>();
  }
}

}  // namespace

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

u32 XAudioGetSpeakerConfig_entry(mapped_u32 config_ptr) {
  *config_ptr = 0x00010001;
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolumeChangeMask_entry(mapped_void driver_ptr, mapped_u32 out_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  rex::thread::Sleep(std::chrono::microseconds(1));

  // Checking these bits to see if any voice volume changed.
  // I think.
  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolume_entry(u32 unk, mapped_f32 out_ptr) {
  // Expects a floating point single. Volume %?
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}

u32 XAudioEnableDucker_entry(u32 unk) {
  return X_ERROR_SUCCESS;
}

u32 XAudioRegisterRenderDriverClient_entry(mapped_u32 callback_ptr, mapped_u32 driver_ptr) {
  REXKRNL_DEBUG("XAudioRegisterRenderDriverClient called! callback_ptr={:08X} driver_ptr={:08X}",
                callback_ptr.guest_address(), driver_ptr.guest_address());
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  uint32_t callback_arg = callback_ptr[1];

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());

  size_t index;
  auto result = audio_system->RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    return result;
  }

  assert_true(!(index & ~0x0000FFFF));
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioUnregisterRenderDriverClient_entry(mapped_void driver_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioSubmitRenderDriverFrame_entry(mapped_void driver_ptr, mapped_void samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  static uint32_t submit_krnl_count = 0;
  if (submit_krnl_count < 10) {
    REXKRNL_DEBUG("XAudioSubmitRenderDriverFrame: driver={:08X} samples={:08X}",
                  driver_ptr.guest_address(), samples_ptr.guest_address());
    submit_krnl_count++;
  }

  // Diagnostics (audio_dump_s): copy of the frame before the driver; nothing else changes.
  DumpFrame(driver_ptr.guest_address() & 0x0000FFFF,
              REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(samples_ptr.guest_address()));

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples_ptr.guest_address());

  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XAudioGetSpeakerConfig, rex::kernel::xboxkrnl::XAudioGetSpeakerConfig_entry)
REX_EXPORT(__imp__XAudioGetVoiceCategoryVolumeChangeMask,
           rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolumeChangeMask_entry)
REX_EXPORT(__imp__XAudioGetVoiceCategoryVolume,
           rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolume_entry)
REX_EXPORT(__imp__XAudioEnableDucker, rex::kernel::xboxkrnl::XAudioEnableDucker_entry)
REX_EXPORT(__imp__XAudioRegisterRenderDriverClient,
           rex::kernel::xboxkrnl::XAudioRegisterRenderDriverClient_entry)
REX_EXPORT(__imp__XAudioUnregisterRenderDriverClient,
           rex::kernel::xboxkrnl::XAudioUnregisterRenderDriverClient_entry)
REX_EXPORT(__imp__XAudioSubmitRenderDriverFrame,
           rex::kernel::xboxkrnl::XAudioSubmitRenderDriverFrame_entry)

REX_EXPORT_STUB(__imp__XAudioRenderDriverInitialize);
REX_EXPORT_STUB(__imp__XAudioRenderDriverLock);
REX_EXPORT_STUB(__imp__XAudioSetVoiceCategoryVolume);
REX_EXPORT_STUB(__imp__XAudioBeginDigitalBypassMode);
REX_EXPORT_STUB(__imp__XAudioEndDigitalBypassMode);
REX_EXPORT_STUB(__imp__XAudioSubmitDigitalPacket);
REX_EXPORT_STUB(__imp__XAudioQueryDriverPerformance);
REX_EXPORT_STUB(__imp__XAudioGetRenderDriverThread);
REX_EXPORT_STUB(__imp__XAudioSetSpeakerConfig);
REX_EXPORT_STUB(__imp__XAudioOverrideSpeakerConfig);
REX_EXPORT_STUB(__imp__XAudioSuspendRenderDriverClients);
REX_EXPORT_STUB(__imp__XAudioRegisterRenderDriverMECClient);
REX_EXPORT_STUB(__imp__XAudioUnregisterRenderDriverMECClient);
REX_EXPORT_STUB(__imp__XAudioCaptureRenderDriverFrame);
REX_EXPORT_STUB(__imp__XAudioGetRenderDriverTic);
REX_EXPORT_STUB(__imp__XAudioSetDuckerLevel);
REX_EXPORT_STUB(__imp__XAudioIsDuckerEnabled);
REX_EXPORT_STUB(__imp__XAudioGetDuckerLevel);
REX_EXPORT_STUB(__imp__XAudioGetDuckerThreshold);
REX_EXPORT_STUB(__imp__XAudioSetDuckerThreshold);
REX_EXPORT_STUB(__imp__XAudioGetDuckerAttackTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerAttackTime);
REX_EXPORT_STUB(__imp__XAudioGetDuckerReleaseTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerReleaseTime);
REX_EXPORT_STUB(__imp__XAudioGetDuckerHoldTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerHoldTime);
REX_EXPORT_STUB(__imp__XAudioGetUnderrunCount);
REX_EXPORT_STUB(__imp__XAudioSetProcessFrameCallback);
