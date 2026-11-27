#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <utility>

#include <rex/runtime.h>
#include <rex/system/xevent.h>

using rex::X_STATUS;

namespace {
struct EventRuntime {
  rex::Runtime runtime{std::filesystem::current_path()};
  EventRuntime() {
    rex::RuntimeConfig config{};
    config.tool_mode = true;
    REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  }
};

rex::X_STATUS Poll(rex::system::XEvent& event) {
  uint64_t timeout = 0;
  return event.Wait(0, 0, 0, &timeout);
}
}  // namespace

TEST_CASE("Created events report their reset type and current state", "[runtime][event]") {
  EventRuntime fixture;
  for (bool manual : {false, true}) {
    auto event = rex::system::object_ref<rex::system::XEvent>(
        new rex::system::XEvent(fixture.runtime.kernel_state()));
    event->Initialize(manual, false);
    uint32_t type = 99, state = 99;
    event->Query(&type, &state);
    CHECK(type == (manual ? 0u : 1u));
    CHECK(state == 0);
    event->Set(0, false);
    event->Query(&type, &state);
    CHECK(type == (manual ? 0u : 1u));
    CHECK(state == 1);
    // Query must leave a pending signal available to the waiter.
    CHECK(Poll(*event) == X_STATUS_SUCCESS);
    CHECK(Poll(*event) == (manual ? X_STATUS_SUCCESS : X_STATUS_TIMEOUT));
    event->Reset();
    event->Query(nullptr, &state);
    CHECK(state == 0);
    CHECK(Poll(*event) == X_STATUS_TIMEOUT);
  }
}

TEST_CASE("Manual reset events remain signaled until cleared", "[runtime][event]") {
  EventRuntime fixture;
  auto event = rex::system::object_ref<rex::system::XEvent>(
      new rex::system::XEvent(fixture.runtime.kernel_state()));
  event->Initialize(true, true);
  CHECK(Poll(*event) == X_STATUS_SUCCESS);
  CHECK(Poll(*event) == X_STATUS_SUCCESS);
  event->Clear();
  CHECK(Poll(*event) == X_STATUS_TIMEOUT);
  event->Set(0, false);
  CHECK(Poll(*event) == X_STATUS_SUCCESS);
}

TEST_CASE("Auto reset events consume one signal and coalesce repeated sets", "[runtime][event]") {
  EventRuntime fixture;
  auto event = rex::system::object_ref<rex::system::XEvent>(
      new rex::system::XEvent(fixture.runtime.kernel_state()));
  event->Initialize(false, true);
  CHECK(Poll(*event) == X_STATUS_SUCCESS);
  CHECK(Poll(*event) == X_STATUS_TIMEOUT);
  event->Set(0, false);
  event->Set(0, false);
  CHECK(Poll(*event) == X_STATUS_SUCCESS);
  CHECK(Poll(*event) == X_STATUS_TIMEOUT);
}
