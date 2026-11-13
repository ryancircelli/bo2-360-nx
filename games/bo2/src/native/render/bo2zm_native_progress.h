// The screen shown while the shader library's index is made (the first start with a new pack; see
// bo2zm_shader_index.h). Without it that start is a black screen for about a minute on a Switch.
//
// It is deliberately simple: a line of text in a small built-in bitmap font, a progress bar and a percentage, all
// drawn with clear rectangles (vkCmdClearAttachments) into the presenter's guest output image, so it needs no
// shaders, pipelines or uploads, and it presents through the same path as the game's frames. It runs on the thread
// that makes the index, between shaders, and draws at most every 100 ms.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bo2zm::native {

class ProgressScreen {
 public:
  ProgressScreen(rex::ui::Presenter* presenter, const rex::ui::vulkan::VulkanDevice* device);
  ~ProgressScreen();  // waits for its last frame and frees what it made
  ProgressScreen(const ProgressScreen&) = delete;
  ProgressScreen& operator=(const ProgressScreen&) = delete;

  // done of total bytes. Draws a frame if the last one is 100 ms old (the first call always draws).
  void Update(uint64_t done, uint64_t total);
  // One black frame, which is what a start without the screen shows until the game's first image.
  void Finish();

  uint32_t frames() const;

 private:
  struct Vulkan;
  void Draw(int percent, bool blank);
  std::unique_ptr<Vulkan> vk_;
  std::chrono::steady_clock::time_point last_{};
  bool drawn_ = false;
  bool captured_ = false;
};

}  // namespace bo2zm::native
