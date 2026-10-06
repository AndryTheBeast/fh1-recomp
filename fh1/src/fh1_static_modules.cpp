// Nintendo Switch only: the game's two run-time modules, linked into the program.
//
// On the PC, XMediaFacade_default.xex and SpeechFacade_default.xex are recompiled into two DLLs next to fh1.exe
// and the SDK opens them when the game loads the module. An NRO cannot open a library, so the Switch build
// links their objects into the program, with the four names every module defines renamed after the module
// (sdk/resources/templates/codegen/dll_targets_cmake.inja), and this file gives their entry points to the
// SDK's loader (KernelState::RegisterStaticModuleLibrary). The names are those of
// generated/default/module_registry.cpp.

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/image_info.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

extern "C" void ReXModule_Register_XMediaFacade_default(rex::runtime::IModuleRegistrar* registrar);
extern "C" const rex::PPCImageInfo* ReXModule_GetImageInfo_XMediaFacade_default();
extern "C" void ReXModule_Register_SpeechFacade_default(rex::runtime::IModuleRegistrar* registrar);
extern "C" const rex::PPCImageInfo* ReXModule_GetImageInfo_SpeechFacade_default();

void Fh1RegisterStaticModules() {
  rex::system::KernelState::RegisterStaticModuleLibrary(
      "fh1_XMediaFacade_default", reinterpret_cast<void*>(&ReXModule_Register_XMediaFacade_default),
      reinterpret_cast<void*>(&ReXModule_GetImageInfo_XMediaFacade_default));
  rex::system::KernelState::RegisterStaticModuleLibrary(
      "fh1_SpeechFacade_default", reinterpret_cast<void*>(&ReXModule_Register_SpeechFacade_default),
      reinterpret_cast<void*>(&ReXModule_GetImageInfo_SpeechFacade_default));
}

#else

void Fh1RegisterStaticModules() {}

#endif
