// fh1 - ReXGlue Recompiled Project
//
// Forza Horizon (Xbox 360, USA), recompiled. The title's facts (name, title id, executable, disc
// folder, setting defaults) are in games/fh1/game.toml; the shared GameApp
// runs whatever RegisterGame() adds (no hooks yet; a Guide page).

#include "generated/default/fh1_init.h"

#include <format>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/game_api.h>
#include <rex/ui/flags.h>  // video_mode_refresh_rate
#include <rex/ui/guide.h>

#include "rex_game_manifest.h"  // rex::kGameManifest, from games/fh1/game.toml

namespace {
// Must match [renderer.settings] metal_shader_constants in games/fh1/game.toml.
constexpr const char* kBlurThreshold = "ps E68FE55ECD482EC5 253 3 0.5";

// The Guide's Game blade: the settings this port already has (games/fh1/app_settings.toml). Every item is a cvar,
// so the change goes through the cvar path (no guest state is touched from the UI thread).
void RegisterGuidePage() {
  namespace guide = rex::ui::guide;
  guide::Page page;
  page.id = "fh1.settings";
  page.title = "Forza Horizon";
  page.description = "Settings for Forza Horizon.";
  page.items = {
      guide::CvarChoice("metal_shader_constants", {kBlurThreshold, ""}, {"On", "Off"}, "Speed Blur Fix",
                        "Keeps the speed blur off the car and the parts of the scene it should not touch."),
      guide::CvarToggle("gpu_allow_invalid_fetch_constants", "Draw All Geometry",
                        "Draws objects the game marks oddly instead of dropping them. Off leaves holes in the world."),
      guide::CvarChoice("metal_resolve_readback_max_bytes", {"65536", "0"}, {"On", "Off"}, "Auto Exposure",
                        "Lets the game measure scene brightness. Off washes the picture out and makes it pulse."),
      guide::CvarToggle("input_remember_slots", "Remember Controllers",
                        "A controller that drops out gets its player back when it comes back on."),
      guide::Info(
          "Refresh Rate",
          [] { return std::format("{:g} Hz", REXCVAR_GET(video_mode_refresh_rate)); },
          "The game draws every second refresh: 120 Hz gives about 60 fps at the right speed. Set it in fh1.toml "
          "(video_mode_refresh_rate); it takes effect after a restart."),
  };
  guide::RegisterPage(std::move(page));
}
}  // namespace

void rex::RegisterGame(rex::GameApi& api) {
  rex::UseGameManifest(api, rex::kGameManifest, "games/fh1/game.toml");
  api.OnPostSetup([](rex::GameApp&) { RegisterGuidePage(); });
}

namespace {
std::unique_ptr<rex::ui::WindowedApp> CreateApp(rex::ui::WindowedAppContext& ctx) {
  return rex::GameApp::Create(ctx, "fh1", PPCImageConfig);
}
}  // namespace

REX_DEFINE_APP(fh1, CreateApp)
