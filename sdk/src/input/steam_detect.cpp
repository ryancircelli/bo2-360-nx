/**
 * @file        input/steam_detect.cpp
 * @brief       Notices Steam and Steam Input, which can take controllers away
 *              from the game.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/steam_detect.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include <rex/platform.h>

#if REX_PLATFORM_MAC
#include <libproc.h>
#elif REX_PLATFORM_WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace rex::input {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool IsSteamProcessName(std::string_view name) {
  const std::string lower = Lower(name);
  return lower == "steam_osx" || lower == "steam" || lower == "steam.exe";
}

bool SteamProcessRunning() {
#if REX_PLATFORM_MAC
  const int count = proc_listallpids(nullptr, 0);
  if (count <= 0) {
    return false;
  }
  std::vector<pid_t> pids(static_cast<size_t>(count) + 32);
  const int filled =
      proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
  for (int i = 0; i < filled; i++) {
    char name[2 * MAXCOMLEN + 1] = {};
    if (proc_name(pids[i], name, sizeof(name)) > 0 && IsSteamProcessName(name)) {
      return true;
    }
  }
  return false;
#elif REX_PLATFORM_LINUX
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    const auto pid = entry.path().filename().string();
    if (pid.empty() || !std::all_of(pid.begin(), pid.end(), ::isdigit)) {
      continue;
    }
    std::ifstream comm(entry.path() / "comm");
    std::string name;
    if (std::getline(comm, name) && IsSteamProcessName(name)) {
      return true;
    }
  }
  return false;
#elif REX_PLATFORM_WIN32
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  bool found = false;
  for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found;
       ok = Process32NextW(snapshot, &entry)) {
    std::string name;
    for (const wchar_t* c = entry.szExeFile; *c; c++) {
      name += static_cast<char>(*c < 0x80 ? *c : '?');
    }
    found = IsSteamProcessName(name);
  }
  CloseHandle(snapshot);
  return found;
#else
  return false;
#endif
}

std::vector<std::filesystem::path> SteamConfigCandidates() {
  std::vector<std::filesystem::path> out;
#if REX_PLATFORM_WIN32
  if (const char* x86 = std::getenv("ProgramFiles(x86)")) {
    out.emplace_back(std::filesystem::path(x86) / "Steam" / "config" / "config.vdf");
  }
#else
  if (const char* home = std::getenv("HOME")) {
    const std::filesystem::path h(home);
#if REX_PLATFORM_MAC
    out.push_back(h / "Library" / "Application Support" / "Steam" / "config" / "config.vdf");
#else
    out.push_back(h / ".steam" / "steam" / "config" / "config.vdf");
    out.push_back(h / ".local" / "share" / "Steam" / "config" / "config.vdf");
#endif
  }
#endif
  return out;
}

// Steam stores its per-family Steam Input switches as
// "SteamController_<Family>Support" "<n>" in config.vdf; nonzero is on (for
// PlayStation, 1 means on in games without their own PlayStation support).
void ReadSteamInputSettings(SteamInputReport& report) {
  static const std::regex kSetting(R"re("SteamController_(\w+?)Support"\s+"(\d+)")re");
  for (const auto& path : SteamConfigCandidates()) {
    std::ifstream file(path);
    if (!file) {
      continue;
    }
    std::stringstream text;
    text << file.rdbuf();
    const std::string contents = text.str();
    for (auto it = std::sregex_iterator(contents.begin(), contents.end(), kSetting);
         it != std::sregex_iterator(); ++it) {
      const std::string family = (*it)[1];
      const std::string value = (*it)[2];
      report.settings.push_back(family + "=" + value);
      if (value != "0") {
        report.steam_input_enabled = true;
      }
    }
    return;
  }
}

}  // namespace

SteamInputReport DetectSteamInput() {
  SteamInputReport report;
  report.steam_running = SteamProcessRunning();
  for (const char* name : {"SteamAppId", "SteamGameId", "SteamClientLaunch", "SteamEnv"}) {
    if (const char* value = std::getenv(name); value && *value) {
      report.launched_by_steam = true;
    }
  }
  if (report.steam_running || report.launched_by_steam) {
    ReadSteamInputSettings(report);
  }
  return report;
}

std::string SteamInputWarning(const SteamInputReport& report) {
  if (!report.steam_running && !report.launched_by_steam) {
    return {};
  }
  std::string text = report.launched_by_steam ? "Launched from Steam" : "Steam is running";
  if (!report.settings.empty()) {
    text += " (Steam Input settings:";
    for (const auto& setting : report.settings) {
      text += " " + setting;
    }
    text += ")";
  }
  text +=
      ". With Steam Input on for a controller type, Steam takes those controllers: they show as "
      "connected but do nothing, or appear as a 'Steam Virtual Gamepad'. If a controller does not "
      "respond, turn Steam Input off for it (Steam > Settings > Controller, and the game's "
      "Properties > Controller > Disable Steam Input) or quit Steam.";
  if (!report.settings.empty() && !report.steam_input_enabled) {
    text += " Steam Input appears to be off for every controller type, so this is likely fine.";
  }
  return text;
}

bool IsSteamVirtualPad(uint16_t vendor_id, uint16_t product_id, std::string_view name) {
  constexpr uint16_t kValve = 0x28DE;
  constexpr uint16_t kSteamVirtualGamepad = 0x11FF;
  if (vendor_id == kValve && product_id == kSteamVirtualGamepad) {
    return true;
  }
  return Lower(name).find("steam virtual") != std::string::npos;
}

}  // namespace rex::input
