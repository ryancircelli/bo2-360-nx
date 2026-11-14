#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/rex_app.h>

class Bo1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo1App>(new Bo1App(ctx, "bo1", PPCImageConfig));
  }

  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\default.xex";
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (rex::cvar::GetFlagSource("gpu_plugin") == rex::cvar::Source::kDefault) {
      config.gpu_plugin = "metal";
    }
    // BO1 keeps per-CPU script-parser state indexed by CPU number; with the default round-robin the
    // main thread lands on CPU 1, which the game also pins another script thread to, so the two share
    // one parser state block and ~65% of boots die with a random playeranim.script parse error.
    if (rex::cvar::GetFlagSource("guest_fake_cpu_start") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("guest_fake_cpu_start", "1");
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      const char* home = std::getenv("HOME");
      if (home) {
        auto disc = std::filesystem::path(home) / "Games" / "Xbox360" / "bo1" / "disc";
        if (std::filesystem::is_directory(disc, ec)) {
          paths.game_data_root = disc;
        }
      }
    }
  }
};
