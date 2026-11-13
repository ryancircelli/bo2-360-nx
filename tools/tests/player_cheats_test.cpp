// Exercise the real core, hooks, preferences and queues with authored memory.
#include "../../games/bo2/src/bo2zm_cheats.cpp"

#include <cassert>
#include <csetjmp>
#include <iostream>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace c = bo2zm::cheats;
using c::detail::Option;
static bool zombies = true;
namespace bo2zm::mode { bool IsZombies() { return zombies; } }
namespace bo2zm::preset {
std::string LoadedMap() { return "zm_transit"; }
bool client_in_game = false;
bool ClientInGame() { return client_in_game; }
}
namespace bo2zm::cheat_sync {
void EveryFrame(PPCContext&, uint8_t*) {}
void TraceFrame(bool, bool) {}
static std::vector<std::string> forwarded;
bool Forward(int controller, std::string_view name, std::string_view argument, bool pap) {
  forwarded.push_back(std::to_string(controller) + " " + std::string(name) + " " + std::string(argument) +
                      (pap ? " pap" : ""));
  return true;
}
}
namespace bo2zm::autoexec {
void ExecuteCommands(PPCContext&, uint8_t*, std::string_view, int) {}
}
static bool throw_held = false;
static std::jmp_buf escape;
namespace bo2zm::dvars {
std::optional<Dvar> Find(std::string_view name) {
  return name == "g_ai" ? std::optional<Dvar>({0x42001000, kBool}) : std::nullopt;
}
std::string Text(const uint8_t*, const Dvar&) { return "1"; }
bool Matches(const uint8_t*, const Dvar&, std::string_view) { return false; }
void ExecuteIgnoringCheats(PPCContext&, uint8_t*, std::string_view, std::span<const uint32_t>) {
  assert(g_held_mutex.try_lock());
  g_held_mutex.unlock();
  if (throw_held) std::longjmp(escape, 1);
}
}

struct Memory {
  uint8_t* base;
  Memory() {
#if defined(_WIN32)
    base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(1) << 32, MEM_RESERVE, PAGE_READWRITE));
#else
    base = static_cast<uint8_t*>(mmap(nullptr, size_t(1) << 32, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(base != MAP_FAILED);
#endif
    assert(base);
    Commit(0x40000000, 4 * kCgSize);
    Commit(0x41000000, 0x100000);
    Commit(0x42000000, 0x20000);
    Commit(0x70000000, 0x100000);
    Commit(0x82000000, 0x2600000);
  }
  void Commit(uint32_t address, size_t length) {
#if defined(_WIN32)
    assert(VirtualAlloc(base + address, length, MEM_COMMIT, PAGE_READWRITE));
#else
    (void)address; (void)length;
#endif
  }
  ~Memory() {
#if defined(_WIN32)
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, size_t(1) << 32);
#endif
  }
};

static int damage_calls = 0, move_calls = 0;
static uint32_t observed_pm_type = 0;
struct Call { uint32_t address, a, b, c; std::string text; };
static std::vector<Call> calls;
static bool throw_script = false;
template<uint32_t Address>
void Guest(PPCContext& ctx, uint8_t* base) {
  assert(g_script_mutex.try_lock()); g_script_mutex.unlock();
  assert(g_actors_mutex.try_lock()); g_actors_mutex.unlock();
  assert(g_held_mutex.try_lock()); g_held_mutex.unlock();
  std::string text;
  if constexpr (Address == kScriptPushString) text = reinterpret_cast<const char*>(base + ctx.r4.u32);
  calls.push_back({Address, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, text});
  if constexpr (Address == kClipSize) ctx.r3.u64 = 30 + ctx.r3.u32;
  else if constexpr (Address == kScriptFunction) ctx.r3.u64 = 0x50000000;
  else if constexpr (Address == kScriptRunEntityThread) {
    if (throw_script) std::longjmp(escape, 1);
    ctx.r3.u64 = 123;
  } else ctx.r3.u64 = 0;
}
namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t address) {
#define CASE(name) case name: return &Guest<name>
  switch (address) {
    CASE(kClipSize); CASE(kSetClip); CASE(kScriptFunction); CASE(kScriptRunEntityThread);
    CASE(kScriptPushString); CASE(kScriptPushInt); CASE(kScriptPushFloat); CASE(kScriptPushVector);
    CASE(kScriptFreeThread); CASE(kPerkIndex); CASE(kHasWeapon);
    CASE(kRadiusDamage);
    default: assert(false && "unexpected guest service"); return nullptr;
  }
#undef CASE
}
}
void __imp__sub_8269AE30(PPCContext& ctx, uint8_t* base) {
  ++move_calls;
  const auto ps = Load32(base, ctx.r3.u32);
  observed_pm_type = Load32(base, ps + kPsPmType);
  StoreFloat(base, ps + kPsOrigin, LoadFloat(base, ps + kPsOrigin) + 1);
}
void __imp__sub_82325CA8(PPCContext&, uint8_t*) { ++damage_calls; }
void __imp__sub_826BAF08(PPCContext& ctx, uint8_t* base) {
  StoreFloat(base, ctx.r4.u32, 88);
  StoreFloat(base, ctx.r5.u32, 99);
}
void __imp__sub_82336790(PPCContext&, uint8_t*) {}
void __imp__sub_8223A410(PPCContext&, uint8_t*) {}
void __imp__sub_82687E20(PPCContext&, uint8_t*) {}
void __imp__sub_82300AF0(PPCContext&, uint8_t*) {}
void __imp__sub_823696B8(PPCContext&, uint8_t*) {}
void __imp__sub_823743A8(PPCContext&, uint8_t*) {}

static PPCContext Context() {
  PPCContext ctx;
  ctx.r1.u64 = 0x700F0000;
  ctx.r13.u64 = 0x70080000;
  return ctx;
}

static void Mapping(uint8_t* base) {
  c::BeginMapLoad();
  Store32(base, kCgArray, 0x40000000);
  Store32(base, 0x82C70F44, 4);
  Store32(base, 0x82976AC8, 2); // actual primary is controller three / server client twelve
  Store32(base, 0x82C153E8, 1280);
  Store32(base, 0x82C153EC, 720);
  Store32(base, 0x70080000, 0x70090000);
  constexpr uint32_t controllers[]{2, 0, 3, 1}, numbers[]{7, 3, 12, 1};
  for (int local = 0; local < 4; ++local) {
    const uint32_t manager = 0x837F6A28 + local * 20;
    Store32(base, manager, 1);
    Store32(base, manager + 4, local);
    Store32(base, manager + 8, controllers[local]);
    base[0x83E402C0 + controllers[local] * 172] = 1;
    Store32(base, 0x82C6FDC8 + local * 1112, 1);
    const uint32_t cg = 0x40000000 + local * kCgSize;
    const uint32_t client = 0x41000000 + numbers[local] * 0x6000;
    const uint32_t entity = kGEntities + numbers[local] * kEntitySize;
    Store32(base, cg, numbers[local]);
    Store32(base, cg + 0x480A8 + kPsClientNum, numbers[local]);
    Store32(base, cg + 0x480A8 + kPsPmType, 0);
    Store32(base, client + kPsClientNum, numbers[local]);
    Store32(base, client + kPsPmType, 0);
    Store32(base, client + kClientSessionState, 0);
    Store32(base, entity + kEntClient, client);
    base[entity + 251] = 1;
    Store32(base, client + kClientScore, 0);
    Store32(base, cg + 0x4D880, (local % 2) * 640);
    Store32(base, cg + 0x4D884, (local / 2) * 360);
    Store32(base, cg + 0x4D888, 640);
    Store32(base, cg + 0x4D88C, 360);
  }
  c::EndMapLoad();
  UpdateActors(base);
  for (auto& player : g_players) player.options.ResetAll();
  for (int local = 0; local < 4; ++local) assert(c::SetPlayerOption(local, "cheats", 1));
  calls.clear();
}

static void OptionsAndRouting(uint8_t* base) {
  Mapping(base);
  assert(c::PrimaryLocalPlayer() == 2 && c::IsPrimaryController(3));
  assert(c::LocalPlayerForController(2) == 0 && c::LocalPlayerForController(0) == 1);
  assert(c::LocalPlayerForController(-1) == -1 && c::LocalPlayerForController(4) == -1);
  int value = -1;
  for (int local = 0; local < 4; ++local) {
    assert(c::SetPlayerOption(local, "tpp_color", local + 2));
    assert(c::GetPlayerOption(local, "tpp_color", value) && value == local + 2);
  }
  for (auto name : {"god", "ammo", "explosive", "norecoil", "zm_ignore", "keep_perks", "feather",
                    "hud", "third", "give_pap", "cross_perks", "combo", "collision"}) {
    assert(c::SetPlayerOption(1, name, 1));
    assert(c::GetPlayerOption(1, name, value) && value == 1);
    assert(c::GetPlayerOption(0, name, value) && value == 0);
  }
  assert(c::SetPlayerOption(1, "noclip", 1));
  assert(c::SetPlayerOption(1, "ufo", 1));
  assert(c::GetPlayerOption(1, "noclip", value) && value == 0);
  assert(c::GetPlayerOption(1, "ufo", value) && value == 1);
  assert(c::SetPlayerOption(1, "noclip", 0)); // does not clear UFO
  assert(c::GetPlayerOption(1, "ufo", value) && value == 1);
  assert(c::SetPlayerOption(1, "combo_mode", 99));
  assert(c::GetPlayerOption(1, "combo_mode", value) && value == 1);
  assert(c::SetPlayerOption(1, "fov", 99) && c::SetPlayerOption(1, "jump", -99));
  assert(c::GetPlayerOption(1, "fov", value) && value == 11);
  assert(c::GetPlayerOption(1, "jump", value) && value == 0);
  assert(c::SetPlayerOption(1, "cheats", 0));
  assert(c::GetPlayerOption(1, "god", value) && value == 1);
  assert(StateForLocal(1).options.Effective(Option::God) == 0);
  assert(c::SetPlayerOption(1, "cheats", 1));
  assert(StateForLocal(1).options.Effective(Option::God) == 1);
  for (auto name : {"round", "power", "doors", "spawns", "fps_counter", "fly", "unknown"}) {
    assert(!c::GetPlayerOption(1, name, value));
    assert(!c::SetPlayerOption(1, name, 1));
  }
  assert(!c::SetPlayerOption(-1, "god", 1) && !c::GetPlayerOption(4, "god", value));
  assert(!c::RunPlayerCommand(1, "bo2zm_doors", "open"));
  assert(!c::RunPlayerCommand(1, "bo2zm_hold", "g_ai 0"));
  c::SetGodMode(true); // legacy selects actual primary actor, not controller/server client zero
  assert(StateForLocal(2).options.Effective(Option::God));
  assert(c::GodMode());
}

static uint32_t Pmove(uint8_t* base, int local, bool server) {
  const Actor actor = LocalActor(local);
  const uint32_t pm = 0x70001000 + uint32_t(local) * 0x1000;
  Store32(base, pm, server ? actor.client : actor.cg + 0x480A8);
  base[pm + 604] = uint8_t(server);
  Store32(base, pm + 608, server ? uint32_t(-1) : uint32_t(local));
  Store32(base, pm + kPmTraceMask, 123);
  return pm;
}

static void Hooks(uint8_t* base) {
  Mapping(base);
  assert(c::SetPlayerOption(1, "god", 1));
  PPCContext ctx = Context();
  const auto chosen = LocalActor(1), other = LocalActor(2);
  damage_calls = 0;
  ctx.r3.u64 = chosen.entity; sub_82325CA8(ctx, base); assert(damage_calls == 0);
  ctx.r3.u64 = other.entity; sub_82325CA8(ctx, base); assert(damage_calls == 1);
  ctx.r3.u64 = kGEntities; sub_82325CA8(ctx, base); assert(damage_calls == 2);
  assert(c::SetPlayerOption(1, "noclip", 1));
  for (bool server : {false, true}) {
    ctx.r3.u64 = Pmove(base, 1, server);
    sub_8269AE30(ctx, base);
    assert(observed_pm_type == kPmSpectator);
    assert(Load32(base, (server ? chosen.client : chosen.cg + 0x480A8) + kPsPmType) == 0);
    ctx.r3.u64 = Pmove(base, 2, server);
    sub_8269AE30(ctx, base);
    assert(observed_pm_type == kPmNormal);
  }
  const uint32_t wrong_domain = Pmove(base, 1, true);
  base[wrong_domain + 604] = 0; Store32(base, wrong_domain + 608, 1);
  ctx.r3.u64 = wrong_domain; sub_8269AE30(ctx, base);
  assert(observed_pm_type == kPmNormal);
  const uint32_t wrong_local = Pmove(base, 1, false);
  Store32(base, wrong_local + 608, 2);
  ctx.r3.u64 = wrong_local; sub_8269AE30(ctx, base);
  assert(observed_pm_type == kPmNormal);
  // A spoofed clientNum on unrelated state is insufficient for ownership.
  const uint32_t pm = Pmove(base, 1, true), foreign = 0x42000000;
  Store32(base, foreign + kPsClientNum, chosen.number);
  Store32(base, foreign + kPsPmType, 0);
  Store32(base, pm, foreign);
  ctx.r3.u64 = pm; sub_8269AE30(ctx, base); assert(observed_pm_type == 0);
  assert(c::SetPlayerOption(1, "norecoil", 1));
  ctx.r4.u64 = 0x70040000; ctx.r5.u64 = 0x70040010;
  StoreFloat(base, ctx.r4.u32, 5); StoreFloat(base, ctx.r5.u32, 6);
  ctx.r3.u64 = chosen.client; sub_826BAF08(ctx, base);
  assert(LoadFloat(base, ctx.r4.u32) == 5 && LoadFloat(base, ctx.r5.u32) == 6);
  ctx.r3.u64 = foreign; sub_826BAF08(ctx, base);
  assert(LoadFloat(base, ctx.r4.u32) == 88 && LoadFloat(base, ctx.r5.u32) == 99);
  // Holding the primary at a test spot does not suppress the secondary fly path.
  const float feet[]{10, 20, 30}; c::HoldAt(feet, 0, 90);
  ctx.r3.u64 = Pmove(base, 1, true); sub_8269AE30(ctx, base);
  assert(observed_pm_type == kPmSpectator);
  ctx.r3.u64 = Pmove(base, 2, true); sub_8269AE30(ctx, base);
  assert(LoadFloat(base, other.client + kPsOrigin) == 10);
  c::ReleaseHold();
  assert(c::SetPlayerOption(1, "explosive", 1));
  ctx.r5.u64 = 0x70050000; ctx.r6.u64 = 5; ctx.r7.u64 = chosen.entity;
  StoreVec(base, ctx.r5.u32 + kTraceHit, {100, 200, 300});
  calls.clear(); sub_82300AF0(ctx, base);
  assert(std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
    return call.address == kRadiusDamage && call.b == chosen.entity && call.c == chosen.entity;
  }));
  assert(!g_blasts.empty() && SameActor(g_blasts.back().actor, chosen));
  for (int local : {0, 1}) {
    const Actor actor = LocalActor(local);
    assert(c::SetPlayerOption(local, "ammo", 1));
    Store32(base, actor.client + kClientWeapon, 5 + local);
    calls.clear();
    KeepAmmo(ctx, base, actor, StateForActor(actor));
    assert(std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
      return call.address == kSetClip && call.a == actor.client && call.b == uint32_t(5 + local) &&
             call.c == uint32_t(35 + local);
    }));
  }
}

static void QueueAndEpoch(uint8_t* base) {
  Mapping(base);
  auto ctx = Context();
  for (int local : {0, 1, 3}) assert(c::RunPlayerCommand(local, "bo2zm_max_points", ""));
  RunPlayerRequests(ctx, base);
  for (int local = 0; local < 4; ++local) {
    assert(Load32(base, LocalActor(local).client + kClientScore) == (local == 2 ? 0u : kMaxPoints));
  }
  const Actor target = LocalActor(1);
  g_map_guns = {"ray_gun_zm"}; g_map_weapons.resize(256); g_map_weapons[5] = "ray_gun_zm";
  Store32(base, target.client + kClientWeapon, 0);
  assert(c::RunPlayerCommand(1, "bo2zm_give_weapon", "ray_gun_zm"));
  assert(!g_player_requests.back().pap);
  assert(c::SetPlayerOption(1, "give_pap", 1));
  assert(!g_player_requests.back().pap); // captured preference
  calls.clear(); RunPlayerRequests(ctx, base);
  assert(std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
    return call.address == kScriptRunEntityThread && call.a == target.entity;
  }));
  assert(c::RunPlayerCommand(1, "bo2zm_perk", "specialty_armorvest"));
  calls.clear(); RunPlayerRequests(ctx, base);
  assert(std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
    return call.address == kScriptRunEntityThread && call.a == target.entity;
  }));
  assert(c::RunPlayerCommand(1, "bo2zm_max_points", ""));
  const auto stale = g_player_requests.front();
  const uint64_t old_epoch = target.epoch;
  c::BeginMapLoad();
  assert(g_player_requests.empty() && !CurrentActor(base, target));
  assert(c::RunPlayerCommand(1, "bo2zm_perk", "specialty_armorvest"));
  assert(g_player_requests.empty());
  c::EndMapLoad();
  assert(!CurrentActor(base, target)); // completion alone does not republish old identity
  UpdateActors(base);
  const Actor fresh = LocalActor(1);
  assert(fresh.entity == target.entity && fresh.client == target.client && fresh.cg == target.cg);
  assert(fresh.epoch != old_epoch && !CurrentActor(base, target));
  Store32(base, fresh.client + kClientScore, 0);
  g_player_requests.push_back(stale); // simulate an already-drained old batch
  g_player_waiting = true;
  RunPlayerRequests(ctx, base);
  assert(Load32(base, fresh.client + kClientScore) == 0);
  // A server-side ownership change is rejected even before main refresh.
  const auto still_cached = LocalActor(0);
  Store32(base, 0x837F6A28 + 8, 1);
  assert(!CurrentActor(base, still_cached));
  Store32(base, 0x837F6A28 + 8, 2);
  // Queued actor cannot follow a remapped local slot to a different controller.
  assert(c::RunPlayerCommand(1, "bo2zm_max_points", ""));
  const uint32_t cg1 = LocalActor(1).cg, cg2 = LocalActor(2).cg;
  std::vector<uint8_t> saved(base + cg1, base + cg1 + kCgSize);
  std::memcpy(base + cg1, base + cg2, kCgSize); std::memcpy(base + cg2, saved.data(), kCgSize);
  Store32(base, 0x837F6A28 + 20 + 8, 3); Store32(base, 0x837F6A28 + 40 + 8, 0);
  Store32(base, 0x82976AC8, 1);
  assert(c::SetPlayerOption(2, "god", 1));
  UpdateActors(base);
  assert(c::PrimaryLocalPlayer() == 1 && c::IsPrimaryController(3));
  int value = 0;
  assert(c::GetPlayerOption(1, "god", value) && value == 1); // follows controller three
  assert(c::GetPlayerOption(2, "god", value) && value == 0);
  RunPlayerRequests(ctx, base);
  assert(Load32(base, fresh.client + kClientScore) == 0);
}

static void PerkHistory(uint8_t* base) {
  Mapping(base);
  auto ctx = Context();
  const auto actor = LocalActor(0), other = LocalActor(1);
  c::SetPlayerOption(0, "keep_perks", 1); c::SetPlayerOption(1, "keep_perks", 1);
  PrepareServerState(actor, StateForActor(actor)); PrepareServerState(other, StateForActor(other));
  Store32(base, actor.client + kPsPerks, 1); Store32(base, other.client + kPsPerks, 0);
  KeepPerks(ctx, base, actor, StateForActor(actor)); KeepPerks(ctx, base, other, StateForActor(other));
  assert(StateForActor(actor).noted_perks[0] == 1 && StateForActor(other).noted_perks[0] == 0);
  Store32(base, actor.client + kPsPmType, 2);
  KeepPerks(ctx, base, actor, StateForActor(actor));
  Store32(base, actor.client + kPsPmType, 0); Store32(base, actor.client + kPsPerks, 0);
  calls.clear(); KeepPerks(ctx, base, actor, StateForActor(actor));
  int threads = 0;
  for (const auto& call : calls) if (call.address == kScriptRunEntityThread) {
    ++threads; assert(call.a == actor.entity);
  }
  assert(threads > 0 && StateForActor(other).noted_perks[0] == 0);
  Store32(base, actor.client + kPsPerks, 1);
  RemovePerks(ctx, base, actor);
  assert(StateForActor(actor).noted_perks[0] == 0);
  KeepPerks(ctx, base, actor, StateForActor(actor));
  assert(StateForActor(actor).noted_perks[0] == 0); // delayed notify's quiet period remains intact
}

static void SnapshotAndBounds(uint8_t* base) {
  Mapping(base);
  for (int local = 0; local < 4; ++local) {
    c::SetPlayerOption(local, "third", 1); c::SetPlayerOption(local, "third_crosshair", 2);
    c::SetPlayerOption(local, "tpp_color", local);
  }
  PublishCrosshairOverlays(base);
  c::CrosshairOverlay output[4];
  assert(c::GetCrosshairOverlays(output, 4) == 4);
  for (int i = 0; i < 4; ++i) {
    assert(output[i].enabled && output[i].color == i);
    assert(output[i].normalizedRect[0] == float(i % 2) / 2);
    assert(output[i].normalizedRect[1] == float(i / 2) / 2);
    assert(output[i].normalizedRect[2] == .5f && output[i].normalizedRect[3] == .5f);
  }
  assert(c::GetCrosshairOverlays(output, 2) == 2 && c::GetCrosshairOverlays(nullptr, 4) == 0);
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) readers.emplace_back([] {
    c::CrosshairOverlay views[4];
    for (int read = 0; read < 150; ++read) {
      const int count = c::GetCrosshairOverlays(views, 4);
      assert(count == 4 || count == 0);
      for (int j = 0; j < count; ++j) assert(views[j].color == j && views[j].normalizedRect[2] == .5f);
    }
  });
  for (int i = 0; i < 100; ++i) PublishCrosshairOverlays(base);
  c::BeginMapLoad();
  for (auto& reader : readers) reader.join();
  assert(c::GetCrosshairOverlays(output, 4) == 0);
  c::EndMapLoad(); UpdateActors(base);
  Store32(base, 0x82C70F44, 2); UpdateActors(base); PublishCrosshairOverlays(base);
  assert(c::GetCrosshairOverlays(output, 4) == 2 && !LocalActor(2));
  Store32(base, 0x82C70F44, 4); Store32(base, kCgArray, 0xFFFFF000); UpdateActors(base);
  assert(!LocalActor(0) && !LocalActor(1)); // overflow must be rejected before any CG read
  Store32(base, kCgArray, 0x40000000);
  Store32(base, kGEntities + 7 * kEntitySize + kEntClient, 0xFFFFF000); UpdateActors(base);
  assert(!LocalActor(0));
  Mapping(base);
  Store32(base, 0x82C153E8, 1920); Store32(base, 0x82C153EC, 1080);
  for (int i = 0; i < 4; ++i) {
    const auto cg = LocalActor(i).cg;
    Store32(base, cg + 0x4D880, (i % 2) * 960); Store32(base, cg + 0x4D884, (i / 2) * 540);
    Store32(base, cg + 0x4D888, 960); Store32(base, cg + 0x4D88C, 540);
  }
  PublishCrosshairOverlays(base);
  assert(c::GetCrosshairOverlays(output, 4) == 4 && output[3].normalizedRect[0] == .5f);
  zombies = false; PublishCrosshairOverlays(base); assert(c::GetCrosshairOverlays(output, 4) == 0);
  zombies = true;
}

static void LongjmpBoundaries(uint8_t* base) {
  Mapping(base);
  auto ctx = Context();
  assert(c::RunPlayerCommand(1, "bo2zm_perk", "specialty_armorvest"));
  throw_script = true;
  if (setjmp(escape) == 0) { RunPlayerRequests(ctx, base); assert(false); }
  throw_script = false;
  assert(g_script_mutex.try_lock()); g_script_mutex.unlock();
  assert(g_actors_mutex.try_lock()); g_actors_mutex.unlock();
  assert(c::RunPlayerCommand(3, "bo2zm_max_points", ""));
  ctx = Context(); RunPlayerRequests(ctx, base);
  assert(Load32(base, LocalActor(3).client + kClientScore) == kMaxPoints);
  assert(Load32(base, LocalActor(1).client + kClientScore) == 0);
  assert(c::RunPlayerCommand(c::PrimaryLocalPlayer(), "bo2zm_hold", "g_ai 0"));
  throw_held = true;
  if (setjmp(escape) == 0) { KeepHeldDvars(ctx, base); assert(false); }
  throw_held = false;
  assert(g_held_mutex.try_lock()); g_held_mutex.unlock();
  assert(c::SetPlayerOption(c::PrimaryLocalPlayer(), "cheats", 0));
}

struct TaggedPad : bo2zm::pad::Pad { uint32_t user = 0; };

static void ReviewedRegressions(uint8_t* base) {
  Mapping(base);
  const auto old = LocalActor(1); // controller zero, server client three
  auto& state = StateForActor(old);
  TaggedPad press{{bo2zm::pad::kDown, 0, true}, 0};
  c::SetPlayerOption(1, "combo", 1);
  assert(ScopedPlayerPad(press, 1) == bo2zm::pad::kDown);
  c::SetPlayerOption(1, "cheats", 0);
  TakeComboPresses(state, old.generation);
  assert(state.options.Effective(Option::Fly) == 0); // pending input cannot bypass master OFF
  c::SetPlayerOption(1, "cheats", 1);
  TakeComboPresses(state, old.generation);
  assert(state.options.Effective(Option::Fly) == 0); // OFF/ON did not resurrect the old press
  assert(ScopedPlayerPad(press, 1) == bo2zm::pad::kDown);
  state.options.ResetGameplay();
  TakeComboPresses(state, old.generation);
  assert(state.options.Effective(Option::Fly) == 0); // reset clears pending input too

  // Arrange the exact local-counter collision: old destination N-1 would
  // become N after a swap, matching this controller's old cache generation.
  state.main_epoch = state.server_epoch = state.camera_epoch = old.epoch;
  state.main_generation = state.server_generation = state.camera_generation = old.generation;
  state.sized_weapon = 777; state.noted_perks[0] = 0x80; state.shoulder_blend = .75f;
  { std::lock_guard lock(g_actors_mutex); g_actors[2].generation = old.generation - 1; }
  const auto cg1 = LocalActor(1).cg, cg2 = LocalActor(2).cg;
  std::vector<uint8_t> saved(base + cg1, base + cg1 + kCgSize);
  std::memcpy(base + cg1, base + cg2, kCgSize); std::memcpy(base + cg2, saved.data(), kCgSize);
  Store32(base, 0x837F6A28 + 20 + 8, 3); Store32(base, 0x837F6A28 + 40 + 8, 0);
  Store32(base, 0x82976AC8, 1);
  UpdateActors(base);
  const auto moved = LocalActor(2);
  assert(moved.controller == 0 && moved.generation != old.generation);
  PrepareMainState(moved, state); PrepareServerState(moved, state);
  assert(state.sized_weapon == 0 && state.noted_perks[0] == 0);
  auto ctx = Context(); ctx.r3.u64 = 2; sub_8223A410(ctx, base);
  assert(state.shoulder_blend == 0);

  // The bridge resolved controller zero to old local one before the swap.
  // Core must validate the tag and never mutate controller three's bank.
  g_players[3].aiming = false; g_players[0].aiming = false;
  assert(ScopedPlayerPad(press, 1) == 0);
  assert(!g_players[3].aiming && !g_players[0].aiming);
  assert(ScopedPlayerPad(press, 2) == bo2zm::pad::kDown);
  TakeComboPresses(state, moved.generation);
  assert(state.options.Effective(Option::Fly) == c::kNoclip);
  assert(g_players[3].options.Effective(Option::Fly) == c::kFlyOff);

  // A failed load leaves effect actors blocked, but frontend controller and
  // primary ownership must still track further joins/leaves/reorders.
  Mapping(base);
  c::BeginMapLoad(); // deliberately no EndMapLoad before publication
  Store32(base, 0x837F6A28 + 8, 0); Store32(base, 0x837F6A28 + 20 + 8, 2);
  Store32(base, 0x82976AC8, 0);
  UpdateActors(base);
  assert(c::LocalPlayerForController(0) == 0 && c::LocalPlayerForController(2) == 1);
  assert(c::PrimaryLocalPlayer() == 0 && c::IsPrimaryController(0));
  for (int local = 0; local < 4; ++local) assert(!LocalActor(local));
  c::SetGodMode(true);
  assert(g_players[0].options.Effective(Option::God) && !g_players[3].options.Effective(Option::God));
  Store32(base, 0x837F6A28 + 20, 0); UpdateActors(base);
  assert(c::LocalPlayerForController(2) == -1 && c::PrimaryLocalPlayer() == 0);
  assert(!LocalActor(0));
}

static void ProjectionAndJumpAPI(uint8_t* base) {
  Mapping(base);
  StoreFloat(base, kJumpHeight, 39);
  for (int local = 0; local < 4; ++local) {
    const auto actor = LocalActor(local);
    c::SetPlayerOption(local, "jump", local + 1);
    assert(c::JumpLevelForState(base, actor.client) == local + 1);
    assert(c::JumpLevelForState(base, actor.cg + 0x480A8) == local + 1);
  }
  Store32(base, 0x42000000 + kPsClientNum, LocalActor(1).number);
  assert(c::JumpLevelForState(base, 0x42000000) == 0 && c::JumpLevelForState(nullptr, 0) == 0);
  c::SetPlayerOption(0, "fov", 0); c::SetPlayerOption(1, "fov", 11);
  assert(c::FieldOfViewScaleForLocal(0) == 55.0f / 65 && c::FieldOfViewScaleForLocal(1) == 110.0f / 65);
  c::SetPlayerOption(1, "cheats", 0);
  assert(c::FieldOfViewScaleForLocal(1) == 1 && c::JumpLevelForState(base, LocalActor(1).client) == 0);
  assert(c::FieldOfViewScaleForLocal(-1) == 1 && c::FieldOfViewScaleForLocal(4) == 1);
  assert(LoadFloat(base, kJumpHeight) == 39); // selections never write a raised global height
  c::BeginMapLoad();
  assert(c::FieldOfViewScaleForLocal(0) == 1);
}

static void TestAllGod(uint8_t* base) {
  assert(!REXCVAR_GET(bo2zm_test_all_god));
  Mapping(base);
  for (auto& player : g_players) player.options.ResetAll();
  const auto snapshot = [] {
    std::array<int, 4 * (2 * size_t(Option::Count) + 1)> result{};
    size_t index = 0;
    for (const auto& player : g_players) {
      for (const auto& spec : c::detail::kOptions) {
        int wanted = -1;
        assert(player.options.Get(spec.name == "fly" ? "noclip" : spec.name, wanted));
        result[index++] = wanted;
        result[index++] = player.options.Effective(spec.option);
      }
      assert(player.options.Get("ufo", result[index++]));
    }
    assert(index == result.size());
    return result;
  };
  const auto hit = [&](uint32_t target, bool blocked) {
    const auto options = snapshot();
    const bool legacy = g_legacy_boot_picks.load();
    const size_t guest_calls = calls.size();
    PPCContext ctx = Context();
    ctx.r3.u64 = target;
    const auto before = ctx;
    const int before_damage = damage_calls;
    sub_82325CA8(ctx, base);
    assert(damage_calls == before_damage + (blocked ? 0 : 1));
    assert(std::memcmp(&ctx, &before, sizeof(ctx)) == 0);
    assert(snapshot() == options && g_legacy_boot_picks.load() == legacy);
    assert(calls.size() == guest_calls); // no guest settings/flag writes
  };
  std::array<Actor, 4> actors;
  for (int local = 0; local < 4; ++local) {
    actors[local] = LocalActor(local);
    assert(CurrentActor(base, actors[local]));
    assert(!StateForLocal(local).options.Effective(Option::Cheats));
    assert(!StateForLocal(local).options.Effective(Option::God));
    hit(actors[local].entity, false); // default false leaves masters-off actors alone
  }
  FLAGS_bo2zm_test_all_god = true;
  for (const auto& actor : actors) {
    const auto flags = Load32(base, actor.client + kClientFlags);
    const auto blocked = StateForActor(actor).god_blocked.load();
    hit(actor.entity, true);
    assert(Load32(base, actor.client + kClientFlags) == flags);
    assert(StateForActor(actor).god_blocked.load() == blocked + 1);
  }
  // The test flag must still use all live ownership predicates, not merely a
  // cached local entity or a matching server client number.
  for (const auto& actor : actors) {
    const uint32_t manager = 0x837F6A28 + actor.local * 20;
    const std::array<std::pair<uint32_t, uint32_t>, 8> invalid_words{{
        {manager, 0}, {manager + 4, 99}, {manager + 8, uint32_t((actor.controller + 1) % 4)},
        {0x82C6FDC8 + uint32_t(actor.local) * 1112, 0}, {0x82C70F44, uint32_t(actor.local)},
        {actor.cg, actor.number + 1}, {actor.entity + kEntClient, actor.client + 4},
        {actor.client + kPsClientNum, actor.number + 1}}};
    for (const auto& [address, value] : invalid_words) {
      const auto saved = Load32(base, address);
      Store32(base, address, value); hit(actor.entity, false); Store32(base, address, saved);
    }
    for (const auto address : {actor.entity + 251, 0x83E402C0 + uint32_t(actor.controller) * 172}) {
      const auto saved = base[address];
      base[address] = 0; hit(actor.entity, false); base[address] = saved;
    }
    auto stale = actor;
    --stale.epoch;
    { std::lock_guard lock(g_actors_mutex); g_actors[actor.local] = stale; }
    hit(actor.entity, false);
    { std::lock_guard lock(g_actors_mutex); g_actors[actor.local] = actor; }
    assert(CurrentActor(base, actor));
  }
  const uint32_t nonlocal = kGEntities + 19 * kEntitySize;
  base[nonlocal + 251] = 1;
  Store32(base, nonlocal + kEntClient, 0x410C0000);
  Store32(base, 0x410C0000 + kPsClientNum, 19);
  for (const auto target : {uint32_t(0), kGEntities, nonlocal, actors[1].entity + 1})
    hit(target, false);
  zombies = false;
  for (const auto& actor : actors) hit(actor.entity, false);
  zombies = true;
  c::BeginMapLoad();
  for (const auto& actor : actors) hit(actor.entity, false);
  Mapping(base);
  FLAGS_bo2zm_test_all_god = false;
  // Existing primary-only test_god/legacy selection remains primary-only.
  c::SetGodMode(true);
  const int primary = c::PrimaryLocalPlayer();
  for (int local = 0; local < 4; ++local) hit(LocalActor(local).entity, local == primary);
  for (auto& player : g_players) player.options.ResetAll();
  for (int local = 0; local < 4; ++local) hit(LocalActor(local).entity, false);
  std::cout << "PASS test_all_god: four validated actors with masters off, live/mode/epoch rejection, "
               "unchanged options and legacy primary-only gate\n";
}

// A System Link joiner's player: matched by the server's XUID table (getxuid's path), applied like a local actor,
// never matched to a local player, invalidated by a changed XUID and forgotten on a map load.
static void RemotePlayers(uint8_t* base) {
  Mapping(base);
  for (auto& player : g_remote_players) player.options.ResetAll();
  g_remote_slots = {};
  UpdateRemoteActors(base);  // takes the map load's forget, as the main frame does before any message
  constexpr uint32_t number = 5;
  constexpr uint64_t xuid = 0xB13E123456789A00ull;
  const uint32_t client = 0x41000000 + number * 0x6000, entity = kGEntities + number * kEntitySize;
  Store32(base, client + kPsClientNum, number);
  Store32(base, client + kPsPmType, 0);
  Store32(base, client + kClientSessionState, 0);
  Store32(base, client + kClientXuidSlot, number);
  Store32(base, entity + kEntClient, client);
  base[entity + 251] = 1;
  const auto put_xuid = [&](uint32_t slot, uint64_t value) {
    Store32(base, kServerXuids + slot * 8, uint32_t(value >> 32));
    Store32(base, kServerXuids + slot * 8 + 4, uint32_t(value));
  };
  put_xuid(number, xuid);
  // Local player 1 (server client 3) carries the same XUID: it must never become a remote actor.
  Store32(base, 0x41000000 + 3 * 0x6000 + kClientXuidSlot, 3);
  put_xuid(3, 0xB13E000000000077ull);
  int values[c::detail::kSyncedCount]{};
  values[0] = 1;  // god
  values[5] = 1;  // ammo
  values[c::detail::kSyncedCount - 1] = 1;  // the player's CHEATS switch
  int mapped = -2;
  bool changed = false;
  assert(c::SetRemotePlayer(base, xuid, 10, values, &mapped, &changed) && mapped == int(number) && changed);
  assert(c::SetRemotePlayer(base, xuid, 10, values, &mapped, &changed) && !changed);  // a resend
  assert(!c::SetRemotePlayer(base, xuid, 9, values, &mapped, &changed));             // older
  assert(c::SetRemotePlayer(base, 0xB13E000000000077ull, 1, values, &mapped, &changed) && mapped == -1);
  UpdateRemoteActors(base);
  const Actor remote = RemoteActor(0);
  assert(remote && remote.number == number && remote.entity == entity && CurrentActor(base, remote));
  assert(!RemoteActor(1));  // the local player's XUID stays unmapped
  assert(SameActor(ActorForEntity(base, entity), remote) && SameActor(ActorForPs(base, client), remote));
  PPCContext ctx = Context();
  damage_calls = 0;
  ctx.r3.u64 = entity; sub_82325CA8(ctx, base); assert(damage_calls == 0);
  ctx.r3.u64 = LocalActor(0).entity; sub_82325CA8(ctx, base); assert(damage_calls == 1);
  // Server movement only: the remote player has no prediction on this console.
  values[1] = 1;  // noclip
  assert(c::SetRemotePlayer(base, xuid, 11, values, &mapped, &changed) && changed);
  const uint32_t pm = 0x70009000;
  Store32(base, pm, client); base[pm + 604] = 1; Store32(base, pm + 608, uint32_t(-1));
  ctx.r3.u64 = pm; sub_8269AE30(ctx, base); assert(observed_pm_type == kPmSpectator);
  base[pm + 604] = 0; Store32(base, pm + 608, 0);
  ctx.r3.u64 = pm; sub_8269AE30(ctx, base); assert(observed_pm_type == kPmNormal);
  Store32(base, client + kClientWeapon, 7);
  calls.clear();
  KeepAmmo(ctx, base, remote, StateForActor(remote));
  assert(std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
    return call.address == kSetClip && call.a == client && call.b == 7u;
  }));
  // Another player takes the slot: the old actor is no longer current.
  put_xuid(number, 0xB13E000000000099ull);
  assert(!CurrentActor(base, remote));
  ctx.r3.u64 = entity; sub_82325CA8(ctx, base); assert(damage_calls == 2);
  put_xuid(number, xuid);
  UpdateRemoteActors(base);
  assert(RemoteActor(0));
  c::BeginMapLoad();
  c::EndMapLoad();
  UpdateRemoteActors(base);
  assert(!RemoteActor(0) && g_remote_slots[0].xuid == 0 && !g_remote_players[0].options.Effective(Option::God));
  std::cout << "PASS remote players: XUID mapping, hooks, stale-XUID rejection, local exclusion and map-load reset\n";
}

// The other sides of System Link: a joined console's predicted actors (client-side cheats here, everything else sent
// to the host), the host running a remote player's commands, and the per-player move speed.
static void JoinedAndHostCommands(uint8_t* base) {
  // Host side first (the remote player from RemotePlayers is still in the server's XUID table).
  Mapping(base);
  UpdateRemoteActors(base);
  constexpr uint64_t xuid = 0xB13E123456789A00ull;
  int values[c::detail::kSyncedCount]{};
  values[c::detail::kSyncedCount - 1] = 1;
  int mapped = -2;
  bool changed = false;
  assert(c::SetRemotePlayer(base, xuid, 100, values, &mapped, &changed) && mapped == 5);
  UpdateRemoteActors(base);
  const Actor remote = RemoteActor(0);
  assert(remote);
  std::string why;
  {
    std::lock_guard lock(g_script_mutex);
    g_player_requests.clear();
  }
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_perk", "specialty_armorvest", false, why) == 0);
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_perk", "Bad;perk", false, why) == 1);
  assert(c::RunRemoteCommand(base, 0x1234, "bo2zm_max_points", "", false, why) == 1);  // not in the match
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_doors", "open", false, why) == 0);
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_doors", "explode", false, why) == 1);
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_goto_round", "300", false, why) == 1);
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_hold", "timescale 0.5", false, why) == 0);
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_hold", "sv_cheats 1", false, why) == 1);  // not a world setting
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_hold", "timescale 9", false, why) == 1);  // not a menu value
  {
    std::lock_guard lock(g_script_mutex);
    assert(!g_player_requests.empty() && SameActor(g_player_requests.back().actor, remote));
    assert(SameActor(g_world_target, remote) && g_doors_request == 1);
    g_player_requests.clear();
    g_doors_request = 0;
  }
  assert(c::WorldStateText().find("timescale=0.5;") != std::string::npos);
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_release", "timescale 1", false, why) == 0);
  {
    std::lock_guard lock(g_held_mutex);
    g_held.clear();
    g_shown.clear();
  }
  values[c::detail::kSyncedCount - 1] = 0;  // CHEATS off for that player: refused
  assert(c::SetRemotePlayer(base, xuid, 101, values, &mapped, &changed));
  assert(c::RunRemoteCommand(base, xuid, "bo2zm_max_points", "", false, why) == 1);

  // Move speed: the server's movement steps only, scaled once per value the game writes.
  assert(c::SetPlayerOption(1, "speed", 2));
  const Actor fast = LocalActor(1);
  PPCContext ctx = Context();
  Store32(base, fast.client + kPsSpeed, 190);
  ctx.r3.u64 = Pmove(base, 1, true); sub_8269AE30(ctx, base);
  assert(Load32(base, fast.client + kPsSpeed) == 320);
  ctx.r3.u64 = Pmove(base, 1, true); sub_8269AE30(ctx, base);
  assert(Load32(base, fast.client + kPsSpeed) == 320);  // not scaled again
  Store32(base, fast.client + kPsSpeed, 200);           // the game's next think (a perk)
  ctx.r3.u64 = Pmove(base, 1, true); sub_8269AE30(ctx, base);
  assert(Load32(base, fast.client + kPsSpeed) == 337);
  Store32(base, LocalActor(2).client + kPsSpeed, 190);
  ctx.r3.u64 = Pmove(base, 2, true); sub_8269AE30(ctx, base);
  assert(Load32(base, LocalActor(2).client + kPsSpeed) == 190);  // another player keeps the game's speed
  assert(c::SetPlayerOption(1, "speed", 0));

  // A joined console: no server here, the client is in a match.
  for (uint32_t number : {7u, 3u, 12u, 1u}) {
    const uint32_t entity = kGEntities + number * kEntitySize;
    Store32(base, entity + kEntClient, 0);
    base[entity + 251] = 0;
  }
  bo2zm::preset::client_in_game = true;
  UpdateActors(base);
  assert(c::JoinedRemoteGame());
  const Actor predicted = LocalActor(1);
  assert(predicted && predicted.predicted && !predicted.client && !predicted.entity && CurrentActor(base, predicted));
  assert(c::PrimaryLocalPlayer() == 2 && LocalActor(2).predicted);
  // The prediction's movement step flies; nothing resolves on the server's side.
  assert(c::SetPlayerOption(1, "noclip", 1));
  ctx.r3.u64 = Pmove(base, 1, false); sub_8269AE30(ctx, base);
  assert(observed_pm_type == kPmSpectator);
  assert(c::SetPlayerOption(1, "noclip", 0));
  // The view kick on the predicted state, the field of view.
  assert(c::SetPlayerOption(1, "norecoil", 1));
  ctx.r4.u64 = 0x70040000; ctx.r5.u64 = 0x70040010;
  StoreFloat(base, ctx.r4.u32, 5); StoreFloat(base, ctx.r5.u32, 6);
  ctx.r3.u64 = predicted.cg + 0x480A8; sub_826BAF08(ctx, base);
  assert(LoadFloat(base, ctx.r4.u32) == 5 && LoadFloat(base, ctx.r5.u32) == 6);
  assert(c::SetPlayerOption(1, "fov", 7));
  assert(c::FieldOfViewScaleForLocal(1) > 1.3f);
  // The frame's server-side work skips them (no entity to read); zombies ignore stays the host's to apply.
  assert(c::SetPlayerOption(1, "zm_ignore", 1));
  assert(c::SetPlayerOption(1, "ammo", 1));
  c::EveryFrame(ctx, base);
  // Personal and world commands go to the host.
  bo2zm::cheat_sync::forwarded.clear();
  assert(c::SetPlayerOption(1, "give_pap", 1));
  assert(c::RunPlayerCommand(1, "bo2zm_give_weapon", "ray_gun_zm"));
  assert(c::RunPlayerCommand(1, "bo2zm_max_points", ""));
  assert(c::RunCommand("bo2zm_doors", "open"));
  assert(c::RunCommand("bo2zm_hold", "timescale 0.5"));
  assert(c::RunCommand("bo2zm_hold", "cg_drawGun 0"));  // the view's own: held here
  c::GoToRound(12);
  c::SetStopSpawns(true);
  {
    std::lock_guard lock(g_script_mutex);
    assert(g_player_requests.empty() && g_doors_request == 0);
  }
  assert(!c::StopSpawns());
  const std::vector<std::string> expected{"0 bo2zm_give_weapon ray_gun_zm pap", "0 bo2zm_max_points ",
                                          "3 bo2zm_doors open", "3 bo2zm_hold timescale 0.5",
                                          "3 bo2zm_goto_round 12", "3 bo2zm_zm_nospawn 1"};
  if (bo2zm::cheat_sync::forwarded != expected) {
    for (const auto& line : bo2zm::cheat_sync::forwarded) std::cerr << "forwarded: [" << line << "]" << std::endl;
  }
  assert(bo2zm::cheat_sync::forwarded == expected);
  // The host's world settings, mirrored here, and given back.
  c::ApplyWorldState("timescale=0.5;bg_gravity=150;zm_nospawn=1;");
  assert(c::StopSpawns());
  {
    std::lock_guard lock(g_held_mutex);
    int mirrors = 0;
    for (const Held& h : g_held) mirrors += h.mirror && !h.release;
    assert(mirrors == 2);
  }
  assert(c::RunCommand("bo2zm_reset_cheats", ""));  // the reset keeps the host's world settings
  {
    std::lock_guard lock(g_held_mutex);
    for (const Held& h : g_held) assert(h.mirror ? !h.release : h.release);
  }
  c::ApplyWorldState("");
  assert(!c::StopSpawns());
  {
    std::lock_guard lock(g_held_mutex);
    for (const Held& h : g_held) assert(!h.mirror || h.release);
  }
  bo2zm::preset::client_in_game = false;
  UpdateActors(base);
  assert(!c::JoinedRemoteGame() && !LocalActor(1));
  std::cout << "PASS joined consoles: predicted actors, forwarded commands, mirrored world settings; host commands and "
               "move speed\n";
}

int main() {
  Memory memory;
  OptionsAndRouting(memory.base);
  Hooks(memory.base);
  QueueAndEpoch(memory.base);
  PerkHistory(memory.base);
  SnapshotAndBounds(memory.base);
  LongjmpBoundaries(memory.base);
  ReviewedRegressions(memory.base);
  ProjectionAndJumpAPI(memory.base);
  TestAllGod(memory.base);
  RemotePlayers(memory.base);
  JoinedAndHostCommands(memory.base);
  std::cout << "PASS production player options, permuted actor routing, hooks, captured queues, "
               "reload epochs, bounded/coherent overlays and guest-longjmp lock boundaries\n";
}
