// fh1 - native renderer, step C1: the app's own graphics system.
//
// With fh1_renderer = "native", OnPreSetup puts this system in
// config.graphics and the Xenos emulation plugin is not loaded. Details are
// in docs/nfsmw-nx/native-renderer.md.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace fh1::native {

// true if the fh1_renderer cvar asks for the native path.
bool Active();

// Ring thread progress, for the game's D3D waits (fh1_d3d_wait.cpp): it increases every time
// the ring thread returns the read pointer or delivers an interrupt.
uint32_t ProgressRing();

// Waits at most 'limit' for ProgressRing() to differ from 'seen'. true if it advanced.
bool WaitProgressRing(uint32_t seen, std::chrono::microseconds limit);

// Called by the ring thread after writing the read pointer to game memory, and when delivering
// an interrupt. Cheap if nobody is waiting.
void NotifyProgressRing();

// Swaps presented by the native renderer since startup (0 with Xenos emulation). The watchdog in
// fh1_app.h counts them as progress: with blocking waits the game threads sleep in the same place
// for most of the frame.
uint64_t SwapsNative();

// The pipeline list (this PC's and the one shipped with the port) is built before the game's code starts:
// fh1_app.h calls this instead of launching, shows the progress and launches when it has ended. A pipeline
// compiled while the game runs is a hitch, and one frame of about 3.2 s freezes the game for good; before
// the game runs there is nothing to freeze. false = nothing to wait for (launch at once).
bool PrewarmBeforeLaunch();
// Pipelines walked, pipelines in the list, and whether it has ended. Any thread.
void PrewarmProgress(uint32_t& done, uint32_t& total, bool& finished);

// The native graphics system: the SDK presenter, a reserved MMIO range, the vblank
// thread and the command ring sink.
std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics();

}  // namespace fh1::native
