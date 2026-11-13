// Authored guest state drives the production HUD hooks and the CRT copy path.
//
// The first part covers the provenance the fitting depends on: HUD allocation, free and reset tags, the script call
// chain that marks an element as menu-created, the copies into a player's playerstate and the text ID evidence. The
// second part covers how a qualified element is drawn: the fitted placement and the full-screen HUD scale during its
// draw, the evened placement outside it, material quads kept to the view, the evened safe area and the local isHost
// answer.
//
// The stock functions the hooks wrap are modelled here, except the two placement lookups (0x822A8570 and
// 0x822A85A0), which are the generated functions themselves, copied from the local generated tree at test time.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>
#include <fmt/format.h>
#include <rex/ppc/context.h>
#include <fixture_guest_jump.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

std::vector<std::string> logs;
void FixtureBeforeLog(const std::string&);
void FixtureLog(std::string text) { FixtureBeforeLog(text); logs.push_back(std::move(text)); }
// The system heap the hooks allocate their placement records from: below the range GuestSpan accepts, as on the
// console, and readable through the heap's own protection.
constexpr uint32_t kSystemHeap = 0x00100000, kSystemHeapSize = 0x10000;
uint32_t system_heap_next = kSystemHeap;
uint32_t FixtureSystemHeapAlloc(uint32_t size, uint32_t alignment) {
  alignment = std::max(alignment, 16u);
  const uint32_t at = (system_heap_next + alignment - 1) & ~(alignment - 1);
  assert(at + size <= kSystemHeap + kSystemHeapSize);
  system_heap_next = at + size;
  return at;
}
uint32_t FixtureProtection(uint32_t at) {
  if (at >= kSystemHeap && at < system_heap_next) return 3;
  return at >= 0x10000 && at < 0x14000 ? 1 : at >= 0x40000000 && at < 0xA0000000 ? 3 : 0;
}
#define FIXTURE_SYSTEM_HEAP_ALLOC(size, alignment) FixtureSystemHeapAlloc(size, alignment)
#define FIXTURE_PROTECTION(at) FixtureProtection(at)
#include <fixture_mod_hud_fit_source.h>
#include "../../games/bo2/src/bo2zm_crt.cpp"

// Big-endian guest halves, as the payloads hold them.
int16_t Short(const uint8_t* data, uint32_t offset) {
  return int16_t((uint16_t(data[offset]) << 8) | data[offset + 1]);
}
void PutShort(uint8_t* data, uint32_t offset, int16_t value) {
  data[offset] = uint8_t(uint16_t(value) >> 8); data[offset + 1] = uint8_t(value);
}

namespace bo2zm::page_writes {
std::atomic<bool> g_enabled{false};
void MarkPhysical(uint32_t, uint32_t) { assert(false); }
}
namespace bo2zm::gsc {
size_t MenuCodeRanges(MenuCodeRange* out, size_t capacity) {
  if (!out || !capacity) return 0;
  out[0] = {0x10000, 0x11000};
  return 1;
}
// Script positions: 0x10000..0x11000 is the loaded mod's menu code, 0x12000..0x13000 the bot and shared code that
// vetoes menu provenance, anything else the game's own scripts.
bool MenuAncestry(const uint32_t* positions, size_t count) {
  bool menu = false;
  for (size_t i = 0; i < count; ++i) {
    if (positions[i] >= 0x12000 && positions[i] < 0x13000) return false;
    if (positions[i] >= 0x10000 && positions[i] < 0x11000) menu = true;
  }
  return menu;
}
}

uint8_t* memory;
bool require_pre_copy_log = false, observed_pre_copy_log = false;
uint32_t watched_copy_destination = 0;
void FixtureBeforeLog(const std::string& text) {
  if (!require_pre_copy_log || text.find("[mod-hud-id] menu ") != 0) return;
  assert(memory[watched_copy_destination] == 0xD7);
  observed_pre_copy_log = true;
}
uint32_t next_hud = 0;
unsigned allocate_calls = 0, free_calls = 0, reset_calls = 0;
bool jump_on_allocate = false, epoch_on_allocate = false;
bool epoch_on_view = false;
unsigned view_calls = 0;
unsigned draw_calls = 0;
uint32_t expected_draw_hud = 0;
std::array<std::array<int, 4>, 4> pending_rects{};
constexpr uint32_t cg_array = 0x90000000, clients = 0x91000000;
constexpr uint32_t numbers[] = {9, 3, 14, 1}, controllers[] = {2, 0, 3, 1};
// Registered playerstates (0x83B44E24 root, 0x83B44E28 stride), the guest stack and a Lua state.
constexpr uint32_t ps_root = 0x93000000, ps_stride = 0x57F8, stack = 0x94020000;
constexpr uint32_t lua_state = 0x94030000, lua_top = 0x94030800;
std::jmp_buf jump_target;

void Put(uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(memory + address, &value, 4);
}
float Float(uint32_t address) { return bo2zm::mod_hud_fit::Scalar(memory, address); }
void PutFloat(uint32_t address, float value) { bo2zm::mod_hud_fit::PutScalar(memory, address, value); }
template<class T> T FixtureLoad(uint8_t* base, uint32_t at) {
  T value; std::memcpy(&value, base + at, sizeof(value));
  if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
  if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
  if constexpr (sizeof(T) == 8) return __builtin_bswap64(value);
}
template<class T> void FixtureStore(uint8_t* base, uint32_t at, T value) {
  if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
  if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
  std::memcpy(base + at, &value, sizeof(value));
}

// The game's local-player-to-placement mapping (0x82403B90), called by the stock lookup 0x822A8570.
extern "C" REX_FUNC(__imp__sub_82403B90) { ctx.r3.u64 = ctx.r3.u32; (void)base; }
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_GUEST_BARRIER() ((void)0)
#define REX_LOAD_U16(a) FixtureLoad<uint16_t>(base,uint32_t(a))
#define REX_LOAD_U32(a) FixtureLoad<uint32_t>(base,uint32_t(a))
#define REX_LOAD_U64(a) FixtureLoad<uint64_t>(base,uint32_t(a))
#define REX_STORE_U32(a,v) FixtureStore<uint32_t>(base,uint32_t(a),uint32_t(v))
#define REX_STORE_U64(a,v) FixtureStore<uint64_t>(base,uint32_t(a),uint64_t(v))
#define DEFINE_REX_FUNC(name) extern "C" void __imp__##name(PPCContext& ctx, [[maybe_unused]] uint8_t* base)
#include <fixture_placement_lookups.h>
#undef DEFINE_REX_FUNC
#undef REX_FUNC_PROLOGUE
#undef REX_GUEST_BARRIER
#undef REX_LOAD_U16
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U32
#undef REX_STORE_U64

void __imp__sub_8232A280(PPCContext& ctx, uint8_t* base) {
  ++allocate_calls;
  if (jump_on_allocate) std::longjmp(jump_target, 1);
  if (epoch_on_allocate) bo2zm::mod_hud_fit::BeginEpoch();
  const auto owner = ctx.r3.u32;
  ctx.r3.u64 = next_hud;
  if (!next_hud) return;
  std::memset(base + next_hud, 0x5A, 136);
  // Constructor-default label/body IDs are empty, within the legal 9-bit domain.
  base[next_hud + 0x56] = base[next_hud + 0x57] = 0;
  base[next_hud + 0x64] = base[next_hud + 0x65] = 0;
  Put(next_hud + 124, owner);
  base[next_hud + 112] = 1;
  base[next_hud + 121] = 255;
}
void __imp__sub_8237C968(PPCContext&, uint8_t*) { ++free_calls; }
void __imp__sub_8232A420(PPCContext&, uint8_t*) { ++reset_calls; }
void __imp__sub_8223BD90(PPCContext& ctx, uint8_t*) {
  ++view_calls;
  const auto local = ctx.r3.u32;
  if (epoch_on_view) bo2zm::mod_hud_fit::BeginEpoch();
  for (unsigned i = 0; i < 4; ++i) Put(cg_array + local * 0x87700 + 0x4D880 + 4 * i, uint32_t(pending_rects[local][i]));
  ctx.r3.u64 = 0xABCDEF; ctx.f1.f64 = 17.25;
}

// The material quads the stock draw passes on (0x82262A20: x, y, w, h, s0, t0, s1, t1 in f1-f8).
using Quad = std::array<double, 8>;
std::vector<Quad> drawn_quads;
void __imp__sub_82262A20(PPCContext& ctx, uint8_t*) {
  drawn_quads.push_back({ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, ctx.f5.f64, ctx.f6.f64, ctx.f7.f64, ctx.f8.f64});
}
void SubmitQuad(const PPCContext& at, const Quad& quad) {
  PPCContext call = at;
  call.f1.f64 = quad[0]; call.f2.f64 = quad[1]; call.f3.f64 = quad[2]; call.f4.f64 = quad[3];
  call.f5.f64 = quad[4]; call.f6.f64 = quad[5]; call.f7.f64 = quad[6]; call.f8.f64 = quad[7];
  sub_82262A20(call, memory);
}

// What the stock per-client HUD draw (0x821DC058) saw: the shared HUD scale, the placement both lookups returned
// from inside its frame, and the one returned at the draw's own stack depth.
struct DrawObservation {
  float hud_scale = 0;
  uint32_t by_local = 0, by_index = 0, at_entry = 0;
};
std::vector<DrawObservation> draws;
std::vector<Quad> quads_in_draw, quads_at_entry;
bool draw_jumps = false;
constexpr uint32_t kDrawFrame = 0x4E0;
uint32_t LookupByLocal(const PPCContext& at, uint32_t local) {
  PPCContext call = at; call.r3.u64 = local;
  sub_822A8570(call, memory);
  return call.r3.u32;
}
uint32_t LookupByIndex(const PPCContext& at, uint32_t index) {
  PPCContext call = at; call.r3.u64 = index;
  sub_822A85A0(call, memory);
  return call.r3.u32;
}
void __imp__sub_821DC058(PPCContext& ctx, uint8_t* base) {
  ++draw_calls;
  PPCContext inner = ctx; inner.r1.u64 = ctx.r1.u32 - kDrawFrame; inner.lr = 0x821DC100;
  DrawObservation seen;
  seen.hud_scale = bo2zm::mod_hud_fit::Scalar(base, bo2zm::mod_hud_fit::kHudScale);
  seen.by_local = LookupByLocal(inner, ctx.r3.u32);
  seen.by_index = LookupByIndex(inner, ctx.r3.u32);
  seen.at_entry = LookupByLocal(ctx, ctx.r3.u32);
  for (const auto& quad : quads_in_draw) SubmitQuad(inner, quad);
  for (const auto& quad : quads_at_entry) SubmitQuad(ctx, quad);
  draws.push_back(seen);
  if (draw_jumps) ppc_longjmp(0xAD01, 7);
  ctx.r3.u64 = 0xFEDCBA; ctx.f1.f64 = 42.5;
}

// isHost (0x8231DD80) as the game answers it: only the host's entity (number 9) is the host. Every answer is
// pushed through Scr_AddInt (0x8253D758).
unsigned stock_host_calls = 0;
std::vector<std::array<uint32_t, 2>> pushed_ints;
void __imp__sub_8253D758(PPCContext& ctx, uint8_t*) { pushed_ints.push_back({ctx.r3.u32, ctx.r4.u32}); }
void __imp__sub_8231DD80(PPCContext& ctx, uint8_t* base) {
  ++stock_host_calls;
  PPCContext call = ctx;
  call.r3.u64 = 0; call.r4.u64 = uint32_t(ctx.r3.u64 >> 48) == numbers[0];
  __imp__sub_8253D758(call, base);
}

// Engine.GetUserSafeAreaForController (0x8271F3F0) as the game answers it: four numbers pushed on the Lua stack.
std::array<float, 4> game_safe_area{};
uint32_t safe_area_count = 4, safe_area_type = 3;
void __imp__sub_8271F3F0(PPCContext& ctx, uint8_t*) {
  Put(ctx.r3.u32 + 36, lua_top);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t slot = lua_top - 8 * (4 - i);
    PutFloat(slot, game_safe_area[i]); Put(slot + 4, safe_area_type);
  }
  ctx.r3.u64 = safe_area_count;
}

void Commit(uint32_t address, size_t size) {
#ifdef _WIN32
  assert(VirtualAlloc(memory + address, size, MEM_COMMIT, PAGE_READWRITE));
#else
  assert(mprotect(memory + address, size, PROT_READ | PROT_WRITE) == 0);
#endif
}
PPCContext CreationContext(uint32_t owner = 3) {
  PPCContext ctx{};
  ctx.r3.u64 = owner;
  ctx.lr = 0x8232C400;
  return ctx;
}
PPCContext Copy(uint32_t source, uint32_t lr = 0x8232CDB0, uint32_t n = 124) {
  PPCContext ctx{};
  ctx.r3.u64 = 0x20000;
  ctx.r4.u64 = source;
  ctx.r5.u64 = n;
  ctx.lr = lr;
  return ctx;
}
void ValidVm() {
  using namespace bo2zm::mod_hud_fit;
  Put(kVm + 8, 2); Put(kVm + 12, kFrames + 40); Put(kVm + 4, kMaxStack); Put(kVm + 16, kStack);
  Put(kFrames + 20, 0x10081); Put(kFrames + 40, 0x13001);
}
void SetupLayout(unsigned count) {
  using namespace bo2zm::mod_hud_fit;
  Put(0x837F6A80, 4); Put(0x837F8CB4, 1); Put(0x837F8DC0, clients + 0x28000);
  Put(clients + 0x28000 + 0x10, 1); memory[clients + 0x28000 + 0x18] = 1;
  Put(0x82C70F50, 0x92000000); Put(0x84456C84, 0);
  Put(0x82C70F44, count); Put(0x82BBAE68, cg_array);
  Put(0x82C153E8, 1280); Put(0x82C153EC, 720);
  for (unsigned local = 0; local < 4; ++local) {
    Put(0x92000000 + local * 0x358A8 + 0x24368, 2);
    const uint32_t manager = 0x837F6A28 + local * 20;
    Put(manager, local < count); Put(manager + 4, local); Put(manager + 8, controllers[local]);
    memory[0x83E402C0 + controllers[local] * 172] = uint8_t(local < count);
    Put(0x82C6FDC8 + local * 1112, local < count);
    Put(cg_array + local * 0x87700, numbers[local]);
    const uint32_t entity = 0x833D0640 + numbers[local] * 796;
    Put(entity + 340, clients + local * 0x10000); memory[entity + 251] = 1;
    Put(clients + local * 0x10000 + 416, numbers[local]);
    // Three-view fixture uses an unequal-width bottom view; four uses borders.
    pending_rects[local] = count == 1 ? std::array<int, 4>{0, 0, 1280, 720} :
        count == 2 ? std::array<int, 4>{0, int(local) * 360, 1280, 360} :
        count == 3 && local == 2 ? std::array<int, 4>{0, 360, 1280, 360} :
        count == 3 ? std::array<int, 4>{int(local) * 640, 0, 640, 360} :
        std::array<int, 4>{int(local % 2) * 640 + 4, int(local / 2) * 360 + 4, 632, 352};
    for (unsigned i = 0; i < 4; ++i) Put(cg_array + local * 0x87700 + 0x4D880 + i * 4, 0);
  }
}
void PublishViews(unsigned count) {
  // First pass installs the new CG rectangles. The second is a stable layout
  // in which every active view is freshly observed after signature invalidation.
  for (unsigned pass = 0; pass < 2; ++pass) for (unsigned local = 0; local < count; ++local) {
    PPCContext ctx{}; ctx.r3.u64 = local;
    sub_8223BD90(ctx, memory);
    assert(ctx.r3.u64 == 0xABCDEF && ctx.f1.f64 == 17.25);
  }
}
void AuthorPayload(uint32_t hud, uint8_t type) {
  using namespace bo2zm::mod_hud_fit;
  uint8_t* p = memory + hud;
  PutScalar(p, 0, 200.0f); PutScalar(p, 4, -120.0f); PutScalar(p, 8, 0.0f);
  PutScalar(p, 0xC, 2.0f); PutScalar(p, 0x10, 1.25f);
  PutScalar(p, 0x28, -100.0f); PutScalar(p, 0x2C, 80.0f);
  PutShort(p, 0x58, 640); PutShort(p, 0x5A, 803); PutShort(p, 0x5C, 320); PutShort(p, 0x5E, 400);
  PutShort(p, 0x60, 1200); PutShort(p, 0x62, 1200);
  p[0x70] = type; p[0x71] = 3; p[0x72] = p[0x76] = 5;
  p[0x73] = 0x21; p[0x77] = 0x32; p[0x79] = 255;
  Put(hud + 128, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Placement records, the shared HUD scale and a qualified element in each local player's snapshot.
// ---------------------------------------------------------------------------------------------------------------
constexpr float kSplitHudScale = 1.6f;  // the value split screen raises the shared HUD scale to
uint32_t ViewRecord(unsigned index) { return bo2zm::mod_hud_fit::kViewPlacements + index * 124; }
uint32_t FittedRecord(unsigned local) { return bo2zm::mod_hud_fit::synthetic_placements + local * 124; }
uint32_t EvenRecord(unsigned index) { return bo2zm::mod_hud_fit::even_placements + index * 124; }
// Every field gets a value of its own, so a field taken from the wrong place or left out shows.
void AuthorPlacements(unsigned count) {
  using namespace bo2zm::mod_hud_fit;
  for (uint32_t offset = 0; offset < 124; offset += 4) PutFloat(kFullPlacement + offset, 0.5f + float(offset));
  PutFloat(kFullPlacement + 0x00, 1.0f); PutFloat(kFullPlacement + 0x04, 1.0f);
  PutFloat(kFullPlacement + 0x10, 1.0f); PutFloat(kFullPlacement + 0x14, 1.0f);
  PutFloat(kFullPlacement + 0x38, 0.0f); PutFloat(kFullPlacement + 0x3C, 0.0f);
  PutFloat(kFullPlacement + 0x40, 1280.0f); PutFloat(kFullPlacement + 0x44, 720.0f);
  for (unsigned index = 0; index < 4; ++index) {
    const uint32_t record = ViewRecord(index);
    for (uint32_t offset = 0; offset < 124; offset += 4) PutFloat(record + offset, 0.25f + float(offset) + 1000.0f * index);
    const auto& rect = index < count ? pending_rects[index] : std::array<int, 4>{0, 0, 1280, 720};
    PutFloat(record + 0x00, 2.0f); PutFloat(record + 0x04, 1.5f);
    PutFloat(record + 0x38, float(rect[0])); PutFloat(record + 0x3C, float(rect[1]));
    PutFloat(record + 0x40, float(rect[2])); PutFloat(record + 0x44, float(rect[3]));
  }
  PutFloat(kHudScale, count > 1 ? kSplitHudScale : 1.0f);
}
// A layout whose views draw from registered playerstates (the only ones a fitted draw accepts).
void SetupWorld(unsigned count) {
  SetupLayout(count);
  Put(0x83B44E24, ps_root); Put(0x83B44E28, ps_stride);
  for (unsigned local = 0; local < 4; ++local) {
    const uint32_t ps = ps_root + numbers[local] * ps_stride;
    std::memset(memory + ps, 0, ps_stride); Put(ps + 416, numbers[local]);
    Put(0x833D0640 + numbers[local] * 796 + 340, ps);
  }
  AuthorPlacements(count);
}
// A menu-created text element is copied into the player's playerstate and proven by a full playerstate copy; the
// client's snapshot then holds the same element. Returns the element as the client draws it.
uint32_t QualifyText(unsigned local, uint16_t body, uint16_t label) {
  using namespace bo2zm::mod_hud_fit;
  ValidVm();
  const uint32_t ps = ps_root + numbers[local] * ps_stride;
  auto creation = CreationContext(numbers[local]); sub_8232A280(creation, memory);
  assert(creations[0].menu);
  AuthorPayload(kPool, 1); PutShort(memory + kPool, 0x64, int16_t(body)); PutShort(memory + kPool, 0x56, int16_t(label));
  auto copy = Copy(kPool); copy.r3.u64 = ps + 3068;
  rexcrt_bo2zm_memcpy(copy, memory);
  assert(Short(memory + ps + 3068, 0x64) == int16_t(body) && Short(memory + ps + 3068, 0x56) == int16_t(label));
  auto scan = Copy(ps, 0x82439010, 10760); scan.r3.u64 = clients + 0x20000;
  rexcrt_bo2zm_xmemcpy(scan, memory);
  assert(owner_ready.test(numbers[local]) && qualified_body_ids[numbers[local]].test(body));
  const uint32_t cg = cg_array + local * 0x87700, snapshot = cg + 0x80;
  std::memcpy(memory + snapshot + 16, memory + ps, 10760); Put(cg + 40, snapshot);
  return snapshot + 16 + 3068;
}
PPCContext DrawContext(unsigned local, uint32_t hud) {
  PPCContext ctx{};
  ctx.r1.u64 = stack; ctx.r3.u64 = local; ctx.r4.u64 = hud; ctx.lr = 0x821DC5AC;
  return ctx;
}
// Draws one element and checks what every draw must keep: the stock draw runs once, the caller's registers come back
// as the stock draw left them, and the element itself is not changed.
const DrawObservation& Draw(unsigned local, uint32_t hud) {
  std::array<uint8_t, 124> before{}; std::memcpy(before.data(), memory + hud, before.size());
  const auto calls = draw_calls;
  const auto input = DrawContext(local, hud);
  auto ctx = input;
  sub_821DC058(ctx, memory);
  auto expected = input; expected.r3.u64 = 0xFEDCBA; expected.f1.f64 = 42.5;
  assert(draw_calls == calls + 1 && std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
  assert(std::memcmp(before.data(), memory + hud, before.size()) == 0);
  return draws.back();
}
bool Fitted(const DrawObservation& seen, unsigned local) {
  return bo2zm::mod_hud_fit::synthetic_placements && seen.by_local == FittedRecord(local);
}

// The full-screen record mapped into the view: the full screen shrunk by the view's factor and centred in the view;
// pixel positions and pixel scales follow the factor, the pixel-to-unit scales its inverse, the HUD-unit fields stay.
void CheckFittedRecord(unsigned local) {
  using namespace bo2zm::mod_hud_fit;
  const int width = pending_rects[local][2], height = pending_rects[local][3];
  const uint32_t screen_width = 1280, screen_height = 720;
  const float factor = std::min(float(width) / screen_width, float(height) / screen_height);
  const float ox = (width - factor * screen_width) * 0.5f, oy = (height - factor * screen_height) * 0.5f;
  const uint32_t record = FittedRecord(local);
  for (uint32_t offset = 0; offset < 124; offset += 4) {
    const float full = Float(kFullPlacement + offset);
    float expected = full;
    if (offset <= 0x0C || offset == 0x40 || offset == 0x44 || offset == 0x70 || offset == 0x74) expected = full * factor;
    else if (offset == 0x10 || offset == 0x14) expected = full / factor;
    else if (offset >= 0x38 && offset <= 0x6C) expected = full * factor + ((offset & 4) ? oy : ox);
    assert(Float(record + offset) == expected);
  }
  // The shrunk screen fits the view and is centred in it.
  assert(Float(record + 0x40) <= float(width) && Float(record + 0x44) <= float(height));
  assert(std::fabs(2 * Float(record + 0x38) + Float(record + 0x40) - float(width)) < 0.01f);
  assert(std::fabs(2 * Float(record + 0x3C) + Float(record + 0x44) - float(height)) < 0.01f);
}
// The view's record with bo2zm_split_margin_percent of its size as the margin on all four sides, in pixels and in
// HUD units; every other field as the game has it.
void CheckEvenRecord(unsigned index) {
  const uint32_t view = ViewRecord(index), copy = EvenRecord(index);
  const float width = Float(view + 0x40), height = Float(view + 0x44);
  const float sx = Float(view + 0x00), sy = Float(view + 0x04), x0 = Float(view + 0x38), y0 = Float(view + 0x3C);
  const float share = float(FLAGS_bo2zm_split_margin_percent / 100.0);
  const float mx = share * width, my = share * height;
  for (uint32_t offset = 0; offset < 124; offset += 4) {
    float expected = Float(view + offset);
    switch (offset) {
      case 0x50: case 0x60: expected = x0 + mx; break;
      case 0x54: case 0x64: expected = y0 + my; break;
      case 0x58: case 0x68: expected = x0 + width - mx; break;
      case 0x5C: case 0x6C: expected = y0 + height - my; break;
      case 0x18: case 0x28: expected = mx / sx; break;
      case 0x1C: case 0x2C: expected = my / sy; break;
      case 0x20: case 0x30: expected = (width - mx) / sx; break;
      case 0x24: case 0x34: expected = (height - my) / sy; break;
      default: break;
    }
    assert(Float(copy + offset) == expected);
  }
  // The same margin on opposite sides.
  assert(std::fabs((Float(copy + 0x50) - x0) - (x0 + width - Float(copy + 0x58))) < 0.01f);
  assert(std::fabs((Float(copy + 0x54) - y0) - (y0 + height - Float(copy + 0x5C))) < 0.01f);
}

// ---------------------------------------------------------------------------------------------------------------
// 1 and 2: the placement and the HUD scale inside and outside a fitted draw.
// ---------------------------------------------------------------------------------------------------------------
void FittedDrawCases() {
  using namespace bo2zm::mod_hud_fit;
  static_assert(std::is_trivially_destructible_v<ClientPlacement>);
  FLAGS_bo2zm_test_mod_hud_trace = false; FLAGS_bo2zm_mod_hud_fit = true; FLAGS_bo2zm_split_even_margins = true;
  for (unsigned count : {2u, 4u}) {
    BeginEpoch(); SetupWorld(count); ValidVm(); EndMapLoad(); PublishViews(count);
    std::array<uint32_t, 4> huds{};
    for (unsigned local = 0; local < count; ++local)
      huds[local] = QualifyText(local, uint16_t(460 + count * 10 + local * 2), uint16_t(461 + count * 10 + local * 2));
    for (unsigned local = 0; local < count; ++local) {
      const uint32_t game_record = ViewRecord(local);
      std::array<uint8_t, 124> game_bytes{}; std::memcpy(game_bytes.data(), memory + game_record, 124);
      const auto seen = Draw(local, huds[local]);
      // Inside the draw both lookups hand out the full-screen record fitted to this view, at the full screen's scale.
      assert(Fitted(seen, local) && seen.by_index == FittedRecord(local));
      CheckFittedRecord(local);
      assert(seen.hud_scale == 1.0f);
      // At the draw's own depth, and after it, the lookups hand out the evened view record and the scale is back.
      assert(seen.at_entry == EvenRecord(local));
      assert(Float(kHudScale) == kSplitHudScale && !active_placement.record);
      PPCContext after{}; after.r1.u64 = stack - kDrawFrame;
      assert(LookupByLocal(after, local) == EvenRecord(local) && LookupByIndex(after, local) == EvenRecord(local));
      CheckEvenRecord(local);
      assert(std::memcmp(game_bytes.data(), memory + game_record, 124) == 0);  // the game's own record is untouched
    }
    if (count == 2) {
      // Two players: the full screen at half size, centred in a full-width view.
      assert(Float(FittedRecord(0) + 0x38) == 320.0f && Float(FittedRecord(0) + 0x40) == 640.0f);
      assert(Float(FittedRecord(0) + 0x3C) == 0.0f && Float(FittedRecord(0) + 0x44) == 360.0f);
    }
    // A changed view record is evened again.
    PutFloat(ViewRecord(1) + 0x44, 300.0f);
    PPCContext after{}; after.r1.u64 = stack;
    assert(LookupByIndex(after, 1) == EvenRecord(1)); CheckEvenRecord(1);
    PutFloat(ViewRecord(1) + 0x44, float(pending_rects[1][3]));

    // Elements that do not qualify draw with the evened view record at the split-screen scale.
    const auto other = Draw(1, huds[1] + 124);  // past the active prefix of the snapshot
    assert(!Fitted(other, 1) && other.by_local == EvenRecord(1) && other.hud_scale == kSplitHudScale);
    auto wrong_caller = DrawContext(0, huds[0]); wrong_caller.lr += 4;
    sub_821DC058(wrong_caller, memory);
    assert(!Fitted(draws.back(), 0) && draws.back().hud_scale == kSplitHudScale);
    FLAGS_bo2zm_mod_hud_fit = false;
    const auto off = Draw(0, huds[0]);
    assert(!Fitted(off, 0) && off.by_local == EvenRecord(0) && off.hud_scale == kSplitHudScale);
    FLAGS_bo2zm_split_even_margins = false;
    const auto stock = Draw(0, huds[0]);  // both settings off: the game's own records
    assert(stock.by_local == ViewRecord(0) && stock.by_index == ViewRecord(0) && stock.at_entry == ViewRecord(0));
    FLAGS_bo2zm_split_even_margins = true;
    FLAGS_bo2zm_mod_hud_fit = true;
  }
  // The full-screen view is never evened.
  BeginEpoch(); SetupWorld(1); EndMapLoad(); PublishViews(1);
  PPCContext single{}; single.r1.u64 = stack;
  assert(LookupByLocal(single, 0) == ViewRecord(0));

  // A guest error unwinding out of a fitted draw leaves the full-screen scale behind; the next draw on the thread puts
  // the split-screen scale back and draws without the fitted placement.
  BeginEpoch(); SetupWorld(2); ValidVm(); EndMapLoad(); PublishViews(2);
  const uint32_t first = QualifyText(0, 490, 491);
  static PPCContext jumping;
  jumping = DrawContext(0, first);
  const auto saved = jumping;
  const auto jumped_calls = draw_calls;
  const int jump_value = ppc_setjmp(0xAD01);
  if (!jump_value) { draw_jumps = true; sub_821DC058(jumping, memory); assert(false); }
  draw_jumps = false;
  assert(jump_value == 7 && draw_calls == jumped_calls + 1 && std::memcmp(&jumping, &saved, sizeof(saved)) == 0);
  assert(Fitted(draws.back(), 0) && draws.back().hud_scale == 1.0f);
  assert(Float(kHudScale) == 1.0f && active_placement.record == FittedRecord(0));
  const auto next = Draw(1, ps_root);  // not a snapshot element: an unfitted draw
  assert(!Fitted(next, 1) && next.hud_scale == kSplitHudScale && next.by_local == EvenRecord(1));
  assert(Float(kHudScale) == kSplitHudScale && !active_placement.record);
  assert(mutex.try_lock()); mutex.unlock();
  std::puts("PASS fitted draw: 2/4 views, full-screen placement and HUD scale 1.0 inside the draw, evened view record and "
            "split scale at its depth and after it, unqualified/disabled draws, longjmp recovery");
}

// ---------------------------------------------------------------------------------------------------------------
// System Link: a game this console hosts fits its local players' menus as a Local game does; an online session, a
// console that joined (no server, no loopback connection) and a game in neither mode keep the elements unfitted.
// ---------------------------------------------------------------------------------------------------------------
void SystemLinkCases() {
  using namespace bo2zm::mod_hud_fit;
  FLAGS_bo2zm_test_mod_hud_trace = false; FLAGS_bo2zm_mod_hud_fit = true; FLAGS_bo2zm_split_even_margins = true;
  // The System Link lobby's modes: the private match game mode (bit 1), a System Link session (bit 1).
  const auto modes = [](uint32_t game, uint32_t session) { Put(0x837F6A80, game); Put(0x837F8CB4, session); };
  for (const auto& [game, session] : {std::pair{2u, 2u}, std::pair{0u, 2u}, std::pair{6u, 3u}}) {
    BeginEpoch(); SetupWorld(2); modes(game, session); ValidVm(); EndMapLoad(); PublishViews(2);
    std::array<uint32_t, 2> huds{QualifyText(0, 430, 431), QualifyText(1, 432, 433)};
    for (unsigned local = 0; local < 2; ++local) {
      const auto seen = Draw(local, huds[local]);
      assert(Fitted(seen, local) && seen.hud_scale == 1.0f);
      CheckFittedRecord(local);
    }
  }
  // Online (bit 2 of the session), neither a Local nor a System Link game, a console without a running server, and
  // one whose connection is not the loopback one: no qualification and no fitting.
  const uint32_t server_dvar = clients + 0x28000, connection = 0x92000000 + 0x358A8 + 0x24368;
  for (unsigned gate = 0; gate < 5; ++gate) {
    BeginEpoch(); SetupWorld(2); modes(2, 2); ValidVm(); EndMapLoad(); PublishViews(2);
    if (gate == 0) modes(2, 6);
    if (gate == 1) modes(2, 4);
    if (gate == 2) modes(4, 0);
    if (gate == 3) memory[server_dvar + 0x18] = 0;
    if (gate == 4) Put(connection, 4);
    const uint32_t ps = ps_root + numbers[1] * ps_stride;
    auto creation = CreationContext(numbers[1]); sub_8232A280(creation, memory);
    AuthorPayload(kPool, 1); PutShort(memory + kPool, 0x64, 434); PutShort(memory + kPool, 0x56, 0);
    auto copy = Copy(kPool); copy.r3.u64 = ps + 3068; rexcrt_bo2zm_memcpy(copy, memory);
    auto scan = Copy(ps, 0x82439010, 10760); scan.r3.u64 = clients + 0x20000; rexcrt_bo2zm_xmemcpy(scan, memory);
    assert(!owner_ready.test(numbers[1]));
    const uint32_t cg = cg_array + 0x87700, snapshot = cg + 0x80;
    std::memcpy(memory + snapshot + 16, memory + ps, 10760); Put(cg + 40, snapshot);
    const auto seen = Draw(1, snapshot + 16 + 3068);
    assert(!Fitted(seen, 1) && seen.hud_scale == kSplitHudScale);
    memory[server_dvar + 0x18] = 1; Put(connection, 2);
  }
  modes(4, 1);
  std::puts("PASS System Link: a hosted game's local views fit as in a Local game; online, other modes, no server and a "
            "joined (non-loopback) connection stay unfitted");
}

// ---------------------------------------------------------------------------------------------------------------
// System Link, the consoles that joined: the host publishes each player's menu elements by snapshot slot, and a
// joined console (no server, no loopback connection) fits only an element its table names in that very slot.
// ---------------------------------------------------------------------------------------------------------------
std::vector<bo2zm::mod_hud_wire::Table> Tables() {
  std::vector<bo2zm::mod_hud_wire::Table> tables(32);
  tables.resize(bo2zm::mod_hud_fit::HostTables(tables.data(), tables.size()));
  return tables;
}
const bo2zm::mod_hud_wire::Table* TableFor(const std::vector<bo2zm::mod_hud_wire::Table>& tables, uint32_t client) {
  for (const auto& table : tables) if (table.client == client) return &table;
  return nullptr;
}
// A material menu element in a player's playerstate slot, proven by its live copy and the snapshot copy.
void QualifyMaterial(unsigned local, uint32_t slot) {
  using namespace bo2zm::mod_hud_fit;
  ValidVm();
  const uint32_t ps = ps_root + numbers[local] * ps_stride;
  auto creation = CreationContext(numbers[local]); sub_8232A280(creation, memory);
  AuthorPayload(kPool, 8); PutShort(memory + kPool, 0x64, 0); PutShort(memory + kPool, 0x56, 0);
  PutShort(memory + kPool, 0x58, int16_t(700 + slot));
  auto copy = Copy(kPool); copy.r3.u64 = ps + 3068 + slot * 124;
  rexcrt_bo2zm_memcpy(copy, memory);
  auto scan = Copy(ps, 0x82439010, 10760); scan.r3.u64 = clients + 0x20000;
  rexcrt_bo2zm_xmemcpy(scan, memory);
}
// Turns the fixture's console into one that joined another's System Link game: no server records, no running
// server, a connection that is not the loopback one. Each view's snapshot holds the given playerstate.
void BecomeJoiner() {
  Put(0x837F6A80, 2); Put(0x837F8CB4, 2);
  memory[clients + 0x28000 + 0x18] = 0;
  for (unsigned local = 0; local < 4; ++local) {
    Put(0x92000000 + local * 0x358A8 + 0x24368, 4);
    const uint32_t entity = 0x833D0640 + numbers[local] * 796;
    memory[entity + 251] = 0; Put(entity + 340, 0);
  }
  Put(0x83B44E24, 0); Put(0x83B44E28, 0);
}
uint32_t JoinedSnapshot(unsigned local, uint32_t from_ps) {
  const uint32_t cg = cg_array + local * 0x87700, snapshot = cg + 0x80;
  std::memcpy(memory + snapshot + 16, memory + from_ps, 10760); Put(cg + 40, snapshot);
  return snapshot + 16;
}
void JoinedCases() {
  using namespace bo2zm::mod_hud_fit;
  namespace wire = bo2zm::mod_hud_wire;
  FLAGS_bo2zm_test_mod_hud_trace = false; FLAGS_bo2zm_mod_hud_fit = true; FLAGS_bo2zm_split_even_margins = true;
  ResetJoinedTables();

  // The host: a System Link game it hosts publishes each player's text and material menu elements by slot.
  BeginEpoch(); SetupWorld(2); Put(0x837F6A80, 2); Put(0x837F8CB4, 2); ValidVm(); EndMapLoad(); PublishViews(2);
  QualifyText(1, 440, 441);
  QualifyMaterial(1, 1);
  const uint32_t host_ps = ps_root + numbers[1] * ps_stride;
  auto tables = Tables();
  const auto* table = TableFor(tables, numbers[1]);
  assert(table && table->count == 2 && table->client == numbers[1] && table->epoch == uint32_t(epoch));
  assert(table->entries[0].slot == 0 && table->entries[0].id == wire::IdOf(memory + host_ps + 3068));
  assert(table->entries[1].slot == 1 && table->entries[1].id == wire::IdOf(memory + host_ps + 3068 + 124));
  assert(!TableFor(tables, numbers[0]));  // a player without a menu has no table
  const auto host_table = *table;
  // Nothing changed: the same revision. A stock text ID in the menu's slot: published without it.
  auto scan = Copy(host_ps, 0x82439010, 10760); scan.r3.u64 = clients + 0x20000;
  rexcrt_bo2zm_xmemcpy(scan, memory);
  assert(TableFor(Tables(), numbers[1])->revision == host_table.revision);
  stock_ids.set(441); rexcrt_bo2zm_xmemcpy(scan, memory);
  {
    const auto now = Tables();
    const auto* vetoed = TableFor(now, numbers[1]);
    assert(vetoed && vetoed->revision != host_table.revision && (vetoed->count == 0 ||
           (vetoed->count == 1 && vetoed->entries[0].slot == 1)));
  }
  // A Local game, or no running server: nothing published.
  for (unsigned gate = 0; gate < 2; ++gate) {
    BeginEpoch(); SetupWorld(2); ValidVm(); EndMapLoad(); PublishViews(2);
    if (gate == 1) { Put(0x837F6A80, 2); Put(0x837F8CB4, 2); memory[clients + 0x28000 + 0x18] = 0; }
    QualifyMaterial(1, 0);
    assert(Tables().empty());
  }
  // A player who dies and watches another: the game copies the followed player's playerstate over theirs, and their
  // table empties.
  BeginEpoch(); SetupWorld(2); Put(0x837F6A80, 2); Put(0x837F8CB4, 2); ValidVm(); EndMapLoad(); PublishViews(2);
  QualifyMaterial(1, 0);
  assert(TableFor(Tables(), numbers[1]) && TableFor(Tables(), numbers[1])->count == 1);
  auto follow = Copy(ps_root + numbers[0] * ps_stride, 0x8230369C, 10760); follow.r3.u64 = host_ps;
  rexcrt_bo2zm_xmemcpy(follow, memory);
  assert(TableFor(Tables(), numbers[1]) && TableFor(Tables(), numbers[1])->count == 0);

  // The joined console: the same elements in its snapshot, the host's table stored.
  BeginEpoch(); SetupWorld(2); Put(0x837F6A80, 2); Put(0x837F8CB4, 2); ValidVm(); EndMapLoad(); PublishViews(2);
  stock_ids.reset(441);
  QualifyText(1, 442, 443); QualifyMaterial(1, 1);
  const auto published_table = *TableFor(Tables(), numbers[1]);
  std::vector<uint8_t> host_bytes(memory + host_ps, memory + host_ps + 10760);
  BeginEpoch();  // the joiner holds none of the host's evidence
  SetupLayout(2); AuthorPlacements(2); BecomeJoiner(); PublishViews(2);
  std::memcpy(memory + host_ps, host_bytes.data(), host_bytes.size());  // only as the snapshot source
  const uint32_t ps1 = JoinedSnapshot(1, host_ps);
  const uint32_t text = ps1 + 3068, material = ps1 + 3068 + 124;
  // Without a table: unfitted.
  assert(!Fitted(Draw(1, text), 1) && !Fitted(Draw(1, material), 1));
  // Encoded and decoded as the side channel carries it, then stored.
  std::array<uint8_t, wire::kMaxBody> body{};
  const size_t size = wire::Encode(published_table, body.data());
  wire::Table received;
  assert(size && wire::Decode(wire::kFormat, published_table.count, published_table.revision, body.data(), size,
                              received));
  StoreJoinedTable(received);
  for (const uint32_t hud : {text, material}) {
    const auto& seen = Draw(1, hud);
    assert(Fitted(seen, 1) && seen.hud_scale == 1.0f);
    CheckFittedRecord(1);
  }
  // An element the table does not name: another slot, a changed text, a changed material, past the active prefix.
  std::memcpy(memory + ps1 + 3068 + 2 * 124, memory + material, 124);
  assert(!Fitted(Draw(1, ps1 + 3068 + 2 * 124), 1));
  memory[ps1 + 3068 + 2 * 124 + 0x70] = 0;
  PutShort(memory + text, 0x64, 444); assert(!Fitted(Draw(1, text), 1)); PutShort(memory + text, 0x64, 442);
  PutShort(memory + material, 0x58, 5); assert(!Fitted(Draw(1, material), 1)); PutShort(memory + material, 0x58, 701);
  assert(Fitted(Draw(1, material), 1));
  PutScalar(memory + text, 0, 33.0f); assert(Fitted(Draw(1, text), 1));  // moved: still the menu's element
  // Another client's snapshot (player 0's, who has no table), or the playerstate of another client number.
  const uint32_t ps0 = JoinedSnapshot(0, host_ps);
  Put(ps0 + 416, numbers[0]);
  assert(!Fitted(Draw(0, ps0 + 3068), 0));
  // A dead player watching player 1: their snapshot carries player 1's number and elements, fitted in their view.
  Put(ps0 + 416, numbers[1]);
  assert(Fitted(Draw(0, ps0 + 3068), 0));
  Put(ps0 + 416, numbers[0]);
  // The setting off, one view only, or this console running the server: the joined path stays out.
  FLAGS_bo2zm_mod_hud_fit = false; assert(!Fitted(Draw(1, text), 1)); FLAGS_bo2zm_mod_hud_fit = true;
  Put(0x837F6A28, 0);  // player 0 gone: a single view
  assert(!Fitted(Draw(1, text), 1));
  Put(0x837F6A28, 1);
  assert(Fitted(Draw(1, text), 1));
  // Stale: no news for longer than the host's resends allow.
  for (auto& entry : joined) if (entry.used) entry.received -= std::chrono::seconds(4);
  assert(!Fitted(Draw(1, text), 1));
  StoreJoinedTable(received);  // the same revision again refreshes it
  assert(Fitted(Draw(1, text), 1));
  // An older revision does not replace a newer one; an emptied table stops the fitting.
  auto empty = received; empty.count = 0; empty.revision = received.revision + 1;
  StoreJoinedTable(empty);
  assert(!Fitted(Draw(1, text), 1) && joined_count.load() == 0);
  StoreJoinedTable(received);
  assert(!Fitted(Draw(1, text), 1));
  // A table from the host's earlier map is ignored; a newer map's or another host's replaces everything.
  auto newer = received; newer.revision = received.revision + 2;
  StoreJoinedTable(newer); assert(Fitted(Draw(1, text), 1));
  auto old_map = newer; old_map.epoch = newer.epoch - 1; old_map.revision = newer.revision + 1; old_map.client = 7;
  StoreJoinedTable(old_map);
  for (const auto& entry : joined) assert(!entry.used || entry.table.client != 7);
  auto next_map = newer; next_map.epoch = newer.epoch + 1; next_map.client = 9;
  StoreJoinedTable(next_map);
  assert(!Fitted(Draw(1, text), 1));  // player 1's table was the old map's
  auto other_host = newer; other_host.session ^= 0x5A5A;
  StoreJoinedTable(other_host); assert(Fitted(Draw(1, text), 1));
  // Leaving the joined game: every table goes.
  ResetJoinedTables();
  assert(!Fitted(Draw(1, text), 1) && joined_count.load() == 0);
  // A console with its own server never takes the joined path, even with a table held: without its own evidence
  // the element stays unfitted.
  StoreJoinedTable(other_host);
  assert(Fitted(Draw(1, text), 1));
  memory[clients + 0x28000 + 0x18] = 1;
  assert(!Fitted(Draw(1, text), 1));
  memory[clients + 0x28000 + 0x18] = 0;
  ResetJoinedTables();
  std::puts("PASS System Link joined: host tables by slot (text and material, stock veto, Local/no-server publish "
            "nothing, emptied on the spectator overwrite), joined draws fitted only for the named slot and element, "
            "spectating, setting off, single view, stale, older revision, emptied, older map, new host, reset");
}

// ---------------------------------------------------------------------------------------------------------------
// 3: material quads in a fitted two-player view (1280x360; the full screen shrunk to 640x360 at x 320).
// ---------------------------------------------------------------------------------------------------------------
void QuadCases() {
  using namespace bo2zm::mod_hud_fit;
  FLAGS_bo2zm_test_mod_hud_trace = false; FLAGS_bo2zm_mod_hud_fit = true;
  BeginEpoch(); SetupWorld(2); ValidVm(); EndMapLoad(); PublishViews(2);
  const uint32_t hud = QualifyText(0, 500, 501);
  const auto as_float = [](const Quad& quad) {
    Quad out{}; for (size_t i = 0; i < out.size(); ++i) out[i] = double(float(quad[i])); return out;
  };
  const Quad off_left{100, 100, 100, 50, 0, 0, 1, 1};       // inside the view, wholly left of the full screen
  const Quad off_below{400, 400, 50, 50, 0, 0, 1, 1};       // wholly below the full screen
  const Quad straddle_bottom{400, 300, 100, 100, 0, 0, 1, 1};  // past the view's bottom edge
  const Quad straddle_left{-100, 100, 500, 50, 0, 0, 1, 1};    // past the view's left edge
  const Quad to_right_edge{900, 100, 100, 50, 0.25, 0, 0.75, 1};  // reaches the full screen's right edge
  const Quad inside{400, 100, 100, 50, 0, 0, 1, 1};
  quads_in_draw = {off_left, off_below, straddle_bottom, straddle_left, to_right_edge, inside};
  quads_at_entry = {off_left};
  drawn_quads.clear();
  const auto seen = Draw(0, hud);
  assert(Fitted(seen, 0));
  const std::vector<Quad> expected = {
      as_float({400, 300, 100, 60, 0, 0, 1, 0.6}),     // cut to the view, texture cut in proportion
      as_float({0, 100, 400, 50, 0.2, 0, 1, 1}),
      as_float({900, 100, 380, 50, 0.25, 0, 0.75, 1}),  // carried on to the view's edge, texture stretched
      as_float(inside),
      off_left,                                         // at the draw's own depth: as submitted
  };
  assert(drawn_quads == expected);
  // Outside any fitted draw, and in a draw that is not fitted, quads pass through as submitted.
  drawn_quads.clear();
  PPCContext outside{}; outside.r1.u64 = stack - kDrawFrame;
  SubmitQuad(outside, off_left);
  assert(drawn_quads.size() == 1 && drawn_quads[0] == off_left);
  drawn_quads.clear();
  const auto unfitted = Draw(0, hud + 124);
  assert(!Fitted(unfitted, 0));
  assert(drawn_quads.size() == quads_in_draw.size() + 1 && drawn_quads[0] == off_left && drawn_quads[2] == straddle_bottom);
  quads_in_draw.clear(); quads_at_entry.clear();
  std::puts("PASS fitted quads: off-screen dropped, straddling cut to the view with texture cut, full-screen edge "
            "carried to the view's edge, entry-depth and unfitted quads untouched");
}

// ---------------------------------------------------------------------------------------------------------------
// 4: the safe area for a split-screen view.
// ---------------------------------------------------------------------------------------------------------------
std::array<float, 4> SafeArea(std::array<float, 4> game) {
  game_safe_area = game;
  PPCContext ctx{}; ctx.r3.u64 = lua_state;
  sub_8271F3F0(ctx, memory);
  assert(ctx.r3.u64 == safe_area_count);
  std::array<float, 4> out{};
  for (uint32_t i = 0; i < 4; ++i) out[i] = Float(lua_top - 8 * (4 - i));
  return out;
}
// Low..high inside -half..half with the same margin on both sides, bo2zm_split_margin_percent of the view.
void CheckEven(float low, float high, float half) {
  const float margin = 2.0f * half * float(FLAGS_bo2zm_split_margin_percent / 100.0);
  assert(low == -half + margin && high == half - margin);
}
void SafeAreaCases() {
  FLAGS_bo2zm_split_even_margins = true;
  // Four players, top-left view: the game keeps the margin only on the screen's outer edges.
  auto area = SafeArea({-448, -252, 640, 360});
  CheckEven(area[0], area[2], 640); CheckEven(area[1], area[3], 360);
  // The other three views of four come back the same.
  for (auto game : {std::array<float, 4>{-640, -252, 448, 360}, std::array<float, 4>{-448, -360, 640, 252},
                    std::array<float, 4>{-640, -360, 448, 252}})
    assert(SafeArea(game) == area);
  // Two players, full-width views: off-centre only vertically.
  const auto top = SafeArea({-544, -252, 544, 360}), bottom = SafeArea({-544, -360, 544, 252});
  for (const auto& two : {top, bottom}) {
    assert(two[0] == -two[2] && two[1] == -two[3]);  // even on both axes
    CheckEven(two[1], two[3], 360);
    assert(two[0] >= -544 && two[2] <= 544);
  }
  assert(top == bottom);
  // A centred (full-screen) area, another count of values, a value that is not a number, or the setting off: the
  // game's own values.
  assert((SafeArea({-544, -306, 544, 306}) == std::array<float, 4>{-544, -306, 544, 306}));
  safe_area_count = 3; assert((SafeArea({-448, -252, 640, 360}) == std::array<float, 4>{-448, -252, 640, 360}));
  safe_area_count = 4;
  safe_area_type = 4; assert((SafeArea({-448, -252, 640, 360}) == std::array<float, 4>{-448, -252, 640, 360}));
  safe_area_type = 3;
  FLAGS_bo2zm_split_even_margins = false;
  assert((SafeArea({-448, -252, 640, 360}) == std::array<float, 4>{-448, -252, 640, 360}));
  FLAGS_bo2zm_split_even_margins = true;
  std::puts("PASS safe area: four-player and two-player views evened, full-screen/other/disabled answers untouched");
}

// ---------------------------------------------------------------------------------------------------------------
// 5: isHost for the loaded mod's scripts in a Local game.
// ---------------------------------------------------------------------------------------------------------------
uint32_t IsHost(uint32_t number, uint32_t kind = 0) {
  PPCContext ctx{}; ctx.r3.u64 = (uint64_t(number) << 48) | (uint64_t(kind) << 32);
  const auto pushes = pushed_ints.size();
  sub_8231DD80(ctx, memory);
  assert(pushed_ints.size() == pushes + 1 && pushed_ints.back()[0] == 0);
  return pushed_ints.back()[1];
}
void LocalHostCases() {
  using namespace bo2zm::mod_hud_fit;
  BeginEpoch(); SetupWorld(2); ValidVm(); EndMapLoad(); PublishViews(2);
  FLAGS_bo2zm_mod_local_host = true;
  auto calls = stock_host_calls;
  assert(IsHost(numbers[1]) == 1 && stock_host_calls == calls);  // the second local player, from the mod's menu
  assert(IsHost(numbers[0]) == 1 && stock_host_calls == calls);
  assert(IsHost(20) == 0 && stock_host_calls == ++calls);         // not a local player (a bot)
  assert(IsHost(numbers[1], 1) == 0 && stock_host_calls == ++calls);  // not an entity
  Put(bo2zm::mod_hud_fit::kFrames + 20, 0x13081);                   // the game's own script asks
  assert(IsHost(numbers[1]) == 0 && stock_host_calls == ++calls);
  ValidVm();
  Put(0x92000000 + 0x358A8 + 0x24368, 4);                         // not a Local game
  assert(IsHost(numbers[1]) == 0 && stock_host_calls == ++calls);
  Put(0x92000000 + 0x358A8 + 0x24368, 2);
  FLAGS_bo2zm_mod_local_host = false;
  assert(IsHost(numbers[1]) == 0 && stock_host_calls == ++calls);  // the game's own answer
  assert(IsHost(numbers[0]) == 1 && stock_host_calls == ++calls);
  FLAGS_bo2zm_mod_local_host = true;
  assert(IsHost(numbers[1]) == 1);
  std::puts("PASS isHost: every local player for the mod's scripts with the setting on; bots, other classes, the "
            "game's scripts, non-Local games and the setting off keep the game's answer");
}

// ---------------------------------------------------------------------------------------------------------------
// Provenance: text ID evidence, completed copies and a producer that is proven before its viewport is ready.
// ---------------------------------------------------------------------------------------------------------------
void CompletedEvidenceCases();
void ColdProducerCases();
void IdProbeCases() {
  using namespace bo2zm::mod_hud_fit;
  FLAGS_bo2zm_test_mod_hud_trace = true;
  FLAGS_bo2zm_mod_hud_fit = false;
  BeginEpoch(); SetupLayout(2); ValidVm(); EndMapLoad(); PublishViews(2);
  const uint32_t connection_array = 0x92000000, server_dvar = clients + 0x28000;
  Put(0x837F6A80, 4); Put(0x837F8CB4, 1); Put(0x837F8DC0, server_dvar);
  Put(server_dvar + 0x10, 1); memory[server_dvar + 0x18] = 1;
  Put(0x82C70F50, connection_array); Put(0x84456C84, 0);
  Put(connection_array + 0x24368, 2); Put(connection_array + 0x358A8 + 0x24368, 2);
  auto creation = CreationContext(numbers[0]); sub_8232A280(creation, memory);
  AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 11); PutShort(memory + kPool, 0x56, 17);
  std::array<uint8_t, 136> source{}; std::memcpy(source.data(), memory + kPool, source.size());
  auto copy = Copy(kPool); copy.r3.u64 = clients + 3068;
  std::memset(memory + copy.r3.u32, 0xD7, 124);
  BeforeCopy(copy, memory);
  assert(menu_body_ids[numbers[0]].test(11) && !stock_ids.test(11) && !stock_ids.test(17));
  assert(memory[copy.r3.u32] == 0xD7);  // metadata is published before raw transport
  // Observe the actual CRT path's first publication while the old destination
  // marker is still present. Moving the callback after memmove fails this.
  PutShort(memory + kPool, 0x64, 13);
  watched_copy_destination = copy.r3.u32; require_pre_copy_log = true;
  rexcrt_bo2zm_memcpy(copy, memory);
  require_pre_copy_log = false; assert(observed_pre_copy_log);
  std::memcpy(memory + kPool, source.data(), source.size());
  const auto registers = copy;
  rexcrt_bo2zm_memcpy(copy, memory);
  assert(std::memcmp(&copy, &registers, sizeof(copy)) == 0);
  assert(std::memcmp(memory + copy.r3.u32, source.data(), 124) == 0);
  assert(std::memcmp(memory + kPool, source.data(), source.size()) == 0);

  // Model decoded data in the actual alternating CG buffer, not the server PS.
  const uint32_t snapshot = cg_array + 0x80, ps = snapshot + 16;
  Put(cg_array + 40, snapshot); Put(ps + 416, numbers[0]);
  std::memset(memory + ps + 3068, 0, 62 * 124);
  expected_draw_hud = ps + 3068;
  std::memcpy(memory + expected_draw_hud, source.data(), 124);
  PPCContext draw = DrawContext(0, expected_draw_hud);
  const auto observe = [&] {
    std::array<uint8_t, 124> before{}; std::memcpy(before.data(), memory + expected_draw_hud, 124);
    auto ctx = draw; const auto calls = draw_calls;
    sub_821DC058(ctx, memory);
    auto expected = draw; expected.r3.u64 = 0xFEDCBA; expected.f1.f64 = 42.5;
    assert(draw_calls == calls + 1 && std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
    assert(std::memcmp(before.data(), memory + expected_draw_hud, 124) == 0);
  };
  // A retained decoded record can be seen before the new positive history.
  // The diagnostic must report deny -> candidate -> later veto-driven deny.
  BeginEpoch(); SetupLayout(2); ValidVm(); EndMapLoad(); PublishViews(2);
  observe(); assert(denied_draw_reports == 1 && allowed_draw_reports == 0);
  creation = CreationContext(numbers[0]); sub_8232A280(creation, memory); AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 11); PutShort(memory + kPool, 0x56, 17);
  BeforeCopy(copy, memory);
  observe(); assert(allowed_draw_reports == 1);
  const auto accepted = allowed_draw_reports;
  auto wrong = draw; wrong.r4.u64 += 1; ObserveClientDraw(wrong, memory);
  wrong = draw; wrong.r4.u64 = clients + 3068; ObserveClientDraw(wrong, memory);
  wrong = draw; wrong.r3.u64 = 1; ObserveClientDraw(wrong, memory);
  wrong = draw; wrong.lr += 4; ObserveClientDraw(wrong, memory);
  memory[expected_draw_hud + 0x70] = 0;
  wrong = draw; wrong.r4.u64 += 124; ObserveClientDraw(wrong, memory);
  memory[expected_draw_hud + 0x70] = 1;
  assert(allowed_draw_reports == accepted);

  // Unknown/global label IDs veto a mod BODY ID across all owners. This must
  // work after the creation tag is cleared and with logging already saturated.
  Clear(kPool); Put(kPool + 124, 1023);
  PutShort(memory + kPool, 0x64, 19); PutShort(memory + kPool, 0x56, 11);
  veto_id_reports = 64; BeforeCopy(copy, memory);
  assert(stock_ids.test(11) && stock_ids.test(19));
  observe(); assert(denied_draw_reports == 2 && allowed_draw_reports == accepted);
  // A stock body also vetoes a label used by the mod.
  PutShort(memory + kPool, 0x64, 17); PutShort(memory + kPool, 0x56, 0);
  BeforeCopy(copy, memory); assert(stock_ids.test(17));
  observe(); assert(denied_draw_reports == 3);
  uint64_t session = 0;
  assert(LocalListenScope(memory, 0, session) && session);
  for (uint32_t gate : {0x837F6A80u, 0x837F8CB4u, 0x84456C84u, connection_array + 0x24368, server_dvar + 0x10}) {
    const auto before = Word(memory, gate);
    Put(gate, gate == 0x84456C84 ? 2 : 0);
    assert(!LocalListenScope(memory, 0, session));
    ObserveClientDraw(draw, memory); assert(menu_body_ids[numbers[0]].test(11));
    Put(gate, before);
  }
  memory[server_dvar + 0x18] = 0;
  assert(!LocalListenScope(memory, 0, session));
  ObserveClientDraw(draw, memory);  // shutdown/disconnect cannot retain positives
  memory[server_dvar + 0x18] = 1;

  // Positive history is owner-scoped and cannot undo prior negative evidence.
  creation = CreationContext(numbers[1]); sub_8232A280(creation, memory); AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 23); PutShort(memory + kPool, 0x56, 0);
  positive_id_reports = 64; rexcrt_bo2zm_memcpy(copy, memory);
  assert(menu_body_ids[numbers[1]].test(23) && !menu_body_ids[numbers[0]].test(23));
  creation = CreationContext(numbers[0]); sub_8232A280(creation, memory); AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 11); PutShort(memory + kPool, 0x56, 0);
  rexcrt_bo2zm_memcpy(copy, memory);
  assert(menu_body_ids[numbers[0]].test(11) && stock_ids.test(11));

  // Bot/shared ancestry, global ownership and missing epoch tags are all
  // negative evidence even before map readiness or a local view is available.
  BeginEpoch(); ValidVm();
  Put(kFrames + 40, 0x12001);
  creation = CreationContext(numbers[0]); sub_8232A280(creation, memory); AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 43); PutShort(memory + kPool, 0x56, 47);
  BeforeCopy(copy, memory);
  assert(stock_ids.test(43) && stock_ids.test(47) && !menu_body_ids[numbers[0]].test(43));
  ValidVm(); creation = CreationContext(1023); sub_8232A280(creation, memory); AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 53); PutShort(memory + kPool, 0x56, 59);
  BeforeCopy(copy, memory); assert(stock_ids.test(53) && stock_ids.test(59));
  BeginEpoch(); PutShort(memory + kPool, 0x64, 61); PutShort(memory + kPool, 0x56, 67);
  BeforeCopy(copy, memory); assert(stock_ids.test(61) && stock_ids.test(67));

  // The exact unknown tail-copy source is a full PS. Even trailing or type 0
  // slots can only add vetoes; this source never becomes a positive.
  const uint32_t archive = clients + 0x30000, destination = clients + 0x20000;
  std::memset(memory + archive, 0, 10760);
  PutShort(memory + archive + 3068, 0x64, 29);
  memory[archive + 3068 + 0x70] = 1;
  PutShort(memory + archive + 3068 + 30 * 124, 0x56, 31);
  PutShort(memory + archive + 6912 + 30 * 124, 0x64, 37);
  auto archived_copy = Copy(archive, 0x82439010, 10760); archived_copy.r3.u64 = destination;
  const auto archived_registers = archived_copy;
  rexcrt_bo2zm_xmemcpy(archived_copy, memory);
  assert(std::memcmp(&archived_copy, &archived_registers, sizeof(archived_copy)) == 0);
  assert(std::memcmp(memory + archive, memory + destination, 10760) == 0);
  assert(stock_ids.test(29) && !stock_ids.test(31) && !stock_ids.test(37));
  assert(!menu_body_ids[numbers[0]].test(29));
  auto known_ps = Copy(clients, 0x82439010, 10760); known_ps.r3.u64 = destination;
  BeforeCopy(known_ps, memory);
  assert(logs.back().find("source_local=0 source_owner=9") != std::string::npos);
  PutShort(memory + archive + 3068, 0x64, 41);
  archived_copy.lr += 4; BeforeCopy(archived_copy, memory); assert(!stock_ids.test(41));
  archived_copy.lr = 0x82439010; archived_copy.r5.u64 = 10759;
  BeforeCopy(archived_copy, memory); assert(!stock_ids.test(41));

  // Copies are never changed: a menu material reaches the playerstate as created.
  FLAGS_bo2zm_mod_hud_fit = true;
  SetupLayout(2); ValidVm(); EndMapLoad(); PublishViews(2);
  creation = CreationContext(numbers[0]); sub_8232A280(creation, memory);
  AuthorPayload(kPool, 8); PutShort(memory + kPool, 0x64, 0); PutShort(memory + kPool, 0x56, 0);
  rexcrt_bo2zm_memcpy(copy, memory);
  assert(!owner_ready.test(numbers[0]) && std::memcmp(memory + copy.r3.u32, memory + kPool, 124) == 0);
  BeginEpoch(); Clear(kPool);
  assert(stock_ids.test(11) && stock_ids.test(17) && stock_ids.test(29));
  assert(!menu_body_ids[numbers[0]].test(11));
  CompletedEvidenceCases();
  ColdProducerCases();
  // Malformed observed producers close the whole diagnostic admission gate;
  // there is no permissive skip that could authorize their unobserved IDs.
  auto invalid = Copy(0xFFFFF000, 0x82439010, 10760);
  BeforeCopy(invalid, memory); assert(id_observation_failed);
  BeginEpoch(); assert(id_observation_failed && stock_ids.test(11));
  FLAGS_bo2zm_test_mod_hud_trace = false;
  BeforeCopy(invalid, memory);  // disabled diagnostics don't access guest input
  std::puts("PASS production pre-copy ID probe: body/label cross-veto, owner isolation, lifetime negatives, unknown PS "
            "groups/aliases, fresh Local scope, byte-original copies and unchanged draws");
}
void CompletedEvidenceCases() {
  using namespace bo2zm::mod_hud_fit;
  FLAGS_bo2zm_mod_hud_fit = false;
  BeginEpoch(); SetupLayout(2); ValidVm();
  constexpr uint32_t root = ps_root, stride = ps_stride, output = clients + 0x20000;
  Put(0x83B44E24, root); Put(0x83B44E28, stride);
  const uint32_t ps = root + numbers[0] * stride, other = root + numbers[1] * stride;
  for (unsigned local = 0; local < 2; ++local) {
    const uint32_t at = root + numbers[local] * stride;
    std::memset(memory + at, 0, stride);
    Put(at + 416, numbers[local]); Put(0x833D0640 + numbers[local] * 796 + 340, at);
  }
  EndMapLoad(); PublishViews(2);
  auto full = Copy(ps, 0x82439010, 10760); full.r3.u64 = output;
  auto pack = [&](uint16_t body, uint16_t label = 0, uint8_t type = 1) {
    auto creation = CreationContext(numbers[0]); sub_8232A280(creation, memory);
    AuthorPayload(kPool, type); PutShort(memory + kPool, 0x64, body); PutShort(memory + kPool, 0x56, label);
    auto ctx = Copy(kPool); ctx.r3.u64 = ps + 3068;
    rexcrt_bo2zm_memcpy(ctx, memory);
    return ctx;
  };
  PsIdentity identity; size_t slot = 99;
  assert(RegisteredPs(memory, ps, identity) && identity.owner == numbers[0] && identity.root == root);
  assert(!RegisteredPs(memory, ps + 1, identity));
  auto copy = pack(300, 301);
  assert(RegisteredSlot(memory, copy.r3.u32, identity, slot) && slot == 0);
  auto* bank = EvidenceFor(identity, false); assert(bank && bank->slots[0].complete && bank->slots[0].menu);
  assert(std::memcmp(bank->slots[0].bytes.data(), memory + copy.r3.u32, 124) == 0);
  std::array<uint8_t, 10760> source{}; std::memcpy(source.data(), memory + ps, source.size());
  const auto registers = full;
  rexcrt_bo2zm_xmemcpy(full, memory);
  assert(!stock_ids.test(300) && !stock_ids.test(301));
  assert(std::memcmp(&full, &registers, sizeof(full)) == 0);
  assert(std::memcmp(memory + ps, source.data(), source.size()) == 0);
  assert(std::memcmp(memory + output, source.data(), source.size()) == 0);
  // The reference is immutable destination bytes, not a mutable pool pointer.
  PutScalar(memory + kPool, 0, 999); Clear(kPool); BeforeCopy(full, memory);
  assert(!stock_ids.test(300) && !stock_ids.test(301));
  memory[ps + 3068 + 123] ^= 1; BeforeCopy(full, memory);
  assert(stock_ids.test(300) && stock_ids.test(301));

  // Same payload at another slot/group/owner never borrows the reference.
  copy = pack(310);
  std::memcpy(memory + ps + 3068 + 124, memory + ps + 3068, 124);
  BeforeCopy(full, memory); assert(stock_ids.test(310));
  std::memset(memory + ps + 3068 + 124, 0, 124);
  copy = pack(320);
  std::memcpy(memory + ps + 6912, memory + ps + 3068, 124);
  BeforeCopy(full, memory); assert(stock_ids.test(320));
  std::memset(memory + ps + 6912, 0, 124);
  copy = pack(330);
  std::memcpy(memory + other + 3068, memory + ps + 3068, 124);
  auto other_full = full; other_full.r4.u64 = other;
  BeforeCopy(other_full, memory); assert(stock_ids.test(330));

  // New in-flight writes invalidate the prior complete occupant immediately.
  copy = pack(340); BeforeCopy(copy, memory); BeforeCopy(full, memory);
  assert(stock_ids.test(340));
  copy = pack(350); BeginEpoch(); EndMapLoad(); PublishViews(2);
  BeforeCopy(full, memory); assert(stock_ids.test(350));
  copy = pack(360); Put(0x83B44E28, 10760); BeforeCopy(full, memory);
  assert(stock_ids.test(360)); Put(0x83B44E28, stride);

  // Completed bytes are the copy's own bytes, with fitting on as well.
  BeginEpoch(); EndMapLoad(); PublishViews(2);
  copy = pack(365); BeforeCopy(full, memory);
  FLAGS_bo2zm_mod_hud_fit = true;
  copy = pack(0, 0, 8);
  assert(RegisteredSlot(memory, copy.r3.u32, identity, slot)); bank = EvidenceFor(identity, false);
  assert(bank && std::memcmp(bank->slots[slot].bytes.data(), memory + kPool, 124) == 0);
  assert(std::memcmp(memory + copy.r3.u32, memory + kPool, 124) == 0);
  FLAGS_bo2zm_mod_hud_fit = false;

  // Reallocation between the pre/post callbacks cannot lend a new menu tag.
  copy = pack(370); BeforeCopy(copy, memory);
  std::memmove(memory + copy.r3.u32, memory + copy.r4.u32, 124);
  auto creation = CreationContext(numbers[0]); sub_8232A280(creation, memory);
  AuthorPayload(kPool, 1); PutShort(memory + kPool, 0x64, 370); PutShort(memory + kPool, 0x56, 0);
  AfterCopy(copy, memory);
  BeforeCopy(full, memory); assert(stock_ids.test(370));

  // Report saturation is unrelated to completed-record capture or matching.
  positive_id_reports = veto_id_reports = archive_id_reports = menu_ps_reports = 64;
  copy_reports = menu_copy_reports = 32;
  copy = pack(380); BeforeCopy(full, memory); assert(!stock_ids.test(380));
  copy = pack(400); Clear(kPool); rexcrt_bo2zm_memcpy(copy, memory);
  assert(RegisteredSlot(memory, copy.r3.u32, identity, slot)); bank = EvidenceFor(identity, false);
  assert(bank && bank->slots[slot].complete && !bank->slots[slot].menu);
  BeforeCopy(full, memory); assert(stock_ids.test(400));
  copy = pack(410); Put(0x92000000 + 0x24368, 4);
  BeforeCopy(full, memory); assert(!stock_ids.test(410)); Put(0x92000000 + 0x24368, 2);
  copy = pack(420); Put(0x84456C84, 2);
  BeforeCopy(full, memory); assert(!stock_ids.test(420)); Put(0x84456C84, 0);
  // The exact prefix rule ignores old IDs beyond the first type 0 in a group.
  PutShort(memory + ps + 3068 + 30 * 124, 0x64, 390);
  memory[ps + 3068 + 30 * 124 + 0x70] = 1;
  BeforeCopy(full, memory); assert(!stock_ids.test(390));
  BeginEpoch(); assert(!EvidenceFor(identity, false) && stock_ids.test(370));
  std::puts("PASS completed live-copy evidence: registered owner/group/slot, immutable 124 B, prefix, "
            "mismatch/pending/epoch vetoes, unchanged copy bytes and report-budget independence");
}
void ColdProducerCases() {
  using namespace bo2zm::mod_hud_fit;
  FLAGS_bo2zm_test_mod_hud_trace = true; FLAGS_bo2zm_mod_hud_fit = true;
  BeginEpoch(); SetupLayout(2); AuthorPlacements(2); ValidVm();
  constexpr uint32_t root = ps_root, stride = ps_stride;
  Put(0x83B44E24, root); Put(0x83B44E28, stride);
  std::memset(memory + root, 0, stride); Put(root + 416, 0);
  Put(cg_array, 0); Put(0x833D0640 + 340, root); memory[0x833D0640 + 251] = 1;
  const uint32_t other = root + numbers[1] * stride;
  std::memset(memory + other, 0, stride); Put(other + 416, numbers[1]);
  Put(0x833D0640 + numbers[1] * 796 + 340, other);
  EndMapLoad();
  const uint32_t news = root + 6912 + 13 * 124;
  for (unsigned slot = 0; slot < 13; ++slot) memory[root + 6912 + slot * 124 + 0x70] = 8;
  auto copy_news = [&] {
    auto creation = CreationContext(0); sub_8232A280(creation, memory); AuthorPayload(kPool, 1);
    PutShort(memory + kPool, 0x64, 197); PutShort(memory + kPool, 0x56, 0);
    PutScalar(memory + kPool, 0, -850); PutScalar(memory + kPool, 4, 210);
    PutScalar(memory + kPool, 0xC, 1.5f); PutScalar(memory + kPool, 0x10, 0); PutShort(memory + kPool, 0x52, 0);
    auto copy = Copy(kPool, 0x8232D044); copy.r3.u64 = news; rexcrt_bo2zm_memcpy(copy, memory);
  };
  auto scan = [&](uint32_t ps) {
    auto copy = Copy(ps, 0x82439010, 10760); copy.r3.u64 = clients + 0x20000; rexcrt_bo2zm_xmemcpy(copy, memory);
  };
  // A registered producer and its completed group 1 / slot 13 element come
  // before a usable client viewport and loopback channel.
  Put(cg_array + 0x4D888, 0); Put(0x92000000 + 0x24368, 0);
  copy_news(); PsIdentity identity; assert(RegisteredPs(memory, root, identity));
  assert(EvidenceFor(identity, false)->slots[44].complete && menu_body_ids[0].test(197));
  scan(root);
  assert(!stock_ids.test(197) && !owner_blocked.test(0) && !owner_ready.test(0));
  const uint32_t snapshot = cg_array + 0x80, ps = snapshot + 16;
  Put(cg_array + 40, snapshot); std::memcpy(memory + ps, memory + root, 10760);
  const uint32_t hud = ps + 6912 + 13 * 124;
  auto invoke = [&](bool fitted) {
    const auto& seen = Draw(0, hud);
    assert(Fitted(seen, 0) == fitted && seen.hud_scale == (fitted ? 1.0f : kSplitHudScale));
  };
  invoke(false);
  assert(EvidenceFor(identity, false) && !stock_ids.test(197));
  Put(0x92000000 + 0x24368, 2); invoke(false);  // connection warm, viewport still cold
  assert(EvidenceFor(identity, false) && menu_body_ids[0].test(197));
  PublishViews(2); scan(root);
  assert(qualified_body_ids[0].test(197) && owner_ready.test(0) && !owner_blocked.test(0));
  invoke(true);

  // Temporary demo/scope loss cannot erase producer identity or invent stock.
  Put(0x84456C84, 2); invoke(false); scan(root);
  assert(EvidenceFor(identity, false) && !stock_ids.test(197));
  Put(0x84456C84, 0); invoke(true);
  // A real eligible identity change still retires old evidence.
  const auto new_dvar = clients + 0x29000;
  Put(new_dvar + 0x10, 1); memory[new_dvar + 0x18] = 1; Put(0x837F8DC0, new_dvar);
  invoke(false); assert(!EvidenceFor(identity, false) && menu_body_ids[0].none());
  assert(!stock_ids.test(197)); copy_news(); scan(root); invoke(true);

  // Stock/unknown records remain negative even while another owner is ready.
  auto stock = CreationContext(1023); sub_8232A280(stock, memory); AuthorPayload(kPool, 1);
  PutShort(memory + kPool, 0x64, 198); PutShort(memory + kPool, 0x56, 0);
  auto copy = Copy(kPool); copy.r3.u64 = other + 3068; rexcrt_bo2zm_memcpy(copy, memory);
  std::memcpy(memory + other + 3068 + 124, memory + other + 3068, 124);
  PutShort(memory + other + 3068 + 124, 0x64, 199); scan(other);
  assert(stock_ids.test(198) && stock_ids.test(199) && !owner_blocked.test(0)); invoke(true);
  bool saw_unknown_reason = false;
  for (const auto& line : logs)
    if (line.find("first_unknown") != std::string::npos && line.find("body=199 ") != std::string::npos &&
        line.find("complete=false") != std::string::npos) saw_unknown_reason = true;
  assert(saw_unknown_reason);

  // A real changed byte still poisons 197 and disables this owner's text AND
  // material path. Preserving a cold exact match does not weaken mismatches.
  memory[news + 123] ^= 1; scan(root);
  assert(stock_ids.test(197) && owner_blocked.test(0)); invoke(false);
  auto cursor = CreationContext(0); sub_8232A280(cursor, memory); AuthorPayload(kPool, 8);
  PutShort(memory + kPool, 0x64, 0); PutShort(memory + kPool, 0x56, 0);
  copy = Copy(kPool); copy.r3.u64 = root + 3068; rexcrt_bo2zm_memcpy(copy, memory);
  assert(std::memcmp(memory + copy.r3.u32, memory + kPool, 124) == 0);
  BeginEpoch(); assert(!EvidenceFor(identity, false) && stock_ids.test(197) && stock_ids.test(199));
  FLAGS_bo2zm_mod_hud_fit = false;
  std::puts("PASS cold producer: completed registered group 1 / slot 13 survives a cold viewport and scope, fits once "
            "ready, retires on identity/epoch change, stock/unknown/mismatch vetoes stop fitting");
}
int main() {
  using namespace bo2zm::mod_hud_fit;
  constexpr size_t size = size_t(1) << 32;
#ifdef _WIN32
  memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
  assert(memory);
#else
  memory = static_cast<uint8_t*>(mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(memory != MAP_FAILED);
#endif
  Commit(0x10000, 0x20000);
  Commit(kSystemHeap, kSystemHeapSize);
  Commit(0x833A5000, 0x27000);
  Commit(0x840DF000, 0x6000);
  Commit(0x82BB0000, 0xD0000); Commit(0x82CBC000, 0x1000); Commit(0x833D0000, 0x4000);
  Commit(0x837F6000, 0x2000); Commit(0x83E40000, 0x1000);
  Commit(0x837F8000, 0x1000); Commit(0x84456000, 0x1000); Commit(0x92000000, 0xE0000);
  Commit(0x83B44000, 0x1000); Commit(ps_root, 0x60000);
  Commit(0x94000000, 0x40000);
  Commit(cg_array, 0x220000); Commit(clients, 0x40000);
  next_hud = kPool;
  assert(FLAGS_bo2zm_mod_hud_fit && !FLAGS_bo2zm_test_mod_hud_trace);
  assert(FLAGS_bo2zm_mod_local_host && FLAGS_bo2zm_split_even_margins && FLAGS_bo2zm_split_margin_percent == 3.0);
  auto ctx = CreationContext();
  sub_8232A280(ctx, memory);
  assert(allocate_calls == 1 && ctx.r3.u32 == kPool && logs.empty());
  assert(creations[0].serial && !creations[0].menu);  // ordinary default-on metadata is quiet
  FLAGS_bo2zm_mod_hud_fit = false;
  BeginEpoch(); ctx = CreationContext(); sub_8232A280(ctx, memory);
  assert(creations[0].serial == 0 && logs.empty());  // explicit rollback, both diagnostics off

  FLAGS_bo2zm_test_mod_hud_trace = true;
  Put(kVm + 8, 1); Put(kVm + 12, kFrames + 20);
  Put(kFrames + 20, 0x10081); Put(kFrames + 28, 71);
  Put(kVm + 4, kMaxStack); Put(kVm + 16, kStack);
  ctx = CreationContext(); sub_8232A280(ctx, memory);
  assert(creations[0].serial && creations[0].owner == 3);
  assert(logs[0].find("synchronous_menu=true") != std::string::npos);
  assert(creations[0].menu);
  // A stock utility on top retains the calling menu continuation below it.
  Put(kVm + 8, 2); Put(kVm + 12, kFrames + 40); Put(kFrames + 40, 0x13001);
  assert(ObserveVm(memory, 0x8232C400).menu);
  // The same entity in a resumed stock-only root is not menu provenance.
  Put(kFrames + 20, 0x13081);
  assert(!ObserveVm(memory, 0x8232C400).menu);
  Put(kFrames + 20, 0x10081); Put(kFrames + 40, 0x12001);
  assert(!ObserveVm(memory, 0x8232C400).menu);  // bot/shared veto
  Put(kFrames + 40, 0xFFFFF000);
  assert(!ObserveVm(memory, 0x8232C400).readable);  // even with an earlier menu
  Put(kFrames + 40, 0x13001);
  assert(!ObserveVm(memory, 0x8232C380).readable);  // global HUD allocator caller
  Put(kVm + 4, kMaxStack - 8);
  assert(!ObserveVm(memory, 0x8232C400).readable);  // exact maxstack field
  Put(kVm + 4, kMaxStack);
  Put(kVm + 16, kStack + 1);
  assert(!ObserveVm(memory, 0x8232C400).readable);
  Put(kVm + 16, kStack);
  Put(kVm + 12, kFrames + 20);  // inconsistent with count, even in array
  assert(!ObserveVm(memory, 0x8232C400).readable);
  Put(kVm + 12, kFrames + 40);
  Put(kVm + 8, 32); assert(!ObserveVm(memory, 0x8232C400).readable);
  Put(kVm + 8, 0); assert(!ObserveVm(memory, 0x8232C400).readable);
  Put(kVm + 8, 2);
  Put(kFrames, 0xFFFFF000);  // sentinel must not be read as an ancestor
  assert(ObserveVm(memory, 0x8232C400).menu);
  const auto first_serial = creations[0].serial;
  std::array<uint8_t, 136> original{};
  std::memcpy(original.data(), memory + kPool, original.size());
  for (uint32_t lr : {0x8232CDB0u, 0x8232CE8Cu, 0x8232CF68u, 0x8232D044u}) {
    std::memset(memory + 0x20000, 0xD7, 160);
    ctx = Copy(kPool, lr);
    const auto registers = ctx;
    rexcrt_bo2zm_memcpy(ctx, memory);
    assert(std::memcmp(&registers, &ctx, sizeof(ctx)) == 0);
    assert(std::memcmp(memory + 0x20000, original.data(), 124) == 0);
    assert(memory[0x20000 + 124] == 0xD7);
    assert(std::memcmp(memory + kPool, original.data(), original.size()) == 0);
  }
  assert(menu_copy_reports == 4);
  ctx = Copy(kPool, 0x8232CDB4); rexcrt_bo2zm_memcpy(ctx, memory);
  ctx = Copy(kPool, 0x8232CDB0, 123); rexcrt_bo2zm_memcpy(ctx, memory);
  ctx = Copy(kPool + 1); rexcrt_bo2zm_memcpy(ctx, memory);
  ctx = Copy(kPool - 136); AfterCopy(ctx, memory);
  ctx = Copy(kPool + 1116 * 136); AfterCopy(ctx, memory);
  assert(menu_copy_reports == 4);
  ctx = CreationContext(4); ctx.lr = 0x8232C380;
  sub_8232A280(ctx, memory);
  assert(!creations[0].menu && creations[0].owner == 4 && creations[0].serial > first_serial);
  // Same slot, another owner: prior identity must not survive reuse.
  ctx = CreationContext(7); sub_8232A280(ctx, memory);
  assert(creations[0].serial > first_serial && creations[0].owner == 7);
  sub_8237C968(ctx, memory);
  assert(free_calls == 1 && creations[0].serial == 0);
  ctx = Copy(kPool); AfterCopy(ctx, memory); assert(copy_reports + menu_copy_reports == 4);
  ctx = CreationContext(); sub_8232A280(ctx, memory);
  const auto old_epoch = epoch;
  BeginEpoch();
  assert(epoch > old_epoch && creations[0].serial == 0);
  ctx = Copy(kPool); AfterCopy(ctx, memory); assert(copy_reports + menu_copy_reports == 0);
  ctx = CreationContext(); sub_8232A280(ctx, memory);
  sub_8232A420(ctx, memory);
  assert(reset_calls == 1 && creations[0].serial == 0);
  // A load/reset during allocation cannot publish a stale allocation tag.
  epoch_on_allocate = true;
  ctx = CreationContext(); sub_8232A280(ctx, memory);
  assert(creations[0].serial == 0);
  epoch_on_allocate = false;
  // The real hook has no held host mutex or lifetime guard across guest calls.
  jump_on_allocate = true;
  if (!setjmp(jump_target)) { ctx = CreationContext(); sub_8232A280(ctx, memory); assert(false); }
  jump_on_allocate = false;
  ctx = CreationContext(); sub_8232A280(ctx, memory);
  assert(creations[0].serial);
  // An invalid guest frame pointer is observed but never dereferenced.
  Put(kVm + 12, 0xFFFFF000);
  const auto vm = ObserveVm(memory, 0x8232C400);
  assert(!vm.readable && !vm.position);
  for (unsigned i = 0; i < 80; ++i) {
    ctx = CreationContext(); sub_8232A280(ctx, memory);
    ctx = Copy(kPool); AfterCopy(ctx, memory);
  }
  assert(creation_reports == kCreationReports && copy_reports == kCopyReports);
  next_hud = 0;
  ctx = CreationContext(); sub_8232A280(ctx, memory); assert(!ctx.r3.u32);
  next_hud = kPool;
  std::puts("PASS HUD allocation/free/reset hooks, menu ancestry, bounded trace, epoch/reuse/longjmp and the four CRT copy sites");
  // A view reset during the view hook drops the views observed before it.
  BeginEpoch(); SetupLayout(2); EndMapLoad(); PublishViews(2);
  assert(map_ready && views[0].local == 0);
  epoch_on_view = true; PPCContext view{}; view.r3.u64 = 0; sub_8223BD90(view, memory); epoch_on_view = false;
  assert(!map_ready && views[0].local == -1);

  FittedDrawCases();
  SystemLinkCases();
  JoinedCases();
  QuadCases();
  SafeAreaCases();
  LocalHostCases();
  IdProbeCases();
#ifdef _WIN32
  assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
  assert(munmap(memory, size) == 0);
#endif
}
