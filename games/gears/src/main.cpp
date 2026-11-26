// gears - ReXGlue Recompiled Project
//
// Gears of War (Xbox 360, 2006), recompiled. The title's facts are in games/gears/game.toml;
// the shared GameApp runs whatever RegisterGame() adds (no hooks).

#include "generated/default/gears_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/gears/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/gears/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "gears", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(gears, CreateApp)
