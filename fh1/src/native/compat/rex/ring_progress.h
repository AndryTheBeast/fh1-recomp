/**
 * @file        ring_progress.h
 * @brief       "The GPU command processor made progress": lets a game thread that polls the ring
 *              read pointer sleep until it has something new to read, instead of spinning.
 *
 * The command processor calls NotifyRingProgress() right after it publishes the read pointer (and after
 * interrupts and vblank). An app hook on the game's Direct3D wait calls WaitRingProgress().
 *
 * Design from nfsmw-nx (StevensND, GPL-3.0; app/src/nfsmw_espera_anillo.cpp, "ProgresoAnillo"), moved into the
 * SDK core so the GPU plugin and the app can both reach it.
 */

#pragma once

#include <chrono>
#include <cstdint>

namespace rex {

/// Counter that increases on every notification.
uint32_t RingProgress();

/// Called by the command processor after it published progress.
void NotifyRingProgress();

/// Waits at most `limit` for RingProgress() to differ from `seen`. Returns true if it advanced.
bool WaitRingProgress(uint32_t seen, std::chrono::microseconds limit);

}  // namespace rex
