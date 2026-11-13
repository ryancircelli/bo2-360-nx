// The shader library's first-start progress screen (see bo2zm_native_progress.h).

#include "bo2zm_native_progress.h"
#include "bo2zm_native_capture.h"

#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bo2zm::native {
namespace {

using OutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

constexpr uint32_t kWidth = 1280;  // the guest output, as the renderer's own frames
constexpr uint32_t kHeight = 720;
constexpr auto kInterval = std::chrono::milliseconds(100);

// Laid out for 720 lines, in the colors of the pipeline precompile screen (bo2zm_native_precompile.cpp).
constexpr int kScale = 3;                 // pixels per font dot
constexpr int kTextTop = 306;
constexpr int kBarTop = 350;
constexpr int kBarWidth = 640;
constexpr int kBarHeight = 24;
constexpr std::array<float, 3> kText{1.0f, 1.0f, 1.0f};
constexpr std::array<float, 3> kBarBack{40 / 255.0f, 40 / 255.0f, 40 / 255.0f};
constexpr std::array<float, 3> kBarFill{230 / 255.0f, 140 / 255.0f, 20 / 255.0f};

// A 5x7 font with what the screen says: capitals, digits and a little punctuation. Each row reads left to right.
struct Glyph {
  char c;
  const char* rows[7];
};
constexpr Glyph kFont[] = {
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'%', {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
};

const Glyph* FindGlyph(char c) {
  if (c >= 'a' && c <= 'z') {
    c = char(c - 'a' + 'A');
  }
  for (const Glyph& g : kFont) {
    if (g.c == c) {
      return &g;
    }
  }
  return nullptr;  // a space, or anything the font lacks
}

constexpr int kAdvance = 6 * kScale;  // five dots and a gap

int TextWidth(const char* text) {
  const int n = int(std::strlen(text));
  return n > 0 ? n * kAdvance - kScale : 0;
}

// The text as rectangles, one per run of dots in a row of a glyph.
void TextRects(const char* text, int x, int y, std::vector<VkClearRect>& out) {
  for (const char* p = text; *p; ++p, x += kAdvance) {
    const Glyph* g = FindGlyph(*p);
    if (!g) {
      continue;
    }
    for (int row = 0; row < 7; ++row) {
      const char* dots = g->rows[row];
      for (int col = 0; col < 5;) {
        if (dots[col] != '#') {
          ++col;
          continue;
        }
        const int first = col;
        while (col < 5 && dots[col] == '#') {
          ++col;
        }
        VkClearRect r{};
        r.rect.offset = {x + first * kScale, y + row * kScale};
        r.rect.extent = {uint32_t((col - first) * kScale), uint32_t(kScale)};
        r.baseArrayLayer = 0;
        r.layerCount = 1;
        out.push_back(r);
      }
    }
  }
}

VkClearRect Rect(int x, int y, int width, int height) {
  VkClearRect r{};
  r.rect.offset = {x, y};
  r.rect.extent = {uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0))};
  r.layerCount = 1;
  return r;
}

}  // namespace

struct ProgressScreen::Vulkan {
  rex::ui::Presenter* presenter = nullptr;
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool fence_pending = false;
  bool broken = false;  // something could not be made: no more frames (the start goes on as without the screen)
  uint32_t frames = 0;
  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
  };
  std::array<Framebuffer, 3> framebuffers{};  // one per guest output image
  size_t next_framebuffer = 0;

  bool Setup();
  VkFramebuffer FramebufferFor(const OutputContext& context);
  bool Record(OutputContext& context, int percent, bool blank);
  void WaitIdle();
  void Destroy();
};

bool ProgressScreen::Vulkan::Setup() {
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();
  VkAttachmentDescription attachment{};
  attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
  VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkRenderPassCreateInfo pass_info{};
  pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  pass_info.attachmentCount = 1;
  pass_info.pAttachments = &attachment;
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  if (dfn.vkCreateRenderPass(vk_device, &pass_info, nullptr, &render_pass) != VK_SUCCESS) {
    render_pass = VK_NULL_HANDLE;
    return false;
  }
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(vk_device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    pool = VK_NULL_HANDLE;
    return false;
  }
  VkCommandBufferAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate.commandPool = pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(vk_device, &allocate, &commands) != VK_SUCCESS) {
    commands = VK_NULL_HANDLE;
    return false;
  }
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (dfn.vkCreateFence(vk_device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    fence = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

VkFramebuffer ProgressScreen::Vulkan::FramebufferFor(const OutputContext& context) {
  for (const Framebuffer& f : framebuffers) {
    if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
      return f.framebuffer;
    }
  }
  const auto& dfn = device->functions();
  Framebuffer& f = framebuffers[next_framebuffer];
  next_framebuffer = (next_framebuffer + 1) % framebuffers.size();
  if (f.framebuffer != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device->device(), f.framebuffer, nullptr);  // its frame is done (WaitIdle)
    f.framebuffer = VK_NULL_HANDLE;
  }
  VkImageView view = context.image_view();
  VkFramebufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = render_pass;
  info.attachmentCount = 1;
  info.pAttachments = &view;
  info.width = kWidth;
  info.height = kHeight;
  info.layers = 1;
  if (dfn.vkCreateFramebuffer(device->device(), &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
    f.framebuffer = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }
  f.version = context.image_version();
  return f.framebuffer;
}

void ProgressScreen::Vulkan::WaitIdle() {
  if (fence_pending) {
    const auto& dfn = device->functions();
    dfn.vkWaitForFences(device->device(), 1, &fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(device->device(), 1, &fence);
    fence_pending = false;
  }
}

bool ProgressScreen::Vulkan::Record(OutputContext& context, int percent, bool blank) {
  const auto& dfn = device->functions();
  WaitIdle();  // one command buffer: the previous frame's must be done (it is, 100 ms later)
  dfn.vkResetCommandPool(device->device(), pool, 0);
  const VkFramebuffer framebuffer = FramebufferFor(context);
  if (framebuffer == VK_NULL_HANDLE) {
    return false;
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
    return false;
  }
  VkClearValue black{};
  black.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo pass{};
  pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  pass.renderPass = render_pass;
  pass.framebuffer = framebuffer;
  pass.renderArea.extent = {kWidth, kHeight};
  pass.clearValueCount = 1;
  pass.pClearValues = &black;
  dfn.vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
  if (!blank) {
    const auto clear = [&](const std::array<float, 3>& color, const std::vector<VkClearRect>& rects) {
      if (rects.empty()) {
        return;
      }
      VkClearAttachment attachment{};
      attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      attachment.colorAttachment = 0;
      attachment.clearValue.color.float32[0] = color[0];
      attachment.clearValue.color.float32[1] = color[1];
      attachment.clearValue.color.float32[2] = color[2];
      attachment.clearValue.color.float32[3] = 1.0f;
      dfn.vkCmdClearAttachments(commands, 1, &attachment, uint32_t(rects.size()), rects.data());
    };
    // The percentage keeps three places, so the line does not move as it counts.
    char line[96];
    std::snprintf(line, sizeof(line), "Preparing the shader library (first start only)  %3d %%", percent);
    std::vector<VkClearRect> text;
    TextRects(line, (int(kWidth) - TextWidth(line)) / 2, kTextTop, text);
    clear(kText, text);
    const int bar_x = (int(kWidth) - kBarWidth) / 2;
    clear(kBarBack, {Rect(bar_x, kBarTop, kBarWidth, kBarHeight)});
    const int filled = kBarWidth * std::clamp(percent, 0, 100) / 100;
    if (filled > 0) {
      clear(kBarFill, {Rect(bar_x, kBarTop, filled, kBarHeight)});
    }
  }
  dfn.vkCmdEndRenderPass(commands);
  if (dfn.vkEndCommandBuffer(commands) != VK_SUCCESS) {
    return false;
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commands;
  {
    const auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence) != VK_SUCCESS) {
      return false;
    }
  }
  fence_pending = true;
  context.SetIs8bpc(true);
  return true;
}

void ProgressScreen::Vulkan::Destroy() {
  if (!device) {
    return;
  }
  WaitIdle();
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();
  for (Framebuffer& f : framebuffers) {
    if (f.framebuffer != VK_NULL_HANDLE) {
      dfn.vkDestroyFramebuffer(vk_device, f.framebuffer, nullptr);
      f = Framebuffer{};
    }
  }
  if (fence != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(vk_device, fence, nullptr);
    fence = VK_NULL_HANDLE;
  }
  if (pool != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(vk_device, pool, nullptr);  // frees commands
    pool = VK_NULL_HANDLE;
    commands = VK_NULL_HANDLE;
  }
  if (render_pass != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(vk_device, render_pass, nullptr);
    render_pass = VK_NULL_HANDLE;
  }
}

ProgressScreen::ProgressScreen(rex::ui::Presenter* presenter, const rex::ui::vulkan::VulkanDevice* device)
    : vk_(std::make_unique<Vulkan>()) {
  vk_->presenter = presenter;
  vk_->device = device;
  vk_->broken = !presenter || !device || !vk_->Setup();
  if (vk_->broken && device) {
    REXLOG_WARN("[native] C5a: the progress screen could not be set up: the index is made behind a black screen");
  }
}

ProgressScreen::~ProgressScreen() {
  vk_->Destroy();
}

uint32_t ProgressScreen::frames() const {
  return vk_->frames;
}

void ProgressScreen::Draw(int percent, bool blank) {
  if (vk_->broken) {
    return;
  }
  bool recorded = false;
  vk_->presenter->RefreshGuestOutput(kWidth, kHeight, kWidth, kHeight,
                                     [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
                                       recorded = vk_->Record(static_cast<OutputContext&>(context), percent, blank);
                                       return recorded;
                                     });
  if (recorded) {
    ++vk_->frames;
  }
#if defined(_WIN32)
  // The index is made on the window's thread, which pumps no messages meanwhile. Peeking now and then keeps Windows
  // from taking the window for hung and covering it with a frozen copy; nothing is taken out of the queue.
  MSG message;
  PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
#endif
}

void ProgressScreen::Update(uint64_t done, uint64_t total) {
  const auto now = std::chrono::steady_clock::now();
  if (drawn_ && now - last_ < kInterval) {
    return;
  }
  drawn_ = true;
  last_ = now;
  const int percent = total ? int(std::min<uint64_t>(done, total) * 100 / total) : 0;
  Draw(percent, false);
  if (percent >= 50 && !captured_) {
    captured_ = true;
    bo2zm::capture::CaptureNowForTests(vk_->presenter, "shader_index_progress");  // test captures only
  }
}

void ProgressScreen::Finish() {
  if (drawn_) {
    Draw(0, true);
    REXLOG_INFO("[native] C5a: progress screen shown while the index was made ({} frames)", vk_->frames);
  }
}

}  // namespace bo2zm::native
