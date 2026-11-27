/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <thread>

#include <rex/kernel/xam/text_input.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

#include <imgui.h>

// Test runs (runq, gate.py, runq live) set REX_HEADLESS=true: on macOS the game also renders to an off-screen
// CAMetalLayer with no window, Dock icon or audio device (src/ui/window_sdl.cpp, windowed_app_context_sdl.cpp).
REXCVAR_DEFINE_BOOL(headless, false, "Kernel",
                    "Don't display any UI, using defaults for prompts as needed. macOS: no window, Dock icon or "
                    "audio device either; frames render to an off-screen Metal layer at the window size");
#include <rex/kernel/xam/private.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

// TODO(gibbed): This is all one giant WIP that seems to work better than the
// previous immediate synchronous completion of dialogs.
//
// The deferred execution of dialog handling is done in such a way that there is
// a pre-, peri- (completion), and post- callback steps.
//
// pre();
// result = completion();
// CompleteOverlapped(result);
// post();
//
// There are games that are batshit insane enough to wait for the X_OVERLAPPED
// to be completed (ie not X_ERROR_PENDING) before creating a listener to
// receive a notification, which is why we have distinct pre- and post- steps.
//
// We deliberately delay the XN_SYS_UI = false notification to give games time
// to create a listener (if they're insane enough do this).

extern std::atomic<int> xam_dialogs_shown_;

class XamDialog : public rex::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(rex::ui::ImGuiDrawer* imgui_drawer) : rex::ui::ImGuiDialog(imgui_drawer) {}

  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

template <typename T>
X_RESULT xeXamDispatchDialog(T* dialog, std::function<X_RESULT(T*)> close_callback,
                             uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback]() -> X_RESULT {
    X_RESULT result;
    dialog->set_close_callback(
        [&dialog, &result, &close_callback]() { result = close_callback(dialog); });
    rex::thread::Fence fence;
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

template <typename T>
X_RESULT xeXamDispatchDialogEx(T* dialog,
                               std::function<X_RESULT(T*, uint32_t&, uint32_t&)> close_callback,
                               uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto run = [dialog, close_callback](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    rex::ui::WindowedAppContext* app_context = REX_KERNEL_STATE()->emulator()->app_context();
    X_RESULT result;
    dialog->set_close_callback([&dialog, &result, &extended_error, &length, &close_callback]() {
      result = close_callback(dialog, extended_error, length);
    });
    rex::thread::Fence fence;
    if (app_context &&
        app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
      ++xam_dialogs_shown_;
      fence.Wait();
      --xam_dialogs_shown_;
    } else {
      delete dialog;
    }
    // dialog should be deleted at this point!
    return result;
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {
  auto pre = []() {
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = true");
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    REXKRNL_DEBUG("xeXamDispatchHeadless: Broadcasting XN_SYS_UI = false");
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    auto result = run_callback();
    post();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

X_RESULT xeXamDispatchHeadlessEx(std::function<X_RESULT(uint32_t&, uint32_t&)> run_callback,
                                 uint32_t overlapped) {
  auto pre = []() {
    // Broadcast XN_SYS_UI = true
    REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  };
  auto post = []() {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    // Broadcast XN_SYS_UI = false
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  };
  if (!overlapped) {
    pre();
    uint32_t extended_error, length;
    auto result = run_callback(extended_error, length);
    post();
    // TODO(gibbed): do something with extended_error/length?
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run_callback, overlapped, pre, post);
    return X_ERROR_IO_PENDING;
  }
}

u32 XamIsUIActive_entry() {
  return xeXamIsUIActive();
}

class MessageBoxDialog : public XamDialog {
 public:
  MessageBoxDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title, std::string description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::Text("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      for (size_t i = 0; i < buttons_.size(); ++i) {
        if (ImGui::Button(buttons_[i].c_str())) {
          chosen_button_ = static_cast<uint32_t>(i);
          ImGui::CloseCurrentPopup();
          Close();
        }
        ImGui::SameLine();
      }
      ImGui::Spacing();
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
};

// https://www.se7ensins.com/forums/threads/working-xshowmessageboxui.844116/
u32 XamShowMessageBoxUI_entry(u32 user_index, mapped_wstring title_ptr, mapped_wstring text_ptr,
                              u32 button_count, mapped_u32 button_ptrs, u32 active_button,
                              u32 flags, mapped_u32 result_ptr, mapped_void overlapped) {
  REXKRNL_DEBUG(
      "XamShowMessageBoxUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
      uint32_t(user_index), title_ptr.guest_address(), text_ptr.guest_address(),
      uint32_t(button_count), button_ptrs.guest_address(), uint32_t(active_button), uint32_t(flags),
      result_ptr.guest_address(), overlapped.guest_address());
  std::string title;
  if (title_ptr) {
    title = rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(title_ptr.guest_address())));
  } else {
    title = "";  // TODO(gibbed): default title based on flags?
  }
  std::string text_str = text_ptr
                             ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                   REX_KERNEL_MEMORY()->TranslateVirtual(text_ptr.guest_address())))
                             : "";

  std::vector<std::string> buttons;
  for (uint32_t i = 0; i < button_count; ++i) {
    uint32_t button_ptr = button_ptrs[i];
    auto button = rex::memory::load_and_swap<std::u16string>(
        REX_KERNEL_MEMORY()->TranslateVirtual(button_ptr));
    buttons.push_back(rex::string::to_utf8(button));
  }

  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    // Auto-pick the focused button.
    auto run = [result_ptr, active_button]() -> X_RESULT {
      *result_ptr = static_cast<uint32_t>(active_button);
      return X_ERROR_SUCCESS;
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    // TODO(benvanik): setup icon states.
    switch (flags & 0xF) {
      case 0:
        // config.pszMainIcon = nullptr;
        break;
      case 1:
        // config.pszMainIcon = TD_ERROR_ICON;
        break;
      case 2:
        // config.pszMainIcon = TD_WARNING_ICON;
        break;
      case 3:
        // config.pszMainIcon = TD_INFORMATION_ICON;
        break;
    }
    auto close = [result_ptr](MessageBoxDialog* dialog) -> X_RESULT {
      *result_ptr = dialog->chosen_button();
      return X_ERROR_SUCCESS;
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
    if (imgui_drawer) {
      result = xeXamDispatchDialog<MessageBoxDialog>(
          new MessageBoxDialog(imgui_drawer, title, text_str, buttons, active_button), close,
          overlapped.guest_address());
    } else {
      // Fallback to headless if no drawer available
      auto run = [result_ptr, active_button]() -> X_RESULT {
        *result_ptr = static_cast<uint32_t>(active_button);
        return X_ERROR_SUCCESS;
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

class KeyboardInputDialog : public XamDialog {
 public:
  KeyboardInputDialog(rex::ui::ImGuiDrawer* imgui_drawer, std::string title,
                      std::string description, std::string default_text, size_t max_length)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    rex::string::copy_truncating(text_buffer_.data(), default_text_, text_buffer_.size());
  }

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override {
    bool first_draw = false;
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
      first_draw = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (description_.size()) {
        ImGui::TextWrapped("%s", description_.c_str());
      }
      if (first_draw) {
        ImGui::SetKeyboardFocusHere();
      }
      if (ImGui::InputText("##body", text_buffer_.data(), text_buffer_.size(),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        text_ = text_buffer_.data();
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      if (ImGui::Button("OK")) {
        text_ = text_buffer_.data();
        cancelled_ = false;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) {
        text_ = "";
        cancelled_ = true;
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::Spacing();
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
};

// No keyboard UI: answer like a player who pressed Done. default_text is guest (big-endian) text, so it is
// copied as is; swapping it again gave games a byte-swapped name. With no default, the profile's gamertag:
// an empty name makes a name prompt ask again forever (Fallout 3's birth scene).
void FillKeyboardAnswer(char16_t* buffer, size_t buffer_length, std::u16string_view guest_default,
                        std::string_view fallback_text) {
  std::memset(buffer, 0, buffer_length * sizeof(char16_t));
  if (!guest_default.empty()) {
    rex::string::copy_truncating(buffer, guest_default, buffer_length);
  } else {
    rex::string::copy_and_swap_truncating(buffer, rex::string::to_utf16(fallback_text), buffer_length);
  }
}

X_RESULT AnswerKeyboardWithoutUI(uint32_t user_index, mapped_wstring default_text, mapped_wstring buffer,
                                 uint32_t buffer_length) {
  const auto* profile = REX_KERNEL_STATE()->user_profile(user_index);
  FillKeyboardAnswer(buffer, buffer_length, default_text.value(), profile ? profile->name() : "Player");
  REXKRNL_INFO("XamShowKeyboardUI: no keyboard UI, answered '{}'",
               rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(buffer)));
  return X_ERROR_SUCCESS;
}

// https://www.se7ensins.com/forums/threads/release-how-to-use-xshowkeyboardui-release.906568/
u32 XamShowKeyboardUI_entry(u32 user_index, u32 flags, mapped_wstring default_text,
                            mapped_wstring title, mapped_wstring description, mapped_wstring buffer,
                            u32 buffer_length, mapped_void overlapped) {
  REXKRNL_DEBUG("XamShowKeyboardUI({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(flags), default_text.guest_address(),
                title.guest_address(), description.guest_address(), buffer.guest_address(),
                uint32_t(buffer_length), overlapped.guest_address());
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  assert_true(overlapped.guest_address() != 0);


  X_RESULT result;
  if (REXCVAR_GET(headless)) {
    auto run = [user_index, default_text, buffer, buffer_length]() -> X_RESULT {
      return AnswerKeyboardWithoutUI(user_index, default_text, buffer, buffer_length);
    };
    result = xeXamDispatchHeadless(run, overlapped.guest_address());
  } else {
    auto close = [buffer, buffer_length](KeyboardInputDialog* dialog, uint32_t& extended_error,
                                         uint32_t& length) -> X_RESULT {
      if (dialog->cancelled()) {
        extended_error = X_ERROR_CANCELLED;
        length = 0;
        return X_ERROR_SUCCESS;
      } else {
        // Zero the output buffer.
        auto text = rex::string::to_utf16(dialog->text());
        rex::string::copy_and_swap_truncating(buffer, text, buffer_length);
        extended_error = X_ERROR_SUCCESS;
        length = 0;
        return X_ERROR_SUCCESS;
      }
    };
    const Runtime* emulator = REX_KERNEL_STATE()->emulator();
    ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();

    // Read and convert title/description/default_text from guest memory as utf16 to utf8 strings
    std::string title_str = title
                                ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                                      REX_KERNEL_MEMORY()->TranslateVirtual(title.guest_address())))
                                : "";
    std::string desc_str =
        description ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                          REX_KERNEL_MEMORY()->TranslateVirtual(description.guest_address())))
                    : "";
    std::string def_text_str =
        default_text ? rex::string::to_utf8(rex::memory::load_and_swap<std::u16string>(
                           REX_KERNEL_MEMORY()->TranslateVirtual(default_text.guest_address())))
                     : "";

    if (imgui_drawer) {
      uint32_t buffer_length_safe = buffer_length + 1;  // +1 for null terminator, just in case
      result = xeXamDispatchDialogEx<KeyboardInputDialog>(
          new KeyboardInputDialog(imgui_drawer, title_str, desc_str, def_text_str,
                                  buffer_length_safe),
          close, overlapped.guest_address());
    } else {
      // Fallback to headless
      auto run = [user_index, default_text, buffer, buffer_length]() -> X_RESULT {
        return AnswerKeyboardWithoutUI(user_index, default_text, buffer, buffer_length);
      };
      result = xeXamDispatchHeadless(run, overlapped.guest_address());
    }
  }
  return result;
}

u32 XamShowDeviceSelectorUI_entry(u32 user_index, u32 content_type, u32 content_flags,
                                  u64 total_requested, mapped_u32 device_id_ptr,
                                  mapped_void overlapped) {
  REXKRNL_DEBUG("XamShowDeviceSelectorUI({:08X}, {:08X}, {:08X}, {:016X}, {:08X}, {:08X})",
                uint32_t(user_index), uint32_t(content_type), uint32_t(content_flags),
                uint64_t(total_requested), device_id_ptr.guest_address(),
                overlapped.guest_address());
  return xeXamDispatchHeadless(
      [device_id_ptr]() -> X_RESULT {
        // NOTE: 0x00000001 is our dummy device ID from xam_content.cc
        *device_id_ptr = 0x00000001;
        return X_ERROR_SUCCESS;
      },
      overlapped.guest_address());
}

void XamShowDirtyDiscErrorUI_entry(u32 user_index) {
  REXKRNL_ERROR("XamShowDirtyDiscErrorUI called! user_index={}", uint32_t(user_index));
  REXKRNL_ERROR("This indicates a disc/file read error - check that all game files exist");

  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (imgui_drawer) {
    xeXamDispatchDialog<MessageBoxDialog>(
        new MessageBoxDialog(imgui_drawer, "Disc Read Error",
                             "There's been an issue reading content from the game disc.\nThis is "
                             "likely caused by bad or unimplemented file IO calls.",
                             {"OK"}, 0),
        [](MessageBoxDialog*) -> X_RESULT { return X_ERROR_SUCCESS; }, 0);
  } else {
    // No UI available - log prominently and pause to let user see the error
    REXKRNL_ERROR("===========================================");
    REXKRNL_ERROR("FATAL: Disc Read Error (no UI to display)");
    REXKRNL_ERROR("Check that all game content files are present");
    REXKRNL_ERROR("Missing files or bad mounts cause this error");
    REXKRNL_ERROR("===========================================");
  }
  // This is death, and should never return. Hard exit like a window close (ReXApp::OnClosing): exit(1)
  // ran static destructors under live guest, GPU and frame-dump threads.
  rex::FlushLogging();
  std::_Exit(1);
}

u32 XamShowPartyUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

// Offline: no marketplace. Validate like xenia-canary, then complete as if the dialog closed.
u32 XamShowMarketplaceDownloadItemsUI_entry(u32 user_index, u32 ui_type, mapped_u64 offers,
                                            u32 num_offers, mapped_u32 hresult_ptr,
                                            u32 overlapped_ptr) {
  if (user_index >= 4 || !offers || !num_offers || num_offers > 6) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (hresult_ptr) *hresult_ptr = 0;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamShowCommunitySessionsUI_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

// Offline guide panel (stub-noref-research specs/stub-specs.md #1/#2; checks after xenia-canary):
// no profile -> NO_SUCH_USER, UI already up -> ACCESS_DENIED, else "opened and dismissed" = 0.
u32 XamShowOfflinePanelUI_entry(u32 user_index) {
  if (user_index >= 4 || !REX_KERNEL_STATE()->user_profile(user_index)) return X_ERROR_NO_SUCH_USER;
  if (xeXamIsUIActive()) return X_ERROR_ACCESS_DENIED;
  REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
  REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  return X_ERROR_SUCCESS;
}

u32 XamShowNuiTroubleshooterUI_entry(u32, u32, u32);

// xenia-canary 2da2519 xam_ui.cc (BSD-3-Clause): fail an unavailable
// subscription flow. GoldUpgrade has three arguments, no overlapped pointer.
u32 XamShowGoldUpgradeUI_entry(u32, u32, u32) { return X_ERROR_FUNCTION_FAILED; }
u32 XamShowMultiplayerUpgradeUI_entry(u32 user) {
  return XamShowGoldUpgradeUI_entry(user, 6, 0);
}
u32 XamShowNuiGuideUI_entry(u32, u32) { return X_ERROR_ACCESS_DENIED; }
u32 XamShowNuiHardwareRequiredUI_entry(u32 flags) {
  if (flags) return X_ERROR_INVALID_PARAMETER;
  return XamShowNuiTroubleshooterUI_entry(0xFF, 0, 0x400000);
}
// Preserve Canary's argument and profile checks; there is no marketplace
// backend. A valid request follows our existing offline guide close path.
u32 XamShowMarketplaceUIEx_entry(u32 user, u32 type, u64, u32, u32, u32, u32, u32) {
  if (user >= 4 || type >= 25) return X_ERROR_INVALID_PARAMETER;
  return XamShowOfflinePanelUI_entry(user);
}
u32 XamShowMarketplaceUI_entry(u32 user, u32 type, u64 offer, u32 offer_type,
                               u32 category, u32 title) {
  return XamShowMarketplaceUIEx_entry(user, type, offer, offer_type, category, 0, 0, title);
}

// The app's text input (rex/kernel/xam/text_input.h).
namespace {

std::atomic<bool> text_input_open_{false};
std::atomic<int64_t> text_input_closed_ms_{std::numeric_limits<int64_t>::min() / 2};
constexpr int64_t kTextInputQuietMs = 300;  // no game input this long after closing

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The platform's keyboard, on the worker thread; nothing when cancelled or unavailable.
std::optional<std::string> RunTextInput(const std::string& title, const std::string& default_text,
                                        size_t max_length) {
#if REX_PLATFORM_SWITCH
  SwkbdConfig config;
  if (R_FAILED(swkbdCreate(&config, 0))) {
    REXKRNL_WARN("ShowTextInput: the software keyboard could not be created");
    return std::nullopt;
  }
  swkbdConfigMakePresetDefault(&config);
  swkbdConfigSetHeaderText(&config, title.c_str());
  swkbdConfigSetGuideText(&config, title.c_str());
  swkbdConfigSetInitialText(&config, default_text.c_str());
  swkbdConfigSetStringLenMax(&config, static_cast<uint32_t>(max_length));
  std::vector<char> out(max_length * 4 + 1, '\0');  // UTF-8: up to 4 bytes a character
  const Result result = swkbdShow(&config, out.data(), out.size());
  swkbdClose(&config);
  if (R_FAILED(result)) {
    return std::nullopt;  // cancelled, or the applet did not run
  }
  return std::string(out.data());
#else
  const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  ui::ImGuiDrawer* imgui_drawer = emulator ? emulator->imgui_drawer() : nullptr;
  rex::ui::WindowedAppContext* app_context = emulator ? emulator->app_context() : nullptr;
  if (!imgui_drawer || !app_context) {
    REXKRNL_WARN("ShowTextInput: no ImGui drawer to show a text box");
    return std::nullopt;
  }
  auto* dialog = new KeyboardInputDialog(imgui_drawer, title, "", default_text, max_length + 1);
  std::optional<std::string> text;
  dialog->set_close_callback([&dialog, &text]() {
    if (!dialog->cancelled()) {
      text = dialog->text();
    }
  });
  rex::thread::Fence fence;
  if (app_context->CallInUIThreadSynchronous([&dialog, &fence]() { dialog->Then(&fence); })) {
    ++xam_dialogs_shown_;
    fence.Wait();
    --xam_dialogs_shown_;
  } else {
    delete dialog;
  }
  return text;
#endif
}

}  // namespace

bool ShowTextInput(std::string title, std::string default_text, size_t max_length,
                   std::function<void(std::optional<std::string>)> done) {
  if (text_input_open_.exchange(true)) {
    return false;
  }
  // One thread object, joined before the next input, never detached and never destroyed: on Horizon
  // std::thread::detach() throws (newlib has no pthread_detach) and the game closes. The previous input's thread has
  // ended or is about to (it cleared text_input_open_ before calling done).
  static std::thread* thread = nullptr;
  if (thread && thread->joinable()) {
    thread->join();
  }
  try {
    auto body = [title = std::move(title), default_text = std::move(default_text), max_length,
                 done = std::move(done)]() {
      auto text = RunTextInput(title, default_text, max_length);
      text_input_closed_ms_.store(SteadyMs());
      text_input_open_.store(false);
      done(std::move(text));
    };
    if (thread) {
      *thread = std::thread(std::move(body));
    } else {
      thread = new std::thread(std::move(body));
    }
  } catch (const std::system_error&) {
    text_input_open_.store(false);
    return false;
  }
  return true;
}

bool TextInputOpen() {
  return text_input_open_.load(std::memory_order_relaxed) ||
         SteadyMs() - text_input_closed_ms_.load(std::memory_order_relaxed) < kTextInputQuietMs;
}

// Delegations from xenia-canary 445ce54 xam_ui.cc (BSD-3-Clause); rewritten. We have no Kinect, so the
// Nui variants skip NuiHudCheck(tracking_id) and run the plain dialog.
u32 XamShowMessageBoxUIEx_entry(u32 user_index, mapped_wstring title_ptr, mapped_wstring text_ptr,
                                u32 button_count, mapped_u32 button_ptrs, u32 active_button,
                                u32 flags, u32 unused, mapped_u32 result_ptr,
                                mapped_void overlapped) {
  return XamShowMessageBoxUI_entry(user_index, title_ptr, text_ptr, button_count, button_ptrs,
                                   active_button, flags, result_ptr, overlapped);
}

// XUI heap (spec round 2: XuiAlloc/XuiFree = system heap). Sizes tracked so XuiRealloc can copy.
namespace {
std::mutex xui_heap_mutex_;
std::unordered_map<uint32_t, uint32_t> xui_heap_;
}  // namespace

u32 XuiAlloc_entry(u32 size) {
  if (!size) return 0;
  uint32_t addr = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (addr) {
    std::lock_guard<std::mutex> lock(xui_heap_mutex_);
    xui_heap_[addr] = size;
  }
  return addr;
}
void XuiFree_entry(u32 ptr) {
  if (!ptr) return;
  {
    std::lock_guard<std::mutex> lock(xui_heap_mutex_);
    if (!xui_heap_.erase(ptr)) return;  // not ours: ignore rather than corrupt the heap
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(ptr);
}
u32 XuiRealloc_entry(u32 ptr, u32 size) {
  if (!ptr) return XuiAlloc_entry(size);
  if (!size) {
    XuiFree_entry(ptr);
    return 0;
  }
  uint32_t old_size = 0;
  {
    std::lock_guard<std::mutex> lock(xui_heap_mutex_);
    auto it = xui_heap_.find(ptr);
    if (it == xui_heap_.end()) return 0;
    old_size = it->second;
  }
  uint32_t fresh = XuiAlloc_entry(size);
  if (!fresh) return 0;  // original block stays valid, like realloc
  std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(fresh), REX_KERNEL_MEMORY()->TranslateVirtual(ptr),
              std::min(old_size, uint32_t(size)));
  XuiFree_entry(ptr);
  return fresh;
}
// Duplicates a guest UTF-16 string into an XuiAlloc block; E_OUTOFMEMORY when allocation fails.
u32 XuiCopyString_entry(mapped_u32 dst, u32 src) {
  if (!dst || !src) return 0x80070057;  // E_INVALIDARG
  const auto* chars = reinterpret_cast<const uint8_t*>(REX_KERNEL_MEMORY()->TranslateVirtual(src));
  uint32_t n = 0;
  while (chars[n * 2] || chars[n * 2 + 1]) ++n;
  uint32_t out = XuiAlloc_entry((n + 1) * 2);
  if (!out) return 0x8007000E;
  std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(out), chars, (n + 1) * 2);
  *dst = out;
  return 0;
}
u32 XuiDefault_True_entry() { return 1; }
u32 XuiDefault_Zero_entry() { return 0; }
// Message boxes, no UI: forward to the shared path (default button chosen, XN_SYS_UI open/close).
u32 XamShowMessageBox_entry(u32 user_index, mapped_wstring title, mapped_wstring text, u32 button_count,
                            mapped_u32 buttons, u32 active_button, u32 flags, mapped_u32 result,
                            mapped_void overlapped) {
  return XamShowMessageBoxUI_entry(user_index, title, text, button_count, buttons, active_button, flags,
                                   result, overlapped);
}
// Synchronous form: no overlapped, so the headless dispatcher completes before returning.
u32 XamShowAndWaitForMessageBoxEx_entry(u32 user_index, mapped_wstring title, mapped_wstring text,
                                        u32 button_count, mapped_u32 buttons, u32 active_button, u32 flags,
                                        u32 unused, mapped_u32 result) {
  return XamShowMessageBoxUIEx_entry(user_index, title, text, button_count, buttons, active_button, flags,
                                     unused, result, mapped_void(0));
}
void XamShowNuiDirtyDiscErrorUI_entry(u32 user_index) {
  XamShowDirtyDiscErrorUI_entry(user_index);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamIsUIActive, rex::kernel::xam::XamIsUIActive_entry)
REX_EXPORT(__imp__XamShowMessageBoxUI, rex::kernel::xam::XamShowMessageBoxUI_entry)
REX_EXPORT(__imp__XamShowKeyboardUI, rex::kernel::xam::XamShowKeyboardUI_entry)
REX_EXPORT(__imp__XamShowDeviceSelectorUI, rex::kernel::xam::XamShowDeviceSelectorUI_entry)
REX_EXPORT(__imp__XamShowDirtyDiscErrorUI, rex::kernel::xam::XamShowDirtyDiscErrorUI_entry)
REX_EXPORT(__imp__XamShowPartyUI, rex::kernel::xam::XamShowPartyUI_entry)
REX_EXPORT(__imp__XamShowCommunitySessionsUI, rex::kernel::xam::XamShowCommunitySessionsUI_entry)
REX_EXPORT(__imp__XamShowMessageBoxUIEx, rex::kernel::xam::XamShowMessageBoxUIEx_entry)

REX_EXPORT(__imp__XamIsMessageBoxActive, rex::kernel::xam::XamIsUIActive_entry)
REX_EXPORT(__imp__XamShowMessageBox, rex::kernel::xam::XamShowMessageBox_entry)
REX_EXPORT(__imp__XamShowAndWaitForMessageBoxEx, rex::kernel::xam::XamShowAndWaitForMessageBoxEx_entry)
REX_EXPORT(__imp__XamShowNuiDirtyDiscErrorUI, rex::kernel::xam::XamShowNuiDirtyDiscErrorUI_entry)
REX_EXPORT(__imp__XuiAlloc, rex::kernel::xam::XuiAlloc_entry)
REX_EXPORT(__imp__XuiFree, rex::kernel::xam::XuiFree_entry)
REX_EXPORT(__imp__XuiRealloc, rex::kernel::xam::XuiRealloc_entry)
REX_EXPORT(__imp__XuiCopyString, rex::kernel::xam::XuiCopyString_entry)
REX_EXPORT(__imp__XuiDefault_True, rex::kernel::xam::XuiDefault_True_entry)
REX_EXPORT(__imp__XuiDefault_False, rex::kernel::xam::XuiDefault_Zero_entry)
REX_EXPORT(__imp__XuiDefault_IntegerZero, rex::kernel::xam::XuiDefault_Zero_entry)
REX_EXPORT(__imp__XuiHandleIsValid, rex::kernel::xam::XuiDefault_Zero_entry)  // no XUI objects exist
REX_EXPORT_STUB_RETURN(__imp__XamIsGuideDisabled, 0);  // FALSE: no guide/UI state emulated
REX_EXPORT(__imp__XamIsNuiUIActive, rex::kernel::xam::XamIsUIActive_entry)
REX_EXPORT_STUB_RETURN(__imp__XamIsSysUiInvokedByTitle, 0);  // FALSE: no guide/UI state emulated
REX_EXPORT_STUB_RETURN(__imp__XamIsSysUiInvokedByXenonButton, 0);  // FALSE: no guide/UI state emulated
REX_EXPORT_STUB_RETURN(__imp__XamIsUIThread, 0);  // FALSE: no guide/UI state emulated
REX_EXPORT_STUB_RETURN(__imp__XamNavigate, 0x65B);
REX_EXPORT_STUB_RETURN(__imp__XamNavigateBack, 0x65B);
REX_EXPORT_STUB_RETURN(__imp__XamOverrideHudOpenType, 0);
REX_EXPORT_STUB_RETURN(__imp__XamShowAchievementDetailsUI, 0x4C7);
REX_EXPORT(__imp__XamShowAchievementsUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowAchievementsUIEx, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowAvatarAwardGamesUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowAvatarAwardsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowAvatarMiniCreatorUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowBadDiscErrorUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowBeaconsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowBrandedKeyboardUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowChangeGamerTileUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowComplaintUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowCreateProfileUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowCreateProfileUIEx, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowCsvTransitionUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowCustomMessageComposeUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowCustomPlayerListUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowDirectAcquireUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowEditProfileUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowFirstRunWelcomeUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowFitnessBodyProfileUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowFitnessClearUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowFitnessWarnAboutPrivacyUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowFitnessWarnAboutTimeUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowFofUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowForcedNameChangeUI, 0x4C7);
REX_EXPORT(__imp__XamShowFriendRequestUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT(__imp__XamShowFriendsUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowFriendsUIp, 0x4C7);
REX_EXPORT(__imp__XamShowGameInviteUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT(__imp__XamShowGameVoiceChannelUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowGamerCardUI, 0x4C7);
REX_EXPORT(__imp__XamShowGamerCardUIForXUID, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowGamerCardUIForXUIDp, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowGamesUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowGenericOnlineAppUI, 0x4C7);
REX_EXPORT(__imp__XamShowGoldUpgradeUI, rex::kernel::xam::XamShowGoldUpgradeUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowGraduateUserUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowGuideUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowJoinPartyUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowJoinSessionByIdInProgressUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowJoinSessionInProgressUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowKeyboardUIMessenger, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowLiveSignupUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowLiveUpsellUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowLiveUpsellUIEx, 0x32);  // offline: not supported
REX_EXPORT(__imp__XamShowMarketplaceDownloadItemsUI,
           rex::kernel::xam::XamShowMarketplaceDownloadItemsUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowMarketplaceGetOrderReceipts, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowMarketplacePurchaseOrderUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowMarketplacePurchaseOrderUIEx, 0x4C7);
REX_EXPORT(__imp__XamShowMarketplaceUI, rex::kernel::xam::XamShowMarketplaceUI_entry)
REX_EXPORT(__imp__XamShowMarketplaceUIEx, rex::kernel::xam::XamShowMarketplaceUIEx_entry)
REX_EXPORT(__imp__XamShowMessageComposeUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT(__imp__XamShowMessagesUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowMessagesUIEx, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowMessengerUI, 0x32);  // offline: not supported
REX_EXPORT(__imp__XamShowMultiplayerUpgradeUI, rex::kernel::xam::XamShowMultiplayerUpgradeUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowNetworkStorageSyncUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiAchievementsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiCommunitySessionsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiControllerRequiredUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiDeviceSelectorUI, 0x32);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiFriendRequestUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiFriendsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiGameInviteUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiGamerCardUIForXUID, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiGamesUI, 0x4C7);
REX_EXPORT(__imp__XamShowNuiGuideUI, rex::kernel::xam::XamShowNuiGuideUI_entry)
REX_EXPORT(__imp__XamShowNuiHardwareRequiredUI, rex::kernel::xam::XamShowNuiHardwareRequiredUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiJoinSessionInProgressUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiMarketplaceDownloadItemsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiMarketplaceUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiMessageBoxUI, 0x32);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiMessagesUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiPartyUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiSigninUI, 0x32);
REX_EXPORT_STUB_RETURN(__imp__XamShowNuiVideoRichPresenceUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowOptionalMediaUpdateRequiredUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowOptionalMediaUpdateRequiredUIEx, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowOptionsUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowPamUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowPartyInviteUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowPartyJoinInProgressUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowPasscodeVerifyUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowPasscodeVerifyUIEx, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowPaymentOptionsUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowPersonalizationUI, 0x4C7);
REX_EXPORT(__imp__XamShowPlayerReviewUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT(__imp__XamShowPlayersUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowPrivateChatInviteUI, 0x32);  // offline: not supported
REX_EXPORT(__imp__XamShowQuickChatUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowQuickChatUIp, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowQuickLaunchUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowRecentMessageUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowRecentMessageUIEx, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowReputationUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowSigninUIEx, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowSignupCreditCardUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowSocialPostUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowStorePickerUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowTFAUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowTermsOfUseUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowUpdaterUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowVideoChatInviteUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowVideoRichPresenceUI, 0x4C7);
REX_EXPORT(__imp__XamShowVoiceMailUI, rex::kernel::xam::XamShowOfflinePanelUI_entry)
REX_EXPORT_STUB_RETURN(__imp__XamShowVoiceSettingsUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamShowWhatsOnUI, 0x32);  // offline: not supported
REX_EXPORT_STUB_RETURN(__imp__XamShowWordRegisterUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamSysUiDisableAutoClose, 0);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_Clear, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_Construct, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_Destruct, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_SetBool, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_SetColorFromUint, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_SetFloat, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_SetInt, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_SetString, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUIElementPropVal_SetUint, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XUITimeline_Run, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamLoaderIsKinectUIPreferredForLogonTitle, 0);
REX_EXPORT_STUB_RETURN(__imp__XamResumeUpdaterUI, 0);
REX_EXPORT_STUB_RETURN(__imp__XamTestShowNuiTroubleshooterUI, 0x4C7);
REX_EXPORT_STUB_RETURN(__imp__XamUIThreadDisableFontPatching, 0);
REX_EXPORT_STUB_RETURN(__imp__XamUIThreadEnableFontPatching, 0);
REX_EXPORT_STUB_RETURN(__imp__XuiAnimRun, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiApplyLocale, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiAttachTextureBrush, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiBroadcastMessage, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiBrushGetTexture, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiBubbleMessage, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiClassDerivesFrom, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlAttachVisual, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlGetItemAssociation, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlGetNavigation, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlGetVisual, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlIsBackButton, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlIsNavButton, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlPlayOptionalVisual, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlSetItemAssociation, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiControlWantsUnfocusedInput, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiCreateObject, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiCreateSolidBrush, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiCreateTextureBrush, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiDestroyBrush, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiDestroyObject, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiDrawShape, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiDynamicCast, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiEditGetTextLimit, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiEditSetTextFormatInfo, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiEditSetTextLimit, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementAddChild, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementBeginRender, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementDisallowRecursiveTimelineControl, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementDiscardResources, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementEnableInput, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementEndRender, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementFindNamedFrame, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetChildById, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetDescendantById, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetFirstChild, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetFocusUser, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetFullXForm, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetId, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetLastChild, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetNext, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetOpacity, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetParent, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetPivot, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetPosition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetPrev, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetScale, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetScreenPositionCenter, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetTimeline, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetUserData, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetUserFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementGetXForm, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementHasFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementInitFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementInitUserFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementInputEnabled, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementInsertChild, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementIsDescendant, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementLayoutTree, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementPlayNamedFrames, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementPlayTimeline, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementRenderChildren, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetBounds, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetHittable, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetOpacity, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetPivot, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetPosition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetRotation, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetScale, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetUserData, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementSetUserFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementTreeGetFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementTreeHasFocus, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiElementUnlink, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFigureClose, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFigureGetShape, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFigureSetFill, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFigureSetShape, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFigureSetTexture, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFillRect, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFindClass, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFreeStringTable, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFreeUnusedTextures, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiFreeVisuals, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiGetBaseClass, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiGetBaseObject, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiGetClass, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiGetObjectClass, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiGetOuter, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiImageElementGetImagePath, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiImageElementSetImagePath, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiInit, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiIsInstanceOf, 0);
REX_EXPORT_STUB_RETURN(__imp__XuiKillTimer, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiListEnableItemOverride, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiListGetDefaultItemSize, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiLoadFromBinary, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiLoadStringTableFromFile, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiLoadVisualFromBinary, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiLookupStringTable, 0);
REX_EXPORT_STUB_RETURN(__imp__XuiLookupStringTableByIndex, 0);
REX_EXPORT_STUB_RETURN(__imp__XuiMatrixMultiply, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiNavButtonGetPressPath, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiObjectFromHandle, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiObjectGetProperty, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiObjectGetPropertyDef, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiObjectGetPropertyId, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiObjectSetProperty, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiPNGTextureLoader, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiProcessInput, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiProgressBarGetRange, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiProgressBarGetValue, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiProgressBarSetRange, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiProgressBarSetValue, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRegisterClass, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderBegin, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderCreateDC, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderDCDeviceChanged, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderDestroyDC, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderEnd, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderGetBackBufferSize, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderGetColorSpace, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderGetDevice, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderGetViewTransform, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderGetXuiDevice, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderInit, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderInitShared, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderPresent, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderRestoreState, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderSetViewTransform, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiRenderUninit, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceClose, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceComposeLocator, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceGetBuffer, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceGetPackageEntryCount, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceGetPackageEntryInfo, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceGetTotalSize, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceLoadAll, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceLocatorIsAbsolute, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceOpen, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceOpenAndVerifyNoLoc, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceOpenNoLoc, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceOpenPackage, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceRead, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceReleasePackage, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiResourceSeek, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneCreate, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneEnableTransitionDependency, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneInterruptTransitions, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneIsInTransition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneNavigateBack, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneNavigateBackToFirst, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneNavigateFirst, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSceneNavigateForward, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiScenePlayBackFromTransition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiScenePlayBackToTransition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiScenePlayFromTransition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiScenePlayToTransition, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSelectBrush, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSelectBrushEx, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSendMessage, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSetLocale, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSetMessageFilter, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSetTimer, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSliderGetRange, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSliderGetStep, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSliderGetValue, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSliderSetRange, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSliderSetStep, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiSliderSetValue, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTabSceneGetCount, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTabSceneGetCurrentTab, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTabSceneGoto, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTextElementGetText, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTextElementMeasureText, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTextElementSetText, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiTimersRun, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiUninit, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiUnregisterClass, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiVec2TransformCoord, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiVisualCreateInstance, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiVisualGetBasePath, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XuiVisualSetBasePath, 0x80004005);
