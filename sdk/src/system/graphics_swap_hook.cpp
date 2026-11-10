#include <atomic>

#include <rex/graphics/swap_hook.h>

namespace rex::graphics {

namespace {
std::atomic<SwapHook> g_hook{nullptr};
}

void SetSwapHook(SwapHook hook) {
  g_hook.store(hook, std::memory_order_release);
}

void NotifySwap(uint32_t width, uint32_t height) {
  if (SwapHook hook = g_hook.load(std::memory_order_acquire)) {
    hook(width, height);
  }
}

}  // namespace rex::graphics
