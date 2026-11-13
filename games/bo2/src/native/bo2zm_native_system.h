// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// BO2's own graphics system, used instead of the Xenos GPU emulation.
//
// With bo2zm_renderer = "native", OnPreSetup puts this system in config.graphics and the emulation plugin is never
// loaded. The game keeps its own Direct3D layer, which writes the PM4 command ring exactly as on the Xbox 360; a ring
// thread reads that ring, tracks the GPU registers and answers everything the game waits on (the read pointer,
// SCRATCH writes, WAIT_REG_MEM, interrupts and occlusion queries).
//
// The "skeleton" mode is a minimal version of this system kept for bring-up and debugging: it draws nothing, and each
// Swap clears the screen to a color that changes with the swap count, so a capture tells a running game from a
// stalled one. The full renderer ("native") is the one in native/render/.

#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace bo2zm::native {

// true if the bo2zm_renderer cvar asks for a native path: "native" (the full renderer in native/render/) or
// "skeleton" (the minimal system described above, which only keeps the game running).
bool Active();

// true for "native": the full renderer.
bool PortedRendererActive();

// Diagnostics: log vertex shaders the library cannot identify from the ring.
bool LogVertexShaderMisses();

// Diagnostics: draw everything without depth or stencil testing.
bool DebugNoDepth();

// Diagnostics: every pixel shader replaced by solid magenta.
bool DebugMagenta();

// Run Direct3D's Clear() rectangles as real clears: they are drawn with D3D's own shaders, which are not in the
// shader library.
bool ClearQuads();

// Swaps presented since startup (0 with Xenos emulation).
uint64_t Swaps();

// The system the cvar selects (call only when Active()).
std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

}  // namespace bo2zm::native
