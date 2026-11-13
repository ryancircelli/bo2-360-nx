// See bo2zm_settings.h.

#include "bo2zm_settings.h"

#include <fstream>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>

namespace {

std::mutex g_mutex;

std::string Trim(std::string_view text) {
  const size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(" \t\r");
  return std::string(text.substr(first, last - first + 1));
}

// The file's lines as (key, value), in order.
std::vector<std::pair<std::string, std::string>> ReadAll(const std::filesystem::path& path) {
  std::vector<std::pair<std::string, std::string>> entries;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    std::string key = Trim(std::string_view(line).substr(0, equals));
    if (!key.empty()) {
      entries.emplace_back(std::move(key), Trim(std::string_view(line).substr(equals + 1)));
    }
  }
  return entries;
}

}  // namespace

std::filesystem::path bo2zm::settings::File() {
  const auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->user_data_root() / "bo2zm_settings.txt" : std::filesystem::path();
}

std::optional<std::string> bo2zm::settings::Get(std::string_view key) {
  return GetAt(File(), key);
}

std::optional<std::string> bo2zm::settings::GetAt(const std::filesystem::path& path, std::string_view key) {
  std::lock_guard lock(g_mutex);
  for (auto& [name, value] : ReadAll(path)) {
    if (name == key) {
      return value;
    }
  }
  return std::nullopt;
}

bool bo2zm::settings::Set(std::string_view key, std::string_view value) {
  return SetMany({{std::string(key), std::string(value)}});
}

bool bo2zm::settings::SetMany(const std::vector<std::pair<std::string, std::string>>& values) {
  return SetManyAt(File(), values);
}

bool bo2zm::settings::SetAt(const std::filesystem::path& path, std::string_view key, std::string_view value) {
  return SetManyAt(path, {{std::string(key), std::string(value)}});
}

bool bo2zm::settings::SetManyAt(const std::filesystem::path& path,
                             const std::vector<std::pair<std::string, std::string>>& values) {
  std::lock_guard lock(g_mutex);
  if (path.empty() || values.empty()) {
    return false;
  }
  auto entries = ReadAll(path);
  for (const auto& [key, value] : values) {
    bool found = false;
    for (auto& [name, saved] : entries) {
      if (name == key) {
        saved = value;
        found = true;
      }
    }
    if (!found) {
      entries.emplace_back(key, value);
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::trunc);
  for (const auto& [name, saved] : entries) {
    file << name << " = " << saved << "\n";
  }
  if (!file) {
    REXLOG_WARN("[settings] could not save {} = {} ({} keys) to {}", values.front().first, values.front().second,
                values.size(), path.string());
    return false;
  }
  if (values.size() == 1) {
    REXLOG_INFO("[settings] saved {} = {} ({})", values.front().first, values.front().second, path.string());
  } else {
    REXLOG_INFO("[settings] saved {} = {} and {} more ({})", values.front().first, values.front().second,
                values.size() - 1, path.string());
  }
  return true;
}
