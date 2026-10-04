// nfsc-recomp: short, accurate sleep (high-resolution timer on Windows). See fh1_short_wait.cpp.
#pragma once

#include <cstdint>

namespace fh1 {
void WaitShort(uint32_t microseconds);
}  // namespace fh1
