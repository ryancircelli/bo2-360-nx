// Calling the game's own functions from the port. A function the port only calls (and does not hook) is called
// through the code's dispatch table, the way the game's own indirect calls go, so the game's direct calls to it stay
// direct in the generated code: tools/direct_calls.py counts an address as hooked when it appears in the port's code,
// but not in comments or on a line marked "call only". Hooked functions keep REX_EXTERN / REX_HOOK_RAW.
#pragma once

#include <cstdint>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

namespace bo2zm::guest {

// Calls the game function at Address with ctx (arguments and stack as the game would set them up).
template <uint32_t Address>
inline void Call(PPCContext& ctx, uint8_t* base) {
  static PPCFunc* const function = rex::runtime::ResolveIndirectFunction(Address);
  function(ctx, base);
}

}  // namespace bo2zm::guest
