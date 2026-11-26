/**
 * @file        kernel/xam/text_input.h
 * @brief       A line of text from the player, for the app's own menus
 *
 * The game's keyboard (XamShowKeyboardUI) is for the game; this is the same kind of prompt for the app: on PC a text
 * box over the game (ImGui), on Switch the console's software keyboard (libnx swkbd). While it is open, and for a
 * moment after, the game gets no controller input (the input drivers' active check asks TextInputOpen), the way the
 * console's own keyboard takes the controller, so the keys typed and the button that confirms stay out of the game.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace rex::kernel::xam {

// Opens the text input without blocking the caller. done runs on a worker thread with the text (UTF-8, at most
// max_length characters), or nothing when the player cancels or no keyboard can open. false, without calling done,
// while another text input is open.
bool ShowTextInput(std::string title, std::string default_text, size_t max_length,
                   std::function<void(std::optional<std::string>)> done);

// The answer of a keyboard with no UI (XamShowKeyboardUI), as if the player pressed Done: guest_default (the game's
// big-endian text) as is, else fallback_text (the gamertag). Fills buffer (big-endian, buffer_length chars with NUL).
void FillKeyboardAnswer(char16_t* buffer, size_t buffer_length, std::u16string_view guest_default,
                        std::string_view fallback_text);

// Whether a text input is open or closed moments ago (any thread).
bool TextInputOpen();

}  // namespace rex::kernel::xam
