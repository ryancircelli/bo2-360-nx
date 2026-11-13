// See bo2zm_name.h.
#include "bo2zm_name.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include <rex/kernel/xam/text_input.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_settings.h"

namespace {

constexpr const char* kSavedAs = "user_name";
constexpr size_t kMaxLength = 15;  // the gamertag's: the game asks for the name in 16-byte buffers

std::mutex g_mutex;
std::optional<std::string> g_entered;  // from the text input, for the main thread; under g_mutex
std::atomic<bool> g_entered_waiting{false};
std::atomic<bool> g_show{true};        // bo2zm_name_ui needs setting

rex::system::xam::UserProfile* Profile() {
  auto* kernel_state = REX_KERNEL_STATE();
  return kernel_state ? kernel_state->user_profile() : nullptr;
}

// What the game shows as typed: printable ASCII without the characters that mean something to it: quotes, semicolons
// and backslashes (console text), % and ^ (format and color codes), @ (a localized string reference); the spaces at
// the ends trimmed.
std::string Clean(std::string_view text) {
  std::string clean;
  for (const char c : text) {
    if (c >= 0x20 && c <= 0x7E && std::string_view("\";\\%^@").find(c) == std::string_view::npos) {
      clean += c;
    }
  }
  const size_t first = clean.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  clean = clean.substr(first, clean.find_last_not_of(' ') - first + 1);
  return clean.substr(0, kMaxLength);
}

}  // namespace

void bo2zm::name::ApplySaved() {
  const auto saved = bo2zm::settings::Get(kSavedAs);
  auto* profile = Profile();
  if (!saved || !profile) {
    return;
  }
  const std::string name = Clean(*saved);
  if (name.empty()) {
    REXLOG_WARN("[name] the saved {} has no usable characters; the profile keeps {}", kSavedAs, profile->name());
    return;
  }
  profile->set_name(name);
  REXLOG_INFO("[name] {}, from {}", name, bo2zm::settings::File().string());
}

void bo2zm::name::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!g_entered_waiting.load(std::memory_order_relaxed) && !g_show.load(std::memory_order_relaxed)) [[likely]] {
    return;
  }
  std::optional<std::string> entered;
  if (g_entered_waiting.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_mutex);
    g_entered_waiting.store(false, std::memory_order_relaxed);
    entered.swap(g_entered);
  }
  auto* profile = Profile();
  if (entered && profile) {
    profile->set_name(*entered);
    bo2zm::settings::Set(kSavedAs, *entered);
    REXLOG_INFO("[name] {} (from the next lobby or match)", *entered);
    g_show.store(true, std::memory_order_relaxed);
  }
  if (profile && g_show.exchange(false, std::memory_order_relaxed)) {
    bo2zm::autoexec::ExecuteCommands(ctx, base, "set bo2zm_name_ui \"" + profile->name() + "\"");
  }
}

bool bo2zm::name::RunCommand(std::string_view name, std::string_view argument) {
  if (name != "bo2zm_edit_name") {
    return false;
  }
  auto* profile = Profile();
  if (!profile) {
    return true;
  }
  const bool opened = rex::kernel::xam::ShowTextInput(
      "Username", profile->name(), kMaxLength, [](std::optional<std::string> text) {
        if (!text) {
          REXLOG_INFO("[name] editing cancelled");
          return;
        }
        std::string name = Clean(*text);
        if (name.empty()) {
          REXLOG_INFO("[name] the name entered has no usable characters; the name stays");
          return;
        }
        std::lock_guard lock(g_mutex);
        g_entered = std::move(name);
        g_entered_waiting.store(true, std::memory_order_relaxed);
      });
  if (!opened) {
    REXLOG_INFO("[name] a text input is already open");
  }
  return true;
}
