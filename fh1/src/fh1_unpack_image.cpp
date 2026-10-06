// fh1 - "unpack only" mode for the installer: --fh1_unpack_image=<file>.
//
// About 480 of the game's shaders (post-processing, UI...) are inside default.xex, which is encrypted and
// compressed on the disc. The installer needs them to build the shader library on the user's PC, and this
// program already knows how to load default.xex: with this option it writes the loaded image
// (0x82000000-0x83620000) to the file as soon as the SDK has loaded it, before any of the game's code runs, and
// ends. The image is the user's own game data: the installer reads the shaders out of it and deletes it.

#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#endif

REXCVAR_DEFINE_STRING(fh1_unpack_image, "", "FH1",
                      "Installer: write default.xex's loaded image to this file and exit without starting the game");

// `image` = host address of guest 0x82000000. Does not return when the option is set.
void Fh1UnpackImageIfAsked(const uint8_t* image) {
  const std::string path = REXCVAR_GET(fh1_unpack_image);
  if (path.empty() || !image) return;
  constexpr uint32_t kImageBegin = 0x82000000, kImageEnd = 0x83620000;
  const size_t bytes = kImageEnd - kImageBegin;
  // Under another name until it is complete: the installer never reads half an image.
  const std::string part = path + ".part";
  bool ok = false;
  if (FILE* f = std::fopen(part.c_str(), "wb")) {
    ok = std::fwrite(image, 1, bytes, f) == bytes;
    ok = std::fclose(f) == 0 && ok;
  }
  if (ok) {
    std::remove(path.c_str());
    ok = std::rename(part.c_str(), path.c_str()) == 0;
  }
  REXLOG_INFO("[unpack] image {:08X}-{:08X} {} {}", kImageBegin, kImageEnd, ok ? "written to" : "NOT written to", path);
  // The window and the SDK's threads exist already: end the process here, without their shutdown.
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), ok ? 0 : 3);
#else
  std::_Exit(ok ? 0 : 3);
#endif
}
