/**
 * @file        input/script/script_events.cpp
 * @brief       Named events that scripted pads can wait for.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/script/script_events.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/sinks/base_sink.h>

#include <rex/logging/api.h>

namespace rex::input {

namespace {

struct Registry {
  std::mutex mutex;
  std::map<std::string, uint64_t, std::less<>> counts;
  std::vector<std::string> watched;
  bool sink_added = false;
};

thread_local int ignore_depth = 0;

Registry& Get() {
  static Registry* registry = new Registry();  // outlives logging at exit
  return *registry;
}

// Passes each log line to OnLogLine. Lines are checked whatever their level,
// as long as the logger lets them through.
class WatchSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    script::OnLogLine(std::string_view(msg.payload.data(), msg.payload.size()));
  }
  void flush_() override {}
};

}  // namespace

void SignalScriptEvent(std::string_view name) {
  auto& registry = Get();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto it = registry.counts.find(name);
  if (it == registry.counts.end()) {
    registry.counts.emplace(std::string(name), 1);
  } else {
    it->second++;
  }
}

namespace script {

uint64_t ScriptEventCount(std::string_view name) {
  auto& registry = Get();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto it = registry.counts.find(name);
  return it == registry.counts.end() ? 0 : it->second;
}

std::string LogEventName(std::string_view text) {
  return "log:" + std::string(text);
}

void WatchLogText(std::string_view text) {
  bool add_sink = false;
  {
    auto& registry = Get();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (std::find(registry.watched.begin(), registry.watched.end(), text) ==
        registry.watched.end()) {
      registry.watched.emplace_back(text);
    }
    add_sink = !registry.sink_added;
    registry.sink_added = true;
  }
  if (add_sink) {
    rex::AddSink(std::make_shared<WatchSink>());
  }
}

IgnoreLogLinesHere::IgnoreLogLinesHere() {
  ignore_depth++;
}

IgnoreLogLinesHere::~IgnoreLogLinesHere() {
  ignore_depth--;
}

void OnLogLine(std::string_view line) {
  if (ignore_depth > 0) {
    return;
  }
  auto& registry = Get();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (const auto& text : registry.watched) {
    if (line.find(text) != std::string_view::npos) {
      registry.counts[LogEventName(text)]++;
    }
  }
}

void ResetScriptEvents() {
  auto& registry = Get();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.counts.clear();
  registry.watched.clear();
}

}  // namespace script
}  // namespace rex::input
