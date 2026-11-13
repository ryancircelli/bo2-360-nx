// Shared launch preferences and startup policy. This layer needs no runtime or guest image.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bo2zm::launch {

enum class Mode { Campaign = 0, Multiplayer = 1, Zombies = 2 };
enum class Program { Main, Campaign };

// Switch: user/ beside the NRO, including before Runtime exists. PC: the configured
// runtime root; the SDK's default bo2cp profile shares bo2zm's launch preferences.
std::filesystem::path SharedUserRoot(const std::filesystem::path& runtime_user_dir = {});
void Configure(const std::filesystem::path& runtime_user_dir);

// Constructor-101 reader: direct stdio, no logging, cache, mutex or dynamic
// static state. Use an explicit SharedUserRoot() resolved from Switch argv[0].
int ReadDefaultModeIndexEarly(const std::filesystem::path& user_root);

// Test-only overrides never select Campaign or update the saved default.
std::optional<Mode> ParseTestMode(std::string_view value);
struct StartupTestOptions {
  bool campaign = false;
  std::optional<Mode> mode;
};
// Reads the flat test settings before runtime configuration exists, using stdio.
StartupTestOptions ReadStartupTestsEarly(const std::filesystem::path& toml);

// 0 = Campaign, 1 = Multiplayer, 2 = Zombies. Missing/invalid settings use Zombies.
// Runtime reads are cached per root for menu polling. Set updates the cache
// only after a successful replacement on disk.
// Set returns false on an invalid index or a failed write; the old value survives.
int GetDefaultModeIndex();
bool SetDefaultModeIndex(int index);
int GetDefaultModeIndex(const std::filesystem::path& user_root);
bool SetDefaultModeIndex(int index, const std::filesystem::path& user_root);
// Preserve existing preferences; a transient test must not seed a missing one.
bool MigrateLegacyDefault(bool boot_zombies, std::optional<Mode> test_mode,
                          const std::filesystem::path& user_root);

// Call only from main EveryFrame, outside command/VM loading. Each program's
// relaunch module supplies this entry point. It preserves an explicit pending
// launch, or writes the current boot's mode before using its normal restart path.
// Success exits the process; preparation errors log and return.
void RestartCurrentMode();

// The modes a player has installed. The build kit (tools/install/game_files.py) copies only the chosen modes'
// files, and the game names its base zones after the mode that loads them: sub_822C7CA0 in default_mp builds
// "code_post_gfx" (0x8201A804), "patch", "patch_ui" and "common" (0x8201A7C8), then appends "_zm" (0x82013164) or
// "_mp" (0x82013160) to each, while the campaign loads them without a suffix. A mode counts as installed when its
// code_post_gfx and common zones are in the game folder; the campaign also needs its program (bo2cp) beside this
// one, and Zombies and Multiplayer need bo2zm. A game folder with none of the six zones is not one we recognise
// (a missing or unreadable folder): every mode then counts as installed, and the game itself reports what is
// missing.
struct InstalledModes {
  bool campaign = true;
  bool multiplayer = true;
  bool zombies = true;

  bool Has(Mode mode) const;
  int Count() const;
  // "cp mp zm", the installed ones only, for the menus (bo2zm_modes_ui).
  std::string Text() const;
};

// Uses stdio only, so it is safe in the early handover constructor. campaign_program and main_program say whether
// bo2cp and bo2zm are installed beside the running program.
InstalledModes DetectInstalledModes(const std::filesystem::path& game_root, bool campaign_program,
                                    bool main_program = true);

// wanted when it is installed, else the first installed one of Zombies, Multiplayer and Campaign; wanted when none
// is (the game then says what is missing).
Mode FirstInstalled(Mode wanted, const InstalledModes& installed);

// The mode a launch request asks for: Campaign unless its path is default_mp.xex, then Zombies when the launch data
// is the block the game writes for Zombies (1000 bytes, byte 3 = 0x0B, byte 860 = 1), else Multiplayer.
Mode LaunchMode(std::string_view launch_path, const std::vector<uint8_t>& launch_data);

// Pending > direct Campaign / campaign test > transient MP/ZM test > saved default.
// A direct campaign start never consults the saved default.
Mode StartupMode(Program program, std::optional<Mode> pending, Mode saved,
                 bool test_campaign = false, std::optional<Mode> test_mode = std::nullopt);

}  // namespace bo2zm::launch
