// Recording the frame's command buffer on a second thread (bo2zm_native_deferred_recording).
//
// The ring thread decodes the game's GPU commands and records Vulkan for each draw. On the console the Vulkan calls
// themselves (NVK building its command stream: binding pipelines and descriptor sets, setting dynamic state, the draw)
// took about 3.7-4 us of the roughly 10.5 us a draw costs, on the one thread that sets the frame rate: about 2.6-2.9 us
// of recording plus about 1.1 us of pipeline binds. Here the vkCmd* calls made on the work slot's job buffer are
// queued instead, with their arguments copied, and a recorder thread makes them in the same order while the ring
// thread moves on to the next draws. Everything else (other command buffers, every vkCreate*/vkUpdate*/vkQueue* call)
// is left alone.
//
// How it is wired (see the .cpp for the details and the rules it relies on):
//   - The renderer's function table is Table(device): a copy of the device's with every vkCmd* entry replaced by a
//     wrapper. A wrapper called with any other command buffer calls the real function at once.
//   - The work slot's BeginRecording calls Begin(job buffer) after vkBeginCommandBuffer; SubmitJob calls Finish()
//     before vkEndCommandBuffer. Between the two, the job buffer's calls are queued.
//   - vkCmd* functions loaded separately (the extended dynamic state setters, vkCmdWriteTimestamp) go through Wrap.
//   - A vkCmd* function without a queued form (anything the renderer uses rarely) waits until the recorder has
//     caught up and calls the real function on the ring thread: always correct, only slower.

#pragma once

#include <rex/ui/vulkan/device.h>

namespace bo2zm::native::deferred {

using Functions = rex::ui::vulkan::VulkanDevice::Functions;

// The renderer's function table (built on first use from the device's; one device per process).
const Functions& Table(const rex::ui::vulkan::VulkanDevice& device);

// The wrapper of a vkCmd* function loaded separately (vkGetDeviceProcAddr), by name; 'real' if it has none.
PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real);

// Ring thread: start queuing the calls made on 'commands' (after its vkBeginCommandBuffer). Nothing happens when the
// feature is off (bo2zm_native_deferred_recording).
void Begin(VkCommandBuffer commands);
// Ring thread: wait until every queued call was made, then stop queuing (before vkEndCommandBuffer).
void Finish();
// True while calls are being queued.
bool Deferring();
// Ring thread (bo2zm_native_deferred_batch, on by default): publish the queued calls to the recorder in batches of
// 512 bytes instead of one by one; Finish still waits for all of them.
void PublishInBatches(bool on);
// A pipeline about to be bound, for NVK's prefetch (vk_switch_prefetch_pipeline): queued in order when deferring.
void Prefetch(void (*prefetch)(VkPipeline), VkPipeline pipeline);
// Every 10 s from the ring thread: one line with the counters.
void Report();

}  // namespace bo2zm::native::deferred
