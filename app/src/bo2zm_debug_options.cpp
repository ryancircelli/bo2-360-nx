// The DEBUG page (Extras > DEBUG, app/src/ui/menus.lua): switches a player can be asked to turn on to help track a bug
// down, and the page's tools for logs and debug files.
//
// Renderer shortcuts. Each one skips work the full path would do: texture rechecks by write stamps, copying only the
// constants a shader reads, and the vertex and index caches. The renderer reads these switches every frame, at the
// same points where the benchmark's A/B arms toggle the same shortcuts (bo2zm_native_draws.cpp), so turning one off
// falls back to the path the "off" arms have always run. FULL texture checks also set bo2zm_native_hash_sampling to 0
// (every recheck hashes the whole texture), and SAFE RENDERER turns all of them off at once. If a bug goes away with
// one of them, that shortcut is the suspect.
//
// LOG DETAIL sets log_level, adding the debug lines. STACK PROFILE creates or deletes logs/rex/profile_stacks.flag,
// which the Switch profiler checks 8 s after start (bo2zm_profile_stacks = -1), so rex_profile.log records each
// thread's stacks from the next start on. MARK THE LOG writes a line a bug report can point to and takes a screenshot.
//
// DEBUG is the page's master switch. Off, everything above behaves as in normal play whatever is picked (the picks are
// kept and saved); on, the picks apply. Each pick is read only through On() here, and MARK LOCATION's pause-menu
// button only appears with DEBUG on (menus.lua), so the page lets every option be changed while DEBUG is off. The
// page's buttons (MARK THE LOG, TAKE SCREENSHOT, the info line, RESET DEBUG) act whenever they are pressed.
// DEBUG PRESET picks the options for a kind of bug, so a tester can simply be told "DEBUG on, preset CRASH": BASIC
// (nothing extra: mark the log and send it), VERBOSE (the detailed log), GRAPHICS (the detailed log and the safe
// renderer), PERFORMANCE (the stack profile), CRASH (the detailed log and the stack profile) and FULL (all three).
// CUSTOM works like the graphics preset's: it keeps the player's own picks (saved), changing an option starts it from
// the preset in use, and picking it gives those picks back.
//
// The quiet log. Someone who is just playing should get a log that stays small but can still report a crash. A toml
// or command line that sets log_level keeps that log, the Switch profiler's report (logs/rex/rex_profile.log) and
// every log file whatever DEBUG is; every test toml sets it and app/run_windows.sh passes it, so test runs (the
// benchmark, log uploads, per-thread CPU) are unaffected. Without it, as in a player's default toml, DEBUG off writes
// only warnings and errors and no profiler report (the profiler's thread registry, counters and the overlays' FPS keep
// running); DEBUG on brings the info log and the report back at once, with LOG DETAIL as usual. The level is decided
// before logging starts, from the DEBUG choice saved in bo2zm_settings.txt (ConfigureStartup, called from the app's
// OnConfigLoaded), so a quiet start writes none of the startup lines either.
//
// MAX LOG FILES (saved as max_log_files; bo2zm_max_log_files in the toml wins; independent of DEBUG). The SDK names a
// new bo2zm_NNN.log at every start and never deletes one: log_max_files only counts the size parts of a single log
// (bo2zm_NNN.<k>.log). Each start therefore keeps the newest 5, 10 or 20 numbers and deletes the older ones, or keeps
// them all (OFF, which test tomls use). A new pick applies from the next start. Only the SDK's own names are touched,
// and only when no log_file was given, so the PC's run_*.log files are never deleted. The default is 10 rather than 5:
// on the Switch every relaunch after an error is a new process with a new log (bo2zm_relaunch.cpp), so a few restarts
// in a row would already push out the log with the first error, while quiet logs are only about 60 KB each.
//
// PURGE DEBUG INFO. Its page picks a tier, and its DELETE button shows how many files and MB the tier holds (counted
// when the page opens and whenever the tier changes), then what it deleted. What the game and the runtime write, by
// tier (next to the NRO, or next to the exe on the PC, and in the user folder):
//   LOGS: the numbered game logs (bo2zm_NNN.log and their parts) except this run's, logs/marks.txt (MARK THE LOG,
//     MARK LOCATION), and logs/rex/*.log except the open rex_stderr.log (rex_stderr_previous.log, rex_crash.log,
//     rex_profile.log, rex_d3d.log, rex_d3d_timing.log): what a bug report sends.
//   SCREENSHOTS: captures/*.png (TAKE SCREENSHOT, MARK THE LOG, MARK LOCATION, test captures): the biggest files.
//   TRACES AND TEST FILES: d3d_shaders/ (bo2zm_d3d_trace_shaders), pgo/ (PGO training builds), logs/
//     bo2zm_prewarm_list.bin (a test helper's output), user/launch_capture_NNN.bin (every relaunch's launch data, kept
//     since the early bring-up; nothing reads them back) and pc_profile_*.txt (PC): only tests and tracing write these.
//   ALL: the three together.
// Never touched: the NRO, the toml, the shader library and its index, game/ and update/, the user folder's saves,
// the settings (bo2zm_settings.txt), relaunch_pending.bin (a relaunch's hand-over), cache/ (the pipeline cache and
// prewarm lists: deleting them would only force the pipelines to be built again), a development console's link
// settings and state, debug_overlay.txt (the overlay's position), logs/rex/profile_stacks.flag (STACK PROFILE's
// state), this run's log and the open rex_stderr.log.
//
// MARK LOCATION (saved, off by default, and only with DEBUG on): the pause menu in a match gets MARK LOCATION at the
// top (menus.lua). It runs bo2zm_mark_here and closes the pause menu: the log and logs/marks.txt get the map, the
// round, where the player stands and looks (bo2zm::cheats::PlayerSpot), the preset and the cores, with the
// bo2zm_test_spot line a test toml takes to stand there again; four screenshots follow, then "LOCATION N SAVED" at the
// top of the screen.

#include "bo2zm_debug_options.h"

#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "bo2zm_cheats.h"
#include "bo2zm_preset.h"
#include "bo2zm_settings.h"
#include "bo2zm_switch.h"
#include "native/render/bo2zm_native_capture.h"

#if defined(__SWITCH__)
extern "C" const char* RexSwitchLogDir(void);
extern "C" void RexSwitchPerfSetReport(bool on);
#endif

REXCVAR_DEFINE_STRING(bo2zm_max_log_files, "10", "BO2",
                      "Logs kept, one per start: \"5\", \"10\", \"20\" or \"off\" (every log; test tomls). Set here, it "
                      "wins over MAX LOG FILES on the DEBUG page")
    .allowed({"5", "10", "20", "off"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

std::atomic<int> g_textures{0};
std::atomic<int> g_constants{0};
std::atomic<int> g_caches{0};
std::atomic<int> g_safe{0};
std::atomic<int> g_log{0};
std::atomic<int> g_profile{0};   // the pick; the flag file exists when DEBUG is on and it is picked
std::atomic<int> g_enabled{0};

bool On() {
  return g_enabled.load(std::memory_order_relaxed) != 0;
}

// The quiet log (see the top of the file), as ConfigureStartup found it: whether the toml or the command line set
// log_level, and to what. Main thread.
bool g_level_given = false;
std::string g_given_level = "info";
std::string g_applied_level;  // the level set last
bool g_quiet_start = false;

// MAX LOG FILES (see the top of the file): the pick (an index of kMaxLogCounts; 0 = keep every log), and the count
// this start keeps.
constexpr const char* kMaxLogNames[] = {"5", "10", "20", "off"};
constexpr int kMaxLogCounts[] = {5, 10, 20, 0};
constexpr int kDefaultMaxLogs = 1;
std::atomic<int> g_max_logs{kDefaultMaxLogs};
int g_logs_kept = kMaxLogCounts[kDefaultMaxLogs];

// PURGE DEBUG INFO (see the top of the file): the tier on its page, and its DELETE button's label. Main thread.
std::atomic<int> g_purge_tier{0};
std::atomic<bool> g_purge_count{false};  // count the tier's files before the label is next read
std::string g_purge_label = "COUNTING...";

void SetLevel(const std::string& level) {
  if (level != g_applied_level) {
    g_applied_level = level;
    rex::cvar::SetFlagByName("log_level", level);
  }
}

// The profiler's report goes with the full log.
void ApplyReport() {
#if defined(__SWITCH__)
  RexSwitchPerfSetReport(g_level_given || On());
#endif
}

// A value saved in the settings file, read straight from it ("" if none): this runs before the runtime
// (bo2zm::settings) exists.
std::string SavedValue(const std::filesystem::path& file, std::string_view key) {
  const auto trim = [](const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    const size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
  };
  std::ifstream in(file);
  std::string line;
  std::string value;
  while (std::getline(in, line)) {
    const size_t equals = line.find('=');
    if (equals != std::string::npos && trim(line.substr(0, equals)) == key) {
      value = trim(line.substr(equals + 1));
    }
  }
  return value;
}

// The SDK's numbered logs: bo2zm_NNN.log and their size parts bo2zm_NNN.<k>.log (the SDK names them after the app,
// "bo2zm"). The number of such a file name, or -1.
int LogNumber(const std::string& name) {
  constexpr std::string_view kPrefix = "bo2zm_";
  const auto digits = [](std::string_view text) {
    return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
  };
  if (name.size() <= kPrefix.size() + 4 || name.compare(0, kPrefix.size(), kPrefix) != 0 ||
      name.compare(name.size() - 4, 4, ".log") != 0) {
    return -1;
  }
  const std::string_view rest = std::string_view(name).substr(kPrefix.size(), name.size() - kPrefix.size() - 4);
  const size_t dot = rest.find('.');
  if (!digits(rest.substr(0, dot)) || (dot != std::string_view::npos && !digits(rest.substr(dot + 1))) ||
      rest.substr(0, dot).size() > 6) {
    return -1;
  }
  return std::stoi(std::string(rest.substr(0, dot)));
}

// This run's log number, or -1 when its log is not a numbered one (a log_file was given, as on the PC).
int CurrentLogNumber() {
  if (!rex::cvar::GetFlagByName("log_file").empty()) {
    return -1;
  }
  return LogNumber(rex::CurrentLogFile().filename().string());
}

// The numbered logs in dir but the newest `keep` numbers go.
void PruneLogs(const std::filesystem::path& dir, int keep) {
  std::map<int, std::vector<std::filesystem::path>> by_number;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (const int number = LogNumber(entry.path().filename().string()); number >= 0 && entry.is_regular_file(ec)) {
      by_number[number].push_back(entry.path());
    }
  }
  int kept = 0;
  int removed = 0;
  for (auto it = by_number.rbegin(); it != by_number.rend(); ++it) {
    if (++kept <= keep) {
      continue;
    }
    for (const auto& path : it->second) {
      removed += std::filesystem::remove(path, ec) ? 1 : 0;
    }
  }
  if (removed) {
    REXLOG_INFO("[debug] MAX LOG FILES {}: {} older log files deleted", keep, removed);
  }
}

// The files of a PURGE DEBUG INFO tier (see the top of the file).
struct Found {
  std::vector<std::filesystem::path> files;
  uintmax_t bytes = 0;
};

void AddFile(Found& found, const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    found.bytes += std::filesystem::file_size(path, ec);
    found.files.push_back(path);
  }
}

// The files in dir (and below it with recurse) whose name passes keep.
template <typename Keep>
void AddFiles(Found& found, const std::filesystem::path& dir, bool recurse, Keep keep) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return;
  }
  const auto add = [&](const std::filesystem::directory_entry& entry) {
    if (entry.is_regular_file(ec) && keep(entry.path().filename().string())) {
      AddFile(found, entry.path());
    }
  };
  if (recurse) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
      add(entry);
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      add(entry);
    }
  }
}

bool EndsWith(const std::string& text, std::string_view end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

// 0 LOGS, 1 SCREENSHOTS, 2 TRACES AND TEST FILES, 3 ALL.
Found Collect(int tier) {
  Found found;
  const std::filesystem::path exe = rex::filesystem::GetExecutableFolder();
  if (exe.empty()) {
    return found;
  }
  std::vector<std::filesystem::path> log_dirs = {exe / "logs"};
  if (const auto current = rex::CurrentLogFile().parent_path(); !current.empty() && current != log_dirs[0]) {
    log_dirs.push_back(current);
  }
  const std::filesystem::path user = bo2zm::settings::File().parent_path();
  const bool all = tier == 3;
  if (all || tier == 0) {
    const int current = CurrentLogNumber();
    for (const auto& dir : log_dirs) {
      AddFiles(found, dir, false, [&](const std::string& name) {
        const int number = LogNumber(name);
        return (number >= 0 && number != current) || name == "marks.txt";
      });
    }
    AddFiles(found, exe / "logs" / "rex", false,
             [](const std::string& name) { return EndsWith(name, ".log") && name != "rex_stderr.log"; });
  }
  if (all || tier == 1) {
    AddFiles(found, (std::getenv("BO2_LOG_DIR") && *std::getenv("BO2_LOG_DIR")) ? std::filesystem::path(std::getenv("BO2_LOG_DIR")) / "captures" : exe / "captures", false, [](const std::string& name) { return EndsWith(name, ".png"); });
  }
  if (all || tier == 2) {
    AddFiles(found, exe / "d3d_shaders", true, [](const std::string&) { return true; });
    AddFiles(found, exe / "pgo", true, [](const std::string&) { return true; });
    for (const auto& dir : log_dirs) {
      AddFile(found, dir / "bo2zm_prewarm_list.bin");
    }
    if (!user.empty()) {
      AddFiles(found, user, false, [](const std::string& name) {
        return name.rfind("launch_capture_", 0) == 0 && EndsWith(name, ".bin");
      });
    }
    AddFiles(found, exe, false,
             [](const std::string& name) { return name.rfind("pc_profile_", 0) == 0 && EndsWith(name, ".txt"); });
  }
  return found;
}

std::string Amount(size_t files, uintmax_t bytes) {
  const std::string size = bytes >= 1024 * 1024 ? fmt::format("{:.1f} MB", double(bytes) / (1024.0 * 1024.0))
                                                : fmt::format("{} KB", (bytes + 1023) / 1024);
  return fmt::format("{} FILE{} ({})", files, files == 1 ? "" : "S", size);
}

// The options, in this order: detailed log, stack profile, texture checks, shader constants, geometry caches off,
// safe renderer. Each preset's picks:
constexpr int kNumPicks = 6;
using Picks = std::array<int, kNumPicks>;
constexpr Picks kPresets[] = {
    {0, 0, 0, 0, 0, 0},  // 1 BASIC
    {1, 0, 0, 0, 0, 0},  // 2 VERBOSE
    {1, 0, 0, 0, 0, 1},  // 3 GRAPHICS
    {0, 1, 0, 0, 0, 0},  // 4 PERFORMANCE
    {1, 1, 0, 0, 0, 0},  // 5 CRASH
    {1, 1, 0, 0, 0, 1},  // 6 FULL
};
constexpr int kNumPresets = int(std::size(kPresets));
// CUSTOM's picks in the settings file: key and each value's name (the keys the page has always saved under).
struct SavedPick {
  const char* key;
  const char* names[2];
};
constexpr SavedPick kSaved[kNumPicks] = {
    {"debug_log_detail", {"normal", "detailed"}},
    {"debug_stack_profile", {"off", "on"}},
    {"debug_texture_checks", {"fast", "full"}},
    {"debug_shader_constants", {"fast", "full"}},
    {"debug_geometry_caches", {"on", "off"}},
    {"debug_safe_renderer", {"off", "on"}},
};
std::atomic<int> g_preset{1};  // BASIC
Picks g_custom{};              // CUSTOM's picks (main thread)
std::string g_hash_sampling;  // bo2zm_native_hash_sampling before the texture checks went full
bool g_hash_full = false;
int g_marks = 0;
std::atomic<int> g_mark_location{0};  // MARK LOCATION (saved): the pause menu's button
int g_locations = 0;                  // locations marked in this run (main thread)

// bo2zm_native_hash_sampling follows TextureShortcutsOff (main thread).
void ApplyHashSampling() {
  const bool full = bo2zm::debug::TextureShortcutsOff();
  if (full == g_hash_full) {
    return;
  }
  g_hash_full = full;
  if (full) {
    g_hash_sampling = rex::cvar::GetFlagByName("bo2zm_native_hash_sampling");
    rex::cvar::SetFlagByName("bo2zm_native_hash_sampling", "0");
  } else {
    rex::cvar::SetFlagByName("bo2zm_native_hash_sampling", g_hash_sampling.empty() ? "8" : g_hash_sampling);
  }
}

std::filesystem::path ProfileFlag() {
#if defined(__SWITCH__)
  return std::filesystem::path(RexSwitchLogDir()) / "profile_stacks.flag";
#else
  return {};
#endif
}

// The flag file follows the pick and the switch.
void ApplyProfileFlag() {
  const auto flag = ProfileFlag();
  if (flag.empty()) {
    return;
  }
  const bool want = On() && g_profile.load(std::memory_order_relaxed);
  std::error_code ec;
  if (want == std::filesystem::exists(flag, ec)) {
    return;
  }
  if (want) {
    std::filesystem::create_directories(flag.parent_path(), ec);
    if (std::FILE* f = std::fopen(flag.string().c_str(), "wb")) {
      std::fclose(f);
    }
  } else {
    std::filesystem::remove(flag, ec);
  }
  REXLOG_INFO("[debug] stack profile {} from the next start ({})", want ? "on" : "off", flag.string());
}

// DEBUG on: the full log (LOG DETAIL DETAILED: debug lines too). Off: the toml's level when it set one, else warnings
// and errors only (see the top of the file). Only a change is applied: DEBUG off at start leaves the start's level.
void ApplyLog() {
  const std::string full = g_level_given ? g_given_level : "info";
  if (On()) {
    SetLevel(g_log.load(std::memory_order_relaxed) ? "debug" : full);
  } else {
    SetLevel(g_level_given ? g_given_level : "warn");
  }
  ApplyReport();
}

std::atomic<int>* const kPickAtomics[kNumPicks] = {&g_log, &g_profile, &g_textures, &g_constants, &g_caches, &g_safe};

Picks Current() {
  Picks picks{};
  for (int i = 0; i < kNumPicks; ++i) {
    picks[i] = kPickAtomics[i]->load(std::memory_order_relaxed);
  }
  return picks;
}

void ApplyPicks(const Picks& picks) {
  for (int i = 0; i < kNumPicks; ++i) {
    kPickAtomics[i]->store(picks[i] ? 1 : 0, std::memory_order_relaxed);
  }
  bo2zm::debug::Sync();
}

void SaveCustom() {
  for (int i = 0; i < kNumPicks; ++i) {
    bo2zm::settings::Set(kSaved[i].key, kSaved[i].names[g_custom[i] ? 1 : 0]);
  }
}

// An option changed from the page: CUSTOM, starting from the preset in use.
void SetPick(int index, int value) {
  if (g_preset.load(std::memory_order_relaxed) != 0) {
    g_custom = Current();
    g_preset.store(0, std::memory_order_relaxed);
  }
  g_custom[index] = value ? 1 : 0;
  SaveCustom();
  ApplyPicks(g_custom);
}

std::string Stamp() {
  const std::time_t now = std::time(nullptr);
  char text[32];
  std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", std::localtime(&now));
  return text;
}

// MARK LOCATION (the pause menu's button; see the top of the file). The menu closes the pause menu right after the
// command, so the four screenshots, 0.7 to 1.6 s later, show the scene without it, 0.3 s apart to catch what blinks.
// The banner comes after them, so it is in none.
void SaveLocation() {
  bo2zm::cheats::Spot spot;
  if (!bo2zm::cheats::PlayerSpot(spot)) {
    REXLOG_INFO("[mark] MARK LOCATION: not in a match");
    return;
  }
  ++g_locations;
  const std::string label = fmt::format("location_{:02}_{}", g_locations, Stamp());
  const std::string map = bo2zm::preset::LoadedMap();
  const std::string location = bo2zm::preset::LoadedLocation();
  const std::string text = fmt::format(
      "LOCATION {}: {} ({}), round {}, feet {:.1f} {:.1f} {:.1f}, camera {:.1f} {:.1f} {:.1f}, looking pitch {:.1f} "
      "yaw {:.1f} roll {:.1f}; preset {}, {} cores, debug {}, build {}, log {}; screenshots captures/{}_1.png to _4.png",
      g_locations, location.empty() ? "?" : location, map, bo2zm::cheats::CurrentRound(), spot.feet[0], spot.feet[1],
      spot.feet[2], spot.camera[0], spot.camera[1], spot.camera[2], spot.angles[0], spot.angles[1], spot.angles[2],
      bo2zm::preset::PresetLabel(), bo2zm::CoresInUse(), On() ? "on" : "off", bo2zm::BuildId(),
      rex::CurrentLogFile().filename().string(), label);
  // The line a test toml takes to stand there again: the map, the feet, the view's pitch and yaw.
  const std::string again = fmt::format("bo2zm_test_spot = \"{} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}\"",
                                        map.substr(0, map.find(' ')), spot.feet[0], spot.feet[1], spot.feet[2],
                                        spot.angles[0], spot.angles[1]);
  REXLOG_INFO("[mark] ===== {} =====", text);
  REXLOG_INFO("[mark] {}", again);
  const std::filesystem::path marks = rex::CurrentLogFile().parent_path() / "marks.txt";
  std::ofstream file(marks, std::ios::app);
  file << Stamp() << " " << text << "\n    " << again << "\n";
  if (!file) {
    REXLOG_WARN("[mark] could not add the location to {}", marks.string());
  }
  for (int i = 0; i < 4; ++i) {
    bo2zm::capture::RequestCapture(label + "_" + std::to_string(i + 1), std::chrono::milliseconds(700 + 300 * i));
  }
  bo2zm::preset::ShowBanner(fmt::format("LOCATION {} SAVED", g_locations), 2.2, 2.5);
}

}  // namespace

bool bo2zm::debug::TextureShortcutsOff() {
  return On() && (g_textures.load(std::memory_order_relaxed) || g_safe.load(std::memory_order_relaxed));
}
bool bo2zm::debug::ConstantsShortcutOff() {
  return On() && (g_constants.load(std::memory_order_relaxed) || g_safe.load(std::memory_order_relaxed));
}
bool bo2zm::debug::CachesOff() {
  return On() && (g_caches.load(std::memory_order_relaxed) || g_safe.load(std::memory_order_relaxed));
}

void bo2zm::debug::ConfigureStartup(const std::filesystem::path& user_data_root) {
  const std::filesystem::path settings = user_data_root / "bo2zm_settings.txt";
  g_level_given = rex::cvar::GetFlagSource("log_level") != rex::cvar::Source::kDefault;
  g_given_level = rex::cvar::GetFlagByName("log_level");
  const bool debug_on = SavedValue(settings, "debug") == "on";
  g_quiet_start = !g_level_given && !debug_on;
  // MAX LOG FILES: the toml's, else the saved pick, else 10.
  const std::string max_logs = rex::cvar::GetFlagSource("bo2zm_max_log_files") != rex::cvar::Source::kDefault
                                   ? std::string(REXCVAR_GET(bo2zm_max_log_files))
                                   : SavedValue(settings, "max_log_files");
  for (int i = 0; i < int(std::size(kMaxLogNames)); ++i) {
    if (max_logs == kMaxLogNames[i]) {
      g_max_logs.store(i, std::memory_order_relaxed);
    }
  }
  g_logs_kept = kMaxLogCounts[g_max_logs.load(std::memory_order_relaxed)];
  if (g_level_given) {
    g_applied_level = g_given_level;
  } else {
    SetLevel(debug_on ? "info" : "warn");
  }
#if defined(__SWITCH__)
  // log_level = "off" in the toml: no profiler report either.
  RexSwitchPerfSetReport(!g_quiet_start && !(g_level_given && g_given_level == "off"));
#endif
}

void bo2zm::debug::AfterLoggingStarted() {
  if (g_quiet_start) {
    // A warning so it is in the file: why the log is short, and which build wrote it.
    REXLOG_WARN("[debug] quiet log: DEBUG is off, so only warnings and errors are written (build {}). Extras > DEBUG "
                "on gives the full log.", bo2zm::BuildId());
  }
  if (g_logs_kept > 0 && CurrentLogNumber() >= 0) {
    PruneLogs(rex::CurrentLogFile().parent_path(), g_logs_kept);
  }
}

int bo2zm::debug::MaxLogFiles() {
  return g_max_logs.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetMaxLogFiles(int index) {
  if (index >= 0 && index < int(std::size(kMaxLogCounts))) {
    g_max_logs.store(index, std::memory_order_relaxed);  // saved by bo2zm_extras.cpp; from the next start
  }
}

int bo2zm::debug::PurgeTier() {
  return g_purge_tier.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetPurgeTier(int tier) {
  if (tier >= 0 && tier <= 3) {
    g_purge_tier.store(tier, std::memory_order_relaxed);
    g_purge_count.store(true, std::memory_order_relaxed);
  }
}
std::string bo2zm::debug::PurgeLabel() {
  if (g_purge_count.exchange(false, std::memory_order_relaxed)) {
    const Found found = Collect(g_purge_tier.load(std::memory_order_relaxed));
    g_purge_label = found.files.empty() ? "NOTHING TO DELETE" : "DELETE " + Amount(found.files.size(), found.bytes);
  }
  return g_purge_label;
}

int bo2zm::debug::Enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetEnabled(int on) {
  g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
  // The line goes to the full log either way: a quiet log takes info lines only once DEBUG on has applied, and until
  // DEBUG off does.
  if (on) {
    Sync();
    REXLOG_INFO("[debug] DEBUG on: the debug picks apply");
  } else {
    REXLOG_INFO("[debug] DEBUG off: normal play");
    Sync();
  }
}
void bo2zm::debug::Sync() {
  ApplyHashSampling();
  ApplyLog();
  ApplyProfileFlag();
}

int bo2zm::debug::Preset() {
  return g_preset.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetPreset(int preset) {
  if (preset < 0 || preset > kNumPresets) {
    return;
  }
  g_preset.store(preset, std::memory_order_relaxed);
  ApplyPicks(preset == 0 ? g_custom : kPresets[preset - 1]);
}
void bo2zm::debug::LoadCustom() {
  for (int i = 0; i < kNumPicks; ++i) {
    if (const auto saved = bo2zm::settings::Get(kSaved[i].key)) {
      g_custom[i] = *saved == kSaved[i].names[1];
    }
  }
}

int bo2zm::debug::TextureChecks() {
  return g_textures.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetTextureChecks(int full) {
  SetPick(2, full);
}
int bo2zm::debug::Constants() {
  return g_constants.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetConstants(int full) {
  SetPick(3, full);
}
int bo2zm::debug::Caches() {
  return g_caches.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetCaches(int off) {
  SetPick(4, off);
}
int bo2zm::debug::SafeRenderer() {
  return g_safe.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetSafeRenderer(int on) {
  SetPick(5, on);
}
int bo2zm::debug::LogDetail() {
  return g_log.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetLogDetail(int detailed) {
  SetPick(0, detailed);
}
int bo2zm::debug::StackProfile() {
  return g_profile.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetStackProfile(int on) {
  SetPick(1, on);
}
int bo2zm::debug::MarkLocation() {
  return g_mark_location.load(std::memory_order_relaxed);
}
void bo2zm::debug::SetMarkLocation(int on) {
  g_mark_location.store(on ? 1 : 0, std::memory_order_relaxed);
}

bool bo2zm::debug::RunCommand(std::string_view name, std::string_view argument) {
  if (name == "bo2zm_mark") {
    ++g_marks;
    const std::string label = "mark_" + Stamp();
    REXLOG_INFO("[debug] ===== MARK {}: the player marked this moment (round {}); screenshot captures/{}.png =====",
                g_marks, bo2zm::cheats::CurrentRound(), label);
    bo2zm::capture::RequestCapture(label);
    return true;
  }
  if (name == "bo2zm_mark_here") {
    SaveLocation();
    return true;
  }
  if (name == "bo2zm_screenshot") {
    const std::string label = "shot_" + Stamp();
    REXLOG_INFO("[debug] screenshot captures/{}.png", label);
    bo2zm::capture::RequestCapture(label);
    return true;
  }
  if (name == "bo2zm_purge_scan") {  // PURGE DEBUG INFO's page opened
    g_purge_count.store(true, std::memory_order_relaxed);
    return true;
  }
  if (name == "bo2zm_purge") {  // its DELETE button
    const int tier = g_purge_tier.load(std::memory_order_relaxed);
    const Found found = Collect(tier);
    size_t deleted = 0;
    uintmax_t bytes = 0;
    std::error_code ec;
    for (const auto& path : found.files) {
      const uintmax_t size = std::filesystem::file_size(path, ec);
      if (std::filesystem::remove(path, ec)) {
        ++deleted;
        bytes += size;
      }
    }
    g_purge_label = found.files.empty() ? "NOTHING TO DELETE" : "DELETED " + Amount(deleted, bytes);
    if (deleted < found.files.size()) {
      g_purge_label += fmt::format(" ({} COULD NOT GO)", found.files.size() - deleted);
    }
    REXLOG_WARN("[debug] PURGE DEBUG INFO, tier {}: {} of {} files deleted ({} bytes)", tier, deleted,
                found.files.size(), bytes);
    return true;
  }
  if (name == "bo2zm_debug_reset") {
    SetPreset(1);
    REXLOG_INFO("[debug] every debug option back to normal (preset BASIC; CUSTOM keeps its picks)");
    return true;
  }
  (void)argument;
  return false;
}

std::string bo2zm::debug::BuildText() {
  return bo2zm::BuildId();
}

std::string bo2zm::debug::LogFileName() {
  return rex::CurrentLogFile().filename().string();
}
