#include <catch2/catch_test_macros.hpp>

#include <rex/input/input.h>
#include <rex/ui/overlay/guide_overlay.h>

using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_X;
using rex::input::X_INPUT_STATE;
using rex::ui::GuideInputGate;

namespace {

uint16_t Seen(GuideInputGate& gate, uint32_t user, uint16_t buttons) {
  X_INPUT_STATE state{};
  state.gamepad.buttons = buttons;
  gate.Filter(user, state);
  return state.gamepad.buttons;
}

}  // namespace

TEST_CASE("a button held through a Guide take-over stays hidden after the close", "[guide]") {
  GuideInputGate gate([](int) {});
  gate.SetHolder(0);                         // player 1 opens the Guide
  gate.SetHolder(GuideInputGate::kAllPads);  // player 1's pad disconnects: every pad is held
  REQUIRE(Seen(gate, 2, X_INPUT_GAMEPAD_X) == 0);
  gate.SetHolder(1);  // player 2 takes it over; player 3 still holds X
  REQUIRE(Seen(gate, 2, X_INPUT_GAMEPAD_X) == 0);
  gate.SetHolder(-1);  // player 2 closes it
  CHECK(Seen(gate, 2, X_INPUT_GAMEPAD_X) == 0);
  CHECK(Seen(gate, 1, X_INPUT_GAMEPAD_A) == 0);
  // Let go, then press again: a real press reaches the game.
  CHECK(Seen(gate, 2, 0) == 0);
  CHECK(Seen(gate, 2, X_INPUT_GAMEPAD_X) == X_INPUT_GAMEPAD_X);
}
