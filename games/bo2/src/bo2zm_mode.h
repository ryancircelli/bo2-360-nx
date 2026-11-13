// The mode chosen by the launch data, fixed before the guest starts. Multiplayer
// shares the Zombies executable; the campaign supplies its own false answer.
// False until ApplyPendingRelaunch has consumed or synthesized the launch data.
#pragma once

namespace bo2zm::mode {

bool IsZombies();

}  // namespace bo2zm::mode
