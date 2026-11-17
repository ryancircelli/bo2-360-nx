// Compat: v0.10 rex/filesystem.h host-path helpers on v0.11 rex::platform / rex::string.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <rex/platform/file.h>
#include <rex/string/text.h>
namespace rex::filesystem {
inline std::filesystem::path GetExecutablePath() { return ::rex::platform::GetExecutablePath(); }
inline std::filesystem::path GetExecutableFolder() { return ::rex::platform::GetExecutableFolder(); }
inline std::filesystem::path GetUserFolder() { return ::rex::platform::GetUserFolder(); }
inline std::string path_to_utf8(const std::filesystem::path& p) { return ::rex::string::PathToUTF8(p); }
inline std::filesystem::path to_path(std::string_view s) { return std::filesystem::u8path(s.begin(), s.end()); }
}  // namespace rex::filesystem
