// nfsmw - ReXGlue Recompiled Project
//
// Need for Speed: Most Wanted (Xbox 360, USA), recompiled. This first cut has
// no game hooks: it is the ReXApp shell around the generated code, to boot the
// image and see how far it gets.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/rex_app.h>

class NfsmwApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NfsmwApp>(new NfsmwApp(ctx, "nfsmw", PPCImageConfig));
  }

  // The recompiled image is the disc's default.xex.
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\default.xex";
  }

  // The xenos GPU plugin (MoltenVK/Metal on macOS) unless the config sets one.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // config.gpu_plugin was already read from the cvar before this hook runs, so
    // setting the cvar here is too late; set the config field directly.
    if (rex::cvar::GetFlagSource("gpu_plugin") == rex::cvar::Source::kDefault) {
      config.gpu_plugin = "xenos";
    }
  }

  // With no --game_data_root, the game data root is the (read-only) nfsmw disc
  // folder. Saves and caches keep their defaults (never the disc).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      const char* home = std::getenv("HOME");
      if (home) {
        auto disc = std::filesystem::path(home) / "Games" / "Xbox360" / "nfsmw" / "disc";
        if (std::filesystem::is_directory(disc, ec)) {
          paths.game_data_root = disc;
        }
      }
    }
  }
};
