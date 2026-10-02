// fh1 - native renderer, step N1: the app's own graphics system (docs/native-renderer-fh1.md).
//
// With --fh1_renderer=native, OnPreSetup (fh1_app.h) puts this system in config.graphics and the
// xenos GPU emulation plugin is not loaded. Modelled on nfsmw-nx's nfsmw_nativo_sistema.cpp (stage
// C1): it answers what the game expects from the GPU and presents a test colour; draws come later.

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace fh1::native {

// true if --fh1_renderer asks for the native graphics system.
bool Active();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace fh1::native
