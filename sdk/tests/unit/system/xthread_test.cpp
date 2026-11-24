/**
 * @file        system/xthread_test.cpp
 * @brief       Tests for host-backed thread lifecycle behavior
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/thread/fiber.h>

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

#if !defined(_WIN32)
TEST_CASE("Nested host suspends resume the thread after the matching resumes", "[thread][suspend]") {
  std::atomic<uint64_t> ticks{0};
  std::atomic<bool> stop{false};
  auto thread = rex::thread::Thread::Create({}, [&]() {
    while (!stop.load(std::memory_order_acquire)) {
      ticks.fetch_add(1, std::memory_order_relaxed);
    }
  });
  REQUIRE(thread);
  REQUIRE(thread->Suspend());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the first suspend park it
  REQUIRE(thread->Suspend());
  REQUIRE(thread->Resume());
  REQUIRE(thread->Resume());
  uint64_t before = ticks.load(std::memory_order_relaxed);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (ticks.load(std::memory_order_relaxed) == before && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(ticks.load(std::memory_order_relaxed) != before);
  stop.store(true, std::memory_order_release);
  REQUIRE(rex::thread::Wait(thread.get(), false) == rex::thread::WaitResult::kSuccess);
}
#endif

#if !defined(_WIN32)
namespace {
rex::thread::Fiber* g_test_main_fiber = nullptr;
volatile uint8_t g_deep_sum = 0;

void DeepStackFiber(void* done) {
  volatile uint8_t big[4 * 1024 * 1024];  // more than the old 256 KB fiber stack
  for (size_t i = 0; i < sizeof(big); i += 4096) big[i] = uint8_t(i >> 12);
  g_deep_sum = big[sizeof(big) - 4096];
  *static_cast<bool*>(done) = true;
  rex::thread::Fiber::SwitchTo(g_test_main_fiber);
}
}  // namespace

TEST_CASE("Fibers run code that needs megabytes of host stack", "[thread][fiber]") {
  bool done = false;
  std::thread([&]() {
    g_test_main_fiber = rex::thread::Fiber::ConvertCurrentThread();
    auto* fiber = rex::thread::Fiber::Create(16u * 1024u * 1024u, DeepStackFiber, &done);
    if (!fiber) return;  // done stays false
    rex::thread::Fiber::SwitchTo(fiber);
    fiber->Destroy();
    g_test_main_fiber->Destroy();
  }).join();
  CHECK(done);
}
#endif
