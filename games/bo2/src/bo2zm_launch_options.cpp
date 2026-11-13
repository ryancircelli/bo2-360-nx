// Small, independent preference file shared by the launcher and campaign.
#include "bo2zm_launch_options.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__SWITCH__)
extern "C" int __system_argc;
extern "C" char** __system_argv;
#endif

namespace bo2zm::launch {
namespace {

constexpr const char* kFile = "launch_options.txt";

// Runtime-only state; early handover uses ReadDefaultModeIndexEarly instead.
std::filesystem::path& ConfiguredRoot() {
  static std::filesystem::path root;
  return root;
}

std::mutex& PreferenceMutex() {
  static std::mutex mutex;
  return mutex;
}

struct DefaultCache {
  std::filesystem::path root;
  std::optional<int> index;
};

DefaultCache& Cache() {
  static DefaultCache cache;
  return cache;
}

int ReadDefault(const std::filesystem::path& root) {
  if (root.empty()) {
    return 2;
  }
  FILE* file = std::fopen((root / kFile).string().c_str(), "rb");
  if (!file) {
    // Recover an interrupted replacement on filesystems without overwrite-rename.
    file = std::fopen((root / "launch_options.txt.bak").string().c_str(), "rb");
    if (!file) {
      return 2;
    }
  }
  // Accept only the complete record, so damaged or newer formats fail closed.
  char record[64]{};
  const size_t size = std::fread(record, 1, sizeof(record), file);
  const bool ok = !std::ferror(file);
  std::fclose(file);
  for (int index = 0; ok && index < 3; ++index) {
    const std::string expected = "default_mode=" + std::to_string(index) + "\n";
    if (size == expected.size() && std::string(record, size) == expected) {
      return index;
    }
  }
  return 2;
}

int CachedDefault(const std::filesystem::path& root) {
  auto& cache = Cache();
  if (!cache.index || cache.root != root) {
    cache.root = root;
    cache.index = ReadDefault(root);
  }
  return *cache.index;
}

}  // namespace

std::filesystem::path SharedUserRoot(const std::filesystem::path& runtime_user_dir) {
#if defined(__SWITCH__)
  if (__system_argc > 0 && __system_argv && __system_argv[0] && *__system_argv[0]) {
    return (std::filesystem::path(__system_argv[0]).parent_path() / "user").lexically_normal();
  }
  // Do not touch runtime-only static state when early startup has no argv.
  return runtime_user_dir.lexically_normal();
#else
  if (!runtime_user_dir.empty()) {
    const auto root = runtime_user_dir.lexically_normal();
    if (root.filename() == "bo2cp") {
      return root.parent_path() / "bo2zm";
    }
    return root;
  }
  return ConfiguredRoot();
#endif
}

int ReadDefaultModeIndexEarly(const std::filesystem::path& user_root) {
  return ReadDefault(user_root);
}

std::optional<Mode> ParseTestMode(std::string_view value) {
  if (value == "mp") return Mode::Multiplayer;
  if (value == "zm") return Mode::Zombies;
  return std::nullopt;
}

StartupTestOptions ReadStartupTestsEarly(const std::filesystem::path& toml) {
  StartupTestOptions result;
  FILE* file = std::fopen(toml.string().c_str(), "rb");
  if (!file) return result;
  const auto trim = [](std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return std::string_view{};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  };
  char line[512];
  bool first_line = true;
  while (std::fgets(line, sizeof(line), file)) {
    std::string_view text(line);
    // Discard a whole overlong line instead of treating its tail as a new key.
    if (!text.ends_with('\n') && !std::feof(file)) {
      int c;
      while ((c = std::fgetc(file)) != EOF && c != '\n') {}
      continue;
    }
    if (first_line && text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    first_line = false;
    text = trim(text.substr(0, text.find('#')));
    if (text.starts_with('[')) break;  // Runtime cvars are root-level keys.
    const auto equals = text.find('=');
    if (equals == text.npos) continue;
    const auto key = trim(text.substr(0, equals));
    const auto value = trim(text.substr(equals + 1));
    if (key == "bo2zm_test_campaign") result.campaign = value == "true";
    if (key == "bo2zm_test_mode") {
      result.mode.reset();
      if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
          value.back() == value.front()) {
        result.mode = ParseTestMode(value.substr(1, value.size() - 2));
      }
    }
  }
  if (std::ferror(file)) result = {};
  std::fclose(file);
  return result;
}

void Configure(const std::filesystem::path& runtime_user_dir) {
  std::lock_guard lock(PreferenceMutex());
  ConfiguredRoot() = SharedUserRoot(runtime_user_dir);
}

int GetDefaultModeIndex() {
  std::lock_guard lock(PreferenceMutex());
  return CachedDefault(SharedUserRoot());
}

int GetDefaultModeIndex(const std::filesystem::path& user_root) {
  std::lock_guard lock(PreferenceMutex());
  return CachedDefault(user_root);
}

bool SetDefaultModeIndex(int index) {
  return SetDefaultModeIndex(index, SharedUserRoot());
}

bool SetDefaultModeIndex(int index, const std::filesystem::path& user_root) {
  if (index < 0 || index > 2 || user_root.empty()) {
    return false;
  }
  std::lock_guard lock(PreferenceMutex());
  std::error_code ec;
  std::filesystem::create_directories(user_root, ec);
  if (ec) {
    return false;
  }
  const auto destination = user_root / kFile;
  const auto temporary = user_root / "launch_options.txt.tmp";
  FILE* file = std::fopen(temporary.string().c_str(), "wb");
  if (!file) {
    return false;
  }
  const bool written = std::fprintf(file, "default_mode=%d\n", index) > 0;
  const bool closed = std::fclose(file) == 0;
  bool replaced = false;
  if (written && closed) {
#if defined(_WIN32)
    replaced = MoveFileExW(temporary.c_str(), destination.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__SWITCH__)
    const std::string source = temporary.string();
    const std::string target = destination.string();
    const auto backup = user_root / "launch_options.txt.bak";
    replaced = std::rename(source.c_str(), target.c_str()) == 0;
    if (!replaced && std::filesystem::is_regular_file(destination, ec) && !ec) {
      // Horizon rename does not replace an existing destination. Keep the old
      // record recoverable until the closed temporary file has taken its place.
      std::filesystem::remove(backup, ec);
      if (!ec && std::rename(target.c_str(), backup.string().c_str()) == 0) {
        replaced = std::rename(source.c_str(), target.c_str()) == 0;
        if (!replaced) {
          std::rename(backup.string().c_str(), target.c_str());
        }
      }
    }
    if (replaced) {
      std::filesystem::remove(backup, ec);
    }
#else
    // Same-directory rename replaces the old record without first deleting it.
    replaced = std::rename(temporary.string().c_str(), destination.string().c_str()) == 0;
#endif
  }
  if (!replaced) {
    std::filesystem::remove(temporary, ec);
  } else {
    Cache().root = user_root;
    Cache().index = index;
  }
  return replaced;
}

bool MigrateLegacyDefault(bool boot_zombies, std::optional<Mode> test_mode,
                          const std::filesystem::path& user_root) {
  if (test_mode) return true;
  std::error_code ec;
  if (std::filesystem::exists(user_root / "launch_options.txt", ec) ||
      std::filesystem::exists(user_root / "launch_options.txt.bak", ec)) return true;
  return SetDefaultModeIndex(boot_zombies ? 2 : 1, user_root);
}

namespace {

bool FileExists(const std::filesystem::path& path) {
  FILE* file = std::fopen(path.string().c_str(), "rb");
  if (!file) {
    return false;
  }
  std::fclose(file);
  return true;
}

}  // namespace

bool InstalledModes::Has(Mode mode) const {
  switch (mode) {
    case Mode::Campaign:
      return campaign;
    case Mode::Multiplayer:
      return multiplayer;
    case Mode::Zombies:
      return zombies;
  }
  return false;
}

int InstalledModes::Count() const { return int(campaign) + int(multiplayer) + int(zombies); }

std::string InstalledModes::Text() const {
  const std::pair<bool, const char*> modes[] = {{campaign, "cp"}, {multiplayer, "mp"}, {zombies, "zm"}};
  std::string text;
  for (const auto& [on, name] : modes) {
    if (on) {
      if (!text.empty()) {
        text += ' ';
      }
      text += name;
    }
  }
  return text;
}

InstalledModes DetectInstalledModes(const std::filesystem::path& game_root, bool campaign_program,
                                    bool main_program) {
  const auto zones = [&](const char* suffix) {
    return FileExists(game_root / (std::string("code_post_gfx") + suffix + ".ff")) &&
           FileExists(game_root / (std::string("common") + suffix + ".ff"));
  };
  InstalledModes installed;
  if (!game_root.empty()) {
    const bool campaign = zones(""), multiplayer = zones("_mp"), zombies = zones("_zm");
    if (campaign || multiplayer || zombies) {
      installed.campaign = campaign;
      installed.multiplayer = multiplayer;
      installed.zombies = zombies;
    }
  }
  installed.campaign = installed.campaign && campaign_program;
  installed.multiplayer = installed.multiplayer && main_program;
  installed.zombies = installed.zombies && main_program;
  return installed;
}

Mode FirstInstalled(Mode wanted, const InstalledModes& installed) {
  if (installed.Has(wanted)) {
    return wanted;
  }
  for (const Mode mode : {Mode::Zombies, Mode::Multiplayer, Mode::Campaign}) {
    if (installed.Has(mode)) {
      return mode;
    }
  }
  return wanted;
}

Mode LaunchMode(std::string_view launch_path, const std::vector<uint8_t>& launch_data) {
  if (!launch_path.ends_with("default_mp.xex")) {
    return Mode::Campaign;
  }
  const bool zombies = launch_data.size() == 1000 && launch_data[3] == 0x0B && launch_data[860] == 1;
  return zombies ? Mode::Zombies : Mode::Multiplayer;
}

Mode StartupMode(Program program, std::optional<Mode> pending, Mode saved, bool test_campaign,
                 std::optional<Mode> test_mode) {
  if (pending) {
    return *pending;
  }
  if (program == Program::Campaign || test_campaign) {
    return Mode::Campaign;
  }
  if (test_mode == Mode::Multiplayer || test_mode == Mode::Zombies) return *test_mode;
  return saved;
}

}  // namespace bo2zm::launch
