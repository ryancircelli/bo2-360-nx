// fallout3 - ReXGlue Recompiled Project
//
// Need for Speed: Hot Pursuit (Xbox 360, USA, 2010), recompiled. The title's facts are in games/fallout3/game.toml;
// the shared GameApp runs whatever RegisterGame() adds (no hooks).

#include "generated/default/fallout3_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/fallout3/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/fallout3/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "fallout3", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(fallout3, CreateApp)
