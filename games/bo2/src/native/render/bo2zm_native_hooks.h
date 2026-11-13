// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// What the command ring does not say about each draw, taken on the game thread instead.
//
// The vertex shader microcode reaches the ring already patched by D3D: fetches reordered, swizzles taken from the
// vertex declaration, and outputs the pixel shader does not read nulled out. Looking it up in the shader library by
// its contents is therefore not reliable. The exact identity is known on the game thread:
//   - The shader constructors (sub_8259BC90 PS, sub_8259C038 VS) receive the original container and
//     return the object: object -> container.
//   - Each D3D Draw* reads the bound VS and PS from the device and leaves a record in a queue. The ring
//     sink pairs each draw with its record, in the same order, checking primitive type and count.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace bo2zm::native {

class NativeShaders;
struct ShaderInput;

enum class DrawFunction : uint8_t {
  kVertices,      // DrawVertices(device, type, start, count)
  kIndexed,     // DrawIndexedVertices(device, type, base, start, count)
  kVerticesUP,    // DrawVerticesUP(device, type, count, data, stride)
  kIndexedUP,   // DrawIndexedVerticesUP: argument order unconfirmed
};

struct DrawRecord {
  uint32_t vs = 0;        // bound vertex shader object
  uint32_t ps = 0;        // bound pixel shader object
  uint32_t args[4] = {};  // r4..r7 of the call
  DrawFunction function = DrawFunction::kVertices;
  uint16_t vegetation = 0;  // the game's vegetation verdict (kVeg* flags; 0 = none)
  uint64_t shadow = 0;    // sequence of its mirror snapshot (0 = none), see MirrorSnapshot
};

/*
 * The D3D shadow: checking the device's register mirror against the ring.
 *
 * The game's D3D keeps a copy (mirror) of the Xenos registers in the device, and FlushState (825A40C0) dumps it to
 * the ring group by group as they become dirty. If that mirror holds everything a draw needs, a renderer could read it
 * directly instead of parsing the ~35 packets of each draw. This checks that without changing anything: on 1 in 64
 * Draw* calls the mirror is snapshotted, and when the ring thread pairs that draw it compares the snapshot, register
 * by register, with what it read from the packets (the "D3D shadow" report line).
 */
constexpr uint32_t kGroupsMirror = 9;  // 0x2000..0x2380 in steps of 0x80, and 0x4900 (booleans)
struct MirrorSnapshot {
  uint64_t mask[kGroupsMirror] = {};         // registers of each group that D3D handles (learned)
  uint32_t base_register[kGroupsMirror] = {};
  uint32_t state[kGroupsMirror][64] = {};      // state[g][i] = register base_register[g] + i
  uint32_t fetch[192] = {};                     // 0x4800.. (+0x480 of the device)
  uint32_t constants[2048] = {};               // 0x4000.. VS (+0x780) and 0x4400.. PS (+0x1780)
  // For the report on the constants that differ: the mirror's own constants and where each word of `constants`
  // came from (kFrom*).
  uint32_t constants_mirror[2048] = {};
  uint8_t constants_from[2048] = {};
};
// kFromUnknown: a word the device's stream has not given the GPU since its buffer began (the snapshot shows the
// mirror's; on the GPU it is what ran before).
enum : uint8_t { kFromMirror = 0, kFromLiteral = 1, kFromGame = 2, kFromBuffer = 3, kFromUnknown = 4 };
// From the register dump hook (825A2AA0): which registers of which group, and where their mirror is.
void LearnGroupMirror(uint32_t register_base, uint64_t mask, uint32_t displacement);
// Ring thread only. false if the snapshot has already been reused for another draw.
bool ReadSnapshot(uint64_t sequence, MirrorSnapshot& output);

/*
 * FlushState's compound marker. See bo2zm_d3d_marker.cpp.
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
constexpr uint32_t kMarkerMagic = 0x425A4D00;  // "BZM" and the mode in the low byte (draw modes: see below)
constexpr uint32_t kMarkerApply = 1;
constexpr uint32_t kMarkerCheck = 2;

// A marker segment must fit entirely within one of the groups FlushState dumps: 0x2000+16, 0x2100+21,
// 0x2180+5, 0x2200+12, 0x2280+21, 0x2300+38, 0x2380+8, the VS (0x4000) and PS (0x4400) constants, the
// fetches (0x4800+192) and the booleans and loops (0x4900+40). That way it never touches a register with
// side effects in the ring (COHER_STATUS_HOST, SCRATCH, the gamma ramp: all below 0x2000) or mixes
// classes.
inline bool DumpSpan(uint32_t reg_index, uint32_t count) {
  static constexpr uint8_t kSize2000[8] = {16, 0, 21, 5, 12, 21, 38, 8};  // per 0x80 block (0x2080 is not included)
  if (count == 0) {
    return false;
  }
  if (reg_index >= 0x2000 && reg_index < 0x2400) {
    return (reg_index & 0x7Fu) + count <= kSize2000[(reg_index - 0x2000) >> 7];
  }
  if (reg_index >= 0x4000 && reg_index < 0x4800) {
    return reg_index + count <= (reg_index < 0x4400 ? 0x4400u : 0x4800u);
  }
  if (reg_index >= 0x4800 && reg_index < 0x48C0) {
    return reg_index + count <= 0x48C0;
  }
  if (reg_index >= 0x4900 && reg_index < 0x4928) {
    return reg_index + count <= 0x4928;
  }
  return false;
}

// The native system turns it on when the ring thread starts and off at shutdown. When off, FlushState
// always takes the game's path: with the Xenos emulation nobody would understand the marker.
void ActivateConsumerMarkers(bool active);
// Every CP_RB_WPTR write (the game publishing ring words to the ring thread), counted by the native system. BO2's
// marker producer (games/bo2/src/bo2zm_d3d_marker.cpp) only extends a marker it wrote if nothing has been published since.
inline std::atomic<uint32_t> g_publications_ring{0};
// Ring thread only, when reading a check marker: whether the register state matches the marker. A
// difference (or a malformed marker) turns the marker path off for the rest of the session.
void AnnotateCheckMarker(bool equal, uint32_t sequence, uint32_t reg_index, uint32_t in_registers,
                                uint32_t in_marker);

/*
 * The draw record inside the marker.
 *
 * Each Draw* stores its DrawRecord before calling the original (AnnotateDraw), and its FlushState runs inside that
 * call. That FlushState's marker then carries the record, and the DRAW_INDX that follows it in the ring uses the
 * record directly if it matches (type, count and shaders, as usual), without the queue or PairDraw's lookup; otherwise
 * it is looked up as before. The draw mode goes in the high nibble of the marker's mode byte (the low nibble is the
 * registers' mode; 0 means a marker without segments) and, when it is not 0, kWordsDraw words follow right after the
 * sequence: the function, the VS, the PS, the four arguments and the D3D shadow snapshot (high and low words).
 */
constexpr uint32_t kDrawApply = 2;    // the ring uses this record and does no lookup
constexpr uint32_t kDrawCheck = 3;  // the record also goes through the queue: the ring looks up and compares
constexpr uint32_t kWordsDraw = 9;
// From FlushState (bo2zm_d3d_marker.cpp): takes the record of the current Draw*, if there is one.
bool TakeDrawInOngoing(DrawRecord& reg_index);
// For the record just taken: 0 = through the queue; kDrawApply or kDrawCheck = in the marker.
uint32_t DecideModeDraw();
// After writing (or not) the marker: to the queue if it does not go in the marker, or if it goes in check mode.
void DeliverDraw(const DrawRecord& reg_index, uint32_t mode, bool in_marker);
// From the Draw* hooks, after the original: if its FlushState did not take the record, to the queue.
void FinishDraw();

// BO2 records its world draws into command buffers on the job workers, and the Main thread runs those buffers later
// in a different order, so the queue (AnnotateDraw and PairDraw's lookup) cannot pair them. Instead each Draw* hook
// (bo2zm_d3d_trace.cpp) writes its record into the command stream itself, before the original, as a record-only
// marker (BeforeDraw, bo2zm_d3d_marker.cpp). The queue stays empty, so in check mode the ring compares the record with
// its own identity. RecordForStream, from any game thread, gives the record and its mode (kDrawCheck or kDrawApply);
// false means no record (draw records off, the library not loaded, or no device).
bool RecordForStream(DrawFunction function, const uint8_t* base, uint32_t device, uint32_t r4, uint32_t r5,
                     uint32_t r6, uint32_t r7, DrawRecord& record, uint32_t& mode);
// From the wrapper of sub_82174868 (bo2zm_d3d_trace.cpp), after it returns: a SET_SHADER_CONSTANTS of `vectors`
// float4 values that the calling game code writes at `pointer` (the register index is the word before). Kept per
// thread; the device's next record copies the values into the device's game-written constants, which the D3D shadow
// lays over the mirror's, because the mirror never sees them. Does nothing unless bo2zm_native_d3d_shadow is on.
void NoteGameConstants(const uint8_t* base, uint32_t device, uint32_t pointer, uint32_t vectors);
// From the wrappers of the D3D calls that flush the device's dirty state first (the draws, the resolves, End and
// Run; bo2zm_d3d_trace.cpp), before they run. The device's stream constants (what its stream has given the GPU, which
// the shadow snapshots lay over the mirror) take, in stream order: the game-written constants noted for `device`, then
// the mirror's words under the dirty bits of the VS and PS float constants (device+0, device+8), which the flush sends
// after them, then the bound shaders' literals. Does nothing unless bo2zm_native_d3d_shadow is on.
void BeforeFlush(const uint8_t* base, uint32_t device);
// The same for the command buffers (BeginCommandBuffer sub_8216D5C8 before it runs, EndCommandBuffer sub_8216DB38 after
// BeforeFlush, RunCommandBuffer sub_8216E110 after it returns): the constants a buffer leaves on the GPU go to the
// device that runs it.
void BeginBuffer(uint32_t device, uint32_t buffer);
void EndBuffer(uint32_t device);
void AfterRun(uint32_t device, uint32_t buffer);
// From the wrapper of DrawIndexedVertices after the original returns: the snapshot its record reserved
// (RecordForStream), taken now, after the draw's flush has bound the shaders (which sets the mirror's SQ_PROGRAM_CNTL
// and its neighbours, 0x2180-0x2182, as it sends them).
void AfterDraw(const uint8_t* base);
// Their counts since the last call, for the D3D shadow's report.
std::string StreamConstantsReport();
// Whether bo2zm_native_d3d_shadow is on.
bool D3DShadowOn();
// Records RecordForStream gave so far, and how many of them in check mode (for the marker's report).
void StreamRecordCounts(uint64_t& records, uint64_t& checking);
// The Draw* hook's part (bo2zm_d3d_marker.cpp): the record marker at the stream's write pointer, if it fits.
void BeforeDraw(uint8_t* base, uint32_t device, DrawFunction function, uint32_t r4, uint32_t r5, uint32_t r6,
                uint32_t r7);
// Ring thread only, with a marker record the draw accepts: whether it gives the same shaders as the usual lookup.
// kind: 1 = the lookup's record gives different shaders, 3 = the lookup finds nothing and the identity gives
// different shaders.
void AnnotateCheckDraw(bool equal, uint32_t kind, const DrawRecord* lookup, const DrawRecord* marker);

/*
 * Shadow map vegetation filtered on the game side (bo2zm_d3d_game_vegetation).
 *
 * See DecideVegetation in bo2zm_native_hooks.cpp. Every DrawVertices and DrawIndexedVertices carries the game's
 * verdict in its record (DrawRecord::vegetation; in the draw record marker, in bits 8-23 of the function word), and
 * the ring compares it in Draw with its own (VerdictVegetation in bo2zm_native_draws.cpp).
 */
constexpr uint16_t kVegHas = 1u << 0;       // there is a verdict: the game has looked at this Draw*
constexpr uint16_t kVegYes = 1u << 1;        // Draw's criterion, with the D3D mirror, says vegetation
constexpr uint16_t kVegOcclusion = 1u << 2;  // a D3D occlusion query is open
constexpr uint16_t kVegSettings = 1u << 3;   // bo2zm_native_ps_alpha_only (not alternating) and bo2zm_shadows_without_vegetation
constexpr uint16_t kVegBlock = 1u << 4;    // inside a D3D block (tiling, ZPass...: dev+0x28C0 & 0x3F)
constexpr uint16_t kVegTargets = 1u << 5;  // render targets still to be flushed (group 0x2000 dirty in the mirror)
constexpr uint16_t kVegWouldSkip = 1u << 6;  // the game would skip it: yes, settings, none of the above, count 1..65535
constexpr uint16_t kVegSample = 1u << 7;   // applying phase: sent anyway so the ring checks it
constexpr uint32_t kVegReason = 8;          // bits 8-11: why the game says no (kVegReason*)
constexpr uint16_t kVegReasonNoShaders = 1;    // no PS or no VS bound
constexpr uint16_t kVegReasonUnknown = 2;  // PS or VS not in the library
constexpr uint16_t kVegReasonMode = 3;          // the effective EDRAM mode is not 4 (color and depth)
constexpr uint16_t kVegReasonColor = 4;         // writes color
constexpr uint16_t kVegReasonNoDiscard = 5;   // no alpha test, no kill and no depth in the PS

// What the ring sees for that draw, for the guard and the MISMATCH line.
struct VegetationDetail {
  uint32_t mode = 0;        // RB_MODECONTROL
  uint32_t mask = 0;     // RB_COLOR_MASK
  uint32_t control = 0;     // RB_COLORCONTROL
  int32_t vs = -1;          // number in the library (-1: none)
  int32_t ps = -1;
  uint32_t outputs = 0;     // of the PS
  bool discards = false;    // of the PS
  bool structural = false;  // the criterion without the settings or the occlusion
  bool settings = false;     // the two discard settings, as Draw reads them
  bool occlusion = false;    // occlusion query open on the ring
  bool early = false;    // only on a model error: what Draw's early discard decided
};

// Game thread, in the Draw* hooks (bo2zm_d3d_trace.cpp), before the original: the kVeg* flags for the record (0
// if it does not look at this Draw*). With skip = true the original is not called and nothing is recorded: the
// Draw* does not exist for the ring.
uint16_t DecideVegetation(DrawFunction function, uint8_t* base, uint32_t vulkan_device, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip);
// Game thread: IDirect3DQuery9::Issue (8258F810), before the original (query and flags: 2 BEGIN, 1 END).
void AnnotateQueryD3D(const uint8_t* base, uint32_t query, uint32_t flags);
// Ring thread (UseDrawRegister): the record's flags if the draw uses its shaders; otherwise 0.
uint16_t IdentityForVegetation(uint16_t flags, bool with_shaders);
// Ring thread (Draw): the ring's verdict against the game's.
void AnnotateVegetationRing(uint16_t flags, const VegetationDetail& ring);
// Ring thread (Draw): the computed verdict does not match the early discard Draw made.
void AnnotateVegetationModel(uint16_t flags, const VegetationDetail& ring);

struct HookStats {
  uint64_t created_vs = 0;
  uint64_t known_vs = 0;
  uint64_t created_ps = 0;
  uint64_t known_ps = 0;
  uint64_t lost = 0;  // records that did not fit in the queue
};

// The native system enables them after loading the library; nullptr disables them.
// While disabled the hooks only cost an atomic load.
void ActivateHooks(NativeShaders* shaders);
// From BO2's shader registration hooks (sub_82169BF8 VS, sub_821698D8 PS; bo2zm_d3d_marker.cpp), before the
// original: which library entry the object's container and physical part are, for ObjectShader. Counted in the shader
// report line as "shaders created VS=.. (known ..)".
void RegisterShaderObject(const uint8_t* base, uint32_t object, uint32_t physical, bool vertices);
// The library that ActivateHooks enabled (nullptr if none), from any thread. For pipeline prewarming
// (bo2zm_native_draws.cpp).
const NativeShaders* LibraryActive();

// From the Draw* hooks (bo2zm_d3d_trace.cpp), before the original.
// vegetation = the kVeg* flags from DecideVegetation for the record (0 = no verdict).
void AnnotateDraw(DrawFunction function, const uint8_t* base, uint32_t vulkan_device, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation = 0);

// Ring thread only.
bool ExtractDraw(DrawRecord& reg_index);
const ShaderInput* ObjectShader(uint32_t object);  // nullptr if not known
uint64_t GenerationObjects();  // changes with every shader created
HookStats HooksStats();

}  // namespace bo2zm::native
