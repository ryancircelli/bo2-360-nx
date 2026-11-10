// Frame dumps for comparing renderers; see metal_frame_dump.h.

#include "rex/graphics/metal/metal_frame_dump.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "rex/ui/metal/metal_presenter.h"
#include "xxhash.h"

DEFINE_string(metal_frame_dump, "",
              "Save the guest output of these guest frames (swap numbers, "
              "counted from 1) for renderer comparisons: a comma list "
              "(\"300,600,900\") and/or ranges \"first:count:step\". Written in "
              "the GPU test harness replay layout to metal_frame_dump_dir.",
              "Metal");
DEFINE_string(metal_frame_dump_dir, "frame_dumps",
              "Output directory of metal_frame_dump (created if missing).",
              "Metal");
DEFINE_int32(metal_frame_dump_timeout_s, 3600,
             "Give up on metal_frame_dump frames not reached after this many "
             "seconds.",
             "Metal");

namespace rex::graphics::metal {
namespace {

std::vector<uint64_t> ParseFrames(const std::string& spec) {
  std::vector<uint64_t> frames;
  std::stringstream items(spec);
  std::string item;
  while (std::getline(items, item, ',')) {
    unsigned long long first = 0, count = 1, step = 1;
    if (std::sscanf(item.c_str(), "%llu:%llu:%llu", &first, &count, &step) < 1 ||
        !first || !count || !step) {
      continue;
    }
    for (unsigned long long i = 0; i < count && i < 100000; ++i) {
      frames.push_back(first + i * step);
    }
  }
  std::sort(frames.begin(), frames.end());
  frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
  return frames;
}

// numpy .npy v1.0, float32 little endian, C order, shape (h, w, 4).
bool WriteNpy(const std::filesystem::path& path, const std::vector<float>& data,
              uint32_t width, uint32_t height) {
  std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" +
                       std::to_string(height) + ", " + std::to_string(width) +
                       ", 4), }";
  // magic (6) + version (2) + length (2) + header, padded to 64 with '\n' last.
  size_t total = 10 + header.size() + 1;
  header.append((64 - total % 64) % 64, ' ');
  header += '\n';
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  const uint16_t length = uint16_t(header.size());
  out.write("\x93NUMPY\x01\x00", 8);
  out.put(char(length & 0xFF));
  out.put(char(length >> 8));
  out.write(header.data(), std::streamsize(header.size()));
  out.write(reinterpret_cast<const char*>(data.data()),
            std::streamsize(data.size() * sizeof(float)));
  return bool(out);
}

void DumpThread(ui::metal::MetalPresenter* presenter, std::vector<uint64_t> frames,
                std::filesystem::path dir, uint32_t title_id, uint32_t timeout_s) {
  std::error_code ec;
  std::filesystem::create_directories(dir / "draws", ec);
  std::vector<ui::metal::MetalPresenter::FrameCapture> captures;
  uint64_t current = 0;
  const int got = presenter->CaptureGuestOutputAtFrames(frames, timeout_s * 1000u,
                                                        captures, &current);
  std::ofstream jsonl(dir / "draws.jsonl");
  uint32_t written = 0;
  for (size_t i = 0; i < captures.size(); ++i) {
    const auto& capture = captures[i];
    const ui::RawImage& image = capture.image;
    if (!capture.ok || !image.width || !image.height) {
      continue;
    }
    // RGBX8 to float RGBA (alpha 1, as the guest output has none).
    std::vector<float> pixels(size_t(image.width) * image.height * 4);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + y * image.stride;
      float* dst = pixels.data() + size_t(y) * image.width * 4;
      for (uint32_t x = 0; x < image.width; ++x) {
        dst[x * 4 + 0] = row[x * 4 + 0] / 255.0f;
        dst[x * 4 + 1] = row[x * 4 + 1] / 255.0f;
        dst[x * 4 + 2] = row[x * 4 + 2] / 255.0f;
        dst[x * 4 + 3] = 1.0f;
      }
    }
    char file[64];
    std::snprintf(file, sizeof(file), "draws/%06u_frame.npy", written);
    if (!WriteNpy(dir / file, pixels, image.width, image.height)) {
      REXLOG_ERROR("metal_frame_dump: could not write {}", (dir / file).string());
      continue;
    }
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016" PRIX64,
                  uint64_t(XXH3_64bits(pixels.data(), pixels.size() * sizeof(float))));
    jsonl << "{\"draw\":" << written << ",\"frame\":" << capture.requested_frame
          << ",\"packet\":null,\"prim_type\":null,\"index_count\":null,"
             "\"succeeded\":true,\"is_resolve\":false,\"vs\":null,\"ps\":null,"
             "\"regs\":{},\"targets\":[{\"target\":\"frame\",\"key\":\"frame_"
          << image.width << "_" << image.height << "\",\"width\":" << image.width
          << ",\"height\":" << image.height
          << ",\"channels\":4,\"format\":\"rgba32f\",\"hash\":\"" << hash
          << "\",\"base\":\"full\",\"file\":\"" << file << "\",\"rect\":[0,0,"
          << image.width << "," << image.height << "]}]}\n";
    ++written;
  }
  jsonl.close();
  char title[9];
  std::snprintf(title, sizeof(title), "%08X", title_id);
  std::ofstream summary(dir / "summary.json");
  summary << "{\n  \"source\": \"live\",\n  \"backend\": \"metal\",\n"
          << "  \"title_id\": \"" << title << "\",\n"
          << "  \"frames\": " << written << ",\n  \"draws\": " << written << ",\n"
          << "  \"requested_frames\": " << frames.size() << ",\n"
          << "  \"last_frame_seen\": " << current << ",\n"
          << "  \"ok\": " << (got > 0 && written == frames.size() ? "true" : "false")
          << "\n}\n";
  REXLOG_INFO("metal_frame_dump: saved {} of {} frames to {} (last frame seen {})",
              written, frames.size(), dir.string(), current);
}

}  // namespace

void StartFrameDump(ui::Presenter* presenter, uint32_t title_id) {
  const std::string spec = REXCVAR_GET(metal_frame_dump);
  if (spec.empty() || !presenter) {
    return;
  }
  std::vector<uint64_t> frames = ParseFrames(spec);
  if (frames.empty()) {
    REXLOG_ERROR("metal_frame_dump: no frames in \"{}\"", spec);
    return;
  }
  REXLOG_INFO("metal_frame_dump: {} frames from {} to {}", frames.size(), frames.front(),
              frames.back());
  std::thread(DumpThread, static_cast<ui::metal::MetalPresenter*>(presenter),
              std::move(frames), std::filesystem::path(REXCVAR_GET(metal_frame_dump_dir)),
              title_id, uint32_t(std::max(1, REXCVAR_GET(metal_frame_dump_timeout_s))))
      .detach();
}

}  // namespace rex::graphics::metal
