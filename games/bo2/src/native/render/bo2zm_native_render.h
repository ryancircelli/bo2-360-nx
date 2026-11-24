// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The native renderer's graphics system: what replaces the Xenos emulation plugin.
//
// With bo2zm_c1_renderer = "native", OnPreSetup puts this system in config.graphics and the emulation plugin is not
// loaded. The design is described in docs/native-renderer.md.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace bo2zm::native {

// true if the bo2zm_c1_renderer cvar asks for the native path.
bool IsActive();

// Ring thread progress, for the game's D3D waits (bo2zm_waits.cpp): it increases every time
// the ring thread returns the read pointer or delivers an interrupt.
uint32_t ProgressRing();

// Waits at most 'limit' for ProgressRing() to differ from 'seen'. true if it advanced.
bool WaitProgressRing(uint32_t seen, std::chrono::microseconds limit);

// Swaps presented by the native renderer since startup (0 with Xenos emulation). The watchdog in
// bo2zm_register.cpp counts them as progress: with blocking waits the game threads sleep in the same place
// for most of the frame.
uint64_t SwapsNative();

// The native graphics system: the SDK presenter, a reserved MMIO range, the vblank
// thread and the command ring sink.
std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics();

}  // namespace bo2zm::native
