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

namespace {
rex::input::X_INPUT_STATE Busy() {
  rex::input::X_INPUT_STATE state{};
  state.gamepad.buttons = X_INPUT_GAMEPAD_A;
  state.gamepad.left_trigger = 200;
  state.gamepad.thumb_lx = 20000;
  return state;
}
}  // namespace

TEST_CASE("the holder's pad is zeroed while other pads pass, and keys follow the holder", "[guide]") {
  GuideInputGate gate([](int) {});
  gate.SetHolder(1);
  auto held = Busy();
  gate.Filter(1, held);
  CHECK(held.gamepad.buttons == 0);
  CHECK(held.gamepad.left_trigger == 0);
  CHECK(held.gamepad.thumb_lx == 0);
  auto other = Busy();
  gate.Filter(0, other);
  CHECK(other.gamepad.buttons == X_INPUT_GAMEPAD_A);
  CHECK(other.gamepad.left_trigger == 200);
  CHECK(gate.BlocksKeys(1));
  CHECK_FALSE(gate.BlocksKeys(0));
  gate.SetHolder(GuideInputGate::kAllPads);
  for (uint32_t u = 0; u < 4; ++u) CHECK(gate.BlocksKeys(u));
  gate.SetHolder(-1);
  CHECK_FALSE(gate.BlocksKeys(1));
  CHECK(gate.holder() == -1);
}

TEST_CASE("after a close buttons are stripped until released but triggers and sticks pass at once", "[guide]") {
  GuideInputGate gate([](int) {});
  gate.SetHolder(0);
  gate.SetHolder(-1);
  auto state = Busy();
  gate.Filter(0, state);
  CHECK(state.gamepad.buttons == 0);
  CHECK(state.gamepad.left_trigger == 200);  // a held accelerator keeps working
  CHECK(state.gamepad.thumb_lx == 20000);
  state = Busy();
  state.gamepad.buttons = 0;
  gate.Filter(0, state);  // released: draining ends
  state = Busy();
  gate.Filter(0, state);
  CHECK(state.gamepad.buttons == X_INPUT_GAMEPAD_A);
}

TEST_CASE("a user past the fourth is left alone", "[guide]") {
  GuideInputGate gate([](int) {});
  gate.SetHolder(GuideInputGate::kAllPads);
  auto state = Busy();
  gate.Filter(7, state);
  CHECK(state.gamepad.buttons == X_INPUT_GAMEPAD_A);
}
