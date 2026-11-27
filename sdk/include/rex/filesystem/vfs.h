/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/cvar.h>
#include <rex/thread/mutex.h>

REXCVAR_DECLARE(bool, allow_game_relative_writes);

namespace rex::filesystem {

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  bool RegisterDevice(std::unique_ptr<Device> device);
  bool UnregisterDevice(const std::string_view path);
  // Puts `device` in place of the registered device with the same mount path (disc swap). The old
  // device is kept alive until shutdown: open files and resolved entries still point into it.
  bool ReplaceDevice(std::unique_ptr<Device> device);

  bool RegisterSymbolicLink(const std::string_view path, const std::string_view target);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  Entry* ResolvePath(const std::string_view path);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition, uint32_t desired_access,
                    bool is_directory, bool is_non_directory, File** out_file,
                    FileAction* out_action);

 private:
  rex::thread::global_critical_region global_critical_region_;
  std::vector<std::unique_ptr<Device>> devices_;
  std::vector<std::unique_ptr<Device>> retired_devices_;
  std::unordered_map<std::string, std::string> symlinks_;

  bool ResolveSymbolicLink(const std::string_view path, std::string& result);
};

// Host folder holding disc `disc` of a multi-disc game: <game_root>/../disc<N> when it exists,
// else game_root itself for the boot disc. Empty when that disc is not in the game folder.
std::filesystem::path DiscRoot(const std::filesystem::path& game_root, uint32_t disc,
                               uint32_t boot_disc);

}  // namespace rex::filesystem
