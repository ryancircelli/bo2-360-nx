// XNotify*/XamNotifyCreateListener guest entry points on a real kernel state: startup notifications,
// broadcast, match-id dequeue, null out-pointers, and a Save/Restore round trip of a queued listener.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <vector>

#include <rex/runtime.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xnotifylistener.h>

namespace rex::kernel::xam {
u32 XamNotifyCreateListener_entry(u64 mask, u32 max_version);
u32 XamNotifyCreateListenerInternal_entry(u64 mask, u32 is_system, u32 max_version);
u32 XNotifyGetNext_entry(u32 handle, u32 match_id, mapped_u32 id_ptr, mapped_u32 param_ptr);
}  // namespace rex::kernel::xam

using namespace rex;
using namespace rex::kernel::xam;

namespace {

struct Fixture {
  rex::Runtime runtime{std::filesystem::current_path()};
  Fixture() {
    rex::RuntimeConfig config{};
    config.tool_mode = true;
    REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  }
  rex::system::KernelState* kernel() { return runtime.kernel_state(); }
};

// Category 1 has no synthetic startup notifications; version 0.
constexpr uint32_t Id(uint32_t local, uint32_t version = 0) { return (1u << 25) | (version << 16) | local; }
constexpr uint64_t kCategory1 = 2;

struct Got {
  bool any;
  uint32_t id, param;
};
Got Next(uint32_t handle, uint32_t match = 0) {
  be<uint32_t> id = 0xAAAA, param = 0xBBBB;
  const uint32_t r = XNotifyGetNext_entry(handle, match, mapped_u32(&id, 0), mapped_u32(&param, 0));
  return {r != 0, uint32_t(id), uint32_t(param)};
}

}  // namespace

TEST_CASE("The first system listener gets the startup notifications, later ones do not", "[runtime][notify][xam]") {
  Fixture f;
  const uint32_t first = XamNotifyCreateListener_entry(1, 10);
  const uint32_t second = XamNotifyCreateListener_entry(1, 10);
  const uint32_t expected[][2] = {{0x9, 1}, {0x9, 0},
                                  {0xA, f.kernel()->signed_in_user_mask()}, {0xA, f.kernel()->signed_in_user_mask()},
                                  {0x12, 0}, {0x12, 0}, {0x13, 0}, {0x13, 0}};
  for (const auto& e : expected) {
    auto g = Next(first);
    REQUIRE(g.any);
    CHECK(g.id == e[0]);
    CHECK(g.param == e[1]);
  }
  CHECK_FALSE(Next(first).any);
  CHECK_FALSE(Next(second).any);
}

TEST_CASE("A listener that is not a system listener does not consume the startup notifications", "[runtime][notify][xam]") {
  Fixture f;
  const uint32_t other = XamNotifyCreateListener_entry(kCategory1, 10);
  CHECK_FALSE(Next(other).any);
  const uint32_t sys = XamNotifyCreateListener_entry(1, 10);
  CHECK(Next(sys).any);
}

TEST_CASE("Broadcast reaches every matching listener once and skips mask and version mismatches", "[runtime][notify][xam]") {
  Fixture f;
  const uint32_t a = XamNotifyCreateListener_entry(kCategory1, 10);
  const uint32_t b = XamNotifyCreateListenerInternal_entry(kCategory1, 1, 10);
  const uint32_t other_category = XamNotifyCreateListener_entry(4, 10);
  const uint32_t old_version = XamNotifyCreateListener_entry(kCategory1, 0);
  f.kernel()->BroadcastNotification(Id(5, 1), 77);
  for (uint32_t h : {a, b}) {
    auto g = Next(h);
    REQUIRE(g.any);
    CHECK(g.id == Id(5, 1));
    CHECK(g.param == 77);
    CHECK_FALSE(Next(h).any);
  }
  CHECK_FALSE(Next(other_category).any);
  CHECK_FALSE(Next(old_version).any);  // version 1 is newer than its max 0
}

TEST_CASE("XNotifyGetNext match id takes the first matching entry and leaves the rest in order", "[runtime][notify][xam]") {
  Fixture f;
  const uint32_t h = XamNotifyCreateListener_entry(kCategory1, 10);
  f.kernel()->BroadcastNotification(Id(1), 10);
  f.kernel()->BroadcastNotification(Id(2), 20);
  f.kernel()->BroadcastNotification(Id(2), 21);
  auto m = Next(h, Id(2));
  REQUIRE(m.any);
  CHECK(m.id == Id(2));
  CHECK(m.param == 20);
  CHECK_FALSE(Next(h, Id(9)).any);
  CHECK(Next(h).param == 10);
  CHECK(Next(h).param == 21);
  CHECK_FALSE(Next(h).any);
}

TEST_CASE("XNotifyGetNext tolerates null pointers and unknown handles and zeroes its outputs", "[runtime][notify][xam]") {
  Fixture f;
  const uint32_t h = XamNotifyCreateListener_entry(kCategory1, 10);
  f.kernel()->BroadcastNotification(Id(3), 5);
  // No id pointer: nothing is consumed.
  be<uint32_t> param = 0xBBBB;
  CHECK(XNotifyGetNext_entry(h, 0, nullptr, mapped_u32(&param, 0)) == 0);
  CHECK(uint32_t(param) == 0);
  // No param pointer (one retail demo does this) still dequeues.
  be<uint32_t> id = 0xAAAA;
  CHECK(XNotifyGetNext_entry(h, 0, mapped_u32(&id, 0), nullptr) == 1);
  CHECK(uint32_t(id) == Id(3));
  // Unknown handle.
  id = 0xAAAA;
  param = 0xBBBB;
  CHECK(XNotifyGetNext_entry(0x7777, 0, mapped_u32(&id, 0), mapped_u32(&param, 0)) == 0);
  CHECK(uint32_t(id) == 0);
  CHECK(uint32_t(param) == 0);
}

TEST_CASE("A restored listener with queued notifications is signaled", "[runtime][notify][xam]") {
  std::vector<uint8_t> blob(4096);
  uint32_t handle = 0;
  {
    Fixture f;
    handle = XamNotifyCreateListener_entry(kCategory1, 10);
    f.kernel()->BroadcastNotification(Id(4), 44);
    auto listener = f.kernel()->object_table()->LookupObject<rex::system::XNotifyListener>(handle);
    REQUIRE(listener);
    rex::stream::ByteStream out(blob.data(), blob.size());
    REQUIRE(listener->Save(&out));
  }
  Fixture g;
  rex::stream::ByteStream in(blob.data(), blob.size());
  // Save wrote the object header first; Restore reads it back in the same order.
  auto restored = rex::system::XNotifyListener::Restore(g.kernel(), &in);
  REQUIRE(restored);
  uint64_t timeout = 0;
  CHECK(restored->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS);  // a guest waiting on it must wake
  auto got = Next(handle);
  REQUIRE(got.any);
  CHECK(got.id == Id(4));
  CHECK(got.param == 44);
  CHECK(restored->Wait(0, 0, 0, &timeout) == X_STATUS_TIMEOUT);
}
