// The port's own console commands, all named "bo2zm_*", through which the menu scripts (app/src/ui/menus.lua) reach
// the program:
//   bo2zm_preset <lowest|...|custom|0-5>          sets the graphics preset (the Graphics Settings menu)
//   bo2zm_gfx_<option> <level|index>              one graphics option; the preset becomes custom (bo2zm_preset.cpp)
//   bo2zm_profile <0-10>, bo2zm_profile_on <0|1|2>, bo2zm_profile_map <map> <game type> <start location>
//                                                 map profiles: the Graphics menu's and the loading screen's
//   bo2zm_fps <0|1>, bo2zm_god <0|1>, ...         the Extras menu's settings (bo2zm_extras.cpp)
//   bo2zm_player <controller 0-3> <bo2zm_command> personal settings/actions for an active local controller
//   bo2zm_hold <dvar> <value>, bo2zm_release <dvar> <value>, bo2zm_reset_cheats
//                                                 the Extras menu's built-in cheats (bo2zm_cheats.cpp)
//   bo2zm_powerup, bo2zm_perk, bo2zm_remove_perks, bo2zm_kill_zombies, bo2zm_max_points, ...
//                                                 the cheat pages' actions (bo2zm_cheats.cpp)
//   bo2zm_give_weapon <name>, bo2zm_pap_weapon, bo2zm_doors <open|close>, bo2zm_power <on|off>,
//   bo2zm_debris <remove|restore>                 the WEAPONS and DOORS + POWER pages (bo2zm_cheats.cpp)
//   bo2zm_edit_name                               EDIT USERNAME's text input (bo2zm_name.cpp)
//   bo2zm_online <host|join|cancel|off>           internet play from the home menu (bo2zm_online.cpp)
//   bo2zm_update <check|cancel|install>           the home screen's CHECK FOR UPDATES (bo2zm_updates.cpp)
//   bo2zm_credits <0|1>                           the home screen's credits line shown or not (bo2zm_credits.cpp)
//   bo2zm_log <text>                              writes "[lua] text" to the log (menu diagnostics)
//
// The game runs console text through sub_82402E78(..., text in r6): commands from configs, the menus' Engine.ExecNow
// (sub_824032C0) and bo2zm::autoexec::ExecuteCommands alike; it splits the text on ';' and new lines. This hook
// handles our commands first and blanks them out of the text for the game's call, so the game never sees an unknown
// command (everything else passes through untouched), and then puts the text back. Restoring it matters because the
// text can be the menu script's own string: Engine.ExecNow (sub_8271D9C8) hands the console the string's characters
// themselves (lua_tolstring, sub_826DEEF0: the string object + 0xC), so a constant such as "bo2zm_goto_round" blanked
// in place would stay blank, and a menu button with a fixed command would work only once per run.

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_cheats.h"
#include "bo2zm_controllers.h"
#include "bo2zm_bots.h"
#include "bo2zm_credits.h"
#include "bo2zm_extras.h"
#include "bo2zm_name.h"
#include "bo2zm_online.h"
#include "bo2zm_preset.h"
#include "bo2zm_updates.h"
#include "bo2zm_script_manager.h"

REX_EXTERN(__imp__sub_82402E78);

namespace {

constexpr uint32_t kMaxText = 16 * 1024;
constexpr std::string_view kPrefix = "bo2zm_";

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

bool RunPlayerScoped(std::string_view argument) {
  // One controller token and one command only; never recurse through general
  // dispatch or let a nested scope select another actor.
  if (argument.size() < 3 || argument[0] < '0' || argument[0] > '3' || !IsSpace(argument[1])) return true;
  for (const char c : argument) {
    if (c == ';' || c == '"' || (static_cast<unsigned char>(c) < 32 && c != '\t')) return true;
  }
  const int controller = argument[0] - '0';
  argument.remove_prefix(2);
  while (!argument.empty() && IsSpace(argument.front())) argument.remove_prefix(1);
  const auto space = argument.find_first_of(" \t");
  const auto name = argument.substr(0, space);
  auto value = space == std::string_view::npos ? std::string_view{} : argument.substr(space + 1);
  while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
  while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
  if (!name.starts_with(kPrefix) || name == "bo2zm_player") return true;
  const int local = bo2zm::cheats::LocalPlayerForController(controller);
  if (local < 0 || local >= 4) {
    REXLOG_WARN("[commands] controller {} has no active local player", controller);
    return true;
  }
  if (!bo2zm::extras::RunPlayerCommand(local, name, value))
    REXLOG_WARN("[commands] unsupported player command: {}", name);
  return true;
}

// Runs one of our commands. false: not one of ours (left for the game).
bool RunOurs(PPCContext& ctx, uint8_t* base, std::string_view command) {
  const size_t space = command.find_first_of(" \t");
  const std::string_view name = command.substr(0, space);
  std::string_view argument = space == std::string_view::npos ? std::string_view() : command.substr(space + 1);
  while (!argument.empty() && IsSpace(argument.front())) {
    argument.remove_prefix(1);
  }
  while (!argument.empty() && IsSpace(argument.back())) {
    argument.remove_suffix(1);
  }
  if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"') {
    argument = argument.substr(1, argument.size() - 2);
  }
  if (name == "bo2zm_player") return RunPlayerScoped(argument);
  if (name == "bo2zm_log") {
    REXLOG_INFO("[lua] {}", argument);
    return true;
  }
  if (name == "bo2zm_test_goto") {
    bo2zm::cheats::TestGoto(argument);
    return true;
  }
  if (bo2zm::bots::Command(name, argument)) {
    bo2zm::bots::EveryFrame(ctx, base);
    return true;
  }
  if (bo2zm::scripts::Command(name, argument)) {
    bo2zm::scripts::Publish(ctx, base);
    return true;
  }
  if (name == "bo2zm_update") {
    bo2zm::updates::Command(argument);
    return true;
  }
  if (name == "bo2zm_credits") {
    bo2zm::credits::SetShown(argument == "1");
    return true;
  }
  if (name == "bo2zm_preset") {
    bo2zm::preset::SetFromMenu(argument);
    return true;
  }
  if (name == "bo2zm_input_block") {
    bo2zm::preset::SetInputBlocked(argument == "1");
    return true;
  }
  if (name == "bo2zm_profile_map") {
    bo2zm::preset::MapLoading(argument);
    return true;
  }
  if (name == "bo2zm_profile") {
    bo2zm::preset::SelectProfileFromMenu(argument);
    return true;
  }
  if (name == "bo2zm_profile_on") {
    bo2zm::preset::EnableProfileFromMenu(argument);
    return true;
  }
  if (constexpr std::string_view kGfx = "bo2zm_gfx_";
      name.substr(0, kGfx.size()) == kGfx && bo2zm::preset::SetOptionFromMenu(name.substr(kGfx.size()), argument)) {
    return true;
  }
  if (bo2zm::controllers::RunCommand(name, argument) || bo2zm::extras::RunCommand(name, argument) ||
      bo2zm::name::RunCommand(name, argument) || bo2zm::online::RunCommand(name, argument)) {
    return true;
  }
  if (name.substr(0, kPrefix.size()) == kPrefix) {
    REXLOG_WARN("[commands] unknown command: {}", command);
    return true;  // ours by name: never passed to the game
  }
  return false;
}

}  // namespace

REX_HOOK_RAW(sub_82402E78) {
  const uint32_t text = ctx.r6.u32;
  char* p = nullptr;
  std::vector<char> original;  // the text as it came, when ours were blanked out of it
  if (text >= 0x10000) {
    p = reinterpret_cast<char*>(base + text);
    const size_t length = strnlen(p, kMaxText);
    if (std::string_view(p, length).find(kPrefix) != std::string_view::npos) {
      original.assign(p, p + length);
      size_t start = 0;
      while (start < length) {
        size_t end = start;
        bool quoted = false;
        while (end < length && (quoted || (p[end] != ';' && p[end] != '\n'))) {
          quoted ^= p[end] == '"';
          ++end;
        }
        size_t first = start;
        while (first < end && IsSpace(p[first])) {
          ++first;
        }
        const std::string_view command(p + first, end - first);
        if (command.substr(0, kPrefix.size()) == kPrefix && RunOurs(ctx, base, command)) {
          std::memset(p + first, ' ', end - first);
        }
        start = end + 1;
      }
    }
  }
  __imp__sub_82402E78(ctx, base);
  if (!original.empty()) {
    std::memcpy(p, original.data(), original.size());
  }
}
