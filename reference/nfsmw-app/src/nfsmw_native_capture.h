// nfsmw - captures of the game image for the native renderer tests. With
// nfsmw_capture_every_s > 0 it saves a PNG every N seconds in capturas/ next
// to the executable, in native and in emulation mode, to compare the two
// without looking at the screen.

#pragma once

#include <functional>

namespace rex::ui {
class Presenter;
}

namespace nfsmw::capture {

// Starts the capture thread if the cvar asks for it. The presenter is requested
// on every capture: it may not exist yet at startup.
void Start(std::function<rex::ui::Presenter*()> get_presenter);

// Stops the thread (idempotent).
void Stop();

}  // namespace nfsmw::capture
