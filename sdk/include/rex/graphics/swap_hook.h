// A hook an application can set to hear about each frame the guest presents, whichever GPU plugin draws it
// (the Vulkan "xenos" plugin or the Metal plugin). The functions live in rexruntime so the app and the plugin
// libraries share one hook. The hook runs on the GPU command thread and must be quick.
#pragma once

#include <cstdint>

namespace rex::graphics {

using SwapHook = void (*)(uint32_t width, uint32_t height);

void SetSwapHook(SwapHook hook);
// Called by a command processor's IssueSwap.
void NotifySwap(uint32_t width, uint32_t height);

}  // namespace rex::graphics
