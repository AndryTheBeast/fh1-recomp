// fh1 - ReXGlue Recompiled Project

#include "generated/default/fh1_init.h"

#include "fh1_app.h"

REX_DEFINE_APP(fh1, Fh1App::Create)

#ifdef _WIN32
// Ask NVIDIA Optimus / AMD switchable-graphics drivers for the discrete GPU. The drivers only
// read these exports from the .exe; the SDK's copies live in rexruntime.dll and are ignored,
// so on a laptop with two GPUs the game ran on the integrated one.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
