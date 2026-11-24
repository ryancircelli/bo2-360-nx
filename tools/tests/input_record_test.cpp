// Input ring and recorded-pad replayer (the glitch report's input.txt round trip).
// Build: c++ -std=c++23 -DFMT_HEADER_ONLY -Isdk/include -Isdk/thirdparty/fmt/include -Isdk/thirdparty/simde
//        -Isdk/thirdparty/spdlog/include -Isdk/thirdparty/tomlplusplus/include tools/tests/input_record_test.cpp
//        sdk/src/input/input_record.cpp -o /tmp/input_record_test && /tmp/input_record_test
#include <rex/input/input_record.h>

#include <cassert>
#include <cstdio>

using namespace rex::input;
using namespace rex;

static PadSample S(uint32_t ms, uint32_t user, uint16_t buttons, int16_t lx = 0) {
  PadSample s;
  s.ms = ms;
  s.frame = ms / 16;
  s.user = user;
  s.buttons = buttons;
  s.lx = lx;
  return s;
}

int main() {
  // Ring: unchanged inputs are not stored; the window starts from the state in force.
  InputRing ring;
  ring.Record(S(0, 0, 0));
  ring.Record(S(10, 0, 0));      // same inputs, dropped
  ring.Record(S(100, 0, 0x1000));
  ring.Record(S(5000, 0, 0));
  ring.Record(S(5000, 1, 0x2000));
  auto w = ring.Window(6000, 2000);  // from 4000
  assert(w.size() == 3);
  assert(w[0].ms == 4000 && w[0].buttons == 0x1000 && w[0].user == 0);  // state in force for user 0
  assert(w[1].ms == 5000 && w[1].user == 0 && w[1].buttons == 0);
  assert(ring.Window(6000, 60000).size() == 4);  // 0:0, 0:0x1000, 0:0, 1:0x2000 (the 10 ms repeat was dropped)

  // Wraparound keeps the newest kCapacity.
  InputRing big;
  for (uint32_t i = 0; i < InputRing::kCapacity + 50; i++) big.Record(S(i, 0, uint16_t(i + 1)));
  auto all = big.Window(InputRing::kCapacity + 50, 0xFFFFFFFF);
  assert(all.size() == InputRing::kCapacity && all.back().ms == InputRing::kCapacity + 49);

  // Text round trip, ms rebased to the first sample; bad lines skipped.
  std::string text = SamplesToText(w) + "garbage\n";
  auto back = SamplesFromText(text);
  assert(back.size() == w.size());
  assert(back[0].ms == 0 && back[1].ms == 1000 && back[1].buttons == 0 && back[2].buttons == 0x2000);
  assert(back[2].user == 1);

  // Replayer: the state at a time, and the driver reports the right pad.
  double now = 0;
  RecordedPadDriver drv(back, 0.5, [&] { return now; });
  std::vector<DeviceInfo> devs;
  drv.EnumerateDevices(devs);
  assert(devs.size() == 2 && devs[0].fixed_user == 0 && devs[1].fixed_user == 1);
  X_INPUT_STATE st;
  now = 0.4;  // before the start delay: not connected (leaves the slot to other drivers)
  assert(drv.GetDeviceState(devs[0].id, &st) == X_ERROR_DEVICE_NOT_CONNECTED);
  now = 0.5 + 0.2;  // 200 ms in: user 0 holds 0x1000
  drv.GetDeviceState(devs[0].id, &st);
  assert(uint16_t(st.gamepad.buttons) == 0x1000);
  now = 0.5 + 1.5;  // 1500 ms: user 0 released, user 1 holds 0x2000
  drv.GetDeviceState(devs[0].id, &st);
  assert(uint16_t(st.gamepad.buttons) == 0);
  drv.GetDeviceState(devs[1].id, &st);
  assert(uint16_t(st.gamepad.buttons) == 0x2000);
  std::puts("input_record_test: PASS");
}
