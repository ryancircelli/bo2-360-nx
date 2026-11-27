/**
 * @file        rexglue/commands/patches_command.cpp
 * @brief       `rexglue patches XEX DIR`: list which game patch files match an executable, and what kind each patch is
 */

#include "patches_command.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/game_patches.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

namespace rexglue::cli {
using rex::X_STATUS;

namespace fs = std::filesystem;

void RegisterPatches(CLI::App& parent, const CliContext&, DeferredAction& pending) {
  auto xex = std::make_shared<std::string>();
  auto dir = std::make_shared<std::string>();
  auto* sub = parent.add_subcommand("patches", "List the game patch files (.patch.toml) that match an executable");
  sub->add_option("xex", *xex, "Game executable (default.xex); a title update beside it (<name>p) is applied")
      ->required();
  sub->add_option("dir", *dir, "Folder of xenia-canary patch files")->required();
  auto dump = std::make_shared<std::string>();
  sub->add_option("--dump-image", *dump, "Also write the loaded image (title update applied) here, for writing patches");
  sub->callback([xex, dir, dump, &pending]() {
    pending = [xex, dir, dump]() -> rex::Result<void> {
      const fs::path path = fs::canonical(*xex);
      rex::patches::Configure({fs::path(*dir)}, {});
      auto runtime = std::make_unique<rex::Runtime>(path.parent_path().string());
      if (runtime->Setup(rex::RuntimeConfig{.kernel_init = rex::kernel::InitializeKernel, .tool_mode = true}) !=
          X_STATUS_SUCCESS) {
        return rex::Err<void>(rex::ErrorCategory::IO, "runtime setup failed");
      }
      if (runtime->LoadXexImage("game:\\" + path.filename().string()) != X_STATUS_SUCCESS) {
        return rex::Err<void>(rex::ErrorCategory::IO, "could not load " + path.string());
      }
      for (const auto& m : rex::patches::ModuleSummary()) std::printf("module %s\n", m.c_str());
      if (!dump->empty()) {
        auto* xm = runtime->kernel_state()->GetExecutableModule()->xex_module();
        const uint32_t size = xm->xex_security_info()->image_size;
        std::ofstream(*dump, std::ios::binary)
            .write(runtime->memory()->TranslateVirtual<const char*>(xm->base_address()), size);
        std::printf("image\t%08X\t%u bytes\t%s\n", xm->base_address(), size, dump->c_str());
      }
      std::map<std::string, int> counts;
      for (const auto& e : rex::patches::List()) {
        const char* kind = e.kind == rex::patches::Kind::kCode ? "code" : "data";
        const std::string state = e.available ? "usable" : e.reason;
        ++counts[fmt::format("{} {}", kind, e.available ? "usable" : e.reason.substr(0, e.reason.find(' ', 12)))];
        std::printf("%s\t%s\t%s\t%s\n", kind, e.file.c_str(), e.title.c_str(), state.c_str());
      }
      for (const auto& [k, n] : counts) std::printf("count\t%s\t%d\n", k.c_str(), n);
      return rex::Ok();
    };
  });
}

}  // namespace rexglue::cli
