// SPDX-License-Identifier: BSD-3-Clause
// Internet play is not on this engine: the home menu's online commands report "not available".
#include <cstdint>
#include <string_view>

#include <rex/ppc/context.h>

#include "../bo2zm_online.h"

namespace bo2zm::online {
bool RunCommand(std::string_view, std::string_view) { return false; }
void EveryFrame(PPCContext&, uint8_t*) {}
void Stop() {}
}  // namespace bo2zm::online
