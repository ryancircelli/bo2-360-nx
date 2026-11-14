#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/rex_app.h>

class Fh1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fh1App>(new Fh1App(ctx, "fh1", PPCImageConfig));
  }

  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\default.xex";
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // SetupPresentation() copies the cvar into config before this hook runs, so
    // set the config field itself. Default is the native Metal renderer;
    // --gpu_plugin=xenos still selects the Vulkan/MoltenVK one.
    if (rex::cvar::GetFlagSource("gpu_plugin") == rex::cvar::Source::kDefault) {
      config.gpu_plugin = "metal";
    }
    // The game presents every 2nd guest vblank, so the default 60 Hz vblank timer
    // locks it to 30 fps. 120 Hz gives ~56 fps (simulation speed checked against
    // wall-clock at 60 and 120 Hz, see /tmp/bo2/fh1-speed/REPORT.md). The cvar is read
    // when the kernel video and window code run, after this hook.
    if (rex::cvar::GetFlagSource("video_mode_refresh_rate") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("video_mode_refresh_rate", "120");
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      const char* home = std::getenv("HOME");
      if (home) {
        auto disc = std::filesystem::path(home) / "Games" / "Xbox360" / "fh1" / "disc";
        if (std::filesystem::is_directory(disc, ec)) {
          paths.game_data_root = disc;
        }
      }
    }
  }
};
