// Test helper: a virtual gamepad driven by a button script, so PC test runs can
// reach a match unattended (each native renderer test needs Survival: Town).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
//   --bo2zm_test_buttons="25:down,26:down,27:a,35:a"
//
// "T:button" presses for 0.25 s, "T1-T2:button" holds; seconds from when the
// input system is created. Buttons: a b x y start back up down left right lb rb
// lt rt ls rs (stick clicks), stick_up stick_down stick_left stick_right (left stick: move) and
// look_up look_down look_left look_right (right stick: camera, e.g.
// "60-75:look_right" turns in place for test captures). The pad is added to the SDK
// input system as a synthetic device of user 0, beside the real pad and
// keyboard. Optional _p2, _p3 and _p4 scripts feed their own guest ports. Empty
// defaults add nothing; a secondary script also enables an idle primary pad.
// Any player's script accepts "60-65:disconnect": that script pad disappears
// for [60, 65) seconds, then returns with the same identity. Other devices stay
// connected. Disconnect requires an explicit finite interval and suppresses
// buttons during it; expired presses are not replayed on reconnect.
#include "bo2zm_test_pad.h"
#include <rex/cvar.h>
#include <rex/logging.h>


REXCVAR_DEFINE_STRING(test_buttons, "", "BO2",
                      "Test helper: virtual pad script, e.g. \"25:down,27:a,60-65:disconnect\" (seconds from startup)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(test_buttons_p2, "", "BO2",
                      "Test helper: independent second-player pad script; empty disables it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(test_buttons_p3, "", "BO2",
                      "Test helper: independent third-player pad script; empty disables it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(test_buttons_p4, "", "BO2",
                      "Test helper: independent fourth-player pad script; empty disables it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(test_look_speed, 35, "BO2",
                     "Test helper: right-stick tilt of the look_* buttons, in percent (lower turns slower)")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Old names, still accepted from configuration files, the command line, REX_ variables and scripts.
REXCVAR_ALIAS(bo2zm_test_buttons, test_buttons);
REXCVAR_ALIAS(bo2zm_test_buttons_p2, test_buttons_p2);
REXCVAR_ALIAS(bo2zm_test_buttons_p3, test_buttons_p3);
namespace bo2zm::test_pad {
// v0.11: scripted pads come from the SDK pad driver (--hid.drivers=sdl3,script --hid.script.file=...); the old
// in-game pad script driver is not ported, so the test_buttons settings are accepted and ignored.
void WrapInput(rex::RuntimeConfig&) {}
}  // namespace bo2zm::test_pad
