/**
 * @file        kernel/input_events.h
 * @brief       Observer of the controller state the guest reads (XamInputGetState).
 *
 * Lets the app record what the player does (FH1's test autoplay recorder). Called on the reading
 * guest thread after a successful read; keep it cheap.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace rex::kernel {

struct InputSnapshot {
  uint32_t user = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
};

using InputObserver = std::function<void(const InputSnapshot&)>;

// Replaces the observer (nullptr removes it).
void SetInputObserver(InputObserver observer);

}  // namespace rex::kernel
