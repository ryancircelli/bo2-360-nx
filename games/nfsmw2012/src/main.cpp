// nfsmw2012 - ReXGlue Recompiled Project
//
// Need for Speed: Most Wanted 2012 (Xbox 360, 2012), recompiled. The title's facts are in games/nfsmw2012/game.toml;
// the shared GameApp runs whatever RegisterGame() adds (no hooks).

#include "generated/default/nfsmw2012_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/nfsmw2012/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/nfsmw2012/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "nfsmw2012", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(nfsmw2012, CreateApp)
