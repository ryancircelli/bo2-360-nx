/**
 * @file        tools/padtest/padtest_main.cpp
 * @brief       rex-padtest: lists controllers, the player each one gets, and
 *              their live state, with the same input code a game uses.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_log.h>
#include <rex/input/steam_detect.h>
#include <rex/logging.h>
#include <rex/logging/api.h>

namespace {

using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

volatile std::sig_atomic_t g_stop = 0;

void Usage() {
  std::printf(
      "rex-padtest: shows each controller, the player it gets in a game, and what it is "
      "pressing.\n"
      "\n"
      "  rex-padtest [options] [--<input cvar>=<value> ...]\n"
      "\n"
      "  --list         print the controllers once and exit\n"
      "  --seconds=N    run for N seconds (default: until Ctrl+C)\n"
      "  --plain        print changes as lines instead of redrawing a table\n"
      "  --no-sdl       script pads only (no real controllers)\n"
      "\n"
      "While running, each controller rumbles once when it connects (the left motor for\n"
      "player 1 and 3, the right one for 2 and 4), and the triggers drive its motors.\n"
      "Plug and unplug controllers to check that the other players stay where they are.\n"
      "\n"
      "Input settings work as in a game, for example:\n"
      "  --input_player_slots=\"1=<serial>;2=054c:0ce6\"  pin controllers to players\n"
      "  --input_virtual_pads=after|normal|ignore      Steam virtual pads\n"
      "  --hid_virtual_pads=4 --hid_virtual_pad1=\"1-5:stick_up\"   scripted pads\n"
      "  --hid_mappings_file=gamecontrollerdb.txt      extra SDL mappings\n"
      "and the same REX_<NAME> environment variables.\n");
}

std::string Pad(std::string text, size_t width) {
  if (text.size() > width) {
    text.resize(width);
  }
  text.append(width - text.size(), ' ');
  return text;
}

std::string Row(const InputSystem::DeviceSnapshot& snap) {
  const auto& info = snap.info;
  std::string player = snap.user >= 0 ? fmt::format("P{}", snap.user + 1) : "--";
  std::string ids = info.vendor_id || info.product_id
                        ? fmt::format("{:04x}:{:04x}", info.vendor_id, info.product_id)
                        : "";
  std::string link = info.transport.empty() ? (info.synthetic ? "keyboard" : "?") : info.transport;
  std::string flags;
  if (info.virtual_device) {
    flags += " virtual";
  }
  if (!info.synthetic && info.transport != "script") {
    flags += info.can_rumble ? " rumble" : " no-rumble";
  }
  return fmt::format("{} {} {} {}{}\n      serial={} guid={}\n      {}", Pad(player, 3),
                     Pad(info.name.empty() ? "?" : info.name, 28), Pad(ids, 9), Pad(link, 9),
                     flags, info.serial.empty() ? "(none)" : info.serial,
                     info.guid.empty() ? "(none)" : info.guid,
                     snap.has_state ? FormatGamepad(snap.state.gamepad) : "no state");
}

bool IsTerminal() {
  return isatty(STDOUT_FILENO);
}

}  // namespace

int main(int argc, char** argv) {
  bool list_only = false;
  bool plain = !IsTerminal();
  bool use_sdl = true;
  double seconds = 0;
  std::vector<char*> cvar_args = {argv[0]};
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      Usage();
      return 0;
    } else if (arg == "--list") {
      list_only = true;
    } else if (arg == "--plain") {
      plain = true;
    } else if (arg == "--no-sdl") {
      use_sdl = false;
    } else if (arg.rfind("--seconds=", 0) == 0) {
      seconds = std::atof(arg.c_str() + 10);
    } else {
      cvar_args.push_back(argv[i]);
    }
  }

  rex::cvar::Init(static_cast<int>(cvar_args.size()), cvar_args.data());
  rex::cvar::ApplyEnvironment();
  rex::cvar::FinalizeInit();
  // Plain mode prints the log as it goes; the table would wipe it, so there
  // the log goes to a file shown at the bottom.
  const std::string log_path =
      (std::filesystem::temp_directory_path() / "rex-padtest.log").string();
  {
    // bo2's SDK: BuildLogConfig takes the log file first and has no
    // ApplyLogCvarOverrides.
    auto config = rex::BuildLogConfig(nullptr, "info", {});
    config.log_to_console = plain || list_only;
    if (!config.log_to_console) {
      config.log_file = log_path.c_str();
    }
    rex::InitLogging(config);
  }

  std::signal(SIGINT, [](int) { g_stop = 1; });
  std::signal(SIGTERM, [](int) { g_stop = 1; });

  const SteamInputReport steam = DetectSteamInput();
  const std::string steam_warning = SteamInputWarning(steam);
  std::printf("Steam: %s\n", steam_warning.empty() ? "not running" : steam_warning.c_str());

  InputSystem input(nullptr);
  if (auto script = CreateScriptPadDriverFromConfig()) {
    input.AddDriver(std::move(script));
  }
  sdl::SDLInputDriver* sdl_driver = nullptr;
  if (use_sdl) {
    auto driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
    if (driver->Setup() == X_STATUS_SUCCESS && driver->InitHeadless()) {
      sdl_driver = driver.get();
      input.AddDriver(std::move(driver));
    } else {
      std::printf("SDL could not start: no real controllers will show.\n");
    }
  }
  input.SetDeviceAssignment(std::make_unique<SlotAssignment>());

  auto pump = [&] {
    if (sdl_driver) {
      sdl_driver->PumpHeadless();
    }
  };

  if (list_only) {
    // Give the OS a moment to report controllers already connected.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < until) {
      pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto snaps = input.SnapshotDevices();
    std::printf("%zu controller(s)\n", snaps.size());
    for (const auto& snap : snaps) {
      std::printf("%s\n", Row(snap).c_str());
    }
    return 0;
  }

  struct Rumble {
    std::chrono::steady_clock::time_point until;
    bool active = false;
    uint16_t left = 0;
    uint16_t right = 0;
  };
  std::map<uint64_t, Rumble> rumble;
  std::map<uint64_t, std::string> last_line;
  const auto start = std::chrono::steady_clock::now();
  auto last_draw = start - std::chrono::seconds(1);

  while (!g_stop) {
    pump();
    const auto now = std::chrono::steady_clock::now();
    if (seconds > 0 && now - start > std::chrono::duration<double>(seconds)) {
      break;
    }
    const auto snaps = input.SnapshotDevices();
    input.LogStateIfDue();

    for (const auto& snap : snaps) {
      if (snap.info.synthetic) {
        continue;
      }
      const uint64_t key = static_cast<uint64_t>(snap.info.id);
      auto [it, fresh] = rumble.try_emplace(key);
      Rumble& r = it->second;
      if (fresh && snap.user >= 0) {
        // Hello buzz: left motor for odd players, right for even.
        r.until = now + std::chrono::milliseconds(400);
        r.left = snap.user % 2 == 0 ? 0xC000 : 0;
        r.right = snap.user % 2 == 1 ? 0xC000 : 0;
      }
      uint16_t left = 0;
      uint16_t right = 0;
      if (now < r.until) {
        left = r.left;
        right = r.right;
      } else if (snap.has_state) {
        left = static_cast<uint16_t>(snap.state.gamepad.left_trigger * 257);
        right = static_cast<uint16_t>(snap.state.gamepad.right_trigger * 257);
      }
      const bool active = left || right;
      if (active || r.active) {
        X_INPUT_VIBRATION vibration = {};
        vibration.left_motor_speed = left;
        vibration.right_motor_speed = right;
        input.SetDeviceVibration(snap.info.id, &vibration);
      }
      r.active = active;
    }

    if (plain) {
      for (const auto& snap : snaps) {
        const uint64_t key = static_cast<uint64_t>(snap.info.id);
        std::string line = Row(snap);
        std::replace(line.begin(), line.end(), '\n', ' ');
        if (last_line[key] != line) {
          last_line[key] = line;
          std::printf("%.2f %s\n", std::chrono::duration<double>(now - start).count(),
                      line.c_str());
        }
      }
      for (auto it = last_line.begin(); it != last_line.end();) {
        const bool present = std::any_of(snaps.begin(), snaps.end(), [&](const auto& snap) {
          return static_cast<uint64_t>(snap.info.id) == it->first;
        });
        if (present) {
          ++it;
          continue;
        }
        std::printf("%.2f gone: %s\n", std::chrono::duration<double>(now - start).count(),
                    it->second.substr(0, it->second.find("serial=")).c_str());
        rumble.erase(it->first);
        it = last_line.erase(it);
      }
      std::fflush(stdout);
    } else if (now - last_draw > std::chrono::milliseconds(100)) {
      last_draw = now;
      std::string screen = "\x1b[H\x1b[J";
      screen += fmt::format(
          "rex-padtest  {} controller(s)  Ctrl+C to quit. Triggers rumble; plug and unplug to "
          "test.\n\n",
          snaps.size());
      for (const auto& snap : snaps) {
        screen += Row(snap) + "\n\n";
      }
      const auto reserved = input.slot_table().Reservations();
      if (!reserved.empty()) {
        screen += "Waiting for:";
        for (const auto& [identity, ordinal] : reserved) {
          screen += fmt::format(" P{} <- {}", ordinal + 1, identity);
        }
        screen += "\n";
      }
      screen += fmt::format("\nLog (connects, players, Steam): {}\n", log_path);
      std::fputs(screen.c_str(), stdout);
      std::fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  // Leave every motor off.
  for (const auto& snap : input.SnapshotDevices()) {
    X_INPUT_VIBRATION off = {};
    input.SetDeviceVibration(snap.info.id, &off);
  }
  input.Shutdown();
  return 0;
}
