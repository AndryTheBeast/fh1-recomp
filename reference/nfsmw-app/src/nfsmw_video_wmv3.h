// nfsmw - WMV movies decoded natively with FFmpeg
//
// The game's videos (Movies/*.wmv) are WMV3 main profile, 1280x720 at 30 fps and without B frames. On
// the Switch the XDK's recompiled decoder runs at ~98 % of a core and produces ~22 frames per second:
// the picture falls behind and the audio ends first.
//  - DecoderWmv3: decodes with FFmpeg's WMV3 the compressed frames as stored in the container.
//    nfsmw_video_native.cpp passes it the same bytes the game hands to its own decoder.
//  - ReadInfoWmv: reads the size and the 4 WMV3 sequence bytes from the movie's ASF header.
//  - MovieWmv: full ASF reader (diagnostic: decodes the file on its own).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nfsmw::video_wmv3 {

// YUV 4:2:0 planes of the last decoded frame; valid until the next call.
struct Frame {
  const uint8_t* planes[3] = {nullptr, nullptr, nullptr};
  int steps[3] = {0, 0, 0};
  int width = 0;
  int height = 0;
  bool key = false;
  uint32_t bytes = 0;  // compressed frame size
};

struct InfoWmv {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> sequence;  // extra data of the WMV3 stream (STRUCT_C, 4 bytes)
};

// Reads the movie's ASF header using the guest path (the same one NtCreateFile uses).
bool ReadInfoWmv(const std::string& path, InfoWmv& info);

class DecoderWmv3 {
 public:
  DecoderWmv3();
  ~DecoderWmv3();
  DecoderWmv3(const DecoderWmv3&) = delete;
  DecoderWmv3& operator=(const DecoderWmv3&) = delete;

  bool Open(const InfoWmv& info);
  // Decodes one complete compressed frame. false if FFmpeg fails or returns no picture.
  bool Decode(const uint8_t* data, size_t bytes, bool key, Frame& output);

  int width() const { return width_; }
  int height() const { return height_; }
  int64_t frames() const { return frames_; }

 private:
  struct State;
  std::unique_ptr<State> e_;
  int width_ = 0;
  int height_ = 0;
  int64_t frames_ = 0;
};

class MovieWmv {
 public:
  MovieWmv();
  ~MovieWmv();
  MovieWmv(const MovieWmv&) = delete;
  MovieWmv& operator=(const MovieWmv&) = delete;

  // Opens the movie with the guest path and prepares the decoder.
  bool Open(const std::string& path);
  // Reads the next video frame from the container and decodes it. false at the end or on failure.
  bool Next(Frame& output);

  const std::string& path() const { return path_; }
  int width() const { return info_.width; }
  int height() const { return info_.height; }
  int64_t frames() const { return frames_; }

 private:
  struct State;
  std::unique_ptr<State> e_;
  std::string path_;
  InfoWmv info_;
  int64_t frames_ = 0;
};

}  // namespace nfsmw::video_wmv3
