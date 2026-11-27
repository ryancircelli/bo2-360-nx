/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation - compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <cstdlib>
#include <stdexcept>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/instance_env.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/guide.h>
#include <rex/ui/overlay/guide_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/downmix.h>
#if REX_PLATFORM_SWITCH
#include <rex/audio/switch/switch_audio_system.h>
#else
#include <rex/audio/sdl/sdl_audio_system.h>
#endif
#include <rex/input/input_record.h>
#include <rex/input/input_system.h>
#include <rex/system/savepoint.h>
#include <rex/input/script/kbm_script.h>
#include <rex/kernel/init.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/text_input.h>
#include <rex/string/numeric.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/savepoint.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#if REX_PLATFORM_SWITCH
#include <cstdio>
#include <rex/ui/switch_imgui_input.h>
// Only the syscall header: switch.h drags typedefs that collide with rex.
extern "C" {
#include <switch/types.h>
// libnx runtime internals used by the Switch exit in OnClosing.
extern u32 __nx_applet_exit_mode;
NX_NORETURN void __libnx_exit(int rc);
}
#endif

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <filesystem>
#include <string_view>

REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "GPU emulation plugin to load at startup (e.g. 'xenos'); empty disables "
                      "GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(glitch_savepoint, false, "UI",
                    "Report a glitch also writes a savepoint (all guest memory, hundreds of MB) next to the report");

REXCVAR_DEFINE_INT32(guest_capture_every_s, 0, "UI",
                     "Write the guest output as captures/guest-NNNN.png next to the log every N seconds (0 = off); for hidden test runs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {

namespace {
std::filesystem::path GlitchRoot();
std::filesystem::path GlitchPending();

// Copy i of a game sharing this machine (REX_INSTANCE) keeps its own profile, console id, saves, settings and caches
// under <user>/instances/<i>, so copies never fight over a file. Its settings start as a copy of the main ones.
void IsolateInstancePaths(PathConfig& paths, int instance) {
  const auto own = std::filesystem::path("instances") / std::to_string(instance);
  const auto user = paths.user_data_root;
  paths.user_data_root /= own;
  paths.cache_root = paths.cache_root == user / "cache" ? paths.user_data_root / "cache" : paths.cache_root / own;
  const auto main_config = paths.config_path;
  paths.config_path = paths.user_data_root / main_config.filename();
  std::error_code ec;
  std::filesystem::create_directories(paths.user_data_root, ec);
  if (ec) throw std::runtime_error("Could not create the instance folder: " + ec.message());
  if (!std::filesystem::exists(paths.config_path, ec) && std::filesystem::exists(main_config, ec)) {
    std::filesystem::copy_file(main_config, paths.config_path, ec);
    if (ec) throw std::runtime_error("Could not seed the instance settings: " + ec.message());
  }
}
}  // namespace


// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::unique_ptr<ui::ImGuiDialog> ReXApp::CreateAchievementsOverlay() {
  if (!runtime_ || !runtime_->kernel_state() || !imgui_drawer_ || !immediate_drawer_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementsOverlayDialog>(
      imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), &achievements());
}

std::unique_ptr<ui::AchievementNotificationDialog> ReXApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer_ || !immediate_drawer_ || !runtime_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementToastDialog>(imgui_drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
}

system::AchievementManager& ReXApp::achievements() const {
  assert_not_null(runtime_);
  assert_not_null(runtime_->kernel_state());
  return runtime_->kernel_state()->achievements();
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_root_cvar = REXCVAR_GET(cache_root);
  if (!cache_root_cvar.empty()) {
    cache_dir = cache_root_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  std::filesystem::path metadata_dir;
  std::string metadata_root_cvar = REXCVAR_GET(metadata_root);
  if (!metadata_root_cvar.empty()) {
    metadata_dir = metadata_root_cvar;
  }

  // Settings file: cvar override (an app bundle keeps it in the user's support folder,
  // since the bundle beside the executable is signed), or <name>.toml beside the executable.
  std::filesystem::path config_path = exe_dir / (std::string(GetName()) + ".toml");
  std::string config_file_cvar = REXCVAR_GET(config_file);
  if (!config_file_cvar.empty()) {
    config_path = config_file_cvar;
  }

  PathConfig path_config{game_dir,  user_dir,     update_dir,
                         cache_dir, metadata_dir, config_path};
  OnConfigurePaths(path_config);
  if (const int instance = rex::InstanceIndex(); instance >= 0) {
    IsolateInstancePaths(path_config, instance);
  } else if (rex::InstanceEnv()) {
    throw std::runtime_error("REX_INSTANCE must be an integer from 0 to 15");
  }
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  metadata_root_ = path_config.metadata_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Load config FIRST so log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);
  OnConfigLoaded();
  if (rex::InstanceCountEnv()) {
    const int count = rex::InstanceCount();
    if (count < 0 || rex::InstanceIndex() < 0 || rex::InstanceIndex() >= count) {
      throw std::runtime_error("REX_INSTANCE_COUNT must be 2 to 8 and REX_INSTANCE below it");
    }
    rex::cvar::SetFlagByName("fullscreen", "false");  // the copies share the display as a tiled grid
  }

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  // log_level "off" with no log_file: no log file at all (an empty one would still be made at every start).
  if (log_file_cvar.empty() && log_level_str != "off") {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  // Arm the GPU frame recorder for "Report a glitch": it records the next frame when the trigger file appears,
  // and does nothing (one stat per frame) until then. An explicit REX_GPU_TRACE_* setting wins.
  if (!std::getenv("REX_GPU_TRACE_TRIGGER") && !std::getenv("REX_GPU_TRACE_FRAME")) {
    const auto pending = GlitchPending();
    std::error_code ec;
    std::filesystem::create_directories(pending, ec);
    std::filesystem::remove(pending / "trigger", ec);  // left by a crash, it would record frame 1
    setenv("REX_GPU_TRACE_TRIGGER", (pending / "trigger").c_str(), 0);
    setenv("REX_GPU_TRACE_DIR", pending.c_str(), 0);
  }

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_DEBUG("Loaded config: {}", config_path_.filename().string());

  REXLOG_DEBUG("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_DEBUG("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_DEBUG("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_DEBUG("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_DEBUG("  Cache root:     {}", cache_root_.string());
  if (!metadata_root_.empty()) {
    REXLOG_DEBUG("  Metadata root:  {}", metadata_root_.string());
  }

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = paths.game_data_root;
  user_data_root_ = paths.user_data_root;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;
  metadata_root_ = paths.metadata_root;

  runtime_ =
      std::make_unique<rex::Runtime>(paths.game_data_root, paths.user_data_root,
                                     paths.update_data_root, paths.cache_root, paths.metadata_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (runtime_->graphics_system()) {
    rex::input::SetFrameSource([](void* gfx) { return static_cast<system::IGraphicsSystem*>(gfx)->swap_count(); },
                               runtime_->graphics_system());
  }

  if (guide_gate_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->SetGuestGate(guide_gate_->MakeGate());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        if (rex::kernel::xam::TextInputOpen()) {
          return false;  // the app's text input has the keyboard and the controller
        }
        if (!debug_overlay_ && !console_overlay_ && !settings_overlay_ && !achievements_overlay_)
          return true;
#if REX_PLATFORM_SWITCH
        // The menu and the game share the controller: while a menu window
        // has navigation focus, the buttons belong to the menu.
        return rex::ui::nx::GameReceivesController(imgui_drawer_->GetIO());
#else
        return !imgui_drawer_->GetIO().WantCaptureMouse;
#endif
      });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  // Code generated from another executable or title update would run against the wrong data silently.
  if (auto* xex = runtime_->kernel_state()->GetExecutableModule()->xex_module();
      xex->base_address() != ppc_info_.image_base || xex->image_size() != ppc_info_.image_size) {
    auto msg = fmt::format(
        "The game executable {} does not match this build (loaded image {:08X}+{:X}, built for "
        "{:08X}+{:X}). Check the game files and the title update.",
        xex_image, xex->base_address(), xex->image_size(), ppc_info_.image_base,
        ppc_info_.image_size);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
  config_.gpu_plugin = REXCVAR_GET(gpu_plugin);
#if REX_PLATFORM_SWITCH
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::nx::SwitchAudioSystem);
#else
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
#endif
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin);
    if (!config_.graphics) {
      // Fatal by design: no silent headless fallback.
      auto msg =
          fmt::format("Failed to load GPU plugin '{}'. See log for details.", config_.gpu_plugin);
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  // A fullscreen window takes over the screen (its own Space on macOS) whether or not it is activated, so a window
  // that starts minimized starts windowed.
  if (REXCVAR_GET(fullscreen) && !REXCVAR_GET(window_start_minimized)) {
    window_->SetFullscreen(true);
  }
  rex::cvar::RegisterChangeCallback("fullscreen", [this](std::string_view, std::string_view value) {
    if (window_) {
      window_->SetFullscreen(rex::string::from_string<bool>(value, false));
    }
  });
  // Game pages (a window mode item) read and set the window's real state through the Guide's window hooks.
  rex::ui::guide::SetWindowHooks([this] { return window_ ? window_->IsFullscreen() : REXCVAR_GET(fullscreen); },
                                 [this](bool on) {
                                   app_context().CallInUIThreadDeferred([this, on] {
                                     if (window_) window_->SetFullscreen(on);
                                   });
                                 });
  window_->Open();

  auto* graphics_system = config_.graphics.get();
  if (graphics_system && graphics_system->presenter()) {
    // SDK mode: the emulated-Xenos presenter drives the overlays.
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        SetupOverlays(presenter, immediate_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  } else if (!graphics_system) {
    // Detached mode: the app brings its own renderer and drives its own paint
    // loop. ReXApp owns the returned drawer via immediate_drawer_.
    immediate_drawer_ = OnCreateImmediateDrawer();
    if (immediate_drawer_) {
      SetupOverlays(/*presenter=*/nullptr, immediate_drawer_.get());
      // No window_->SetPresenter, no drawer SetPresenter: the app owns the
      // surface and the present cadence.
    }
  }

  return true;
}

void ReXApp::SetupOverlays(rex::ui::Presenter* presenter, rex::ui::ImmediateDrawer* drawer) {
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_.get(), 64,
      [this](ImFontAtlas* atlas) {
        OnConfigureFonts(atlas);
        rex::ui::LoadGuideFonts(atlas);
      },
      [this](ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
        OnConfigureStyle(imgui_style, ui_style);
      });
  // presenter is nullptr in detached mode; ImGuiDrawer tolerates that and the
  // gated eager font upload in SetImmediateDrawer is skipped (font uploads
  // lazily on the first Draw instead).
  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);
  rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
    if (debug_overlay_) {
      debug_overlay_.reset();
    } else {
      debug_overlay_ =
          std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(), frame_stats_provider_);
#if REX_PLATFORM_SWITCH
      // The position set with L+R and the right stick, stored next to the configuration file.
      debug_overlay_->SetPositionFile(config_path_.parent_path() / "debug_overlay.txt");
#endif
    }
  });
  rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
    if (console_overlay_) {
      console_overlay_.reset();
    } else {
      console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
    }
  });
  rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
    if (settings_overlay_) {
      settings_overlay_.reset();
    } else {
      settings_overlay_ = std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
    }
  });
  rex::ui::RegisterBind("bind_achievements", "F7", "Toggle achievements overlay", [this] {
    if (achievements_overlay_) {
      achievements_overlay_.reset();
    } else {
      achievements_overlay_ = CreateAchievementsOverlay();
    }
  });

  SetupGuide();
  // Tests: scripted keyboard and mouse, aimed at the Guide's own targets.
  rex::input::script::StartKbmScriptIfSet(
      [](std::string_view name, float rect[4]) { return ui::GuideTarget(name, rect); });

  OnCreateDialogs(imgui_drawer_.get());
}

void ReXApp::SetupGuide() {
  namespace guide = rex::ui::guide;
  guide_gate_ = std::make_unique<ui::GuideInputGate>([this](int user) {
    // Guest thread: only post the open to the UI thread.
    app_context().CallInUIThreadDeferred([this, user]() {
      if (!guide_overlay_ && !shutting_down_) {
        ToggleGuide(user);
      }
    });
  });
  // The gate goes on the input system once the runtime exists (OnInitialize, after runtime setup).
  // F1 or Home opens and closes it for player 1.
  rex::ui::RegisterBind("bind_guide", "F1", "Open or close the Guide", [this] { ToggleGuide(); });
  rex::ui::RegisterBind("bind_glitch", "F8", "Report a glitch (screenshot, GPU frame, logs, input)",
                        [this] { ReportGlitch(); });
  rex::ui::RegisterBind("bind_guide_alt", "Home", "Open or close the Guide", [this] { ToggleGuide(); });

  // ---- System ----
  guide::Page display;
  display.id = "rex.system.display";
  display.title = "Display";
  display.blade = std::string(guide::kBladeSystem);
  display.order = 0;
  display.description = "Full screen, vertical sync, render scale and the frame counter.";
  display.items = {
      // The window's real state, not the cvar: the cvar is the start-up choice, and a window that starts minimized
      // (test runs) or tiled starts windowed while the cvar still says full screen.
      guide::Toggle(
          "Full Screen", [this] { return window_ ? window_->IsFullscreen() : REXCVAR_GET(fullscreen); },
          [this](bool on) {
            rex::cvar::SetFlagByName("fullscreen", on ? "true" : "false");
            if (window_) window_->SetFullscreen(on);  // the cvar may already hold this value (no callback)
          },
          "Fill the screen, or play in a window."),
      guide::CvarToggle("vsync", "Vertical Sync", "Wait for the display before showing each frame."),
      guide::Cvar("resolution_scale", "Render Scale"),
      guide::Cvar("resolution", "Window Resolution"),
      guide::Cvar("internal_resolution", "Internal Resolution"),
      guide::Cvar("fps_limit", "Frame Limit"),
      guide::Cvar("bo2zm_show_fps", "Frame Counter"),
      guide::Cvar("show_fps", "Frame Counter"),
  };
  guide::RegisterPage(std::move(display));

  guide::Page support;
  support.id = "rex.system.support";
  support.title = "Report a Glitch";
  support.blade = std::string(guide::kBladeSystem);
  support.order = 20;
  support.description =
      "Saves a screenshot, the next GPU frame, the log, your settings, build ids, the last minute of controller "
      "input and a savepoint to a folder in the game's log folder (glitches), so a bug can be reproduced.";
  support.items = {
      guide::Action(
          "Report a Glitch", [this] { ReportGlitch(); }, "Capture everything needed to reproduce what is on screen."),
  };
  guide::RegisterPage(std::move(support));

  guide::Page audio;
  audio.id = "rex.system.audio";
  audio.title = "Audio";
  audio.blade = std::string(guide::kBladeSystem);
  audio.order = 10;
  audio.description = "Volume and mute.";
  audio.items = {
      // No "volume" setting exists; the slider drives the output gain every audio backend applies.
      guide::Slider(
          "Volume", 0, 100, 5, [] { return std::round(rex::audio::GetOutputGain() * 100.0f); },
          [](double v) { rex::audio::SetOutputGain(static_cast<float>(std::clamp(v, 0.0, 100.0) / 100.0)); },
          "Game volume.", "%.0f%%"),
      guide::CvarToggle("audio_mute", "Mute", "Silence all game audio."),
  };
  guide::RegisterPage(std::move(audio));

  guide::Page input;
  input.id = "rex.system.input";
  input.title = "Controller Settings";
  input.blade = std::string(guide::kBladeControllers);
  input.order = 10;
  input.description = "Vibration, the Guide button and which pad plays as which player.";
  input.items = {
      guide::CvarToggle("input_vibration", "Vibration", "Let the game rumble the controllers."),
      guide::CvarChoice("guide_button", {"on", "hold", "off"}, {"Opens Guide", "Hold to Open", "Off"},
                        "Guide Button"),
      guide::Cvar("input_player_slots", "Player Slots"),
  };
  guide::RegisterPage(std::move(input));

  // ---- Controllers ----
  guide::Page pads;
  pads.id = "rex.controllers";
  pads.title = "Connected Controllers";
  pads.blade = std::string(guide::kBladeControllers);
  pads.description = "Every connected controller and the player it plays as.";
  pads.build = [this]() {
    std::vector<guide::Item> items;
    auto* sys = static_cast<rex::input::InputSystem*>(runtime_ ? runtime_->input_system() : nullptr);
    if (!sys) return items;
    for (const auto& snap : sys->SnapshotDevices()) {
      if (snap.info.name == "None") continue;
      std::string name = snap.info.name.empty() ? std::string("Controller") : snap.info.name;
      if (snap.info.transport.size()) name += " (" + snap.info.transport + ")";
      const std::string player =
          snap.user >= 0 ? fmt::format("Player {}", snap.user + 1) : std::string("Not playing");
      std::string desc = fmt::format("{}\n{}", name, player);
      if (!snap.info.location.empty()) desc += "\nPort: " + snap.info.location;
      items.push_back(guide::Info(name, [player] { return player; }, desc));
    }
    if (items.empty()) {
      items.push_back(guide::Info("No controllers connected", [] { return std::string(); }));
    }
    return items;
  };
  guide::RegisterPage(std::move(pads));

  // ---- Leave Game ----
  guide::Page leave;
  leave.id = "rex.leave";
  leave.title = "Leave Game";
  leave.blade = std::string(guide::kBladeLeave);
  leave.description = "Quit the game and close the window.";
  leave.items = {
      guide::Action("Resume Game", [this] { CloseGuide(); }, "Close the Guide and go back to the game."),
      guide::Action(
          "Quit to Desktop",
          [this] {
            REXLOG_INFO("[guide] quit requested");
            app_context().CallInUIThreadDeferred([this] {
              CloseGuide();
              if (window_) window_->RequestClose();
            });
          },
          "Close the game. Unsaved progress is lost."),
  };
  guide::RegisterPage(std::move(leave));
}

namespace {

// ~/Library/Logs/<support folder>/glitches next to the log (or the user data folder without a log file).
std::filesystem::path GlitchRoot() {
  std::filesystem::path log = rex::CurrentLogFile();
  return (log.empty() ? rex::filesystem::GetUserFolder() : log.parent_path()) / "glitches";
}

// Per process: 4-player split screen runs one process per player against the same log folder.
// shortcut: folders of exited processes stay behind (empty or one small trace); prune by pid if they pile up.
std::filesystem::path GlitchPending() {
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  return GlitchRoot() / (".pending-" + std::to_string(pid));
}

uint32_t Crc32(uint32_t crc, const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool init = [] {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)init;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// shortcut: PNG with stored (uncompressed) deflate blocks, so a 1080p capture is ~8 MB; swap in zlib if size matters.
bool WritePng(const std::filesystem::path& path, const rex::ui::RawImage& img) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t(img.width) * 3 + 1) * img.height);
  for (uint32_t y = 0; y < img.height; y++) {
    raw.push_back(0);
    const uint8_t* row = img.data.data() + y * img.stride;
    for (uint32_t x = 0; x < img.width; x++) raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.insert(z.end(), {uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t((~n) >> 8)});
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (last) break;
  }
  uint32_t adler = (b << 16) | a;
  z.insert(z.end(), {uint8_t(adler >> 24), uint8_t(adler >> 16), uint8_t(adler >> 8), uint8_t(adler)});
  std::ofstream out(path, std::ios::binary);
  auto be32 = [&](uint32_t v) {
    const char c[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    out.write(c, 4);
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
    be32(uint32_t(d.size()));
    std::vector<uint8_t> t(type, type + 4);
    t.insert(t.end(), d.begin(), d.end());
    out.write(reinterpret_cast<const char*>(t.data()), t.size());
    be32(Crc32(0, t.data(), t.size()));
  };
  out.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr;
  for (uint32_t v : {img.width, img.height})
    ihdr.insert(ihdr.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  return bool(out);
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary) << text;
}

std::atomic<bool> g_glitch_busy{false};
std::atomic<bool> g_glitch_stop{false};  // OnDestroy: stop waiting for the GPU trace
constexpr size_t kGlitchReportsKept = 5;  // each holds a screenshot and a trace (tens of MB)

}  // namespace

void ReXApp::ReportGlitch() {
  if (g_glitch_busy.exchange(true)) return;
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
  const std::filesystem::path root = GlitchRoot();
  const std::filesystem::path pending = GlitchPending();
  std::error_code ec;
  std::filesystem::path dir = root / stamp;
  for (int n = 2; std::filesystem::exists(dir, ec); n++) dir = root / fmt::format("{}-{}", stamp, n);
  std::filesystem::create_directories(dir, ec);
  {
    // Keep only the newest reports; names sort by time (same-second reports get -2, -3 ...).
    std::vector<std::filesystem::path> reports;
    for (const auto& e : std::filesystem::directory_iterator(root, ec))
      if (e.is_directory(ec) && e.path().filename().string()[0] != '.') reports.push_back(e.path());
    std::sort(reports.begin(), reports.end());
    for (size_t i = 0; i + kGlitchReportsKept < reports.size(); i++) std::filesystem::remove_all(reports[i], ec);
  }

  auto* gfx = runtime_ ? runtime_->graphics_system() : nullptr;
  const uint64_t frame = gfx ? gfx->swap_count() : 0;
  std::string notes = fmt::format("glitch report {}\nname: {}\nbuild: {} {} ({})\nswap_count: {}\n", stamp, GetName(),
                                  REXGLUE_VERSION_STRING, REXGLUE_BUILD_TITLE, REXGLUE_BUILD_TIMESTAMP, frame);

  // Input first: it is the cheapest and the part that must not move on.
  WriteText(dir / "input.txt",
            rex::input::SamplesToText(rex::input::GlobalInputRing().Window(rex::input::ProcessMs(), 60000)));
  WriteText(dir / "settings.toml", rex::cvar::SerializeToTOML());

  // Arm the trace now, so it records the frame after this call; collected by the worker below.
  const bool trace_armed = std::getenv("REX_GPU_TRACE_TRIGGER") != nullptr;
  if (trace_armed) {
    for (const auto& e : std::filesystem::directory_iterator(pending, ec))
      if (e.path().extension() == ".xtr") std::filesystem::remove(e.path(), ec);
    WriteText(std::getenv("REX_GPU_TRACE_TRIGGER"), "1");
  }
  if (REXCVAR_GET(glitch_savepoint)) {
    rex::system::RequestSavepoint((dir / "savepoint.bin").string());
    notes += "savepoint: requested (savepoint.bin, written at the next frame boundary)\n";
  }
  rex::ui::Presenter* presenter = gfx ? gfx->presenter() : nullptr;

  // The rest can wait on frames, so it runs off the UI thread. The previous worker has finished (busy flag).
  if (glitch_thread_.joinable()) glitch_thread_.join();
  glitch_thread_ = std::thread([=, notes = std::move(notes)]() mutable {
    rex::ui::RawImage img;
    if (presenter && presenter->CaptureGuestOutput(img) && img.width && img.height && WritePng(dir / "screenshot.png", img)) {
      notes += fmt::format("screenshot: screenshot.png {}x{}\n", img.width, img.height);
    } else {
      notes += "screenshot: not captured\n";
    }
    rex::FlushLogging();
    std::string log;
    {
      std::ifstream in(rex::CurrentLogFile(), std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      log = ss.str();
    }
    size_t keep = log.size(), lines = 0;
    while (keep > 0 && lines < 5000) {
      keep = log.rfind('\n', keep - 1);
      if (keep == std::string::npos) { keep = 0; break; }
      lines++;
    }
    WriteText(dir / "log-tail.txt", log.substr(keep == 0 ? 0 : keep + 1));
    notes += "log: log-tail.txt (last 5000 lines)\n";

    if (trace_armed) {
      std::filesystem::path found;
      uintmax_t last_size = 0;
      for (int i = 0; i < 100 && found.empty() && !g_glitch_stop; i++) {  // up to 10 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::error_code e;
        for (const auto& f : std::filesystem::directory_iterator(pending, e)) {
          if (f.path().extension() != ".xtr") continue;
          auto size = std::filesystem::file_size(f.path(), e);
          // The recorder closes the file when the frame ends; a size that stops growing means it is done.
          if (size && size == last_size) found = f.path();
          last_size = size;
        }
      }
      if (!found.empty()) {
        std::filesystem::rename(found, dir / found.filename(), ec);
        notes += fmt::format("gpu trace: {}\n", found.filename().string());
      } else {
        std::filesystem::remove(std::getenv("REX_GPU_TRACE_TRIGGER"), ec);
        notes += "gpu trace: none (no frame was swapped within 10 s, or the renderer has no recorder)\n";
      }
    } else {
      notes += "gpu trace: off (REX_GPU_TRACE_* set by hand or the renderer has no recorder)\n";
    }
    WriteText(dir / "report.txt", notes);
    REXLOG_INFO("[glitch] report written to {}", dir.string());
    g_glitch_busy = false;
  });
}

void ReXApp::ToggleGuide(int user, std::string_view blade) {
  if (guide_overlay_) {
    CloseGuide();
    return;
  }
  if (!imgui_drawer_ || !guide_gate_) {
    return;
  }
  auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_ ? runtime_->input_system() : nullptr);
  ui::GuideDialog::Hooks hooks;
  if (input_sys) {
    hooks.snapshot = [input_sys] { return input_sys->SnapshotDevices(); };
  }
  hooks.request_close = [this] {
    app_context().CallInUIThreadDeferred([this] { CloseGuide(); });
  };
  hooks.take_over = [this](int new_user) {
    if (guide_gate_) guide_gate_->SetHolder(new_user);
  };
  // Opened by a player's Xbox button: only that pad drives it and the game
  // gets nothing from it; the other players keep playing (or not, as the game
  // decides on XN_SYS_UI). Opened from the keyboard or mouse: every pad is
  // held and any pad drives it.
  guide_gate_->SetHolder(user >= 0 ? user : ui::GuideInputGate::kAllPads);
  guide_overlay_ = std::make_unique<ui::GuideDialog>(imgui_drawer_.get(), user, std::move(hooks));
  // Tell the game the system UI is up, as the console does when its Guide opens.
  if (runtime_ && runtime_->kernel_state()) {
    rex::kernel::xam::xeXamSetGuideOpen(runtime_->kernel_state(), true);
    REXLOG_INFO("[guide] XN_SYS_UI true sent (system UI open)");
  }
  // The Guide is used with the mouse too: show the cursor (the keyboard and
  // mouse hold above has already asked the game's mouse capture to let go).
  if (window_) {
    guide_saved_cursor_ = static_cast<int>(window_->GetCursorVisibility());
    window_->SetCursorVisibility(ui::Window::CursorVisibility::kVisible);
  }
  if (!blade.empty()) {
    guide_overlay_->SelectBlade(blade);
  }
}

void ReXApp::CloseGuide() {
  if (!guide_overlay_) {
    return;
  }
  guide_overlay_.reset();
  if (window_ && guide_saved_cursor_ >= 0) {
    window_->SetCursorVisibility(static_cast<ui::Window::CursorVisibility>(guide_saved_cursor_));
    guide_saved_cursor_ = -1;
  }
  if (guide_gate_) {
    guide_gate_->SetHolder(-1);
  }
  if (runtime_ && runtime_->kernel_state()) {
    rex::kernel::xam::xeXamSetGuideOpen(runtime_->kernel_state(), false);
    REXLOG_INFO("[guide] XN_SYS_UI false sent (system UI closed)");
  }
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    // Register the achievement notification callback now that the runtime and
    // KernelState are guaranteed to exist. Done here (not OnCreateDialogs)
    // because KernelState is null during SetupPresentation.
    /*
     * The achievements notification is created when needed, not at startup.
     *
     * An ImGuiDialog registers itself in the ImGuiDrawer in its constructor and stays there for its
     * whole life. And as soon as the Presenter sees one UI drawer registered, it stops presenting from
     * the game thread and asks the UI thread to paint, which coalesces the requests: the mailbox ends
     * up discarding half of the frames with their GPU work already done.
     *
     * Measured on the console: this dialog (which draws nothing until an achievement pops) was created
     * 0.7 s after startup and dragged presentation down from ~18 to ~9 images per second for the whole
     * session. By creating it only when the first event arrives, the game presents every frame it
     * renders.
     */
    if (achievement_notification_listener_ == 0 && runtime_ && runtime_->kernel_state()) {
      achievement_notification_listener_ = achievements().RegisterNotificationCallback(
          [this](const rex::system::AchievementEvent& event) {
            rex::system::AchievementEvent copy = event;
            app_context().CallInUIThreadDeferred([this, copy]() {
              if (!achievement_notification_) {
                achievement_notification_ = std::shared_ptr<ui::AchievementNotificationDialog>(
                    CreateAchievementNotificationDialog());
              }
              if (achievement_notification_) {
                achievement_notification_->Push(copy);
              }
            });
          });
    }

    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system = runtime_->graphics_system();
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    if (int loaded = system::LoadSavepointAtLaunch()) {
      if (loaded < 0) {
        app_context().QuitFromUIThread();
        return;
      }
      // shortcut: stage 1 restores memory + GPU and shows the saved frame only; guest threads stay parked
      // until thread restore lands.
      (void)main_thread.release();
      return;
    }

    OnPostLaunchModule(main_thread.get());
    // The thread object lives on in module_thread_ below, which waits for it to end: a held start resumes it later,
    // on this (the UI) thread as before.
    system::XThread* const held = main_thread.get();
    if (!OnHoldModuleLaunch([this, held]() {
          app_context().CallInUIThreadDeferred([held]() { held->Resume(); });
        })) {
      main_thread->Resume();
    }

    if (const int every = REXCVAR_GET(guest_capture_every_s); every > 0 && !capture_thread_.joinable()) {
      capture_thread_ = std::thread([this, every]() {
        auto* gfx = runtime_ ? runtime_->graphics_system() : nullptr;
        const auto dir = GlitchRoot().parent_path() / "captures";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (int n = 0, waited = 0; !g_glitch_stop && !shutting_down_.load(std::memory_order_acquire);) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          if (++waited < every) continue;
          waited = 0;
          rex::ui::Presenter* presenter = gfx ? gfx->presenter() : nullptr;
          rex::ui::RawImage img;
          if (presenter && presenter->CaptureGuestOutput(img) && img.width && img.height)
            WritePng(dir / fmt::format("guest-{:04d}.png", n++), img);
        }
      });
    }

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        // The title ended by itself (it quit, or handed over to another executable): leave the way a
        // window close does. A full teardown here faulted forever in the input driver and then let the
        // frame-dump thread abort on the destroyed presenter (BO1, 2026-11-14).
        app_context().CallInUIThread([this]() {
          ui::UIEvent event(window_.get());
          OnClosing(event);
        });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnMouseDown(ui::MouseEvent& e) {
  // The mouse's thumb (back) button opens and closes the Guide, so the mouse
  // alone can reach it. Games never see that button.
  if (e.button() == ui::MouseEvent::Button::kX1) {
    ToggleGuide();
  }
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  // Nothing is torn down below, so the frame-dump thread is ended here: it writes the frames it reached
  // instead of dying mid-file with the process (or, before this, outliving a torn-down presenter).
  if (runtime_ && runtime_->graphics_system()) {
    runtime_->graphics_system()->PrepareForHardExit();
  }
  // Hard-exit rather than run subsystem teardown, which can deadlock on a host
  // lock still held by a straggler TerminateTitle left running. Flush (not
  // ShutdownLogging, which frees loggers a straggler may still use); the OS
  // reclaims the rest.
  REXLOG_INFO("Title terminated; hard-exiting process.");
  rex::FlushLogging();
#if REX_PLATFORM_SWITCH
  // std::_Exit returns the NRO to hbloader with every guest, GPU and audio
  // thread still alive, and hbmenu then crashes on heap pages those threads'
  // stacks keep aliased. The process must end, and an application has to ask
  // the system for it (svcExitProcess is reported as "closed because an error
  // occurred"): with __nx_applet_exit_mode = 1, libnx closes the services and
  // sends ISelfController::Exit. See windowed_app_main_switch.cpp.
  std::fflush(nullptr);
  __nx_applet_exit_mode = 1;
  __libnx_exit(0);
#else
  std::_Exit(0);
#endif
}

bool ReXApp::OnCloseRequested(ui::UIEvent& e) {
  (void)e;
  return OnWindowCloseRequested();
}

void ReXApp::OnResize(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnWindowPixelSizeChanged(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  OnWindowResized(window_->GetActualLogicalWidth(), window_->GetActualLogicalHeight());
}

void ReXApp::OnDpiChanged(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnDpiScaleChanged(float(window_->GetDpi()) / float(window_->GetMediumDpi()));
}

void ReXApp::OnGotFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(true);
}

void ReXApp::OnLostFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(false);
}

void ReXApp::OnMinimized(ui::UIEvent& e) {
  (void)e;
  OnWindowMinimized();
}

void ReXApp::OnRestored(ui::UIEvent& e) {
  (void)e;
  OnWindowRestored();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_achievements");
  rex::ui::UnregisterBind("bind_guide");
  rex::ui::UnregisterBind("bind_guide_alt");
  rex::ui::UnregisterBind("bind_glitch");
  rex::ui::guide::SetWindowHooks({}, {});

  // ImGui cleanup (reverse of setup)
  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  achievement_notification_.reset();
  achievements_overlay_.reset();
  guide_overlay_.reset();
  if (runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->SetGuestGate({});
  }
  guide_gate_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  // immediate_drawer_ was already unlinked from imgui_drawer_ above. Detach it
  // from its presenter so SDK mode runs OnLeavePresenter() before disposal; in
  // detached mode the drawer never had a presenter, so SetPresenter(nullptr) is
  // a no-op.
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  g_glitch_stop = true;
  if (capture_thread_.joinable()) capture_thread_.join();
  if (glitch_thread_.joinable()) {
    glitch_thread_.join();
  }
  // Retire the input drivers while the window they listen to still exists. A title that quits by
  // itself (XamLoaderLaunchTitle / TerminateTitle) never sends OnClosing, so the mouse-and-keyboard
  // driver would otherwise detach from the freed window inside runtime_.reset() and fault forever.
  // Runtime::Shutdown calls this again later; the second call finds no drivers.
  if (window_ && runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->Shutdown();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

}  // namespace rex
