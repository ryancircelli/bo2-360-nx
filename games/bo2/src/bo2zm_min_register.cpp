// bo2zm on the v0.11 SDK, stage 1: the manifest only. The hand-written hooks return as they are ported.
#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/bo2/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/bo2/game.toml");
}
