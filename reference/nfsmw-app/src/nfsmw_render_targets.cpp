// nfsmw - scene render targets without tiling or MSAA
//
// ===========================================================================
//  What the original game does
//
//  To get 4x MSAA at 720p with the 360's 10 MB of EDRAM, NFSMW does not draw
//  the scene into a 1280x720 surface: it uses the XDK Direct3D "tiling".
//  BeginTiling records the scene commands and EndTiling replays them once
//  per tile, on 1280x256 surfaces with 4 samples, resolving each tile into
//  the 1280x720 texture. Three tiles = the scene is drawn three times every
//  frame.
//
//  Console D3D trace: Count=3 and 1280x256 surfaces with
//  D3DMULTISAMPLE_4_SAMPLES (r6=2) in the menu. A/B test: menu +30 % FPS; in
//  a race the scene was not replayed per tile (no change).
//
//  Why it is so expensive here
//  On the 360, replaying the scene per tile is almost free. Under Xenos
//  emulation each replay goes through the command processor again (~80 us
//  of CPU per draw) and through the GPU, with 4 samples per pixel, three
//  4-sample resolve dumps per tile and transfers between the tiles' 4x
//  render targets and the 1x ones of the rest of the frame. Those are
//  exactly the three costs measured with the A/B bench on the console:
//  draws -95 ms, transfers -50 ms, resolves -50 ms per frame.
//
//  How the game chooses (recompiled code)
//    sub_82458310  renderer constructor: table of 6 AA modes at
//                  object+4 (tiles), +28 (width), +52 (height), +76 (MSAA)
//                  and +100+64*mode (tile rectangles).
//                    mode 2: 1 tile  1280x736  1x   <- no antialiasing
//                    mode 3: 2 tiles  640x736  2x
//                    mode 4: 3 tiles 1280x256  4x   <- the one used at 720p
//                    mode 5: 4 tiles  320x736  4x
//    sub_824402F0  XGetVideoMode: in HD with width >= 1280 it sets mode 4.
//    sub_82441990  switches live between modes 2, 3 and 4. So the retail
//                  game already renders in mode 2 when it lowers quality:
//                  it is not an invented state.
//    sub_82458850  registers one set of render targets per mode.
//    sub_8245D320  copies the descriptor (128 bytes) to the table 0x82A4527C.
//    sub_8245D5F8  binds a set; if byte +41 is 1 -> BeginTiling with Count
//                  at +124 and the rectangles at +44.
//
//  What we change
//  1. Before the sets are registered, modes 3, 4 and 5 get the
//     configuration of mode 2 (1 tile of 1280x736 without MSAA). The game's
//     mode selector is untouched: whichever it picks, the scene is drawn
//     once. Everything else that depends on the mode stays as in retail.
//  2. Any set registered with MSAA is changed to 1 sample. This covers the
//     256x256 reflection with 4x (sub_8243C1C8) and the SD modes.
//
//  MarathonRecomp-NX does the same in its renderer: SurfaceSize returns 0
//  (no tiling) and on Switch it leaves MSAA off. Antialiasing is lost.
//
//  How to disable it without a new build
//  In nfsmw.toml, next to the .nro:   nfsmw_render_without_tile = false
// ===========================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/platform.h>
#if REX_PLATFORM_SWITCH
// The handheld/docked mode to obey, Reverse-NX included.
#include <switch.h>

#include <rex/ui/switch_saltynx.h>
#endif
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(nfsmw_render_without_tile, REX_PLATFORM_SWITCH != 0, "NFSMW",
                    "Draw la scene one sola time, sin tiling de 3 strips ni MSAA "
                    "(evita repeat la scene por tira low la emulacion de Xenos)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// With the mode 4 table made equal to mode 2's, the game stayed in mode 4 with a one-tile scene. With
// this it really uses its mode 2, the game's own mode without antialiasing (sub_82441990 picks it when
// lowering quality). It was done on the theory that this mismatch caused the blue edges against the sky,
// but no: they look the same in both modes and in the emulated renderer; they come from the game's bright
// pass (see nfsmw_sky_glow).
REXCVAR_DEFINE_BOOL(nfsmw_render_mode_without_aa, true, "NFSMW",
                    "Con nfsmw_render_without_tile: el game usa de truth su mode 2 (one tira sin antialiasing) en time "
                    "del 3, 4 o 5 con la table del 2. No cambia los bordes azules del sky (eso es "
                    "nfsmw_sky_glow). false: as before de la build 148")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Testing only: split the two parts of the hook to see which one causes the blue edges.
REXCVAR_DEFINE_BOOL(nfsmw_render_test_keep_strips, false, "NFSMW",
                    "Solo tests: con nfsmw_render_without_tile, no match los modes 3-5 al 2 (se remain sus strips)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(nfsmw_render_test_keep_msaa, false, "NFSMW",
                    "Solo tests: con nfsmw_render_without_tile, no remove el MSAA ni de los modes ni de los sets")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The internal resolution comes from nfsmw_internal_resolution (Graphics category), and with
// "automatico" it follows the dock live: 1920x1080 docked and 1280x720 handheld.
//
// How it works without a restart: the game already switches AA mode on the fly (sub_82441990 jumps
// between 2, 3 and 4 when lowering quality) and each mode has its own set of render targets, registered
// at startup. So mode 2 keeps its usual 1280x736 and mode 4 gets a 1920x1088 with a visible area of
// 1920x1080; then it only takes writing 2 or 4 into the mode index the game reads. It is the same path
// retail uses.
//
// "Docked" is the effective mode: the real dock or the one Reverse-NX fakes. With Reverse-NX the clocks
// do not go up, so 1080p there costs a third of the FPS; that is a deliberate choice.
REXCVAR_DECLARE(std::string, nfsmw_internal_resolution);

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfResolution(unsigned width, unsigned height);
#endif

namespace nfsmw::render_targets {
namespace {

// Mode table inside the renderer object (see the header comment).
constexpr uint32_t kModes = 6;
constexpr uint32_t kOffStrips = 4;
constexpr uint32_t kOffWidth = 28;
constexpr uint32_t kOffHeight = 52;
constexpr uint32_t kOffMsaa = 76;
constexpr uint32_t kOffRects = 100;
constexpr uint32_t kBytesByModeRects = 64;  // 4 D3DRECT de 16 bytes
constexpr uint32_t kModeWithoutAa = 2;
constexpr uint32_t kMode1080p = 4;  // its set becomes the 1920x1088 one
constexpr uint32_t kWidth1080p = 1920;
constexpr uint32_t kHeight1080p = 1088;   // 1080 rounded up to a multiple of 32, like its own 720 -> 736
constexpr uint32_t kVisible1080p = 1080;

// The four video size globals that sub_82441990 hard-codes to 1280/720, next to the output mode variable
// (0x82A2CF80). The game's front buffer size is taken from them (see below).
constexpr uint32_t kGlobalWidth0 = 0x82A2CF68;
constexpr uint32_t kGlobalHeight0 = 0x82A2CF6C;
constexpr uint32_t kGlobalWidth1 = 0x82A2CF70;
constexpr uint32_t kGlobalHeight1 = 0x82A2CF74;
constexpr uint32_t kGlobalModeOutput = 0x82A2CF80;
constexpr uint32_t kModeOutput1080p = 3;  // su table: 0 -> 640x480, 2 -> 1280x720, 3 -> 1920x1080

// Pointer global al renderer (lis r11,-32093 / lwz -11860).
constexpr uint32_t kRendererGlobal = 0x82A2D1AC;

// Set descriptor that sub_8245D320 receives in r5.
constexpr uint32_t kDescWidth = 8;
constexpr uint32_t kDescHeight = 12;
constexpr uint32_t kDescMsaa = 36;
constexpr uint32_t kDescTiling = 41;  // byte
constexpr uint32_t kDescStrips = 124;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + address, &v, sizeof(v));
}

struct Mode {
  uint32_t strips, width, height, msaa;
};

// Original table MSAA per mode, valid once g_table_matched is true.
std::array<std::atomic<uint32_t>, kModes> g_msaa_original{};
std::atomic<bool> g_table_matched{false};

Mode ReadMode(const uint8_t* base, uint32_t obj, uint32_t m) {
  return {Read32(base, obj + kOffStrips + 4 * m), Read32(base, obj + kOffWidth + 4 * m),
          Read32(base, obj + kOffHeight + 4 * m), Read32(base, obj + kOffMsaa + 4 * m)};
}

void MatchModesToModeWithoutAa(uint8_t* base, uint32_t obj) {
  if (obj == 0) {
    REXLOG_WARN("[render] renderer nulo al registrar los modes: no se due nothing");
    return;
  }

  const uint32_t mode_current = Read32(base, kRendererGlobal) == obj ? Read32(base, obj) : ~0u;
  for (uint32_t m = 0; m < kModes; ++m) {
    const Mode x = ReadMode(base, obj, m);
    REXLOG_INFO("[render] mode {} del game: {} tira(s), {}x{}, MSAA {}", m, x.strips, x.width,
                x.height, x.msaa);
  }

  // Only act if the table is the one that was analyzed. If another version of the
  // executable had a different one, better not to write blindly.
  const Mode base_2 = ReadMode(base, obj, kModeWithoutAa);
  const uint32_t rect2 = obj + kOffRects + kBytesByModeRects * kModeWithoutAa;
  const uint32_t r2_x1 = Read32(base, rect2), r2_y1 = Read32(base, rect2 + 4);
  const uint32_t r2_x2 = Read32(base, rect2 + 8), r2_y2 = Read32(base, rect2 + 12);
  const bool table_expected = base_2.strips == 1 && base_2.msaa == 0 && base_2.width == 1280 &&
                              base_2.height >= 720 && r2_x1 == 0 && r2_y1 == 0 &&
                              r2_x2 == base_2.width && r2_y2 >= 720 && r2_y2 <= base_2.height;
  if (!table_expected) {
    REXLOG_WARN("[render] la table de modes no coincide con la analizada "
                "(mode 2: {} tira(s) {}x{} MSAA {}, rect {},{},{},{}): se leaves as esta",
                base_2.strips, base_2.width, base_2.height, base_2.msaa, r2_x1, r2_y1, r2_x2, r2_y2);
    return;
  }

  for (uint32_t m = 0; m < kModes; ++m) {
    // The MSAA of each mode before touching it (SamplesOriginalModeCurrent).
    g_msaa_original[m].store(ReadMode(base, obj, m).msaa, std::memory_order_relaxed);
  }
  g_table_matched.store(true, std::memory_order_relaxed);
  if (REXCVAR_GET(nfsmw_render_test_keep_strips)) {
    REXLOG_INFO("[render] test: los modes 3-5 se remain con sus strips (nfsmw_render_test_keep_strips)");
    return;
  }
  // Mode 2 keeps its usual size (1280x736). The 1080p one is mode 4, below: that way two sets are
  // registered, one per resolution, and it switches live by choosing the mode.
  const uint32_t width = base_2.width;
  const uint32_t height = base_2.height;
  for (uint32_t m = 3; m < kModes; ++m) {
    Write32(base, obj + kOffStrips + 4 * m, 1);
    Write32(base, obj + kOffWidth + 4 * m, width);
    Write32(base, obj + kOffHeight + 4 * m, height);
    if (!REXCVAR_GET(nfsmw_render_test_keep_msaa)) {
      Write32(base, obj + kOffMsaa + 4 * m, 0);
    }
    // Only the first tile is read (sub_82458850 copies tiles*16 bytes).
    std::memcpy(base + obj + kOffRects + kBytesByModeRects * m, base + rect2, 16);
  }

  // And mode 4 becomes the 1080p one, with its own set. That way the resolution changes live by choosing
  // mode 2 or mode 4, without registering anything again.
  {
    const uint32_t rect4 = obj + kOffRects + kBytesByModeRects * kMode1080p;
    Write32(base, obj + kOffStrips + 4 * kMode1080p, 1);
    Write32(base, obj + kOffWidth + 4 * kMode1080p, kWidth1080p);
    Write32(base, obj + kOffHeight + 4 * kMode1080p, kHeight1080p);
    Write32(base, obj + kOffMsaa + 4 * kMode1080p, 0);
    Write32(base, rect4 + 0, 0);
    Write32(base, rect4 + 4, 0);
    Write32(base, rect4 + 8, kWidth1080p);
    Write32(base, rect4 + 12, kVisible1080p);
    REXLOG_INFO("[resolution] mode {} prepared a {}x{} (area visible {}x{}): es el de docked", kMode1080p,
                kWidth1080p, kHeight1080p, kWidth1080p, kVisible1080p);
  }

  REXLOG_INFO("[render] modes 3-5 igualados al mode 2: 1 tira {}x{} sin MSAA "
              "(mode seleccionado now: {}). La scene ya no se repite por tira.",
              width, height, mode_current);
}

std::atomic<uint32_t> g_sets_without_msaa{0};

// Mode the game had chosen before it was forced to 2 (0 = never forced).
std::atomic<uint32_t> g_mode_requested{0};
std::atomic<uint32_t> g_warnings_mode{0};

bool ModeWithoutAaActive() {
  return REXCVAR_GET(nfsmw_render_without_tile) && REXCVAR_GET(nfsmw_render_mode_without_aa);
}

// After each mode choice by the game (sub_824402F0 and sub_82441990): if it chose 3, 4 or 5, it stays at
// 2. It is written after the game has done its part with that choice (its quality calls do not read the
// index).
/*
 * Effective docked mode: the real dock or the one Reverse-NX fakes.
 *
 * Looking only at the hardware would avoid a known cost: in handheld mode with Reverse-NX, 1080p leaves a
 * race at 9.3-9.9 FPS against 13.8-14.8 at 720p (the clocks do not go up: the profile shows 307.2 MHz in
 * all fourteen reports, and the clock sysmodule in use does not register "sys:clk").
 *
 * Reverse-NX still decides the resolution too, by design and knowing that price. The fixed options are
 * there for whoever prefers otherwise.
 */
bool InDocked() {
#if REX_PLATFORM_SWITCH
  return rex::ui::switch_saltynx::ModeBase(appletGetOperationMode() == AppletOperationMode_Console);
#else
  return false;
#endif
}

/*
 * The output size latch. Decided only once, in ImposeSizeOfOutput (below), and needed up here because
 * ModeThatDue has to follow it.
 */
std::atomic<int> g_output_latch{-1};

/*
 * The mode to use: 2 (1280x720) or 4 (1920x1080).
 *
 * The output latch decides, not the current Reverse-NX mode.
 *
 * The game's front buffer is created once at startup and never changes (log: "output del game al
 * start: 1920x1080, impuesta"). If the scene followed Reverse-NX live and the front buffer did not,
 * the scene ended up drawn at 1280x720 in the corner of a 1920x1080 front buffer, with the HUD spanning
 * the 1920 width and the rest uninitialized: the cropping and the white blotch seen on the console. It is
 * the same reasoning already written in WriteSizeVideo: the logical screen size and the front buffer
 * size must always agree.
 *
 * So Reverse-NX decides the resolution, but it is read at startup. Changing it with the game running
 * moves the window and the clocks, not the internal resolution; that requires restarting the game.
 */
uint32_t ModeThatDue() {
  const int latch = g_output_latch.load(std::memory_order_acquire);
  if (latch >= 0) {
    const uint32_t quiero = latch > 0 ? kMode1080p : kModeWithoutAa;
    // Only once: if Reverse-NX asks for the opposite, say so. The game's output is already created and
    // cannot change on the fly, so neither can the internal resolution: a restart is needed.
    static std::atomic<bool> warned{false};
    if (!warned.load(std::memory_order_relaxed) &&
        REXCVAR_GET(nfsmw_internal_resolution) == "automatico") {
      const bool docked = InDocked();
      if ((docked ? kMode1080p : kModeWithoutAa) != quiero && !warned.exchange(true)) {
        REXLOG_INFO("[resolution] Reverse-NX dice {} pero la output del game ya se creo para {}: la "
                    "resolution resolution NO cambia en marcha, there_is que reset el game",
                    docked ? "docked" : "handheld",
                    latch > 0 ? "1920x1080" : "1280x720");
      }
    }
    return quiero;
  }
  const std::string r = REXCVAR_GET(nfsmw_internal_resolution);
  if (r == "1920x1080") {
    return kMode1080p;
  }
  if (r == "automatico") {
    return InDocked() ? kMode1080p : kModeWithoutAa;
  }
  return kModeWithoutAa;  // 1280x720 and 1024x576 (the latter goes through the video mode)
}

std::atomic<uint32_t> g_mode_set{0};

void ForceModeWithoutAa(uint8_t* base) {
  if (!ModeWithoutAaActive()) {
    return;
  }
  const uint32_t obj = Read32(base, kRendererGlobal);
  if (obj == 0) {
    return;
  }
  const uint32_t mode = Read32(base, obj);
  const uint32_t quiero = ModeThatDue();
  if (mode == quiero) {
    return;
  }
  if (mode < kModes) {
    const uint32_t before = g_mode_requested.exchange(mode, std::memory_order_relaxed);
    if (before != mode && g_warnings_mode.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXLOG_INFO("[render] el game eligio el mode {}: se usa el {}", mode, quiero);
    }
  }
  Write32(base, obj, quiero);
  if (g_mode_set.exchange(quiero, std::memory_order_relaxed) != quiero) {
    REXLOG_INFO("[resolution] {} ({}): mode {}", quiero == kMode1080p ? "1920x1080" : "1280x720",
                InDocked() ? "docked" : "handheld", quiero);
#if REX_PLATFORM_SWITCH
    RexSwitchPerfResolution(quiero == kMode1080p ? kWidth1080p : 1280, quiero == kMode1080p ? kVisible1080p : 720);
#endif
  }
}

}  // namespace

// What the output table hook needs.
bool QuiereOutput1080p() { return ModeThatDue() == kMode1080p; }

/*
 * Where the game's front buffer really comes from.
 *
 * Read in the recompiled code:
 *
 *   sub_824402F0 (the one that calls XGetVideoMode) hard-codes 1280 and 720 into the four globals as soon
 *   as the video mode is HD with width >= 1280:
 *       stw 1280 -> 0x82A2CF70 and 0x82A2CF68      stw 720 -> 0x82A2CF74 and 0x82A2CF6C
 *   and then sub_82440420 builds the D3DPRESENT_PARAMETERS (at 0x828FBB70) reading exactly the first two:
 *       lwz 0x82A2CF68 -> [params+0] BackBufferWidth      lwz 0x82A2CF6C -> [params+4] BackBufferHeight
 *   before calling CreateDevice (sub_825A1658).
 *
 * That is why raising the video mode to 1920x1080 did nothing: the game overwrites it with its constant
 * and never looks at what the system says beyond "it is HD". Here we overwrite the game's value.
 *
 * The pair 0x82A2CF70/74 is the logical screen size (read in dozens of places: HUD, projection, UI) and
 * the pair 0x82A2CF68/6C is the buffer's. All four are raised, which is what the game itself does.
 *
 * One limitation: the front buffer is created once, so this is decided at startup and does not change
 * later. Docking or undocking still changes the scene's internal resolution, not the output resolution.
 */
bool OutputOfGameIs1080p() { return g_output_latch.load(std::memory_order_acquire) > 0; }

void ImposeSizeOfOutput(uint8_t* base, const char* where) {
  int quiero = g_output_latch.load(std::memory_order_acquire);
  if (quiero < 0) {
    quiero = (ModeWithoutAaActive() && QuiereOutput1080p()) ? 1 : 0;
    int expected = -1;
    if (g_output_latch.compare_exchange_strong(expected, quiero, std::memory_order_acq_rel)) {
      // 0 does not mean "1280x720": nothing is touched and the game picks the size, which with 1024x576 is
      // not 720p.
      REXLOG_INFO("[resolution] output del game al start: {} ({})",
                  quiero ? "1920x1080, impuesta" : "la que elija el game", where);
    } else {
      quiero = expected;
    }
  }
  if (quiero <= 0) {
    return;
  }
  Write32(base, kGlobalWidth0, kWidth1080p);
  Write32(base, kGlobalHeight0, kVisible1080p);
  Write32(base, kGlobalWidth1, kWidth1080p);
  Write32(base, kGlobalHeight1, kVisible1080p);
  Write32(base, kGlobalModeOutput, kModeOutput1080p);
  static std::atomic<uint32_t> warnings{0};
  if (warnings.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("[resolution] buffer front del game: {}x{} ({})", kWidth1080p, kVisible1080p, where);
  }
}

// The game's video size, which its front buffer should come from.
void WriteSizeVideo(uint8_t* base) {
  // The latch decides, not the current mode. The logical screen size and the front buffer size must always
  // agree: otherwise the game lays out a 1920x1080 HUD over a 1280x720 buffer.
  if (!OutputOfGameIs1080p()) {
    return;
  }
  Write32(base, kGlobalWidth0, kWidth1080p);
  Write32(base, kGlobalHeight0, kVisible1080p);
  Write32(base, kGlobalWidth1, kWidth1080p);
  Write32(base, kGlobalHeight1, kVisible1080p);
  Write32(base, kGlobalModeOutput, kModeOutput1080p);
  static std::atomic<uint32_t> warnings{0};
  if (warnings.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_INFO("[resolution] size de video del game: {}x{}, mode de output {}", kWidth1080p, kVisible1080p,
                kModeOutput1080p);
  }
}

void WriteOutput1080p(uint8_t* base, uint32_t address_width, uint32_t address_height) {
  Write32(base, address_width, kWidth1080p);
  Write32(base, address_height, kVisible1080p);
  static std::atomic<uint32_t> warnings{0};
  if (warnings.fetch_add(1, std::memory_order_relaxed) < 4) {
    REXLOG_INFO("[resolution] output del game: {}x{}", kWidth1080p, kVisible1080p);
  }
}

// Samples per pixel of the AA mode the game has chosen, from its original table. Xbox 360 occlusion
// queries count samples (sub_82225610 expects 2048 in mode 4 and 1024 in mode 2), and the native
// renderer's scene always uses 1.
uint32_t SamplesOriginalModeCurrent(const uint8_t* base) {
  const uint32_t obj = Read32(base, kRendererGlobal);
  if (obj == 0) {
    return 1;
  }
  uint32_t mode = Read32(base, obj);
  // With mode 2 forced, the samples of the mode the game had requested (what the Xbox 360 would count).
  if (const uint32_t requested = g_mode_requested.load(std::memory_order_relaxed);
      mode == kModeWithoutAa && requested != 0 && ModeWithoutAaActive()) {
    mode = requested;
  }
  if (mode >= kModes) {
    return 1;
  }
  // Without a saved table (hook off) the game's table is read as is. D3DMULTISAMPLE_TYPE: 0 = 1, 1 = 2,
  // 2 = 4 samples.
  const uint32_t msaa = g_table_matched.load(std::memory_order_relaxed)
                            ? g_msaa_original[mode].load(std::memory_order_relaxed)
                            : Read32(base, obj + kOffMsaa + 4 * mode);
  return msaa == 1 ? 2 : msaa == 2 ? 4 : 1;
}

}  // namespace nfsmw::render_targets

// Diagnostic nfsmw_diag_luminance: sub_82223308 averages the 64x64 luminance texture the game has just
// resolved (locked with LockRect) and sub_822234F0 stores the result as the target luminance for
// brightness adaptation (g_fAdaptedLum). The address, the first bytes and the result are logged.
REXCVAR_DEFINE_BOOL(nfsmw_diag_luminance, false, "NFSMW",
                    "Solo tests: anota every second la measurement de luminance del brightness (sub_82223308)");
REX_EXTERN(__imp__sub_82223308);
REX_HOOK_RAW(sub_82223308) {
  const uint32_t address = ctx.r3.u32;
  const uint32_t texels = ctx.r4.u32;
  __imp__sub_82223308(ctx, base);
  if (!REXCVAR_GET(nfsmw_diag_luminance)) {
    return;
  }
  static std::atomic<uint64_t> last{0};
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
  if (now - last.load(std::memory_order_relaxed) < 1000) {
    return;
  }
  last.store(now, std::memory_order_relaxed);
  std::string bytes;
  uint64_t sum_b3 = 0, sum_b0 = 0;
  for (uint32_t i = 0; i < texels && i < 4096; ++i) {
    sum_b0 += base[address + i * 4];
    sum_b3 += base[address + i * 4 + 3];
  }
  for (uint32_t i = 0; i < 16; ++i) {
    bytes += fmt::format("{:02X}{}", base[address + i], (i % 4) == 3 ? " " : "");
  }
  REXLOG_INFO("[luminance] sub_82223308({:08X}, {}) = {:.4f}; first_3 bytes {}; media byte 0 {:.1f}, byte 3 {:.1f}",
              address, texels, ctx.f1.f64, bytes, double(sum_b0) / std::max<uint32_t>(texels, 1),
              double(sum_b3) / std::max<uint32_t>(texels, 1));
}

// The two functions that choose the AA mode. After the original runs, 3, 4 or 5 becomes 2.
REX_EXTERN(__imp__sub_824402F0);
REX_HOOK_RAW(sub_824402F0) {
  __imp__sub_824402F0(ctx, base);
  // This is the one that hard-codes 1280x720 into the four screen size globals.
  nfsmw::render_targets::ImposeSizeOfOutput(base, "XGetVideoMode");
  nfsmw::render_targets::ForceModeWithoutAa(base);
}

// sub_82440420 builds the D3DPRESENT_PARAMETERS from those globals and creates the device. It is the
// last chance to change the front buffer size: after that it already exists.
REX_EXTERN(__imp__sub_82440420);
REX_HOOK_RAW(sub_82440420) {
  nfsmw::render_targets::ImposeSizeOfOutput(base, "before de CreateDevice");
  __imp__sub_82440420(ctx, base);
}
REX_EXTERN(__imp__sub_82441990);
REX_HOOK_RAW(sub_82441990) {
  __imp__sub_82441990(ctx, base);
  // This is the one that sets up video and hard-codes the size, so it is changed here.
  nfsmw::render_targets::WriteSizeVideo(base);
  nfsmw::render_targets::ForceModeWithoutAa(base);
}
// Sample reference for the flare occlusion queries (sub_82225610): with mode 2 forced it is computed
// with the mode the game had requested, so the flare fades as on the Xbox 360 in that mode.
REX_EXTERN(__imp__sub_82225610);
REX_HOOK_RAW(sub_82225610) {
  using namespace nfsmw::render_targets;
  const uint32_t requested = g_mode_requested.load(std::memory_order_relaxed);
  const uint32_t obj = Read32(base, kRendererGlobal);
  if (requested != 0 && obj != 0 && ModeWithoutAaActive() && Read32(base, obj) == kModeWithoutAa) {
    Write32(base, obj, requested);
    __imp__sub_82225610(ctx, base);
    Write32(base, obj, kModeWithoutAa);
    return;
  }
  __imp__sub_82225610(ctx, base);
}

// Registration of the 6 main scene sets, one per AA mode.
// r3 = renderer. The table is fixed before the original reads it.
REX_EXTERN(__imp__sub_82458850);
REX_HOOK_RAW(sub_82458850) {
  if (REXCVAR_GET(nfsmw_render_without_tile)) {
    nfsmw::render_targets::MatchModesToModeWithoutAa(base, ctx.r3.u32);
  }
  __imp__sub_82458850(ctx, base);
}

// The game's output resolution, which its front buffer comes from. The video mode does not decide it
// (tested: with video_mode 1920x1080 the front buffer stayed at 1280x720); this table of the game's does:
// it reads a global and returns 640x480, 1280x720 or 1920x1080 through its output pointers. The 1080p
// mode was already in the 2005 binary; nothing selects it, so it is imposed here.
REX_EXTERN(__imp__sub_82447F78);
REX_HOOK_RAW(sub_82447F78) {
  const uint32_t output_width = ctx.r4.u32;
  const uint32_t output_height = ctx.r5.u32;
  __imp__sub_82447F78(ctx, base);
  if (!nfsmw::render_targets::QuiereOutput1080p() || !output_width || !output_height) {
    return;
  }
  nfsmw::render_targets::WriteOutput1080p(base, output_width, output_height);
}

// Binding of a render target set. The game does it every frame, so it is where the resolution is checked
// for a change because the console has been docked or undocked.
REX_EXTERN(__imp__sub_8245D5F8);
REX_HOOK_RAW(sub_8245D5F8) {
  nfsmw::render_targets::ForceModeWithoutAa(base);
  __imp__sub_8245D5F8(ctx, base);
}

// Registration of a set of render targets. r5 = the caller's temporary descriptor,
// which the original copies to the global table. MSAA is removed from it; the
// caller writes that field again before each registration.
REX_EXTERN(__imp__sub_8245D320);
REX_HOOK_RAW(sub_8245D320) {
  using namespace nfsmw::render_targets;
  const uint32_t desc = ctx.r5.u32;
  if (desc != 0 && REXCVAR_GET(nfsmw_internal_resolution) == "1920x1080") {
    // With the test on, every set matters, with or without MSAA: if their sizes grow with the table, this
    // is the 1080p path; if they stay at 1280x720, they come from somewhere else.
    static std::atomic<uint32_t> warnings{0};
    if (warnings.fetch_add(1, std::memory_order_relaxed) < 32) {
      REXLOG_INFO("[1080p] set registered: {}x{}, MSAA {}, tiling {} con {} tira(s)",
                  Read32(base, desc + kDescWidth), Read32(base, desc + kDescHeight), Read32(base, desc + kDescMsaa),
                  base[desc + kDescTiling], Read32(base, desc + kDescStrips));
    }
  }
  if (REXCVAR_GET(nfsmw_render_without_tile) && !REXCVAR_GET(nfsmw_render_test_keep_msaa) && desc != 0) {
    const uint32_t msaa = Read32(base, desc + kDescMsaa);
    if (msaa != 0) {
      Write32(base, desc + kDescMsaa, 0);
      const uint32_t n = g_sets_without_msaa.fetch_add(1, std::memory_order_relaxed) + 1;
      REXLOG_INFO("[render] set {}x{} registered sin MSAA (pedia {}), tiling {} con {} tira(s); "
                  "{} set(s) corregidos",
                  Read32(base, desc + kDescWidth), Read32(base, desc + kDescHeight), msaa,
                  base[desc + kDescTiling], Read32(base, desc + kDescStrips), n);
    }
  }
  __imp__sub_8245D320(ctx, base);
}
