#pragma once

namespace bo2zm::waits {
// Runtime benchmark switch. Existing sleeps keep their interest mask and bounded timeout.
bool FenceWake();
void SetFenceWake(bool on);
}  // namespace bo2zm::waits
