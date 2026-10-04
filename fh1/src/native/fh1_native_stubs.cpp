// fh1: neutral stand-ins for the Most Wanted-specific helpers that the copied native renderer sources call
// (phase 1 of docs/carbon/native-renderer-plan.md). Each one does nothing or returns the safe default; they are
// replaced one by one as the Carbon equivalents are found. The original code is in nfsmw-nx (StevensND, GPL-3.0).

#include <rex/ring_progress.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace fh1::reflection_demand {
// Reflection-on-demand is a Most Wanted shortcut (a reflection resolved to a fixed address); off here.
void NoteRead() {}
void NoteCopy() {}
void NoteSwap() {}
bool MeasureVisibility() { return false; }
bool VisibilityChecked() { return false; }
void NoteVisible(bool) {}
void NoteHidden() {}
void NoteWitness(bool) {}
}  // namespace fh1::reflection_demand

namespace fh1::guard30 {
// The 30 FPS guard (drops shadows when the frame rate falls): not used.
void Beat(double) {}
void Report() {}
bool WithoutShadows(bool of_user) { return of_user; }
}  // namespace fh1::guard30

namespace fh1::scenery_lod {
std::string Summary() { return std::string(); }
}  // namespace fh1::scenery_lod

namespace fh1::render_targets {
// Samples per pixel of the game's current anti-aliasing mode: 1 until the Carbon location is known.
uint32_t SamplesOriginalModeCurrent(const uint8_t*) { return 1; }
}  // namespace fh1::render_targets

// Frames counted by the game thread (Most Wanted hook); the ring thread compares it with its own count.
std::atomic<uint64_t> g_fh1_frames_game{0};

namespace fh1::native {
// Direct3D-level markers: need the Carbon D3D hooks (phase 2).
void ActivateConsumerMarkers(bool) {}
void NoteCheckMarker(bool, uint32_t, uint32_t, uint32_t, uint32_t) {}
// Ring progress: wakes the game's Direct3D ring wait (carbon/src/fh1_d3d_wait.cpp), which sleeps in
// rex::WaitRingProgress. This was an empty stand-in, so on the native renderer every one of those sleeps ran to its
// 2 ms cap (log: "0 ended by GPU progress") and a run of them showed up as the 160-180 ms hitches in Free Roam.
void NotifyProgressRing() { rex::NotifyRingProgress(); }
}  // namespace fh1::native
