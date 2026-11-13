// Access to the game's console from our own code: running console commands, a RenderDoc capture on the PC, and the
// program's volume. The details are in bo2zm_autoexec.cpp.

#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::autoexec {

// Runs console commands ("cmd1; cmd2") right away with the game's text runner (sub_824032C0). Only from a hook of
// a function the game's main thread runs (the per-frame hook in bo2zm_autoexec.cpp); ctx is left as it was.
// controller: the controller the commands run for (0-3), e.g. for "setprofile".
void ExecuteCommands(PPCContext& ctx, uint8_t* base, std::string_view commands, int controller = 0);

// Test helper, PC only: under RenderDoc, capture the next frame (bo2zm_renderdoc_path names the files). false when
// not running under RenderDoc.
bool CaptureRenderDoc();

// Applies PC test attenuation at startup; Switch output gain stays at 100 percent.
void ApplyVolume();

}  // namespace bo2zm::autoexec
