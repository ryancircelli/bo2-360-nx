// Standalone preference and launch-policy tests; no SDK, guest image or game process.
// c++ -std=c++20 -pthread tools/tests/launch_options_test.cpp games/bo2/src/bo2zm_launch_options.cpp -o launch_options_test
#include "../../games/bo2/src/bo2zm_launch_options.h"

#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bo2zm::launch;

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

void Write(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file.write(contents.data(), contents.size());
  file.close();
  Check(!file.fail(), "fixture write failed");
}

struct TemporaryDirectory {
  fs::path root;
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("bo2-launch-options-" + std::to_string(stamp));
    Check(fs::create_directory(root), "temporary directory already exists");
  }
  ~TemporaryDirectory() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void PreferenceTests(const fs::path& temporary) {
  const auto root = temporary / "profile";
  const auto file = root / "launch_options.txt";
  Check(ReadDefaultModeIndexEarly(root) == 2, "early fresh profile must use Zombies");
  Check(GetDefaultModeIndex(root) == 2, "fresh profile must use Zombies");
  Check(!fs::exists(root), "reading defaults must not create a profile");
  for (int index = 0; index < 3; ++index) {
    Check(SetDefaultModeIndex(index, root), "mode could not be saved");
    Check(GetDefaultModeIndex(root) == index, "saved mode mismatch");
    Check(ReadDefaultModeIndexEarly(root) == index, "early reader cannot see the persisted mode");
    Check(Read(file) == "default_mode=" + std::to_string(index) + "\n", "saved record mismatch");
  }
  Check(SetDefaultModeIndex(0, root), "setup save failed");
  const auto saved = Read(file);
  for (int index : {-1, 3, INT_MAX, INT_MIN}) {
    Check(!SetDefaultModeIndex(index, root), "invalid mode accepted");
    Check(GetDefaultModeIndex(root) == 0 && Read(file) == saved, "invalid input changed preference");
  }
  Check(!SetDefaultModeIndex(1, {}), "empty root accepted");
  fs::create_directory(root / "launch_options.txt.tmp");
  Check(!SetDefaultModeIndex(1, root), "blocked temporary file accepted");
  Check(GetDefaultModeIndex(root) == 0 && Read(file) == saved, "failed save changed preference");
  fs::remove(root / "launch_options.txt.tmp");

  const auto blocked = temporary / "regular-file";
  Write(blocked, "not a directory");
  Check(!SetDefaultModeIndex(1, blocked), "non-directory root accepted");
  Check(Read(blocked) == "not a directory", "failed save damaged the root");

  const auto replacement = temporary / "replacement";
  Check(SetDefaultModeIndex(0, replacement), "replacement setup failed");
  fs::remove(replacement / "launch_options.txt");
  fs::create_directory(replacement / "launch_options.txt");
  Check(!SetDefaultModeIndex(1, replacement), "directory accepted as preference destination");
  Check(GetDefaultModeIndex(replacement) == 0, "failed replacement updated the cache");

  const auto recovered = temporary / "recovered";
  Write(recovered / "launch_options.txt.bak", "default_mode=1\n");
  Check(ReadDefaultModeIndexEarly(recovered) == 1, "interrupted replacement was not recovered early");
  Check(GetDefaultModeIndex(recovered) == 1, "interrupted replacement was not recovered at runtime");
  Check(SetDefaultModeIndex(0, recovered), "recovered preference could not be replaced");
  Check(ReadDefaultModeIndexEarly(recovered) == 0, "new preference lost to old recovery record");

  const std::string malformed[] = {
      "", "default_mode=-1\n", "default_mode=3\n", "default_mode=1junk\n",
      "default_mode=1\ndefault_mode=0\n", "other=0\n", std::string(1000, 'x'),
      std::string("default_mode=1\0\n", 16)};
  for (size_t i = 0; i < std::size(malformed); ++i) {
    const auto invalid = temporary / ("invalid-" + std::to_string(i));
    Write(invalid / "launch_options.txt", malformed[i]);
    Check(GetDefaultModeIndex(invalid) == 2, "malformed preference did not fall back to Zombies");
  }
}

void CacheAndSharingTests(const fs::path& temporary) {
  const auto main_root = temporary / "bo2zm";
  const auto campaign_root = temporary / "bo2cp";
  Check(SharedUserRoot(campaign_root) == main_root, "default CP root is not shared with main");
  Check(SharedUserRoot(main_root) == main_root, "main root changed");
  Check(SetDefaultModeIndex(1, main_root), "sharing setup failed");
  Configure(campaign_root);
  Check(GetDefaultModeIndex() == 1, "CP did not read main's saved preference");
  Check(SetDefaultModeIndex(0), "CP could not save a shared preference");
  Configure(main_root);
  Check(GetDefaultModeIndex() == 0, "main did not see CP's saved preference");

  // Removing the file makes any accidental poll-time reread return Zombies.
  fs::remove(main_root / "launch_options.txt");
  for (int poll = 0; poll < 100; ++poll) {
    Check(GetDefaultModeIndex() == 0, "menu polling reread the preference file");
  }
  Configure(main_root);
  Check(GetDefaultModeIndex() == 0, "configuration discarded the runtime cache");
  Check(SetDefaultModeIndex(2), "cached preference could not be updated");
  Check(GetDefaultModeIndex() == 2, "successful save did not update the cache");

  const auto custom = temporary / "custom-profile";
  Check(SharedUserRoot(custom) == custom, "custom PC profile root changed");
  Configure(custom);
  Check(GetDefaultModeIndex() == 2, "new profile inherited another root's cache");
}

void StartupTests() {
  constexpr Mode modes[] = {Mode::Campaign, Mode::Multiplayer, Mode::Zombies};
  constexpr Program programs[] = {Program::Main, Program::Campaign};
  const std::optional<Mode> tests[] = {std::nullopt, Mode::Multiplayer, Mode::Zombies};
  for (auto saved : modes) {
    Check(StartupMode(Program::Main, std::nullopt, saved) == saved, "fresh launcher ignored default");
    Check(StartupMode(Program::Main, std::nullopt, saved, true) == Mode::Campaign,
          "campaign test loop ignored on a fresh launcher start");
    Check(StartupMode(Program::Campaign, std::nullopt, saved) == Mode::Campaign,
          "direct CP start chained back to the saved default");
    for (auto pending : modes) {
      for (auto program : programs) {
        for (bool test_campaign : {false, true}) {
          for (auto test : tests) {
            Check(StartupMode(program, pending, saved, test_campaign, test) == pending,
                  "explicit pending mode lost to preferences or test override");
          }
        }
      }
      // A script restart carries the current mode as explicit pending data.
      Check(StartupMode(Program::Main, pending, saved, true) == pending,
            "script restart diverted to campaign test loop");
    }
  }
  // CP -> ZM/MP -> CP handovers retain the explicit mode until the receiver consumes it.
  Check(StartupMode(Program::Campaign, Mode::Zombies, Mode::Campaign, true) == Mode::Zombies,
        "CP-to-ZM handover changed mode");
  Check(StartupMode(Program::Main, Mode::Zombies, Mode::Campaign, true) == Mode::Zombies,
        "CP-to-ZM handover looped back to CP");
  Check(StartupMode(Program::Campaign, Mode::Multiplayer, Mode::Campaign, true) == Mode::Multiplayer,
        "CP-to-MP handover changed mode");
  Check(StartupMode(Program::Main, Mode::Multiplayer, Mode::Campaign, true) == Mode::Multiplayer,
        "CP-to-MP handover looped back to CP");
  Check(StartupMode(Program::Main, Mode::Campaign, Mode::Zombies) == Mode::Campaign,
        "ZM-to-CP handover ignored explicit mode");
  Check(StartupMode(Program::Campaign, Mode::Campaign, Mode::Zombies) == Mode::Campaign,
        "ZM-to-CP handover looped back to ZM");
  for (auto saved : modes) {
    for (auto test : tests) {
      Check(StartupMode(Program::Main, std::nullopt, saved, false, test) == test.value_or(saved),
            "transient mode did not override the saved default");
      Check(StartupMode(Program::Main, std::nullopt, saved, true, test) == Mode::Campaign,
            "transient mode diverted the campaign test loop");
      Check(StartupMode(Program::Campaign, std::nullopt, saved, false, test) == Mode::Campaign,
            "transient mode redirected a direct CP start");
    }
    Check(StartupMode(Program::Main, std::nullopt, saved, false, Mode::Campaign) == saved,
          "MP/ZM test override accepted Campaign");
  }
}

void TransientStartupTests(const fs::path& temporary) {
  const auto root = temporary / "transient";
  const auto config = root / "bo2zm.toml";
  const auto user = root / "user";
  const auto preference = user / "launch_options.txt";
  Check(!ReadStartupTestsEarly(config).mode, "missing test config enabled an override");
  Check(!fs::exists(root), "early config read created files");
  for (const auto value : {"", "campaign", "MP", "m p", " mp", "zm "}) {
    Check(!ParseTestMode(value), "unsupported test-mode value accepted");
  }
  for (auto test : {Mode::Multiplayer, Mode::Zombies}) {
    const std::string value = test == Mode::Multiplayer ? "mp" : "zm";
    Write(config, "\xEF\xBB\xBF" "# Test configuration\r\nbo2zm_test_mode = \"" + value +
                  "\" # temporary\r\nbo2zm_test_campaign = false\r\n");
    const auto early = ReadStartupTestsEarly(config);
    const auto runtime = ParseTestMode(value);
    Check(early.mode == runtime && !early.campaign, "early/runtime test options disagree");
    // This is the migration helper used by ApplyPendingLaunchData, not a mock.
    Check(MigrateLegacyDefault(false, runtime, user), "transient migration failed");
    Check(!fs::exists(user), "transient startup created a saved default");
    Check(StartupMode(Program::Main, std::nullopt,
                      static_cast<Mode>(ReadDefaultModeIndexEarly(user)), early.campaign, early.mode) == test,
          "early startup ignored the transient mode");
    Check(StartupMode(Program::Main, std::nullopt,
                      static_cast<Mode>(GetDefaultModeIndex(user)), false, runtime) == test,
          "runtime startup ignored the transient mode");
  }
  for (int saved = 0; saved < 3; ++saved) {
    Check(SetDefaultModeIndex(saved, user), "saved-default fixture setup failed");
    const auto before = Read(preference);
    for (auto test : {Mode::Multiplayer, Mode::Zombies}) {
      Check(MigrateLegacyDefault(true, test, user), "saved transient startup failed");
      Check(StartupMode(Program::Main, std::nullopt, static_cast<Mode>(GetDefaultModeIndex(user)),
                        false, test) == test, "saved mode blocked the runtime override");
      Check(Read(preference) == before, "test overwrote the saved default");
      Check(ReadDefaultModeIndexEarly(user) == saved, "test changed the early saved-default read");
      Check(GetDefaultModeIndex(user) == saved, "test changed the saved-default cache");
    }
  }
  // Clearing the override restores the saved mode; ordinary legacy migration survives.
  Write(config, "bo2zm_test_mode = \"\"\n");
  const auto empty = ReadStartupTestsEarly(config);
  Check(StartupMode(Program::Main, std::nullopt, Mode::Campaign, empty.campaign, empty.mode) == Mode::Campaign,
        "empty test mode did not restore the saved default");
  const auto fresh = root / "legacy";
  Check(MigrateLegacyDefault(false, std::nullopt, fresh), "normal legacy migration failed");
  Check(Read(fresh / "launch_options.txt") == "default_mode=1\n", "normal legacy mode changed");
  Check(MigrateLegacyDefault(true, std::nullopt, fresh), "existing preference migration failed");
  Check(Read(fresh / "launch_options.txt") == "default_mode=1\n", "migration replaced an existing preference");

  Write(config, "bo2zm_test_mode='mp'\nbo2zm_test_campaign=true\n");
  const auto campaign = ReadStartupTestsEarly(config);
  Check(campaign.mode == Mode::Multiplayer && campaign.campaign, "early Campaign test not read");
  Check(StartupMode(Program::Main, std::nullopt, Mode::Zombies, campaign.campaign, campaign.mode) == Mode::Campaign,
        "early MP override diverted the Campaign test");
  for (const auto line : {"bo2zm_test_mode=\"campaign\"", "bo2zm_test_mode=\"m p\"",
                          "# bo2zm_test_mode=\"mp\"", "[other]\nbo2zm_test_mode=\"mp\""}) {
    Write(config, line);
    Check(!ReadStartupTestsEarly(config).mode, "early reader enabled an unsupported test override");
  }
  Write(config, "#" + std::string(510, ' ') + "bo2zm_test_mode=\"mp\"\n");
  Check(!ReadStartupTestsEarly(config).mode, "overlong comment tail became a startup setting");
}

void InstalledModeTests(const fs::path& temporary) {
  const auto game = temporary / "game";
  const auto install = [&](std::initializer_list<const char*> zones) {
    fs::remove_all(game);
    fs::create_directories(game);
    for (const char* zone : zones) {
      Write(game / zone, "zone");
    }
  };
  // No recognisable game folder: every mode counts, Campaign only with its program.
  auto modes = DetectInstalledModes(temporary / "missing", true);
  Check(modes.campaign && modes.multiplayer && modes.zombies && modes.Text() == "cp mp zm", "missing folder");
  modes = DetectInstalledModes({}, false);
  Check(!modes.campaign && modes.multiplayer && modes.zombies, "empty root or missing campaign program");
  install({"common.ff", "code_post_gfx.ff", "common_mp.ff", "code_post_gfx_mp.ff", "common_zm.ff",
           "code_post_gfx_zm.ff"});
  modes = DetectInstalledModes(game, true);
  Check(modes.Count() == 3, "full install");
  Check(!DetectInstalledModes(game, false).campaign, "Campaign without its program");
  modes = DetectInstalledModes(game, true, false);
  Check(modes.campaign && !modes.multiplayer && !modes.zombies, "Zombies and Multiplayer without bo2zm");
  install({"common_zm.ff", "code_post_gfx_zm.ff", "patch_ui_zm.ff", "patch.ff"});
  modes = DetectInstalledModes(game, true);
  Check(modes.Text() == "zm" && modes.Count() == 1, "Zombies only");
  Check(FirstInstalled(Mode::Campaign, modes) == Mode::Zombies, "Campaign default without Campaign");
  Check(FirstInstalled(Mode::Multiplayer, modes) == Mode::Zombies, "Multiplayer default without Multiplayer");
  install({"common_mp.ff", "code_post_gfx_mp.ff", "common.ff"});  // half a campaign: not installed
  modes = DetectInstalledModes(game, true);
  Check(modes.Text() == "mp", "Multiplayer only");
  Check(FirstInstalled(Mode::Zombies, modes) == Mode::Multiplayer, "Zombies default without Zombies");
  install({"common.ff", "code_post_gfx.ff"});
  modes = DetectInstalledModes(game, true);
  Check(modes.Text() == "cp", "Campaign only");
  Check(FirstInstalled(Mode::Zombies, modes) == Mode::Campaign, "Zombies default with Campaign only");
  Check(FirstInstalled(Mode::Campaign, modes) == Mode::Campaign, "installed default changed");
  modes = DetectInstalledModes(game, false);
  Check(modes.Count() == 0 && FirstInstalled(Mode::Multiplayer, modes) == Mode::Multiplayer, "nothing installed");
  install({"common.ff", "code_post_gfx.ff", "common_mp.ff", "code_post_gfx_mp.ff"});
  modes = DetectInstalledModes(game, true);
  Check(FirstInstalled(Mode::Zombies, modes) == Mode::Multiplayer && FirstInstalled(Mode::Campaign, modes) ==
        Mode::Campaign, "Campaign and Multiplayer");

  std::vector<uint8_t> zombies(1000, 0);
  zombies[3] = 0x0B;
  zombies[860] = 1;
  auto multiplayer = zombies;
  multiplayer[860] = 0;
  Check(LaunchMode("game:\\default_mp.xex", zombies) == Mode::Zombies, "Zombies launch");
  Check(LaunchMode("game:\\default_mp.xex", multiplayer) == Mode::Multiplayer, "Multiplayer launch");
  Check(LaunchMode("game:\\default_mp.xex", {}) == Mode::Multiplayer, "launch without data");
  Check(LaunchMode("game:\\default.xex", zombies) == Mode::Campaign, "Campaign launch");
}

int main(int argc, char** argv) {
  try {
    // Run in a second process to check persistence independently of the cache:
    // launch_options_test --write <root> <index>; launch_options_test --read <root> <index>
    if (argc == 4) {
      Configure(argv[2]);
      const int index = std::stoi(argv[3]);
      if (std::string(argv[1]) == "--write") {
        return SetDefaultModeIndex(index) ? 0 : 1;
      }
      if (std::string(argv[1]) == "--read") {
        return GetDefaultModeIndex() == index ? 0 : 1;
      }
    }
    Check(argc == 1, "unexpected test arguments");
    TemporaryDirectory directory;
    PreferenceTests(directory.root);
    CacheAndSharingTests(directory.root);
    StartupTests();
    TransientStartupTests(directory.root);
    InstalledModeTests(directory.root);
    std::cout << "Launch preference and startup-policy checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
