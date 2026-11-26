// me2 - ReXGlue Recompiled Project
//
// Mass Effect 2 (Xbox 360, 2010), recompiled. The title's facts are in games/me2/game.toml;
// the shared GameApp runs whatever RegisterGame() adds (no hooks).

#include "generated/default/me2_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/me2/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/me2/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "me2", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(me2, CreateApp)
