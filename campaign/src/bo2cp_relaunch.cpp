// Title relaunch (XLaunchNewImage) for the recompiled campaign, the counterpart of games/bo2/src/bo2zm_relaunch.cpp.
//
// Choosing Zombies or Multiplayer, the campaign's sub_821F5FF8 writes the mode into a 1000-byte launch block
// (byte 860: 1 = Zombies, 0 = Multiplayer), stores it with XamLoaderSetLaunchData and calls sub_825C09D0
// ("default_mp.xex", 0), the wrapper of XamLoaderLaunchTitle (the same code as default_mp's sub_82274C38 and
// sub_827D36E8). On the Switch, with bo2zm.nro next to bo2cp.nro, the launch data goes into the pending file both
// programs share (user/relaunch_pending.bin, the same format) and the application restarts through the system
// (appletRestartProgram): its NRO, bo2zm.nro, takes the pending file, or hands it back to this one from its first
// constructor when it is for the campaign. Chain-loading straight from the running game is only the fallback: it
// leaves the game's threads in hbloader's process (games/bo2/src/bo2zm_relaunch.cpp). Coming from Zombies, the pending file
// is handed to XAM at start, as after a console title reboot.
//
// Only installed modes are started (games/bo2/src/bo2zm_launch_options.h, InstalledModes): a request for Zombies or
// Multiplayer when that mode's files or bo2zm are missing is dropped and the campaign goes on, and a campaign started
// without its own files hands over to bo2zm when that has an installed mode.

#include "bo2cp_relaunch.h"
#include "../../games/bo2/src/bo2zm_launch_options.h"
#include "../../games/bo2/src/bo2zm_relaunch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/hook.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if BO2CP_NATIVE
#include "bo2zm_benchmark.h"
#include "native/bo2zm_native_system.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#elif REX_PLATFORM_GNU_LINUX
#include <unistd.h>
#endif

#if defined(__SWITCH__)
#include <switch.h>
extern "C" void RexSwitchSetRestartOnCrash(bool on);  // the SDK's switch_crash_hooks.c
extern "C" int __system_argc;
extern "C" char** __system_argv;
#endif

namespace fs = std::filesystem;

REXCVAR_DEFINE_INT32(bo2cp_watchdog_s, 0, "BO2",
                     "Test consoles (Switch, from a forwarder): when the game swaps no frame for this many seconds "
                     "while in focus, the application restarts (appletRestartProgram); bo2zm.nro then sends the logs "
                     "and, with bo2zm_test_campaign, starts the campaign again. 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2cp_restart_on_crash, false, "BO2",
                    "Test consoles (Switch, from a forwarder): a crash restarts the application once its report is "
                    "written (logs/rex/rex_crash.log) instead of stopping on the error screen, so a test loop goes on. "
                    "false = the error screen")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2cp_test_seconds, 0, "BO2",
                     "Test consoles (Switch, from a forwarder): the application restarts this many seconds after the "
                     "campaign started, frozen or not, so a test loop always comes back. 0 = off")
    .range(0, 86400)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kMagic = 0x42325A4C;  // "B2ZL", as bo2zm_relaunch.cpp
constexpr const char* kPendingFile = "relaunch_pending.bin";

fs::path g_user_dir;
std::atomic<bool> g_restart_requested{false};
rex::kernel::xam::XamModule::LoaderData g_startup_launch;
bool g_launch_ready = false;

rex::kernel::xam::XamModule::LoaderData* LoaderData() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->kernel_state()) {
    return nullptr;
  }
  auto xam = runtime->kernel_state()->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  return xam ? &xam->loader_data() : nullptr;
}

bool WriteBlob(const fs::path& path, const rex::kernel::xam::XamModule::LoaderData& ld) {
  std::ofstream f(path, std::ios::binary);
  const uint32_t path_len = uint32_t(ld.launch_path.size());
  const uint32_t data_len = ld.launch_data_present ? uint32_t(ld.launch_data.size()) : 0;
  f.write(reinterpret_cast<const char*>(&kMagic), 4);
  f.write(reinterpret_cast<const char*>(&ld.launch_flags), 4);
  f.write(reinterpret_cast<const char*>(&path_len), 4);
  f.write(ld.launch_path.data(), path_len);
  f.write(reinterpret_cast<const char*>(&data_len), 4);
  f.write(reinterpret_cast<const char*>(ld.launch_data.data()), data_len);
  f.close();
  return !f.fail();
}

bool ReadBlob(const fs::path& path, uint32_t& flags, std::string& launch_path,
              std::vector<uint8_t>& data) {
  // Stdio is also safe in the early handover constructor, before iostream setup.
  FILE* file = std::fopen(path.string().c_str(), "rb");
  if (!file) {
    return false;
  }
  uint32_t magic = 0, path_len = 0, data_len = 0;
  bool ok = std::fread(&magic, 4, 1, file) == 1 && magic == kMagic &&
            std::fread(&flags, 4, 1, file) == 1 && std::fread(&path_len, 4, 1, file) == 1 && path_len <= 4096;
  if (ok) {
    launch_path.resize(path_len);
    ok = std::fread(launch_path.data(), 1, path_len, file) == path_len &&
         std::fread(&data_len, 4, 1, file) == 1 && data_len <= (1u << 20);
  }
  if (ok) {
    data.resize(data_len);
    ok = std::fread(data.data(), 1, data_len, file) == data_len;
  }
  std::fclose(file);
  return ok;
}

std::string HexPreview(const std::vector<uint8_t>& data, size_t max_bytes = 64) {
  std::string out;
  for (size_t i = 0; i < data.size() && i < max_bytes; i++) {
    out += fmt::format("{:02X}", data[i]);
  }
  return data.size() > max_bytes ? out + "..." : out;
}

bool ProgramAvailable(const char* name) {
  fs::path directory;
#if defined(__SWITCH__)
  if (__system_argc < 1 || !__system_argv || !__system_argv[0]) {
    return false;
  }
  directory = fs::path(__system_argv[0]).parent_path();
  const fs::path filename = name;
#elif defined(_WIN32)
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!length || length >= MAX_PATH) {
    return false;
  }
  directory = fs::path(exe).parent_path();
  fs::path filename = name;
  filename.replace_extension(".exe");
#elif REX_PLATFORM_GNU_LINUX
  std::error_code error;
  directory = fs::read_symlink("/proc/self/exe", error).parent_path();
  if (error) {
    return false;
  }
  const fs::path filename = fs::path(name).stem();
#else
  (void)name;
  return false;
#endif
#if defined(__SWITCH__) || defined(_WIN32) || REX_PLATFORM_GNU_LINUX
  std::error_code ec;
  return fs::is_regular_file(directory / filename, ec);
#endif
}

// The modes installed beside this program, read once the runtime knows the game folder.
const bo2zm::launch::InstalledModes& Installed() {
  static const bo2zm::launch::InstalledModes installed = [] {
    fs::path game;
    if (const auto* runtime = rex::Runtime::instance()) {
      game = runtime->game_data_root();
    }
    const auto modes = bo2zm::launch::DetectInstalledModes(game, true, ProgramAvailable("bo2zm.nro"));
    REXLOG_INFO("[relaunch] installed modes: {} (game files: {})", modes.Text().empty() ? "none" : modes.Text(),
                game.string());
    return modes;
  }();
  return installed;
}

// Restarts through the system on Switch; PC programs use a companion beside this executable.
[[noreturn]] void StartAndExit(const char* name) {
#if defined(__SWITCH__)
  rex::FlushLogging();
  if (R_SUCCEEDED(appletRestartProgram(nullptr, 0))) {
    REXLOG_INFO("[relaunch] restarting the application for {}", name);
    rex::FlushLogging();
    for (int i = 0; i < 120; ++i) {
      svcSleepThread(250'000'000);
    }
    REXLOG_WARN("[relaunch] still running 30 s after the restart request: chain-loading instead");
  } else {
    REXLOG_WARN("[relaunch] the system refused the restart (started from hbmenu?): chain-loading instead");
  }
  if (envHasNextLoad() && __system_argc > 0 && __system_argv[0]) {
    const std::string next = (fs::path(__system_argv[0]).parent_path() / name).string();
    const Result rc = envSetNextLoad(next.c_str(), next.c_str());
    REXLOG_INFO("[relaunch] envSetNextLoad('{}') -> {:#x}", next, rc);
  } else {
    REXLOG_ERROR("[relaunch] the loader does not support chaining NROs; exiting");
  }
#elif defined(_WIN32)
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length && length < MAX_PATH) {
    fs::path next = fs::path(exe).parent_path() / name;
    next.replace_extension(".exe");
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(next.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process)) {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    } else {
      REXLOG_ERROR("[relaunch] CreateProcessW failed: {}", GetLastError());
    }
  }
#elif REX_PLATFORM_GNU_LINUX
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    const fs::path next = exe.parent_path() / fs::path(name).stem();
    std::vector<std::string> arguments;
    if (FILE* command = std::fopen("/proc/self/cmdline", "rb")) {
      std::string argument;
      for (int c; (c = std::fgetc(command)) != EOF;) {
        if (c == '\0') {
          arguments.push_back(argument);
          argument.clear();
        } else {
          argument += static_cast<char>(c);
        }
      }
      std::fclose(command);
    }
    if (!arguments.empty()) {
      arguments[0] = next.string();
      std::vector<char*> argv;
      for (auto& argument : arguments) {
        argv.push_back(argument.data());
      }
      argv.push_back(nullptr);
      rex::FlushLogging();
      close_range(3, ~0u, 0);
      execv(next.c_str(), argv.data());
      std::perror("bo2cp: relaunch failed");
      std::_Exit(1);
    }
  }
#else
  REXLOG_ERROR("[relaunch] starting {} is only done on the Switch; exiting", name);
#endif
  rex::FlushLogging();
  // Teardown deadlocks on guest threads that TerminateTitle left running: leave without it.
  std::_Exit(0);
}

#if defined(__SWITCH__)
// The first thing this process does: a pending relaunch for Zombies or Multiplayer chain-loads bo2zm.nro, while the
// main thread is the only one (as bo2zm_relaunch.cpp's HandOverEarly does the other way).
__attribute__((constructor(101))) void HandOverEarly() {
  if (__system_argc < 1 || !__system_argv || !__system_argv[0] || !envHasNextLoad()) {
    return;
  }
  const fs::path dir = fs::path(__system_argv[0]).parent_path();
  uint32_t flags = 0;
  std::string launch_path;
  std::vector<uint8_t> data;
  const fs::path pending = bo2zm::launch::SharedUserRoot() / kPendingFile;
  if (!ReadBlob(pending, flags, launch_path, data) || !launch_path.ends_with("default_mp.xex")) {
    return;
  }
  const std::string zombies = (dir / "bo2zm.nro").string();
  bool main_program = false;
  if (FILE* f = std::fopen(zombies.c_str(), "rb")) {
    std::fclose(f);
    main_program = true;
  }
  const auto installed = bo2zm::launch::DetectInstalledModes(dir / "game", true, main_program);
  if (!installed.Has(bo2zm::launch::LaunchMode(launch_path, data))) {
    // That mode is not installed: drop the request and start the campaign.
    std::remove(pending.string().c_str());
    return;
  }
  if (R_SUCCEEDED(envSetNextLoad(zombies.c_str(), zombies.c_str()))) {
    std::_Exit(0);
  }
}
#endif

#if defined(__SWITCH__)
// bo2cp_watchdog_s and bo2cp_test_seconds: a libnx thread at the priority of the Zombies program's benchmark
// watchdog, above the game's threads. Nothing here logs or takes a lock while the game may be frozen
// (holding the log's lock): the reason goes into logs/bo2cp_watchdog.txt, which the next Zombies start sends.
struct Watchdog {
  Thread thread{};
  const rex::graphics::CommandProcessor* gpu = nullptr;
  int frozen_s = 0;
  int total_s = 0;
  std::string marker;
};
Watchdog g_watchdog;

// Frames swapped so far, by whichever renderer runs.
uint64_t SwapsSoFar() {
#if BO2CP_NATIVE
  if (bo2zm::native::Active()) {
    return bo2zm::benchmark::FramesSeen();  // the frames the renderer presented (bo2cp_native_stubs.cpp)
  }
#endif
  return g_watchdog.gpu ? g_watchdog.gpu->swap_count() : 0;
}

void Note(const char* text) {
  if (FILE* f = std::fopen(g_watchdog.marker.c_str(), "ab")) {
    std::fputs(text, f);
    std::fclose(f);
  }
}

void WatchdogMain(void*) {
  uint64_t frames = 0;
  int still = 0;
  for (int seconds = 1;; ++seconds) {
    svcSleepThread(1'000'000'000LL);
    const uint64_t now = SwapsSoFar();
    char text[160];
    if (g_watchdog.total_s > 0 && seconds >= g_watchdog.total_s) {
      std::snprintf(text, sizeof(text), "test time of %d s over after %llu frames: restarting\n", seconds,
                    (unsigned long long)now);
    } else if (g_watchdog.frozen_s > 0 && now == frames && appletGetFocusState() == AppletFocusState_InFocus &&
               ++still >= g_watchdog.frozen_s) {
      std::snprintf(text, sizeof(text), "no frame for %d s after %llu frames (%d s in): restarting\n", still,
                    (unsigned long long)now, seconds);
    } else {
      if (now != frames || appletGetFocusState() != AppletFocusState_InFocus) {
        frames = now;
        still = 0;
      }
      continue;
    }
    Note(text);
    appletRestartProgram(nullptr, 0);
    still = 0;  // refused (not a forwarder start): look again
  }
}
#endif

}  // namespace

namespace bo2cp {

// Starts the watchdog when bo2cp_watchdog_s or bo2cp_test_seconds asks for it (Switch). Call from OnPostSetup, once
// the graphics system exists.
void StartWatchdog(const fs::path& logs_dir) {
#if defined(__SWITCH__)
  if (REXCVAR_GET(bo2cp_restart_on_crash)) {
    RexSwitchSetRestartOnCrash(true);
    REXLOG_INFO("[watchdog] a crash restarts the application (bo2cp_restart_on_crash)");
  }
  const int frozen = REXCVAR_GET(bo2cp_watchdog_s);
  const int total = REXCVAR_GET(bo2cp_test_seconds);
  if (frozen <= 0 && total <= 0) {
    return;
  }
  bool native = false;
#if BO2CP_NATIVE
  native = bo2zm::native::Active();
#endif
  if (!native) {
    auto* graphics = static_cast<rex::graphics::GraphicsSystem*>(rex::Runtime::instance()->graphics_system());
    g_watchdog.gpu = graphics ? graphics->command_processor() : nullptr;
  }
  g_watchdog.frozen_s = frozen;
  g_watchdog.total_s = total;
  std::error_code ec;
  fs::create_directories(logs_dir, ec);
  g_watchdog.marker = (logs_dir / "bo2cp_watchdog.txt").string();
  if (R_SUCCEEDED(threadCreate(&g_watchdog.thread, WatchdogMain, nullptr, nullptr, 0x4000, 0x2C, -2)) &&
      R_SUCCEEDED(threadStart(&g_watchdog.thread))) {
    REXLOG_INFO("[watchdog] on: restart after {} s without a frame{}", frozen,
                total > 0 ? fmt::format(", or {} s after the start", total) : std::string());
  } else {
    REXLOG_ERROR("[watchdog] could not start its thread");
  }
#else
  (void)logs_dir;
#endif
}


// Hands a pending relaunch meant for the campaign (written by the Zombies program, or by this one) to XAM. Call from
// OnPostSetup, before the game starts.
void ApplyPendingRelaunch(const fs::path& user_dir) {
  bo2zm::launch::Configure(user_dir);
  g_user_dir = bo2zm::launch::SharedUserRoot(user_dir);
  auto* ld = LoaderData();
  if (!ld) {
    return;
  }
  // Direct starts always remain Campaign, independently of the launcher's default.
  g_startup_launch = *ld;
  g_startup_launch.launch_path = "game:\\default.xex";
  g_launch_ready = true;
  const fs::path pending = g_user_dir / kPendingFile;
  std::error_code ec;
  if (!fs::exists(pending, ec)) {
    // Started without the campaign's files: bo2zm starts one of its installed modes instead.
    const auto& installed = Installed();
    if (!installed.campaign && (installed.zombies || installed.multiplayer)) {
      REXLOG_WARN("[relaunch] the campaign's files are not installed; starting bo2zm ({})", installed.Text());
      StartAndExit("bo2zm.nro");
    }
    return;
  }
  uint32_t flags = 0;
  std::string launch_path;
  std::vector<uint8_t> data;
  const bool ok = ReadBlob(pending, flags, launch_path, data);
  if (!ok) {
    fs::remove(pending, ec);
    REXLOG_ERROR("[relaunch] could not read {}", pending.string());
    return;
  }
  if (launch_path.ends_with("default_mp.xex") && !Installed().Has(bo2zm::launch::LaunchMode(launch_path, data))) {
    fs::remove(pending, ec);
    REXLOG_WARN("[relaunch] pending launch '{}' asks for a mode that is not installed; starting the campaign",
                launch_path);
    return;
  }
  if (launch_path.ends_with("default_mp.xex")) {
#if !defined(__SWITCH__)
    if (ProgramAvailable("bo2zm.nro")) {
      StartAndExit("bo2zm.nro");
    }
#endif
    REXLOG_WARN("[relaunch] could not hand over pending launch '{}'; preserving it", launch_path);
    return;
  }
  fs::remove(pending, ec);  // consumed once, as on a console
  ld->launch_path = launch_path;
  ld->launch_flags = flags;
  ld->launch_data = data;
  ld->launch_data_present = !data.empty();
  g_startup_launch = *ld;
  g_startup_launch.launch_path = "game:\\default.xex";
  REXLOG_INFO("[relaunch] booted from a relaunch of '{}': flags {:#x}, {} bytes of launch data: {}", launch_path,
              flags, data.size(), HexPreview(data));
}

void RequestRestart() { g_restart_requested.store(true, std::memory_order_release); }

bool RestartForScriptChanges() {
  if (!g_restart_requested.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  if (!g_launch_ready) {
    REXLOG_ERROR("[relaunch] cannot restart before launch data is initialized");
    return false;
  }
  const fs::path pending = g_user_dir / kPendingFile;
  const char* program = "bo2cp.nro";
  std::error_code ec;
  if (fs::exists(pending, ec)) {
    uint32_t flags = 0;
    std::string path;
    std::vector<uint8_t> data;
    if (!ReadBlob(pending, flags, path, data)) {
      REXLOG_ERROR("[relaunch] cannot preserve pending launch {}", pending.string());
      return false;
    }
    if (path.ends_with("default_mp.xex")) {
      program = "bo2zm.nro";
    }
  } else {
    if (ec) {
      return false;
    }
    fs::create_directories(g_user_dir, ec);
    if (ec || !WriteBlob(pending, g_startup_launch)) {
      REXLOG_ERROR("[relaunch] could not save the current mode for restart");
      return false;
    }
  }
  if (!ProgramAvailable(program)) {
    REXLOG_ERROR("[relaunch] cannot restart: {} is not installed", program);
    return false;
  }
  StartAndExit(program);
}

}  // namespace bo2cp

const bo2zm::launch::InstalledModes& bo2zm::ModesInstalled() { return Installed(); }

void bo2zm::launch::RestartCurrentMode() {
  bo2cp::RequestRestart();
  bo2cp::RestartForScriptChanges();
}

// The campaign's "launch title" wrapper: sub_825C09D0(r3 = guest path, r4 = flags). The original is not called: its
// XamLoaderLaunchTitle -> TerminateTitle would stop this thread and the guest would run on past the call.
REX_HOOK_RAW(sub_825C09D0) {
  std::string target = "default.xex";
  if (ctx.r3.u32) {
    target = rex::Runtime::instance()->memory()->TranslateVirtual<const char*>(ctx.r3.u32);
  }
  REXLOG_INFO("[relaunch] sub_825C09D0('{}', {:#x}) from lr {:08X}", target, ctx.r4.u32, uint32_t(ctx.lr));
  auto* ld = LoaderData();
  if (!ld) {
    REXLOG_ERROR("[relaunch] no XAM module; exiting");
    rex::FlushLogging();
    std::_Exit(1);
  }
  ld->launch_path = "game:\\" + target;
  ld->launch_flags = ctx.r4.u32;
  REXLOG_INFO("[relaunch] launch data: {} bytes: {}", ld->launch_data_present ? ld->launch_data.size() : 0,
              HexPreview(ld->launch_data));
  if (target.ends_with("default_mp.xex") &&
      !Installed().Has(bo2zm::launch::LaunchMode(
          target, ld->launch_data_present ? ld->launch_data : std::vector<uint8_t>{}))) {
    // The mode menu offers installed modes only (menus.lua); this catches anything else, such as a console command.
    // The game has already left its menus for the launch, so the campaign starts again.
    REXLOG_WARN("[relaunch] '{}' asks for a mode that is not installed; starting the campaign again", target);
    if (g_launch_ready) {
      *ld = g_startup_launch;
    } else {
      *ld = {};
      ld->launch_path = "game:\\default.xex";
    }
    target = "default.xex";
  }
  std::error_code ec;
  fs::create_directories(g_user_dir, ec);
  if (ec || !WriteBlob(g_user_dir / kPendingFile, *ld)) {
    REXLOG_ERROR("[relaunch] could not preserve the explicit mode launch; exiting");
    rex::FlushLogging();
    std::_Exit(1);
  }
  StartAndExit(target.ends_with("default_mp.xex") ? "bo2zm.nro" : "bo2cp.nro");
}
