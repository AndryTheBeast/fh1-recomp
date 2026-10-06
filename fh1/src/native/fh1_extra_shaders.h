// fh1 - native renderer: shaders the library built from the disc does not have.
//
// The console's Direct3D rewrites some vertex shaders when it binds them to a vertex declaration (they come out
// shorter than any shader on the disc), so the game uploads microcode that no installer can find in the game's
// files. Until 2026-10-06 they were collected by hand from a running game (--fh1_dump_ring_shaders +
// tools/fh1_synth_containers.py) and added to the developer's library. Here the port does it itself on the
// user's PC: the first time the game uploads an unknown shader, a helper thread builds a container from the
// microcode alone (the same rebuild as that tool, taken from nfsc-recomp, GoatHonks, GPL-3.0), runs the
// translator and DXC that the installer put next to fh1.exe (folder "tools"), and saves container and SPIR-V in
// the folder "shaders_extra" next to fh1.exe. The renderer takes the result in when it is ready (the object
// drawn with it appears a moment late, once) and reads the folder at every later start.
// Everything in that folder is derived from the user's own game and never leaves the PC.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "fh1_shader_library.h"

namespace fh1::native {

// A 2008-layout container made from microcode alone; empty when the microcode cannot be walked.
std::vector<uint8_t> SynthContainer(bool vertices, std::span<const uint32_t> microcode);

namespace extra_shaders {

// The shaders saved by earlier sessions (container + SPIR-V), in a fixed order (file names).
std::vector<Shader> LoadSaved();

// Asks the helper thread for this shader (once per fingerprint and session). Cheap: copies the microcode.
// Does nothing when the shader tools are not there.
void Request(bool vertices, uint64_t fingerprint, std::span<const uint32_t> microcode);

// Waits until the helper thread is done with a shader asked for with Request (made, or failed), within what is
// left of --fh1_native_extra_shaders_wait_ms in the current 3 s. False when it was not asked for, the wait is off
// or the time ran out: the shader is then finished in the background (TakeFinished). Ring thread only.
bool Wait(bool vertices, uint64_t fingerprint);

// One shader the helper thread has finished since the last call; false when there is none (one atomic read).
bool TakeFinished(Shader& out);

}  // namespace extra_shaders
}  // namespace fh1::native
