// bo2zm - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>
#include <stdexcept>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_controllers.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_gsc_mods.h"
#include "bo2zm_lifecycle.h"
#include "bo2zm_script_manager.h"
#include "bo2zm_name.h"
#include "bo2zm_pc_profiler.h"
#include "bo2zm_perf_log.h"
#include "bo2zm_relaunch.h"
#include "bo2zm_switch.h"
#include "bo2zm_test_pad.h"
#include "bo2zm_online.h"
#include "bo2zm_updates.h"
#include "bo2zm_test_waits.h"
#include "native/bo2zm_native_system.h"
#include "native/render/bo2zm_graphics_settings.h"
#include "native/render/bo2zm_native_capture.h"
#include "native/render/bo2zm_native_precompile.h"

class Bo2zmApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo2zmApp>(new Bo2zmApp(ctx, "bo2zm",
        PPCImageConfig));
  }

  // The SDK loads game:\default.xex (the campaign executable). Zombies and
  // multiplayer live in default_mp.xex, which is what app/ recompiles.
  // Loading the wrong one fails with "No function registered at 825C69B8"
  // (default.xex's TU18 entry point).
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\default_mp.xex";
  }

  // Switch: start the services the game uses (sockets) before it runs.
  // With bo2zm_renderer = "native" the graphics system is the port's own
  // (native/bo2zm_native_system.cpp); ReXApp only loads the Xenos plugin when
  // config.graphics is empty.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!bo2zm::lifecycle::Prepare()) {
      throw std::runtime_error("Could not disable automated diagnostics after a fatal restart");
    }
    // Local MP and Zombies expose an offline profile for each controller port.
    if (rex::cvar::GetFlagSource("offline_user_count") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("offline_user_count", "4");
    }
    bo2zm::InitSwitchServices();
    bo2zm::lifecycle::OnBoot();  // bo2zm_lifecycle.h
    bo2zm::autoexec::ApplyVolume();  // bo2zm_volume (PC test runs: 5 %)
    bo2zm::controllers::WrapInput(config);  // Switch: the system's player numbers
    bo2zm::test_pad::WrapInput(config);  // test runs only (bo2zm_test_buttons)
    if (bo2zm::native::Active()) {
      config.graphics = bo2zm::native::CreateGraphicsSystem();
    }
  }

  // Mode switches (MP <-> Zombies) relaunch the title; see bo2zm_relaunch.cpp.
  void OnPostSetup() override {
    if (!bo2zm::native::Active()) {
      bo2zm::perf_log::InstallSwapHook(rex::cvar::GetFlagByName("gpu_plugin") == "metal" ? "metal" : "xenos");
    }
    bo2zm::name::ApplySaved();  // before the game signs in
    bo2zm::pc_profiler::Start();  // bo2zm_pc_profile (PC test helper; off by default)
    bo2zm::test_waits::RunIfRequested();  // bo2zm_test_waits, off by default
    bo2zm::ApplyPendingRelaunch(user_data_root());
    bo2zm::scripts::Boot();
    bo2zm::gsc::Configure();
    // PNG captures of the game image (bo2zm_capture_every_s, off by default), to
    // compare the native renderer with Xenos emulation without watching the screen.
    bo2zm::capture::Boot([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
  }

  // The game waits while the native renderer compiles the pipelines that earlier runs used and that this build,
  // driver or shader pack has not compiled yet, behind its progress screen (native/render/bo2zm_native_precompile.h).
  bool OnHoldModuleLaunch(std::function<void()> start) override {
    return bo2zm::native::precompile::Hold(std::move(start), imgui_drawer());
  }

  void OnShutdown() override {
    bo2zm::perf_log::Close();  // the "# end" line; a run stopped by a signal ends without it, which the readers allow
    bo2zm::capture::Stop();
    bo2zm::lifecycle::Stop();
    bo2zm::updates::Stop();
    bo2zm::online::Stop();
  }

  // Without command-line paths (always the case on the Switch, where the NRO
  // is started from hbmenu or a forwarder), use folders next to the
  // executable:
  //   sdmc:/switch/bo2zm/bo2zm.nro
  //   sdmc:/switch/bo2zm/game/     the disc files + default_mp.xexp (TU18)
  //   sdmc:/switch/bo2zm/update/   the rest of the TU18 package
  //   sdmc:/switch/bo2zm/user/     profile, saves, caches
  // Command-line --game_data_root / --update_data_root still win (as on the PC).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    auto dir = rex::filesystem::GetExecutableFolder();
#if REX_PLATFORM_SWITCH
    // Emulators (Ryujinx) may run the NRO from a temporary copy, away from
    // its folder: fall back to the standard install folder.
    if (!std::filesystem::is_directory(dir / "game", ec) &&
        std::filesystem::is_directory("sdmc:/switch/bo2zm/game", ec)) {
      dir = "sdmc:/switch/bo2zm";
      paths.config_path = dir / "bo2zm.toml";
    }
#endif
    if (dir.empty()) {
      return;
    }
    if (paths.game_data_root.empty() && std::filesystem::is_directory(dir / "game", ec)) {
      paths.game_data_root = dir / "game";
    }
    if (paths.update_data_root.empty() && std::filesystem::is_directory(dir / "update", ec)) {
      paths.update_data_root = dir / "update";
    }
#if REX_PLATFORM_SWITCH
    paths.user_data_root = dir / "user";
    paths.cache_root = dir / "user" / "cache";
#else
    // A portable PC install keeps its saves, settings and DLC in user/ next to the program when that folder exists
    // and no --user_data_root was given, so a plain shortcut to the exe (which carries the icon) is enough.
    if (std::filesystem::is_directory(dir / "user", ec) &&
        paths.user_data_root == rex::filesystem::GetUserFolder() / GetName()) {
      if (paths.cache_root == paths.user_data_root / "cache") {
        paths.cache_root = dir / "user" / "cache";
      }
      paths.user_data_root = dir / "user";
    }
#endif
  }

  // The log's level and the profiler's report, from the toml and the saved DEBUG, before logging starts (the quiet
  // log: bo2zm_debug_options.cpp).
  void OnConfigLoaded() override { bo2zm::debug::ConfigureStartup(user_data_root()); }

  // Clocks and the overlay FPS block, once the toml has been read (Switch; nothing on the PC), then the quiet log's
  // note and the pruning of old logs.
  void OnPostInitLogging() override {
    bo2zm::settings::RequestBootClocks();
    bo2zm::debug::AfterLoggingStarted();
  }

  // Override virtual hooks for customization:
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
