/**
 * @file        system/xthread_test.cpp
 * @brief       Tests for host-backed thread lifecycle behavior
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>

namespace {

using rex::X_STATUS;
using Platform = rex::ppc::FPSCRRegister::Platform;

#if defined(__x86_64__) || defined(_M_X64)
constexpr uint32_t ArmTraps(uint32_t csr) {
  return csr & ~Platform::ExceptionMask;
}
constexpr bool TrapsMasked(uint32_t csr) {
  return (csr & Platform::ExceptionMask) == Platform::ExceptionMask;
}
#else
constexpr uint32_t ArmTraps(uint32_t csr) {
  return csr | Platform::ExceptionMask;
}
constexpr bool TrapsMasked(uint32_t csr) {
  return (csr & Platform::ExceptionMask) == 0;
}
#endif

class HostileFpscrHostThread final : public rex::system::XHostThread {
 public:
  using XHostThread::XHostThread;

  void Execute() override {
    Platform::setcsr(ArmTraps(Platform::getcsr()));
    XHostThread::Execute();
  }
};

}  // namespace

TEST_CASE("XHostThread initializes host floating-point exception masks", "[runtime][thread][fpscr]") {
  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);

  std::atomic<uint32_t> callback_csr{0};
  std::atomic<bool> callback_called{false};
  auto thread = rex::system::object_ref<HostileFpscrHostThread>(new HostileFpscrHostThread(
      runtime.kernel_state(), 128 * 1024, rex::system::X_CREATE_SUSPENDED, [&]() {
        callback_csr.store(Platform::getcsr(), std::memory_order_relaxed);
        callback_called.store(true, std::memory_order_release);
        return 0;
      }));

  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  REQUIRE(thread->Resume() == X_STATUS_SUCCESS);
  REQUIRE(thread->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS);
  REQUIRE(callback_called.load(std::memory_order_acquire));
  CHECK(TrapsMasked(callback_csr.load(std::memory_order_relaxed)));
}

TEST_CASE("Suspended host threads expose suspend count and guest priority", "[runtime][thread]") {
  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  std::atomic<bool> called{false};
  auto thread = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
      runtime.kernel_state(), 128 * 1024, rex::system::X_CREATE_SUSPENDED, [&]() {
        called.store(true, std::memory_order_release);
        return 0;
      }));
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  CHECK(thread->suspend_count() == 1);
  CHECK_FALSE(called.load(std::memory_order_acquire));
  for (int32_t priority : {-40, 0, 20, 40}) {
    thread->SetPriority(priority);
    CHECK(thread->priority() == priority);
    CHECK(thread->guest_object<rex::system::X_KTHREAD>()->priority ==
          static_cast<uint8_t>(std::clamp(priority, 0, 31)));
  }
  uint32_t previous = 99;
  REQUIRE(thread->Resume(&previous) == X_STATUS_SUCCESS);
  CHECK(previous == 1);
  uint64_t timeout = static_cast<uint64_t>(-10000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS);
  CHECK(called.load(std::memory_order_acquire));
  CHECK(thread->suspend_count() == 0);
}
