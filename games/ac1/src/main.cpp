// ac1 - ReXGlue Recompiled Project
//
// Need for Speed: Hot Pursuit (Xbox 360, USA, 2010), recompiled. The title's facts are in games/ac1/game.toml;
// the shared GameApp runs whatever RegisterGame() adds (no hooks).

#include "generated/default/ac1_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/ac1/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/ac1/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "ac1", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(ac1, CreateApp)
