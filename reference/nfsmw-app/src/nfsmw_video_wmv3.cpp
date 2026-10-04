// nfsmw - WMV movies decoded natively with FFmpeg (see nfsmw_video_wmv3.h)
//
// ASF container (the minimum for these files)
//  - Header: child objects with GUID and size. File properties (packet size and packet count), the
//    properties of each stream (the video one carries a BITMAPINFOHEADER with the width, the height
//    and the 4 WMV3 sequence bytes after it) and the data object, whose packets start 50 bytes later.
//  - Fixed-size packets with one or more payloads. Each payload is a piece of a media object (a
//    frame) with its number, its offset within the object and, in the replicated data, the total size
//    and the time. A frame is complete once all its bytes have been received.
//  - 1-byte replicated data = compressed payloads: several small objects in a row, each with its size
//    in one byte.
// Without B frames, each compressed frame yields one output frame in the same order.

#include "nfsmw_video_wmv3.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <span>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

extern "C" {
#include "libavcodec/avcodec.h"
}

namespace nfsmw::video_wmv3 {
namespace {

constexpr uint8_t kGuidHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                       0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidFile[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                      0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidFlow[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                    0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidVideo[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                    0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint8_t kGuidData[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr size_t kMaxHeader = 512 * 1024;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

// ASF variable-length field: 0, 1, 2 or 4 bytes depending on the type (0-3).
bool ReadVariable(const std::vector<uint8_t>& b, size_t& p, int type, uint32_t& input_value) {
  static constexpr int kBytes[4] = {0, 1, 2, 4};
  const int n = kBytes[type & 3];
  if (p + size_t(n) > b.size()) {
    return false;
  }
  input_value = n == 0 ? 0 : n == 1 ? b[p] : n == 2 ? Le16(&b[p]) : Le32(&b[p]);
  p += size_t(n);
  return true;
}

struct Object {
  std::vector<uint8_t> data;
  uint32_t received = 0;
  bool key = false;
};

// File opened through the SDK's VFS (folder or ISO).
struct FileVfs {
  rex::filesystem::File* f = nullptr;
  uint64_t size = 0;

  ~FileVfs() {
    if (f) {
      f->Destroy();
    }
  }

  bool Open(const std::string& path) {
    rex::filesystem::FileAction action;
    const auto state = REX_KERNEL_FS()->OpenFile(nullptr, path, rex::filesystem::FileDisposition::kOpen,
                                                  rex::filesystem::FileAccess::kGenericRead, false, true, &f,
                                                  &action);
    if (state != 0 || !f) {
      REXLOG_WARN("[video] WMV3 native: no se can open '{}' ({:08X})", path, uint32_t(state));
      f = nullptr;
      return false;
    }
    size = f->entry()->size();
    return true;
  }

  bool Read(uint64_t displacement, uint8_t* target, size_t bytes) {
    size_t read = 0;
    const auto state = f->ReadSync(std::span<uint8_t>(target, bytes), size_t(displacement), &read);
    return state == 0 && read == bytes;
  }
};

struct Container {
  InfoWmv info;
  uint64_t start_data = 0;
  uint64_t packets = 0;
  uint32_t size_packet = 0;
  uint32_t flow_video = 0;
};

bool ReadHeader(FileVfs& file, const std::string& path, Container& c) {
  std::vector<uint8_t> header(size_t(std::min<uint64_t>(file.size, kMaxHeader)));
  if (header.size() < 128 || !file.Read(0, header.data(), header.size()) ||
      std::memcmp(header.data(), kGuidHeader, 16) != 0) {
    REXLOG_WARN("[video] WMV3 native: '{}' no begins por one header ASF", path);
    return false;
  }
  const uint64_t tam_header = Le64(&header[16]);
  size_t p = 30;
  bool video = false;
  while (p + 24 <= header.size() && p < tam_header) {
    const uint8_t* o = &header[p];
    const uint64_t tam = Le64(o + 16);
    if (tam < 24 || p + tam > header.size()) {
      break;
    }
    if (std::memcmp(o, kGuidFile, 16) == 0 && tam >= 104) {
      c.packets = Le64(o + 56);
      c.size_packet = Le32(o + 92);
    } else if (std::memcmp(o, kGuidFlow, 16) == 0 && tam >= 78 + 11 + 40 && !video &&
               std::memcmp(o + 24, kGuidVideo, 16) == 0) {
      c.flow_video = Le16(o + 72) & 0x7F;
      const uint8_t* bmi = o + 78 + 11;
      const uint32_t tam_bmi = Le32(bmi);
      c.info.width = int(Le32(bmi + 4));
      c.info.height = std::abs(int(Le32(bmi + 8)));
      if (tam_bmi > 40 && 78 + 11 + uint64_t(tam_bmi) <= tam) {
        c.info.sequence.assign(bmi + 40, bmi + tam_bmi);
      }
      video = std::memcmp(bmi + 16, "WMV3", 4) == 0;
    }
    p += size_t(tam);
  }
  if (!video || !c.size_packet || !c.info.width || !c.info.height) {
    REXLOG_WARN("[video] WMV3 native: '{}' sin flow WMV3 utilizable (packet {}, {}x{})", path, c.size_packet,
                c.info.width, c.info.height);
    return false;
  }
  // The data object comes right after the header; its packets start 50 bytes later.
  std::vector<uint8_t> data(50);
  if (!file.Read(tam_header, data.data(), data.size()) || std::memcmp(data.data(), kGuidData, 16) != 0) {
    REXLOG_WARN("[video] WMV3 native: '{}' sin object de data behind de la header", path);
    return false;
  }
  c.start_data = tam_header + 50;
  return true;
}

}  // namespace

bool ReadInfoWmv(const std::string& path, InfoWmv& info) {
  FileVfs file;
  Container c;
  if (!file.Open(path) || !ReadHeader(file, path, c)) {
    return false;
  }
  info = std::move(c.info);
  return true;
}

// --- DecoderWmv3 ------------------------------------------------------------------------------------

struct DecoderWmv3::State {
  AVCodecContext* codec = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;

  ~State() {
    if (frame) {
      av_frame_free(&frame);
    }
    if (pkt) {
      av_packet_free(&pkt);
    }
    if (codec) {
      avcodec_free_context(&codec);
    }
  }
};

DecoderWmv3::DecoderWmv3() : e_(std::make_unique<State>()) {}
DecoderWmv3::~DecoderWmv3() = default;

bool DecoderWmv3::Open(const InfoWmv& info) {
  const AVCodec* wmv3 = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  if (!wmv3) {
    REXLOG_ERROR("[video] WMV3 native: FFmpeg sin decoder WMV3");
    return false;
  }
  e_ = std::make_unique<State>();
  e_->codec = avcodec_alloc_context3(wmv3);
  e_->codec->width = e_->codec->coded_width = info.width;
  e_->codec->height = e_->codec->coded_height = info.height;
  e_->codec->thread_count = 1;
  if (!info.sequence.empty()) {
    e_->codec->extradata =
        static_cast<uint8_t*>(av_mallocz(info.sequence.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(e_->codec->extradata, info.sequence.data(), info.sequence.size());
    e_->codec->extradata_size = int(info.sequence.size());
  }
  if (avcodec_open2(e_->codec, wmv3, nullptr) < 0) {
    REXLOG_ERROR("[video] WMV3 native: avcodec_open2 miss ({}x{})", info.width, info.height);
    e_ = std::make_unique<State>();
    return false;
  }
  e_->pkt = av_packet_alloc();
  e_->frame = av_frame_alloc();
  width_ = info.width;
  height_ = info.height;
  frames_ = 0;
  return true;
}

bool DecoderWmv3::Decode(const uint8_t* data, size_t bytes, bool key, Frame& output) {
  if (!e_->codec || !bytes) {
    return false;
  }
  av_packet_unref(e_->pkt);
  if (av_new_packet(e_->pkt, int(bytes)) < 0) {
    return false;
  }
  std::memcpy(e_->pkt->data, data, bytes);
  e_->pkt->flags = key ? AV_PKT_FLAG_KEY : 0;
  int r = avcodec_send_packet(e_->codec, e_->pkt);
  if (r < 0) {
    REXLOG_WARN("[video] WMV3 native: avcodec_send_packet {} en el frame {} ({} bytes)", r, frames_, bytes);
    return false;
  }
  r = avcodec_receive_frame(e_->codec, e_->frame);
  if (r < 0) {
    REXLOG_WARN("[video] WMV3 native: avcodec_receive_frame {} en el frame {} ({} bytes)", r, frames_,
                bytes);
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    output.planes[i] = e_->frame->data[i];
    output.steps[i] = e_->frame->linesize[i];
  }
  output.width = e_->frame->width;
  output.height = e_->frame->height;
  output.key = key;
  output.bytes = uint32_t(bytes);
  ++frames_;
  return true;
}

// --- MovieWmv (diagnostic) ------------------------------------------------------------------------------

struct MovieWmv::State {
  FileVfs file;
  Container c;
  uint64_t next_packet = 0;
  std::vector<uint8_t> packet;
  std::map<uint32_t, Object> in_progress;
  std::deque<Object> ready;
  DecoderWmv3 decoder;

  // Adds a payload to its media object; once complete, the object moves to the ready queue.
  void Load(uint32_t object, uint32_t displacement, uint32_t size_object, bool key, const uint8_t* data,
             uint32_t bytes) {
    auto& o = in_progress[object];
    if (o.data.empty()) {
      o.data.assign(size_object, 0);
      o.key = key;
    }
    if (uint64_t(displacement) + bytes > o.data.size()) {
      in_progress.erase(object);  // inconsistent piece: the object is dropped
      return;
    }
    std::memcpy(o.data.data() + displacement, data, bytes);
    o.received += bytes;
    if (o.received >= o.data.size()) {
      ready.push_back(std::move(o));
      in_progress.erase(object);
    }
  }

  bool ReadPacket() {
    if (next_packet >= c.packets) {
      return false;
    }
    packet.resize(c.size_packet);
    if (!file.Read(c.start_data + next_packet * uint64_t(c.size_packet), packet.data(),
                      c.size_packet)) {
      return false;
    }
    ++next_packet;
    const auto& b = packet;
    size_t p = 0;
    const uint8_t ec = b[p];
    if (ec & 0x80) {
      p += 1 + (ec & 0x0F);
    }
    if (p + 2 > b.size()) {
      return false;
    }
    const uint8_t types = b[p];
    const uint8_t properties = b[p + 1];
    p += 2;
    uint32_t length_packet = 0, sequence = 0, fill = 0;
    if (!ReadVariable(b, p, (types >> 5) & 3, length_packet) || !ReadVariable(b, p, (types >> 1) & 3, sequence) ||
        !ReadVariable(b, p, (types >> 3) & 3, fill)) {
      return false;
    }
    p += 6;  // send time and duration
    const bool multiples = types & 1;
    int n_loads = 1;
    int type_length_load = 0;
    if (multiples) {
      if (p >= b.size()) {
        return false;
      }
      n_loads = b[p] & 0x3F;
      type_length_load = (b[p] >> 6) & 3;
      ++p;
    }
    const size_t fin = std::min<size_t>(b.size(), (length_packet ? length_packet : c.size_packet)) -
                       std::min<size_t>(fill, b.size());
    for (int i = 0; i < n_loads && p < fin; ++i) {
      const uint8_t number = b[p++];
      const uint32_t flow = number & 0x7F;
      const bool key = number & 0x80;
      uint32_t object = 0, displacement = 0, replicated = 0;
      if (!ReadVariable(b, p, (properties >> 4) & 3, object) ||
          !ReadVariable(b, p, (properties >> 2) & 3, displacement) ||
          !ReadVariable(b, p, properties & 3, replicated) || p + replicated > b.size()) {
        return false;
      }
      const size_t pos_replicated = p;
      p += replicated;
      uint32_t bytes = 0;
      if (multiples) {
        if (!ReadVariable(b, p, type_length_load, bytes)) {
          return false;
        }
      } else {
        bytes = uint32_t(fin > p ? fin - p : 0);
      }
      if (p + bytes > b.size()) {
        return false;
      }
      if (flow == c.flow_video) {
        if (replicated >= 8) {
          Load(object, displacement, Le32(&b[pos_replicated]), key, &b[p], bytes);
        } else if (replicated == 1) {
          // Compressed payloads: whole objects in a row, each preceded by its size in one byte.
          size_t q = p;
          uint32_t sub = object;
          while (q < p + bytes) {
            const uint32_t n = b[q++];
            if (q + n > p + bytes) {
              break;
            }
            Load(sub++, 0, n, key, &b[q], n);
            q += n;
          }
        }
      }
      p += bytes;
    }
    return true;
  }
};

MovieWmv::MovieWmv() : e_(std::make_unique<State>()) {}
MovieWmv::~MovieWmv() = default;

bool MovieWmv::Open(const std::string& path) {
  path_ = path;
  if (!e_->file.Open(path) || !ReadHeader(e_->file, path, e_->c)) {
    return false;
  }
  info_ = e_->c.info;
  if (!e_->decoder.Open(info_)) {
    return false;
  }
  const auto& s = info_.sequence;
  REXLOG_INFO("[video] WMV3 native: '{}' {}x{}, {} packets de {} bytes, flow {}, sequence {:02X}{:02X}{:02X}{:02X}",
              path, info_.width, info_.height, e_->c.packets, e_->c.size_packet, e_->c.flow_video,
              s.size() > 0 ? s[0] : 0, s.size() > 1 ? s[1] : 0, s.size() > 2 ? s[2] : 0, s.size() > 3 ? s[3] : 0);
  return true;
}

bool MovieWmv::Next(Frame& output) {
  while (e_->ready.empty()) {
    if (!e_->ReadPacket()) {
      return false;
    }
  }
  Object o = std::move(e_->ready.front());
  e_->ready.pop_front();
  if (!e_->decoder.Decode(o.data.data(), o.data.size(), o.key, output)) {
    return false;
  }
  ++frames_;
  return true;
}

}  // namespace nfsmw::video_wmv3
