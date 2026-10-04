// nfsc - native renderer, part C5b: what the ring does not say about each draw, taken on the game
// thread.
//
// The vertex shader microcode reaches the ring already patched by D3D: reordered fetches, swizzles from
// the vertex declaration, and outputs the pixel shader does not read nulled out (measured on PC). Looking
// it up in the library by content is not reliable. The exact identity is on the game thread:
//   - The shader constructors (sub_8259BC90 PS, sub_8259C038 VS) receive the original container and
//     return the object: object -> container.
//   - Each D3D Draw* reads the bound VS and PS from the device and leaves a record in a queue. The ring
//     sink pairs each draw with its record, in the same order, checking primitive type and count.

#pragma once

#include <cstdint>

namespace nfsc::native {

class ShadersNative;
struct EntryShader;

enum class FunctionDraw : uint8_t {
  kVertices,      // DrawVertices(device, type, start, count)
  kIndexed,     // DrawIndexedVertices(device, type, base, start, count)
  kVerticesUP,    // DrawVerticesUP(device, type, count, data, stride)
  kIndexedUP,   // DrawIndexedVerticesUP: argument order unconfirmed
};

struct RegisterDraw {
  uint32_t vs = 0;        // bound vertex shader object
  uint32_t ps = 0;        // bound pixel shader object
  uint32_t args[4] = {};  // r4..r7 of the call
  FunctionDraw function = FunctionDraw::kVertices;
  uint16_t vegetation = 0;  // the game's vegetation verdict (kVeg* flags; 0 = none)
  uint64_t shadow = 0;    // sequence of its mirror snapshot (0 = none), see SnapshotMirror
};

/*
 * Direct3D-level renderer, phase 1 (shadow mode).
 *
 * The game's D3D keeps a copy (mirror) of the Xenos registers in the device, and FlushState (825A40C0)
 * dumps it to the ring by dirty groups. If that mirror holds everything a draw needs, the renderer can
 * read it when drawing instead of reading the ~35 packets of each draw. This checks that without
 * touching anything: on 1 in 64 Draw* calls the mirror is snapshotted, and the ring, when pairing that
 * draw, compares the snapshot register by register with what it read from the packets ("shadow D3D"
 * line).
 */
constexpr uint32_t kGroupsMirror = 9;  // 0x2000..0x2380 in steps of 0x80, and 0x4900 (booleans)
struct SnapshotMirror {
  uint64_t mask[kGroupsMirror] = {};         // registers of each group that D3D handles (learned)
  uint32_t base_register[kGroupsMirror] = {};
  uint32_t state[kGroupsMirror][64] = {};      // state[g][i] = register base_register[g] + i
  uint32_t fetch[192] = {};                     // 0x4800.. (+0x480 of the device)
  uint32_t constants[2048] = {};               // 0x4000.. VS (+0x780) y 0x4400.. PS (+0x1780)
};
// From the register dump hook (825A2AA0): which registers of which group, and where their mirror is.
void LearnGroupMirror(uint32_t register_base, uint64_t mask, uint32_t displacement);
// Ring thread only. false if the snapshot has already been reused for another draw.
bool ReadSnapshot(uint64_t sequence, SnapshotMirror& output);

/*
 * Phase 2 of the Direct3D-level renderer: FlushState's compound marker. See nfsc_d3d_registers_native.cpp.
 *
 * FlushState (825A40C0) dumps the dirty registers of the device mirror with ~7 type 0 packets and ~6
 * padding words per draw. With the marker it writes a single type 3 NOP packet with all the segments
 * inside, at the same place in the ring where the packets would have gone:
 *
 *   [0] 0xC0001000 | (P - 1) << 16    PM4 type 3, NOP (0x10), P payload words, no predicate
 *   [1] kMarkerMagic | mode         1 = apply; 2 = check (the type 0 packets of the dump go first)
 *   [2] sequence                      marker number, for the log only
 *   [3] count << 16 | register        first segment, followed by its 'count' values as in the mirror (big-endian)
 *   ...                               the remaining segments, in FlushState order
 *
 * A NOP without the magic belongs to the game itself and is ignored, as before.
 */
constexpr uint32_t kMarkerMagic = 0x4E465300;  // "NFS" and the mode in the low byte (see phase 2b)
constexpr uint32_t kMarkerApply = 1;
constexpr uint32_t kMarkerCheck = 2;

// A marker segment must fit entirely within one of the groups FlushState dumps: 0x2000+16, 0x2100+21,
// 0x2180+5, 0x2200+12, 0x2280+21, 0x2300+38, 0x2380+8, the VS (0x4000) and PS (0x4400) constants, the
// fetches (0x4800+192) and the booleans and loops (0x4900+40). That way it never touches a register with
// side effects in the ring (COHER_STATUS_HOST, SCRATCH, the gamma ramp: all below 0x2000) or mixes
// classes.
inline bool RangeOfDump(uint32_t reg_entry, uint32_t count) {
  static constexpr uint8_t kSize2000[8] = {16, 0, 21, 5, 12, 21, 38, 8};  // per 0x80 block (0x2080 is not included)
  if (count == 0) {
    return false;
  }
  if (reg_entry >= 0x2000 && reg_entry < 0x2400) {
    return (reg_entry & 0x7Fu) + count <= kSize2000[(reg_entry - 0x2000) >> 7];
  }
  if (reg_entry >= 0x4000 && reg_entry < 0x4800) {
    return reg_entry + count <= (reg_entry < 0x4400 ? 0x4400u : 0x4800u);
  }
  if (reg_entry >= 0x4800 && reg_entry < 0x48C0) {
    return reg_entry + count <= 0x48C0;
  }
  if (reg_entry >= 0x4900 && reg_entry < 0x4928) {
    return reg_entry + count <= 0x4928;
  }
  return false;
}

// The native system turns it on when the ring thread starts and off at shutdown. When off, FlushState
// always takes the game's path: with the Xenos emulation nobody would understand the marker.
void ActivateConsumerMarkers(bool active);
// Ring thread only, when reading a check marker: whether the register state matches the marker. A
// difference (or a malformed marker) turns the marker path off for the rest of the session.
void NoteCheckMarker(bool equal, uint32_t sequence, uint32_t reg_entry, uint32_t in_registers,
                                uint32_t in_marker);

/*
 * Phase 2b of the Direct3D-level renderer: the draw record inside the marker.
 * Each Draw* stores its RegisterDraw before the original (NoteDraw), and its FlushState runs inside
 * it. With 2b that FlushState's marker carries the record, and the DRAW_INDX that follows in the ring, if
 * it accepts it (type, count and shaders, as usual), uses it without the queue or MatchDraw's
 * lookup; otherwise it looks it up as usual. The draw mode goes in the high nibble of the marker's mode
 * byte (the low one is the registers': 0 = marker without segments) and, if it is not 0,
 * kWordsDraw words follow right after the sequence: function, VS, PS, the four arguments and the
 * D3D shadow snapshot (high and low).
 */
constexpr uint32_t kDrawApply = 2;    // the ring uses this record and does no lookup
constexpr uint32_t kDrawCheck = 3;  // the record also goes through the queue: the ring looks up and compares
constexpr uint32_t kWordsDraw = 9;
// From FlushState (nfsc_d3d_registers_native.cpp): takes the record of the current Draw*, if there is one.
bool TakeDrawInProgress(RegisterDraw& reg_entry);
// For the record just taken: 0 = through the queue; kDrawApply or kDrawCheck = in the marker.
uint32_t DecideModeDraw();
// After writing (or not) the marker: to the queue if it does not go in the marker, or if it goes in check mode.
void DeliverDraw(const RegisterDraw& reg_entry, uint32_t mode, bool in_marker);
// From the Draw* hooks, after the original: if its FlushState did not take the record, to the queue.
void FinishDraw();
// Ring thread only, with a marker record the draw accepts: whether it gives the same shaders as the usual lookup.
// what: 1 the lookup's record gives different ones, 3 the lookup finds nothing and the identity gives different
// ones.
void NoteCheckDraw(bool equal, uint32_t what, const RegisterDraw* lookup, const RegisterDraw* marker);

/*
 * Shadow map vegetation filtered on the game side (nfsc_d3d_game_vegetation).
 * See DecideVegetation in nfsc_native_hooks.cpp. Every DrawVertices and DrawIndexedVertices carries the game's
 * verdict in its record (RegisterDraw::vegetation; in the phase 2b marker, in bits 8-23 of the function word),
 * and the ring compares it in Draw with its own (VerdictVegetation in nfsc_native_draws.cpp).
 */
constexpr uint16_t kVegThereIs = 1u << 0;       // there is a verdict: the game has looked at this Draw*
constexpr uint16_t kVegYes = 1u << 1;        // Draw's criterion, with the D3D mirror, says vegetation
constexpr uint16_t kVegOcclusion = 1u << 2;  // a D3D occlusion query is open
constexpr uint16_t kVegSettings = 1u << 3;   // nfsc_native_alpha_only_ps (not alternating) and nfsc_shadows_without_vegetation
constexpr uint16_t kVegBlock = 1u << 4;    // inside a D3D block (tiling, ZPass...: dev+0x28C0 & 0x3F)
constexpr uint16_t kVegTargets = 1u << 5;  // render targets still to be flushed (group 0x2000 dirty in the mirror)
constexpr uint16_t kVegWouldSkip = 1u << 6;  // the game would skip it: yes, settings, none of the above, count 1..65535
constexpr uint16_t kVegSample = 1u << 7;   // applying phase: sent anyway so the ring checks it
constexpr uint32_t kVegReason = 8;          // bits 8-11: why the game says no (kVegReason*)
constexpr uint16_t kVegReasonWithoutShaders = 1;    // no PS or no VS bound
constexpr uint16_t kVegReasonUnknown = 2;  // PS or VS not in the library
constexpr uint16_t kVegReasonMode = 3;          // the effective EDRAM mode is not 4 (color and depth)
constexpr uint16_t kVegReasonColor = 4;         // writes color
constexpr uint16_t kVegReasonWithoutDiscard = 5;   // no alpha test, no kill and no depth in the PS

// What the ring sees for that draw, for the guard and the DIFFERENCE line.
struct DetailVegetation {
  uint32_t mode = 0;        // RB_MODECONTROL
  uint32_t mask = 0;     // RB_COLOR_MASK
  uint32_t control = 0;     // RB_COLORCONTROL
  int32_t vs = -1;          // number in the library (-1: none)
  int32_t ps = -1;
  uint32_t outputs = 0;     // of the PS
  bool discards = false;    // of the PS
  bool structure = false;  // the criterion without the settings or the occlusion
  bool settings = false;     // the two discard settings, as Draw reads them
  bool occlusion = false;    // occlusion query open on the ring
  bool early = false;    // only on a model error: what Draw's early discard decided
};

// Game thread, in the Draw* hooks (nfsmw-nx's D3D trace; stubbed in Carbon), before the original: the kVeg* flags for the record (0
// if it does not look at this Draw*). With skip = true the original is not called and nothing is recorded: the
// Draw* does not exist for the ring.
uint16_t DecideVegetation(FunctionDraw function, uint8_t* base, uint32_t vulkan_device, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip);
// Game thread: IDirect3DQuery9::Issue (8258F810), before the original (query and flags: 2 BEGIN, 1 END).
void NoteQueryD3D(const uint8_t* base, uint32_t query, uint32_t flags);
// Ring thread (UseRegisterOfDraw): the record's flags if the draw uses its shaders; otherwise 0.
uint16_t IdentityForVegetation(uint16_t flags, bool con_sus_shaders);
// Ring thread (Draw): the ring's verdict against the game's.
void NoteVegetationRing(uint16_t flags, const DetailVegetation& ring);
// Ring thread (Draw): the computed verdict does not match the early discard Draw made.
void NoteVegetationModel(uint16_t flags, const DetailVegetation& ring);

struct StatisticsHooks {
  uint64_t created_vs = 0;
  uint64_t known_vs = 0;
  uint64_t created_ps = 0;
  uint64_t known_ps = 0;
  uint64_t lost_count = 0;  // records that did not fit in the queue
};

// The native system enables them after loading the library; nullptr disables them.
// While disabled the hooks only cost an atomic load.
void ActivateHooks(ShadersNative* shaders);
// The library that ActivateHooks enabled (nullptr if none), from any thread. For pipeline prewarming
// (nfsc_native_draws.cpp).
const ShadersNative* LibraryActive();

// From the Draw* hooks (nfsmw-nx's D3D trace; stubbed in Carbon), before the original.
// vegetation = the kVeg* flags from DecideVegetation for the record (0 = no verdict).
void NoteDraw(FunctionDraw function, const uint8_t* base, uint32_t vulkan_device, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation = 0);

// Ring thread only.
bool TakeDraw(RegisterDraw& reg_entry);
const EntryShader* ShaderOfObject(uint32_t object);  // nullptr if unknown
uint64_t GenerationObjects();  // changes with every shader created
StatisticsHooks StatisticsOfHooks();

}  // namespace nfsc::native
