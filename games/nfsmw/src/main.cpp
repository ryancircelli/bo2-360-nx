// nfsmw - ReXGlue Recompiled Project
//
// Need for Speed: Most Wanted (Xbox 360, USA), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/nfsmw/game.toml; the shared GameApp
// runs whatever RegisterGame() adds (no hooks yet).

#include "generated/default/nfsmw_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/nfsmw/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/nfsmw/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "nfsmw", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(nfsmw, CreateApp)
