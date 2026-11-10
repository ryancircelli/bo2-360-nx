// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Draw identity and register state taken on the game thread (see bo2zm_native_hooks.h).

#include "bo2zm_native_hooks.h"

#include "bo2zm_native_shaders.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include "bo2zm_deferred_report.h"  // deferred reports

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>
#include <array>  // bo2zm_d3d_game_vegetation
#include <string>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(bo2zm_native_d3d_shadow, false, "BO2",
                    "Native renderer (Direct3D-level renderer): on 1 in 64 draws it snapshots the "
                    "device's register mirror and the ring compares it with what it reads from the packets ('D3D "
                    "shadow' line). Does not change what is drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Draw records inside FlushState's marker: see g_draw_in_ongoing and AnnotateDraw below.
REXCVAR_DEFINE_BOOL(bo2zm_d3d_marker_record, false, "BO2",
                    "Native renderer, Direct3D-level renderer: each DrawIndexedVertices writes its "
                    "record (VS, PS, arguments) into the command stream and the ring draws with it instead of its own "
                    "shader identity. Starts by checking against that identity and turns itself off at the first "
                    "disagreement. Off by default again: Origins binds pixel shaders whose program the "
                    "ring loads differently (the record's PS is not the IM_LOAD's: case 3, PC runs), and a variant "
                    "that first appears after the checks could draw a few frames with the record's shader; the "
                    "records measured no gain. false = the ring identifies every draw itself")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Shadow map vegetation filtered on the game thread (see DecideVegetation).
REXCVAR_DEFINE_BOOL(bo2zm_d3d_game_vegetation, true, "BO2",
                    "Native renderer: the DrawVertices and DrawIndexedVertices the ring would drop "
                    "as shadow-map vegetation (no color, with an alpha test or discard) are skipped whole on the game "
                    "thread: no FlushState, DRAW_INDX or record. Starts by observing (the ring checks each draw's "
                    "verdict) and turns itself off at the first disagreement. false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The settings of the ring's vegetation discard (bo2zm_native_draws.cpp): the game side checks the
// same ones.
REXCVAR_DECLARE(bool, bo2zm_native_ps_alpha_only);
REXCVAR_DECLARE(int32_t, bo2zm_native_ps_alpha_only_alternate_s);
REXCVAR_DECLARE(bool, bo2zm_shadows_without_vegetation);

namespace bo2zm::native {
namespace {

// Where SetVertexShader and SetPixelShader store the bound shader inside the device object. BO2's SetVertexShader
// (sub_82169DE0) stores r4 at device+13072 (0x82169E80) and its SetPixelShader (sub_82169A00) at device+13068
// (0x82169A84).
constexpr uint32_t kDeviceVs = 13072;
constexpr uint32_t kDevicePs = 13068;

// One producer (the thread using D3D) and one consumer (the ring thread).
constexpr uint32_t kSizeQueue = uint32_t(1) << 16;

std::atomic<NativeShaders*> g_shaders{nullptr};

std::mutex g_mutex;
std::unordered_map<uint32_t, const ShaderInput*> g_objects;
// Shader objects registered by BO2 (RegisterShaderObject) and their library numbers, turned into g_objects entries
// the first time the ring asks for one (ObjectShader).
std::unordered_map<uint32_t, uint32_t> g_object_numbers;
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_created_vs{0};
std::atomic<uint64_t> g_known_vs{0};
std::atomic<uint64_t> g_created_ps{0};
std::atomic<uint64_t> g_known_ps{0};
std::atomic<uint32_t> g_warnings{0};

DrawRecord g_queue[kSizeQueue];
std::atomic<uint32_t> g_write{0};
std::atomic<uint32_t> g_read{0};
std::atomic<uint64_t> g_lost{0};

uint32_t ReadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The D3D shadow's snapshots (see MirrorSnapshot in the header).
constexpr uint32_t kShotEvery = 64;            // 1 in 64 draws
constexpr uint32_t kShots = 256;              // snapshots in flight between the game thread and the ring thread
constexpr uint32_t kDeviceFetch = 0x480;
constexpr uint32_t kDeviceConstantsVs = 0x780;
constexpr uint32_t kDeviceConstantsPs = 0x1780;

std::atomic<uint64_t> g_mask_group[kGroupsMirror];
std::atomic<uint32_t> g_offset_group[kGroupsMirror];  // 0 = not seen yet

struct Shot {
  std::atomic<uint64_t> sequence{0};  // 0 while being written
  MirrorSnapshot data;
};
Shot g_shots[kShots];
// Atomic, since BO2 takes snapshots on every thread that records draws (RecordForStream).
std::atomic<uint64_t> g_next_shot{1};
uint32_t g_counter_shots = 0;  // AnnotateDraw's thread only

// As REX_RAW_ADDR in the generated code: on Win32 and macOS arm64 the area from 0xE0000000 is 0x1000 higher.
inline const uint8_t* Guest(const uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}

// Per device, the shader constants its stream has given the GPU as far as the shadow can tell, laid over the mirror's
// constants in the shadow snapshots. They come from four places: what the game code writes straight into the stream
// (sub_82174868's SET_SHADER_CONSTANTS, filled in by the caller, which the mirror never sees), the mirror's own words
// when a flush sends them, the bound shaders' literals, and, after RunCommandBuffer, whatever the command buffer
// left behind.
struct StreamConstants {
  uint32_t value[2048] = {};  // 0x4000.. as MirrorSnapshot::constants
  uint64_t known[32] = {};
  uint8_t from[2048] = {};  // kFrom*
  void Set(uint32_t r, uint32_t v, uint8_t source) {
    value[r] = v;
    known[r >> 6] |= uint64_t(1) << (r & 63);
    from[r] = source;
  }
};
std::mutex g_stream_constants_mutex;
std::unordered_map<uint32_t, std::unique_ptr<StreamConstants>> g_stream_constants;  // by device
std::unordered_map<uint32_t, std::unique_ptr<StreamConstants>> g_buffer_constants;  // by command buffer, at its End
std::unordered_map<uint32_t, uint32_t> g_buffer_of_device;                         // since its Begin
struct PendingConstants {
  uint32_t device, pointer, index, words;
};
thread_local std::vector<PendingConstants> t_pending_constants;  // written by the caller before its next draw
// For the D3D shadow's report (StreamConstantsReport): the command buffer hand-overs.
std::atomic<uint64_t> g_stream_begins{0}, g_stream_ends{0}, g_stream_ends_unknown{0}, g_stream_runs{0},
    g_stream_runs_unknown{0};

// Under g_stream_constants_mutex.
StreamConstants& StreamOf(std::unordered_map<uint32_t, std::unique_ptr<StreamConstants>>& map, uint32_t key) {
  std::unique_ptr<StreamConstants>& slot = map[key];
  if (!slot) {
    slot = std::make_unique<StreamConstants>();
  }
  return *slot;
}

// The values of this thread's noted constants for `device`, read now (the device's next flush: the caller has filled
// them). Under g_stream_constants_mutex.
void ApplyGameConstants(const uint8_t* base, uint32_t device, StreamConstants& slot) {
  std::vector<PendingConstants>& pending = t_pending_constants;
  size_t keep = 0;
  for (const PendingConstants& p : pending) {
    if (p.device != device) {
      pending[keep++] = p;
      continue;
    }
    const uint8_t* source = Guest(base, p.pointer);
    for (uint32_t i = 0; i < p.words; ++i) {
      slot.Set(p.index - 0x4000 + i, ReadBE(source + 4 * i), kFromGame);
    }
  }
  pending.resize(keep);
}

// A shader's literal constants, as sub_821760A8 loads them when the shader is bound (one LOAD_ALU_CONSTANT per entry,
// from the shader's physical block): the table's offset at [container+0x14] (`lwz r11,20(r4)`), its size in bytes
// at table+16 and its 8-byte entries from table+20, each {u16 float4 register, u16 dwords, u32 offset into the
// physical part}; the packet's type-and-index word is 4 x that u16 (`rlwinm r10,r26,2,0,29`), so its top bits pick
// the constant file as the ring's PM4_LOAD_ALU_CONSTANT reads it (0 ALU, 1 fetch; bool and loop are not followed).
// `use(type, first word, source, words)` for each entry.
template <typename Use>
void ForEachLiteral(const uint8_t* base, uint32_t container, uint32_t physical, Use use) {
  if (!container || !physical) {
    return;
  }
  const uint32_t table = ReadBE(Guest(base, container + 20));
  if (!table) {
    return;
  }
  const uint32_t start = container + table + 20;
  const uint32_t bytes = ReadBE(Guest(base, container + table + 16));
  if (bytes > 4096) {
    return;
  }
  for (uint32_t p = start; p + 8 <= start + bytes; p += 8) {
    const uint8_t* e = Guest(base, p);
    const uint32_t reg = (uint32_t(e[0]) << 8) | e[1];
    const uint32_t count = (uint32_t(e[2]) << 8) | e[3];
    if (!count) {
      break;
    }
    use((4 * reg) >> 16, (4 * reg) & 0x7FF, Guest(base, physical + ReadBE(e + 4)), count);
  }
}
// VS: container at object+0x368, physical part at [object+0x20]; PS: container at object+0x28, physical part at
// [object+0x18] (BO2's shader objects).
template <typename Use>
void ForEachLiteralOf(const uint8_t* base, uint32_t vs, uint32_t ps, Use use) {
  if (vs) {
    ForEachLiteral(base, vs + 0x368, ReadBE(Guest(base, vs + 0x20)), use);
  }
  if (ps) {
    ForEachLiteral(base, ps + 0x28, ReadBE(Guest(base, ps + 0x18)), use);
  }
}

// Copies the device's register mirror into the next snapshot slot (MirrorSnapshot) and returns its sequence, for the
// record. The constants have the device's stream constants (StreamConstants) laid over the mirror's, and the fetch
// constants have the bound shaders' fetch-constant literals laid over them.
// `sequence`: one reserved earlier (AfterDraw); 0 = a new one.
uint64_t TakeShot(const uint8_t* base, uint32_t device, uint32_t vs = 0, uint32_t ps = 0, uint64_t sequence = 0) {
  if (!sequence) {
    sequence = g_next_shot.fetch_add(1, std::memory_order_relaxed);
  }
  Shot& shot = g_shots[sequence % kShots];
  shot.sequence.store(0, std::memory_order_release);  // being written
  MirrorSnapshot& d = shot.data;
  const uint8_t* disp = base + device;
  for (uint32_t g = 0; g < kGroupsMirror; ++g) {
    const uint32_t off = g_offset_group[g].load(std::memory_order_relaxed);
    d.mask[g] = off ? g_mask_group[g].load(std::memory_order_relaxed) : 0;
    d.base_register[g] = g < 8 ? 0x2000 + g * 0x80 : 0x4900;
    if (!off) {
      continue;
    }
    for (uint32_t i = 0; i < 64; ++i) {
      if ((d.mask[g] >> (63 - i)) & 1) {
        d.state[g][i] = ReadBE(disp + off + i * 4);
      }
    }
  }
  for (uint32_t i = 0; i < 192; ++i) {
    d.fetch[i] = ReadBE(disp + kDeviceFetch + i * 4);
  }
  for (uint32_t i = 0; i < 1024; ++i) {
    d.constants[i] = ReadBE(disp + kDeviceConstantsVs + i * 4);
    d.constants[1024 + i] = ReadBE(disp + kDeviceConstantsPs + i * 4);
  }
  std::memcpy(d.constants_mirror, d.constants, sizeof(d.constants));
  std::memset(d.constants_from, kFromUnknown, sizeof(d.constants_from));
  ForEachLiteralOf(base, vs, ps, [&](uint32_t type, uint32_t first, const uint8_t* source, uint32_t count) {
    for (uint32_t i = 0; type == 1 && i < count && first + i < 192; ++i) {
      d.fetch[first + i] = ReadBE(source + 4 * i);
    }
  });
  {
    std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
    const auto it = g_stream_constants.find(device);
    if (it != g_stream_constants.end()) {
      const StreamConstants& c = *it->second;
      for (uint32_t i = 0; i < 2048; ++i) {
        if ((c.known[i >> 6] >> (i & 63)) & 1) {
          d.constants[i] = c.value[i];
          d.constants_from[i] = c.from[i];
        }
      }
    }
  }
  shot.sequence.store(sequence, std::memory_order_release);
  return sequence;
}

int GroupOf(uint32_t register_base) {
  if (register_base >= 0x2000 && register_base < 0x2400 && (register_base & 0x7F) == 0) {
    return int((register_base - 0x2000) >> 7);
  }
  return register_base == 0x4900 ? 8 : -1;
}

/*
 * The draw record inside the marker.
 *
 * Each Draw* stores its record before calling the original, and its FlushState runs inside the original. The record
 * does not go straight to the queue: it waits in g_draw_in_ongoing, that FlushState takes it (TakeDrawInOngoing) and
 * puts it in its marker, and the DRAW_INDX that follows in the ring uses it without a lookup if it matches. If the
 * marker cannot carry it (no room, or markers off) or nobody takes it, it goes to the queue as usual (DeliverDraw,
 * FinishDraw).
 *
 * The path checks itself. While observing, each record goes both ways (to the queue, and in the marker in check
 * mode); the draw uses the usual lookup and the ring compares the shaders the lookup found with those in the marker's
 * record (CompareDrawMarker). After kChecksDraw matches it switches to the marker alone, and still checks 1 in
 * kCheckDrawEvery. At the first difference it turns itself off for the rest of the session ("[d3d_marker] draw
 * record: MISMATCH" in the log) and records go back to the queue.
 */
constexpr uint64_t kChecksDraw = 20000;
constexpr uint64_t kCheckDrawEvery = 1024;  // power of 2
DrawRecord g_draw_in_ongoing;  // only the thread using D3D (the Draw* calls and their FlushState)
bool g_draw_pending = false;
uint64_t g_turn_draw = 0;
std::atomic<bool> g_draw_off{false};
std::atomic<bool> g_draw_applying{false};
std::atomic<uint64_t> g_draw_equal{0};  // written only by the ring thread
std::atomic<bool> g_draw_different{false};
// The first disagreement, written by the ring thread before g_draw_different (release).
uint32_t g_draw_case = 0;
DrawRecord g_draw_lookup;
DrawRecord g_draw_marker;
bool g_draw_has_lookup = false;
bool g_draw_has_marker = false;
// Report every 10 s (only the thread using D3D).
uint64_t g_i_draws = 0;
uint64_t g_i_in_marker = 0;
uint64_t g_i_checking = 0;
uint64_t g_i_per_queue = 0;
uint64_t g_i_no_flushstate = 0;
int64_t g_i_next_ms = 0;

bool DrawInMarker() {
  static const bool active = REXCVAR_GET(bo2zm_d3d_marker_record);
  return active && !g_draw_off.load(std::memory_order_relaxed);
}

// The usual path: to the ring's queue (if it is full, the record is dropped and counted, as before).
void EnqueueDraw(const DrawRecord& reg_index) {
  const uint32_t write = g_write.load(std::memory_order_relaxed);
  const uint32_t next = (write + 1) & (kSizeQueue - 1);
  if (next == g_read.load(std::memory_order_acquire)) {
    g_lost.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_queue[write] = reg_index;
  g_write.store(next, std::memory_order_release);
}

void TurnOffDraw() {
  // An exchange, since BO2 calls it from several game threads (RecordForStream): only one of them logs.
  if (g_draw_off.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  static constexpr const char* kCaseText[] = {
      "?", "the records give different shaders", "?",
      "the lookup finds no record and the ring's identity gives other shaders"};
  const DrawRecord& b = g_draw_lookup;
  const DrawRecord& m = g_draw_marker;
  REXLOG_ERROR("[d3d_marker] draw record: MISMATCH with the ring's lookup ({}): lookup {} (function {} type {} VS "
               "{:08X} PS {:08X}), marker {} (function {} type {} VS {:08X} PS {:08X}). Draw records OFF for the rest of "
               "the session: the records go back to the queue",
               kCaseText[g_draw_case < 4 ? g_draw_case : 0], g_draw_has_lookup ? "with a record" : "without a record",
               int(b.function), b.args[0], b.vs, b.ps, g_draw_has_marker ? "with a record" : "without a record",
               int(m.function), m.args[0], m.vs, m.ps);
}

void ReportDraw() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now < g_i_next_ms) {
    return;
  }
  const bool first = g_i_next_ms == 0;
  g_i_next_ms = now + 10000;
  if (!first) {
    BO2ZM_REPORT_DEFERRED("[d3d_marker] draw records, last 10 s: {} in the marker ({} checking), {} through "
                "the queue, {} without FlushState | phase {} | checked on the ring {} of {}",
                g_i_in_marker, g_i_checking, g_i_per_queue, g_i_no_flushstate,
                g_draw_off.load(std::memory_order_relaxed)     ? "OFF"
                : g_draw_applying.load(std::memory_order_relaxed) ? "applying"
                                                                     : "observing",
                g_draw_equal.load(std::memory_order_relaxed), kChecksDraw);
  }
  g_i_in_marker = g_i_checking = g_i_per_queue = g_i_no_flushstate = 0;
}

}  // namespace


namespace hooks_detail {

const ShaderInput* IdentifyCreation(const uint8_t* base, uint32_t address, bool vertices) {
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !address || address > UINT32_MAX - 24) {
    return nullptr;
  }
  const uint8_t* p = base + address;
  if (ReadBE(p) != (vertices ? 0x102A0E01u : 0x102A0E00u)) {
    return nullptr;
  }
  const uint64_t total = uint64_t(ReadBE(p + 4)) + ReadBE(p + 8);
  if (total < 24 || total > 65536 || uint64_t(address) + total > (uint64_t(1) << 32)) {
    return nullptr;
  }
  return shaders->IdentifyContainer(std::span<const uint8_t>(p, size_t(total)));
}

void RememberCreation(uint32_t object, const ShaderInput* input, bool vertices) {
  if (!g_shaders.load(std::memory_order_acquire)) {
    return;
  }
  (vertices ? g_created_vs : g_created_ps).fetch_add(1, std::memory_order_relaxed);
  if (input) {
    (vertices ? g_known_vs : g_known_ps).fetch_add(1, std::memory_order_relaxed);
  } else if (g_warnings.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[native] C5b: {} shader created at {:08X} that is not in the library",
                vertices ? "vertex" : "pixel", object);
  }
  if (!object) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  // The address of a freed object gets reused: an unknown shader clears any earlier association with it.
  if (input) {
    g_objects.insert_or_assign(object, input);
  } else {
    g_objects.erase(object);
  }
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace hooks_detail

// see bo2zm_native_hooks.h.
const NativeShaders* LibraryActive() {
  return g_shaders.load(std::memory_order_acquire);
}

void ActivateHooks(NativeShaders* shaders) {
  g_shaders.store(shaders, std::memory_order_release);
  if (!shaders) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_objects.clear();
    g_object_numbers.clear();
    g_generation.fetch_add(1, std::memory_order_acq_rel);
  }
}

void RegisterShaderObject(const uint8_t* base, uint32_t object, uint32_t physical, bool vertices) {
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !object || !physical) {
    return;
  }
  // The library's blob: the container (flags 0x102A11xx, bit 0 for a vertex shader; virtual size at +4, physical
  // size at +8) followed by the physical part, as tools/inspect/ff_shaders.py built the pack.
  // As REX_RAW_ADDR in the generated code: on Win32 and macOS arm64 the area from 0xE0000000 is 0x1000 higher.
  auto host = [base](uint32_t address) {
    return base + address + rex::memory::detail::PhysicalHostOffset(address);
  };
  const uint8_t* c = host(object + (vertices ? 0x368u : 0x28u));
  const uint32_t flags = ReadBE(c);
  const uint32_t size_virtual = ReadBE(c + 4);
  const uint32_t size_physical = ReadBE(c + 8);
  int32_t number = -1;
  if ((flags & 0xFFFFFF00u) == 0x102A1100u && bool(flags & 1) == vertices && size_virtual >= 24 &&
      uint64_t(size_virtual) + size_physical <= 65536) {
    thread_local std::vector<uint8_t> blob;
    blob.assign(c, c + size_virtual);
    const uint8_t* p = host(physical);
    blob.insert(blob.end(), p, p + size_physical);
    number = shaders->NumberOfContainer(blob);
  }
  (vertices ? g_created_vs : g_created_ps).fetch_add(1, std::memory_order_relaxed);
  if (number >= 0) {
    (vertices ? g_known_vs : g_known_ps).fetch_add(1, std::memory_order_relaxed);
  } else if (g_warnings.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[native] C5b: {} shader registered at {:08X} (flags {:08X}) that is not in the library",
                vertices ? "vertex" : "pixel", object, flags);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  // The address of a freed object gets reused: whatever was known for it before goes.
  g_objects.erase(object);
  if (number >= 0) {
    g_object_numbers.insert_or_assign(object, uint32_t(number));
  } else {
    g_object_numbers.erase(object);
  }
  g_generation.fetch_add(1, std::memory_order_acq_rel);
}

void AnnotateDraw(DrawFunction function, const uint8_t* base, uint32_t vulkan_device, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation) {
  if (!vulkan_device || !g_shaders.load(std::memory_order_relaxed)) {
    return;
  }
  DrawRecord reg_index;
  reg_index.vs = ReadBE(base + vulkan_device + kDeviceVs);
  reg_index.ps = ReadBE(base + vulkan_device + kDevicePs);
  reg_index.args[0] = r4;
  reg_index.args[1] = r5;
  reg_index.args[2] = r6;
  reg_index.args[3] = r7;
  reg_index.function = function;
  reg_index.shadow = 0;
  reg_index.vegetation = vegetation;  // bo2zm_d3d_game_vegetation
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  if (shadow && ++g_counter_shots % kShotEvery == 0) {
    reg_index.shadow = TakeShot(base, vulkan_device);
  }
  // With records in the marker, this Draw*'s FlushState decides where the record goes (TakeDrawInOngoing);
  // otherwise it goes to the queue as usual.
  if (DrawInMarker()) {
    if (g_draw_pending) {  // should not happen: the previous Draw* reached neither its FlushState nor FinishDraw
      EnqueueDraw(g_draw_in_ongoing);
      ++g_i_no_flushstate;
    }
    g_draw_in_ongoing = reg_index;
    g_draw_pending = true;
    if ((++g_i_draws & 4095) == 0) {
      ReportDraw();
    }
    return;
  }
  EnqueueDraw(reg_index);
}

bool TakeDrawInOngoing(DrawRecord& reg_index) {
  if (!g_draw_pending) {
    return false;
  }
  g_draw_pending = false;
  reg_index = g_draw_in_ongoing;
  return true;
}

uint32_t DecideModeDraw() {
  if (!DrawInMarker()) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_draw_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffDraw();
    return 0;
  }
  if (!g_draw_applying.load(std::memory_order_relaxed)) {
    const uint64_t equal = g_draw_equal.load(std::memory_order_relaxed);
    if (equal < kChecksDraw) {
      return kDrawCheck;
    }
    g_draw_applying.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marker] draw record: {} draws checked against the ring's lookup, 0 disagreements: the marker now "
                "carries the record and the ring does not look it up (1 in {} is still checked)",
                equal, kCheckDrawEvery);
  }
  return (++g_turn_draw & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
}

void DeliverDraw(const DrawRecord& reg_index, uint32_t mode, bool in_marker) {
  if (!in_marker) {
    ++g_i_per_queue;
    EnqueueDraw(reg_index);
    return;
  }
  ++g_i_in_marker;
  if (mode == kDrawCheck) {  // the ring's lookup has to be able to find it to compare
    ++g_i_checking;
    EnqueueDraw(reg_index);
  }
}

void FinishDraw() {
  if (!g_draw_pending) {
    return;
  }
  g_draw_pending = false;
  ++g_i_no_flushstate;
  EnqueueDraw(g_draw_in_ongoing);
}

namespace {
std::atomic<uint64_t> g_stream_records{0};
std::atomic<uint64_t> g_stream_checking{0};
}  // namespace

struct ShotReserved {
  uint64_t sequence;
  uint32_t device, vs, ps;
};
thread_local ShotReserved t_shot = {};

void AfterDraw(const uint8_t* base) {
  if (t_shot.sequence) {
    TakeShot(base, t_shot.device, t_shot.vs, t_shot.ps, t_shot.sequence);
    t_shot.sequence = 0;
  }
}

bool RecordForStream(DrawFunction function, const uint8_t* base, uint32_t device, uint32_t r4, uint32_t r5,
                     uint32_t r6, uint32_t r7, DrawRecord& record, uint32_t& mode) {
  if (!device || !g_shaders.load(std::memory_order_relaxed) || !DrawInMarker()) {
    return false;
  }
  // As DecideModeDraw, but for any number of game threads: the turn is per thread and the switch to applying is an
  // exchange, so one thread logs it.
  if (g_draw_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffDraw();
    return false;
  }
  thread_local uint64_t turn = 0;
  if (!g_draw_applying.load(std::memory_order_relaxed)) {
    const uint64_t equal = g_draw_equal.load(std::memory_order_relaxed);
    if (equal < kChecksDraw) {
      mode = kDrawCheck;
    } else {
      if (!g_draw_applying.exchange(true, std::memory_order_relaxed)) {
        REXLOG_INFO("[d3d_marker] draw record in the stream: {} draws checked against the ring's identity, 0 "
                    "disagreements: the ring now takes each draw's shaders from its record (1 in {} still checked)",
                    equal, kCheckDrawEvery);
      }
      mode = (++turn & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
    }
  } else {
    mode = (++turn & (kCheckDrawEvery - 1)) == 0 ? kDrawCheck : kDrawApply;
  }
  record = DrawRecord{};
  record.vs = ReadBE(base + device + kDeviceVs);
  record.ps = ReadBE(base + device + kDevicePs);
  record.args[0] = r4;
  record.args[1] = r5;
  record.args[2] = r6;
  record.args[3] = r7;
  record.function = function;
  // 1 in kShotEvery records (per thread) carries a snapshot of the device's register mirror, which the ring compares
  // with what it decoded for that draw (CompareShadow, the "D3D shadow" report line).
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  thread_local uint32_t shots = 0;
  if (shadow) {  // the game-written constants are applied by BeforeFlush, which the wrapper calls first
    if (++shots % kShotEvery == 0) {
      // Taken when the draw call returns (AfterDraw); if the ring gets there first, it counts the snapshot as lost.
      record.shadow = g_next_shot.fetch_add(1, std::memory_order_relaxed);
      t_shot = {record.shadow, device, record.vs, record.ps};
    }
  }
  g_stream_records.fetch_add(1, std::memory_order_relaxed);
  if (mode == kDrawCheck) {
    g_stream_checking.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void NoteGameConstants(const uint8_t* base, uint32_t device, uint32_t pointer, uint32_t vectors) {
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  if (!shadow || !device || pointer < 4 || vectors == 0 || vectors > 512) {
    return;
  }
  // [header][register index][values]: the index word sits just before the data (sub_82174868; the ring reads
  // SET_SHADER_CONSTANTS the same way).
  const uint32_t index = ReadBE(Guest(base, pointer - 4)) & 0xFFFF;
  const uint32_t words = 4 * vectors;
  if (index < 0x4000 || index + words > 0x4800) {
    return;
  }
  std::vector<PendingConstants>& pending = t_pending_constants;
  if (pending.size() >= 256) {  // notes for a device that never draws on this thread again
    pending.erase(pending.begin());
  }
  pending.push_back({device, pointer, index, words});
}

bool D3DShadowOn() {
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  return shadow;
}

void BeforeFlush(const uint8_t* base, uint32_t device) {
  if (!D3DShadowOn() || !device) {
    return;
  }
  const uint8_t* d = base + device;
  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  StreamConstants& c = StreamOf(g_stream_constants, device);
  // In stream order: what the game wrote since the last flush, then the flush itself. sub_821752D0(device, mask, first
  // register, mirror): bit 63 - i sends the mirror's 16 words from first register + 16 i (four float4); then the
  // bound shaders' literals (sub_821760A8, under the shader dirty bits). Taking the literals at every flush matched the
  // ring every time we checked: the "shader literal" count in the report stays at 0.
  ApplyGameConstants(base, device, c);
  const uint64_t dirty[2] = {(uint64_t(ReadBE(d)) << 32) | ReadBE(d + 4),
                             (uint64_t(ReadBE(d + 8)) << 32) | ReadBE(d + 12)};
  for (uint32_t kind = 0; kind < 2; ++kind) {
    const uint8_t* mirror = d + (kind ? kDeviceConstantsPs : kDeviceConstantsVs);
    for (uint32_t i = 0; dirty[kind] && i < 64; ++i) {
      if ((dirty[kind] >> (63 - i)) & 1) {
        for (uint32_t w = 16 * i; w < 16 * i + 16; ++w) {
          c.Set(kind * 1024 + w, ReadBE(mirror + 4 * w), kFromMirror);
        }
      }
    }
  }
  ForEachLiteralOf(base, ReadBE(d + kDeviceVs), ReadBE(d + kDevicePs),
                   [&](uint32_t type, uint32_t first, const uint8_t* source, uint32_t count) {
                     for (uint32_t i = 0; type == 0 && i < count && first + i < 2048; ++i) {
                       c.Set(first + i, ReadBE(source + 4 * i), kFromLiteral);
                     }
                   });
}

void BeginBuffer(uint32_t device, uint32_t buffer) {
  if (!D3DShadowOn() || !device) {
    return;
  }
  // Begin marks the whole mirror dirty, yet a buffer's flushes leave some words alone (VS c32-c34): the buffer's
  // stream starts empty, so whatever it never sends shows as kFromUnknown.
  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  g_buffer_of_device[device] = buffer;
  StreamOf(g_stream_constants, device) = StreamConstants{};
  g_stream_begins.fetch_add(1, std::memory_order_relaxed);
}

void EndBuffer(uint32_t device) {
  if (!D3DShadowOn() || !device) {
    return;
  }
  // After End's own flush (BeforeFlush, called first): what the buffer leaves on the GPU when it runs. Kept per buffer,
  // since its device may record the other set's buffer before this one runs.
  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  const auto it = g_buffer_of_device.find(device);
  if (it == g_buffer_of_device.end()) {
    g_stream_ends_unknown.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const StreamConstants& now = StreamOf(g_stream_constants, device);
  StreamOf(g_buffer_constants, it->second) = now;
  g_stream_ends.fetch_add(1, std::memory_order_relaxed);
}

void AfterRun(uint32_t device, uint32_t buffer) {
  if (!D3DShadowOn() || !device) {
    return;
  }
  // RunCommandBuffer(device, buffer, predication) splices the buffer into the device's stream (INDIRECT_BUFFER), then
  // marks the device dirty from the buffer's persist tags: what the buffer left stays on the GPU until the device's
  // next flush sends those registers (BeforeFlush).
  std::lock_guard<std::mutex> lock(g_stream_constants_mutex);
  const auto it = g_buffer_constants.find(buffer);
  if (it == g_buffer_constants.end()) {
    g_stream_runs_unknown.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_stream_runs.fetch_add(1, std::memory_order_relaxed);
  const StreamConstants& from = *it->second;
  StreamConstants& c = StreamOf(g_stream_constants, device);
  for (uint32_t i = 0; i < 2048; ++i) {
    if ((from.known[i >> 6] >> (i & 63)) & 1) {
      c.Set(i, from.value[i], kFromBuffer);
    }
  }
}

std::string StreamConstantsReport() {
  return fmt::format("command buffers: {} begun, {} ended ({} on a device never begun), {} run ({} never ended)",
                     g_stream_begins.exchange(0, std::memory_order_relaxed),
                     g_stream_ends.exchange(0, std::memory_order_relaxed),
                     g_stream_ends_unknown.exchange(0, std::memory_order_relaxed),
                     g_stream_runs.exchange(0, std::memory_order_relaxed),
                     g_stream_runs_unknown.exchange(0, std::memory_order_relaxed));
}

void StreamRecordCounts(uint64_t& records, uint64_t& checking) {
  records = g_stream_records.load(std::memory_order_relaxed);
  checking = g_stream_checking.load(std::memory_order_relaxed);
}

void AnnotateCheckDraw(bool equal, uint32_t kind, const DrawRecord* lookup,
                              const DrawRecord* marker) {
  if (equal) {  // written only by the ring thread
    g_draw_equal.store(g_draw_equal.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }
  if (g_draw_different.load(std::memory_order_relaxed)) {
    return;
  }
  g_draw_case = kind;
  g_draw_has_lookup = lookup != nullptr;
  if (lookup) {
    g_draw_lookup = *lookup;
  }
  g_draw_has_marker = marker != nullptr;
  if (marker) {
    g_draw_marker = *marker;
  }
  g_draw_different.store(true, std::memory_order_release);
}

void LearnGroupMirror(uint32_t register_base, uint64_t mask, uint32_t displacement) {
  // Called on every register dump (bo2zm_d3d_marker.cpp), so it does nothing unless the shadow is on.
  static const bool shadow = REXCVAR_GET(bo2zm_native_d3d_shadow);
  if (!shadow) {
    return;
  }
  const int g = GroupOf(register_base);
  if (g < 0 || !displacement) {
    return;
  }
  // The dump copies from source + 4 * bit position: the mirror of register base_register + i is at
  // displacement + 4 * i. Always the same per group; the first one seen is stored.
  uint32_t expected = 0;
  g_offset_group[g].compare_exchange_strong(expected, displacement, std::memory_order_relaxed);
  const uint64_t before = g_mask_group[g].load(std::memory_order_relaxed);
  if ((before | mask) != before) {
    g_mask_group[g].store(before | mask, std::memory_order_relaxed);
  }
}

bool ReadSnapshot(uint64_t sequence, MirrorSnapshot& output) {
  const Shot& shot = g_shots[sequence % kShots];
  if (shot.sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = shot.data;
  std::atomic_thread_fence(std::memory_order_acquire);
  return shot.sequence.load(std::memory_order_relaxed) == sequence;  // not rewritten while being copied
}

bool ExtractDraw(DrawRecord& reg_index) {
  const uint32_t read = g_read.load(std::memory_order_relaxed);
  if (read == g_write.load(std::memory_order_acquire)) {
    return false;
  }
  reg_index = g_queue[read];
  g_read.store((read + 1) & (kSizeQueue - 1), std::memory_order_release);
  return true;
}

const ShaderInput* ObjectShader(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_objects.find(object);
  if (it != g_objects.end()) {
    return it->second;
  }
  // A registered object, turned into an entry on its first use.
  const auto n = g_object_numbers.find(object);
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (n == g_object_numbers.end() || !shaders) {
    return nullptr;
  }
  const ShaderInput* input = shaders->PerNumber(n->second);
  if (input) {
    g_objects.emplace(object, input);
  }
  return input;
}

uint64_t GenerationObjects() {
  return g_generation.load(std::memory_order_acquire);
}

/*
 * Shadow map vegetation, filtered on the game thread (bo2zm_d3d_game_vegetation).
 *
 * The addresses, device offsets and figures in this explanation are not BO2's: BO2's Draw* hooks do not call
 * DecideVegetation, so in BO2 the filter stays inactive and the ring keeps dropping these draws on its own. The
 * explanation is kept because it documents how the filter decides.
 *
 * The ring drops, right on entry to Draw, the colorless draws with an alpha test, a kill or PS-written
 * depth: the shadow map vegetation, ~480 per frame in gameplay (267-760). But each one still costs ~2.2-2.6 us
 * of ring time outside Draw (its FlushState marker, the DRAW_INDX, the pairing and its share of IM_LOAD)
 * plus its whole Draw* on the game thread (FlushState with streams, shaders and IM_LOAD, and the EDRAM mode
 * changes 5 and 4 with their packets). Here the same decision is taken in the D3D Draw*, before the
 * original, using the device's register mirror, and if it is vegetation the original is not called: the
 * Draw* never exists for the ring.
 *
 * Why the whole Draw* can be removed (from reading the PowerPC code)
 *   - DrawVertices (82593A10) and DrawIndexedVertices (82593C50), before FlushState, only touch
 *     VGT_INDX_OFFSET in the mirror (dev+0x2D14, with its dirty bit 1<<61 in dev+0x28): the same is done
 *     here. After FlushState they only write their DRAW_INDX (normal path: bit 0x04 of dev+0x28C0 clear and a
 *     single-segment count).
 *   - What the skipped FlushState would have dumped stays marked dirty and the next one dumps it (or the
 *     partial dump 825A42E0 of Resolve and ClearF) with the mirror's values, which are the same: the ring's
 *     state at the next draw is the usual one. The same goes for the shaders: their dirty bits stay set and
 *     the next FlushState patches and loads them for whatever shader pair is bound then.
 *   - What gets written to the ring without a dump first: the occlusion query (8258F810 and 8258EA28)
 *     writes RB_MODECONTROL, RB_COLOR_INFO and RB_SAMPLE_COUNT_ADDR directly and leaves them dirty to be
 *     restored. In the ring the only lazy register read without a dump before it is RB_SURFACE_INFO
 *     (ScaleOcclusion). That is why nothing is skipped with group 0x2000 dirty (render targets pending a
 *     dump) or with an occlusion query open.
 *   - Predicated tiling and ZPass: inside a D3D block (dev+0x28C0 & 0x3F: 0x10 tiling, 0x04 extension path
 *     with SET_BIN_MASK and EVENT_WRITE_EXT, 0x01, 0x02 and 0x20 ZPass, 0x08 the one from 825AB9A0) nothing
 *     is skipped: there the packets are recorded to be replayed per tile.
 *
 * The criterion: Draw's up to its early discard, with the state the ring will see for that draw
 *   - PS and VS bound (dev+0x3290 and dev+0x4FE8) and present in the library (ObjectShader: the identity
 *     the ring takes from the record).
 *   - Effective EDRAM mode 4. It is the mirror's (RB_MODECONTROL, dev+0x2D94) except for one change
 *     FlushState makes before dumping it: with a PS bound and its dirty bit 0x100 in dev+0x30, the shader
 *     loader (825A3AF0) calls 825A3A58, which turns mode 5 into 4. The switch to 5 (825A3968) only happens
 *     without a PS, and the streams (825A2D80) only set bit 0x80. No other function in the FlushState tree
 *     writes RB_MODECONTROL, RB_COLOR_MASK, RB_COLORCONTROL or the PS.
 *   - No color: no target i with RB_COLOR_MASK (dev+0x2D1C) nonzero and the PS writing it.
 *   - Alpha test (RB_COLORCONTROL, dev+0x2D7C: enabled and a function other than ALWAYS), the PS's own
 *     kill, or depth written by the PS.
 *   - And to skip it: bo2zm_native_ps_alpha_only without alternation, bo2zm_shadows_without_vegetation, no
 *     occlusion query open, outside blocks, no render targets pending a dump, and a single segment
 *     (count 1..65535).
 *
 * Self-checking guard, in two phases
 *   1. Observing: nothing is skipped. Each Draw* carries its verdict in the record and the ring computes
 *      its own in Draw, with its own registers and PS, and counts game yes/no against ring yes/no.
 *      After kChecks checked draws the game would skip, with no disagreement, it moves to applying.
 *   2. Applying: they are skipped. 1 in kSampleEvery is still sent, flagged kVegSample, for the ring to
 *      check, and the ones not skipped still carry their negative verdict, which the ring checks on all of
 *      them.
 *   The filter is switched off for the rest of the session, with MISMATCH in the log, on: a differing
 *   verdict in either direction, an occlusion query open in the ring that the game does not see, a draw the
 *   game sees as vegetation that the ring does not draw with its record's shaders, or a ring verdict that
 *   differs from Draw's early discard.
 *
 * Memory ordering
 *   The ring writes the disagreement details and then g_different with release. The game reads g_different
 *   relaxed on every Draw* and, if it is true, issues an acquire fence before reading the details: the fence
 *   synchronizes with the release and the details are seen in full (the same pairing DecideModeDraw uses
 *   for the draw records). Nobody waits or sleeps on this flag: the game only polls it, so no wake-up can be
 *   lost and nothing can hang (the earlier hangs were a thread asleep waiting for a wake-up). The worst case
 *   is seeing it a few microseconds late. The ring's counters have a single writer (the ring) and the game
 *   reads them relaxed: they only count. The verdict travels from the game to the ring inside the draw
 *   record, over the same already synchronized path as the draw records (the marker) or the queue (release
 *   and acquire on its indices).
 */
namespace {
namespace vegetation {

constexpr uint64_t kChecks = 200000;  // observing phase: draws the game would skip, checked in the ring
constexpr uint64_t kSampleEvery = 4096;       // applying phase: 1 in kSampleEvery is sent anyway (power of 2)
// The device's register mirror (FlushState groups: 0x2100 from +0x2D0C, 0x2200 from +0x2D74).
constexpr uint32_t kModeEdram = 0x2D94;     // RB_MODECONTROL (0x2208)
constexpr uint32_t kMaskColor = 0x2D1C;  // RB_COLOR_MASK (0x2104)
constexpr uint32_t kControlColor = 0x2D7C;  // RB_COLORCONTROL (0x2202)
constexpr uint32_t kIndexBase = 0x2D14;    // VGT_INDX_OFFSET (0x2102)
// Its dirty masks (64-bit big-endian): +0x20 fetch, booleans and group 0x2000 (bits 0x3FFFC000 of the low
// word); +0x28 groups 0x2100 (bit 63 = 0x2100), 0x2180, 0x2200 and 0x2280; +0x30 streams (0x400), shaders
// (0x1E0; 0x100 is the PS) and groups 0x2300 and 0x2380.
constexpr uint32_t kDirty20 = 0x20;
constexpr uint32_t kDirty28 = 0x28;
constexpr uint32_t kDirty30 = 0x30;
constexpr uint32_t kBlocks = 0x28C0;      // D3D block byte (BeginTiling 825992F0, ZPass 825999D8...)
constexpr uint8_t kBlocksMask = 0x3F;  // 0x40 and 0x80 are set at device creation: not blocks
constexpr uint32_t kTypeOcclusion = 9;      // D3DQUERYTYPE_OCCLUSION, at query+4 (8258F810)
constexpr uint32_t kMaxQueries = 16;

enum DisagreementKind : uint32_t { kDisagreeNone, kDisagreeGameYes, kDisagreeRingYes, kDisagreeOcclusion, kDisagreeNoIdentity, kDisagreeModel, kDisagreeKinds };

// ---- Game thread (the one using D3D: the Draw* calls and the queries' Issue) ----
int8_t g_active = -1;  // bo2zm_d3d_game_vegetation, read the first time (without a local static guard)
bool g_applying = false;
bool g_off = false;
bool g_settings = false;  // refreshed every 1024 Draw* calls checked
uint64_t g_turn = 0;
uint64_t g_observed_total = 0;
struct Memo {
  uint32_t object = 0;
  const ShaderInput* input = nullptr;
};
std::array<Memo, 32> g_memo{};
uint64_t g_memo_generation = UINT64_MAX;
// Report every 10 s.
int64_t g_i_next_ms = 0;
uint64_t g_i_observed = 0;
uint64_t g_i_yes = 0;
uint64_t g_i_skipped = 0;
uint64_t g_i_samples = 0;
uint64_t g_i_block = 0;
uint64_t g_i_targets = 0;
uint64_t g_i_occlusion = 0;
uint64_t g_i_settings = 0;
uint64_t g_i_count = 0;
std::array<uint32_t, 64> g_i_blocks{};  // the block ones, by value of dev+0x28C0 & 0x3F

// ---- Open D3D occlusion queries (Issue BEGIN without its END), per query object ----
// The D3D thread touches them; the lock only prevents a race should they ever be called from another
// thread.
std::mutex g_queries_lock;
std::array<uint32_t, kMaxQueries> g_queries{};
uint32_t g_n_queries = 0;
std::atomic<uint32_t> g_queries_open{0};  // UINT32_MAX: count lost, nothing is skipped any more

// ---- Shared with the ring thread ----
// Counters: only the ring writes them, without atomic read-modify-write (the A57 has no LSE). The game
// reads them relaxed for the report and to decide when to move to applying.
enum Counter : uint32_t {
  kYesYes,
  kNoNo,
  kYesNo,
  kNoYes,
  kNoYesBlock,
  kNoYesTargets,
  kNoYesOcclusion,
  kNoYesSettings,
  kNoYesCount,
  kSettingsDifferent,
  kNoCompare,
  kAgreements,
  kSamplesOk,
  kCounters
};
// Own cache line: the ring writes them on every draw and they must not share a line with anything the
// game thread reads on every Draw* (g_different), or every write would steal it from the other core.
alignas(64) std::atomic<uint64_t> g_counters[kCounters];
alignas(64) std::array<uint64_t, kCounters> g_previous{};  // own cache line; the previous report's values (game thread only)
// The first disagreement: the ring writes the details and then g_different with release (see Memory
// ordering). Own cache line (the game reads it on every Draw*; see g_counters).
alignas(64) std::atomic<bool> g_different{false};
uint32_t g_disagree_kind = kDisagreeNone;
uint16_t g_flags = 0;
VegetationDetail g_detail;

uint64_t ReadBE64(const uint8_t* p) {
  return (uint64_t(ReadBE(p)) << 32) | ReadBE(p + 4);
}

void WriteBE(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

const char* YesNo(bool value) {
  return value ? "si" : "no";
}

inline void Count(Counter c) {  // ring thread only
  g_counters[c].store(g_counters[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Ring thread only: the details before the flag (release); the game reads them after its acquire fence.
void Disagreement(DisagreementKind kind, uint16_t flags, const VegetationDetail& ring) {
  if (g_different.load(std::memory_order_relaxed)) {
    return;
  }
  g_disagree_kind = kind;
  g_flags = flags;
  g_detail = ring;
  g_different.store(true, std::memory_order_release);
}

// The object of a bound shader, with the same identity the ring takes from the record (ObjectShader).
// The generation is read relaxed: a shader created on another thread only reaches a Draw* on this thread
// through some synchronization in the game, and then this read already sees the new generation
// (write-read coherence).
const ShaderInput* Input(uint32_t object) {
  const uint64_t generation = g_generation.load(std::memory_order_relaxed);
  if (generation != g_memo_generation) {
    g_memo.fill(Memo{});
    g_memo_generation = generation;
  }
  Memo& memo = g_memo[size_t((object * UINT32_C(2654435761)) >> 27)];
  if (memo.object != object) {
    memo.object = object;
    memo.input = ObjectShader(object);
  }
  return memo.input;
}

// Draw's criterion up to its early discard (bo2zm_native_draws.cpp), with the state the ring will
// see.
bool Struct(const uint8_t* d, uint32_t& reason) {
  const uint32_t object_ps = ReadBE(d + kDevicePs);
  const uint32_t object_vs = ReadBE(d + kDeviceVs);
  if (!object_ps || !object_vs) {
    reason = kVegReasonNoShaders;
    return false;
  }
  // The EDRAM mode the ring will see: the mirror's, with the switch from 5 to 4 FlushState makes before
  // dumping it if a PS is bound and its dirty bit is set (825A3AF0 and 825A3A58). Without a PS it already
  // returned above.
  uint32_t mode = ReadBE(d + kModeEdram) & 0x7;
  if (mode == 5 && (ReadBE64(d + kDirty30) & 0x100)) {
    mode = 4;
  }
  if (mode != 4) {
    reason = kVegReasonMode;
    return false;
  }
  const ShaderInput* ps = Input(object_ps);
  const ShaderInput* vs = Input(object_vs);
  if (!ps || !vs) {
    reason = kVegReasonUnknown;
    return false;
  }
  const uint32_t mask = ReadBE(d + kMaskColor);
  for (uint32_t i = 0; i < 4; ++i) {
    if (((mask >> (i * 4)) & 0xF) && ((ps->outputs >> i) & 0x1)) {
      reason = kVegReasonColor;
      return false;
    }
  }
  const uint32_t control = ReadBE(d + kControlColor);
  const bool test_alpha = ((control >> 3) & 0x1) && (control & 0x7) != 7;
  if (!test_alpha && !ps->discards && !(ps->outputs & 0x10)) {
    reason = kVegReasonNoDiscard;
    return false;
  }
  reason = 0;
  return true;
}

void RefreshSettingsVegetation() {
  g_settings = REXCVAR_GET(bo2zm_native_ps_alpha_only) && REXCVAR_GET(bo2zm_native_ps_alpha_only_alternate_s) <= 0 &&
              REXCVAR_GET(bo2zm_shadows_without_vegetation);
}

// Game thread only, after the acquire fence.
void TurnOffVegetation() {
  if (g_off) {
    return;
  }
  g_off = true;
  static constexpr const char* kDisagreeText[kDisagreeKinds] = {
      "?",
      "the game sees vegetation and the ring does not",
      "the ring sees vegetation and the game does not",
      "occlusion query open on the ring and not in the game",
      "the game sees vegetation and the ring does not draw with the shaders of its record",
      "the ring's verdict is not Draw's early discard"};
  const VegetationDetail& a = g_detail;
  const uint16_t b = g_flags;
  REXLOG_ERROR("[vegetation] MISMATCH with the ring ({}): game {} (reason {}, settings {}, occlusion {}, block {}, "
               "targets {}, would skip {}, sample {}; phase {}); ring: structural {}, settings {}, occlusion {}, early "
               "discard in Draw {}, RB_MODECONTROL {:08X}, RB_COLOR_MASK {:08X}, RB_COLORCONTROL {:08X}, PS n{} "
               "(outputs {:X}, discards {}), VS n{}. Vegetation filter in the game OFF for the rest of the session: "
               "every Draw* goes back to the ring",
               kDisagreeText[g_disagree_kind < kDisagreeKinds ? g_disagree_kind : 0], YesNo((b & kVegYes) != 0), (b >> kVegReason) & 0xF,
               YesNo((b & kVegSettings) != 0), YesNo((b & kVegOcclusion) != 0), YesNo((b & kVegBlock) != 0),
               YesNo((b & kVegTargets) != 0), YesNo((b & kVegWouldSkip) != 0), YesNo((b & kVegSample) != 0),
               g_applying ? "applying" : "observing", YesNo(a.structural), YesNo(a.settings), YesNo(a.occlusion),
               YesNo(a.early), a.mode, a.mask, a.control, a.ps, a.outputs, YesNo(a.discards), a.vs);
}

// Every 10 s, from the game thread (BO2ZM_REPORT_DEFERRED). The ring's counters, as differences.
void ReportVegetation() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now < g_i_next_ms) {
    return;
  }
  const bool first = g_i_next_ms == 0;
  g_i_next_ms = now + 10000;
  std::array<uint64_t, kCounters> ring{};
  for (uint32_t i = 0; i < kCounters; ++i) {
    ring[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (first) {
    REXLOG_INFO("[vegetation] shadow-map vegetation filtered in the game: starts by observing; it skips "
                "once the ring has checked {} draws it would skip without any disagreement",
                kChecks);
  } else {
    std::array<uint64_t, kCounters> d{};
    for (uint32_t i = 0; i < kCounters; ++i) {
      d[i] = ring[i] - g_previous[i];
    }
    std::string blocks;
    for (uint32_t v = 1; v < 64; ++v) {
      if (g_i_blocks[v]) {
        blocks += fmt::format(" {:02X}:{}", v, g_i_blocks[v]);
      }
    }
    if (!blocks.empty()) {
      blocks = " (values" + blocks + ")";
    }
    const bool lost = g_queries_open.load(std::memory_order_relaxed) == UINT32_MAX;
    BO2ZM_REPORT_DEFERRED(
        "[vegetation] Draw*, last 10 s: {} looked at, {} vegetation for the game: {} skipped and {} sent "
        "for checking; not skipped {} in D3D blocks{}, {} with targets to dump, {} with an occlusion query open, {} "
        "with the settings off or alternating, {} because of the count | ring: yes and yes {}, no and no {}, game yes "
        "and ring no {}, game no and ring yes {} ({} because of block, {} because of targets, {} because of occlusion "
        "only in the game, {} because of settings, {} because of the count), different settings {}, not compared {}, "
        "good samples {} | phase {} | checked {} of {}{}",
        g_i_observed, g_i_yes, g_i_skipped, g_i_samples, g_i_block, blocks, g_i_targets, g_i_occlusion, g_i_settings,
        g_i_count, d[kYesYes], d[kNoNo], d[kYesNo], d[kNoYes], d[kNoYesBlock], d[kNoYesTargets], d[kNoYesOcclusion],
        d[kNoYesSettings], d[kNoYesCount], d[kSettingsDifferent], d[kNoCompare], d[kSamplesOk],
        g_applying ? "applying" : "observing", ring[kAgreements], kChecks,
        lost ? " | occlusion queries: THE COUNT WAS LOST, nothing is skipped" : "");
  }
  g_previous = ring;
  g_i_observed = g_i_yes = g_i_skipped = g_i_samples = g_i_block = g_i_targets = g_i_occlusion = g_i_settings =
      g_i_count = 0;
  g_i_blocks.fill(0);
}

}  // namespace vegetation
}  // namespace

uint16_t DecideVegetation(DrawFunction function, uint8_t* base, uint32_t vulkan_device, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip) {
  using namespace vegetation;
  skip = false;
  if (g_active < 0) {
    g_active = REXCVAR_GET(bo2zm_d3d_game_vegetation) ? 1 : 0;
  }
  if (!g_active || g_off || !vulkan_device || !g_shaders.load(std::memory_order_relaxed) ||
      (function != DrawFunction::kVertices && function != DrawFunction::kIndexed)) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_different.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    TurnOffVegetation();
    return 0;
  }
  if ((g_observed_total++ & 1023) == 0) {
    RefreshSettingsVegetation();
    ReportVegetation();
  }
  ++g_i_observed;
  const uint8_t* d = base + vulkan_device;
  uint32_t reason = 0;
  const bool yes = Struct(d, reason);
  const uint32_t open = g_queries_open.load(std::memory_order_relaxed);
  const uint8_t blocks = uint8_t(d[kBlocks] & kBlocksMask);
  const bool targets = (uint32_t(ReadBE64(d + kDirty20)) & 0x3FFFC000u) != 0;
  const uint32_t count = function == DrawFunction::kVertices ? r6 : r7;
  uint16_t flags = uint16_t(kVegHas | (reason << kVegReason));
  if (yes) {
    flags = uint16_t(flags | kVegYes);
  }
  if (open) {
    flags = uint16_t(flags | kVegOcclusion);
  }
  if (g_settings) {
    flags = uint16_t(flags | kVegSettings);
  }
  if (blocks) {
    flags = uint16_t(flags | kVegBlock);
  }
  if (targets) {
    flags = uint16_t(flags | kVegTargets);
  }
  if (!yes) {
    return flags;
  }
  ++g_i_yes;
  // Why it would not be skipped, in this order (each draw counts under a single reason).
  if (blocks) {
    ++g_i_block;
    ++g_i_blocks[blocks];
    return flags;
  }
  if (targets) {
    ++g_i_targets;
    return flags;
  }
  if (open) {
    ++g_i_occlusion;
    return flags;
  }
  if (!g_settings) {
    ++g_i_settings;
    return flags;
  }
  if (count == 0 || count > 0xFFFF) {
    ++g_i_count;
    return flags;
  }
  flags = uint16_t(flags | kVegWouldSkip);
  if (!g_applying) {
    const uint64_t checked = g_counters[kAgreements].load(std::memory_order_relaxed);
    if (checked < kChecks) {
      return flags;  // observing phase: sent, and the ring compares it
    }
    g_applying = true;
    REXLOG_INFO("[vegetation] {} vegetation draws the game would skip checked on the ring, 0 disagreements: they are "
                "skipped in the D3D's Draw* now (1 in {} is still sent to check it)",
                checked, kSampleEvery);
  }
  if ((++g_turn & (kSampleEvery - 1)) == 0) {
    ++g_i_samples;
    return uint16_t(flags | kVegSample);
  }
  // The whole Draw* is skipped. The only thing it does to the mirror before FlushState: VGT_INDX_OFFSET and
  // its dirty bit (DrawVertices sets it to its start vertex, r5; DrawIndexedVertices to 0). Everything else
  // stays dirty for the next dump, with the same mirror values.
  uint8_t* mirror = base + vulkan_device;
  const uint32_t index = function == DrawFunction::kVertices ? r5 : 0u;
  if (ReadBE(mirror + kIndexBase) != index) {
    WriteBE(mirror + kIndexBase, index);
    const uint64_t dirty = ReadBE64(mirror + kDirty28) | (uint64_t(1) << 61);
    WriteBE(mirror + kDirty28, uint32_t(dirty >> 32));
    WriteBE(mirror + kDirty28 + 4, uint32_t(dirty));
  }
  ++g_i_skipped;
  skip = true;
  return flags;
}

void AnnotateQueryD3D(const uint8_t* base, uint32_t query, uint32_t flags) {
  using namespace vegetation;
  if (!query || ReadBE(base + query + 4) != kTypeOcclusion) {
    return;
  }
  const bool begins = (flags & 2) != 0;
  if (!begins && !(flags & 1)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_queries_lock);
  uint32_t i = 0;
  while (i < g_n_queries && g_queries[i] != query) {
    ++i;
  }
  if (begins) {  // BEGIN takes precedence over END, as in the original
    if (i == g_n_queries) {
      if (g_n_queries == kMaxQueries) {
        g_queries_open.store(UINT32_MAX, std::memory_order_relaxed);
        return;
      }
      g_queries[g_n_queries++] = query;
    }
  } else if (i < g_n_queries) {
    g_queries[i] = g_queries[--g_n_queries];
  }
  if (g_queries_open.load(std::memory_order_relaxed) != UINT32_MAX) {
    g_queries_open.store(g_n_queries, std::memory_order_relaxed);
  }
}

uint16_t IdentityForVegetation(uint16_t flags, bool with_shaders) {
  using namespace vegetation;
  if (!(flags & kVegHas)) {
    return 0;
  }
  if (with_shaders) {
    return flags;
  }
  Count(kNoCompare);
  if (flags & kVegYes) {
    Disagreement(kDisagreeNoIdentity, flags, VegetationDetail{});
  }
  return 0;
}

void AnnotateVegetationRing(uint16_t flags, const VegetationDetail& ring) {
  using namespace vegetation;
  const bool yes = (flags & kVegYes) != 0;
  const bool would_skip = (flags & kVegWouldSkip) != 0;
  const bool full = ring.structural && ring.settings && !ring.occlusion;
  Count(would_skip ? (full ? kYesYes : kYesNo) : (full ? kNoYes : kNoNo));
  if (yes != ring.structural) {
    Disagreement(yes ? kDisagreeGameYes : kDisagreeRingYes, flags, ring);
    return;
  }
  if (would_skip) {
    if (!ring.settings) {
      Count(kSettingsDifferent);  // a setting just changed and each thread reads it at its own time: not an error
    } else if (ring.occlusion) {
      Disagreement(kDisagreeOcclusion, flags, ring);
    } else {
      Count(kAgreements);
      if (flags & kVegSample) {
        Count(kSamplesOk);
      }
    }
    return;
  }
  if (full) {  // same structure, but the game would not skip it: why
    Count((flags & kVegBlock)       ? kNoYesBlock
           : (flags & kVegTargets)   ? kNoYesTargets
           : (flags & kVegOcclusion)   ? kNoYesOcclusion
           : !(flags & kVegSettings)   ? kNoYesSettings
                                         : kNoYesCount);
  }
}

void AnnotateVegetationModel(uint16_t flags, const VegetationDetail& ring) {
  vegetation::Disagreement(vegetation::kDisagreeModel, flags, ring);
}

HookStats HooksStats() {
  HookStats e;
  e.created_vs = g_created_vs.load(std::memory_order_relaxed);
  e.known_vs = g_known_vs.load(std::memory_order_relaxed);
  e.created_ps = g_created_ps.load(std::memory_order_relaxed);
  e.known_ps = g_known_ps.load(std::memory_order_relaxed);
  e.lost = g_lost.load(std::memory_order_relaxed);
  return e;
}

}  // namespace bo2zm::native

// The constructors and the fetch patcher report what they write into the microcode (IM_LOAD without
// memcmp, see bo2zm_microcode_versions.h).
#include "bo2zm_microcode_versions.h"

// The experimental library's shader hooks hook the same constructors; with it enabled these are not compiled.
// These hooks sit at addresses that are not BO2's shader constructors and VS fetch patcher: in BO2 those addresses
// belong to unrelated functions, so the hooks stay compiled out:
// BO2ZM_REFERENCE_GAME_HOOKS is never defined in this build, and BO2 registers its shaders through
// RegisterShaderObject instead.
#if defined(BO2ZM_REFERENCE_GAME_HOOKS) && !defined(BO2ZM_NATIVE_SHADER_LIBRARY)
// r3 = original container; they return the object in r3. The original is always called and no PPC
// register is touched.
REX_EXTERN(__imp__sub_8259BC90);
REX_HOOK_RAW(sub_8259BC90) {  // pixel shader
  const auto* input =
      bo2zm::native::hooks_detail::IdentifyCreation(base, ctx.r3.u32, false);
  __imp__sub_8259BC90(ctx, base);
  bo2zm::native::hooks_detail::RememberCreation(ctx.r3.u32, input, false);
  bo2zm::native::microcode::WarnCreation(base, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_8259C038);
REX_HOOK_RAW(sub_8259C038) {  // vertex shader
  const auto* input = bo2zm::native::hooks_detail::IdentifyCreation(base, ctx.r3.u32, true);
  __imp__sub_8259C038(ctx, base);
  bo2zm::native::hooks_detail::RememberCreation(ctx.r3.u32, input, true);
  bo2zm::native::microcode::WarnCreation(base, ctx.r3.u32, true);
}
#endif

/*
 * The VS fetch patcher reports what it writes (IM_LOAD without memcmp, see bo2zm_microcode_versions.h).
 * sub_825A2FB8(r3 device, r4 VS, r5 destination, r6 declaration) writes the patched fetches to r5.
 *   - In place (r5 = [VS+40], from sub_825A3AF0): bumps the versions of that microcode's slot before and
 *     after writing.
 *   - On the ring's copy (from sub_825A37D8, return 0x825A38D0): not memory of any IM_LOAD; only counted.
 *   - Any other call: also bumps the destination's versions, but is counted separately and the ring turns
 *     off the shortcut.
 * Careful: the two calls in the generated code must go to sub_825A2FB8 and not to __imp__sub_825A2FB8
 * (two generated files, changed by a patch; tools/direct_calls.py already
 * handles them because this address appears here). If the one from sub_825A3AF0 went to __imp__, this
 * hook would not see the in-place patches and the ring's guard would stay in the observing phase (or
 * switch off with MISMATCH).
 * It changes no PPC register.
 */
#if defined(BO2ZM_REFERENCE_GAME_HOOKS)
REX_EXTERN(__imp__sub_825A2FB8);
REX_HOOK_RAW(sub_825A2FB8) {
  namespace mc = bo2zm::native::microcode;
  const uint32_t target = ctx.r5.u32;
  const uint32_t vs = ctx.r4.u32;
  const uint32_t return_address = uint32_t(ctx.lr);
  const bool in_room = vs != 0 && mc::ReadGuest32(base, vs + 40) == target;
  if (!in_room && return_address == 0x825A38D0u) {
    mc::Count(mc::g_patches_in_copy);
    __imp__sub_825A2FB8(ctx, base);
    return;
  }
  const uint32_t slot = mc::SlotOf(mc::Physical(target) & ~uint32_t(3));
  mc::BeginWrite(slot);
  __imp__sub_825A2FB8(ctx, base);
  // The counts, before FinishWrite's global++: a ring that sees the new global value already sees
  // them.
  if (in_room) {
    mc::Count(mc::g_patches_in_room);
  } else {
    mc::g_other_return.store(return_address, std::memory_order_relaxed);
    mc::g_other_target.store(target, std::memory_order_relaxed);
    mc::Count(mc::g_patches_others);
  }
  mc::FinishWrite(slot);
}
#endif  // BO2ZM_REFERENCE_GAME_HOOKS
