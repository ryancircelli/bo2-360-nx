// Compat: environment access on v0.11.
#pragma once
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
namespace rex::platform::env {
inline std::optional<std::string> get(std::string_view name) {
  const char* v = std::getenv(std::string(name).c_str());
  return v ? std::optional<std::string>(v) : std::nullopt;
}
inline bool set(std::string_view n, std::string_view v) { return setenv(std::string(n).c_str(), std::string(v).c_str(), 1) == 0; }
inline bool unset(std::string_view n) { return unsetenv(std::string(n).c_str()) == 0; }
}  // namespace rex::platform::env
