// macOS-only platform pieces: where this program lives (to find bo2cp beside it) and the relaunch the game asks for
// when it switches modes (bo2zm_relaunch.cpp).
//
// Guest memory needs no startup check here, unlike Linux (bo2zm_linux.cpp): macOS arm64 has 16 KB pages, and the SDK
// maps the 0xE0000000 physical heap 0x1000 bytes higher there, which rex::memory::detail::PhysicalHostOffset and the
// generated code agree on.
#include <rex/platform.h>

#if REX_PLATFORM_MAC

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace bo2zm::platform {

// The running executable, symlinks resolved; empty if the system will not say.
std::filesystem::path ExecutablePathMac() {
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return {};
  }
  path.resize(std::strlen(path.c_str()));
  std::error_code ec;
  auto resolved = std::filesystem::canonical(path, ec);
  return ec ? std::filesystem::path(path) : resolved;
}

// Replaces this process with `program` (empty: this executable again), passing the same arguments. Only stdin, stdout
// and stderr are inherited, so the new image can bind its sockets again (as the Windows and Linux relaunches do).
// posix_spawn with POSIX_SPAWN_SETEXEC execs in place, like execv, and POSIX_SPAWN_CLOEXEC_DEFAULT closes every other
// descriptor. Returns only if that failed.
void RestartProcessMac(const std::filesystem::path& program) {
  const std::filesystem::path next = program.empty() ? ExecutablePathMac() : program;
  if (next.empty()) {
    REXLOG_ERROR("[relaunch] cannot find this executable's path");
    return;
  }
  std::vector<std::string> args;
  const int argc = *_NSGetArgc();
  char** const argv_in = *_NSGetArgv();
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv_in[i] ? argv_in[i] : "");
  }
  if (args.empty()) {
    args.emplace_back();
  }
  args[0] = next.string();
  std::vector<char*> argv;
  for (auto& a : args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);

  posix_spawnattr_t attr;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT);
  for (int fd = 0; fd <= 2; ++fd) {
    posix_spawn_file_actions_addinherit_np(&actions, fd);
  }
  REXLOG_INFO("[relaunch] exec {}", next.string());
  rex::FlushLogging();
  const int rc = posix_spawn(nullptr, next.c_str(), &actions, &attr, argv.data(), *_NSGetEnviron());
  // Still here: the exec failed.
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  std::fprintf(stderr, "bo2zm: relaunch failed: %s\n", std::strerror(rc));
}

}  // namespace bo2zm::platform

#endif  // REX_PLATFORM_MAC
