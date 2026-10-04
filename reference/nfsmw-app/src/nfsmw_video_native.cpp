// nfsmw - cutscenes: the game's WMV3 decoding replaced by FFmpeg's
//
// On the Switch the XDK's recompiled WMV3 decoder runs at ~98 % of a core and produces ~22 frames per
// second: cutscenes drop from ~60 to ~20 FPS and the audio ends before the picture. The game's decoder
// interface (recompiled code):
//  - sub_8272FD30 prepares a movie's context; [ctx+3300] points to {application data, function}.
//  - sub_827312C0 (DecodeData, from sub_82734040) requests the compressed frame in chunks through
//    sub_82749C10, which jumps to that function with r4 = offset, r5 = &pointer to the data, r6 = bytes
//    requested, r7 = &bytes returned and r8 = &data remaining. It reads the picture header, swaps the
//    buffers and decodes according to the type ([ctx+280]): I with [ctx+15708] = sub_828C35D8 and P
//    with [ctx+15712] = sub_8278A518. Those two, with what they call, take almost all of the video
//    thread's time.
//  - The new picture goes into planes [ctx+3672] (Y), [ctx+3676] (U) and [ctx+3680] (V), with the
//    origin at +[ctx+216] and +[ctx+220] (32- and 16-pixel borders: strides of 1344 and 672 for
//    1280x720).
//  - Without postprocessing ([ctx+3844] = 0) both only end up writing context fields: I sets
//    [ctx+15516] = 0; P also sets [ctx+15488] = 1 and [ctx+15512] = ([ctx+14776] != 0 or
//    [ctx+15148] != -1).
//
// Cvars:
//  - nfsmw_video_wmv3_native: sub_828C35D8 and sub_8278A518 do not decode. FFmpeg decodes the same
//    bytes the game requested and its planes are copied into the game's buffers. If a frame cannot be
//    replaced the game decodes it, and the next native one waits for an I frame.
//  - nfsmw_video_wmv3_shadow (diagnostic): the game decodes and its planes are compared with FFmpeg's;
//    the luma of frame 30 of each movie is saved as PGM in the working folder.
//  - nfsmw_video_wmv3_data_diag (diagnostic): logs the calls to the data function, the arguments of
//    sub_8272FD30 and the context fields of each movie.
// FFmpeg needs the size and the 4 sequence bytes: they come from the ASF header of the last movie the
// game read (xboxkrnl_io.cpp in the SDK).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "nfsmw_video_native.h"
#include "nfsmw_video_wmv3.h"

namespace rex::kernel::xboxkrnl {
std::string NfsmwLastWmvRead();  // SDK xboxkrnl_io.cpp: last .wmv movie the game read
}  // namespace rex::kernel::xboxkrnl

// On by default: on the PC the four intro movies play every frame through FFmpeg with no rejections,
// and the shadow comparison gave bit-identical planes.
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_native, true, "NFSMW",
                    "Cinematicas: descodifica los frames WMV3 con FFmpeg en lugar del decoder "
                    "recompiled del game (mismos bytes y mismos buffers de image)");
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_shadow, false, "NFSMW",
                    "Diagnostic: el game descodifica y se comparan sus planes con los de FFmpeg para los "
                    "mismos bytes; guarda la luma del frame 30 de every movie en PGM");
REXCVAR_DEFINE_BOOL(nfsmw_video_wmv3_data_diag, false, "NFSMW",
                    "Diagnostic: anota the calls a la function de data del decoder WMV3 y los fields "
                    "del context_id de every movie");

REX_EXTERN(__imp__sub_82749C10);
REX_EXTERN(__imp__sub_827312C0);
REX_EXTERN(__imp__sub_8278A518);
REX_EXTERN(__imp__sub_828C35D8);
REX_EXTERN(__imp__sub_8272FD30);

namespace nfsmw::video_native {

namespace {
std::atomic<uint64_t> g_frames_native{0};
}  // namespace

uint64_t FramesNative() {
  return g_frames_native.load(std::memory_order_relaxed);
}

namespace {

using video_wmv3::DecoderWmv3;
using video_wmv3::Frame;
using video_wmv3::InfoWmv;

constexpr size_t kMaxFrame = 8 * 1024 * 1024;
constexpr uint32_t kDecodeI = 0x828C35D8;
constexpr uint32_t kDecodeP = 0x8278A518;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + address, &v, sizeof(v));
}

int64_t NowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Active() {
  return REXCVAR_GET(nfsmw_video_wmv3_native) || REXCVAR_GET(nfsmw_video_wmv3_shadow) ||
         REXCVAR_GET(nfsmw_video_wmv3_data_diag);
}

// --- Compressed frame the game requests ------------------------------------------------------------------

struct Capture {
  uint32_t ctx = 0;
  uint32_t structure = 0;  // r3 of sub_82749C10: {application data, function}
  uint32_t sections = 0;
  bool remain = false;      // the last section says frame bytes remain
  bool error = false;
  std::vector<uint8_t> data;
};
Capture g_capture;
std::mutex g_capture_m;
thread_local Capture* t_capture = nullptr;  // non-null inside sub_827312C0 on this thread
std::atomic<uint32_t> g_data_noted{0};

void NoteSection(const uint8_t* base, uint32_t lr, uint32_t structure, uint32_t displacement, uint32_t requested_2,
                   uint32_t p_data, uint32_t p_bytes, uint32_t p_remain, uint32_t result) {
  Capture& c = *t_capture;
  const uint32_t data = p_data ? Read32(base, p_data) : 0;
  const uint32_t bytes = p_bytes ? Read32(base, p_bytes) : 0;
  const uint32_t remain = p_remain ? Read32(base, p_remain) : 0;
  c.structure = structure;
  c.remain = remain != 0;
  ++c.sections;
  if (bytes) {
    if (data && uint64_t(data) + bytes <= 0x100000000ull && c.data.size() + bytes <= kMaxFrame) {
      c.data.insert(c.data.end(), base + data, base + data + bytes);
    } else {
      c.error = true;
    }
  }
  if (REXCVAR_GET(nfsmw_video_wmv3_data_diag) && g_data_noted.fetch_add(1, std::memory_order_relaxed) < 80) {
    REXLOG_INFO("[video] data: lr={:08X} ctx={:08X} structure={:08X} ({:08X} {:08X}) displacement={} "
                "requested_2={} -> result={:08X} data={:08X} bytes={} remain={} | section {}, {} bytes",
                lr, c.ctx, structure, Read32(base, structure), Read32(base, structure + 4), displacement,
                requested_2, result, data, bytes, remain, c.sections, c.data.size());
  }
}

// Asks the game's data function for the rest of the frame, like the game's bit reader (sub_82734DC8,
// object [ctx+76]): structure [reader+44], offset 0, 4 bytes requested and "remaining" in [reader+24].
//  - With a non-zero offset the application function (sub_827204A0) takes another path: the game
//    crashed.
//  - "remaining" must end up in [reader+24]: at the end of DecodeData, sub_8272E128 keeps requesting
//    chunks while it is 1. With "remaining" in another variable the next frame was swallowed
//    (eahd_bumper frame 81 of 120) and at the end of the movie it kept waiting for data.
void CompleteFrame(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  Capture& c = *t_capture;
  const uint32_t reader = Read32(base, obj + 76);
  const uint32_t structure = Read32(base, reader + 44);
  const uint64_t r1 = ctx.r1.u64;
  const uint64_t lr = ctx.lr;
  const uint32_t sp = ctx.r1.u32 - 128;
  Write32(base, sp, ctx.r1.u32);  // stack back chain
  for (int i = 0; Read32(base, reader + 24) != 0 && !c.error && i < 4096; ++i) {
    const size_t before = c.data.size();
    Write32(base, sp + 80, 0);
    Write32(base, sp + 88, 0);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = structure;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = sp + 88;
    ctx.r6.u64 = 4;
    ctx.r7.u64 = sp + 80;
    ctx.r8.u64 = reader + 24;
    __imp__sub_82749C10(ctx, base);
    NoteSection(base, 0, structure, 0, 4, sp + 88, sp + 80, reader + 24, ctx.r3.u32);
    if (c.data.size() == before) {
      break;
    }
  }
  ctx.r1.u64 = r1;
  ctx.lr = lr;
}

// --- The game's picture planes -------------------------------------------------------------------------

struct Planes {
  uint8_t* y = nullptr;
  uint8_t* u = nullptr;
  uint8_t* v = nullptr;
  int step_y = 0;
  int step_c = 0;
};

bool PlanesOfGame(uint8_t* base, uint32_t obj, int width, Planes& p) {
  const uint32_t off_y = Read32(base, obj + 216);
  const uint32_t off_c = Read32(base, obj + 220);
  if (off_y < 32 || off_c < 16 || (off_y - 32) % 32 != 0 || (off_c - 16) % 16 != 0) {
    return false;
  }
  p.step_y = int((off_y - 32) / 32);
  p.step_c = int((off_c - 16) / 16);
  if (p.step_y < width || p.step_c < (width + 1) / 2) {
    return false;
  }
  p.y = base + Read32(base, obj + 3672) + off_y;
  p.u = base + Read32(base, obj + 3676) + off_c;
  p.v = base + Read32(base, obj + 3680) + off_c;
  return true;
}

void CopyPlanes(const Frame& f, const Planes& g) {
  const int width_c = (f.width + 1) / 2;
  const int height_c = (f.height + 1) / 2;
  for (int y = 0; y < f.height; ++y) {
    std::memcpy(g.y + int64_t(y) * g.step_y, f.planes[0] + int64_t(y) * f.steps[0], size_t(f.width));
  }
  for (int y = 0; y < height_c; ++y) {
    std::memcpy(g.u + int64_t(y) * g.step_c, f.planes[1] + int64_t(y) * f.steps[1], size_t(width_c));
    std::memcpy(g.v + int64_t(y) * g.step_c, f.planes[2] + int64_t(y) * f.steps[2], size_t(width_c));
  }
}

struct Difference {
  int max = 0;
  double media = 0.0;
  uint64_t bad = 0;  // pixels differing by more than 3
};

Difference CompareFlat(const uint8_t* g, int step_g, const uint8_t* n, int step_n, int width, int height) {
  Difference d;
  uint64_t sum = 0;
  for (int y = 0; y < height; ++y) {
    const uint8_t* fg = g + int64_t(y) * step_g;
    const uint8_t* fn = n + int64_t(y) * step_n;
    for (int x = 0; x < width; ++x) {
      const int v = std::abs(int(fg[x]) - int(fn[x]));
      sum += uint64_t(v);
      d.max = std::max(d.max, v);
      d.bad += v > 3;
    }
  }
  d.media = double(sum) / double(std::max<int64_t>(int64_t(width) * height, 1));
  return d;
}

void SavePgm(const std::string& name, const uint8_t* flat, int step, int width, int height, int scale,
                const uint8_t* subtract = nullptr, int step_subtract = 0) {
  FILE* f = std::fopen(name.c_str(), "wb");
  if (!f) {
    return;
  }
  std::fprintf(f, "P5\n%d %d\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width));
  for (int y = 0; y < height; ++y) {
    const uint8_t* p = flat + int64_t(y) * step;
    if (subtract) {
      const uint8_t* r = subtract + int64_t(y) * step_subtract;
      for (int x = 0; x < width; ++x) {
        row[size_t(x)] = uint8_t(std::min(255, std::abs(int(p[x]) - int(r[x])) * scale));
      }
    } else {
      std::memcpy(row.data(), p, size_t(width));
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// --- Movie en progress ---------------------------------------------------------------------------------

struct Movie {
  uint32_t obj = 0;
  std::string path;
  DecoderWmv3 dec;
  bool prepared = false;      // FFmpeg open and the game's configuration known
  bool desynchronized = true;  // FFmpeg is waiting for an I frame
  uint64_t frames = 0;     // I and P frames that went through the hooks
  uint64_t native = 0;
  uint64_t rejected = 0;
  uint64_t warnings = 0;
  int worst_max_y = 0;
  double worst_media_y = 0.0;
  // Summary every 5 s
  int64_t since_us = 0;
  uint64_t n_game = 0;
  uint64_t n_native = 0;
  uint64_t n_shadow = 0;
  int64_t us_game = 0;
  int64_t us_native = 0;
};
std::mutex g_movie_m;
std::unique_ptr<Movie> g_movie;

void Summary(Movie& p, int64_t now) {
  if (p.since_us == 0) {
    p.since_us = now;
    return;
  }
  if (now - p.since_us < 5000000) {
    return;
  }
  const double s = double(now - p.since_us) / 1e6;
  REXLOG_INFO("[video] WMV3 '{}': {:.1f} frames/s | game {} a {:.2f} ms | FFmpeg {} a {:.2f} ms | shadow {}",
              p.path, double(p.n_game + p.n_native) / s, p.n_game,
              p.n_game ? double(p.us_game) / double(p.n_game) / 1000.0 : 0.0, p.n_native,
              p.n_native ? double(p.us_native) / double(p.n_native) / 1000.0 : 0.0, p.n_shadow);
  p.since_us = now;
  p.n_game = p.n_native = p.n_shadow = 0;
  p.us_game = p.us_native = 0;
}

void SummaryFinal(const Movie& p) {
  REXLOG_INFO("[video] WMV3 fin de '{}' (context_id {:08X}): {} frames, {} con FFmpeg, {} rejected; shadow: "
              "worst Y max {} media {:.3f}",
              p.path, p.obj, p.frames, p.native, p.rejected, p.worst_max_y, p.worst_media_y);
}

// Con g_movie_m tomado.
Movie& MovieOf(const uint8_t* base, uint32_t obj) {
  if (g_movie && g_movie->obj == obj) {
    return *g_movie;
  }
  if (g_movie) {
    SummaryFinal(*g_movie);
  }
  g_movie = std::make_unique<Movie>();
  Movie& p = *g_movie;
  p.obj = obj;
  p.path = rex::kernel::xboxkrnl::NfsmwLastWmvRead();
  InfoWmv info;
  const bool info_ok = !p.path.empty() && video_wmv3::ReadInfoWmv(p.path, info);
  const bool config_ok = Read32(base, obj + 3844) == 0 && Read32(base, obj + 15424) == 6 &&
                         Read32(base, obj + 15708) == kDecodeI && Read32(base, obj + 15712) == kDecodeP;
  p.prepared = info_ok && config_ok && p.dec.Open(info);
  const auto& s = info.sequence;
  REXLOG_INFO("[video] WMV3: context_id {:08X} -> '{}' {}x{} sequence {:02X}{:02X}{:02X}{:02X}; configuracion {}; "
              "FFmpeg {}",
              obj, p.path, info.width, info.height, s.size() > 0 ? s[0] : 0, s.size() > 1 ? s[1] : 0,
              s.size() > 2 ? s[2] : 0, s.size() > 3 ? s[3] : 0, config_ok ? "conocida" : "different",
              p.prepared ? "ready" : "no available");
  if (REXCVAR_GET(nfsmw_video_wmv3_data_diag)) {
    REXLOG_INFO("[video] context_id {:08X}: +3300={:08X} +15708={:08X} +15712={:08X} +15424={} +3844={} +132={} "
                "+136={} +200={} +204={} +216={:X} +220={:X}",
                obj, Read32(base, obj + 3300), Read32(base, obj + 15708), Read32(base, obj + 15712),
                Read32(base, obj + 15424), Read32(base, obj + 3844), Read32(base, obj + 132),
                Read32(base, obj + 136), Read32(base, obj + 200), Read32(base, obj + 204),
                Read32(base, obj + 216), Read32(base, obj + 220));
  }
  return p;
}

bool Reject(Movie& p, bool intra, const char* reason) {
  ++p.rejected;
  p.desynchronized = true;
  if (p.warnings++ < 10) {
    REXLOG_WARN("[video] WMV3 native: el frame {} ({}) de '{}' lo descodifica el game: {}", p.frames,
                intra ? "I" : "P", p.path, reason);
  }
  return false;
}

// Replaces the decoding of one frame. true if r3 already holds the result.
bool ReplaceFrame(PPCContext& ctx, uint8_t* base, Movie& p, Capture& c, bool intra) {
  const uint32_t obj = p.obj;
  if (p.desynchronized && !intra) {
    ++p.rejected;  // the game decodes until the next I frame
    return false;
  }
  if (Read32(base, obj + 3844) != 0 || Read32(base, obj + 15424) != 6) {
    p.prepared = false;
    return Reject(p, intra, "postprocess o profile different");
  }
  if (c.remain || Read32(base, Read32(base, obj + 76) + 24) != 0) {
    CompleteFrame(ctx, base, obj);
  }
  if (c.remain || c.error || c.data.empty()) {
    return Reject(p, intra, "frame comprimido incompleto");
  }
  const int64_t before = NowUs();
  Frame f;
  if (!p.dec.Decode(c.data.data(), c.data.size(), intra, f)) {
    return Reject(p, intra, "FFmpeg no lo descodifica");
  }
  Planes g;
  if (f.width != p.dec.width() || f.height != p.dec.height() || !PlanesOfGame(base, obj, f.width, g)) {
    p.prepared = false;
    return Reject(p, intra, "los planes del game no cuadran con el video");
  }
  CopyPlanes(f, g);
  // Context as sub_828C35D8 and sub_8278A518 leave it without postprocessing.
  if (!intra) {
    const bool mark = Read32(base, obj + 14776) != 0 || Read32(base, obj + 15148) != 0xFFFFFFFFu;
    Write32(base, obj + 15512, mark ? 1 : 0);
    Write32(base, obj + 15488, 1);
  }
  Write32(base, obj + 15516, 0);
  ctx.r3.u64 = 0;
  const int64_t after = NowUs();
  p.desynchronized = false;
  ++p.frames;
  ++p.native;
  ++p.n_native;
  g_frames_native.fetch_add(1, std::memory_order_relaxed);
  p.us_native += after - before;
  Summary(p, after);
  return true;
}

// After the game decodes: FFmpeg decodes the same bytes and the planes are compared.
void Shadow(uint8_t* base, Movie& p, const Capture& c, bool intra) {
  const uint64_t n = p.frames - 1;
  if (c.remain || c.error || c.data.empty()) {
    if (p.warnings++ < 10) {
      REXLOG_WARN("[video] shadow #{}: frame comprimido incompleto ({} bytes, remain {}, error {})", n,
                  c.data.size(), c.remain, c.error);
    }
    p.desynchronized = true;
    return;
  }
  if (p.desynchronized && !intra) {
    return;
  }
  const int64_t before = NowUs();
  Frame f;
  if (!p.dec.Decode(c.data.data(), c.data.size(), intra, f)) {
    p.desynchronized = true;
    return;
  }
  const int64_t after = NowUs();
  p.desynchronized = false;
  ++p.n_shadow;
  Planes g;
  if (!PlanesOfGame(base, p.obj, f.width, g)) {
    return;
  }
  const int width_c = (f.width + 1) / 2;
  const int height_c = (f.height + 1) / 2;
  const Difference dy = CompareFlat(g.y, g.step_y, f.planes[0], f.steps[0], f.width, f.height);
  const Difference du = CompareFlat(g.u, g.step_c, f.planes[1], f.steps[1], width_c, height_c);
  const Difference dv = CompareFlat(g.v, g.step_c, f.planes[2], f.steps[2], width_c, height_c);
  p.worst_max_y = std::max(p.worst_max_y, dy.max);
  p.worst_media_y = std::max(p.worst_media_y, dy.media);
  const bool odd = dy.max > 8 || du.max > 8 || dv.max > 8;
  if (n < 4 || n % 90 == 0 || (odd && p.warnings++ < 20)) {
    REXLOG_INFO("[video] shadow #{} {} {} bytes en {} sections: Y max {} media {:.3f} >3 {} | U max {} media "
                "{:.3f} | V max {} media {:.3f} | FFmpeg {:.2f} ms",
                n, intra ? "I" : "P", c.data.size(), c.sections, dy.max, dy.media, dy.bad, du.max, du.media,
                dv.max, dv.media, double(after - before) / 1000.0);
  }
  if (n == 30) {
    const std::string prefix = fmt::format("wmv3_{:08X}_030", p.obj);
    SavePgm(prefix + "_game.pgm", g.y, g.step_y, f.width, f.height, 1);
    SavePgm(prefix + "_native.pgm", f.planes[0], f.steps[0], f.width, f.height, 1);
    SavePgm(prefix + "_difference.pgm", g.y, g.step_y, f.width, f.height, 16, f.planes[0], f.steps[0]);
  }
}

void Decode(PPCContext& ctx, uint8_t* base, bool intra) {
  const bool native = REXCVAR_GET(nfsmw_video_wmv3_native);
  const bool shadow = REXCVAR_GET(nfsmw_video_wmv3_shadow);
  const bool diag = REXCVAR_GET(nfsmw_video_wmv3_data_diag);
  if (!native && !shadow && !diag) {
    if (intra) {
      __imp__sub_828C35D8(ctx, base);
    } else {
      __imp__sub_8278A518(ctx, base);
    }
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  std::lock_guard<std::mutex> lock(g_movie_m);
  Movie& p = MovieOf(base, obj);
  Capture* c = t_capture && t_capture->ctx == obj ? t_capture : nullptr;
  if (native && p.prepared) {
    if (c && ReplaceFrame(ctx, base, p, *c, intra)) {
      return;
    }
    if (!c) {
      Reject(p, intra, "call outside de DecodeData");
    }
  }
  const int64_t before = NowUs();
  if (intra) {
    __imp__sub_828C35D8(ctx, base);
  } else {
    __imp__sub_8278A518(ctx, base);
  }
  const int64_t after = NowUs();
  ++p.frames;
  ++p.n_game;
  p.us_game += after - before;
  if (diag && p.frames <= 12 && c) {
    REXLOG_INFO("[video] frame #{} {}: {} bytes en {} sections, remain {}, error {}, result {:08X}, "
                "game {:.2f} ms",
                p.frames - 1, intra ? "I" : "P", c->data.size(), c->sections, c->remain, c->error,
                ctx.r3.u32, double(after - before) / 1000.0);
  }
  if (shadow && !native && p.prepared && c && ctx.r3.u32 == 0) {
    Shadow(base, p, *c, intra);
  }
  Summary(p, after);
}

}  // namespace
}  // namespace nfsmw::video_native

// Decoder data function (jumps to [[r3+4]] with r3 = [r3]).
REX_HOOK_RAW(sub_82749C10) {
  using namespace nfsmw::video_native;
  if (!t_capture || ctx.r3.u32 != Read32(base, t_capture->ctx + 3300)) {
    __imp__sub_82749C10(ctx, base);
    return;
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t structure = ctx.r3.u32;
  const uint32_t displacement = ctx.r4.u32;
  const uint32_t p_data = ctx.r5.u32;
  const uint32_t requested_2 = ctx.r6.u32;
  const uint32_t p_bytes = ctx.r7.u32;
  const uint32_t p_remain = ctx.r8.u32;
  __imp__sub_82749C10(ctx, base);
  NoteSection(base, lr, structure, displacement, requested_2, p_data, p_bytes, p_remain, ctx.r3.u32);
}

// DecodeData: collects the chunks of the compressed frame it requests.
REX_HOOK_RAW(sub_827312C0) {
  using namespace nfsmw::video_native;
  if (!Active() || t_capture) {
    __imp__sub_827312C0(ctx, base);
    return;
  }
  std::unique_lock<std::mutex> lock(g_capture_m, std::try_to_lock);
  if (!lock.owns_lock()) {
    __imp__sub_827312C0(ctx, base);
    return;
  }
  g_capture.ctx = ctx.r3.u32;
  g_capture.structure = 0;
  g_capture.sections = 0;
  g_capture.remain = false;
  g_capture.error = false;
  g_capture.data.clear();
  t_capture = &g_capture;
  __imp__sub_827312C0(ctx, base);
  t_capture = nullptr;
}

// Decoding of an I frame ([ctx+15708]).
REX_HOOK_RAW(sub_828C35D8) {
  nfsmw::video_native::Decode(ctx, base, true);
}

// Decoding of a P frame ([ctx+15712]).
REX_HOOK_RAW(sub_8278A518) {
  nfsmw::video_native::Decode(ctx, base, false);
}

// Preparation of a movie's context: if the context is reused, FFmpeg starts from scratch.
REX_HOOK_RAW(sub_8272FD30) {
  using namespace nfsmw::video_native;
  if (Active()) {
    std::lock_guard<std::mutex> lock(g_movie_m);
    if (g_movie && g_movie->obj == ctx.r3.u32) {
      SummaryFinal(*g_movie);
      g_movie.reset();
    }
    if (REXCVAR_GET(nfsmw_video_wmv3_data_diag)) {
      REXLOG_INFO("[video] sub_8272FD30: ctx={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} "
                  "r10={:08X} f1={} f2={} lr={:08X}",
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                  ctx.f1.f64, ctx.f2.f64, static_cast<uint32_t>(ctx.lr));
    }
  }
  __imp__sub_8272FD30(ctx, base);
}
