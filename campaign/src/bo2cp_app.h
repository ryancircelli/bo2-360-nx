// bo2cp - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "../../core/bo2zm_output_volume.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <system_error>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

#if BO2CP_NATIVE
#include "native/bo2zm_native_system.h"
#include "native/render/bo2zm_native_capture.h"
#include "native/render/bo2zm_native_precompile.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_name.h"
#include "bo2zm_script_manager.h"
#include "bo2zm_switch.h"
#include "bo2zm_test_pad.h"
#endif

REXCVAR_DECLARE(int32_t, bo2cp_volume);

namespace bo2cp {
void ApplyPendingRelaunch(const std::filesystem::path& user_dir);  // bo2cp_relaunch.cpp
void StartWatchdog(const std::filesystem::path& logs_dir);        // bo2cp_relaunch.cpp
}

class Bo2cpApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo2cpApp>(new Bo2cpApp(ctx, "bo2cp",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
#if BO2CP_NATIVE
  // The log's level and the profiler's report from the toml and the saved DEBUG page, before logging starts, then the
  // quiet log's note and the pruning of old logs (bo2zm_debug_options.cpp), as in bo2zm.
  void OnConfigLoaded() override { bo2zm::debug::ConfigureStartup(user_data_root()); }
  void OnPostInitLogging() override { bo2zm::debug::AfterLoggingStarted(); }
#endif
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if !REX_PLATFORM_SWITCH
    // Preserve the test runner's volume across a handover from the launcher.
    if (const char* volume = std::getenv("BO2_PC_TEST_VOLUME");
        volume && rex::cvar::GetFlagSource("bo2cp_volume") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("bo2cp_volume", volume);
    }
#endif
    // The emulated GPU unless the toml or the command line names another; bo2zm_renderer = "native" replaces it with
    // the Zombies program's native renderer (built in with BO2CP_NATIVE).
    if (rex::cvar::GetFlagByName("gpu_plugin").empty()) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
#if BO2CP_NATIVE
    if (bo2zm::native::Active()) {
      // The lighting volume's writer is not hooked in the campaign (bo2zm_volume_writes.cpp): the renderer hashes the
      // volume instead. The other write-stamp caches have their stampers (bo2zm_crt.cpp, the skinning hooks).
      rex::cvar::SetFlagByName("bo2zm_native_volume_by_writes", "false");
      config.graphics = bo2zm::native::CreateGraphicsSystem();
    }
    bo2zm::test_pad::WrapInput(config);  // test runs only (bo2zm_test_buttons)
#endif
#if BO2CP_NATIVE
    // The Switch's services and thread setup (sockets, the fourth core, the audio threads), as in bo2zm.
    bo2zm::InitSwitchServices();
    bo2zm::name::ApplySaved();  // EDIT USERNAME's name, before the game signs in
#elif REX_PLATFORM_SWITCH
    // The game's network code (system link) needs the socket service, as in bo2zm (bo2zm_switch.cpp).
    if (R_FAILED(socketInitializeDefault())) {
      REXLOG_ERROR("socketInitializeDefault failed");
    }
#endif
    const int volume = bo2zm::audio::OutputVolumePercent(REXCVAR_GET(bo2cp_volume));
    rex::audio::SetOutputGain(float(volume) / 100.0f);
    REXLOG_INFO("sound output volume {} %", volume);
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // A hand-over from the Zombies program (bo2cp_relaunch.cpp).
  void OnPostSetup() override {
    bo2cp::ApplyPendingRelaunch(user_data_root());
#if BO2CP_NATIVE
    bo2zm::scripts::Boot();
#endif
    // Test loops (Switch): the reason goes to logs/ beside the NRO, with the Zombies program's logs.
    bo2cp::StartWatchdog(rex::filesystem::GetExecutableFolder() / "logs");
#if BO2CP_NATIVE
    // PNG captures of the game image (bo2zm_capture_every_s, off by default), as in the Zombies program.
    bo2zm::capture::Boot([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
#endif
  }

#if BO2CP_NATIVE
  void OnShutdown() override { bo2zm::capture::Stop(); }
#endif
#if BO2CP_NATIVE
  // The pipelines earlier runs used are compiled before the game starts, as in the Zombies program.
  bool OnHoldModuleLaunch(std::function<void()> start) override {
    return bo2zm::native::precompile::Hold(std::move(start), imgui_drawer());
  }
#endif
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;

  // Without command-line paths (always the case on the Switch), the game files come from game/ and update/ next to
  // the executable, else from a Zombies install beside it (../bo2zm). On the Switch bo2cp.nro sits next to bo2zm.nro
  // in sdmc:/switch/bo2zm, so the two programs share the disc files and user/ (one profile, the 360's shared saves,
  // and the pending relaunch file they hand each other); logs go to logs/ there too. Command-line --game_data_root
  // and --update_data_root still win (as on the PC).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) {
      return;
    }
    auto data = dir;
    if (!std::filesystem::is_directory(data / "game", ec)) {
      data = dir.parent_path() / "bo2zm";
    }
    if (paths.game_data_root.empty() && std::filesystem::is_directory(data / "game", ec)) {
      paths.game_data_root = data / "game";
    }
    if (paths.update_data_root.empty() && std::filesystem::is_directory(data / "update", ec)) {
      paths.update_data_root = data / "update";
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
};
