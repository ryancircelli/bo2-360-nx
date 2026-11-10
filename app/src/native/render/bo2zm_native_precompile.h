// Pipeline precompile at start-up.
//
// The native renderer keeps what it has compiled in cache/bo2zm_native_pipelines.bin next to the executable: the
// driver's pipeline cache plus a list of every pipeline earlier launches created (see LoadCachePipelines in
// bo2zm_native_draws.cpp). After a driver, executable or shader pack change, those pipelines can no longer be assumed
// to be in the cache. Left alone, a background thread would rebuild them during the menus, and anything it had not
// reached yet would be compiled in the middle of a match, with the affected objects missing until it was ready. This
// module compiles them before the game starts instead:
//   - When the ring thread starts, it compares a stamp for this launch (build, the driver's pipeline cache identity,
//     shader pack, extra prewarm lists) with cache/bo2zm_native_pipelines.ok. The pipeline writer leaves that file
//     next to a pipelines file only when every listed pipeline is in its cache. If the stamps match, nothing happens.
//   - Otherwise the game's main thread is held (Bo2zmApp::OnHoldModuleLaunch), the renderer is set up early and the
//     list is compiled on all cores. The first pipeline the driver really has to compile (it answers
//     VK_PIPELINE_COMPILE_REQUIRED) brings up a full-screen progress screen; if the whole list is already in the
//     cache, no screen is shown at all. The game starts once the pass is over.
// The very first launch has no list, so there is nothing to compile ahead of time; that launch compiles pipelines as
// they are needed.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bo2zm::native {

class NativeShaders;

namespace precompile {

// UI thread, in place of starting the game: true when the game waits for the precompile; start is then called once
// it is over (from the ring thread). Waits for the ring thread's decision (normally made already).
bool Hold(std::function<void()> start, rex::ui::ImGuiDrawer* drawer);

// Ring thread, at its start (before the game's first command): whether the precompile runs now. Once.
bool Decide(const rex::ui::vulkan::VulkanDevice* device, const NativeShaders* library);
// This start's stamp (0 before Decide), and whether the pipelines file on disk was vouched for at start.
uint64_t Stamp();
bool KnownWarm();
// bo2zm_native_precompile_keep: the pass keeps its pipelines for the ring, and runs even when the caches are warm.
bool Keep();

// The pass. Begin from the ring thread; Advance from its workers, once per record (compiled = the driver had to
// compile it: the screen comes up); End from the ring thread: the screen closes and the game starts.
void Begin(uint32_t total, uint32_t threads);
void Advance(bool compiled);
void End();

// The stamp file, from the pipeline writer thread. Vouch: every pipeline in the list of that pipelines file (as it is on
// disk now) is in its cache, for this start's stamp. Withdraw: that is not known.
void Vouch(const std::filesystem::path& pipelines_file);
void Withdraw(const std::filesystem::path& pipelines_file);

}  // namespace precompile
}  // namespace bo2zm::native
