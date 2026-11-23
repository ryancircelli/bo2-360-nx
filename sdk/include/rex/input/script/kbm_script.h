/**
 * @file        rex/input/script/kbm_script.h
 * @brief       Scripted keyboard and mouse for tests (kbm_test_script).
 *
 * The script posts real SDL key, mouse-motion, button and wheel events to the
 * game window, so they reach every window listener (the overlays and the
 * keyboard-and-mouse driver) exactly like a person's input.
 *
 * One step per line, '#' starts a comment:
 *   wait_log "<text>" [timeout_s]   wait for a log line containing text
 *   sleep <seconds>
 *   key <Name>[+<Name>...] [hold_ms] press and release (SDL key names:
 *                                    Tab, Return, Escape, Left, Left Shift...)
 *   type <characters>               one key press per character
 *   move <target>                   move the pointer
 *   click <target> [right|thumb]    move, then press and release a button
 *   wheel <target> <detents>        scroll (positive = up)
 *   drag <target> <from> <to>       left-drag across the target, from and to
 *                                   are fractions of its width
 *   mark <label>                    logs "[kbm] mark <label>" (screenshots)
 * A target is a name the lookup resolves (the Guide's controls, such as
 * "blade:System", "row:Volume", "back", "close") or "@x,y" in fractions of
 * the window.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>
#include <string_view>

namespace rex::input::script {

/// Finds a named target: x0, y0, x1, y1 in fractions of the window.
using KbmTargetLookup = std::function<bool(std::string_view name, float rect[4])>;

/// Starts the script named by the kbm_test_script setting, if any, on its own
/// thread. Returns false when no script is set.
bool StartKbmScriptIfSet(KbmTargetLookup lookup);

}  // namespace rex::input::script
