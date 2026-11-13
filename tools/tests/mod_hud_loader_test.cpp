// The entire production loader supplies component roles and bytecode intervals.
#include <cassert>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <string>
#include <vector>
#include <fmt/format.h>
void FixtureLog(std::string) {}
#include "../../games/bo2/src/bo2zm_gsc_mods.cpp"

namespace {
std::array<uint8_t, 1024 * 1024> memory{};
std::string dvar_name;
int requested_bots = 1;
std::jmp_buf begin_jump;
bool simulate_begin = false;
bool simulate_load_failure = false;
unsigned begin_epochs = 0, cheat_begins = 0;
void FindDvar(PPCContext& ctx, uint8_t* base) {
  dvar_name = reinterpret_cast<const char*>(base + ctx.r3.u32);
  ctx.r3.u64 = 1;
}
void DvarString(PPCContext& ctx, uint8_t* base) {
  const char* value = dvar_name == "mapname" ? "zm_transit" : dvar_name == "g_gametype" ? "zclassic" : "town";
  std::strcpy(reinterpret_cast<char*>(base + 0x100), value);
  ctx.r3.u64 = 0x1000;
  std::strcpy(reinterpret_cast<char*>(base + ctx.r3.u32), value);
}
void LoadScript(PPCContext& ctx, uint8_t*) { assert(simulate_load_failure); ctx.r3.u64 = 0; }
void ComError(PPCContext&, uint8_t*) {
  assert(simulate_load_failure);
  bo2zm::gsc::MenuCodeRange range;
  assert(bo2zm::gsc::MenuCodeRanges(&range, 1) == 0);
  std::longjmp(begin_jump, 1);
}
bo2zm::gsc::Object MakeObject(std::string name, uint8_t content = 0) {
  bo2zm::gsc::Object object{{std::move(name), 128}, std::vector<uint8_t>(256, content), {}};
  bo2zm::gsc::Write32(object.bytes.data() + 0x14, 64);
  return object;
}
}
namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t address) {
  if (address == 0x82496430) return FindDvar;
  if (address == 0x82496990) return DvarString;
  if (address == 0x8252DA38) return LoadScript;
  if (address == 0x8240C468) return ComError;
  assert(false); return nullptr;
}
}
namespace bo2zm::mode { bool IsZombies() { return true; } }
namespace bo2zm::bots {
int Requested(const Context&, int) { return requested_bots; }
int HumanCount(const uint8_t*) { return 2; }
int Humans(const uint8_t* base) { return HumanCount(base); }
void RuntimeStatus(std::string_view) {}
void SetAvailability(bool, std::string_view) { assert(false); }
bool MigrateLegacySelection(std::string_view) { assert(false); return false; }
}
namespace bo2zm::scripts {
void SkippedMap(std::string_view) {}
void Boot() { assert(false); }
std::string Selected() { assert(false); return {}; }
bool LegacySizes() { assert(false); return false; }
std::filesystem::path Root() { assert(false); return {}; }
void Configured(std::string_view) { assert(false); }
void Rejected(std::string_view) { assert(simulate_load_failure); }
}
namespace rex::filesystem { std::filesystem::path GetExecutableFolder() { assert(false); return {}; } }
namespace bo2zm::cheats {
void BeginMapLoad() { ++cheat_begins; }
void EndMapLoad() { assert(false); }
}
namespace bo2zm::mod_hud_fit {
void BeginEpoch() { ++begin_epochs; }
void EndMapLoad() { assert(false); }
}
void __imp__sub_822CAE50(PPCContext&, uint8_t*) { assert(false); }
void __imp__sub_8252DED0(PPCContext&, uint8_t*) {
  assert(simulate_begin);
  bo2zm::gsc::MenuCodeRange range;
  assert(bo2zm::gsc::MenuCodeRanges(&range, 1) == 0);
  std::longjmp(begin_jump, 1);
}
void __imp__sub_823599E8(PPCContext&, uint8_t*) { assert(false); }
void __imp__sub_8252EED8(PPCContext&, uint8_t*) { assert(false); }
void __imp__sub_8252F808(PPCContext&, uint8_t*) { assert(false); }

int main() {
  using namespace bo2zm::gsc;
  Component menu, bots;
  menu.objects = {MakeObject("maps/mp/menu.gsc"), MakeObject("maps/mp/shared.gsc")};
  bots.objects = {MakeObject("maps/mp/bots.gsc"), MakeObject("maps/mp/shared.gsc")};
  menu_component = menu; bot_component = bots;
  PPCContext ctx{}; ctx.r1.u64 = 0x70000;
  SelectGeneration(ctx, memory.data());
  assert(active_bot_count == 1 && scripts.size() == 3);
  MenuCodeRange ranges[4]{};
  assert(MenuCodeRanges(ranges, 4) == 0);
  NewGeneration(memory.data(), true);
  assert(MenuCodeRanges(ranges, 4) == 1);
  const auto menu_pc = ranges[0].begin + 1;
  uint32_t bot_pc = 0, shared_pc = 0;
  for (const auto& script : scripts) {
    const auto pc = script.buffer + 65;
    if (script.info.name == "maps/mp/bots.gsc") bot_pc = pc;
    if (script.info.name == "maps/mp/shared.gsc") shared_pc = pc;
  }
  uint32_t frames[] = {0x82000001, menu_pc};
  assert(MenuAncestry(frames, 2));
  frames[0] = bot_pc; assert(!MenuAncestry(frames, 2));
  frames[0] = shared_pc; assert(!MenuAncestry(frames, 2));
  frames[0] = 0x82000001; frames[1] = ranges[0].end;
  assert(!MenuAncestry(frames, 2));
  assert(!MenuAncestry(nullptr, 2) && !MenuAncestry(frames, 32));
  assert(!MenuCodeRanges(nullptr, 4) && !MenuCodeRanges(ranges, 0));
  menu_ranges_active = false;
  assert(!MenuCodeRanges(ranges, 4) && !MenuAncestry(&menu_pc, 1));
  // Bots disabled: identical menu bytes are no longer a shared loaded module.
  requested_bots = 0;
  SelectGeneration(ctx, memory.data()); NewGeneration(memory.data(), true);
  assert(active_bot_count == 0 && MenuCodeRanges(ranges, 4) == 2);
  assert(MenuCodeRanges(ranges, 1) == 1);
  // Refused bot composition likewise preserves menu-only provenance.
  requested_bots = 1;
  bot_component->objects[1].bytes.back() = 7;
  SelectGeneration(ctx, memory.data()); NewGeneration(memory.data(), true);
  assert(active_bot_count == 0 && MenuCodeRanges(ranges, 4) == 2);
  // Ineligible menus publish no code, even if bots remain eligible.
  menu_component->options.maps = "zm_tomb";
  SelectGeneration(ctx, memory.data()); NewGeneration(memory.data(), true);
  assert(!menu_permitted && MenuCodeRanges(ranges, 4) == 0);
  NewGeneration(memory.data(), false);
  assert(!MenuCodeRanges(ranges, 4));
  menu_component->options.maps.clear();
  SelectGeneration(ctx, memory.data()); NewGeneration(memory.data(), true);
  assert(MenuCodeRanges(ranges, 4) > 0);
  simulate_begin = true;
  if (!setjmp(begin_jump)) { ctx.r3.u64 = 0; sub_8252DED0(ctx, memory.data()); assert(false); }
  assert(begin_epochs == 1 && cheat_begins == 1 && MenuCodeRanges(ranges, 4) == 0);
  simulate_load_failure = true;
  menu_component->options.entry = "maps/mp/menu.gsc";
  SelectGeneration(ctx, memory.data()); NewGeneration(memory.data(), true);
  assert(MenuCodeRanges(ranges, 4) > 0);
  if (!setjmp(begin_jump)) { LoadEntry(ctx, memory.data()); assert(false); }
  assert(begin_epochs == 2 && MenuCodeRanges(ranges, 4) == 0);
  std::puts("PASS whole production loader selection/generation: menu, bot/shared veto, disabled/refused bots, map eligibility, interval bounds and invalidation");
}
