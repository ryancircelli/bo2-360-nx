// bo2zm - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_benchmark.h"
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
#include "native/render/bo2zm_native_video.h"
#include "native/render/bo2zm_native_precompile.h"

namespace {

std::optional<int> Bo2InstanceIndex() {
  const char* value = std::getenv("BO2_INSTANCE");
  if (!value) {
    return std::nullopt;
  }
  const std::string text(value);
  int index = -1;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), index);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || index < 0 || index > 15) {
    throw std::runtime_error("BO2_INSTANCE must be an integer from 0 to 15");
  }
  return index;
}

std::optional<int> Bo2InstanceCount() {
  const char* value = std::getenv("BO2_INSTANCE_COUNT");
  if (!value) {
    return std::nullopt;
  }
  const std::string text(value);
  int count = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || count < 2 || count > 4) {
    throw std::runtime_error("BO2_INSTANCE_COUNT must be an integer from 2 to 4");
  }
  return count;
}

void ConfigureInstancePaths(rex::PathConfig& paths) {
  const auto instance = Bo2InstanceIndex();
  if (!instance) {
    return;
  }
  const auto default_config_path = paths.config_path;
  const auto previous_user_root = paths.user_data_root;
  const auto isolated = std::filesystem::path("instances") / std::to_string(*instance);
  paths.user_data_root /= isolated;
  if (paths.cache_root == previous_user_root / "cache") {
    paths.cache_root = paths.user_data_root / "cache";
  } else {
    paths.cache_root /= isolated;
  }
  paths.config_path = paths.user_data_root / "bo2zm.toml";

  // Seed a new instance with the app's bundled defaults, then let its config
  // evolve independently. Without this copy, per-instance configs would skip
  // renderer and other required launch settings on the first run.
  std::error_code ec;
  if (!std::filesystem::exists(paths.config_path, ec) && !ec &&
      std::filesystem::exists(default_config_path, ec) && !ec) {
    std::filesystem::create_directories(paths.config_path.parent_path(), ec);
    if (ec) {
      throw std::runtime_error("Could not create the BO2 instance settings directory: " + ec.message());
    }
    std::filesystem::copy_file(default_config_path, paths.config_path,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
      throw std::runtime_error("Could not seed BO2 instance settings: " + ec.message());
    }
  }
}

}  // namespace

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
    if (const auto instance = Bo2InstanceIndex()) {
      rex::cvar::SetFlagByName("net_system_port_offset", std::to_string(30000 + 16 * *instance));
    }
    if (const auto count = Bo2InstanceCount()) {
      const auto instance = Bo2InstanceIndex();
      if (!instance || *instance >= *count) {
        throw std::runtime_error("BO2_INSTANCE must be less than BO2_INSTANCE_COUNT");
      }
      // WindowSDL tiles this instance as it creates its own window. Disable
      // the default fullscreen mode so SDL can size and place the window.
      rex::cvar::SetFlagByName("fullscreen", "false");
    }
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
    bo2zm::benchmark::InitializeFactors();
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
    auto get_presenter = [this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    };
    bo2zm::capture::Boot(get_presenter);
    // Raw test video (bo2zm_video_rgba, off by default) for tools/mac/boot_test.sh --video.
    bo2zm::video::Boot(get_presenter);
  }

  // The game waits while the native renderer compiles the pipelines that earlier runs used and that this build,
  // driver or shader pack has not compiled yet, behind its progress screen (native/render/bo2zm_native_precompile.h).
  bool OnHoldModuleLaunch(std::function<void()> start) override {
    return bo2zm::native::precompile::Hold(std::move(start), imgui_drawer());
  }

  void OnShutdown() override {
    bo2zm::perf_log::Close();  // the "# end" line; a run stopped by a signal ends without it, which the readers allow
    bo2zm::video::Stop();
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
    if (dir.empty()) {
      ConfigureInstancePaths(paths);
      return;
    }
#if REX_PLATFORM_SWITCH
    // Emulators (Ryujinx) may run the NRO from a temporary copy, away from
    // its folder: fall back to the standard install folder.
    if (!std::filesystem::is_directory(dir / "game", ec) &&
        std::filesystem::is_directory("sdmc:/switch/bo2zm/game", ec)) {
      dir = "sdmc:/switch/bo2zm";
      paths.config_path = dir / "bo2zm.toml";
    }
#endif
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
    ConfigureInstancePaths(paths);
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
