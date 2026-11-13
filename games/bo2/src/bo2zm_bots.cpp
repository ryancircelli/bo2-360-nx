#include "bo2zm_bots.h"
#include "bo2zm_autoexec.h"
#include "bo2zm_launch_options.h"
#include "bo2zm_mode.h"
#include "bo2zm_settings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>
#include <rex/cvar.h>
#include <rex/ppc.h>
#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(bo2zm_test_bot_count, -1, "BO2",
    "Test helper: next-match Zombies bot count; -1 uses the current lobby choice. "
    "Human players always reserve their slots")
    .range(-1, bo2zm::bots::preferences::kTestMaximum).lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::bots {
namespace {
std::mutex mutex;
Context browsing;
bool available = false, dirty = true;
int count = 0, human_count = 1;
// Players in a System Link lobby this console hosts, as its menus count them (0: no such lobby since the last
// bo2zm_bots_open). Remote players have no local controller record, so HumanCount misses them.
int lobby_players = 0;
std::string availability = "Missing bot data or not checked yet";
std::string runtime_status, preference_status;

bool Zombies() {
#if defined(BO2_GAME_CAMPAIGN)
  return false;
#else
  return mode::IsZombies();
#endif
}
std::filesystem::path File() { return launch::SharedUserRoot() / "bo2zm_settings.txt"; }

std::string Clean(std::string_view text) {
  std::string result;
  for (const char c : text.substr(0, 160))
    result += c >= 32 && c < 127 && c != '"' && c != ';' && c != '\\' && c != '^' ? c : ' ';
  return result;
}

// Copy all unrelated settings, close the updated temporary file, then replace
// the destination. A failed replacement leaves the old selection recoverable.
bool CommitMigration(const std::vector<std::pair<std::string, std::string>>& updates) {
  const auto destination = File();
  auto temporary = destination;
  temporary += ".bots.tmp";
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) return false;
  const bool exists = std::filesystem::exists(destination, ec);
  if (ec) return false;
  std::string contents;
  std::vector<bool> found(updates.size());
  if (exists) {
    std::ifstream input(destination);
    if (!input) return false;
    std::string line;
    while (std::getline(input, line)) {
      const auto equals = line.find('=');
      bool updated = false;
      if (equals != std::string::npos) {
        const auto first = line.find_first_not_of(" \t\r");
        const auto last = std::string_view(line).substr(0, equals).find_last_not_of(" \t\r");
        if (first != std::string::npos && last != std::string::npos && first <= last) {
          const auto key = std::string_view(line).substr(first, last - first + 1);
          for (size_t i = 0; i < updates.size(); ++i) {
            if (key == updates[i].first) {
              contents += updates[i].first + " = " + updates[i].second + "\n";
              found[i] = updated = true;
              break;
            }
          }
        }
      }
      if (!updated) contents += line + '\n';
    }
    if (!input.eof()) return false;
  }
  for (size_t i = 0; i < updates.size(); ++i)
    if (!found[i]) contents += updates[i].first + " = " + updates[i].second + "\n";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(contents.data(), contents.size());
  output.close();
  if (!output) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  bool replaced;
#if defined(_WIN32)
  replaced = MoveFileExW(temporary.c_str(), destination.c_str(),
      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  replaced = std::rename(temporary.string().c_str(), destination.string().c_str()) == 0;
#if defined(__SWITCH__)
  if (!replaced && exists) {
    auto backup = destination;
    backup += ".bots.bak";
    // Never discard an existing recovery file.
    if (!std::filesystem::exists(backup, ec) && !ec &&
        std::rename(destination.string().c_str(), backup.string().c_str()) == 0) {
      replaced = std::rename(temporary.string().c_str(), destination.string().c_str()) == 0;
      if (replaced) std::filesystem::remove(backup, ec);
      else std::rename(backup.string().c_str(), destination.string().c_str());
    }
  }
#endif
#endif
  if (!replaced) std::filesystem::remove(temporary, ec);
  return replaced;
}

bool Parse(std::string_view argument, Context& context, std::string_view& value, bool setting) {
  if (argument.size() > 140) return false;
  std::array<std::string_view, 4> words;
  size_t size = 0;
  while (!argument.empty()) {
    const auto first = argument.find_first_not_of(" \t");
    if (first == std::string_view::npos) break;
    argument.remove_prefix(first);
    const auto end = argument.find_first_of(" \t");
    if (size == words.size()) return false;
    words[size++] = argument.substr(0, end);
    if (end == std::string_view::npos) break;
    argument.remove_prefix(end);
  }
  if (size != (setting ? 4u : 3u)) return false;
  context = {std::string(words[0]), std::string(words[1]), words[2] == "-" ? "" : std::string(words[2])};
  if (setting) value = words[3];
  return preferences::Key(context).has_value() && (!setting || preferences::Count(value).has_value());
}
}  // namespace

int HumanCount(const uint8_t* base) {
#if defined(BO2_GAME_CAMPAIGN)
  (void)base;
  return 1;
#else
  if (!base) return 4;
  const auto word = [base](uint32_t address) {
    uint32_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return __builtin_bswap32(value);
  };
  // 82403CF8/82403F58: four 20-byte local-controller manager records.
  unsigned seen = 0;
  int humans = 0;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    const uint32_t record = 0x837F6A28 + slot * 20;
    if (!(word(record) & 1)) continue;
    const uint32_t local = word(record + 4), controller = word(record + 8);
    if (local != slot || controller >= 4 || (seen & (1u << controller))) return 4;
    seen |= 1u << controller;
    ++humans;
  }
  return humans ? humans : 1;
#endif
}

int Humans(const uint8_t* base) {
  const int local = HumanCount(base);
  std::lock_guard lock(mutex);
  return std::max(local, lobby_players);
}

int Requested(const Context& context, int humans) {
  const auto key = preferences::Key(context);
  if (!Zombies() || !key) return 0;
  std::lock_guard lock(mutex);
  if (!available) return 0;
  const int override = REXCVAR_GET(bo2zm_test_bot_count);
  const bool same = browsing.map == context.map && browsing.gametype == context.gametype &&
                    browsing.start == context.start;
  const int requested = override >= 0 && override <= preferences::kTestMaximum ? override : same ? count : 0;
  return preferences::ClampToHumans(requested, humans == -1 ? human_count : humans);
}

void SetAvailability(bool enabled, std::string_view detail) {
  std::lock_guard lock(mutex);
  auto clean = Clean(detail);
  if (clean.empty()) clean = enabled ? "Bot data ready" : "Missing bot data";
  if (available == enabled && availability == clean) return;
  available = enabled;
  availability = std::move(clean);
  dirty = true;
}
void RuntimeStatus(std::string_view detail) {
  std::lock_guard lock(mutex);
  auto clean = Clean(detail);
  if (runtime_status == clean) return;
  runtime_status = std::move(clean);
  dirty = true;
}

bool Command(std::string_view name, std::string_view argument) {
  const bool opening = name == "bo2zm_bots_open";
  const bool setting = name == "bo2zm_bots_count";
  const bool players = name == "bo2zm_bots_humans";
  if (!opening && !setting && !players && name != "bo2zm_bots_context") return false;
  if (!Zombies()) return true;
  Context context;
  std::string_view value;
  std::lock_guard lock(mutex);
  if (players) {
    // "<1-4>": the System Link lobby's player count, local and remote.
    if (argument.size() == 1 && argument[0] >= '1' && argument[0] <= '4' && lobby_players != argument[0] - '0') {
      lobby_players = argument[0] - '0';
      dirty = true;
    }
    return true;
  }
  if (opening) {
    count = 0;
    lobby_players = 0;
    runtime_status.clear();
    preference_status.clear();
    dirty = true;
    return true;
  }
  if (!Parse(argument, context, value, setting)) {
    preference_status = "Invalid map context or bot count";
    dirty = true;
    return true;
  }
  const bool same = browsing.map == context.map && browsing.gametype == context.gametype &&
                    browsing.start == context.start;
  if (!setting && same) return true;
  if (setting && !available) {
    preference_status = "Bot files are not installed";
    dirty = true;
    return true;
  }
  browsing = context;
  preference_status.clear();
  count = setting ? preferences::ClampToHumans(*preferences::Count(value), human_count) : 0;
  dirty = true;
  return true;
}

void EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!Zombies()) return;
  std::string commands;
  {
    std::lock_guard lock(mutex);
    const int humans = std::max(HumanCount(base), lobby_players);
    const int clamped = preferences::ClampToHumans(count, humans);
    if (humans != human_count || clamped != count) dirty = true;
    human_count = humans;
    count = clamped;
    if (!dirty) return;
    dirty = false;
    std::string status = (available ? "" : "Unavailable: ") + availability;
    status += ". Next match: " + (count ? std::to_string(count) : std::string("OFF"));
    if (REXCVAR_GET(bo2zm_test_bot_count) >= 0) status += " (test override)";
    if (!preference_status.empty()) status += ". " + preference_status;
    if (!runtime_status.empty()) status += ". " + runtime_status;
    commands = "set bo2zm_bots_available_ui " + std::to_string(available ? 1 : 0) +
        ";set bo2zm_bots_count_ui " + std::to_string(available ? count : -1) +
        ";set bo2zm_bots_max_ui " + std::to_string(preferences::MaximumForHumans(human_count)) +
        ";set bo2zm_bots_status_ui \"" + status + "\"";
  }
  // The string is bounded below ExecuteCommands' 2000-byte limit. Execute only
  // on the UI/main thread, outside the mutex and the server's load callbacks.
  autoexec::ExecuteCommands(ctx, base, commands);
}

bool MigrateLegacySelection(std::string_view selected) {
  if (!Zombies() || selected.empty() || selected.size() > 128) return false;
  std::lock_guard lock(mutex);
  const auto saved = settings::GetAt(File(), "script_mod");
  if (saved && *saved != selected) return false;
  std::vector<std::pair<std::string, std::string>> updates = {{"script_mod", ""}};
  if (!CommitMigration(updates)) return false;
  count = 0;
  dirty = true;
  return true;
}
}  // namespace bo2zm::bots
