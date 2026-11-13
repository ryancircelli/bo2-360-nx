// Compiled with production bridge functions and the core's real pure option bank.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <rex/ppc/context.h>
#include "../../games/bo2/src/bo2zm_pad.h"
#include "../../games/bo2/src/bo2zm_bots.h"
// CORE_STATE_HEADER
// BOT_REGISTRATION_CONSTANTS

bool bo2zm_test_god = false, bo2zm_test_ammo = false;
std::string bo2zm_test_start;
#define REXCVAR_GET(name) (name)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#undef REX_EXTERN
#define REX_EXTERN(name) void name(PPCContext&, uint8_t*)
#define REX_HOOK_RAW(name) void name(PPCContext& ctx, uint8_t* base)
std::array<int, 4> mapping{-1, 0, 1, -1};
int primary = 1;
bool zombies = true;
std::array<bo2zm::cheats::detail::PlayerOptions, 4> players;
std::map<std::string, int> global_values;
std::map<std::string, std::string> saved, ui;
std::vector<std::pair<std::string, std::string>> saved_writes;
std::vector<std::string> batches;
std::vector<std::tuple<int, std::string, std::string>> actions;
std::vector<std::pair<int, uint32_t>> pad_calls;
unsigned global_personal_gets = 0, global_personal_sets = 0;
unsigned preset_pads = 0, stock_commands = 0, general_commands = 0;
bool pad_connected = true;
std::vector<std::string> lifecycle;
std::jmp_buf boundary;
bool loading = false, fail_begin = false, fail_start = false, fail_entry = false;
int supplied_humans = 2, observed_humans = -1;
unsigned human_reads = 0, bot_requests = 0, bot_registrations = 0, bot_writes = 0, bot_allocations = 0;
int bot_limit = -1, bot_value = -1;
uint32_t bot_dvar_text = 0;
namespace rex {
struct Memory {
  uint32_t SystemHeapAlloc(size_t bytes, size_t alignment) {
    assert(bytes == 128 && alignment == 16); ++bot_allocations; return 0x2000;
  }
};
struct Runtime {
  static Runtime* instance() { static Runtime value; return &value; }
  Memory* memory() { static Memory value; return &value; }
};
}
namespace bo2zm::guest {
template <uint32_t address> void Call(PPCContext& context, uint8_t* base) {
  if constexpr (address == kRegisterInt) {
    assert(std::strcmp(reinterpret_cast<char*>(base + context.r3.u32), "bo2_zm_bots_count") == 0);
    bot_limit = int(context.r6.u32); ++bot_registrations; context.r3.u64 = 0x3000;
  } else {
    static_assert(address == kSetInt);
    assert(context.r3.u64 == 0x3000); bot_value = int(context.r4.u32); ++bot_writes;
  }
}
}
namespace bo2zm::cheats {
void BeginMapLoad() { lifecycle.emplace_back("begin"); loading = true; }
void EndMapLoad() { lifecycle.emplace_back("end"); loading = false; }
}
namespace bo2zm::gsc {
int active_bot_count = 0;
bool active = false, menu_permitted = false;
std::string mod_name = "authored";
void SelectGeneration(PPCContext&, uint8_t*, bool = true) { lifecycle.emplace_back("select"); }
bool SetBotCount(PPCContext&, uint8_t*, int) { return true; }
void PrepareReplacement(PPCContext&, uint8_t*, bool) { lifecycle.emplace_back("prepare"); }
void NewGeneration(uint8_t*, bool) { lifecycle.emplace_back("generation"); }
void LoadEntry(PPCContext&, uint8_t*) { lifecycle.emplace_back("load-entry"); }
void StartEntry(PPCContext&, uint8_t*) {
  lifecycle.emplace_back("entry");
  if (fail_entry) std::longjmp(boundary, 1);
}
}

int Controller(int local) {
  for (int c = 0; c < 4; ++c) if (mapping[c] == local) return c;
  return -1;
}
namespace bo2zm::mode { bool IsZombies() { return zombies; } }
namespace bo2zm::cheats {
int LocalPlayerForController(int c) { return !loading && c >= 0 && c < 4 ? mapping[c] : -1; }
int PrimaryLocalPlayer() { return loading ? -1 : primary; }
bool IsPrimaryController(int c) { return LocalPlayerForController(c) >= 0 && LocalPlayerForController(c) == primary; }
bool GetPlayerOption(int local, std::string_view name, int& value) {
  const int c = Controller(local);
  return c >= 0 && players[c].Get(name, value);
}
bool SetPlayerOption(int local, std::string_view name, int value) {
  const int c = Controller(local);
  if (c < 0) return false;
  if (local == primary && name == "cheats" && value == 0) global_values["zm_nospawn"] = 0;
  return players[c].Set(name, value);
}
void SetGodMode(bool value) {
  const int c = Controller(primary);
  if (c >= 0) players[c].Direct(detail::Option::God, value);
}
void SetInfiniteAmmo(bool value) {
  const int c = Controller(primary);
  if (c >= 0) players[c].Direct(detail::Option::Ammo, value);
}
void GoToRound(int round) { actions.emplace_back(primary, "round", std::to_string(round)); }
bool RunPlayerCommand(int local, std::string_view name, std::string_view argument) {
  constexpr std::string_view supported[] = {"bo2zm_perk", "bo2zm_remove_perks", "bo2zm_powerup", "bo2zm_max_points",
      "bo2zm_give_weapon", "bo2zm_pap_weapon", "bo2zm_reset_cheats", "bo2zm_hold", "bo2zm_release",
      "bo2zm_doors", "bo2zm_power", "bo2zm_debris", "bo2zm_kill_zombies"};
  if (std::find(std::begin(supported), std::end(supported), name) == std::end(supported)) return false;
  // Deliberately no world authorization here: exercise the production bridge guard.
  actions.emplace_back(local, name, argument);
  if (name == "bo2zm_reset_cheats") players[Controller(local)].ResetGameplay();
  return true;
}
uint16_t OnPad(const bo2zm::pad::Pad& pad, int local) {
  pad_calls.emplace_back(local, pad.user);
  int master = 0, combo = 0;
  GetPlayerOption(local, "cheats", master);
  GetPlayerOption(local, "combo", combo);
  return master && combo && pad.aim && pad.Pressed(bo2zm::pad::kDown) ? bo2zm::pad::kDown : 0;
}
}
bool Personal(std::string_view name) { int value; return players[0].Get(name, value); }
int LegacyGet(const char* name) { if (Personal(name)) ++global_personal_gets; return global_values[name]; }
void LegacySet(const char* name, int value) { if (Personal(name)) ++global_personal_sets; global_values[name] = value; }
// OPTION_BOUNDARY_FUNCTIONS

namespace bo2zm::settings {
std::optional<std::string> Get(std::string_view key) {
  const auto found = saved.find(std::string(key));
  return found == saved.end() ? std::nullopt : std::optional(found->second);
}
bool Set(std::string_view key, std::string_view value) {
  saved[std::string(key)] = value;
  saved_writes.emplace_back(key, value);
  return true;
}
std::filesystem::path File() { return "authored-preferences"; }
}
namespace bo2zm::autoexec {
void ExecuteCommands(PPCContext&, uint8_t*, std::string_view text) {
  assert(text.size() < 2000);
  batches.emplace_back(text);
  while (!text.empty()) {
    const auto end = text.find(';');
    auto item = text.substr(0, end);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    if (item.starts_with("set ")) {
      item.remove_prefix(4);
      const auto space = item.find(' ');
      if (space != std::string_view::npos) ui[std::string(item.substr(0, space))] = item.substr(space + 1);
    }
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
}
}
namespace bo2zm::debug {
bool RunCommand(std::string_view, std::string_view) { return false; }
void LoadCustom() {}
void Sync() {}
std::string BuildText() { return "fixture"; }
std::string LogFileName() { return "fixture.log"; }
std::string PurgeLabel() { return "authored"; }
}
namespace rex::cvar {
enum class Source { kDefault };
Source GetFlagSource(std::string_view) { return Source::kDefault; }
}
namespace bo2zm {
bool CampaignAvailable() { return true; }
}
bool LocalCamoAssetsReady() { return false; }
void ApplyVsync() {}
namespace bo2zm::scripts {
void EveryFrame(PPCContext&, uint8_t*) {}
bool Command(std::string_view, std::string_view) { ++general_commands; return false; }
void Publish(PPCContext&, uint8_t*) {}
void Configured(std::string_view) { lifecycle.emplace_back("configured"); }
}
namespace bo2zm::bots {
bool Command(std::string_view, std::string_view) { ++general_commands; return false; }
void EveryFrame(PPCContext&, uint8_t*) {}
void RuntimeStatus(std::string_view) {}
int HumanCount(const uint8_t*) { ++human_reads; return supplied_humans; }
int Requested(const Context&, int humans) { ++bot_requests; observed_humans = humans; return 73; }
}
namespace bo2zm::credits { void SetShown(bool) { ++general_commands; } }
namespace bo2zm::name { bool RunCommand(std::string_view, std::string_view) { ++general_commands; return false; } }
namespace bo2zm::updates { void Command(std::string_view) { ++general_commands; } }
namespace bo2zm::preset {
uint16_t OnPad(const bo2zm::pad::Pad&) { ++preset_pads; return 0; }
void SetFromMenu(std::string_view) { ++general_commands; }
void MapLoading(std::string_view) { ++general_commands; }
void SelectProfileFromMenu(std::string_view) { ++general_commands; }
void EnableProfileFromMenu(std::string_view) { ++general_commands; }
bool SetOptionFromMenu(std::string_view, std::string_view) { ++general_commands; return true; }
}
namespace bo2zm::extras {
bool RunCommand(std::string_view, std::string_view);
bool RunPlayerCommand(int, std::string_view, std::string_view);
void EveryFrame(PPCContext&, uint8_t*);
}

// PRODUCTION_EXTRAS_CODE
// PRODUCTION_BOT_COUNT_CODE
int ProductionRequest(bool zombies_map) {
  const bo2zm::bots::Context context{"zm_transit", "zstandard", "town"};
  const uint8_t* base = nullptr;
  // PRODUCTION_BOT_REQUEST
  return requested;
}
REX_EXTERN(__imp__sub_82402E78);
// PRODUCTION_COMMANDS_CODE

void __imp__sub_82402E78(PPCContext& ctx, uint8_t* base) {
  ++stock_commands;
  assert(std::strstr(reinterpret_cast<char*>(base + ctx.r6.u32), "bo2zm_player") == nullptr);
}
void __imp__sub_827D8A48(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = pad_connected ? 0 : 1; }
// PRODUCTION_PAD_CODE
void __imp__sub_8252DED0(PPCContext& ctx, uint8_t*) {
  lifecycle.emplace_back("original-begin");
  if (ctx.r3.u32 == 0) assert(loading);
  ctx.r3.u64 = 99; // The hook must use its captured pre-call instance.
  if (fail_begin) std::longjmp(boundary, 1);
}
void __imp__sub_823599E8(PPCContext&, uint8_t*) {
  lifecycle.emplace_back("original-start");
  if (fail_start) std::longjmp(boundary, 1);
}
// PRODUCTION_LIFECYCLE_CODE

std::array<uint8_t, 0x20000> guest{};
PPCContext ctx{};
int Value(int controller, std::string_view name) {
  int result = -1;
  assert(players[controller].Get(name, result));
  return result;
}
void Command(std::string_view text) { assert(RunOurs(ctx, guest.data(), text)); }
void Tick() { g_changed.store(true); bo2zm::extras::EveryFrame(ctx, guest.data()); }

void Dispatch() {
  Command("bo2zm_player 2 bo2zm_cheats 1");
  Command("bo2zm_player 1 bo2zm_cheats 1");
  Command("bo2zm_player 1 bo2zm_god 1");
  assert(Value(1, "god") == 1 && Value(2, "god") == 0);
  Command("bo2zm_god 1"); // Legacy helper follows nonzero primary local 1/controller 2.
  assert(Value(2, "god") == 1);
  Command("bo2zm_player 1 bo2zm_max_points");
  assert(std::get<0>(actions.back()) == 0);
  const auto before = actions.size();
  for (std::string_view world : {"bo2zm_hold g_speed 999", "bo2zm_release g_speed 190", "bo2zm_doors open",
      "bo2zm_power on", "bo2zm_debris remove", "bo2zm_powerup insta_kill", "bo2zm_kill_zombies",
      "bo2zm_goto_round", "bo2zm_round 7", "bo2zm_zm_nospawn 1"}) {
    Command("bo2zm_player 1 " + std::string(world));
  }
  assert(actions.size() == before && global_values["zm_nospawn"] == 0 && g_round_choice == 0);
  for (std::string_view forged : {"bo2zm_player 0 bo2zm_god 0", "bo2zm_player 4 bo2zm_god 0",
      "bo2zm_player -1 bo2zm_god 0", "bo2zm_player 01 bo2zm_god 0", "bo2zm_player 1 bo2zm_player 2 bo2zm_god 0",
      "bo2zm_player 1 bo2zm_god 0;bo2zm_god 0", "bo2zm_player 1 bo2zm_god 0\rbo2zm_god 0",
      "bo2zm_player 1 bo2zm_update install", "bo2zm_player 1 bo2zm_preset 0"}) Command(forged);
  assert(Value(1, "god") == 1 && Value(2, "god") == 1 && actions.size() == before);
  Command("bo2zm_player 2 bo2zm_doors open");
  assert(std::get<0>(actions.back()) == primary);
  Command("bo2zm_player 2 bo2zm_zm_nospawn 1");
  Command("bo2zm_player 2 bo2zm_cheats 0");
  assert(Value(2, "god") == 1 && players[2].Effective(bo2zm::cheats::detail::Option::God) == 0);
  Command("bo2zm_player 1 bo2zm_cheats 0");
  Command("bo2zm_player 1 bo2zm_cheats 1");
  assert(Value(1, "god") == 1 && global_values["zm_nospawn"] == 0);
  Command("bo2zm_player 2 bo2zm_cheats 1");
  assert(global_values["zm_nospawn"] == 1 && Value(2, "god") == 1);
  Command("bo2zm_player 1 bo2zm_reset_cheats");
  assert(Value(1, "god") == 0 && Value(2, "god") == 1 && Value(1, "cheats") == 1);
  zombies = false;
  const auto mode_actions = actions.size();
  Command("bo2zm_player 1 bo2zm_god 1");
  Command("bo2zm_player 2 bo2zm_doors close");
  assert(Value(1, "god") == 0 && actions.size() == mode_actions);
  zombies = true;

  const std::string text = "bo2zm_player 1 bo2zm_god 1; echo stock; bo2zm_player 2 bo2zm_max_points";
  std::memcpy(guest.data() + 0x10000, text.c_str(), text.size() + 1);
  ctx.r6.u64 = 0x10000;
  sub_82402E78(ctx, guest.data());
  assert(stock_commands == 1 && std::strcmp(reinterpret_cast<char*>(guest.data() + 0x10000), text.c_str()) == 0);
  assert(Value(1, "god") == 1 && std::get<0>(actions.back()) == primary);
  assert(global_personal_gets == 0 && global_personal_sets == 0);
}

void Preferences() {
  saved = {{"cheats", "on"}, {"field_of_view", "70"}, {"player.1.cheats", "off"},
      {"player.1.field_of_view", "90"}, {"player.2.fly_combo", "on"}, {"player.1.god", "on"}, {"god", "on"}};
  Tick();
  assert(Value(2, "cheats") == 1 && Value(1, "cheats") == 0);
  assert(Value(1, "fov") == 7 && players[1].Effective(bo2zm::cheats::detail::Option::Fov) == 2);
  assert(Value(1, "god") == 0 && Value(2, "god") == 0);
  assert(ui["bo2zm_p1_fov_ui"] == "7" && ui["bo2zm_p2_fov_ui"] == "3" && ui["bo2zm_fov_ui"] == "3");
  assert(ui["bo2zm_p0_god_ui"] == "-1" && saved_writes.empty());
  Command("bo2zm_player 1 bo2zm_cheats 1");
  Command("bo2zm_player 1 bo2zm_fov 80");
  Command("bo2zm_player 1 bo2zm_god 1");
  Tick();
  assert(saved["player.1.field_of_view"] == "80" && saved["field_of_view"] == "70");
  assert(saved["player.1.cheats"] == "on" && saved["cheats"] == "on");
  for (const auto& [key, value] : saved_writes) { (void)value; assert(key.find("god") == std::string::npos); }
  Command("bo2zm_player 1 bo2zm_cheats 0");
  Tick();
  assert(ui["bo2zm_p1_god_ui"] == "1" && ui["bo2zm_p2_god_ui"] == "0");
  assert(Value(2, "cheats") == 1 && players[1].Effective(bo2zm::cheats::detail::Option::God) == 0);
  mapping[1] = -1;
  Tick();
  assert(ui["bo2zm_p1_god_ui"] == "-1");
  mapping[1] = 3;
  mapping[2] = 0;
  primary = 0;
  Tick();
  assert(ui["bo2zm_p1_god_ui"] == "1" && ui["bo2zm_p2_god_ui"] == "0");
  assert(Value(1, "fov") == 5 && ui["bo2zm_fov_ui"] == "3");
  assert(global_personal_gets == 0 && global_personal_sets == 0);
}

void PadRead(int user) {
  const uint16_t buttons = __builtin_bswap16(uint16_t(bo2zm::pad::kDown));
  std::memcpy(guest.data() + 0x8004, &buttons, 2);
  guest[0x8006] = 255;
  ctx.r3.u64 = user;
  ctx.r4.u64 = 0x8000;
  sub_827D8A48(ctx, guest.data());
}
void Pads() {
  players[1].Set("cheats", 1); players[1].Set("combo", 1);
  players[2].Set("cheats", 1); players[2].Set("combo", 1);
  PadRead(1);
  assert(pad_calls.back() == std::make_pair(0, uint32_t(1)));
  assert(guest[0x8004] == 0 && guest[0x8005] == 0 && preset_pads == 0);
  PadRead(2);
  assert(pad_calls.back() == std::make_pair(1, uint32_t(2)));
  assert(guest[0x8004] == 0 && guest[0x8005] == 0 && preset_pads == 1);
  PadRead(2);
  assert(guest[0x8005] == bo2zm::pad::kDown); // Held read, not a second press.
  const auto calls = pad_calls.size();
  PadRead(0); PadRead(4);
  assert(pad_calls.size() == calls && guest[0x8005] == bo2zm::pad::kDown);
  pad_connected = false;
  PadRead(1);
  assert(pad_calls.size() == calls);
}
void Lifecycle() {
  for (bool selected : {false, true}) {
    bo2zm::gsc::active = bo2zm::gsc::menu_permitted = selected;
    lifecycle.clear(); loading = false; ctx.r3.u64 = 0;
    sub_8252DED0(ctx, guest.data());
    assert(loading && lifecycle.front() == "begin" && lifecycle[1] == "original-begin");
    sub_823599E8(ctx, guest.data());
    assert(!loading && lifecycle[lifecycle.size() - 3] == "original-start" &&
           lifecycle[lifecycle.size() - 2] == "entry" && lifecycle.back() == "end");
  }
  lifecycle.clear(); ctx.r3.u64 = 1;
  sub_8252DED0(ctx, guest.data());
  assert((lifecycle == std::vector<std::string>{"original-begin"}));
  for (int point = 0; point < 3; ++point) {
    lifecycle.clear(); loading = false; ctx.r3.u64 = 0;
    fail_begin = point == 0; fail_start = point == 1; fail_entry = point == 2;
    if (setjmp(boundary) == 0) {
      sub_8252DED0(ctx, guest.data());
      sub_823599E8(ctx, guest.data());
      assert(false);
    }
    assert(loading && std::count(lifecycle.begin(), lifecycle.end(), "begin") == 1 &&
           std::find(lifecycle.begin(), lifecycle.end(), "end") == lifecycle.end());
  }
}
void Bots() {
  for (int humans = 1; humans <= 4; ++humans) {
    supplied_humans = humans; human_reads = bot_requests = 0;
    assert(ProductionRequest(true) == 73);
    assert(human_reads == 1 && bot_requests == 1 && observed_humans == humans);
  }
  human_reads = bot_requests = 0;
  assert(ProductionRequest(false) == 0 && human_reads == 0 && bot_requests == 0);
  ctx.r1.u64 = 0xf000;
  const PPCContext input = ctx;
  for (int count = 0; count <= 3; ++count) {
    bot_dvar_text = 0; bot_allocations = bot_registrations = bot_writes = 0;
    assert(SetBotCount(ctx, guest.data(), count));
    assert(bot_allocations == 1 && bot_registrations == 1 && bot_writes == 1 && bot_limit == 3 && bot_value == count);
    assert(std::memcmp(&ctx, &input, sizeof(ctx)) == 0);
  }
  for (int count : {-1, 4, 100}) {
    bot_dvar_text = 0; bot_allocations = bot_registrations = bot_writes = 0;
    assert(!SetBotCount(ctx, guest.data(), count));
    assert(bot_allocations == 0 && bot_registrations == 0 && bot_writes == 0);
    assert(std::memcmp(&ctx, &input, sizeof(ctx)) == 0);
  }
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string_view test = argv[1];
  if (test == "dispatch") Dispatch();
  else if (test == "preferences") Preferences();
  else if (test == "pad") Pads();
  else if (test == "bots") Bots();
  else { assert(test == "lifecycle"); Lifecycle(); }
  std::cout << "production player bridge " << test << " passed\n";
}
