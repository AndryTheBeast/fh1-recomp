// nfsc: neutral stand-ins for the Most Wanted-specific helpers that the copied native renderer sources call
// (phase 1 of docs/carbon/native-renderer-plan.md). Each one does nothing or returns the safe default; they are
// replaced one by one as the Carbon equivalents are found. The original code is in nfsmw-nx (StevensND, GPL-3.0).

#include <rex/ring_progress.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace nfsmw::reflejo_demanda {
// Reflection-on-demand is a Most Wanted shortcut (a reflection resolved to a fixed address); off here.
void AnotarLectura() {}
void AnotarCopia() {}
void AnotarSwap() {}
bool MedirVisibilidad() { return false; }
bool VisibilidadComprobada() { return false; }
void AnotarVisible(bool) {}
void AnotarOculto() {}
void AnotarTestigo(bool) {}
}  // namespace nfsmw::reflejo_demanda

namespace nfsmw::guardia30 {
// The 30 FPS guard (drops shadows when the frame rate falls): not used.
void Latir(double) {}
void Informe() {}
bool SinSombras(bool del_usuario) { return del_usuario; }
}  // namespace nfsmw::guardia30

namespace nfsmw::escenario_lod {
std::string Resumen() { return std::string(); }
}  // namespace nfsmw::escenario_lod

namespace nfsmw::render_targets {
// Samples per pixel of the game's current anti-aliasing mode: 1 until the Carbon location is known.
uint32_t MuestrasOriginalesModoActual(const uint8_t*) { return 1; }
}  // namespace nfsmw::render_targets

// Frames counted by the game thread (Most Wanted hook); the ring thread compares it with its own count.
std::atomic<uint64_t> g_nfsmw_fotogramas_juego{0};

namespace nfsmw::nativo {
// Direct3D-level markers: need the Carbon D3D hooks (phase 2).
void ActivarConsumidorMarcadores(bool) {}
void AnotarComprobacionMarcador(bool, uint32_t, uint32_t, uint32_t, uint32_t) {}
// Ring progress: wakes the game's Direct3D ring wait (carbon/src/nfsc_d3d_wait.cpp), which sleeps in
// rex::WaitRingProgress. This was an empty stand-in, so on the native renderer every one of those sleeps ran to its
// 2 ms cap (log: "0 ended by GPU progress") and a run of them showed up as the 160-180 ms hitches in Free Roam.
void AvisarProgresoAnillo() { rex::NotifyRingProgress(); }
}  // namespace nfsmw::nativo
