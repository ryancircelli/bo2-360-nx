// A native version of BO2's sound limiter sub_8258BE08 (see the .cpp). bo2zm_native_audio.cpp runs it through its
// checked phase: it is compared with the game's own function before it runs alone.

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::native_limiter {

// sub_8258BE08(r3 = count, r5 = parameters, r6 = state, r7 = samples, r8 = the minimum gain out, f1 = the rate).
// Calls the game's own function for a block it does not handle (see the .cpp).
void Limit(PPCContext& ctx, uint8_t* base);

// bo2zm_native_limiter, or its A/B arm (bo2zm_native_limiter_ab): false = the game's function.
bool OnInThisArm();

}  // namespace bo2zm::native_limiter
