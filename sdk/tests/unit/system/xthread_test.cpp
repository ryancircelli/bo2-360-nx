/**
 * @file        system/xthread_test.cpp
 * @brief       Tests for host-backed thread lifecycle behavior
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

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

#if defined(__APPLE__)
TEST_CASE("Thread priority changes keep the user-interactive QoS class on macOS", "[thread][qos]") {
  std::atomic<bool> priority_set{false};
  std::atomic<qos_class_t> qos{QOS_CLASS_UNSPECIFIED};
  auto thread = rex::thread::Thread::Create({}, [&]() {
    while (!priority_set.load(std::memory_order_acquire)) {
    }
    qos_class_t q = QOS_CLASS_UNSPECIFIED;
    pthread_get_qos_class_np(pthread_self(), &q, nullptr);
    qos.store(q, std::memory_order_release);
  });
  REQUIRE(thread);
  thread->set_priority(rex::thread::ThreadPriority::kAboveNormal);
  priority_set.store(true, std::memory_order_release);
  REQUIRE(rex::thread::Wait(thread.get(), false) == rex::thread::WaitResult::kSuccess);
  CHECK(qos.load(std::memory_order_acquire) == QOS_CLASS_USER_INTERACTIVE);
}
#endif
