// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Raw video of the game image for tests (bo2zm_video_rgba), the format peggle1-nx records with REX_PEGGLE1_VIDEO_*:
// <name>.rgba holds the frames back to back, R8 G8 B8 X8 rows without padding, and <name>.txt beside it (the .rgba
// suffix replaced) says "<width> <height> <fps> <count>". tools/mac/make_video.sh turns the pair into an mp4.
//
// A thread reads the presenter's guest output (Presenter::CaptureGuestOutput, the image the game handed over before
// any scaling) at a fixed rate. It is a test tool: each read waits for the GPU, so the frame rate of the run drops a
// little while it records.

#include "bo2zm_native_video.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

REXCVAR_DEFINE_STRING(bo2zm_video_rgba, "", "BO2",
                      "Test video: record the game image as raw frames to this .rgba file plus a .txt beside it "
                      "(\"w h fps count\"; empty = off; the environment variables REX_BO2ZM_VIDEO_RGBA and "
                      "BO2ZM_VIDEO_RGBA do the same)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_video_fps, 30, "BO2", "Test video (bo2zm_video_rgba): frames per second").range(1, 60);
REXCVAR_DEFINE_INT32(bo2zm_video_from_s, 0, "BO2", "Test video: seconds after start-up before the first frame")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(bo2zm_video_s, 120, "BO2", "Test video: seconds to record (then the file is closed)")
    .range(1, 3600);

namespace bo2zm::video {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;

std::string OutputPath() {
  std::string path = REXCVAR_GET(bo2zm_video_rgba);
  for (const char* name : {"REX_BO2ZM_VIDEO_RGBA", "BO2ZM_VIDEO_RGBA"}) {
    if (!path.empty()) {
      break;
    }
    if (const char* env = std::getenv(name); env && *env) {
      path = env;
    }
  }
  return path;
}

std::filesystem::path InfoPath(const std::filesystem::path& rgba) {
  std::filesystem::path info = rgba;
  info.replace_extension(".txt");
  return info;
}

void WriteInfo(const std::filesystem::path& path, uint32_t w, uint32_t h, int fps, uint64_t count) {
  std::ofstream info(path, std::ios::trunc);
  info << w << ' ' << h << ' ' << fps << ' ' << count << '\n';
}

void Loop(std::function<rex::ui::Presenter*()> get_presenter, std::filesystem::path path) {
  const int fps = REXCVAR_GET(bo2zm_video_fps);
  const auto period = std::chrono::nanoseconds(1'000'000'000LL / fps);
  const auto start = std::chrono::steady_clock::now() + std::chrono::seconds(REXCVAR_GET(bo2zm_video_from_s));
  const auto end = start + std::chrono::seconds(REXCVAR_GET(bo2zm_video_s));
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    REXLOG_WARN("[video] Could not create {}", path.string());
    return;
  }
  REXLOG_INFO("[video] Recording {} fps to {} for {} s", fps, path.string(), int(REXCVAR_GET(bo2zm_video_s)));
  uint32_t width = 0, height = 0;
  uint64_t count = 0, skipped = 0;
  std::vector<uint8_t> row_buffer;
  auto next = start;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait_until(lock, next, [] { return g_stop; });
      if (g_stop) {
        break;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) {
      break;
    }
    // A late frame is repeated rather than dropped, so the video keeps the run's real timing.
    uint64_t repeats = 1;
    while (next + period <= now) {
      next += period;
      ++repeats;
    }
    next += period;
    rex::ui::Presenter* presenter = get_presenter ? get_presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
      continue;  // nothing on screen yet
    }
    if (!width) {
      width = image.width;
      height = image.height;
      row_buffer.resize(size_t(width) * 4);
    } else if (image.width != width || image.height != height) {
      ++skipped;  // the guest output changed size; the raw file keeps the first one
      continue;
    }
    for (uint64_t r = 0; r < repeats; ++r) {
      for (uint32_t y = 0; y < height; ++y) {
        file.write(reinterpret_cast<const char*>(image.data.data() + size_t(y) * image.stride),
                   std::streamsize(row_buffer.size()));
      }
      ++count;
    }
    if (count % (uint64_t(fps) * 5) < repeats) {
      file.flush();
      WriteInfo(InfoPath(path), width, height, fps, count);  // a killed run still leaves a usable pair
    }
  }
  file.close();
  if (width) {
    WriteInfo(InfoPath(path), width, height, fps, count);
  }
  REXLOG_INFO("[video] {} frames {}x{} in {}{}", count, width, height, path.string(),
              skipped ? " (" + std::to_string(skipped) + " frames of another size left out)" : std::string());
}

}  // namespace

void Boot(std::function<rex::ui::Presenter*()> get_presenter) {
  const std::string path = OutputPath();
  if (path.empty() || g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = false;
  }
  g_thread = std::thread(Loop, std::move(get_presenter), std::filesystem::path(path));
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

}  // namespace bo2zm::video
