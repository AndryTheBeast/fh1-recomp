// nfsc-recomp: short, accurate sleep (high-resolution timer on Windows). See nfsc_espera_corta.cpp.
#pragma once

#include <cstdint>

namespace nfsc {
void EsperaCorta(uint32_t microsegundos);
}  // namespace nfsc
