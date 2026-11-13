// Pipeline precompile at start-up (see bo2zm_native_precompile.h).

#include "bo2zm_native_precompile.h"
#include "bo2zm_native_pso.h"

#include "bo2zm_native_shaders.h"
#include "../bo2zm_native_system.h"
#include "../../bo2zm_switch.h"  // BuildId

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/vulkan/device.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_BOOL(bo2zm_native_precompile, true, "BO2",
                    "Native renderer: at start, when the pipelines earlier sessions created are not known to be "
                    "compiled for this build, driver and shader pack (cache/bo2zm_native_pipelines.ok), they are "
                    "compiled before the game starts, on every core, behind a progress screen that only comes up if "
                    "the driver really has to compile one. false = they are compiled in the background during the "
                    "menus, as before (bo2zm_native_prewarm_pipelines)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if defined(__APPLE__)
#define BO2ZM_PRECOMPILE_KEEP_DEFAULT true
#else
#define BO2ZM_PRECOMPILE_KEEP_DEFAULT false
#endif
REXCVAR_DEFINE_BOOL(bo2zm_native_precompile_keep, BO2ZM_PRECOMPILE_KEEP_DEFAULT, "BO2",
                    "Native renderer: the precompile keeps the pipelines it creates and the ring uses them, so a match "
                    "creates none of the listed ones; the pass then also runs when the caches already hold every "
                    "pipeline (it is short then). For MoltenVK, where even a cached pipeline costs a Metal pipeline "
                    "state in the match (default on macOS, off elsewhere)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_test_cold_pipelines, false, "BO2",
                    "Test helper: the session starts with an empty pipeline cache but keeps the list, and ignores the "
                    "stamp file, as after a driver change; on the Switch Mesa's own shader cache is off too "
                    "(bo2zm_switch.cpp): every listed pipeline is really compiled by the precompile (or by the prewarm "
                    "thread with bo2zm_native_precompile off). The PC's driver may still answer from its own cache. "
                    "The file is then saved as usual")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_test_precompile_screen_s, 0, "BO2",
                     "Test helper, with a precompile: its screen comes up at once (not at the first pipeline the "
                     "driver has to compile) and the game waits at least this many seconds, to see or capture it. 0 = "
                     "off")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, bo2zm_native_test_fresh_pipelines);
REXCVAR_DECLARE(std::string, bo2zm_native_prewarm_lists);

namespace bo2zm::native::precompile {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr uint32_t kStampVersion = 1;
constexpr const char* kFileOk = "bo2zm_native_pipelines.ok";
constexpr const char* kFilePipelines = "bo2zm_native_pipelines.bin";  // as in bo2zm_native_draws.cpp

struct State {
  std::mutex mutex;
  std::condition_variable decided;
  int decision = -1;  // -1 not yet, 0 no precompile, 1 precompile (with the mutex)
  bool gave_up = false;  // Hold stopped waiting: the game runs, so nothing may hold it any more
  bool ended = false;
  std::function<void()> start;  // the held game's (with the mutex)
  uint64_t stamp = 0;
  bool known_warm = false;
  std::atomic<bool> ended_flag{false};  // for the screen
  std::atomic<bool> visible{false};
  std::atomic<uint32_t> total{0};
  std::atomic<uint32_t> done{0};
  std::atomic<uint32_t> compiled{0};
  std::atomic<uint32_t> done_when_visible{0};
  uint32_t threads = 0;
  Clock::time_point decided_at{};
  Clock::time_point began_at{};
  std::atomic<int64_t> visible_ns{0};  // since began_at
  std::atomic<uint32_t> vouched{0};
};
State g;

fs::path FolderCache() {
#ifndef BO2ZM_NATIVE_CACHE_FOLDER
#define BO2ZM_NATIVE_CACHE_FOLDER "cache"  // as in bo2zm_native_draws.cpp
#endif
  return pso::CacheFolder();  // cache/ beside the executable, or the user's cache folder for a macOS app bundle
}

// The extra prewarm lists (bo2zm_native_prewarm_lists): names and sizes, sorted, for the stamp. A new or changed list
// brings records the stamp file never vouched for.
std::string ExtraLists() {
  const fs::path folder = rex::filesystem::GetExecutableFolder() / std::string(REXCVAR_GET(bo2zm_native_prewarm_lists));
  std::error_code ec;
  std::vector<std::string> entries;
  if (fs::is_directory(folder, ec)) {
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
      if (entry.is_regular_file(ec) && entry.path().extension() == ".bin") {
        entries.push_back(entry.path().filename().string() + ":" + std::to_string(entry.file_size(ec)));
      }
    }
  }
  std::sort(entries.begin(), entries.end());
  std::string all;
  for (const std::string& e : entries) {
    all += e;
    all += '\n';
  }
  all += pso::ListStamp();  // the bo2pso text list (bo2zm_native_pso.h)
  return all;
}

// Everything that has to stay the same for a cache's pipelines to remain valid: this build (how it creates its
// pipelines; on the Switch it also contains the driver, which is linked into the NRO), the driver as the device reports
// it, the shader pack and the extra lists. pipelineCacheUUID follows the driver build on the PC; the Switch build of
// NVK derives it from the Mesa version alone, which is why the build identifier is part of the stamp.
uint64_t ComputeStamp(const rex::ui::vulkan::VulkanDevice* device, const NativeShaders* library) {
  std::string bytes;
  const auto add = [&](const void* data, size_t size) { bytes.append(static_cast<const char*>(data), size); };
  add(&kStampVersion, sizeof(kStampVersion));
  bytes += bo2zm::BuildId();
  bytes += '\n';
  if (device) {
    VkPhysicalDeviceProperties properties{};
    device->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(device->physical_device(), &properties);
    add(&properties.vendorID, sizeof(properties.vendorID));
    add(&properties.deviceID, sizeof(properties.deviceID));
    add(&properties.driverVersion, sizeof(properties.driverVersion));
    add(properties.pipelineCacheUUID, sizeof(properties.pipelineCacheUUID));
  }
  const uint64_t pack = library ? library->Identity() : 0;
  add(&pack, sizeof(pack));
  bytes += ExtraLists();
  return XXH3_64bits(bytes.data(), bytes.size());
}

// cache/bo2zm_native_pipelines.ok: "BZOK <stamp, 16 hex digits> <bytes of the pipelines file it vouches for>".
bool ReadOk(uint64_t& stamp, uint64_t& bytes) {
  std::FILE* f = std::fopen((FolderCache() / kFileOk).string().c_str(), "rb");
  if (!f) {
    return false;
  }
  unsigned long long s = 0, b = 0;
  const bool ok = std::fscanf(f, "BZOK %16llx %llu", &s, &b) == 2;
  std::fclose(f);
  stamp = s;
  bytes = b;
  return ok;
}

void NotifyDecision(bool wanted) {
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.decision = wanted ? 1 : 0;
  }
  g.decided.notify_all();
}

// The full-screen progress screen. It exists from the hold to the end of the pass, and draws nothing until the first
// pipeline that really needs compiling (a black screen, as before the game's first frame); then it closes and deletes
// itself (ImGuiDialog::Close).
class Screen final : public rex::ui::ImGuiDialog {
 public:
  explicit Screen(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (g.ended_flag.load(std::memory_order_acquire)) {
      Close();
      return;
    }
    if (!g.visible.load(std::memory_order_acquire)) {
      return;
    }
    const ImVec2 size = io.DisplaySize;
    // The SDK's one font is ProggyTiny at 10 px: whole-number scales keep it sharp. Laid out for 720 lines.
    const float unit = size.y / 720.0f;
    const float scale_title = std::max(1.0f, std::round(3.0f * unit));
    const float scale_text = std::max(1.0f, std::round(2.0f * unit));
    const uint32_t total = g.total.load(std::memory_order_relaxed);
    const uint32_t done = std::min(g.done.load(std::memory_order_relaxed), total);
    const uint32_t compiled = g.compiled.load(std::memory_order_relaxed);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(40, 40, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(230, 140, 20, 255));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##bo2zm_precompile", nullptr, flags)) {
      const auto centered = [&](const char* text, float scale, float y, ImU32 color) {
        ImGui::SetWindowFontScale(scale);
        const ImVec2 extent = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2(std::floor((size.x - extent.x) * 0.5f), std::floor(y)));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
      };
      const float top = size.y * 0.36f;
      centered("PREPARING SHADERS", scale_title, top, IM_COL32(255, 255, 255, 255));
      centered("Once after an update or a driver change, the game's shaders are compiled before it starts.",
               scale_text, top + 48.0f * unit, IM_COL32(170, 170, 170, 255));
      const float width = std::floor(size.x * 0.5f);
      const float height = std::floor(24.0f * unit);
      const float bar_y = top + 110.0f * unit;
      ImGui::SetCursorPos(ImVec2(std::floor((size.x - width) * 0.5f), std::floor(bar_y)));
      ImGui::SetWindowFontScale(scale_text);
      char label[64];
      std::snprintf(label, sizeof(label), "%u / %u", done, total);
      ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f, ImVec2(width, height), label);
      // The time left, from the pace since the screen came up (the records before were already in the cache).
      char status[128];
      const double since_visible =
          std::chrono::duration<double>(Clock::now() - g.began_at).count() -
          double(g.visible_ns.load(std::memory_order_relaxed)) / 1e9;
      const uint32_t walked = done - std::min(done, g.done_when_visible.load(std::memory_order_relaxed));
      if (since_visible >= 2.0 && walked > 0 && done < total) {
        const double left = double(total - done) * since_visible / double(walked);
        std::snprintf(status, sizeof(status), "%u compiled, about %.0f s left", compiled, std::ceil(left));
      } else {
        std::snprintf(status, sizeof(status), "%u compiled", compiled);
      }
      centered(status, scale_text, bar_y + height + 16.0f * unit, IM_COL32(170, 170, 170, 255));
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
  }
};

}  // namespace

bool Hold(std::function<void()> start, rex::ui::ImGuiDrawer* drawer) {
  if (!bo2zm::native::PortedRendererActive() || !REXCVAR_GET(bo2zm_native_precompile)) {
    return false;
  }
  std::unique_lock<std::mutex> lock(g.mutex);
  // The ring thread decides as it starts, with the graphics system, before the game is launched; this only waits if
  // it has not got there yet.
  if (!g.decided.wait_for(lock, std::chrono::seconds(5), [] { return g.decision >= 0; })) {
    g.gave_up = true;
    REXLOG_WARN("[precompile] the ring thread had not decided after 5 s: the game starts without the precompile");
    return false;
  }
  if (g.decision == 0 || g.ended) {
    return false;
  }
  g.start = std::move(start);
  if (drawer) {
    new Screen(drawer);  // deletes itself when the pass is over
  }
  REXLOG_INFO("[precompile] the game waits for the pipeline precompile");
  return true;
}

bool Decide(const rex::ui::vulkan::VulkanDevice* device, const NativeShaders* library) {
  g.decided_at = Clock::now();
  g.stamp = ComputeStamp(device, library);
  std::error_code ec;
  const fs::path file = FolderCache() / kFilePipelines;
  const uint64_t bytes = fs::file_size(file, ec);
  const bool has_file = !ec && bytes > 0;
  const bool cold = REXCVAR_GET(bo2zm_native_test_cold_pipelines);
  uint64_t ok_stamp = 0, ok_bytes = 0;
  const bool has_ok = ReadOk(ok_stamp, ok_bytes);
  g.known_warm = has_file && has_ok && ok_stamp == g.stamp && ok_bytes == bytes && !cold;
  const bool lists = !ExtraLists().empty();
  const char* why = nullptr;
  if (!REXCVAR_GET(bo2zm_native_precompile)) {
    why = "off (bo2zm_native_precompile)";
  } else if (REXCVAR_GET(bo2zm_native_test_fresh_pipelines)) {
    why = "no list this session (bo2zm_native_test_fresh_pipelines)";
  } else if (!library || !library->loaded()) {
    why = "no shader library";
  } else if (!has_file && !lists) {
    why = "no pipelines file yet (a first start: its pipelines are compiled as they come)";
  } else if (g.known_warm && !Keep()) {
    why = "every listed pipeline is already compiled for this build, driver and shader pack";
  }
  bool wanted = why == nullptr;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.gave_up) {
      wanted = false;
      why = "the game already started";
    }
  }
  REXLOG_INFO("[precompile] stamp {:016X}; pipelines file {} KB, stamp file {}: {}", g.stamp, has_file ? bytes >> 10 : 0,
              !has_ok ? "missing"
              : ok_stamp != g.stamp ? fmt::format("for stamp {:016X}", ok_stamp)
              : ok_bytes != bytes   ? fmt::format("for a file of {} KB", ok_bytes >> 10)
                                    : std::string("matches"),
              wanted ? (cold ? "precompile (bo2zm_native_test_cold_pipelines)" : "precompile before the game starts")
                     : why);
  NotifyDecision(wanted);
  return wanted;
}

uint64_t Stamp() {
  return g.stamp;
}

bool Keep() {
  return REXCVAR_GET(bo2zm_native_precompile_keep);
}

bool KnownWarm() {
  return g.known_warm;
}

void Begin(uint32_t total, uint32_t threads) {
  g.total.store(total, std::memory_order_relaxed);
  g.threads = threads;
  g.began_at = Clock::now();
  REXLOG_INFO("[precompile] {} listed pipelines on {} threads ({:.0f} ms after the decision)", total, threads,
              std::chrono::duration<double, std::milli>(g.began_at - g.decided_at).count());
  if (total && REXCVAR_GET(bo2zm_native_test_precompile_screen_s) > 0) {
    g.visible.store(true, std::memory_order_release);  // bo2zm_native_test_precompile_screen_s
  }
}

void Advance(bool compiled) {
  const uint32_t done = g.done.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!compiled) {
    return;
  }
  g.compiled.fetch_add(1, std::memory_order_relaxed);
  if (!g.visible.load(std::memory_order_relaxed) && !g.visible.exchange(true, std::memory_order_acq_rel)) {
    g.done_when_visible.store(done - 1, std::memory_order_relaxed);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g.began_at).count();
    g.visible_ns.store(ns, std::memory_order_relaxed);
    REXLOG_INFO("[precompile] pipeline {} of {} needs compiling: the screen comes up ({:.0f} ms into the pass)", done,
                g.total.load(std::memory_order_relaxed), double(ns) / 1e6);
  }
}

void End() {
  // bo2zm_native_test_precompile_screen_s: the screen stays up at least that long.
  if (const int32_t hold_s = REXCVAR_GET(bo2zm_native_test_precompile_screen_s); hold_s > 0 && g.total.load()) {
    std::this_thread::sleep_until(g.began_at + std::chrono::seconds(hold_s));
  }
  std::function<void()> start;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.ended = true;
    start = std::move(g.start);
    g.start = nullptr;
  }
  g.ended_flag.store(true, std::memory_order_release);
  const auto now = Clock::now();
  REXLOG_INFO("[precompile] done in {:.2f} s: {} of {} pipelines walked, {} compiled; screen {}; {}",
              std::chrono::duration<double>(now - g.began_at).count(), g.done.load(), g.total.load(),
              g.compiled.load(), g.visible.load() ? "shown" : "never shown",
              start ? "the game starts now" : "the game was not waiting");
  if (start) {
    start();
  }
}

void Vouch(const fs::path& pipelines_file) {
  if (!g.stamp) {
    return;
  }
  std::error_code ec;
  const uint64_t bytes = fs::file_size(pipelines_file, ec);
  if (ec) {
    return;
  }
  const fs::path path = pipelines_file.parent_path() / kFileOk;
  fs::path temporary = path;
  temporary += ".tmp";
  if (std::FILE* f = std::fopen(temporary.string().c_str(), "wb")) {
    std::fprintf(f, "BZOK %016llX %llu\n", static_cast<unsigned long long>(g.stamp),
                 static_cast<unsigned long long>(bytes));
    std::fclose(f);
    fs::rename(temporary, path, ec);
    if (ec) {
      fs::remove(path, ec);
      fs::rename(temporary, path, ec);
    }
  }
  if (g.vouched.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("[precompile] stamp file written: every listed pipeline is in the cache of the {} KB pipelines file{}",
                bytes >> 10, ec ? " (could not rename it)" : "");
  }
}

void Withdraw(const fs::path& pipelines_file) {
  std::error_code ec;
  fs::remove(pipelines_file.parent_path() / kFileOk, ec);
}

}  // namespace bo2zm::native::precompile
