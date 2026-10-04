// nfsc-recomp: short, accurate sleep (high-resolution timer on Windows). See nfsc_short_wait.cpp.
#pragma once

#include <cstdint>

namespace nfsc {
void WaitShort(uint32_t microseconds);
}  // namespace nfsc
