#include "bo2zm_script_manager.h"
#include "bo2zm_gsc_catalog.h"
#include "bo2zm_autoexec.h"
#include "bo2zm_launch_options.h"
#include "bo2zm_mode.h"
#include "bo2zm_settings.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(bo2zm_gsc_mod, "", "BO2", "Initial Zombies mod folder; the Script Loader's saved choice takes priority")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2mp_gsc_mod, "", "BO2", "Initial Multiplayer mod folder; the Script Loader's saved choice takes priority")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_gsc_mods_root, "", "BO2", "Script root; empty uses scripts beside the executable")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_gsc_legacy_sizes, false, "BO2", "Accept legacy GSC code-size omissions and header literals")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::scripts {
namespace {
std::mutex mutex;
std::vector<std::string> mods;
std::vector<std::string> issues;
std::vector<std::pair<std::string, std::string>> targets;
std::string selected, configured, status = "Disabled";
std::string route = "No mod selected", base_path = "Not used", preview = "Choose a mod, then check its scripts";
std::string scan_status = "Not scanned";
std::optional<std::string> browsing_choice;
int pick = 0;
bool dirty = true, restart = false;
bool preview_uses_files = false;

gsc::ScriptMode TargetMode() {
#if defined(BO2_GAME_CAMPAIGN)
  return gsc::ScriptMode::Zombies;
#else
  return mode::IsZombies() ? gsc::ScriptMode::Zombies : gsc::ScriptMode::Multiplayer;
#endif
}
const char* SelectionKey() {
  return TargetMode() == gsc::ScriptMode::Multiplayer ? "script_mod_mp" : "script_mod";
}
const char* ModeName() {
  return TargetMode() == gsc::ScriptMode::Multiplayer ? "Multiplayer" : "Zombies";
}

std::string Clean(std::string_view text, size_t limit = 180) {
  std::string result;
  for (const auto c : text.substr(0, limit))
    result += c >= 32 && c < 127 && c != '"' && c != ';' && c != '\\' ? c : ' ';
  return result;
}

// These helpers run with the manager mutex held. Only an explicit check/load
// reads script bodies; ordinary frame polling publishes cached text.
void PreviewMetadata() {
  targets.clear();
  route = "No mod selected";
  preview_uses_files = false;
  base_path = "Not used";
  preview = "Choose a mod, then check its scripts";
  if (pick <= 0 || size_t(pick) > mods.size()) return;
  try {
    const auto options = gsc::ReadModOptions(Root() / mods[pick - 1]);
    preview_uses_files = options.names_from_files;
    route = options.names_from_files ? "FILES (folder paths)" : "INTERNAL (embedded paths)";
    if (options.names_from_files) base_path = options.base == "/" ? "/ (game script root)" : options.base;
    preview = "Not checked; choose Check Scripts to inspect targets";
    if (!options.maps.empty()) preview = "Only on maps: " + options.maps + "; check scripts to inspect targets";
  } catch (const std::exception& e) {
    route = "Invalid mod.ini";
    preview = "Check failed: " + Clean(e.what());
  }
}

bool CheckSelection() {
  PreviewMetadata();
  dirty = true;
  if (pick <= 0 || size_t(pick) > mods.size()) return false;
  try {
    const auto objects = gsc::ReadMod(Root(), mods[pick - 1], LegacySizes(), TargetMode());
    for (const auto& object : objects) targets.emplace_back(object.source, object.info.name);
    preview = std::to_string(objects.size()) + " scripts valid; review replacement targets";
    const auto options = gsc::ReadModOptions(Root() / mods[pick - 1]);
    if (!options.maps.empty()) preview += "; only on maps: " + options.maps;
    if (!preview_uses_files && std::any_of(targets.begin(), targets.end(), [](const auto& item) {
          return item.second.ends_with("/_imcsx_gsc_studio.gsc");
        }))
      preview = "Warning: generic Studio target; confirm its entry path or use FILES with known targets";
    return true;
  } catch (const std::exception& e) {
    preview = "Check failed: " + Clean(e.what());
    return false;
  }
}

void Refresh() {
  std::vector<std::string> found_issues;
  std::vector<std::string> found;
  try {
    found = gsc::ListMods(Root(), TargetMode(), &found_issues);
  } catch (const std::exception& e) {
    std::lock_guard lock(mutex);
    mods.clear();
    issues = {"Scan failed: " + Clean(e.what(), 450)};
    pick = 0;
    PreviewMetadata();
    scan_status = issues.front();
    preview = "Scan failed; refresh the list to retry";
    dirty = true;
    return;
  }
  std::lock_guard lock(mutex);
  const auto candidate = browsing_choice ? *browsing_choice : selected;
  mods = found;
  issues = std::move(found_issues);
  pick = 0;
  for (size_t i = 0; i < mods.size(); ++i) if (mods[i] == candidate) pick = int(i + 1);
  scan_status = std::to_string(mods.size()) + " matching mods; " + std::to_string(issues.size()) + " folders need setup";
  PreviewMetadata();
  dirty = true;
}

bool Save(std::string_view mod) {
  if (!settings::SetAt(launch::SharedUserRoot() / "bo2zm_settings.txt", SelectionKey(), mod)) {
    status = "Could not save selection";
    dirty = true;
    return false;
  }
  selected = mod;
  return true;
}
}  // namespace

std::filesystem::path Root() {
  const auto override = REXCVAR_GET(bo2zm_gsc_mods_root);
  return override.empty() ? rex::filesystem::GetExecutableFolder() / "scripts" : std::filesystem::path(override);
}
bool LegacySizes() { return REXCVAR_GET(bo2zm_gsc_legacy_sizes); }
bool SupportsCurrentMode() {
#if defined(BO2_GAME_CAMPAIGN)
  return false;
#else
  return true;
#endif
}
std::string Selected() { std::lock_guard lock(mutex); return selected; }

void Boot() {
  {
    std::lock_guard lock(mutex);
    const auto saved = settings::GetAt(launch::SharedUserRoot() / "bo2zm_settings.txt", SelectionKey());
    selected = saved ? *saved : (TargetMode() == gsc::ScriptMode::Multiplayer ?
        REXCVAR_GET(bo2mp_gsc_mod) : REXCVAR_GET(bo2zm_gsc_mod));
    if (!selected.empty() && !gsc::ModName(selected)) {
      selected.clear(); status = "Invalid saved mod name";
    } else {
      status = selected.empty() ? "Disabled" : std::string("Selected for ") + ModeName();
    }
    dirty = true;
  }
  try { Refresh(); }
  catch (const std::exception& e) { Rejected(e.what()); }
}
void Configured(std::string_view mod) {
  std::lock_guard lock(mutex);
  configured = mod;
  status = std::string("Enabled in ") + ModeName();
  dirty = true;
}
void Rejected(std::string_view reason) {
  std::lock_guard lock(mutex);
  status = "Not loaded: " + Clean(reason);
  dirty = true;
}
void SkippedMap(std::string_view maps) {
  std::lock_guard lock(mutex);
  configured.clear();
  status = "Inactive here; only on maps: " + Clean(maps);
  dirty = true;
}

bool Command(std::string_view name, std::string_view argument) {
  if (name == "bo2zm_scripts_refresh") {
    try { Refresh(); }
    catch (const std::exception& e) { Rejected(e.what()); }
    return true;
  }
  if (name != "bo2zm_script_pick" && name != "bo2zm_scripts_load" && name != "bo2zm_scripts_unload" &&
      name != "bo2zm_scripts_check") return false;
  std::lock_guard lock(mutex);
  if (name == "bo2zm_script_pick") {
    int value = -1;
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == argument.data() + argument.size() &&
        value >= 0 && size_t(value) <= mods.size()) {
      pick = value;
      browsing_choice = value ? mods[value - 1] : "";
      PreviewMetadata();
      dirty = true;
    }
    return true;
  }
  if (name == "bo2zm_scripts_check") {
    CheckSelection();
    return true;
  }
  if (name == "bo2zm_scripts_unload") {
    if (Save("")) {
      restart = SupportsCurrentMode() && !configured.empty();
      status = restart ? "Unloading: restarting game" : "Disabled";
      dirty = true;
    }
    return true;
  }
  if (pick <= 0 || size_t(pick) > mods.size()) {
    status = "Choose a mod first"; dirty = true; return true;
  }
  const auto choice = mods[pick - 1];
  if (CheckSelection()) {
    if (Save(choice)) {
      restart = SupportsCurrentMode();
      status = restart ? "Loading: restarting game" :
          "Enabled for next Zombies launch";
      dirty = true;
    }
  } else {
    status = preview;
    dirty = true;
  }
  return true;
}

void Publish(PPCContext& ctx, uint8_t* base) {
  std::vector<std::string> updates;
  {
    std::lock_guard lock(mutex);
    if (dirty) {
      dirty = false;
      updates.push_back("set bo2zm_scripts_count_ui " + std::to_string(mods.size()) +
          ";set bo2zm_script_pick_ui " + std::to_string(pick) +
          ";set bo2zm_scripts_active_ui \"" + Clean(configured.empty() ? "None" : configured) +
          "\";set bo2zm_scripts_status_ui \"" + Clean(status) + "\"");
      updates.push_back("set bo2zm_scripts_route_ui \"" + Clean(route) +
          "\";set bo2zm_scripts_base_ui \"" + Clean(base_path) +
          "\";set bo2zm_scripts_preview_ui \"" + Clean(preview) + "\"");
      updates.push_back("set bo2zm_scripts_scan_ui \"" + Clean(scan_status, 512) + "\"" +
          ";set bo2zm_scripts_target_count_ui " + std::to_string(targets.size()) +
          ";set bo2zm_scripts_issue_count_ui " + std::to_string(issues.size()));
      for (size_t i = 0; i < mods.size(); ++i)
        updates.push_back("set bo2zm_script_" + std::to_string(i) + "_name_ui \"" + mods[i] + "\"");
      for (size_t i = 0; i < issues.size(); ++i)
        updates.push_back("set bo2zm_scripts_issue_" + std::to_string(i) + "_ui \"" + Clean(issues[i], 512) + "\"");
      for (size_t i = 0; i < targets.size(); ++i) {
        updates.push_back("set bo2zm_scripts_source_" + std::to_string(i) + "_ui \"" + Clean(targets[i].first, 512) + "\"");
        updates.push_back("set bo2zm_scripts_target_" + std::to_string(i) + "_ui \"" + Clean(targets[i].second, 512) + "\"");
      }
    }
  }
  // ExecuteCommands has a 2000-byte stack-text limit. Large catalogs and target
  // lists are published in bounded batches instead of silently dropping the UI.
  std::string commands;
  for (const auto& update : updates) {
    if (commands.size() + update.size() + 1 > 1800) {
      autoexec::ExecuteCommands(ctx, base, commands);
      commands.clear();
    }
    if (!commands.empty()) commands += ';';
    commands += update;
  }
  if (!commands.empty()) autoexec::ExecuteCommands(ctx, base, commands);
}
void EveryFrame(PPCContext& ctx, uint8_t* base) {
  Publish(ctx, base);
  bool do_restart;
  {
    std::lock_guard lock(mutex);
    do_restart = restart;
    restart = false;
  }
  if (do_restart) {
    REXLOG_INFO("[scripts] applying saved script selection through a clean restart");
    launch::RestartCurrentMode();
    Rejected("Restart failed; restart the game to apply the saved selection");
  }
}
}  // namespace bo2zm::scripts
