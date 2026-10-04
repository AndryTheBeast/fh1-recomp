// nfsc - captures of the game image for the native renderer tests. With
// nfsc_capture_every_s > 0 it saves a PNG every N seconds in captures/ next
// to the executable, in native and in emulation mode, to compare the two
// without looking at the screen.

#pragma once

#include <cstdint>
#include <functional>

namespace rex::ui {
class Presenter;
}

namespace nfsc::capture {

// Starts the capture thread if the cvar asks for it. The presenter is requested
// on every capture: it may not exist yet at startup.
void Start(std::function<rex::ui::Presenter*()> get_presenter);

// Stops the thread (idempotent).
void Stop();

// NFSC debug: writes an RGBA8 image (rows packed, alpha dropped) as a PNG file. Returns false on failure.
bool SavePngRgba(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace nfsc::capture
