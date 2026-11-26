// Live pad layer (LivePadCommand): what `runq live press/hold/stick/trigger` puts on a script pad.
#include <catch2/catch_test_macros.hpp>

#include <thread>

#include <rex/input/script/script_input_driver.h>

using namespace rex::input;
using namespace rex::input::script;

namespace {
constexpr auto kPad1 = static_cast<DeviceId>(ScriptInputDriver::kScriptPadBase);

X_INPUT_GAMEPAD State(ScriptInputDriver& d) {
  X_INPUT_STATE s{};
  REQUIRE(d.GetDeviceState(kPad1, &s) == X_ERROR_SUCCESS);
  return s.gamepad;
}
}  // namespace

TEST_CASE("live pad input lands on the script pad", "[input][live]") {
  double now = 0;
  ScriptInputDriver d({{}}, 35, [&] { return now; });  // one idle pad
  CHECK(State(d).buttons == 0);

  CHECK(LivePadCommand("press", "a 50").rfind("ok press a p1", 0) == 0);
  CHECK(State(d).buttons == X_INPUT_GAMEPAD_A);
  X_INPUT_KEYSTROKE key{};
  REQUIRE(d.GetDeviceKeystroke(kPad1, 0, &key) == X_ERROR_SUCCESS);
  CHECK(key.virtual_key == 0x5800);
  CHECK(key.flags == X_INPUT_KEYSTROKE_KEYDOWN);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  CHECK(State(d).buttons == 0);  // the tap ended after it was seen

  CHECK(LivePadCommand("hold", "lb+down").rfind("ok hold", 0) == 0);
  CHECK(State(d).buttons == (X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_DPAD_DOWN));
  CHECK(LivePadCommand("stick", "l 0.5 -1").rfind("ok stick", 0) == 0);
  CHECK(LivePadCommand("trigger", "r 1").rfind("ok trigger", 0) == 0);
  auto g = State(d);
  CHECK(int16_t(g.thumb_lx) == 16384);
  CHECK(int16_t(g.thumb_ly) == -32767);
  CHECK(g.right_trigger == 255);
  CHECK(LivePadCommand("release", "all").rfind("ok", 0) == 0);
  g = State(d);
  CHECK(g.buttons == 0);
  CHECK(int16_t(g.thumb_lx) == 0);
  CHECK(g.right_trigger == 0);

  CHECK(LivePadCommand("press", "f7").empty());           // not a pad control: left to the keyboard
  CHECK(LivePadCommand("cvar", "x").empty());             // not a pad verb
  CHECK(LivePadCommand("press", "a p2").rfind("error no virtual pad 2", 0) == 0);
  CHECK(LivePadCommand("stick", "l 2 0").rfind("error usage", 0) == 0);
}
