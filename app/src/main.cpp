// bo2zm - ReXGlue Recompiled Project
//
// The app is game-neutral (rex::GameApp); BO2 registers its hooks in
// bo2zm_register.cpp (rex::RegisterGame).

#include "generated/default_mp/bo2zm_init.h"

#include <rex/game_api.h>

namespace {

std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "bo2zm", PPCImageConfig);
}

}  // namespace

REX_DEFINE_APP(bo2zm, CreateApp)
