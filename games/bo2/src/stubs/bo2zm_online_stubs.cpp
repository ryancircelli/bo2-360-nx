// SPDX-License-Identifier: BSD-3-Clause
// Stubs for the xam/online modules (name entry, internet play, DLC scan) until the clean-room
// online APIs (PAS-48) land on v0.11. Each reports "not available" and does nothing per frame.
#include <cstdint>
#include <string_view>

#include <rex/ppc/context.h>

#include "../bo2zm_dlc.h"
#include "../bo2zm_name.h"
#include "../bo2zm_online.h"

namespace bo2zm::online {
bool RunCommand(std::string_view, std::string_view) { return false; }
void EveryFrame(PPCContext&, uint8_t*) {}
void Stop() {}
}  // namespace bo2zm::online

namespace bo2zm::name {
void ApplySaved() {}
void EveryFrame(PPCContext&, uint8_t*) {}
bool RunCommand(std::string_view, std::string_view) { return false; }
}  // namespace bo2zm::name

namespace bo2zm::dlc {
void EveryFrame(PPCContext&, uint8_t*) {}
}  // namespace bo2zm::dlc
