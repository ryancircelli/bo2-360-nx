// Host simulation of Switch argv-based preferences, before Runtime exists.
// Compile this and bo2zm_launch_options.cpp with -D__SWITCH__=1; no libnx or SDK needed.
#include "../../games/bo2/src/bo2zm_launch_options.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
int __system_argc = 0;
char** __system_argv = nullptr;
}

int main() {
  namespace fs = std::filesystem;
  using namespace bo2zm::launch;
  const auto root = fs::temp_directory_path() /
                    ("bo2-switch-launch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  if (!fs::create_directory(root)) {
    return 1;
  }
  auto check = [](bool result) {
    if (!result) {
      throw std::runtime_error("Switch launch preference check failed");
    }
  };
  int result = 0;
  try {
    check(SharedUserRoot().empty());
    check(ReadDefaultModeIndexEarly({}) == 2);
    char device_program[] = "sdmc:/switch/bo2zm/./bo2cp.nro";
    char* device_arguments[] = {device_program, nullptr};
    __system_argc = 1;
    __system_argv = device_arguments;
    check(SharedUserRoot() == fs::path("sdmc:/switch/bo2zm/user").lexically_normal());
    std::string main_program = (root / "bo2zm.nro").string();
    std::string campaign_program = (root / "bo2cp.nro").string();
    char* arguments[] = {main_program.data(), nullptr};
    __system_argc = 1;
    __system_argv = arguments;
    check(SharedUserRoot() == root / "user");
    check(ReadDefaultModeIndexEarly(SharedUserRoot()) == 2);
    check(GetDefaultModeIndex() == 2);
    check(SetDefaultModeIndex(1));
    check(ReadDefaultModeIndexEarly(SharedUserRoot()) == 1);
    arguments[0] = campaign_program.data();
    Configure(root / "wrong-runtime-root");
    check(SharedUserRoot() == root / "user");
    check(GetDefaultModeIndex() == 1);
    check(SetDefaultModeIndex(0));
    arguments[0] = main_program.data();
    Configure(root / "another-runtime-root");
    check(GetDefaultModeIndex() == 0);
    {
      std::ofstream config(root / "bo2zm.toml");
      config << "bo2zm_test_mode = \"mp\"\nbo2zm_test_campaign = false\n";
    }
    const auto test = ReadStartupTestsEarly(fs::path(arguments[0]).parent_path() / "bo2zm.toml");
    check(test.mode == ParseTestMode("mp") && !test.campaign);
    check(StartupMode(Program::Main, std::nullopt,
                      static_cast<Mode>(ReadDefaultModeIndexEarly(SharedUserRoot())),
                      test.campaign, test.mode) == Mode::Multiplayer);
    check(MigrateLegacyDefault(true, test.mode, SharedUserRoot()));
    check(ReadDefaultModeIndexEarly(SharedUserRoot()) == 0);
    check(GetDefaultModeIndex() == 0);
    arguments[0] = campaign_program.data();
    check(StartupMode(Program::Campaign, std::nullopt, Mode::Zombies, false, test.mode) == Mode::Campaign);
    check(StartupMode(Program::Main, Mode::Zombies, Mode::Campaign, false, test.mode) == Mode::Zombies);
    arguments[0] = main_program.data();
    fs::remove(root / "user" / "launch_options.txt");
    check(MigrateLegacyDefault(false, test.mode, SharedUserRoot()));
    check(!fs::exists(root / "user" / "launch_options.txt"));
    for (int poll = 0; poll < 100; ++poll) {
      check(GetDefaultModeIndex() == 0);
    }
    check(!fs::exists(root / "wrong-runtime-root"));
    check(!fs::exists(root / "another-runtime-root"));
    std::cout << "Switch early/shared-root and cached-poll checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  __system_argc = 0;
  __system_argv = nullptr;
  std::error_code ec;
  fs::remove_all(root, ec);
  return result;
}
