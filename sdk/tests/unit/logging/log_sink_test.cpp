// The log file sink's repeat collapsing (core/log_repeat_collapser.h, core/copyable_file_sink.h) and its per-session
// size cap that cuts out the middle (core/capped_file_sink.h).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>

#include "core/capped_file_sink.h"
#include "core/copyable_file_sink.h"
#include "core/log_repeat_collapser.h"

namespace fs = std::filesystem;
using Clock = rex::LogRepeatCollapser::Clock;

namespace {

fs::path TempDir(const char* name) {
  fs::path dir = fs::temp_directory_path() / (std::string("rex_log_sink_test_") + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) {
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("collapser: identical lines after the first are suppressed and counted", "[log]") {
  rex::LogRepeatCollapser c;
  const auto t0 = Clock::now();
  auto v = c.OnLine("core", 2, "boom", t0);
  REQUIRE(v.write);
  REQUIRE(v.summary == 0);
  for (int i = 0; i < 1000; ++i) {
    v = c.OnLine("core", 2, "boom", t0);
    REQUIRE_FALSE(v.write);
    REQUIRE(v.summary == 0);
  }
  v = c.OnLine("core", 2, "other", t0);  // a different line ends the run: summary for the old one, then the new line
  REQUIRE(v.write);
  REQUIRE(v.summary == 1000);
  REQUIRE(c.SummaryName(true) == "core");
  REQUIRE(c.SummaryLevel(true) == 2);
}

TEST_CASE("collapser: same text on another logger or level is a different line", "[log]") {
  rex::LogRepeatCollapser c;
  const auto t0 = Clock::now();
  REQUIRE(c.OnLine("a", 2, "x", t0).write);
  REQUIRE(c.OnLine("b", 2, "x", t0).write);
  REQUIRE(c.OnLine("b", 3, "x", t0).write);
  REQUIRE_FALSE(c.OnLine("b", 3, "x", t0).write);
}

TEST_CASE("collapser: a long run still reports once per interval, and a flush reports the rest", "[log]") {
  rex::LogRepeatCollapser c;
  const auto t0 = Clock::now();
  REQUIRE(c.OnLine("core", 2, "loop", t0).write);
  REQUIRE(c.OnLine("core", 2, "loop", t0 + std::chrono::seconds(1)).summary == 0);
  auto v = c.OnLine("core", 2, "loop", t0 + rex::LogRepeatCollapser::kSummaryInterval);
  REQUIRE_FALSE(v.write);
  REQUIRE(v.summary == 2);
  REQUIRE(c.OnLine("core", 2, "loop", t0 + rex::LogRepeatCollapser::kSummaryInterval).summary == 0);
  REQUIRE(c.TakePending(t0 + rex::LogRepeatCollapser::kSummaryInterval, false) == 0);  // not due yet
  REQUIRE(c.TakePending(t0 + rex::LogRepeatCollapser::kSummaryInterval, true) == 1);   // forced (a flush)
  REQUIRE(c.TakePending(t0 + rex::LogRepeatCollapser::kSummaryInterval, true) == 0);
}

TEST_CASE("file sink: a million identical lines become one line and a count", "[log]") {
  const fs::path dir = TempDir("collapse");
  const fs::path file = dir / "s.log";
  {
    auto sink = std::make_shared<rex::CopyableFileSink>(file.string(), size_t(64) << 20, false);
    sink->set_pattern("[%l] %v");
    spdlog::logger log("t", sink);
    log.set_level(spdlog::level::trace);
    log.info("start");
    for (int i = 0; i < 100000; ++i) {
      log.error("Unhandled guest access violation at 0x1234");
    }
    log.info("end");
  }
  const std::string text = ReadAll(file);
  REQUIRE(Count(text, "Unhandled guest access violation") == 1);
  REQUIRE(text.find("last message repeated 99999 times") != std::string::npos);
  REQUIRE(text.find("[info] start") != std::string::npos);
  REQUIRE(text.find("[info] end") < text.size());
  fs::remove_all(dir);
}

TEST_CASE("file sink: the deferred writer collapses too, and a copy shows the pending count", "[log]") {
  const fs::path dir = TempDir("collapse_deferred");
  const fs::path file = dir / "s.log";
  auto sink = std::make_shared<rex::CopyableFileSink>(file.string(), size_t(64) << 20, true);
  sink->set_pattern("[%l] %v");
  spdlog::logger log("t", sink);
  log.set_level(spdlog::level::trace);
  for (int i = 0; i < 500; ++i) {
    log.warn("same");
  }
  REQUIRE(sink->CopyTo(dir / "copy.log"));
  const std::string copy = ReadAll(dir / "copy.log");
  REQUIRE(Count(copy, "[warning] same") == 1);
  REQUIRE(copy.find("last message repeated 499 times") != std::string::npos);
  sink.reset();
  fs::remove_all(dir);
}

TEST_CASE("capped file: past the cap the middle goes, the head and the tail stay", "[log]") {
  const fs::path dir = TempDir("cap");
  const fs::path file = dir / "s.log";
  const size_t cap = 100 * 1000;
  {
    rex::CappedFileSink sink(file.string(), cap);
    sink.set_pattern("%v");
    for (int i = 0; i < 20000; ++i) {  // about 20 bytes a line: 400 KB, four times the cap
      char line[64];
      snprintf(line, sizeof line, "line-%06d", i);
      sink.log(spdlog::details::log_msg(spdlog::source_loc{}, "t", spdlog::level::info, line));
    }
  }
  const std::string text = ReadAll(file);
  REQUIRE(text.size() <= cap);
  REQUIRE(text.rfind("line-000000\n", 0) == 0);             // the very first line is still the first
  REQUIRE(text.find("line-019999\n") != std::string::npos);  // and the last line is there
  REQUIRE(text.find("[log truncated: ") != std::string::npos);
  REQUIRE(Count(text, "[log truncated: ") == 1);            // later cuts replace the marker, they do not stack
  // Whole lines only on both sides of the cut: every line is "line-NNNNNN" or the marker.
  std::istringstream in(text);
  std::string l;
  int previous = -1;
  bool after_marker = false;
  while (std::getline(in, l)) {
    if (l.rfind("[log truncated: ", 0) == 0) {
      after_marker = true;
      continue;
    }
    REQUIRE(l.size() == 11);
    REQUIRE(l.rfind("line-", 0) == 0);
    const int n = std::stoi(l.substr(5));
    if (previous >= 0 && !after_marker) {
      REQUIRE(n == previous + 1);
    }
    after_marker = false;
    previous = n;
  }
  fs::remove_all(dir);
}

TEST_CASE("capped file: the marker counts everything left out", "[log]") {
  const fs::path dir = TempDir("cap_count");
  const fs::path file = dir / "s.log";
  const size_t cap = 40 * 1000;
  size_t written = 0;
  {
    rex::CappedFileSink sink(file.string(), cap);
    sink.set_pattern("%v");
    for (int i = 0; i < 10000; ++i) {
      char line[64];
      snprintf(line, sizeof line, "line-%06d", i);
      written += strlen(line) + 1;
      sink.log(spdlog::details::log_msg(spdlog::source_loc{}, "t", spdlog::level::info, line));
    }
  }
  const std::string text = ReadAll(file);
  const size_t at = text.find("[log truncated: ");
  REQUIRE(at != std::string::npos);
  const size_t marker_end = text.find('\n', at);
  const std::string marker = text.substr(at, marker_end - at + 1);
  const size_t omitted = std::stoull(marker.substr(strlen("[log truncated: ")));
  REQUIRE(text.size() - marker.size() + omitted == written);  // kept + left out = everything that was logged
  fs::remove_all(dir);
}

TEST_CASE("capped file: a file under the cap is untouched and a copy of the sink's file works", "[log]") {
  const fs::path dir = TempDir("cap_small");
  const fs::path file = dir / "s.log";
  {
    auto sink = std::make_shared<rex::CopyableFileSink>(file.string(), size_t(1) << 20, false);
    sink->set_pattern("%v");
    spdlog::logger log("t", sink);
    log.info("a");
    log.info("b");
    REQUIRE(sink->CopyTo(dir / "c.log"));
    log.info("c");
  }
  REQUIRE(ReadAll(file) == "a\nb\nc\n");
  REQUIRE(ReadAll(dir / "c.log") == "a\nb\n");
  fs::remove_all(dir);
}
