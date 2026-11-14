// fh1 - ReXGlue Recompiled Project
//
// Forza Horizon (Xbox 360, USA), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/fh1/game.toml; the shared GameApp
// runs whatever RegisterGame() adds (no hooks yet).

#include "generated/default/fh1_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/fh1/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/fh1/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "fh1", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(fh1, CreateApp)
