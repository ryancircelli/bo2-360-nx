// Pipeline state lists: paths and switches (bo2zm_native_pso.h). The list itself is read and written by the
// renderer (bo2zm_native_draws.cpp), which owns the pipeline records.

#include "bo2zm_native_pso.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <cstdlib>
#include <string>
#include <system_error>

#ifndef BO2ZM_NATIVE_CACHE_FOLDER
#define BO2ZM_NATIVE_CACHE_FOLDER "cache"  // as in bo2zm_native_draws.cpp
#endif

REXCVAR_DEFINE_STRING(bo2zm_pso_record, "", "BO2",
                      "Record every pipeline this run creates to this bo2pso text list (docs/mac/pso-list-format.md; "
                      "empty = off; the environment variable REX_BO2ZM_PSO_RECORD does the same)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_pso_verify, false, "BO2",
                    "Log \"[pso] unlisted pipeline <key>\" (and a perf log compile event) for every pipeline created "
                    "that the loaded bo2zm_pso.psolist does not list (the environment variable REX_BO2ZM_PSO_VERIFY=1 "
                    "does the same)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_pso_list, "", "BO2",
                      "The bo2pso text list read at start-up and compiled behind the loading screen (empty = "
                      "bo2zm_pso.psolist beside the executable, or in the app bundle's Resources)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_native_cache_dir, "", "BO2",
                      "Folder of the native renderer's pipeline cache (empty = cache/ beside the executable; on macOS "
                      "~/Library/Caches/bo2-360-nx when the game runs from an .app bundle)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::native::pso {
namespace fs = std::filesystem;

namespace {

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

#if defined(__APPLE__)
// Contents/MacOS of an .app bundle, where codesign and the read-only install location rule out writing files.
bool InBundle(const fs::path& exe_folder) {
  return exe_folder.filename() == "MacOS" && exe_folder.parent_path().filename() == "Contents" &&
         exe_folder.parent_path().parent_path().extension() == ".app";
}
#endif

}  // namespace

fs::path CacheFolder() {
  if (const std::string dir = REXCVAR_GET(bo2zm_native_cache_dir); !dir.empty()) {
    return fs::path(dir);
  }
  const fs::path exe = rex::filesystem::GetExecutableFolder();
#if defined(__APPLE__)
  if (InBundle(exe)) {
    if (const std::string home = Env("HOME"); !home.empty()) {
      return fs::path(home) / "Library" / "Caches" / "bo2-360-nx";
    }
  }
#endif
  return exe / BO2ZM_NATIVE_CACHE_FOLDER;
}

fs::path ListToLoad() {
  if (const std::string path = REXCVAR_GET(bo2zm_pso_list); !path.empty()) {
    return fs::path(path);
  }
  const fs::path exe = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  for (const fs::path& candidate :
       {exe / "bo2zm_pso.psolist", exe.parent_path() / "Resources" / "bo2zm_pso.psolist",
        exe.parent_path() / "Resources" / "runtime" / "bo2zm_pso.psolist"}) {
    if (fs::is_regular_file(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

fs::path RecordPath() {
  std::string path = REXCVAR_GET(bo2zm_pso_record);
  if (path.empty()) {
    path = Env("REX_BO2ZM_PSO_RECORD");
  }
  return path.empty() ? fs::path() : fs::path(path);
}

bool Verify() {
  const std::string env = Env("REX_BO2ZM_PSO_VERIFY");
  return REXCVAR_GET(bo2zm_pso_verify) || (!env.empty() && env != "0");
}

std::string ListStamp() {
  const fs::path list = ListToLoad();
  if (list.empty()) {
    return {};
  }
  std::error_code ec;
  const auto size = fs::file_size(list, ec);
  return "psolist " + list.filename().string() + ":" + std::to_string(ec ? 0 : size) + "\n";
}

}  // namespace bo2zm::native::pso
