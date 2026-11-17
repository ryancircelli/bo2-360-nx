// Compat: v0.10 rex::Runtime facade over v0.11 kernel::TitleRuntime.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <rex/system/xmemory.h>
#include <rex/kernel/title_paths.h>
namespace rex {
struct RuntimeConfig {  // v0.10 input/graphics factories are gone in v0.11; kept so callers compile
  bool tool_mode = false;
};
class Runtime {
 public:
  static Runtime* instance() {
    static Runtime r;
    return kernel::GetProcessRuntime() ? &r : nullptr;
  }
  memory::Memory* memory() const {
    if (!mem_) mem_ = std::make_unique<memory::Memory>(static_cast<kernel::GuestAddressSpace*>(&kernel::GetProcessRuntime()->GetGuestHeap()));
    return mem_.get();
  }
  std::filesystem::path game_data_root() const { return kernel::GetProcessRuntime()->GetTitlePaths().game_root.Get(); }
  std::filesystem::path update_data_root() const { return kernel::GetProcessRuntime()->GetTitlePaths().update_root.Get(); }
  std::filesystem::path user_data_root() const { return {}; }
 private:
  mutable std::unique_ptr<memory::Memory> mem_;
};
}  // namespace rex
