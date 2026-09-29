// fh1 - crash report (Windows): call stack with function names on a fatal exception.
#pragma once

namespace fh1 {
// Call once logging is up. Writes "<log_file>.crash.txt" if the game crashes.
void InstallCrashReport();
}  // namespace fh1
