/**
 * @file        watchdog.h
 * @brief       Freeze watchdog: notices that the game stopped presenting frames, writes what everything
 *              was waiting for, and closes the game instead of leaving a black or frozen screen.
 *
 * Why (2026-10-05, console): a cutscene thread could not be created, the game waited for it forever, and the
 * picture stayed frozen until the owner closed the game by hand. The log said nothing about what the game was
 * waiting for. This watchdog:
 *
 *  1. Counts presented frames (NoteProgress, called by the renderers). If no frame is presented for
 *     `watchdog_seconds` (`watchdog_boot_seconds` before the first one) while the app is in focus, it fires.
 *     Pausing in the HOME menu or sleeping the console does not count (the app is not in focus then).
 *  2. Writes the state to `watchdog.log` next to the other logs, and a short summary to the normal log:
 *     every guest thread that is inside a kernel wait (which object, how long, which game address called it),
 *     every host thread (Switch: where it is), memory, and whatever the sections registered with
 *     RegisterSection print (the GPU ring, fences, ...).
 *  2b. Memory guard: every freeze seen on the console (2026-10-05/06) came with the heap at 2.4-2.5 GB of the ~3.2 GB the
 *     process may use, and the console itself hung, which no watchdog can survive. If `watchdog_heap_mb` is set and the heap
 *     reaches it, the same dump is written and the game closes before the console hangs.
 *  3. Asks the app to close the normal way (so the save and the logs are flushed). If that has not ended the
 *     process after `watchdog_grace_seconds`, it ends it by force.
 *
 * `watchdog_selftest_after_s` makes the watchdog believe presents stopped after that many seconds, to check
 * on a machine that the whole chain (dump, close, forced exit) works.
 */

#pragma once

#include <cstdint>
#include <cstdio>

namespace rex::watchdog {

constexpr int kMaxWaitObjects = 4;

struct WaitObject {
  const char* type = "";      // XObject type name, a string literal
  uint32_t handle = 0;        // guest handle
  uint32_t guest_object = 0;  // guest address of the kernel object, 0 if none
  char name[24] = {};
};

/// One kernel wait of a guest thread (filled by XObject::Wait / WaitMultiple / SignalAndWait).
struct WaitInfo {
  const char* api = "";  // string literal
  bool wait_all = false;
  int64_t timeout_ms = -1;  // -1 = no limit
  uint32_t count = 0;       // objects waited on (only the first kMaxWaitObjects are described)
  WaitObject objects[kMaxWaitObjects];
  uint32_t guest_lr = 0;  // the guest return address of the call (who waits)
  uint32_t thread_id = 0;
  char thread_name[32] = {};
};

/// The calling thread starts / ends a kernel wait. Cheap; never blocks.
void BeginWait(const WaitInfo& info);
void EndWait();

/// A frame was presented (or the app otherwise made visible progress). Cheap.
void NoteProgress();

/// A named block of state printed in the dump. `fn` must only read atomics or data that is safe to read from
/// another thread, and must not take locks.
void RegisterSection(const char* name, void (*fn)(std::FILE*));

/// What the platform can do for the watchdog.
struct Hooks {
  /// Prints host threads (names, what they are doing). Must not take locks a stuck thread may hold.
  void (*dump_threads)(std::FILE*) = nullptr;
  /// Prints memory state.
  void (*dump_memory)(std::FILE*) = nullptr;
  /// Megabytes the program has taken from the system heap (what runs out before the console hangs). Null = no memory guard.
  uint32_t (*heap_mb)() = nullptr;
  /// True while the app is shown and running (not the HOME menu, not asleep). Null = always.
  bool (*in_focus)() = nullptr;
  /// Asks the app to close through its normal path. Must return quickly.
  void (*request_quit)() = nullptr;
  /// Ends the process. Does not return.
  void (*force_exit)(int code) = nullptr;
  /// Folder for watchdog.log, ending in a separator. Null = current folder.
  const char* (*log_dir)() = nullptr;
};
void SetHooks(const Hooks& hooks);

/// Starts the detector thread (once; also started by the first BeginWait/NoteProgress).
void Start();

/// Writes the full state to `f` (also used by the fire path).
void DumpState(std::FILE* f, const char* reason);

}  // namespace rex::watchdog
