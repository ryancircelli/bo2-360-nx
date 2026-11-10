// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// PNG captures of the game image (see bo2zm_native_capture.h).
//
// The PNG is written uncompressed, as deflate "stored" blocks: the SDK does not include stb_image_write, and an
// uncompressed file is good enough for tests. A 1280x720 capture takes about 2.8 MB.

#include "bo2zm_native_capture.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(bo2zm_capture_every_s, 0, "BO2",
                     "Save a PNG capture of the game image every N seconds in captures/ next to the executable (0 = "
                     "never; tests only)")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(bo2zm_capture_max, 20, "BO2", "Maximum captures per run")
    .range(1, 1000);
// Lets a test capture only the match itself (a match is usually loaded about 40 s after start), for example one
// capture every 2 s while the camera turns.
REXCVAR_DEFINE_INT32(bo2zm_capture_from_s, 0, "BO2",
                     "Test captures (bo2zm_capture_every_s): seconds before the first one counts down")
    .range(0, 3600);

// A quick run of captures for what changes from frame to frame (blinking models, popping): "S:N:MS" takes N of
// them MS milliseconds apart from S seconds after start-up, named burst_00.png and on.
REXCVAR_DEFINE_STRING(bo2zm_capture_burst, "", "BO2",
                      "Test captures: \"S:N:MS\", N captures MS milliseconds apart from S seconds after start-up "
                      "(captures/burst_NN.png; empty = none)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::capture {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;
std::function<rex::ui::Presenter*()> g_get_presenter;  // for RequestCapture
// The captures RequestCapture asked for: their names and when each falls due, sorted by that time.
struct Requested {
  std::string label;
  std::chrono::steady_clock::time_point due;
};
std::deque<Requested> g_requested;

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

void PutBe32(std::vector<uint8_t>& output, uint32_t value) {
  output.push_back(uint8_t(value >> 24));
  output.push_back(uint8_t(value >> 16));
  output.push_back(uint8_t(value >> 8));
  output.push_back(uint8_t(value));
}

void Chunk(std::vector<uint8_t>& output, const char type[4], const std::vector<uint8_t>& data) {
  PutBe32(output, uint32_t(data.size()));
  const size_t start_type = output.size();
  output.insert(output.end(), type, type + 4);
  output.insert(output.end(), data.begin(), data.end());
  const uint32_t crc =
      Crc32(output.data() + start_type, output.size() - start_type, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
  PutBe32(output, crc);
}

// RGBX (R8 G8 B8 X8, like rex::ui::RawImage) to 8-bit RGB PNG.
std::vector<uint8_t> EncodePng(const rex::ui::RawImage& image) {
  const uint32_t width = image.width;
  const uint32_t height = image.height;
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);  // no filter
    const uint8_t* row = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < width; ++x) {
      raw.push_back(row[x * 4]);
      raw.push_back(row[x * 4 + 1]);
      raw.push_back(row[x * 4 + 2]);
    }
  }

  // zlib with "stored" blocks and Adler-32 at the end.
  std::vector<uint8_t> zlib = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t chunk = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + chunk == raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(uint8_t(chunk));
    zlib.push_back(uint8_t(chunk >> 8));
    zlib.push_back(uint8_t(~chunk));
    zlib.push_back(uint8_t(~chunk >> 8));
    zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + chunk);
    pos += chunk;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PutBe32(zlib, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBe32(ihdr, width);
  PutBe32(ihdr, height);
  ihdr.push_back(8);  // bits per channel
  ihdr.push_back(2);  // RGB
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // adaptive filter
  ihdr.push_back(0);  // no interlacing
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", zlib);
  Chunk(png, "IEND", {});
  return png;
}

// Captures go to $BO2_LOG_DIR/captures when the launcher gives one (the packed Mac app: a bundle must not be written to),
// otherwise to captures/ beside the executable.
std::filesystem::path CaptureFolder() {
  if (const char* d = std::getenv("BO2_LOG_DIR"); d && *d) return std::filesystem::path(d) / "captures";
  return rex::filesystem::GetExecutableFolder() / "captures";
}

// The capture thread. It takes one capture every every_s seconds (starting after bo2zm_capture_from_s, at most max
// of them; every_s 0 means none), plus the captures RequestCapture asks for, each when it falls due and under its own
// name.
void Loop(std::function<rex::ui::Presenter*()> get_presenter, int every_s, int max) {
  const std::filesystem::path folder = CaptureFolder();
  const auto start = std::chrono::steady_clock::now();
  auto next = start + std::chrono::seconds(REXCVAR_GET(bo2zm_capture_from_s)) + std::chrono::seconds(every_s);
  int n = 0;
  for (;;) {
    std::string requested;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      const auto due = [] {
        return !g_requested.empty() && g_requested.front().due <= std::chrono::steady_clock::now();
      };
      const auto has_work = [&] { return g_stop || due(); };
      const bool periodic = every_s > 0 && n < max;
      // Until the next periodic capture, the first request that falls due, or a new request (which may fall due
      // sooner).
      auto wake = periodic ? next : std::chrono::steady_clock::time_point::max();
      if (!g_requested.empty()) {
        wake = std::min(wake, g_requested.front().due);
      }
      const size_t queued = g_requested.size();
      if (wake == std::chrono::steady_clock::time_point::max()) {
        g_cv.wait(lock, [&] { return has_work() || g_requested.size() != queued; });
      } else {
        g_cv.wait_until(lock, wake, [&] { return has_work() || g_requested.size() != queued; });
      }
      if (g_stop) {
        return;
      }
      if (due()) {
        requested = std::move(g_requested.front().label);
        g_requested.pop_front();
      } else if (periodic && std::chrono::steady_clock::now() >= next) {
        ++n;
        next += std::chrono::seconds(every_s);
      } else {
        continue;
      }
    }
    rex::ui::Presenter* ui_presenter = get_presenter ? get_presenter() : nullptr;
    rex::ui::RawImage image;
    if (!ui_presenter || !ui_presenter->CaptureGuestOutput(image) || !image.width ||
        !image.height) {
      REXLOG_WARN("[capture] {}: there is no game image to capture", requested.empty() ? std::to_string(n) : requested);
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
    char label[64];
    std::snprintf(label, sizeof(label), "capture_%03d_%04llds.png", n,
                  static_cast<long long>(seconds));
    const std::filesystem::path path = folder / (requested.empty() ? std::string(label) : requested + ".png");
    const std::vector<uint8_t> png = EncodePng(image);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!file) {
      REXLOG_WARN("[capture] Could not write {}", path.string());
      continue;
    }
    REXLOG_INFO("[capture] {} ({}x{})", path.string(), image.width, image.height);
  }
}

}  // namespace

void Boot(std::function<rex::ui::Presenter*()> get_presenter) {
  const int every_s = REXCVAR_GET(bo2zm_capture_every_s);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_get_presenter = get_presenter;  // RequestCapture starts the thread on demand
  }
  int burst_s = 0, burst_n = 0, burst_ms = 0;
  if (std::sscanf(REXCVAR_GET(bo2zm_capture_burst).c_str(), "%d:%d:%d", &burst_s, &burst_n, &burst_ms) == 3 &&
      burst_s >= 0 && burst_n > 0 && burst_n <= 500 && burst_ms >= 0) {
    for (int i = 0; i < burst_n; ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "burst_%03d", i);
      RequestCapture(name, std::chrono::milliseconds(int64_t(burst_s) * 1000 + int64_t(i) * burst_ms));
    }
    REXLOG_INFO("[capture] a burst of {} captures {} ms apart from {} s", burst_n, burst_ms, burst_s);
  }
  if (every_s <= 0 || g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
  }
  g_thread = std::thread(Loop, std::move(get_presenter), every_s,
                       int(REXCVAR_GET(bo2zm_capture_max)));
  REXLOG_INFO("[capture] One capture every {} s (maximum {})", every_s,
              int(REXCVAR_GET(bo2zm_capture_max)));
}

void RequestCapture(std::string label, std::chrono::milliseconds delay) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_get_presenter || g_stop) {
    return;
  }
  Requested request{std::move(label), std::chrono::steady_clock::now() + delay};
  const auto at = std::upper_bound(g_requested.begin(), g_requested.end(), request.due,
                                   [](const auto& due, const Requested& r) { return due < r.due; });
  g_requested.insert(at, std::move(request));
  if (!g_thread.joinable()) {
    g_thread = std::thread(Loop, g_get_presenter, 0, 0);
  }
  g_cv.notify_all();
}

void CaptureNowForTests(rex::ui::Presenter* presenter, const std::string& label) {
  if (REXCVAR_GET(bo2zm_capture_every_s) <= 0 || !presenter) {
    return;
  }
  rex::ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
    REXLOG_WARN("[capture] {}: there is no image to capture", label);
    return;
  }
  const std::filesystem::path folder = CaptureFolder();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path path = folder / (label + ".png");
  const std::vector<uint8_t> png = EncodePng(image);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  if (!file) {
    REXLOG_WARN("[capture] Could not write {}", path.string());
    return;
  }
  REXLOG_INFO("[capture] {} ({}x{})", path.string(), image.width, image.height);
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace bo2zm::capture
