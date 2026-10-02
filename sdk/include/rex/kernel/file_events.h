/**
 * @file        kernel/file_events.h
 * @brief       Observer of the files the guest opens (NtCreateFile/NtOpenFile successes).
 *
 * Lets the app follow what the game is loading (tracks, menus, videos) - e.g. FH1's test
 * autoplay waits for a file to be opened instead of a fixed time. Called on the opening guest
 * thread; keep the observer cheap.
 */
#pragma once

#include <functional>
#include <string_view>

namespace rex::kernel {

using FileOpenObserver = std::function<void(std::string_view guest_path)>;

// Replaces the observer (nullptr removes it).
void SetFileOpenObserver(FileOpenObserver observer);

// Called by the kernel after a successful open.
void NotifyFileOpened(std::string_view guest_path);

}  // namespace rex::kernel
