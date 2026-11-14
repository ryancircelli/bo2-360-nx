// bo1 - ReXGlue Recompiled Project
//
// Call of Duty: Black Ops (Xbox 360), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/bo1/game.toml; the shared GameApp
// runs whatever RegisterGame() adds (no hooks yet).

#include "generated/default/bo1_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/bo1/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/bo1/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "bo1", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(bo1, CreateApp)
