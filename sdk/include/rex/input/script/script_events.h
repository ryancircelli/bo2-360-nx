/**
 * @file        input/script/script_events.h
 * @brief       Named events that scripted pads can wait for.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rex::input {

/// Tells scripted pads that `name` happened, so a "wait_event <name>" step can
/// go on. Cheap and thread safe; call it from anywhere in the game or runtime.
/// Each call counts once: a wait takes one signal, and a signal sent before
/// the wait starts is kept for it.
void SignalScriptEvent(std::string_view name);

namespace script {

/// Times `name` was signalled since start (or the last reset).
uint64_t ScriptEventCount(std::string_view name);

/// The key a log wait counts under: every log line containing `text` signals
/// it. WatchLogText starts the watch (it adds a sink to the SDK logger).
std::string LogEventName(std::string_view text);
void WatchLogText(std::string_view text);

/// Checks one log line against the watched texts. The sink calls this; tests
/// can too.
void OnLogLine(std::string_view line);

/// While one is alive, log lines from this thread are not checked, so a
/// script pad logging what it waits for does not end its own wait.
class IgnoreLogLinesHere {
 public:
  IgnoreLogLinesHere();
  ~IgnoreLogLinesHere();
  IgnoreLogLinesHere(const IgnoreLogLinesHere&) = delete;
  IgnoreLogLinesHere& operator=(const IgnoreLogLinesHere&) = delete;
};

/// Forgets all counts and watched texts (tests).
void ResetScriptEvents();

}  // namespace script
}  // namespace rex::input
