// How the ME2 generated code reaches guest memory. Modeled on
// core/bo2zm_guest_memory.h (BO2), appended after the generated PCH via a
// forced include (games/me2/CMakeLists.txt).
//
// Default: the codegen's own REX_LOAD_*/REX_STORE_* (volatile) stay, and
// REX_GUEST_BARRIER() is the empty barrier, same as BO2's default.
#pragma once

#include "me2_pch.h"

#define REX_GUEST_BARRIER() ((void)0)
