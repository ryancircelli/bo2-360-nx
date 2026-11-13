// The player's settings that this port keeps between runs: <user folder>/bo2zm_settings.txt, one "key = value" per
// line (preset = medium, fps_counter = on, ...). The game's own settings (sensitivity, brightness, ...) stay in its
// profile; this file only holds the port's.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bo2zm::settings {

// The settings file (empty before the runtime exists).
std::filesystem::path File();

// The saved value of key, if the file has it.
std::optional<std::string> Get(std::string_view key);
// A shared preference file, used when both executables must see the same key.
std::optional<std::string> GetAt(const std::filesystem::path& path, std::string_view key);
bool SetAt(const std::filesystem::path& path, std::string_view key, std::string_view value);
bool SetManyAt(const std::filesystem::path& path,
               const std::vector<std::pair<std::string, std::string>>& values);

// Saves key = value, keeping the other keys. false (and a warning in the log) when the file cannot be written.
bool Set(std::string_view key, std::string_view value);

// Saves several keys with one write of the file (a map profile: ten of them).
bool SetMany(const std::vector<std::pair<std::string, std::string>>& values);

}  // namespace bo2zm::settings
