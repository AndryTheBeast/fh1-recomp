// nfsmw - who really draws the race shadow map
//
// ===========================================================================
//  The complete chain, read from the binary. Three levers failed because they all pointed at the same
//  place, and that place is the smallest of the three that draw.
//
//  sub_82443B18 is the shadow pass. It receives the one/two maps boolean in r4 and loops once per map
//  (r18 = 1 or 2) over views 13 and 14 (r26 starts at 0x82A38070 + 1456 = view 13 and advances 112
//  bytes). Within each iteration it draws with four different emitters, in this exact order
//  (nfsmw_recomp.55.cpp:16149, asm 299-438):
//
//    1. sub_824FA010(vista, 0x200000)   RenderWorldModels  -> WorldModels with bones
//    2. sub_824FA010(vista, 0x202000)   RenderWorldModels  -> WorldModels without bones
//    3. sub_824C3610(cull, vista, 512)  StuffScenery       -> scenery marked as caster
//    4. a loop over a global list (0x82C84D38: pointer at +4, count at +12) that calls virtual
//       method +12 of each element with (vista, 0). These are the entities: the cars.
//
//  All four end in the same function, sub_8243E358 = eViewPlatInterface::Render(eModel*, matrix,
//  light, flags, blend) (EcstasyData.hpp:232). It is the bottleneck all geometry goes through, which is
//  why it is measured there.
//
// ---------------------------------------------------------------------------------------------
//  Why none of the three moved a single triangle
//
//  All three only touched emitter 3 (the scenery), and only the part of it that was already filtered.
//
//  a) nfsmw_shadows_lod (bit 0x1000 in cull_info+132) and nfsmw_shadows_lod_h (H in cull_info+192)
//     write into the SceneryCullInfo. The only reader is DrawAScenery, which only decides which
//     scenery enters the scenery draw list. Neither RenderWorldModels nor the car loop look at that
//     record: it does not exist for them.
//
//  b) On top of that, StuffScenery is called for shadows with stuff_flags = 512 (0x200), which
//     according to Scenery.cpp:1065-1095 sets exclusive_flags = 0x10000. With that, of the whole
//     scenery list the culling has just built, only what carries that bit is drawn, and only these
//     set it (Scenery.cpp:1131-1147):
//         (SceneryInst->ExcludeFlags & 0x200000) && !(ExcludeFlags & 0x4000000)   -> normal caster
//         (SceneryInst->ExcludeFlags & 0x40000000)                                -> forced caster
//     So the scenery that reaches the shadow map is a subset hand-marked by the artists. Lowering the
//     LOD of the whole list does not touch the few that pass the filter.
//
//  c) nfsmw_shadows_cut (PixelMinSize, vista+36) does target the right emitter (WorldModel::Render
//     uses it, WorldModel.cpp:313), but the value falls six times short. The cutoff is
//         distance = 35 * (1 + H / PixelMinSize)          (eView.cpp:44-64, fixed radius 35)
//     and with the numbers measured on the console (H of view 13 = 9,146.3; H of the scene = 790.6;
//     PixelMinSize = width_target * 0.009375, i.e. 15 in the map and 12 on screen):
//         scene     (H 790.6  P 12)  -> culls at   2,341 units
//         map       (H 9146.3 P 15)  -> culls at  21,376 units     9.1 times farther
//         map with nfsmw_shadows_cut = 150 (P 22) -> culls at 14,583 units
//     All of Rockport fits within 14,583 units: that is why that cvar comes out inert. The lever
//     exists; the number is just absurd for the H of a shadow view.
//
//  d) nfsmw_shadows_maps (1 map instead of 2) is something else and lives in nfsmw_clip_shadows.cpp.
//
// ---------------------------------------------------------------------------------------------
//  C7 read: the shadow map has no fat to trim by LOD. Closed.
//
//  Measured breakdown of view 13 (view 14 always shows 0 draws: nfsmw_shadows_maps leaves a single
//  map). Per-frame averages over 17 race reports (and 15 more from a build that already had C7 but
//  not this lever, so there is a clean A/B):
//
//      emitter                  draws   k triangles   % of triangles
//      world with bones           0.0          0.00         0.0 %
//      static world               5.0          0.21         0.6 %   <- what the lever targeted
//      scenery                  186.4         12.52        35.6 %
//      cars and the rest         77.7         22.41        63.8 %
//      TOTAL                    269.1         35.15
//
//  Fourth failed lever, and for the same reason as the previous three: the wrong emitter. This time
//  the mechanism was right (WorldModel::Render does read PixelMinSize, and the log confirms the write
//  22 -> 72), but that whole emitter is worth 0.6 % of the map. The A/B:
//      without -> with:  WorldModel drawn 3.73 -> 5.00 | world-rest 0.16 -> 0.21 k triangles
//                        whole map (C7) 31.89 -> 35.15 k | shadows (C6) 63.3 -> 67.5 k
//  Everything goes up instead of down; they are two different laps, so the delta is noise, but the
//  conclusion does not depend on the noise: there are not even 0.2 k triangles to save there.
//
//  And the top ten solids by name say the other two emitters have nothing either:
//    - The player's car alone is 45 % of the top ten: "RX8_KIT00_BODY_A" 4,622 tris in ONE draw and
//      "RX8_BASE_A" 1,354, i.e. 5.98 k = 17 % of the whole map in TWO draws. The "_A" is the LOD:
//      the hero enters the shadow map with the highest-detail mesh. And it cannot be lowered:
//      CarRenderInfo::Render does car_body_lod = bMin(mMaxLodLevel, bodyLodIx + mMinLodLevel) and
//      for CarRenderUsage_Player (RideInfo, CarInfo.cpp:814-818) mMin == mMax, so neither the pixel
//      size nor ForceCarLOD (which also goes through bClamp(min,max)) moves it. Getting "_A" rather
//      than "_B" with mMin==mMax==B in the PS2 decomp is precisely the proof that the 360 binary
//      pins it even higher.
//    - The other cars already come in the low mesh: without the hero there are ~76 draws with
//      16.4 k, i.e. 215 triangles per draw. With the H of view 13 (9,146) and the light ~2,000 units
//      away, car_pixel_size comes out ~11 and the CarBodyLodSwapSize ladder {120,25,20,10,0} already
//      puts them on the last step. There is no lower step.
//    - The scenery is "XT_" (vegetation: TREELINE, REDWOOD, POPLAR, JUNIPER, HEDGE, BUSH_FOREST) and
//      "XB_" (buildings: HAPARTMENT, CP_APARTMENT, COLONIALHSE, BEACHHOTEL), almost all "_DEINST",
//      which are already merged batches. Their 186 draws average 67 triangles: hundreds of tiny
//      instances (XT_REDWOODL 26 tris/draw over 54 draws, XO_OVERPASS 16, XO_CRASHBARREL 48). The
//      big ones left are the merged batches, and those are the subset the artists hand-marked as
//      casters: exactly what was already found inert twice.
//
//  In short: of the map's 35 k triangles, 6 k are a car pinned by design, 16 k are cars already at
//  their worst mesh and 12.5 k are hundreds of 67-triangle instances. The ceiling of everything that
//  could be trimmed by LOD is ~6 k guest triangles (~11.5 k real, given the 1.92 factor between C7
//  and C6) = 0.64 ms, and getting it would mean ruining the player car's shadow. Not touched. Any
//  reduction of the pass's 3.5 ms has to come from something other than geometry: the 5.12 Mtexels
//  it opens or the vegetation's alpha material.
//
//  ---------------------------------------------------------------------------------------------
//  How the breakdown is measured
//
//  The right lever differs per emitter:
//      world      -> PixelMinSize of view 13/14 (what nfsmw_shadows_world_cut does)
//      scenery    -> cull_info (already tried, moves nothing)
//      cars       -> not possible with PixelMinSize: the game exempts views 13 and 14 on purpose.
//                    CarRender.cpp:2814 says literally
//                        if (car_pixel_size < view->GetPixelMinSize())
//                            if ((unsigned)(view->ID - 13) > 1) return false;
//                    so in views 13 and 14 the car is always drawn, however small. For cars the
//                    lever is the LOD passed to CarRenderInfo::Render (tireLOD and carLOD
//                    parameters), not the size.
//
//  The breakdown is measured in sub_8243E358 because all four pass through it, and it is attributed
//  with a thread mark set by the RenderWorldModels and StuffScenery hooks. The triangles come from the
//  model itself: eModel+0x0C = eSolid, eSolid+0x14 = NumPolys (int16), eSolid+0xA0 = name
//  (Ecstasy.hpp:15 and 95). Only fields the game dereferences in that same call are read, and after
//  it has done so, so no memory the game does not touch can be touched.
//
//  What the log says (the "C7" line), every 10 s and per frame:
//    - per view (13, 14 and view 1 as reference) and per emitter: draws and k triangles
//    - how many WorldModels were examined and how many were drawn (the rest were culled by the game
//      or by this lever): this shows at a glance whether nfsmw_shadows_world_cut bites
//    - how many times CullView runs with a cull_info of views 13/14 and how many SceneryDrawInfo it
//      leaves in its list. If it is 0, the scenery is not even culled for the shadow map; if it is
//      large but the "scenery" emitter draws little, the caster filter of StuffScenery is what
//      trims, not the LOD.
//    - the 10 solids that put the most triangles into the map, by name. That tells whether it is a
//      car, a building or vegetation, with no interpretation.
//
// ---------------------------------------------------------------------------------------------
//  The lever that was tried: nfsmw_shadows_world_cut -> off (0), measured to have no effect
//
//  It is the PixelMinSize of views 13 and 14, but expressed in world units, which is the only thing
//  that can be reasoned about. It is computed by inverting the formula above:
//
//      PixelMinSize = 35 * H / (cut - 35)          H read from the view in that same frame
//
//  It is written right before RenderWorldModels and the original value is restored right after, so
//  it does not overwrite nfsmw_shadows_cut (which writes the same field from the pass hook) and it
//  does not touch any other view or pass. And it only tightens: if the computed value is lower than
//  the current one, nothing is written.
//
//  Why it was set to 4500 and why it is now 0.
//  The safety reasoning was correct (and so was the write: the log confirms it), but the emitter it
//  targets contributes 0.6 % of the map's triangles. Culling something that costs nothing gains
//  nothing, and background shadows are lost in exchange. It stays at 0.
//
//  The scene stops drawing an object beyond 2,341 units from the scene camera. The light camera is
//  2,000 units away from it (measured on the console: the scene looks from (539, 4549, 230) and the
//  map from (1029, 3694, 1971)). By the triangle inequality, an object the scene draws is at most
//  2,341 + 2,000 = 4,341 units from the light. With the cutoff at 4,500 no object visible on screen
//  loses its shadow: only shadows of things the scene no longer draws are lost. With 15 the map
//  reached 21,376 units, 4.8 times farther than needed.
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_INT32(nfsmw_shadows_world_cut, 0, "NFSMW",
                     "PROBADA INERTE (compilacion 117, con la C7 ahead). La stick SI writes -el log dice "
                     "'PixelMinSize 22 -> 72, los WorldModel dejan de proyectar shadow a 4481 units en time "
                     "de a 14586'- pero el emitter al que apunta pone 0,21 k triangles de los 35,1 k del map, "
                     "o sea el 0,6 %. A/B de la vista 13 (116 sin ella / 117 con ella): WorldModel drawn "
                     "3,73 -> 5,00 y world-rest 0,16 -> 0,21 k triangles; el map whole 31,9 -> 35,1 k y la "
                     "C6 63,3 -> 67,5 k. No low: sube, inside del ruido de dos laps different_2. Aunque "
                     "cortara el 100 % del emitter ahorraria 0,2 k triangles = 0,01 ms. Se leaves en 0 para no "
                     "perder shadows de background a change de nothing. Distance en units del world since la camara "
                     "de la light; 0 = dejar el input_value del game. No due cars ni scenery")
    .range(0, 60000);

REXCVAR_DEFINE_BOOL(nfsmw_shadows_split, true, "NFSMW",
                    "Note every pocos seconds el split REAL del pass de shadows: how_many_2 draws y "
                    "how_many_2 triangles pone every emitter (world con bones, world estatico, scenery y "
                    "cars) en the views 13 y 14, y los diez solids que mas triangles meten, por "
                    "name. Es la measurement que dice which de the palancas can servir. Off no cuesta "
                    "nothing: el hook hot reads un boolean y llama al original");

REXCVAR_DEFINE_INT32(nfsmw_shadows_split_every_s, 10, "NFSMW",
                     "Every how_many_2 seconds se anota el split del pass de shadows")
    .range(2, 120);

REXCVAR_DEFINE_INT32(nfsmw_shadows_lod_h, 0, "NFSMW",
                     "PROBADA INERTE (compilacion 114): writes la H en el SceneryCullInfo, que solo reads "
                     "DrawAScenery, o sea solo el scenery, y encima el que llega al map de shadows ya "
                     "esta filtrado por el bit de caster. El log confirmo la write (9.146 -> 2.372) y "
                     "los triangles pasaron de 64,7 k a 65,5 k. Se leaves en 0 para no perder shadows a "
                     "change de nothing. En percentage de la H de la scene; 0 = no touch")
    .range(0, 1200);

REXCVAR_DEFINE_BOOL(nfsmw_shadows_lod, false, "NFSMW",
                    "PROBADA INERTE (compilacion 113): manda el scenery del map de shadows a la branch del "
                    "cube con el bit 0x1000 del SceneryCullInfo. El log confirmo la mask "
                    "(0x00004114 -> 0x00005114) y los triangles pasaron de 59,1 k a 60,0 k, porque en esa "
                    "branch el scenery marcado 0x1000100 se queda con la mesh good equal");

REXCVAR_DEFINE_BOOL(nfsmw_shadows_lod_diag, false, "NFSMW",
                    "Anota one time por session the masks de all los register_values de vista, para should_check "
                    "que the views 13 y 14 reciben el bit y ninguna other");

namespace nfsmw::shadows_lod {
namespace {

// --- SceneryCullInfo (nfsmwdecomp, Scenery.hpp:123), as laid out on the 360 -------------------
constexpr uint32_t kRegisterBytes = 208;      // sub_82440890: reg_entry[i] = object + i*208
constexpr uint32_t kOffCounter = 2496;       // object+2496 = NumCullInfos (208 * 12)
constexpr uint32_t kOffVista = 128;           // +128 = pView
constexpr uint32_t kOffMask = 132;         // +132 = ExcludeFlags
constexpr uint32_t kOffFirstDraw = 136;    // +136 = pFirstDrawInfo
constexpr uint32_t kOffLastDraw = 140;    // +140 = pCurrentDrawInfo
constexpr uint32_t kOffPosition = 160;        // +160 = Position (bVector3)
constexpr uint32_t kOffHRegister = 192;       // +192 = H
constexpr uint32_t kDrawInfoBytes = 12;       // sizeof(SceneryDrawInfo) on the 360
constexpr uint32_t kBitMeshReduced = 0x1000u;
constexpr uint32_t kRegistersMax = 64;        // sanity: the game builds at most 12

// --- eView (nfsmwdecomp, Ecstasy.hpp:152) -----------------------------------------------------
constexpr uint32_t kBaseViews = 0x82A38070;
constexpr uint32_t kBytesPorVista = 112;
constexpr uint32_t kViews = 24;
constexpr uint32_t kOffIdVista = 4;           // eView+0x04 = ID
constexpr uint32_t kOffHVista = 12;           // eView+0x0C = H
constexpr uint32_t kOffPixelMinSize = 36;     // eView+0x24 = PixelMinSize
constexpr uint32_t kViewScene = 1;
constexpr uint32_t kViewShadows1 = 13;
constexpr uint32_t kViewShadows2 = 14;

// --- eModel / eSolid (nfsmwdecomp, Ecstasy.hpp:15 y 95) ---------------------------------------
constexpr uint32_t kOffSolidInModel = 12;    // eModel+0x0C = eSolid*
constexpr uint32_t kOffNumPolys = 0x14;       // eSolid+0x14 = NumPolys (int16)
constexpr uint32_t kOffNameSolid = 0xA0;   // eSolid+0xA0 = Name[64]
constexpr int32_t kNumPolysMax = 32767;
// On Win32 and Mac ARM64, addresses from here up carry the +0x1000 of REX_PHYS_HOST_OFFSET, and
// this reads with a bare memcpy: nothing counted here lives there, so it is rejected instead of
// reading a shifted address.
constexpr uint32_t kAddressMaxima = 0xE0000000u;
constexpr uint32_t kPage = 0x1000u;

// --- Render constants -------------------------------------------------------------------------
// WorldModel.cpp:253 WorldObjectMaximumRadius; the radius every WorldModel is measured with.
constexpr double kRadioWorld = 35.0;
// exc_flag that sub_82443B18 passes to RenderWorldModels (WorldModel.cpp:281-292).
constexpr uint32_t kFlagShadow = 0x200000u;   // "only those that cast shadows"
constexpr uint32_t kFlagWithoutBones = 0x2000u;  // with the bit: static ones; without it: animated ones
// The three numbers of ScenerySectionHeader::DrawAScenery, in pixels and absolute.
constexpr double kThresholdDraw = 17.0;
constexpr double kThresholdMeshGood = 52.2;    // 8.7 x the Density minimum (6.0)
constexpr double kRadioOfReference = 35.0;
// Range in which an H makes sense: below it the map would empty, above it it is not a scale.
constexpr double kHMinimum = 1.0;
constexpr double kHMaxima = 1.0e6;
constexpr uint32_t kPixelMinSizeMax = 4096;   // the same sanity cap nfsmw_shadows_cut uses

// --- Emitters ---------------------------------------------------------------------------------
// In the shadow pass RenderWorldModels is called twice and the split is exact: 0x200000 carries the
// WorldModels with bones and 0x202000 those without. In the other passes it is called once and all
// of the world lands in "world-rest", which there simply means "world".
enum Emitter : uint8_t {
  kEmitterWorldAnimated = 0,   // RenderWorldModels(vista, 0x200000): WorldModel con bones
  kEmitterWorldRest = 1,     // RenderWorldModels in any other case
  kEmitterScenery = 2,      // StuffScenery
  kEmitterOthers = 3,          // the entity loop (cars) and everything else
  kEmitters = 4,
};
const char* const kNameEmitter[kEmitters] = {"world-conhuesos", "world-rest", "scenery",
                                              "others (cars y demas)"};

struct Cell {
  std::atomic<uint32_t> draws{0};
  std::atomic<uint64_t> triangles{0};
};
Cell g_cell[kViews][kEmitters];
std::atomic<uint32_t> g_world_looked[kViews];   // calls to WorldModel::Render per view
std::atomic<uint32_t> g_cull_passes[kViews];    // times CullView receives a cull_info of that view
std::atomic<uint32_t> g_cull_objects[kViews];    // SceneryDrawInfo entries culling leaves in its list
std::atomic<uint32_t> g_frames{0};

// Thread mark: who gets credited with the draw going through eView::Render right now.
thread_local uint8_t t_emitter = kEmitterOthers;

// The hot hook does not read the cvar: it reads this, refreshed once per frame.
std::atomic<bool> g_split_active{false};

// --- Solids table, only for views 13 and 14 (110 draws per frame) -----------------------------
constexpr uint32_t kSolids = 128;
constexpr uint32_t kPolls = 4;
constexpr uint32_t kNameLong = 28;
struct Solid {
  std::atomic<uint32_t> key{0};   // pointer al eSolid; 0 = free
  std::atomic<uint32_t> draws{0};
  std::atomic<uint64_t> triangles{0};
  char name[kNameLong + 1] = {0};
};
Solid g_solids[kSolids];
std::atomic<uint32_t> g_solids_lost{0};

std::atomic<bool> g_noted{false};
std::atomic<bool> g_noted_diag{false};
std::atomic<bool> g_noted_cameras{false};
std::atomic<int32_t> g_pct_noted{-1};
std::atomic<int32_t> g_cut_noted{-1};
std::atomic<int64_t> g_report_ms{0};

// Position of the current frame's scene camera (report only).
std::atomic<bool> g_scene_view{false};
float g_scene_pos[3] = {0.0f, 0.0f, 0.0f};

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

int32_t Read16With(const uint8_t* base, uint32_t address) {
  uint16_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return int16_t(__builtin_bswap16(v));
}

void Write32(uint8_t* base, uint32_t address, uint32_t input_value) {
  const uint32_t v = __builtin_bswap32(input_value);
  std::memcpy(base + address, &v, sizeof(v));
}

float AsFloat(uint32_t v) {
  float f = 0.0f;
  std::memcpy(&f, &v, sizeof(f));
  return f;
}

uint32_t AsU32(float f) {
  uint32_t v = 0;
  std::memcpy(&v, &f, sizeof(v));
  return v;
}

// Guest floats are big-endian: they must be byte-swapped, not read raw.
float ReadFloat(const uint8_t* base, uint32_t address) { return AsFloat(Read32(base, address)); }

void WriteFloat(uint8_t* base, uint32_t address, float input_value) {
  Write32(base, address, AsU32(input_value));
}

// Only a pointer to the start of a view in the table whose ID matches is accepted.
bool ViewValid(const uint8_t* base, uint32_t vista, uint32_t* id_output) {
  if (vista < kBaseViews) {
    return false;
  }
  const uint32_t displacement = vista - kBaseViews;
  if (displacement % kBytesPorVista != 0) {
    return false;
  }
  const uint32_t index = displacement / kBytesPorVista;
  if (index >= kViews) {
    return false;
  }
  if (Read32(base, vista + kOffIdVista) != index) {
    return false;
  }
  *id_output = index;
  return true;
}

bool HReasonable(double h) { return std::isfinite(h) && h >= kHMinimum && h <= kHMaxima; }

bool IsViewOfShadows(uint32_t id) { return id == kViewShadows1 || id == kViewShadows2; }

// Distance at which an object of radius R stops covering `px` pixels: d = R * (1 + H / px).
double Distance(double h, double px) { return kRadioOfReference * (1.0 + h / px); }

// PixelMinSize that makes an object of radius 35 stop being drawn beyond `cut` units.
// It is the exact inverse of eView::GetPixelSize (eView.cpp:44-64): px = radio * H / (d - radio).
double PixelMinSizeForCut(double h, double cut) {
  return (kRadioWorld * h) / (cut - kRadioWorld);
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// --- Contabilidad ------------------------------------------------------------------------------

void PointSolid(const uint8_t* base, uint32_t solid, uint32_t triangles) {
  const uint32_t start = (solid >> 4) % kSolids;
  for (uint32_t s = 0; s < kPolls; ++s) {
    Solid& row = g_solids[(start + s) % kSolids];
    uint32_t key = row.key.load(std::memory_order_relaxed);
    if (key == 0) {
      // First time it is seen: the name is stored. It is the only time it is read.
      uint32_t expected = 0;
      if (!row.key.compare_exchange_strong(expected, solid, std::memory_order_relaxed)) {
        key = expected;
      } else {
        // The name is only read if it lies in the same 4 KB page as the NumPolys the game has just read:
        // that way no page the game has not touched can be touched. eSolid is 0xE0 bytes, so this holds
        // in 95 % of cases and the rest stay unnamed.
        const uint32_t inside = solid & (kPage - 1);
        if (inside + kOffNameSolid + kNameLong <= kPage) {
          std::memcpy(row.name, base + solid + kOffNameSolid, kNameLong);
          row.name[kNameLong] = 0;
          for (uint32_t i = 0; i < kNameLong; ++i) {
            const unsigned char c = static_cast<unsigned char>(row.name[i]);
            if (c != 0 && (c < 32 || c > 126)) {
              row.name[i] = '?';  // not a name: make it visible in the log without breaking the format
            }
          }
        } else {
          std::memcpy(row.name, "(crosses de page)", 18);
        }
        key = solid;
      }
    }
    if (key == solid) {
      row.draws.fetch_add(1, std::memory_order_relaxed);
      row.triangles.fetch_add(triangles, std::memory_order_relaxed);
      return;
    }
  }
  g_solids_lost.fetch_add(1, std::memory_order_relaxed);
}

void ResetCounts() {
  for (uint32_t v = 0; v < kViews; ++v) {
    for (uint32_t e = 0; e < kEmitters; ++e) {
      g_cell[v][e].draws.store(0, std::memory_order_relaxed);
      g_cell[v][e].triangles.store(0, std::memory_order_relaxed);
    }
    g_world_looked[v].store(0, std::memory_order_relaxed);
    g_cull_passes[v].store(0, std::memory_order_relaxed);
    g_cull_objects[v].store(0, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < kSolids; ++i) {
    g_solids[i].key.store(0, std::memory_order_relaxed);
    g_solids[i].draws.store(0, std::memory_order_relaxed);
    g_solids[i].triangles.store(0, std::memory_order_relaxed);
    g_solids[i].name[0] = 0;
  }
  g_solids_lost.store(0, std::memory_order_relaxed);
  g_frames.store(0, std::memory_order_relaxed);
}

void NoteView(uint32_t vista, double frames) {
  uint32_t draws_total = 0;
  uint64_t triangles_total = 0;
  for (uint32_t e = 0; e < kEmitters; ++e) {
    draws_total += g_cell[vista][e].draws.load(std::memory_order_relaxed);
    triangles_total += g_cell[vista][e].triangles.load(std::memory_order_relaxed);
  }
  const uint32_t cull_passes = g_cull_passes[vista].load(std::memory_order_relaxed);
  if (draws_total == 0 && cull_passes == 0) {
    return;
  }
  const uint32_t looked = g_world_looked[vista].load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[shadows lod] C7 vista {}: TOTAL {:.0f} draws {:.1f} k triangles ({:.0f} tri/draw) = "
      "{} {:.0f}/{:.1f}k | {} {:.0f}/{:.1f}k | {} {:.0f}/{:.1f}k | {} {:.0f}/{:.1f}k; "
      "WorldModel looked {:.0f}, drawn {:.0f}; culling del scenery: {:.2f} passes con "
      "{:.0f} objects en la list",
      vista, double(draws_total) / frames, double(triangles_total) / frames / 1000.0,
      draws_total != 0 ? double(triangles_total) / double(draws_total) : 0.0,
      kNameEmitter[0], double(g_cell[vista][0].draws.load(std::memory_order_relaxed)) / frames,
      double(g_cell[vista][0].triangles.load(std::memory_order_relaxed)) / frames / 1000.0,
      kNameEmitter[1], double(g_cell[vista][1].draws.load(std::memory_order_relaxed)) / frames,
      double(g_cell[vista][1].triangles.load(std::memory_order_relaxed)) / frames / 1000.0,
      kNameEmitter[2], double(g_cell[vista][2].draws.load(std::memory_order_relaxed)) / frames,
      double(g_cell[vista][2].triangles.load(std::memory_order_relaxed)) / frames / 1000.0,
      kNameEmitter[3], double(g_cell[vista][3].draws.load(std::memory_order_relaxed)) / frames,
      double(g_cell[vista][3].triangles.load(std::memory_order_relaxed)) / frames / 1000.0,
      double(looked) / frames,
      double(g_cell[vista][0].draws.load(std::memory_order_relaxed) +
             g_cell[vista][1].draws.load(std::memory_order_relaxed)) /
          frames,
      double(cull_passes) / frames,
      double(g_cull_objects[vista].load(std::memory_order_relaxed)) / frames);
}

void EmitReport(const uint8_t* base, double seconds) {
  (void)base;
  const uint32_t frames = g_frames.load(std::memory_order_relaxed);
  if (frames == 0) {
    ResetCounts();
    return;
  }
  const double f = double(frames);
  REXLOG_INFO(
      "[shadows lod] C7 split del pass de shadows, last {:.1f} s ({} frames), medias POR "
      "FRAME. Los triangles son la sum de eSolid::NumPolys de every eView::Render, o sea el "
      "tally del guest; compare con los \"k triangles\" de la line C6",
      seconds, frames);
  NoteView(kViewShadows1, f);
  NoteView(kViewShadows2, f);
  NoteView(kViewScene, f);

  // The ten solids with the most triangles in the shadow views.
  uint32_t order[kSolids];
  uint32_t n = 0;
  for (uint32_t i = 0; i < kSolids; ++i) {
    if (g_solids[i].key.load(std::memory_order_relaxed) != 0 &&
        g_solids[i].triangles.load(std::memory_order_relaxed) != 0) {
      order[n++] = i;
    }
  }
  std::sort(order, order + n, [](uint32_t a, uint32_t b) {
    return g_solids[a].triangles.load(std::memory_order_relaxed) >
           g_solids[b].triangles.load(std::memory_order_relaxed);
  });
  // All on one line: the whole report comes from the guest thread, and writing fourteen lines in a
  // row in the middle of a frame is noticeable.
  const uint32_t cap = n < 10 ? n : 10;
  std::string list;
  for (uint32_t i = 0; i < cap; ++i) {
    const Solid& s = g_solids[order[i]];
    const uint32_t dib = s.draws.load(std::memory_order_relaxed);
    const uint64_t tri = s.triangles.load(std::memory_order_relaxed);
    list += fmt::format("{}\"{}\" {:.1f}k en {:.0f} dib ({} tri/dib)", i == 0 ? "" : " | ", s.name,
                         double(tri) / f / 1000.0, double(dib) / f, dib != 0 ? uint32_t(tri / dib) : 0u);
  }
  const uint32_t lost_count = g_solids_lost.load(std::memory_order_relaxed);
  REXLOG_INFO(
      "[shadows lod] C7 los {} solids que mas triangles meten en el map de shadows (por frame; "
      "{:.0f} draws no cupieron en la table): {}",
      cap, double(lost_count) / f, list);
  ResetCounts();
}

// Once per frame: refreshes the hot hook's switch and writes the report when due.
void BeatFrame(const uint8_t* base) {
  const bool active = REXCVAR_GET(nfsmw_shadows_split);
  const bool before = g_split_active.exchange(active, std::memory_order_relaxed);
  if (!active) {
    if (before) {
      ResetCounts();
      g_report_ms.store(0, std::memory_order_relaxed);
    }
    return;
  }
  g_frames.fetch_add(1, std::memory_order_relaxed);
  const int64_t now = NowMs();
  int64_t since = g_report_ms.load(std::memory_order_relaxed);
  if (since == 0) {
    g_report_ms.store(now, std::memory_order_relaxed);
    ResetCounts();
    return;
  }
  const int64_t period = int64_t(REXCVAR_GET(nfsmw_shadows_split_every_s)) * 1000;
  if (now - since >= period) {
    g_report_ms.store(now, std::memory_order_relaxed);
    EmitReport(base, double(now - since) / 1000.0);
  }
}

}  // namespace
}  // namespace nfsmw::shadows_lod

// =============================================================================================
// eViewPlatInterface::Render(eModel*, bMatrix4*, eLightContext*, uint32, bMatrix4*)
//
// The bottleneck all geometry goes through: the four emitters of the shadow pass end here, and so do
// the ~920 draws of the scene. It only counts; it changes nothing.
//
// The original is called first and the fields are read afterwards on purpose: the game does
// `Solid = *(model+0x0C)` without checking the model (nfsmw_recomp.105.cpp:15817) and then
// dereferences the Solid, so reading those two fields cannot touch any page the game has not
// already touched.
// =============================================================================================
REX_EXTERN(__imp__sub_8243E358);
// Render runs natively (nfsmw_eview_native.cpp, cvar nfsmw_eview_native). This is still the only hook
// of sub_8243E358: instead of the original it calls nfsmw::eview::Render, which chooses between the
// native version (checked against the original) and the original.
namespace nfsmw::eview {
void Render(PPCContext& ctx, uint8_t* base);
}
REX_HOOK_RAW(sub_8243E358) {
  using namespace nfsmw::shadows_lod;
  if (!g_split_active.load(std::memory_order_relaxed)) {
    nfsmw::eview::Render(ctx, base);
    return;
  }
  const uint32_t vista = ctx.r3.u32;
  const uint32_t model = ctx.r4.u32;
  const uint8_t emitter = t_emitter;
  nfsmw::eview::Render(ctx, base);

  uint32_t id = 0;
  if (model == 0 || model >= kAddressMaxima || !ViewValid(base, vista, &id)) {
    return;
  }
  const uint32_t solid = Read32(base, model + kOffSolidInModel);
  uint32_t triangles = 0;
  if (solid != 0 && solid < kAddressMaxima) {
    const int32_t polys = Read16With(base, solid + kOffNumPolys);
    if (polys > 0 && polys <= kNumPolysMax) {
      triangles = uint32_t(polys);
    }
  }
  g_cell[id][emitter].draws.fetch_add(1, std::memory_order_relaxed);
  if (triangles != 0) {
    g_cell[id][emitter].triangles.fetch_add(triangles, std::memory_order_relaxed);
    if (IsViewOfShadows(id)) {
      PointSolid(base, solid, triangles);
    }
  }
}

// =============================================================================================
// RenderWorldModels(eView* vista, int exc_flag)   (WorldModel.cpp:331)
//
// r3 = the view, r4 = the pass flags. In the shadow pass it is called twice per map: 0x200000 (the
// WorldModels with bones) and 0x202000 (the static ones). Two things happen here:
//   1. mark the emitter so eView::Render knows whom to credit the draws to
//   2. apply nfsmw_shadows_world_cut: raise PixelMinSize only during this call and restore the
//      original value on exit, so nobody else is overwritten (nfsmw_shadows_cut writes the same
//      field from the pass hook and keeps its own bookkeeping).
// =============================================================================================
REX_EXTERN(__imp__sub_824FA010);
REX_HOOK_RAW(sub_824FA010) {
  using namespace nfsmw::shadows_lod;
  const uint32_t vista = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  uint32_t id = 0;
  const bool vista_ok = ViewValid(base, vista, &id);
  const bool in_shadows = vista_ok && IsViewOfShadows(id) && (flags & kFlagShadow) != 0;

  const uint8_t emitter_previous = t_emitter;
  if (vista_ok) {
    // Outside the shadow pass bit 0x2000 means something else, so everything goes to "world-rest".
    t_emitter = ((flags & kFlagShadow) != 0 && (flags & kFlagWithoutBones) == 0) ? kEmitterWorldAnimated
                                                                            : kEmitterWorldRest;
  }

  // --- la stick ------------------------------------------------------------------------------
  bool restore = false;
  uint32_t pmin_original = 0;
  const int32_t cut = REXCVAR_GET(nfsmw_shadows_world_cut);
  if (in_shadows && cut > int32_t(kRadioWorld) + 1) {
    const double h = double(ReadFloat(base, vista + kOffHVista));
    pmin_original = Read32(base, vista + kOffPixelMinSize);
    if (HReasonable(h) && pmin_original != 0 && pmin_original <= kPixelMinSizeMax) {
      const double px = PixelMinSizeForCut(h, double(cut));
      if (std::isfinite(px) && px > 0.0 && px <= double(kPixelMinSizeMax)) {
        const uint32_t pmin_new = uint32_t(std::ceil(px));
        // Only tightens: if the game (or nfsmw_shadows_cut) already asks for more, nothing is touched.
        if (pmin_new > pmin_original) {
          Write32(base, vista + kOffPixelMinSize, pmin_new);
          restore = true;
          if (g_cut_noted.exchange(cut) != cut) {
            const uint32_t view_scene = kBaseViews + kViewScene * kBytesPorVista;
            const double h_scene = double(ReadFloat(base, view_scene + kOffHVista));
            const uint32_t pmin_scene = Read32(base, view_scene + kOffPixelMinSize);
            const double cut_scene =
                (HReasonable(h_scene) && pmin_scene != 0 && pmin_scene <= kPixelMinSizeMax)
                    ? Distance(h_scene, double(pmin_scene))
                    : 0.0;
            REXLOG_INFO(
                "[shadows lod] cut del world en la vista {}: H {:.1f}, PixelMinSize {} -> {} "
                "(los WorldModel dejan de proyectar shadow a {:.0f} units en time de a {:.0f}). "
                "La scene leaves de dibujarlos a {:.0f}. No due cars (el game exime a the views "
                "13 y 14 en CarRender) ni scenery",
                id, h, pmin_original, pmin_new, Distance(h, double(pmin_new)),
                Distance(h, double(pmin_original)), cut_scene);
          }
        }
      }
    }
  }

  __imp__sub_824FA010(ctx, base);

  if (restore) {
    Write32(base, vista + kOffPixelMinSize, pmin_original);
  }
  t_emitter = emitter_previous;
}

// =============================================================================================
// WorldModel::Render(eView* vista, int exc_flag)   (WorldModel.cpp:256)
//
// r3 = the WorldModel, r4 = the view, r5 = the flags. It only counts candidates: how many world
// objects were examined. Subtracting the ones that end up drawn shows how many the set of filters
// removes (mCastsShadow, bones, PixelMinSize and frustum), which is how to check whether
// nfsmw_shadows_world_cut bites.
// =============================================================================================
REX_EXTERN(__imp__sub_824F9D78);
REX_HOOK_RAW(sub_824F9D78) {
  using namespace nfsmw::shadows_lod;
  if (g_split_active.load(std::memory_order_relaxed)) {
    uint32_t id = 0;
    if (ViewValid(base, ctx.r4.u32, &id)) {
      g_world_looked[id].fetch_add(1, std::memory_order_relaxed);
    }
  }
  __imp__sub_824F9D78(ctx, base);
}

// =============================================================================================
// GrandSceneryCullInfo::StuffScenery(eView* vista, int stuff_flags)   (Scenery.cpp:1065)
//
// r3 = the GrandSceneryCullInfo, r4 = the view, r5 = the stuff_flags (512 in the shadow pass).
// It only marks the emitter: within this call, everything that goes through eView::Render is scenery.
// =============================================================================================
REX_EXTERN(__imp__sub_824C3610);
REX_HOOK_RAW(sub_824C3610) {
  using namespace nfsmw::shadows_lod;
  uint32_t id = 0;
  const bool mark = ViewValid(base, ctx.r4.u32, &id);
  const uint8_t emitter_previous = t_emitter;
  if (mark) {
    t_emitter = kEmitterScenery;
  }
  __imp__sub_824C3610(ctx, base);
  t_emitter = emitter_previous;
}

// =============================================================================================
// GrandSceneryCullInfo::DoCulling. Builds the frame's SceneryCullInfo entries and then walks them.
// sub_82440890 calls it and only sub_82445660 calls that one, so: once per race frame. That is why
// the report heartbeat goes here.
// Only ExcludeFlags (+132) is touched here. H (+192) cannot be touched here: the first loop of this
// same function copies it from the view (Scenery.cpp:1005).
// =============================================================================================
REX_EXTERN(__imp__sub_824C3468);
REX_HOOK_RAW(sub_824C3468) {
  using namespace nfsmw::shadows_lod;
  BeatFrame(base);
  const uint32_t object = ctx.r3.u32;
  if (object != 0 && REXCVAR_GET(nfsmw_shadows_lod) && REXCVAR_GET(nfsmw_shadows_lod_h) == 0) {
    const uint32_t count = Read32(base, object + kOffCounter);
    if (count != 0 && count <= kRegistersMax) {
      const bool diag = REXCVAR_GET(nfsmw_shadows_lod_diag) && !g_noted_diag.exchange(true);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t reg_entry = object + i * kRegisterBytes;
        const uint32_t vista = Read32(base, reg_entry + kOffVista);
        uint32_t id = 0;
        if (!ViewValid(base, vista, &id)) {
          continue;
        }
        const uint32_t mask = Read32(base, reg_entry + kOffMask);
        if (diag) {
          REXLOG_INFO("[shadows lod] reg_entry {}: vista {} mask 0x{:08X}", i, id, mask);
        }
        if (!IsViewOfShadows(id) || (mask & kBitMeshReduced) != 0) {
          continue;
        }
        Write32(base, reg_entry + kOffMask, mask | kBitMeshReduced);
        if (!g_noted.exchange(true)) {
          REXLOG_INFO(
              "[shadows lod] vista {}: mask 0x{:08X} -> 0x{:08X}; el threshold de size sube de 17 a 32 px",
              id, mask, mask | kBitMeshReduced);
        }
      }
    }
  }
  __imp__sub_824C3468(ctx, base);
}

// =============================================================================================
// GrandSceneryCullInfo::CullView(SceneryCullInfo*). r3 = GrandSceneryCullInfo, r4 = the cull_info.
// Called after DoCulling copies H and before TreeCull/DrawAScenery use it: the only window where
// writing +192 does anything. Measured inert (see above): nfsmw_shadows_lod_h defaults to 0 and this
// does nothing unless turned on by hand.
// =============================================================================================
REX_EXTERN(__imp__sub_824C33D0);
REX_HOOK_RAW(sub_824C33D0) {
  using namespace nfsmw::shadows_lod;
  const uint32_t reg_entry = ctx.r4.u32;

  // The count that answers the hypothesis: how many times this hook runs with a cull_info of view 13
  // or 14, and how many objects the culling leaves in that view's list. With 0 runs, the scenery is not
  // culled for the shadow map and the three cull_info levers could do nothing. With N runs and M
  // objects, if the "scenery" emitter of the C7 report draws far fewer than M, the trimming comes
  // from the caster filter of StuffScenery.
  const bool register_ok = reg_entry != 0 && reg_entry < kAddressMaxima;
  uint32_t id_count = 0;
  const bool count_2 = register_ok && g_split_active.load(std::memory_order_relaxed) &&
                      ViewValid(base, Read32(base, reg_entry + kOffVista), &id_count);
  uint32_t before_draw = 0;
  if (count_2) {
    g_cull_passes[id_count].fetch_add(1, std::memory_order_relaxed);
    before_draw = Read32(base, reg_entry + kOffLastDraw);
  }

  const int32_t pct = REXCVAR_GET(nfsmw_shadows_lod_h);
  if (register_ok && pct != 0) {
    uint32_t id = 0;
    const uint32_t vista = Read32(base, reg_entry + kOffVista);
    if (ViewValid(base, vista, &id)) {
      if (id == kViewScene) {
        // Stored for the report below: with both cameras it is known how much each % trims.
        for (uint32_t i = 0; i < 3; ++i) {
          g_scene_pos[i] = ReadFloat(base, reg_entry + kOffPosition + i * 4);
        }
        g_scene_view.store(true, std::memory_order_relaxed);
      } else if (IsViewOfShadows(id)) {
        const double h_scene = double(ReadFloat(base, kBaseViews + kViewScene * kBytesPorVista + kOffHVista));
        const double h_map = double(ReadFloat(base, vista + kOffHVista));
        const double h_actual = double(ReadFloat(base, reg_entry + kOffHRegister));
        if (HReasonable(h_scene) && HReasonable(h_map) && HReasonable(h_actual)) {
          const double h_requested = h_scene * double(pct) / 100.0;
          // This lever only tightens: if the request is more permissive than the game, nothing is touched.
          if (h_requested < h_actual) {
            WriteFloat(base, reg_entry + kOffHRegister, float(h_requested));
            if (g_pct_noted.exchange(pct) != pct) {
              REXLOG_INFO(
                  "[shadows lod] H: scene (vista {}) {:.1f}, map (vista {}) {:.1f} = {:.2f}x; con {} % "
                  "escribo {:.1f} en el cull_info, o sea x{:.3f} de lo que usa el game. OJO: esto SOLO "
                  "afecta al scenery, y el que llega al map de shadows ya esta filtrado por el bit de "
                  "caster; measured inerte en la compilacion 114",
                  kViewScene, h_scene, id, h_map, h_map / h_scene, pct, h_requested, h_requested / h_map);
              REXLOG_INFO(
                  "[shadows lod] con esa H un object de radio {:.0f} leaves de proyectar shadow passes "
                  "{:.0f} units (liston 17 px; el game la quitaba a {:.0f}) y low a la mesh reduced "
                  "passes {:.0f} (liston {:.0f} px; el game bajaba a {:.0f}). En la scene esos dos "
                  "limites estan en {:.0f} y {:.0f}",
                  kRadioOfReference, Distance(h_requested, kThresholdDraw), Distance(h_map, kThresholdDraw),
                  Distance(h_requested, kThresholdMeshGood), kThresholdMeshGood,
                  Distance(h_map, kThresholdMeshGood), Distance(h_scene, kThresholdDraw),
                  Distance(h_scene, kThresholdMeshGood));
            }
            if (g_scene_view.load(std::memory_order_relaxed) && !g_noted_cameras.exchange(true)) {
              const double dx = double(ReadFloat(base, reg_entry + kOffPosition + 0)) - double(g_scene_pos[0]);
              const double dy = double(ReadFloat(base, reg_entry + kOffPosition + 4)) - double(g_scene_pos[1]);
              const double dz = double(ReadFloat(base, reg_entry + kOffPosition + 8)) - double(g_scene_pos[2]);
              REXLOG_INFO(
                  "[shadows lod] cameras: la scene looks since ({:.0f}, {:.0f}, {:.0f}) y el map since "
                  "({:.0f}, {:.0f}, {:.0f}); separacion {:.0f} units. Cuanto mas far_depth este la light, mas "
                  "agresivo es el same percentage",
                  double(g_scene_pos[0]), double(g_scene_pos[1]), double(g_scene_pos[2]),
                  double(ReadFloat(base, reg_entry + kOffPosition + 0)),
                  double(ReadFloat(base, reg_entry + kOffPosition + 4)),
                  double(ReadFloat(base, reg_entry + kOffPosition + 8)),
                  std::sqrt(dx * dx + dy * dy + dz * dz));
            }
          }
        }
      }
    }
  }
  __imp__sub_824C33D0(ctx, base);

  if (count_2) {
    // pCurrentDrawInfo advances 12 bytes per object the culling adds to this view's list.
    const uint32_t after = Read32(base, reg_entry + kOffLastDraw);
    const uint32_t first = Read32(base, reg_entry + kOffFirstDraw);
    if (after > before_draw && before_draw >= first && first != 0) {
      const uint32_t bytes = after - before_draw;
      if (bytes % kDrawInfoBytes == 0 && bytes < 0x400000u) {
        g_cull_objects[id_count].fetch_add(bytes / kDrawInfoBytes, std::memory_order_relaxed);
      }
    }
  }
}
