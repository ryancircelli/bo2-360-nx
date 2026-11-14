/**
 * @file        game_api.h
 * @brief       Game registration: one binary, one title pack, hooks registered
 *              through RegisterGame() instead of the app naming a game.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/game_info.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

namespace rex {

class GameApp;

/// What a title registers. Every hook list runs in registration order, in the
/// ReXApp callback of the same name.
class GameApi {
 public:
  GameInfo& info() { return Game(); }

  void OnConfigurePaths(std::function<void(GameApp&, PathConfig&)> fn) {
    configure_paths_.push_back(std::move(fn));
  }
  void OnConfigLoaded(std::function<void(GameApp&)> fn) { config_loaded_.push_back(std::move(fn)); }
  void OnPostInitLogging(std::function<void(GameApp&)> fn) {
    post_init_logging_.push_back(std::move(fn));
  }
  void OnPreSetup(std::function<void(GameApp&, RuntimeConfig&)> fn) {
    pre_setup_.push_back(std::move(fn));
  }
  void OnLoadXexImage(std::function<void(GameApp&, std::string&)> fn) {
    load_xex_image_.push_back(std::move(fn));
  }
  void OnPostSetup(std::function<void(GameApp&)> fn) { post_setup_.push_back(std::move(fn)); }
  /// At most one title hook may hold the module launch; the first that
  /// returns true owns start().
  void OnHoldModuleLaunch(std::function<bool(GameApp&, std::function<void()>)> fn) {
    hold_module_launch_.push_back(std::move(fn));
  }
  void OnShutdown(std::function<void(GameApp&)> fn) { shutdown_.push_back(std::move(fn)); }

 private:
  friend class GameApp;
  std::vector<std::function<void(GameApp&, PathConfig&)>> configure_paths_;
  std::vector<std::function<void(GameApp&)>> config_loaded_;
  std::vector<std::function<void(GameApp&)>> post_init_logging_;
  std::vector<std::function<void(GameApp&, RuntimeConfig&)>> pre_setup_;
  std::vector<std::function<void(GameApp&, std::string&)>> load_xex_image_;
  std::vector<std::function<void(GameApp&)>> post_setup_;
  std::vector<std::function<bool(GameApp&, std::function<void()>)>> hold_module_launch_;
  std::vector<std::function<void(GameApp&)>> shutdown_;
};

/// Defined once per binary by the title pack (games/<name>/).
void RegisterGame(GameApi& api);

/// Loads the title manifest compiled in by rexglue_game_manifest() (rex::kGameManifest)
/// into api.info(); a manifest that does not parse stops start-up.
inline void UseGameManifest(GameApi& api, std::string_view toml_text, std::string_view origin) {
  if (std::string error; !LoadGameManifest(toml_text, api.info(), &error)) {
    throw std::runtime_error(std::string(origin) + ": " + error);
  }
}

/// The game-neutral app: runs whatever RegisterGame() registered.
class GameApp : public ReXApp {
 public:
  static std::unique_ptr<ui::WindowedApp> Create(ui::WindowedAppContext& ctx, std::string_view name,
                                                 PPCImageInfo ppc_info) {
    auto api = std::make_unique<GameApi>();
    RegisterGame(*api);
    return std::unique_ptr<GameApp>(new GameApp(ctx, name, ppc_info, std::move(api)));
  }

  // The accessors title hooks need (ReXApp keeps them protected).
  using ReXApp::cache_root;
  using ReXApp::game_data_root;
  using ReXApp::imgui_drawer;
  using ReXApp::runtime;
  using ReXApp::update_data_root;
  using ReXApp::user_data_root;

 protected:
  GameApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
          std::unique_ptr<GameApi> api)
      : ReXApp(ctx, name, ppc_info), api_(std::move(api)) {}

  void OnConfigurePaths(PathConfig& paths) override {
    // The manifest's local_data_root, when no --game_data_root was given.
    if (paths.game_data_root.empty() && !Game().local_data_root.empty()) {
      std::filesystem::path root = Game().local_data_root;
      const std::string& text = Game().local_data_root;
      if (text.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        root = home ? std::filesystem::path(home) / text.substr(2) : std::filesystem::path();
      }
      std::error_code ec;
      if (!root.empty() && std::filesystem::is_directory(root, ec)) paths.game_data_root = root;
    }
    for (auto& fn : api_->configure_paths_) fn(*this, paths);
  }
  void OnConfigLoaded() override {
    for (auto& fn : api_->config_loaded_) fn(*this);
  }
  void OnPostInitLogging() override {
    for (auto& fn : api_->post_init_logging_) fn(*this);
  }
  void OnPreSetup(RuntimeConfig& config) override {
    // The manifest's [renderer] and [defaults], before the title's own hooks.
    if (!ApplyRendererProfile(Game().renderer)) {
      REXLOG_WARN("{}: a renderer setting in the title manifest did not parse", Game().short_name);
    }
    ApplyGameDefaults(Game());
    // ReXApp copied gpu_plugin into config before this hook; take a manifest default.
    config.gpu_plugin = cvar::GetFlagByName("gpu_plugin");
    for (auto& fn : api_->pre_setup_) fn(*this, config);
  }
  void OnLoadXexImage(std::string& xex_image) override {
    if (!Game().executable.empty()) xex_image = Game().executable;  // the manifest's game.executable
    for (auto& fn : api_->load_xex_image_) fn(*this, xex_image);
  }
  void OnPostSetup() override {
    for (auto& fn : api_->post_setup_) fn(*this);
  }
  bool OnHoldModuleLaunch(std::function<void()> start) override {
    for (auto& fn : api_->hold_module_launch_) {
      if (fn(*this, start)) return true;
    }
    return false;
  }
  void OnShutdown() override {
    for (auto& fn : api_->shutdown_) fn(*this);
  }

 private:
  std::unique_ptr<GameApi> api_;
};

}  // namespace rex
