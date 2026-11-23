/**
 * @file        core/log_repeat_collapser.h
 * @brief       Collapses runs of identical log lines into "last message repeated N times"
 *
 * A fault loop can write the same line millions of times a second. The log file sink (copyable_file_sink.h) asks this
 * class about every line: the first of a run is written, the identical lines after it are suppressed and counted, and
 * the count comes back as a summary to write, either when a different line arrives or, during a long run, once per
 * kSummaryInterval so the log still shows the loop is going on. Same logger name, level and text = identical.
 * Plain logic with no spdlog types, so it can be tested alone. Not thread safe: the sink calls it under its mutex.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace rex {

class LogRepeatCollapser {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds kSummaryInterval{5};

  struct Verdict {
    bool write = true;       // write this line
    uint64_t summary = 0;    // > 0: first write "last message repeated <summary> times" for the PREVIOUS line
  };

  // Decides what to do with a line. When a summary is due for the previous line, PreviousName()/PreviousLevel()
  // still describe that line until the next call.
  Verdict OnLine(std::string_view name, int level, std::string_view text, Clock::time_point now) {
    Verdict v;
    if (have_last_ && level == level_ && name == name_ && text == text_) {
      ++pending_;
      v.write = false;
      if (now - last_emit_ >= kSummaryInterval) {
        v.summary = pending_;
        pending_ = 0;
        last_emit_ = now;
      }
      return v;
    }
    v.summary = pending_;
    prev_name_ = name_;
    prev_level_ = level_;
    pending_ = 0;
    have_last_ = true;
    name_.assign(name);
    level_ = level;
    text_.assign(text);
    last_emit_ = now;
    return v;
  }

  // Name and level of the line a summary returned by OnLine() or TakePending() is about.
  const std::string& SummaryName(bool from_previous) const { return from_previous ? prev_name_ : name_; }
  int SummaryLevel(bool from_previous) const { return from_previous ? prev_level_ : level_; }

  // For a timer or a flush: the suppressed count so far, if `force` or a summary is due. 0 = nothing to write. The
  // summary is about the last line seen (SummaryName(false)).
  uint64_t TakePending(Clock::time_point now, bool force) {
    if (pending_ == 0 || (!force && now - last_emit_ < kSummaryInterval)) {
      return 0;
    }
    const uint64_t n = pending_;
    pending_ = 0;
    last_emit_ = now;
    return n;
  }

  static std::string SummaryText(uint64_t n) {
    return "last message repeated " + std::to_string(n) + (n == 1 ? " time" : " times");
  }

 private:
  bool have_last_ = false;
  std::string name_;
  int level_ = 0;
  std::string text_;
  std::string prev_name_;
  int prev_level_ = 0;
  uint64_t pending_ = 0;
  Clock::time_point last_emit_{};
};

}  // namespace rex
