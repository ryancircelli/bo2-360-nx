/**
 * @file        core/capped_file_sink.h
 * @brief       A log file with a size cap that cuts out the middle, so the start and the end of a session survive
 *
 * spdlog's rotating file sink keeps the newest N size parts and deletes the oldest, so a session that logs a lot
 * loses its beginning (the boot, the first error) first. This sink keeps one file per session instead. When the file
 * passes the cap, CompactHeadTail() rewrites it as: the first cap/4 bytes, one marker line saying how many bytes were
 * left out, and the last cap/4 bytes (both cut at line ends). The file then has room to grow by cap/2 before the next
 * cut, so the cut costs one streamed copy per cap/2 of new log. The rewrite goes to a temporary file that replaces the
 * log, so a crash never leaves a half-written log. Header-only, so it can be tested on the PC.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

namespace rex {

// What a file already lost to cuts, so the marker line can state the running total.
struct LogCutState {
  uint64_t omitted_bytes = 0;  // log text dropped so far, in all cuts
  uint64_t marker_bytes = 0;   // size of the marker line now in the file (it is dropped by the next cut)
};

// Rewrites `path` as head + marker + tail when it is longer than head_bytes + tail_bytes (plus slack for the line cuts).
// Returns true when it cut. On any I/O error the file is left as it was and false is returned.
inline bool CompactHeadTail(const std::filesystem::path& path, size_t head_bytes, size_t tail_bytes,
                            LogCutState& state) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec || size <= uint64_t(head_bytes) + tail_bytes) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  // The head ends after the last line end inside the first head_bytes (the whole span if there is none).
  // Only the last 64 KB before the cut is scanned, so a 4 GB cap does not allocate 1 GB.
  const uint64_t scan_from = head_bytes > (64u << 10) ? head_bytes - (64u << 10) : 0;
  std::vector<char> buffer(size_t(head_bytes - scan_from));
  in.seekg(std::streamoff(scan_from));
  in.read(buffer.data(), std::streamsize(buffer.size()));
  uint64_t head_len = scan_from + uint64_t(in.gcount());
  for (uint64_t i = uint64_t(in.gcount()); i > 0; --i) {
    if (buffer[i - 1] == '\n') {
      head_len = scan_from + i;
      break;
    }
  }
  // The tail starts after the first line end at or after size - tail_bytes (at that byte if it already starts a line).
  uint64_t tail_start = size - tail_bytes;
  if (tail_start > 0) {
    in.clear();
    in.seekg(std::streamoff(tail_start - 1));
    char c = 0;
    in.get(c);
    if (c != '\n') {
      uint64_t pos = tail_start;
      bool found = false;
      while (in.get(c)) {
        if (c == '\n') {
          found = true;
          break;
        }
        ++pos;
      }
      if (found) {
        tail_start = pos + 1;
      }  // else: one line longer than the tail; keep it cut where it is
    }
  }
  if (tail_start < head_len) {
    tail_start = head_len;
  }
  if (tail_start > size) {
    return false;
  }
  const uint64_t omitted_here = tail_start - head_len;
  if (omitted_here == 0) {
    return false;
  }
  const uint64_t total = state.omitted_bytes + omitted_here - std::min(state.marker_bytes, omitted_here);
  const std::string marker = "[log truncated: " + std::to_string(total) +
                             " bytes of this session's log were left out here, to stay under the size cap]\n";

  const std::filesystem::path tmp = path.string() + ".cut";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    std::vector<char> chunk(256 * 1024);
    auto copy_range = [&](uint64_t from, uint64_t to) {
      in.clear();
      in.seekg(std::streamoff(from));
      uint64_t left = to - from;
      while (left > 0 && in) {
        const size_t want = size_t(std::min<uint64_t>(left, chunk.size()));
        in.read(chunk.data(), std::streamsize(want));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
          break;
        }
        out.write(chunk.data(), got);
        left -= uint64_t(got);
      }
      return left == 0;
    };
    const bool ok = copy_range(0, head_len) && out.write(marker.data(), std::streamsize(marker.size())) &&
                    copy_range(tail_start, size);
    out.close();
    if (!ok || out.fail()) {
      std::filesystem::remove(tmp, ec);
      return false;
    }
  }
  in.close();
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  state.omitted_bytes = total;
  state.marker_bytes = marker.size();
  return true;
}

// spdlog sink (not thread safe: the caller serialises) that appends to one file and cuts it when it passes the cap.
class CappedFileSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 public:
  // Throws spdlog::spdlog_ex when the file cannot be opened. Appends to a file that is already there.
  CappedFileSink(std::string path, size_t cap_bytes) : path_(std::move(path)), cap_(cap_bytes) {
    file_.open(path_, false);
    bytes_ = file_.size();
  }

  size_t cap() const { return cap_; }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    file_.write(formatted);
    bytes_ += formatted.size();
    if (bytes_ > std::max(cap_, retry_at_)) {
      Cut();
    }
  }
  void flush_() override { file_.flush(); }

 private:
  void Cut() {
    file_.flush();
    file_.close();
    const bool cut = CompactHeadTail(path_, cap_ / 4, cap_ / 4, state_);
    file_.open(path_, false);  // throws if it cannot; the caller drops that batch and the next write tries again
    bytes_ = file_.size();
    // A failed cut (disk full, rename refused) waits for another cap/4 of log instead of retrying every line.
    retry_at_ = cut ? 0 : bytes_ + cap_ / 4;
  }

  std::string path_;
  size_t cap_;
  size_t bytes_ = 0;
  size_t retry_at_ = 0;
  spdlog::details::file_helper file_;
  LogCutState state_;
};

}  // namespace rex
