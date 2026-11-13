// Title relaunch through the existing XAM launch-data file. Explicit launches
// take precedence over the saved default and the campaign test loop. On Switch,
// program handover happens before runtime threads start, after a system restart.
// Only installed modes are ever started (bo2zm_launch_options.h, InstalledModes):
// a saved default or a request for a missing mode starts an installed one instead.

#include "bo2zm_relaunch.h"
#include "bo2zm_launch_options.h"
#include "bo2zm_mode.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__SWITCH__)
#include <switch.h>
extern "C" int __system_argc;
extern "C" char** __system_argv;
#elif REX_PLATFORM_GNU_LINUX
#include <unistd.h>
namespace bo2zm::platform {
void RestartProcessLinux();  // bo2zm_linux.cpp
}
#elif REX_PLATFORM_MAC
namespace bo2zm::platform {
std::filesystem::path ExecutablePathMac();                            // bo2zm_mac.cpp
void RestartProcessMac(const std::filesystem::path& program);         // bo2zm_mac.cpp
}
#endif

namespace fs = std::filesystem;
namespace launch = bo2zm::launch;

REXCVAR_DEFINE_BOOL(bo2zm_boot_zombies, true, "BO2",
                    "Legacy setting retained for configuration compatibility; launch preferences select the startup mode.");
REXCVAR_DEFINE_BOOL(bo2zm_zombies_only, true, "BO2",
                    "When the game asks for a mode that is not installed (its files or, for Campaign, its program "
                    "are missing), start the current mode again. false = exit instead.");
REXCVAR_DEFINE_BOOL(bo2zm_test_campaign, false, "BO2",
                    "Test consoles (Switch, from a forwarder): every start hands over to the campaign program at once, "
                    "after the start-up sync, so the campaign runs again after each restart of its watchdog "
                    "(bo2cp_watchdog_s, bo2cp_test_seconds) and its logs come back. Read straight from bo2zm.toml before "
                    "anything starts")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_test_mode, "", "BO2",
                     "Test helper: startup mode for this run, mp or zm; empty uses the saved default. "
                     "Pending launches and the Campaign test take precedence; does not save a preference.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_campaign, true, "BO2",
                    "Enable the installed Campaign companion. If unavailable, Campaign uses the Zombies fallback.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kMagic = 0x42325A4C;  // "B2ZL"
constexpr const char* kPendingFile = "relaunch_pending.bin";

fs::path g_user_dir;
std::atomic<bool> g_zombies{false};
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

// Launch data the game itself writes when Zombies is chosen from the MP menu
// (captured from one of its own relaunches): 1000 bytes, all zero except
// byte 3 = 0x0B and byte 860 = 1.
void SetZombiesLaunchData(rex::kernel::xam::XamModule::LoaderData& ld) {
  ld.launch_data.assign(1000, 0);
  ld.launch_data[3] = 0x0B;
  ld.launch_data[860] = 1;
  ld.launch_data_present = true;
  ld.launch_flags = 0;
}

bool IsZombiesLaunchData(const rex::kernel::xam::XamModule::LoaderData& ld) {
  return ld.launch_data_present && ld.launch_data.size() == 1000 && ld.launch_data[3] == 0x0B &&
         ld.launch_data[860] == 1;
}

std::string HexPreview(const std::vector<uint8_t>& data, size_t max_bytes = 64) {
  std::string out;
  for (size_t i = 0; i < data.size() && i < max_bytes; i++) {
    out += fmt::format("{:02X}", data[i]);
  }
  if (data.size() > max_bytes) {
    out += "...";
  }
  return out;
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

bool FileExists(const fs::path& path) {
  FILE* file = std::fopen(path.string().c_str(), "rb");
  if (!file) {
    return false;
  }
  std::fclose(file);
  return true;
}

const char* ModeName(launch::Mode mode) {
  switch (mode) {
    case launch::Mode::Campaign:
      return "Campaign";
    case launch::Mode::Multiplayer:
      return "Multiplayer";
    case launch::Mode::Zombies:
      return "Zombies";
  }
  return "?";
}

// The launch data of a start in Zombies or Multiplayer.
void SetModeLaunchData(rex::kernel::xam::XamModule::LoaderData& ld, launch::Mode mode) {
  SetZombiesLaunchData(ld);
  ld.launch_path = "game:\\default_mp.xex";
  if (mode == launch::Mode::Multiplayer) {
    ld.launch_data[860] = 0;
  }
}

// The campaign program next to this one (Switch: sdmc:/switch/bo2zm/bo2cp.nro), or empty when there is none.
fs::path CampaignProgram() {
#if defined(__SWITCH__)
  if (!REXCVAR_GET(bo2zm_campaign) || __system_argc < 1 || !__system_argv || !__system_argv[0]) {
    return {};
  }
  std::error_code ec;
  const fs::path program = fs::path(__system_argv[0]).parent_path() / "bo2cp.nro";
  return fs::is_regular_file(program, ec) ? program : fs::path();
#elif defined(_WIN32)
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!length || length >= MAX_PATH || !REXCVAR_GET(bo2zm_campaign)) {
    return {};
  }
  std::error_code ec;
  const fs::path program = fs::path(exe).parent_path() / "bo2cp.exe";
  return fs::is_regular_file(program, ec) ? program : fs::path();
#elif REX_PLATFORM_GNU_LINUX
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec || !REXCVAR_GET(bo2zm_campaign)) {
    return {};
  }
  const fs::path program = exe.parent_path() / "bo2cp";
  return fs::is_regular_file(program, ec) ? program : fs::path();
#elif REX_PLATFORM_MAC
  const fs::path exe = bo2zm::platform::ExecutablePathMac();
  if (exe.empty() || !REXCVAR_GET(bo2zm_campaign)) {
    return {};
  }
  std::error_code ec;
  const fs::path program = exe.parent_path() / "bo2cp";
  return fs::is_regular_file(program, ec) ? program : fs::path();
#else
  return {};
#endif
}

// The modes installed beside this program, read once the runtime knows the game folder.
const launch::InstalledModes& Installed() {
  static const launch::InstalledModes installed = [] {
    fs::path game;
    if (const auto* runtime = rex::Runtime::instance()) {
      game = runtime->game_data_root();
    }
    const auto modes = launch::DetectInstalledModes(game, !CampaignProgram().empty());
    REXLOG_INFO("[relaunch] installed modes: {} (game files: {})", modes.Text().empty() ? "none" : modes.Text(),
                game.string());
    return modes;
  }();
  return installed;
}

// Zombies or Multiplayer, whichever this program can start (Zombies first).
launch::Mode MainProgramMode(const launch::InstalledModes& installed) {
  return installed.zombies || !installed.multiplayer ? launch::Mode::Zombies : launch::Mode::Multiplayer;
}

// Starts `program` (empty: this one again) once this process is gone.
[[noreturn]] void RestartProcess(const fs::path& program = {}) {
  rex::FlushLogging();
#if defined(__SWITCH__)
  // The system ends this process and starts the application again; HandOverEarly below takes it from there.
  if (R_SUCCEEDED(appletRestartProgram(nullptr, 0))) {
    REXLOG_INFO("[relaunch] restarting the application");
    rex::FlushLogging();
    for (int i = 0; i < 120; ++i) {
      svcSleepThread(250'000'000);
    }
    REXLOG_WARN("[relaunch] still running 30 s after the restart request: chain-loading instead");
  } else {
    REXLOG_WARN("[relaunch] the system refused the restart (started from hbmenu?): chain-loading instead");
  }
  rex::FlushLogging();
#endif
#if defined(_WIN32)
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmdline = GetCommandLineW();
  STARTUPINFOW yes{};
  yes.cb = sizeof(yes);
  PROCESS_INFORMATION pi{};
  const fs::path next = program.empty() ? fs::path(exe) : program;
  if (CreateProcessW(next.c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &yes, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    REXLOG_ERROR("[relaunch] CreateProcessW failed: {}", GetLastError());
    rex::FlushLogging();
  }
#elif defined(__SWITCH__)
  // hbloader runs the NRO named by envSetNextLoad once this one exits. Our
  // own path is argv[0] (e.g. sdmc:/switch/bo2zm/bo2zm.nro).
  if (envHasNextLoad() && __system_argc > 0 && __system_argv[0]) {
    const std::string next = program.empty() ? std::string(__system_argv[0]) : program.string();
    const Result rc = envSetNextLoad(next.c_str(), next.c_str());
    REXLOG_INFO("[relaunch] envSetNextLoad('{}') -> {:#x}", next, rc);
  } else {
    REXLOG_ERROR("[relaunch] the loader does not support chaining NROs; exiting");
  }
  rex::FlushLogging();
#elif REX_PLATFORM_GNU_LINUX
  if (!program.empty()) {
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
      arguments[0] = program.string();
      std::vector<char*> argv;
      for (auto& argument : arguments) {
        argv.push_back(argument.data());
      }
      argv.push_back(nullptr);
      close_range(3, ~0u, 0);
      execv(program.c_str(), argv.data());
      std::perror("bo2zm: relaunch failed");
    }
    std::_Exit(1);
  }
  bo2zm::platform::RestartProcessLinux();
#elif REX_PLATFORM_MAC
  bo2zm::platform::RestartProcessMac(program);
  if (!program.empty()) {
    std::_Exit(1);
  }
#else
  (void)program;
  REXLOG_ERROR("[relaunch] not implemented on this platform; exiting");
  rex::FlushLogging();
#endif
  // Teardown deadlocks on guest threads that TerminateTitle left running
  // (see KernelState::TerminateTitle), so leave without it.
  std::_Exit(0);
}

}  // namespace

#if defined(__SWITCH__)
namespace bo2zm {

// Called from the first constructor, before runtime threads exist. Leave the pending
// file for the receiving program to consume, so a handover cannot apply defaults twice.
void HandOverEarly() {
  if (__system_argc < 1 || !__system_argv || !__system_argv[0] || !envHasNextLoad()) {
    return;
  }
  const fs::path dir = fs::path(__system_argv[0]).parent_path();
  const fs::path root = launch::SharedUserRoot();
  const std::string campaign = (dir / "bo2cp.nro").string();
  const auto installed = launch::DetectInstalledModes(dir / "game", FileExists(campaign));
  uint32_t flags = 0;
  std::string launch_path;
  std::vector<uint8_t> data;
  std::optional<launch::Mode> pending;
  if (ReadBlob(root / kPendingFile, flags, launch_path, data)) {
    pending = launch::LaunchMode(launch_path, data);
    if (!installed.Has(*pending)) {
      // A request for a mode that is not installed: drop it and start the default, so neither program hands the
      // request back to the other.
      std::remove((root / kPendingFile).string().c_str());
      pending.reset();
    }
  }
  const auto tests = launch::ReadStartupTestsEarly(dir / "bo2zm.toml");
  const auto selected = launch::FirstInstalled(
      launch::StartupMode(launch::Program::Main, pending,
                          static_cast<launch::Mode>(launch::ReadDefaultModeIndexEarly(root)), tests.campaign,
                          tests.mode),
      installed);
  if (selected != launch::Mode::Campaign || !installed.campaign) {
    return;
  }
  if (R_SUCCEEDED(envSetNextLoad(campaign.c_str(), campaign.c_str()))) {
    // The SDK's atexit hook treats exit() as a crash.
    std::_Exit(0);
  }
}

}  // namespace bo2zm
#else
namespace bo2zm {
void HandOverEarly() {}
}  // namespace bo2zm
#endif

namespace bo2zm {

bool CampaignAvailable() { return Installed().campaign; }

const launch::InstalledModes& ModesInstalled() { return Installed(); }

static void ApplyDefaultLaunchData() {
  auto* ld = LoaderData();
  if (!ld) {
    return;
  }
  const auto& installed = Installed();
  const auto wanted = launch::StartupMode(
      launch::Program::Main, std::nullopt,
      static_cast<launch::Mode>(launch::GetDefaultModeIndex()),
      REXCVAR_GET(bo2zm_test_campaign), launch::ParseTestMode(REXCVAR_GET(bo2zm_test_mode)));
  const auto selected = launch::FirstInstalled(wanted, installed);
  if (selected != wanted) {
    REXLOG_WARN("[relaunch] {} is not installed; starting {}", ModeName(wanted), ModeName(selected));
  }
  if (selected == launch::Mode::Campaign && installed.campaign) {
#if defined(_WIN32) || REX_PLATFORM_GNU_LINUX || REX_PLATFORM_MAC
    if (const fs::path campaign = CampaignProgram(); !campaign.empty()) {
      rex::kernel::xam::XamModule::LoaderData launch;
      launch.launch_path = "game:\\default.xex";
      std::error_code ec;
      fs::create_directories(g_user_dir, ec);
      if (!ec && WriteBlob(g_user_dir / kPendingFile, launch)) {
        RestartProcess(campaign);
      }
    }
#endif
    // Switch handover already ran before the runtime. Do not restart again if
    // the companion is missing or the loader refused it.
    REXLOG_WARN("[relaunch] could not start the Campaign program; starting {}",
                ModeName(MainProgramMode(installed)));
  }
  SetModeLaunchData(*ld, selected == launch::Mode::Campaign ? MainProgramMode(installed) : selected);
}

static void ApplyPendingLaunchData(const fs::path& user_dir) {
  launch::Configure(user_dir);
  g_user_dir = launch::SharedUserRoot(user_dir);
  // Migrate the old boot switch once; subsequent menu choices take priority.
  if (!launch::MigrateLegacyDefault(REXCVAR_GET(bo2zm_boot_zombies),
      launch::ParseTestMode(REXCVAR_GET(bo2zm_test_mode)), g_user_dir)) {
    REXLOG_WARN("[relaunch] could not save the initial default mode");
  }
  const fs::path pending = g_user_dir / kPendingFile;
  std::error_code ec;
  if (!fs::exists(pending, ec)) {
    ApplyDefaultLaunchData();
    return;
  }
  uint32_t flags = 0;
  std::string launch_path;
  std::vector<uint8_t> data;
  const bool ok = ReadBlob(pending, flags, launch_path, data);
  auto* ld = LoaderData();
  if (!ld) {
    return;
  }
  if (!ok) {
    fs::remove(pending, ec);
    REXLOG_ERROR("[relaunch] could not read {}", pending.string());
    ApplyDefaultLaunchData();
    return;
  }
  const auto requested = launch::LaunchMode(launch_path, data);
  if (!Installed().Has(requested)) {
    fs::remove(pending, ec);
    REXLOG_WARN("[relaunch] pending launch '{}' asks for {}, which is not installed", launch_path,
                ModeName(requested));
    ApplyDefaultLaunchData();
    return;
  }
  if (requested == launch::Mode::Campaign) {
#if defined(_WIN32) || REX_PLATFORM_GNU_LINUX || REX_PLATFORM_MAC
    if (const fs::path campaign = CampaignProgram(); !campaign.empty()) {
      RestartProcess(campaign);  // leave the explicit pending file for the campaign
    }
#endif
    fs::remove(pending, ec);
    const auto fallback = MainProgramMode(Installed());
    REXLOG_WARN("[relaunch] could not hand over pending launch '{}'; starting {}", launch_path, ModeName(fallback));
    SetModeLaunchData(*ld, fallback);
    return;
  }
  fs::remove(pending, ec);  // consumed once, as on a console
  ld->launch_path = launch_path;
  ld->launch_flags = flags;
  ld->launch_data = data;
  ld->launch_data_present = !data.empty();
  REXLOG_INFO("[relaunch] booted from a relaunch of '{}': flags {:#x}, {} bytes of launch data: {}",
              launch_path, flags, data.size(), HexPreview(data));
}

void ApplyPendingRelaunch(const fs::path& user_dir) {
  ApplyPendingLaunchData(user_dir);
  const auto* ld = LoaderData();
  g_zombies.store(ld && IsZombiesLaunchData(*ld), std::memory_order_relaxed);
  if (ld) {
    g_startup_launch = *ld;
    g_startup_launch.launch_path = "game:\\default_mp.xex";
    g_launch_ready = true;
  }
  REXLOG_INFO("[relaunch] mode: {}", mode::IsZombies() ? "Zombies" : "Multiplayer");
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
  std::error_code ec;
  if (fs::exists(pending, ec)) {
    uint32_t flags = 0;
    std::string path;
    std::vector<uint8_t> data;
    if (!ReadBlob(pending, flags, path, data)) {
      REXLOG_ERROR("[relaunch] cannot preserve pending launch {}", pending.string());
      return false;
    }
    if (!path.ends_with("default_mp.xex")) {
      const auto campaign = CampaignProgram();
      if (campaign.empty()) {
        REXLOG_ERROR("[relaunch] pending Campaign launch has no installed companion");
        return false;
      }
      RestartProcess(campaign);
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
  RestartProcess();
}

namespace mode {
bool IsZombies() { return g_zombies.load(std::memory_order_relaxed); }
}  // namespace mode

}  // namespace bo2zm

void bo2zm::launch::RestartCurrentMode() {
  bo2zm::RequestRestart();
  bo2zm::RestartForScriptChanges();
}

// The game's "launch title" wrapper: sub_827D36E8(r3 = guest path, r4 = 0).
// Its only import call is XamLoaderLaunchTitle; the launch data was already
// stored by an earlier XamLoaderSetLaunchData. The original is not called:
// its XamLoaderLaunchTitle -> TerminateTitle stops this very thread before
// anything after it could run, which only reproduces the crash, unlogged.
REX_HOOK_RAW(sub_827D36E8) {
  const uint32_t path_ptr = ctx.r3.u32;
  std::string target = "default.xex";
  if (path_ptr) {
    target = rex::Runtime::instance()->memory()->TranslateVirtual<const char*>(path_ptr);
  }
  auto* ld = LoaderData();
  REXLOG_INFO("[relaunch] sub_827D36E8('{}', {:#x}) from lr {:08X}", target, ctx.r4.u32,
              uint32_t(ctx.lr));
  if (!ld) {
    REXLOG_ERROR("[relaunch] no XAM module; exiting");
    rex::FlushLogging();
    std::_Exit(1);
  }
  ld->launch_path = "game:\\" + target;
  ld->launch_flags = ctx.r4.u32;
  REXLOG_INFO("[relaunch] launch data: {} bytes: {}", ld->launch_data_present ? ld->launch_data.size() : 0,
              HexPreview(ld->launch_data));
  const auto requested =
      launch::LaunchMode(target, ld->launch_data_present ? ld->launch_data : std::vector<uint8_t>{});
  if (!Installed().Has(requested)) {
    // Every menu offers installed modes only (menus.lua); this catches anything else, such as a console command.
    REXLOG_WARN("[relaunch] '{}' asks for {}, which is not installed", target, ModeName(requested));
    if (!REXCVAR_GET(bo2zm_zombies_only)) {
      REXLOG_ERROR("[relaunch] bo2zm_zombies_only is off; exiting");
      rex::FlushLogging();
      std::_Exit(0);
    }
    if (g_launch_ready) {
      *ld = g_startup_launch;
    } else {
      SetModeLaunchData(*ld, MainProgramMode(Installed()));
    }
    target = "default_mp.xex";
    REXLOG_INFO("[relaunch] starting {} again", bo2zm::mode::IsZombies() ? "Zombies" : "Multiplayer");
  }
  if (!target.ends_with("default_mp.xex")) {
    if (const fs::path campaign = CampaignProgram(); !campaign.empty()) {
      // Campaign: hand over to the campaign program with the launch data the game set, as the 360 does.
      std::error_code ec;
      fs::create_directories(g_user_dir, ec);
      if (ec || !WriteBlob(g_user_dir / kPendingFile, *ld)) {
        REXLOG_ERROR("[relaunch] could not preserve the explicit Campaign launch; exiting");
        rex::FlushLogging();
        std::_Exit(1);
      }
      REXLOG_INFO("[relaunch] '{}' requested: starting the campaign program {}", target, campaign.string());
      RestartProcess(campaign);
    }
  }
  if (!target.ends_with("default_mp.xex")) {
    REXLOG_ERROR("[relaunch] no installed companion for '{}'; exiting.",
                 target);
    rex::FlushLogging();
    std::_Exit(0);
  }

  std::error_code ec;
  fs::create_directories(g_user_dir, ec);
  if (ec || !WriteBlob(g_user_dir / kPendingFile, *ld)) {
    REXLOG_ERROR("[relaunch] could not preserve the explicit mode launch; exiting");
    rex::FlushLogging();
    std::_Exit(1);
  }
  // Keep every request as well (launch_capture_NNN.bin), for inspection: the
  // one sent when choosing Zombies is the launch data SetZombiesLaunchData
  // reproduces.
  for (int i = 0; i < 1000; i++) {
    const fs::path keep = g_user_dir / fmt::format("launch_capture_{:03}.bin", i);
    if (!fs::exists(keep)) {
      WriteBlob(keep, *ld);
      break;
    }
  }
  RestartProcess();
}
