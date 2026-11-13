// The fastfile stream decoder (sub_827E7058, LZX-style) written natively from its PowerPC code (see the .cpp).
// bo2zm_native_audio.cpp runs it through its checked phase: compared with the game's own function before it runs
// alone (bo2zm_native_lzx).

#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace bo2zm::native_lzx {

// sub_827E7058(r3 = decoder state, r4 = output position, r5 = bytes wanted): the same memory writes and r3.
void Decode(PPCContext& ctx, uint8_t* base);

}  // namespace bo2zm::native_lzx
