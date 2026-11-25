#include "generated/default/halo3_init.h"

#include <memory>

#include <rex/game_api.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/halo3/game.toml

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/halo3/game.toml");
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "halo3", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(halo3, CreateApp)
