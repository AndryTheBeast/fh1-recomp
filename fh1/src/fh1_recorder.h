// fh1 - records what the player does (see fh1_recorder.cpp).
#pragma once

#include <string>

namespace fh1 {

// Starts writing controller changes (and car positions, when known) to path.
void StartRecorder(const std::string& path);
// Writes a "mark TEXT" line (autoplay 'log' lines also go here while recording).
void RecordMarker(const std::string& text);

}  // namespace fh1
