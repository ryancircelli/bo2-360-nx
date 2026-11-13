// The game's Direct3D functions in native code (milestone M5 of docs/research/07-d3d-renderer-plan.md).
//
// Why. With the game on three cores, the frame rate follows CPU time spent on any thread, and BO2's D3D layer takes
// about 5 % of the game threads' busy time in a stack profile: per draw the inlined flush and our marker dumps, the
// shader-bind writer, SetVertexShader, and SetTexture (about 3,130 calls a frame). Each function here repeats the
// generated code's stores, word for word, without the register file, the per-load byte swaps and the call frames.
//
// The guard, per function: before a checked call the words the function can write are saved; the game's function
// runs and its results are kept; the saved words go back; the native version runs; every word must be equal. The
// first kChecked calls, then 1 in kCheckEvery; any difference logs a MISMATCH and turns that function's native
// version off for the session (the game's runs from then on). A call the check cannot cover (one that would grow a
// list through another game function) simply runs the game's function.
//
// SetTexture, sub_82164190(device r3, stage r4, texture r5, dirty bit r6), 82164190-82164318:
//   - its fetch constant (six words at device + 1152 + 24 stage) takes the texture header's words 28-48 with some
//     bits kept from the slot's old value: the rlwimi masks below are the generated code's;
//   - the mip range in word 4 is clamped by two per-stage bytes (device + 12356 + stage, + 12382 + stage);
//   - the 64-bit dirty mask at device + 24 gets r6;
//   - no texture: only bits 0-1 of the slot's word 0 are cleared;
//   - the stage's pointer (device + 12920 + 4 stage) is set, and the texture it replaces gets the device's fence
//     ([device + 11036] into its word 2) or, when there is none and its header word 0 shares a bit with
//     [device + 11040], an 8-byte entry in the device's pending list ([device + 13928], up to [device + 13932];
//     sub_8216D208 grows it). The checks skip the fence case (the texture is shared with other devices' threads) and
//     the growing case. The entry's high word keeps bit 0 of a stack word the function never writes before
//     reading it (80(r1) of its frame): the native version reads the same guest word.
//
// SetVertexShader, sub_82169DE0(device r3, shader r4), and SetPixelShader, sub_82169A00(device r3, shader r4):
//   - the shader the device had (device + 13072 for the VS, + 13068 for the PS) is released as SetTexture releases its
//     texture: the device's fence into its word 2, or an entry in the pending list (the same checks, the same skips);
//   - the new shader is stored there; the VS clears bit 7 of the byte at device + 11070 and, with a shader, sets
//     0x80000 in the 64-bit word at device + 16; the PS sets 0x120000 there with or without one;
//   - the shader's state block (VS: shader + 872 + [shader + 892]; PS: shader + 40 + [shader + 60]) clears its
//     literal constants' dirty bits (64-bit mask at device + 0 for the VS, + 8 for the PS), sets bit 56 of the word
//     at device + 32 when its second mask word is not 0, and puts its fetch constants into the mirror at device +
//     1152: after entries that are skipped (8 bytes each) up to an empty one, entries copied word for word up to an
//     empty one, then entries of {mask, value} pairs (mirror word = mirror word & mask | value) up to an empty one.
//   The checks save and compare exactly the bytes a call can write (the walk is done once beforehand to list them).
//
// DrawIndexedVertices, sub_8216B1D0(device r3, type r4, base vertex r5, start index r6, index count r7):
//   - the inlined flush: for each of the five 64-bit dirty words at device + 0, 8, 16, 24, 32 that is not 0, the
//     dumps it selects, in the generated code's order and with its arguments (the float constants sub_821752D0, the
//     shader bind sub_82176298 and the render state sub_82174BA8, which return the word's new value, the register
//     runs sub_82174F38, the fetch constants sub_82175178, sub_82174FF8 and sub_82170000), and the word is cleared;
//   - then per piece (a draw of more than 65,535 indices is split at a multiple of the type's vertices per primitive,
//     the table at 82110EA0, overlapping by its second word): room in the stream ([device + 48] past [device + 56]:
//     sub_82161700), VGT_INDX_OFFSET = base vertex, and DRAW_INDX with the visibility word (device + 12498 +
//     [device + 12563]), the initiator, the index buffer's physical address and size; a command buffer device
//     (bit 0 of device + 11068) wraps the draw in its predication packets and notes it in its list (device + 13616,
//     sub_82160108 grows it).
//   The callees run as the game's code calls them (the same stack frame, return addresses and registers), so the
//   dumps' hooks, and their markers, are the same. This one is not checked call by call (running the dumps twice
//   would advance their markers twice): the D3D shadow (bo2zm_native_d3d_shadow) compares every word of the state
//   the stream gives each draw with the device mirror, and the draw records compare each draw with the ring's.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "bo2zm_d3d_native.h"

#include "bo2zm_benchmark.h"
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(bo2zm_d3d_native_set_texture, true, "BO2",
                    "D3D renderer: the game's SetTexture (sub_82164190) in native code, checked against the "
                    "game's own first (bo2zm_d3d_native.cpp). false = the game's code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_d3d_native_set_shader, true, "BO2",
                    "D3D renderer: the game's SetVertexShader (sub_82169DE0) and SetPixelShader (sub_82169A00) in "
                    "native code, checked against the game's own first (bo2zm_d3d_native.cpp). false = the game's code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_d3d_native_draw, true, "BO2",
                    "D3D renderer: the game's DrawIndexedVertices (sub_8216B1D0) in native code: its flush and its "
                    "packets, with the same calls to the dumps (bo2zm_d3d_native.cpp). false = the game's code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_d3d_native_ab, false, "BO2",
                    "Test, with bo2zm_benchmark_ab_s: A/B factor 1 is the native D3D functions (bo2zm_d3d_native.cpp) "
                    "against the game's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82164190);
REX_EXTERN(__imp__sub_82169DE0);
REX_EXTERN(__imp__sub_82169A00);
REX_EXTERN(__imp__sub_8216B1D0);
// DrawIndexedVertices' callees: the dumps through their hooks (bo2zm_d3d_marker.cpp), the rest as the game's code.
REX_EXTERN(sub_821752D0);
REX_EXTERN(sub_82174F38);
REX_EXTERN(sub_82175178);
REX_EXTERN(__imp__sub_82176298);  // call only: the shader bind
REX_EXTERN(__imp__sub_82174BA8);  // call only: the render state
REX_EXTERN(__imp__sub_82174FF8);  // call only
REX_EXTERN(__imp__sub_82170000);  // call only
REX_EXTERN(__imp__sub_82161700);  // call only: room in the ring
REX_EXTERN(__imp__sub_82160108);  // call only: the command buffer's draw list grows
REX_EXTERN(__imp__sub_8216D208);  // call only: the pending list's grow function

namespace {

// bo2zm_d3d_native_ab: in the A/B's off arms the game's functions run.
bool ArmOn() {
  static const bool ab = REXCVAR_GET(bo2zm_d3d_native_ab);
  if (ab) {
    const int arm = bo2zm::benchmark::AbArmCached();
    if (arm >= 0) {
      return (arm & 1) != 0;
    }
  }
  return true;
}

constexpr uint64_t kChecked = 20000;
constexpr uint64_t kCheckEvery = 1024;  // power of 2

// As REX_RAW_ADDR in the generated code: on Win32 and macOS arm64 0xE0000000 and up is 0x1000 higher.
inline uint8_t* Pointer(uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}
inline uint32_t Read32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Pointer(base, address), 4);
  return __builtin_bswap32(v);
}
inline uint64_t Read64(uint8_t* base, uint32_t address) {
  uint64_t v;
  std::memcpy(&v, Pointer(base, address), 8);
  return __builtin_bswap64(v);
}
inline uint8_t Read8(uint8_t* base, uint32_t address) { return *Pointer(base, address); }
inline uint32_t Read16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, Pointer(base, address), 2);
  return __builtin_bswap16(v);
}
inline void Write8(uint8_t* base, uint32_t address, uint8_t value) { *Pointer(base, address) = value; }
inline void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Pointer(base, address), &value, 4);
}
inline void Write64(uint8_t* base, uint32_t address, uint64_t value) {
  value = __builtin_bswap64(value);
  std::memcpy(Pointer(base, address), &value, 8);
}
// rlwimi: the bits of mask from source, the rest from target.
inline uint32_t Insert(uint32_t target, uint32_t source, uint32_t mask) { return (target & ~mask) | (source & mask); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// One native function's guard state and counters.
struct Guard {
  const char* name;
  std::atomic<int> phase{0};  // 0 checking, 1 native alone, 2 off
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> checked{0};
  std::atomic<uint64_t> skipped{0};  // calls the check cannot cover: the game's function ran
  std::atomic<int64_t> next_report{0};

  // Counters written by several game threads without read-modify-write atomics (a loop on the A57): a lost count
  // only makes the report and the thresholds slightly late.
  static void Add(std::atomic<uint64_t>& c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

  bool Check(uint64_t call) const {
    return phase.load(std::memory_order_relaxed) == 0 || (call & (kCheckEvery - 1)) == 0;
  }
  void Equal() {
    const uint64_t n = checked.load(std::memory_order_relaxed) + 1;
    checked.store(n, std::memory_order_relaxed);
    int expected = 0;
    if (n >= kChecked && phase.compare_exchange_strong(expected, 1)) {
      REXLOG_INFO("[d3d_native] {}: {} checked calls identical to the game's: native alone now (1 in {} still checked)",
                  name, n, kCheckEvery);
    }
  }
  void Different(const char* what, uint32_t at, uint32_t game, uint32_t native) {
    if (phase.exchange(2, std::memory_order_relaxed) == 2) {
      return;
    }
    REXLOG_ERROR("[d3d_native] {}: MISMATCH after {} checked calls ({} at {:08X}: game {:08X}, native {:08X}). The "
                 "game's function runs for the rest of the session",
                 name, checked.load(std::memory_order_relaxed), what, at, game, native);
  }
  void Report() {
    const int64_t now = NowMs();
    int64_t next = next_report.load(std::memory_order_relaxed);
    if (now < next || !next_report.compare_exchange_strong(next, now + 10000)) {
      return;
    }
    if (next == 0) {
      return;
    }
    const int p = phase.load(std::memory_order_relaxed);
    REXLOG_INFO("[d3d_native] {}: {} calls since boot, {} checked identical, {} left to the game's code (not "
                "checkable); phase {}",
                name, calls.load(std::memory_order_relaxed), checked.load(std::memory_order_relaxed),
                skipped.load(std::memory_order_relaxed), p == 0 ? "checking" : p == 1 ? "native" : "OFF");
  }
};

// ---- SetTexture ----

Guard g_set_texture{"SetTexture (sub_82164190)"};

constexpr uint32_t kFetchMirror = 1152;     // device + 1152 + 24 stage: the six fetch constant words
constexpr uint32_t kBound = 12920;          // device + 12920 + 4 stage: the texture bound to the stage (3230 * 4)
constexpr uint32_t kMipLimitLow = 12356;    // byte per stage
constexpr uint32_t kMipLimitHigh = 12382;   // byte per stage
constexpr uint32_t kDirty = 24;             // the 64-bit dirty mask
constexpr uint32_t kFence = 11036;
constexpr uint32_t kFenceMask = 11040;
constexpr uint32_t kListAt = 13928;
constexpr uint32_t kListEnd = 13932;
constexpr uint32_t kFrame = 192;            // stwu r1,-192(r1)

// The texture the stage had and whether the call will add a pending-list entry (and where).
struct SetTexturePlan {
  uint32_t old_texture;
  bool fence;  // the old texture takes the device's fence: a word other devices' threads write too
  bool entry;
  bool grows;  // the list is full: sub_8216D208 runs first
};

SetTexturePlan Plan(uint8_t* base, uint32_t dev, uint32_t stage) {
  SetTexturePlan p{Read32(base, dev + kBound + 4 * stage), false, false, false};
  p.fence = p.old_texture != 0 && Read32(base, dev + kFence) != 0;
  if (p.old_texture != 0 && !p.fence && (Read32(base, dev + kFenceMask) & Read32(base, p.old_texture)) != 0) {
    p.entry = true;
    p.grows = Read32(base, dev + kListAt) >= Read32(base, dev + kListEnd);
  }
  return p;
}

// The native SetTexture. Returns the r3 the game's function leaves.
uint32_t SetTexture(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint32_t stage = ctx.r4.u32;
  const uint32_t texture = ctx.r5.u32;
  const uint64_t bit = ctx.r6.u64;
  const uint32_t slot = dev + kFetchMirror + 24 * stage;  // r11
  const uint32_t old_texture = Read32(base, dev + kBound + 4 * stage);  // r28
  uint32_t r3 = dev;
  if (texture != 0) {
    const uint32_t t28 = Read32(base, texture + 28), t32 = Read32(base, texture + 32), t36 = Read32(base, texture + 36);
    const uint32_t t40 = Read32(base, texture + 40), t44 = Read32(base, texture + 44), t48 = Read32(base, texture + 48);
    const uint32_t f0 = Read32(base, slot), f4 = Read32(base, slot + 4), f12 = Read32(base, slot + 12);
    const uint32_t f16 = Read32(base, slot + 16), f20 = Read32(base, slot + 20);
    const uint32_t hi48 = (((t48 >> 20) & 0xFFF) + 512) & 0x1000;  // rlwinm r9,r27,12,20,31; addi 512; rlwinm 19,19
    const uint32_t hi32 = (((t32 >> 20) & 0xFFF) + 512) & 0x1000;
    Write32(base, slot + 8, t36);
    uint32_t w16 = Insert(t44, f16, 0xFFFFFC03u);               // rlwimi r10,r4,0,30,21
    const uint32_t w0 = Insert(t28, f0, 0x003FFC00u);           // rlwimi r25,r22,0,10,21
    const uint32_t w4 = Insert(hi32 + (t32 & 0x1FFFFFFFu), f4, 0x00000800u);  // rlwimi r3,r27,0,20,20
    const uint32_t w12 = Insert(t40, f12, 0x7FF80000u);         // rlwimi r23,r21,0,1,12
    const uint32_t w20 = Insert(hi48 + (t48 & 0x1FFFFE00u), f20, 0x000001FFu);  // rlwimi r7,r20,0,23,31
    Write32(base, slot + 16, w16);
    Write32(base, slot, w0);
    Write32(base, slot + 4, w4);
    Write32(base, slot + 12, w12);
    Write32(base, slot + 20, w20);
    r3 = w4;
    // The mip range (word 4's bits 2-5 and 6-9), each clamped by its stage byte.
    uint32_t low = Read8(base, dev + kMipLimitLow + stage);
    const uint32_t first = (t44 >> 2) & 0xF;
    if (first > low) {
      low = first;
    }
    w16 = Insert(w16, low << 2, 0x3Cu);
    uint32_t high = Read8(base, dev + kMipLimitHigh + stage);
    const uint32_t last = (t44 >> 6) & 0xF;
    if (last < high) {
      high = last;
    }
    w16 = Insert(w16, high << 6, 0x3C0u);
    Write32(base, slot + 16, w16);
    Write64(base, dev + kDirty, Read64(base, dev + kDirty) | bit);
  } else {
    Write32(base, slot, Read32(base, slot) & ~3u);
  }
  Write32(base, dev + kBound + 4 * stage, texture);
  if (old_texture != 0) {
    const uint32_t fence = Read32(base, dev + kFence);
    if (fence != 0) {
      Write32(base, old_texture + 8, fence);
    } else if ((Read32(base, dev + kFenceMask) & Read32(base, old_texture)) != 0) {
      uint32_t list = Read32(base, dev + kListAt);
      if (list >= Read32(base, dev + kListEnd)) {
        // Never in a checked call (see Plan): the game's grow function, called as its code calls it.
        const uint64_t r1 = ctx.r1.u64, lr = ctx.lr;
        ctx.r3.u64 = dev;
        ctx.r1.u64 = r1 - kFrame;
        ctx.lr = 0x821642ECu;
        __imp__sub_8216D208(ctx, base);  // call only
        list = ctx.r3.u32;
        ctx.r1.u64 = r1;
        ctx.lr = lr;
      }
      const uint32_t frame = ctx.r1.u32 - kFrame;
      const uint32_t temp = Read32(base, frame + 80);
      const uint32_t high_word = Insert(temp, old_texture >> 2, 0x3FFFFFFFu) & ~0x40000000u;
      Write32(base, frame + 80, high_word);
      Write32(base, frame + 84, 0xFFFFFFFFu);
      Write64(base, list, (uint64_t(high_word) << 32) | 0xFFFFFFFFu);
      Write32(base, dev + kListAt, list + 8);
      r3 = list;
    }
  }
  return r3;
}

// The words a SetTexture call can write (see the header comment), to save and compare.
struct SetTextureWords {
  uint32_t slot[6];
  uint32_t bound;
  uint64_t dirty;
  uint32_t old_fence;
  uint32_t list_at;
  uint64_t entry;
  uint64_t temp;
};

SetTextureWords Take(uint8_t* base, uint32_t dev, uint32_t stage, const SetTexturePlan& p, uint32_t frame) {
  SetTextureWords w{};
  for (uint32_t i = 0; i < 6; ++i) {
    w.slot[i] = Read32(base, dev + kFetchMirror + 24 * stage + 4 * i);
  }
  w.bound = Read32(base, dev + kBound + 4 * stage);
  w.dirty = Read64(base, dev + kDirty);
  w.old_fence = 0;  // never written in a checked call (see CheckedSetTexture)
  w.list_at = Read32(base, dev + kListAt);
  w.entry = p.entry ? Read64(base, w.list_at) : 0;
  w.temp = Read64(base, frame + 80);
  return w;
}

void Put(uint8_t* base, uint32_t dev, uint32_t stage, const SetTexturePlan& p, uint32_t frame,
         const SetTextureWords& w, uint32_t entry_at) {
  for (uint32_t i = 0; i < 6; ++i) {
    Write32(base, dev + kFetchMirror + 24 * stage + 4 * i, w.slot[i]);
  }
  Write32(base, dev + kBound + 4 * stage, w.bound);
  Write64(base, dev + kDirty, w.dirty);
  Write32(base, dev + kListAt, w.list_at);
  if (p.entry) {
    Write64(base, entry_at, w.entry);
  }
  Write64(base, frame + 80, w.temp);
}

template <typename Original>
void CheckedSetTexture(PPCContext& ctx, uint8_t* base, Original original) {
  const uint32_t dev = ctx.r3.u32, stage = ctx.r4.u32;
  const uint32_t frame = ctx.r1.u32 - kFrame;
  const SetTexturePlan plan = Plan(base, dev, stage);
  // Not checkable: growing the list goes through another game function, and saving and putting back the old
  // texture's fence word could undo another thread's write to that shared texture.
  if (plan.grows || plan.fence) {
    Guard::Add(g_set_texture.skipped);
    original(ctx, base);
    return;
  }
  const uint32_t entry_at = Read32(base, dev + kListAt);
  const SetTextureWords before = Take(base, dev, stage, plan, frame);
  const PPCContext input = ctx;
  original(ctx, base);
  const uint32_t r3_game = ctx.r3.u32;
  const SetTextureWords game = Take(base, dev, stage, plan, frame);
  const uint64_t entry_game = plan.entry ? Read64(base, entry_at) : 0;
  Put(base, dev, stage, plan, frame, before, entry_at);
  PPCContext native_ctx = input;
  const uint32_t r3_native = SetTexture(native_ctx, base);
  const SetTextureWords native = Take(base, dev, stage, plan, frame);
  const uint64_t entry_native = plan.entry ? Read64(base, entry_at) : 0;
  // The game's results stay in memory, whatever the comparison says.
  Put(base, dev, stage, plan, frame, game, entry_at);
  if (plan.entry) {
    Write64(base, entry_at, entry_game);
  }
  for (uint32_t i = 0; i < 6; ++i) {
    if (game.slot[i] != native.slot[i]) {
      g_set_texture.Different("fetch word", dev + kFetchMirror + 24 * stage + 4 * i, game.slot[i], native.slot[i]);
      return;
    }
  }
  if (game.bound != native.bound || game.dirty != native.dirty || game.old_fence != native.old_fence ||
      game.list_at != native.list_at || entry_game != entry_native || r3_game != r3_native) {
    const bool entry = entry_game != entry_native;
    g_set_texture.Different(game.bound != native.bound           ? "bound texture"
                            : game.dirty != native.dirty         ? "dirty mask"
                            : game.old_fence != native.old_fence ? "old texture's fence"
                            : game.list_at != native.list_at     ? "pending list pointer"
                            : entry                              ? "pending list entry"
                                                                 : "r3",
                            entry ? entry_at : dev, entry ? uint32_t(entry_game >> 32) : r3_game,
                            entry ? uint32_t(entry_native >> 32) : r3_native);
    return;
  }
  g_set_texture.Equal();
}

// ---- Guest bytes a call can write, saved, put back and compared as they are ----

struct Ranges {
  static constexpr uint32_t kMax = 32;
  static constexpr uint32_t kMaxBytes = 4096;
  struct Range {
    uint32_t at, size;
  };
  Range range[kMax];
  uint32_t count = 0;
  uint32_t bytes = 0;

  // False when there are too many to check (the caller then runs the game's function).
  bool Add(uint32_t at, uint32_t size) {
    if (count == kMax || bytes + size > kMaxBytes) {
      return false;
    }
    range[count++] = {at, size};
    bytes += size;
    return true;
  }
  void Save(uint8_t* base, uint8_t* to) const {
    for (uint32_t i = 0; i < count; ++i) {
      std::memcpy(to, Pointer(base, range[i].at), range[i].size);
      to += range[i].size;
    }
  }
  void Load(uint8_t* base, const uint8_t* from) const {
    for (uint32_t i = 0; i < count; ++i) {
      std::memcpy(Pointer(base, range[i].at), from, range[i].size);
      from += range[i].size;
    }
  }
  // The guest address of the first byte that differs, or 0.
  uint32_t FirstDifference(const uint8_t* a, const uint8_t* b) const {
    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
      for (uint32_t j = 0; j < range[i].size; ++j) {
        if (a[offset + j] != b[offset + j]) {
          return range[i].at + j;
        }
      }
      offset += range[i].size;
    }
    return 0;
  }
};

// Runs a call as the guard describes: the game's, then (from the same bytes) the native one; the game's results stay.
// Returns the address of the first difference, or 0.
template <typename Original, typename Native>
uint32_t RunChecked(PPCContext& ctx, uint8_t* base, const Ranges& ranges, Original original, Native native) {
  uint8_t before[Ranges::kMaxBytes], game[Ranges::kMaxBytes], mine[Ranges::kMaxBytes];
  ranges.Save(base, before);
  const PPCContext input = ctx;
  original(ctx, base);
  ranges.Save(base, game);
  ranges.Load(base, before);
  PPCContext native_ctx = input;
  native(native_ctx, base);
  ranges.Save(base, mine);
  ranges.Load(base, game);
  return std::memcmp(game, mine, ranges.bytes) == 0 ? 0 : ranges.FirstDifference(game, mine);
}

// ---- SetVertexShader and SetPixelShader ----

Guard g_set_vertex_shader{"SetVertexShader (sub_82169DE0)"};
Guard g_set_pixel_shader{"SetPixelShader (sub_82169A00)"};

constexpr uint32_t kBoundVs = 13072;
constexpr uint32_t kBoundPs = 13068;
constexpr uint32_t kShaderFrame = 128;  // stwu r1,-128(r1)

// The shader's state block (see the header comment), or 0 when it has none.
uint32_t StateBlock(uint8_t* base, bool pixel, uint32_t shader) {
  if (shader == 0) {
    return 0;
  }
  const uint32_t at = shader + (pixel ? 40 : 872);  // addi r11,r29,40 / addic. r11,r29,872
  if (!pixel && at == 0) {
    return 0;
  }
  const uint32_t offset = Read32(base, pixel ? shader + 60 : at + 20);
  return offset == 0 ? 0 : at + offset;
}

// The fetch-constant part of a state block, in the generated code's order: copy(mirror offset, source, bytes) for
// the copied entries and mask(mirror offset, pair address) for each {mask, value} pair.
template <typename Copy, typename Mask>
void WalkFetchBlock(uint8_t* base, uint32_t block, Copy copy, Mask mask) {
  uint32_t p = block + 20;
  const uint32_t end = p + Read32(base, block + 16);
  if (p >= end) {
    return;
  }
  for (;;) {  // loc_82169EF0: skipped entries
    const uint32_t words = Read16(base, p + 2);
    p += 4;
    if (words == 0) {
      break;
    }
    p += 4;
    if (p >= end) {
      break;
    }
  }
  if (p >= end) {
    return;
  }
  for (;;) {  // loc_82169F14: copied entries
    const uint32_t words = Read16(base, p + 2), offset = Read16(base, p);
    p += 4;
    if (words == 0) {
      break;
    }
    copy(offset, p, words * 4);
    p += words * 4;
    if (p >= end) {
      break;
    }
  }
  while (p < end) {  // loc_82169F9C, loc_82169F50: entries of {mask, value} pairs
    uint32_t words = Read16(base, p + 2);
    uint32_t offset = Read16(base, p);
    p += 4;
    if (words == 0) {
      return;
    }
    do {
      mask(offset, p);
      offset += 4;
      p += 8;
      words = (words - 2) & 0xFFFF;
    } while (words != 0);
  }
}

// What a Set*Shader call will do with the shader it replaces (as SetTexturePlan).
struct ReleasePlan {
  bool fence, entry, grows;
};

ReleasePlan PlanRelease(uint8_t* base, uint32_t dev, uint32_t old_shader) {
  ReleasePlan p{false, false, false};
  p.fence = old_shader != 0 && Read32(base, dev + kFence) != 0;
  if (old_shader != 0 && !p.fence && (Read32(base, dev + kFenceMask) & Read32(base, old_shader)) != 0) {
    p.entry = true;
    p.grows = Read32(base, dev + kListAt) >= Read32(base, dev + kListEnd);
  }
  return p;
}

// The release as both functions' code does it with the old shader; grow_lr is the return address of their call to
// the grow function.
void Release(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t old_shader, uint32_t frame_size, uint32_t grow_lr) {
  const uint32_t fence = Read32(base, dev + kFence);
  if (fence != 0) {
    Write32(base, old_shader + 8, fence);
    return;
  }
  if ((Read32(base, dev + kFenceMask) & Read32(base, old_shader)) == 0) {
    return;
  }
  uint32_t list = Read32(base, dev + kListAt);
  if (list >= Read32(base, dev + kListEnd)) {
    // Never in a checked call (see PlanRelease): the game's grow function, called as its code calls it.
    const uint64_t r1 = ctx.r1.u64, lr = ctx.lr;
    ctx.r3.u64 = dev;
    ctx.r1.u64 = r1 - frame_size;
    ctx.lr = grow_lr;
    __imp__sub_8216D208(ctx, base);  // call only
    list = ctx.r3.u32;
    ctx.r1.u64 = r1;
    ctx.lr = lr;
  }
  const uint32_t frame = ctx.r1.u32 - frame_size;
  const uint32_t high_word = Insert(Read32(base, frame + 80), old_shader >> 2, 0x3FFFFFFFu) & ~0x40000000u;
  Write32(base, frame + 80, high_word);
  Write32(base, frame + 84, 0xFFFFFFFFu);
  Write64(base, list, (uint64_t(high_word) << 32) | 0xFFFFFFFFu);
  Write32(base, dev + kListAt, list + 8);
}

template <bool kPixel>
void SetShader(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
  const uint32_t bound = dev + (kPixel ? kBoundPs : kBoundVs);
  if (!kPixel && shader != 0) {
    Write64(base, dev + 16, Read64(base, dev + 16) | 0x80000u);
  }
  const uint32_t old_shader = Read32(base, bound);
  if (old_shader != 0) {
    Release(ctx, base, dev, old_shader, kShaderFrame, kPixel ? 0x82169A5Cu : 0x82169E50u);
  }
  Write32(base, bound, shader);
  if (kPixel) {
    Write64(base, dev + 16, Read64(base, dev + 16) | 0x120000u);
  } else {
    Write8(base, dev + 11070, Read8(base, dev + 11070) & 0x7F);
  }
  const uint32_t block = StateBlock(base, kPixel, shader);
  if (block == 0) {
    return;
  }
  const uint32_t dirty = dev + (kPixel ? 8 : 0);
  Write64(base, dirty, Read64(base, dirty) & ~Read64(base, block));
  if (Read64(base, block + 8) != 0) {
    Write64(base, dev + 32, Read64(base, dev + 32) | (uint64_t(1) << 56));
  }
  const uint32_t mirror = dev + kFetchMirror;
  WalkFetchBlock(
      base, block,
      [&](uint32_t offset, uint32_t source, uint32_t bytes) {
        std::memcpy(Pointer(base, mirror + offset), Pointer(base, source), bytes);
      },
      [&](uint32_t offset, uint32_t pair) {
        // The words as they are in guest memory: and and or work byte by byte, so no swaps.
        uint32_t word, and_mask, or_value;
        std::memcpy(&word, Pointer(base, mirror + offset), 4);
        std::memcpy(&and_mask, Pointer(base, pair), 4);
        std::memcpy(&or_value, Pointer(base, pair + 4), 4);
        word = (word & and_mask) | or_value;
        std::memcpy(Pointer(base, mirror + offset), &word, 4);
      });
}

// The bytes a Set*Shader call can write, or false when it cannot be checked.
template <bool kPixel>
bool PlanSetShader(uint8_t* base, uint32_t dev, uint32_t shader, uint32_t frame, Ranges& ranges) {
  const ReleasePlan release = PlanRelease(base, dev, Read32(base, dev + (kPixel ? kBoundPs : kBoundVs)));
  // Not checkable, as in SetTexture: growing the list goes through another game function, and the old shader's fence
  // word is shared with the other devices' threads.
  if (release.grows || release.fence) {
    return false;
  }
  bool fits = ranges.Add(dev, 40) && ranges.Add(dev + 11068, 4) && ranges.Add(dev + kBoundPs, 8) &&
              ranges.Add(dev + kListAt, 4) && ranges.Add(frame + 80, 8);
  if (release.entry) {
    fits = fits && ranges.Add(Read32(base, dev + kListAt), 8);
  }
  const uint32_t block = StateBlock(base, kPixel, shader);
  if (block != 0) {
    const uint32_t mirror = dev + kFetchMirror;
    WalkFetchBlock(
        base, block,
        [&](uint32_t offset, uint32_t, uint32_t bytes) { fits = fits && ranges.Add(mirror + offset, bytes); },
        [&](uint32_t offset, uint32_t) { fits = fits && ranges.Add(mirror + offset, 4); });
  }
  return fits;
}

template <bool kPixel>
bool SetShaderNative(PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(bo2zm_d3d_native_set_shader);
  Guard& guard = kPixel ? g_set_pixel_shader : g_set_vertex_shader;
  if (!on || guard.phase.load(std::memory_order_relaxed) == 2 || !ArmOn()) {
    return false;
  }
  const uint64_t call = guard.calls.load(std::memory_order_relaxed) + 1;
  guard.calls.store(call, std::memory_order_relaxed);
  if ((call & 0xFFFF) == 0) {
    guard.Report();
  }
  if (!guard.Check(call)) {
    SetShader<kPixel>(ctx, base);
    return true;
  }
  Ranges ranges;
  if (!PlanSetShader<kPixel>(base, ctx.r3.u32, ctx.r4.u32, ctx.r1.u32 - kShaderFrame, ranges)) {
    Guard::Add(guard.skipped);
    return false;
  }
  const uint32_t at = RunChecked(
      ctx, base, ranges,
      [](PPCContext& c, uint8_t* b) {
        if (kPixel) {
          __imp__sub_82169A00(c, b);
        } else {
          __imp__sub_82169DE0(c, b);
        }
      },
      [](PPCContext& c, uint8_t* b) { SetShader<kPixel>(c, b); });
  if (at != 0) {
    guard.Different("byte", at, 0, 0);
  } else {
    guard.Equal();
  }
  return true;
}

// ---- DrawIndexedVertices ----

std::atomic<uint64_t> g_draws{0};
std::atomic<int64_t> g_draws_report{0};

constexpr uint32_t kDrawFrame = 208;               // stwu r1,-208(r1)
constexpr uint32_t kVerticesPerPrimitive = 0x82110EA0u;  // 8 bytes per type: {per primitive, overlap}

inline uint64_t Rotl64(uint64_t v, int n) { return (v << n) | (v >> (64 - n)); }

// A callee with the generated code's registers: r3 = device, r4-r6 as given, the return address of its bl.
template <typename Function>
inline void Call(PPCContext& ctx, uint8_t* base, Function function, uint32_t lr, uint32_t r3, uint64_t r4 = 0,
                 uint64_t r5 = 0, uint64_t r6 = 0) {
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.lr = lr;
  function(ctx, base);
}

inline void Registers(PPCContext& ctx, uint8_t* base, uint32_t lr, uint32_t dev, uint64_t mask, uint32_t first,
                      uint32_t mirror) {
  Call(ctx, base, sub_82174F38, lr, dev, mask, first, dev + mirror);
}

// The flush and the packets (see the header comment); ctx.r1 is the function's own frame.
void DrawIndexed(PPCContext& ctx, uint8_t* base) {
  const uint32_t dev = ctx.r3.u32;
  const uint32_t type = ctx.r4.u32;        // r18
  const uint32_t base_vertex = ctx.r5.u32;  // r17
  uint32_t start = ctx.r6.u32;             // r21
  uint32_t count = ctx.r7.u32;             // r19
  const uint64_t d0 = Read64(base, dev + 0);
  const uint64_t d8 = Read64(base, dev + 8);
  uint64_t d16 = Read64(base, dev + 16);
  const uint64_t d24 = Read64(base, dev + 24);
  const uint64_t d32 = Read64(base, dev + 32);
  if (d0 != 0) {
    Call(ctx, base, sub_821752D0, 0x8216B21Cu, dev, d0, 0x4000, dev + 1920);
    Write64(base, dev + 0, 0);
  }
  if (d8 != 0) {
    Call(ctx, base, sub_821752D0, 0x8216B23Cu, dev, d8, 0x4400, dev + 6016);
    Write64(base, dev + 8, 0);
  }
  if (d16 != 0) {
    if ((uint32_t(d16) & 0x1E0000u) != 0) {
      Call(ctx, base, __imp__sub_82176298, 0x8216B260u, dev, d16);  // call only
      d16 = ctx.r3.u64;
    }
    if ((Read64(base, dev + 40) & d16) != 0) {
      Call(ctx, base, __imp__sub_82174BA8, 0x8216B284u, dev, d16, Read32(base, dev + 10560));  // call only
      d16 = ctx.r3.u64;
    }
    if ((d16 & 0xFFFu) != 0) {
      Registers(ctx, base, 0x8216B2A8u, dev, Rotl64(d16, 52) & 0xFFF0000000000000ull, 0x2200, 10548);
    }
    if ((uint32_t(d16) & 0x1F000u) != 0) {
      Registers(ctx, base, 0x8216B2C8u, dev, Rotl64(d16, 47) & 0xF800000000000000ull, 0x2180, 10528);
    }
    if ((d16 & 0x03FFFC0000000000ull) != 0) {
      Registers(ctx, base, 0x8216B2F4u, dev, Rotl64(d16, 6) & 0xFFFF000000000000ull, 0x2000, 10368);
    }
    if ((d16 & 0x000003FFFFE00000ull) != 0) {
      Registers(ctx, base, 0x8216B31Cu, dev, Rotl64(d16, 22) & 0xFFFFF80000000000ull, 0x2100, 10444);
    }
    Write64(base, dev + 16, 0);
  }
  if (d24 != 0) {
    if ((d24 & 0xFFFFFFFFull) != 0) {
      Call(ctx, base, sub_82175178, 0x8216B340u, dev, Rotl64(d24, 32) & 0xFFFFFFFF00000000ull);
    }
    if ((d24 & 0x007FFFFC00000000ull) != 0) {
      Registers(ctx, base, 0x8216B36Cu, dev, Rotl64(d24, 9) & 0xFFFFF80000000000ull, 0x2280, 10596);
    }
    Write64(base, dev + 24, 0);
  }
  if (d32 != 0) {
    if ((d32 & 0x00003FC000000000ull) != 0) {
      Registers(ctx, base, 0x8216B3A0u, dev, Rotl64(d32, 18) & 0xFF00000000000000ull, 0x2380, 10832);
    }
    if ((d32 & 0x007E000000000000ull) != 0) {
      Call(ctx, base, __imp__sub_82174FF8, 0x8216B3C0u, dev, Rotl64(d32, 9) & 0xFC00000000000000ull);  // call only
    }
    if ((d32 & (uint64_t(1) << 56)) != 0) {
      Registers(ctx, base, 0x8216B3E8u, dev, 0xFFFFFFFFFF000000ull, 0x4900, 10112);
    }
    if ((d32 & 0x3FFFFFFFFFull) != 0) {
      Registers(ctx, base, 0x8216B408u, dev, Rotl64(d32, 26) & 0xFFFFFFFFFC000000ull, 0x2300, 10680);
    }
    if ((d32 & 0xC000000000000000ull) != 0 && (Read8(base, dev + 11072) & 0xC0) != 0) {
      Call(ctx, base, __imp__sub_82170000, 0x8216B430u, dev);  // call only
    }
    Write64(base, dev + 32, 0);
  }
  // The packets.
  const uint32_t index_buffer = Read32(base, dev + 12812);  // r24
  const uint32_t prim = type & 0x3F;                        // r22
  for (;;) {
    uint32_t p = Read32(base, dev + 48);
    if (p > Read32(base, dev + 56)) {
      Call(ctx, base, __imp__sub_82161700, 0x8216B460u, dev);  // call only
      p = ctx.r3.u32;
    }
    Write32(base, p + 4, 0x2102);  // type 0: VGT_INDX_OFFSET
    Write32(base, p + 8, base_vertex);
    const uint32_t at = p + 8;  // r25
    uint32_t piece = count;     // r27
    if (count > 65535) {
      const uint32_t per = Read32(base, kVerticesPerPrimitive + (type << 3));
      piece = ((65535u / per) & ~1u) * per;
    }
    const uint32_t buffer = Read32(base, index_buffer + 24);
    const uint32_t flags = Read32(base, index_buffer + 0);
    const uint32_t viz_index = Read8(base, dev + 12563);
    const uint32_t viz = ((Read8(base, dev + 12498 + viz_index) & ~0x100u) | (viz_index != 0 ? 0x100u : 0u)) & 0x13F;
    const uint32_t high = (flags << 1) & 0xC0000000u;
    uint32_t initiator = (piece << 16) | prim;  // r30
    uint32_t address = (start << 1) + buffer;
    uint32_t size = (piece & 0xFFFFFF) | high;  // r29
    if ((flags & 0x80000000u) != 0) {  // 32-bit indices
      address = (start << 2) + buffer;
      size = ((piece << 1) & 0xFFFFFE) | high;
      initiator |= 2048;
    }
    const uint32_t physical = ((((address >> 20) & 0xFFF) + 512) & 0x1000) + (address & 0x1FFFFFFF);  // r28
    uint32_t end;
    if ((Read8(base, dev + 11068) & 1) == 0) {
      Write32(base, at + 4, 0xC0032201u);  // DRAW_INDX
      Write32(base, at + 8, viz);
      Write32(base, at + 12, initiator);
      Write32(base, at + 16, physical);
      Write32(base, at + 20, size);
      end = at + 20;
    } else {
      uint32_t list = Read32(base, dev + 13628);
      if (list + 16 > Read32(base, dev + 13632)) {
        Call(ctx, base, __imp__sub_82160108, 0x8216B56Cu, dev + 13616);  // call only
        list = ctx.r3.u32;
      }
      Write32(base, list, at);
      Write32(base, dev + 13628, list + 16);
      const uint32_t entry = list + 4;
      const uint32_t entry_physical = ((((entry >> 20) & 0xFFF) + 512) & 0x1000) + (entry & 0x1FFFFFFE);
      static constexpr uint32_t kWords = 14;
      const uint32_t words[kWords] = {0xC0006000u, 0xFFFFFFFFu, 0xC0032201u, viz,         initiator,
                                      physical,    size,        0xC0006000u, 0x80000000u, 0xC0015A01u,
                                      26,          entry_physical | 1, 0xC0004601u, 25};
      for (uint32_t i = 0; i < kWords; ++i) {
        Write32(base, at + 4 + 4 * i, words[i]);
      }
      end = at + 4 * kWords;
    }
    const uint32_t left = count - piece;
    Write32(base, dev + 48, end);
    if (left == 0) {
      return;
    }
    const uint32_t overlap = Read32(base, kVerticesPerPrimitive + 4 + (type << 3));
    count = overlap + left;
    start += piece - overlap;
  }
}

}  // namespace

namespace bo2zm::d3d_native {

bool DrawIndexedVerticesNative(PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(bo2zm_d3d_native_draw);
  if (!on || !ArmOn()) {
    return false;
  }
  // The split divides by the type's vertices per primitive: a type whose divisor is 0 would trap in the game's code.
  if (ctx.r7.u32 > 65535 && Read32(base, kVerticesPerPrimitive + (ctx.r4.u32 << 3)) == 0) {
    return false;
  }
  const uint64_t n = g_draws.load(std::memory_order_relaxed) + 1;
  g_draws.store(n, std::memory_order_relaxed);
  if ((n & 0xFFFF) == 0) {
    const int64_t now = NowMs();
    int64_t next = g_draws_report.load(std::memory_order_relaxed);
    if (now >= next && g_draws_report.compare_exchange_strong(next, now + 10000) && next != 0) {
      REXLOG_INFO("[d3d_native] DrawIndexedVertices (sub_8216B1D0): {} calls in native code since boot", n);
    } else if (next == 0) {
      g_draws_report.store(now + 10000, std::memory_order_relaxed);
    }
  }
  // The function's frame, as its stwu makes it (the callees' frames then sit where the game's code puts them).
  const uint64_t r1 = ctx.r1.u64;
  const uint32_t frame = ctx.r1.u32 - kDrawFrame;
  Write32(base, frame, ctx.r1.u32);
  ctx.r1.u64 = frame;
  DrawIndexed(ctx, base);
  ctx.r1.u64 = r1;
  return true;
}

bool SetVertexShaderNative(PPCContext& ctx, uint8_t* base) { return SetShaderNative<false>(ctx, base); }
bool SetPixelShaderNative(PPCContext& ctx, uint8_t* base) { return SetShaderNative<true>(ctx, base); }

bool SetTextureNative(PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(bo2zm_d3d_native_set_texture);
  if (!on || g_set_texture.phase.load(std::memory_order_relaxed) == 2 || !ArmOn()) {
    return false;
  }
  const uint64_t call = g_set_texture.calls.load(std::memory_order_relaxed) + 1;
  g_set_texture.calls.store(call, std::memory_order_relaxed);
  if ((call & 0xFFFF) == 0) {
    g_set_texture.Report();
  }
  if (g_set_texture.Check(call)) {
    CheckedSetTexture(ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_82164190(c, b); });
    return true;
  }
  ctx.r3.u64 = SetTexture(ctx, base);
  return true;
}

}  // namespace bo2zm::d3d_native
