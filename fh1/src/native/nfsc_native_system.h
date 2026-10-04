// nfsc - native renderer, step C1: the app's own graphics system.
//
// With nfsc_renderer = "native", OnPreSetup puts this system in
// config.graphics and the Xenos emulation plugin is not loaded. Details are
// in docs/native-renderer.md.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace nfsc::native {

// true if the nfsc_renderer cvar asks for the native path.
bool Active();

// Ring thread progress, for the game's D3D waits (nfsc_d3d_wait.cpp): it increases every time
// the ring thread returns the read pointer or delivers an interrupt.
uint32_t ProgressRing();

// Waits at most 'limit' for ProgressRing() to differ from 'seen'. true if it advanced.
bool WaitProgressRing(uint32_t seen, std::chrono::microseconds limit);

// Called by the ring thread after writing the read pointer to game memory, and when delivering
// an interrupt. Cheap if nobody is waiting.
void NotifyProgressRing();

// Swaps presented by the native renderer since startup (0 with Xenos emulation). The watchdog in
// nfsc_app.h counts them as progress: with blocking waits the game threads sleep in the same place
// for most of the frame.
uint64_t SwapsNative();

// The native graphics system: the SDK presenter, a reserved MMIO range, the vblank
// thread and the command ring sink.
std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics();

}  // namespace nfsc::native
