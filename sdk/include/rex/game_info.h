/**
 * @file        game_info.h
 * @brief       Facts about the running title that SDK code reads without
 *              naming a game. Titles fill them in RegisterGame()
 *              (rex/game_api.h), usually by loading their manifest
 *              (LoadGameManifest below).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex {

/// Facts about the running title that SDK code needs without naming it.
/// Defaults are the values the SDK used before titles registered themselves.
// How the title sets up the GPU backends ([renderer] in its manifest).
struct RendererProfile {
  // Backend workarounds the title needs, by name (metal_title_quirks).
  std::vector<std::string> quirks;
  // Backend settings (metal_*, vulkan_*, d3d12_*, gpu_*, ...) whose default the title
  // replaces; config files, the environment and the command line still win.
  std::vector<std::pair<std::string, std::string>> settings;
};

struct GameInfo {
  /// Short id: config file stem, Switch folder (sdmc:/switch/<short_name>).
  std::string short_name = "rex";
  /// Title ids the build runs (the first is the main one). Empty: any.
  std::vector<uint32_t> title_ids;
  /// Guest path of the module to launch ("game:\\default.xex"). Empty: the
  /// SDK's default.
  std::string executable;
  /// Folder beside the executable that holds the disc files.
  std::string data_root = "game";
  /// Shader library file beside the executable (and its .spvidx index).
  std::string shader_pack;
  /// Pipeline list read at start-up (beside the executable or in the bundle).
  std::string pipeline_list;
  /// Settings whose compiled-in default the title replaces. Applied at
  /// setup only when the user's config and environment left them alone.
  std::vector<std::pair<std::string, std::string>> default_cvars;
  RendererProfile renderer;
  /// First guest port the title's system link uses; the online relay carries
  /// three ports from here (the last one is the SDK's side channel).
  uint16_t first_link_port = 1000;
};

/// The process-wide title facts, set by RegisterGame() before setup.
inline GameInfo& Game() {
  static GameInfo info;
  return info;
}

/// Reads a title manifest (TOML) into `info`. Keys:
///   [game] name, title_ids = ["415608C3"], executable, data_root,
///          shader_pack, pipeline_list, first_link_port
///   [defaults] cvar = value  (strings, booleans or numbers)
/// Keys a manifest leaves out keep their current value. Returns false and
/// fills `error` on a syntax error or a key of the wrong type.
bool LoadGameManifest(std::string_view toml_text, GameInfo& info, std::string* error = nullptr);

/// Applies `info.default_cvars` to settings still at their compiled-in
/// default. Returns the number applied.
int ApplyGameDefaults(const GameInfo& info);

// Makes the renderer profile's quirks and settings the defaults of the backend cvars,
// including those of a GPU plugin loaded later. Returns false when a value does not parse.
bool ApplyRendererProfile(const RendererProfile& profile);

}  // namespace rex
