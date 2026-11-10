// Pipeline state lists (docs/mac/pso-list-format.md) and where the native renderer keeps its pipeline cache.
//
// The renderer already keeps a binary prewarm list and the driver's pipeline cache in one file
// (cache/bo2zm_native_pipelines.bin, see LoadCachePipelines in bo2zm_native_draws.cpp) and compiles that list before
// the game starts when the build, driver or shader pack changed (bo2zm_native_precompile.h). This module adds the
// portable text list on top:
//   - bo2zm_pso_record=<path> (env REX_BO2ZM_PSO_RECORD) writes every pipeline the session creates, once, as a
//     bo2pso 1 text list.
//   - bo2zm_pso.psolist beside the executable (or bo2zm_pso_list=<path>) is read at start-up; its pipelines join
//     the prewarm list, so the precompile builds them behind the loading screen before the first match.
//   - bo2zm_pso_verify logs "[pso] unlisted pipeline <key>" for each pipeline created that the loaded list lacked.
// On macOS a game inside an .app bundle keeps the cache in ~/Library/Caches/bo2-360-nx/ rather than in the bundle.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace bo2zm::native::pso {

// The folder of the pipelines file and its stamp (bo2zm_native_pipelines.bin/.ok).
std::filesystem::path CacheFolder();

// The text list to load at start-up, or an empty path when there is none.
std::filesystem::path ListToLoad();
// The path to record to (bo2zm_pso_record or REX_BO2ZM_PSO_RECORD), or empty.
std::filesystem::path RecordPath();
bool Verify();

// "name:size" of the list to load, for the precompile stamp (a new list brings pipelines the stamp never vouched for).
std::string ListStamp();

}  // namespace bo2zm::native::pso
