// Vector rewrites of BO2's CPU vertex skinning kernels (one, two and three bones per vertex), written from their
// PowerPC code (see the .cpp). bo2zm_native_audio.cpp runs them through its checked phase: each one is compared
// with the game's own function before it runs alone.

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::native_skin {

// sub_82912D78: one bone per vertex.
void Skin1(PPCContext& ctx, uint8_t* base);
// sub_82913378: two bones per vertex, one weight.
void Skin2(PPCContext& ctx, uint8_t* base);
// sub_82913B50: three bones per vertex, two weights.
void Skin3(PPCContext& ctx, uint8_t* base);

// A "[native_skin] timed" line: each kernel's vertices per call and time per vertex in the last 10 s, in the general
// and the fast form (bo2zm_native_skin_fast). Called every 10 s.
void Report();

}  // namespace bo2zm::native_skin
