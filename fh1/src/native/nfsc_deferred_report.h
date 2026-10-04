#pragma once
/*
 * Periodic log lines, off the working threads.
 *
 * The log is synchronous (log_async = false): every few lines the FILE is flushed to the SD card and the
 * writing thread stalls for several ms. Every 10 s the game thread used to write 16 lines, the audio thread 3
 * and the ring thread 7 (the C2 lines of nfsc_native_targets.cpp) on their own. These lines are formatted
 * on the thread that produces them and written by the "NFSC reports" thread (ReportDeferred,
 * nfsc_native_system.cpp). Warnings and errors are still written immediately, from their own thread.
 */
#include <string>

#include <rex/logging.h>

namespace nfsc::native {
void ReportDeferred(std::string line);
}  // namespace nfsc::native

#define NFSC_REPORT_DEFERRED(...) ::nfsc::native::ReportDeferred(fmt::format(__VA_ARGS__))
