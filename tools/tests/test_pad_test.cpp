// Authored script-pad boundary cases. Compile the production driver, not a model.
#include "../../games/bo2/src/bo2zm_test_pad.cpp"

#include <cassert>
#include <iostream>
#include <thread>

namespace bo2zm::benchmark {
bool active = false;
uint32_t buttons = 0;
unsigned button_calls = 0, tilt_calls = 0;
bool IsActive() { return active; }
uint32_t PathButtons() { ++button_calls; return buttons; }
bool PathTilts(int& move, int& look) { ++tilt_calls; move = 40; look = 20; return true; }
}

using namespace bo2zm::test_pad;
using namespace rex::input;
using namespace rex;
namespace bench = bo2zm::benchmark;

static double seconds = 0;
static Clock::time_point Now() {
  return Clock::time_point{} + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

static void ExpectConnected(ScriptPad& pad, DeviceId id, bool connected) {
  X_INPUT_STATE state{};
  X_INPUT_CAPABILITIES caps{};
  X_INPUT_KEYSTROKE key{};
  X_INPUT_VIBRATION vibration{};
  std::vector<DeviceInfo> devices;
  pad.EnumerateDevices(devices);
  assert(devices.size() == (connected ? 1u : 0u));
  if (connected) assert(devices.front().id == id && devices.front().synthetic);
  const auto expected = connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  assert(pad.GetDeviceState(id, &state) == expected);
  assert(pad.GetDeviceCapabilities(id, 0, &caps) == expected);
  assert(pad.SetDeviceVibration(id, &vibration) == expected);
  const auto result = pad.GetDeviceKeystroke(id, 0, &key);
  assert(connected ? (result == X_ERROR_SUCCESS || result == X_ERROR_EMPTY) : result == expected);
  if (result == X_ERROR_SUCCESS) {
    assert(uint16_t(key.virtual_key) != 0);
    const auto found = std::find(kScriptPads.begin(), kScriptPads.end(), id);
    assert(found != kScriptPads.end());
    assert(key.user_index == found - kScriptPads.begin());
    assert(uint16_t(key.unicode) == 0 && key.hid_code == 0);
  }
}

static void ScriptAndConnectionCases() {
  assert(Parse("60:disconnect,60-60:disconnect,65-60:disconnect,60-inf:disconnect,"
               "nan-65:disconnect,-1-2:disconnect,60-65:unknown").empty());
  assert(Parse("1:a;2-3:rt 60-65:disconnect").size() == 3);
  seconds = 0;
  ScriptPad first(Parse("1:a,2-4:rt,60-65:disconnect"), 0, Now);
  ScriptPad second(Parse("61-62:disconnect,62-64:disconnect"), 1, Now);
  X_INPUT_STATE state{};
  seconds = 1;
  assert(first.GetDeviceState(kFirstScriptPad, &state) == X_ERROR_SUCCESS);
  assert(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  const uint32_t packet = state.packet_number;
  assert(first.GetDeviceState(kFirstScriptPad, &state) == X_ERROR_SUCCESS);
  assert(uint32_t(state.packet_number) == packet);  // polling doesn't advance time
  seconds = 1.25;
  first.GetDeviceState(kFirstScriptPad, &state);
  assert(uint16_t(state.gamepad.buttons) == 0);
  seconds = 2;
  first.GetDeviceState(kFirstScriptPad, &state);
  assert(state.gamepad.right_trigger == 255);
  for (double t : {59.999, 60.0, 61.0, 62.0, 64.0, 65.0}) {
    seconds = t;
    ExpectConnected(first, kFirstScriptPad, t < 60 || t >= 65);
    ExpectConnected(second, kSecondScriptPad, t < 61 || t >= 64);
  }
  assert(second.GetDeviceState(kFirstScriptPad, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  seconds = 0;
  ScriptPad initially_unplugged(Parse("0-2:disconnect"), 1, Now);
  ExpectConnected(initially_unplugged, kSecondScriptPad, false);
  seconds = 2;
  ExpectConnected(initially_unplugged, kSecondScriptPad, true);

  // Each interface must observe an unplug without a preceding state poll.
  for (int query = 0; query < 5; ++query) {
    seconds = 0;
    ScriptPad pad(Parse("1-2:disconnect"), 1, Now);
    seconds = 1;
    X_INPUT_CAPABILITIES caps{};
    X_INPUT_KEYSTROKE key{};
    X_INPUT_VIBRATION vibration{};
    std::vector<DeviceInfo> devices;
    switch (query) {
      case 0: pad.EnumerateDevices(devices); assert(devices.empty()); break;
      case 1: assert(pad.GetDeviceState(kSecondScriptPad, &state) == X_ERROR_DEVICE_NOT_CONNECTED); break;
      case 2: assert(pad.GetDeviceCapabilities(kSecondScriptPad, 0, &caps) == X_ERROR_DEVICE_NOT_CONNECTED); break;
      case 3: assert(pad.GetDeviceKeystroke(kSecondScriptPad, 0, &key) == X_ERROR_DEVICE_NOT_CONNECTED); break;
      case 4: assert(pad.SetDeviceVibration(kSecondScriptPad, &vibration) == X_ERROR_DEVICE_NOT_CONNECTED); break;
    }
    seconds = 2;
    ExpectConnected(pad, kSecondScriptPad, true);
  }
}

static void StaleInputCases() {
  for (bool sample_unplug : {false, true}) {
    seconds = 0;
    ScriptPad pad(Parse("0-4:a,1-2:disconnect,1.25:b,3:x"), 1, Now);
    X_INPUT_STATE state{};
    X_INPUT_KEYSTROKE key{};
    pad.GetDeviceState(kSecondScriptPad, &state);  // leave A down queued
    const uint32_t packet = state.packet_number;
    if (sample_unplug) {
      seconds = 1;
      ExpectConnected(pad, kSecondScriptPad, false);
    }
    seconds = 2;
    assert(pad.GetDeviceKeystroke(kSecondScriptPad, 0, &key) == X_ERROR_SUCCESS);
    assert(uint16_t(key.virtual_key) == 0x5800 && uint16_t(key.flags) == X_INPUT_KEYSTROKE_KEYDOWN);
    assert(key.user_index == 1);
    assert(pad.GetDeviceKeystroke(kSecondScriptPad, 0, &key) == X_ERROR_EMPTY);
    pad.GetDeviceState(kSecondScriptPad, &state);
    assert(uint32_t(state.packet_number) > packet);
    seconds = 4;
    assert(pad.GetDeviceKeystroke(kSecondScriptPad, 0, &key) == X_ERROR_SUCCESS);
    assert(uint16_t(key.virtual_key) == 0x5800 && uint16_t(key.flags) == X_INPUT_KEYSTROKE_KEYUP);
    assert(pad.GetDeviceKeystroke(kSecondScriptPad, 0, &key) == X_ERROR_EMPTY);
  }
  seconds = 0;
  ScriptPad expired(Parse("0:a,1-2:disconnect"), 1, Now);
  expired.GetDeviceState(kSecondScriptPad, nullptr); // queued press predates missed unplug
  seconds = 3;
  X_INPUT_KEYSTROKE key{};
  assert(expired.GetDeviceKeystroke(kSecondScriptPad, 0, &key) == X_ERROR_EMPTY);
}

static void ProductionBoundaryCases() {
  RuntimeConfig config;
  config.input_factory = [](bool) { return std::make_unique<InputSystem>(nullptr); };
  WrapInput(config);  // empty defaults leave factory unwrapped
  auto system = config.input_factory(false);
  X_INPUT_STATE state{};
  assert(static_cast<InputSystem*>(system.get())->GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  FLAGS_test_buttons_p2 = "0-3600:a,60-65:disconnect";
  WrapInput(config);
  auto tools = config.input_factory(true);
  assert(static_cast<InputSystem*>(tools.get())->GetState(1, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  auto wrapped = config.input_factory(false);
  auto* input = static_cast<InputSystem*>(wrapped.get());
  assert(input->GetState(0, &state) == X_ERROR_SUCCESS);
  assert(uint16_t(state.gamepad.buttons) == 0);
  assert(input->GetState(1, &state) == X_ERROR_SUCCESS);
  assert(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);

  // Actual SDK assignment and queries, with deterministic script time. The
  // SDK's existing enumeration cache is real time: allow its 50 ms to expire.
  seconds = 0;
  InputSystem routed(nullptr);
  routed.AddDriver(std::make_unique<ScriptPad>(Parse("0-4:x"), 0, Now));
  routed.AddDriver(std::make_unique<ScriptPad>(Parse("0-4:a,1-2:disconnect"), 1, Now));
  routed.SetDeviceAssignment(std::make_unique<ScriptPadAssignment>());
  for (double t : {0.0, 1.0, 2.0}) {
    seconds = t;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const auto expected = t == 1 ? X_ERROR_DEVICE_NOT_CONNECTED : X_ERROR_SUCCESS;
    X_INPUT_CAPABILITIES caps{};
    X_INPUT_VIBRATION vibration{};
    X_INPUT_KEYSTROKE key{};
    assert(routed.GetState(0, &state) == X_ERROR_SUCCESS);
    assert(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_X);
    assert(routed.GetState(1, &state) == expected);
    if (t != 1) assert(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
    assert(routed.GetCapabilities(1, 0, &caps) == expected);
    assert(routed.SetState(1, &vibration) == expected);
    assert(routed.GetKeystroke(1, 0, &key) == expected);
    if (t != 1) assert(key.user_index == 1 && uint16_t(key.virtual_key) == 0x5800);
    assert(routed.GetState(2, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
  bench::buttons = 1u << 24;
  const auto button_calls = bench::button_calls, tilt_calls = bench::tilt_calls;
  seconds = 0;
  ScriptPad second({}, 1, Now);
  second.GetDeviceState(kSecondScriptPad, &state);
  assert(state.gamepad.thumb_rx == 0);
  assert(bench::button_calls == button_calls && bench::tilt_calls == tilt_calls);
  ScriptPad first({}, 0, Now);
  first.GetDeviceState(kFirstScriptPad, &state);
  assert(int16_t(state.gamepad.thumb_rx) == 32767 * 20 / 100);
  assert(bench::button_calls > button_calls && bench::tilt_calls > tilt_calls);
}

static void ResetScripts() {
  FLAGS_test_buttons.clear();
  FLAGS_test_buttons_p2.clear();
  FLAGS_test_buttons_p3.clear();
  FLAGS_test_buttons_p4.clear();
  bench::active = false;
  bench::buttons = 0;
}

static RuntimeConfig BaseConfig() {
  RuntimeConfig config;
  config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = std::make_unique<InputSystem>(nullptr);
    input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
    return input;
  };
  return config;
}

static void FourFactoryCases() {
  ResetScripts();
  std::array<std::string*, kMaxGuestUsers> scripts{
      &FLAGS_test_buttons, &FLAGS_test_buttons_p2,
      &FLAGS_test_buttons_p3, &FLAGS_test_buttons_p4};
  constexpr std::array<uint16_t, 4> bits{
      X_INPUT_GAMEPAD_A, X_INPUT_GAMEPAD_B, X_INPUT_GAMEPAD_X, X_INPUT_GAMEPAD_Y};
  const std::array<std::string, 4> valid{"0-3600:a", "0-3600:b", "0-3600:x", "0-3600:y"};
  // Empty or invalid scripts leave the original factory and all ports alone.
  for (const std::string invalid : {"", "bad", "nan:a,2:unknown,4:disconnect,3-2:b"}) {
    for (auto* script : scripts) *script = invalid;
    auto config = BaseConfig();
    const auto& type = config.input_factory.target_type();
    WrapInput(config);
    assert(config.input_factory.target_type() == type);
    auto system = config.input_factory(false);
    X_INPUT_STATE state{};
    for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
      assert(static_cast<InputSystem*>(system.get())->GetState(user, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
    }
  }
  for (uint32_t enabled = 0; enabled < kMaxGuestUsers; ++enabled) {
    ResetScripts();
    // A single higher script must not compact down to player two. Invalid
    // scripts in the other slots must not create extra connected devices.
    for (auto* script : scripts) *script = "bad";
    *scripts[enabled] = valid[enabled];
    auto config = BaseConfig();
    WrapInput(config);
    auto tools = config.input_factory(true);
    X_INPUT_STATE state{};
    for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
      assert(static_cast<InputSystem*>(tools.get())->GetState(user, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
    }
    auto system = config.input_factory(false);
    auto* input = static_cast<InputSystem*>(system.get());
    for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
      const bool connected = user == 0 || user == enabled;
      assert(input->GetState(user, &state) ==
             (connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED));
      if (connected) assert(uint16_t(state.gamepad.buttons) == (user == enabled ? bits[user] : 0));
    }
  }
  ResetScripts();
  for (uint32_t user = 0; user < kMaxGuestUsers; ++user) *scripts[user] = valid[user];
  auto config = BaseConfig();
  WrapInput(config);
  auto system = config.input_factory(false);
  auto* input = static_cast<InputSystem*>(system.get());
  for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
    X_INPUT_STATE state{};
    X_INPUT_KEYSTROKE key{};
    assert(input->GetState(user, &state) == X_ERROR_SUCCESS);
    assert(uint16_t(state.gamepad.buttons) == bits[user]);
    assert(input->GetKeystroke(user, 0, &key) == X_ERROR_SUCCESS);
    assert(key.user_index == user && uint16_t(key.virtual_key) == 0x5800 + user);
  }
  assert(input->GetState(4, nullptr) == X_ERROR_DEVICE_NOT_CONNECTED);

  // Benchmark alone still creates only P1; secondary pads never sample it.
  ResetScripts();
  bench::active = true;
  bench::buttons = 1u << 24;
  config = BaseConfig();
  WrapInput(config);
  system = config.input_factory(false);
  input = static_cast<InputSystem*>(system.get());
  X_INPUT_STATE state{};
  assert(input->GetState(0, &state) == X_ERROR_SUCCESS);
  assert(int16_t(state.gamepad.thumb_rx) == 32767 * 20 / 100);
  for (uint32_t user = 1; user < kMaxGuestUsers; ++user) {
    assert(input->GetState(user, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
    const auto buttons = bench::button_calls, tilts = bench::tilt_calls;
    ScriptPad pad(Parse("0-4:x"), user, Now);
    assert(pad.GetDeviceState(kScriptPads[user], &state) == X_ERROR_SUCCESS);
    assert(state.gamepad.thumb_rx == 0);
    assert(bench::button_calls == buttons && bench::tilt_calls == tilts);
  }
  ResetScripts();
  // A missing factory remains missing, even with valid P4, and null/tool
  // results from the original factory must be preserved.
  FLAGS_test_buttons_p4 = "0:a";
  config = {};
  WrapInput(config);
  assert(!config.input_factory);
  config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> { return nullptr; };
  WrapInput(config);
  assert(!config.input_factory(false));
  ResetScripts();
}

static void FourDisconnectCases() {
  constexpr std::array<const char*, 4> buttons{"a", "b", "x", "y"};
  for (uint32_t disconnected = 0; disconnected < kMaxGuestUsers; ++disconnected) {
    seconds = 0;
    InputSystem input(nullptr);
    for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
      std::string script = std::string("0-4:") + buttons[user];
      if (user == disconnected) script += ",1-2:disconnect,1.25:start";
      input.AddDriver(std::make_unique<ScriptPad>(Parse(script), user, Now));
    }
    input.SetDeviceAssignment(std::make_unique<ScriptPadAssignment>());
    for (double t : {0.0, 1.0, 2.0, 4.0}) {
      seconds = t;
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
        const bool connected = t != 1 || user != disconnected;
        const auto expected = connected ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
        X_INPUT_STATE state{};
        X_INPUT_CAPABILITIES caps{};
        X_INPUT_KEYSTROKE key{};
        X_INPUT_VIBRATION vibration{};
        assert(input.GetState(user, &state) == expected);
        if (connected) assert(uint16_t(state.gamepad.buttons) == (t < 4 ? uint16_t(0x1000 << user) : 0));
        assert(input.GetCapabilities(user, 0, &caps) == expected);
        assert(input.SetState(user, &vibration) == expected);
        auto result = input.GetKeystroke(user, 0, &key);
        assert(connected ? (result == X_ERROR_SUCCESS || result == X_ERROR_EMPTY) : result == expected);
        if (result == X_ERROR_SUCCESS) {
          assert(key.user_index == user && uint16_t(key.virtual_key) == 0x5800 + user);
        }
        // Drain keys so reconnect can only produce this user's current hold,
        // never the START press that expired while this pad was disconnected.
        while (result == X_ERROR_SUCCESS) {
          result = input.GetKeystroke(user, 0, &key);
          if (result == X_ERROR_SUCCESS) assert(uint16_t(key.virtual_key) == 0x5800 + user);
        }
      }
    }
  }
}

class PhysicalPads final : public InputDriver {
 public:
  PhysicalPads() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    out.push_back({DeviceId{99}, 0, "Keyboard", "", true});
    for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
      out.push_back({DeviceId{10 + user}, 0, "Physical pad", "", false});
    }
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* state) override {
    *state = {};
    if (id != DeviceId{99}) state->gamepad.buttons = uint16_t(1u << (uint64_t(id) - 10));
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    *caps = {};
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE*) override { return X_ERROR_EMPTY; }
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }
};

static void PhysicalAndScriptedCases() {
  seconds = 0;
  InputSystem input(nullptr);
  input.AddDriver(std::make_unique<PhysicalPads>());
  constexpr std::array<const char*, 4> buttons{"a", "b", "x", "y"};
  for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
    input.AddDriver(std::make_unique<ScriptPad>(
        Parse(std::string("0-4:") + buttons[user] + ",1-2:disconnect"), user, Now));
  }
  input.SetDeviceAssignment(std::make_unique<ScriptPadAssignment>());
  for (double t : {0.0, 1.0, 2.0}) {
    seconds = t;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    for (uint32_t user = 0; user < kMaxGuestUsers; ++user) {
      X_INPUT_STATE state{};
      assert(input.GetState(user, &state) == X_ERROR_SUCCESS);
      const uint16_t script = t == 1 ? 0 : uint16_t(0x1000u << user);
      // Enumeration starts with a synthetic keyboard. Physical ordinals and
      // merging must remain independent of all four synthetic script pads.
      assert(uint16_t(state.gamepad.buttons) == uint16_t((1u << user) | script));
    }
  }
}

int main() {
  ScriptAndConnectionCases();
  StaleInputCases();
  ProductionBoundaryCases();
  FourFactoryCases();
  FourDisconnectCases();
  PhysicalAndScriptedCases();
  std::cout << "script pad: four-port timing, disconnect, stale keys, factory gates, benchmark and SDK routing passed\n";
}
