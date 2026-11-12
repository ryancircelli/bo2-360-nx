/**
 * Cross-process ownership for local SDL pads shared by BO2 instances.
 */
#pragma once

#include <optional>
#include <filesystem>
#include <string>
#include <string_view>

namespace rex::input::sdl {

/// A stable key for a physical pad. Serial is preferred because it survives
/// changing USB ports; path is used for devices that do not report a serial.
std::string MakePadIdentity(std::string_view guid, std::string_view serial,
                            std::string_view path);

/// Coordinates up to eight pads between BO2_INSTANCE processes. If the
/// environment variable is absent, ownership is disabled and all pads pass.
class PadOwnership {
 public:
  PadOwnership();
  /// Explicit registry constructor used by focused ownership tests.
  PadOwnership(int instance, std::filesystem::path registry_path);

  bool enabled() const { return instance_.has_value(); }
  bool Owns(std::string_view identity) const;

 private:
  std::optional<int> instance_;
  std::filesystem::path registry_path_;
};

}  // namespace rex::input::sdl
