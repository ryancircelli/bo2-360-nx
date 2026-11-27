/**
 * @file        system/xthread_test.cpp
 * @brief       Tests for host-backed thread lifecycle behavior
 */

#include <algorithm>
#include <atomic>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsemaphore.h>
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
// Every guest thread is created suspended and then resumed (XThread::Create).
TEST_CASE("A thread created suspended can be suspended again from outside", "[thread][suspend]") {
  std::atomic<uint64_t> ticks{0};
  std::atomic<bool> stop{false};
  rex::thread::Thread::CreationParameters params;
  params.create_suspended = true;
  auto thread = rex::thread::Thread::Create(params, [&]() {
    while (!stop.load(std::memory_order_acquire)) {
      ticks.fetch_add(1, std::memory_order_relaxed);
    }
  });
  REQUIRE(thread);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(ticks.load(std::memory_order_relaxed) == 0);  // not started yet
  REQUIRE(thread->Resume());
  auto moved = [&](uint64_t from) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ticks.load(std::memory_order_relaxed) == from && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ticks.load(std::memory_order_relaxed) != from;
  };
  REQUIRE(moved(0));
  for (int round = 0; round < 3; ++round) {  // each resume must leave the next suspend working
    REQUIRE(thread->Suspend());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the signal park it
    uint64_t parked = ticks.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(ticks.load(std::memory_order_relaxed) == parked);
    REQUIRE(thread->Resume());
    CHECK(moved(parked));
  }
  stop.store(true, std::memory_order_release);
  REQUIRE(rex::thread::Wait(thread.get(), false) == rex::thread::WaitResult::kSuccess);
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

// Stress: the host thread names itself at start (Create) while other threads run SetThreadName on it; both write the
// host thread's name string. Before the fix this double-freed the string ("pointer being freed was not allocated").
TEST_CASE("XThread::set_name is safe against the start-up name write", "[runtime][thread][name]") {
  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);

  for (int round = 0; round < 300; ++round) {
    auto thread = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        runtime.kernel_state(), 128 * 1024, rex::system::X_CREATE_SUSPENDED, [] { return 0; }));
    REQUIRE(thread->Create() == X_STATUS_SUCCESS);
    std::atomic<bool> go{false};
    std::vector<std::thread> namers;
    for (int i = 0; i < 3; ++i) {
      namers.emplace_back([&, i] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int n = 0; n < 20; ++n) {
          thread->set_name("a-fairly-long-thread-name-to-force-heap-allocation-" + std::to_string(i * 100 + n));
        }
      });
    }
    go.store(true, std::memory_order_release);
    REQUIRE(thread->Resume() == X_STATUS_SUCCESS);
    thread->Wait(0, 0, 0, nullptr);
    for (auto& t : namers) t.join();
  }
}

// An object created after a save point's capture loses its handle slot on rewind but keeps the handle in
// its own list; the slot's next owner must not be closed through it.
TEST_CASE("An object that lost its handle slot cannot close the slot's next owner", "[runtime][object]") {
  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* table = runtime.kernel_state()->object_table();

  auto saved = table->CaptureRewindState();
  auto stale = rex::system::object_ref<rex::system::XEvent>(new rex::system::XEvent(runtime.kernel_state()));
  stale->Initialize(true, false);
  const auto handle = stale->handle();
  table->Rewind(saved);

  auto owner = rex::system::object_ref<rex::system::XEvent>(new rex::system::XEvent(runtime.kernel_state()));
  owner->Initialize(true, false);
  REQUIRE(owner->handle() == handle);

  stale->ReleaseHandle();
  CHECK(table->LookupObject<rex::system::XObject>(handle).get() == owner.get());

  CHECK(table->ReleaseHandle(handle) == X_STATUS_SUCCESS);
  CHECK(table->LookupObject<rex::system::XObject>(handle).get() == nullptr);
  CHECK(table->ReleaseHandle(handle) == X_STATUS_INVALID_HANDLE);  // closed handles stay closed
  CHECK(table->RetainHandle(handle) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("Starting a deferred overlapped call clears the caller's event", "[runtime][overlapped]") {
  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* ks = runtime.kernel_state();

  auto event = rex::system::object_ref<rex::system::XEvent>(new rex::system::XEvent(ks));
  event->Initialize(true, true);  // manual reset, still set from "the previous call"
  const uint32_t overlapped = ks->memory()->SystemHeapAlloc(sizeof(rex::system::XAM_OVERLAPPED));
  REQUIRE(overlapped != 0);
  auto* ov = ks->memory()->TranslateVirtual<rex::system::XAM_OVERLAPPED*>(overlapped);
  *ov = {};
  ov->event = event->handle();

  std::atomic<uint32_t> wait_status{0xFFFFFFFFu};
  // The deferred call records the calling guest thread, so it has to be made from one.
  auto caller = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(ks, 128 * 1024, 0, [&]() {
        ks->CompleteOverlappedDeferred([]() {}, overlapped, 0);
        uint64_t no_wait = 0;
        wait_status.store(event->Wait(0, 0, 0, &no_wait), std::memory_order_release);
        return 0;
      }));
  REQUIRE(caller->Create() == X_STATUS_SUCCESS);
  REQUIRE(caller->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS);
  CHECK(wait_status.load(std::memory_order_acquire) == uint32_t(X_STATUS_TIMEOUT));
}

TEST_CASE("A guest object body that is initialised again gets a fresh host object", "[runtime][object]") {
  rex::Runtime runtime(std::filesystem::current_path());
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* ks = runtime.kernel_state();
  auto* table = ks->object_table();

  const uint32_t body = ks->memory()->SystemHeapAlloc(sizeof(rex::system::X_KSEMAPHORE));
  REQUIRE(body != 0);
  auto* sem = ks->memory()->TranslateVirtual<rex::system::X_KSEMAPHORE*>(body);
  // What KeInitializeSemaphore does to the body.
  auto init = [&](uint32_t count) {
    std::memset(static_cast<void*>(sem), 0, sizeof(*sem));
    sem->header.type = 5;
    sem->header.signal_state = count;
    sem->limit = 1;
    return rex::system::XObject::GetNativeObject<rex::system::XSemaphore>(ks, sem, 5);
  };

  auto first = init(1);  // left with one count that nobody takes
  REQUIRE(first);
  const auto first_handle = first->handle();
  first.reset();

  auto second = init(0);
  REQUIRE(second);
  uint64_t no_wait = 0;
  CHECK(second->Wait(0, 0, 0, &no_wait) == X_STATUS_TIMEOUT);  // the old count is not inherited
  // The first use's object is gone (its slot is free or already reused by the second), not leaked.
  auto at_first = table->LookupObject<rex::system::XObject>(first_handle);
  CHECK((!at_first || at_first.get() == second.get()));

  const size_t before = table->GetAllObjects().size();
  for (int i = 0; i < 50; ++i) {
    REQUIRE(init(0));
  }
  CHECK(table->GetAllObjects().size() == before);
}

#if !defined(_WIN32)
TEST_CASE("Waiting twice on a finished thread does not join a newer thread", "[thread][join]") {
  using namespace std::chrono_literals;
  auto first = rex::thread::Thread::Create({}, []() {});
  REQUIRE(first);
  REQUIRE(rex::thread::Wait(first.get(), false, 2000ms) == rex::thread::WaitResult::kSuccess);

  std::atomic<bool> stop{false};
  // Likely to reuse the finished thread's stack, and with it the pthread_t.
  auto second = rex::thread::Thread::Create({}, [&]() {
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(1ms);
    }
  });
  REQUIRE(second);

  std::atomic<bool> done{false};
  std::thread waiter([&]() {
    rex::thread::Wait(first.get(), false, 2000ms);
    done.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(300ms);
  const bool returned = done.load(std::memory_order_acquire);
  stop.store(true, std::memory_order_release);  // also frees a waiter stuck joining the second thread
  waiter.join();
  CHECK(returned);
  REQUIRE(rex::thread::Wait(second.get(), false, 2000ms) == rex::thread::WaitResult::kSuccess);
}
#endif

TEST_CASE("A zero-timeout wait on several objects sees a signaled one while another is busy",
          "[thread][wait]") {
  auto busy = rex::thread::Event::CreateManualResetEvent(false);
  auto set = rex::thread::Event::CreateManualResetEvent(true);
  std::atomic<bool> stop{false};
  // Keeps the first object's mutex busy; the object itself stays unsignaled between passes.
  std::thread hammer([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      busy->Reset();
    }
  });
  rex::thread::WaitHandle* handles[] = {busy.get(), set.get()};
  int timeouts = 0;
  for (int i = 0; i < 200000; ++i) {
    auto result = rex::thread::WaitAny(handles, 2, false, std::chrono::milliseconds(0));
    if (result.first != rex::thread::WaitResult::kSuccess) ++timeouts;
  }
  stop = true;
  hammer.join();
  CHECK(timeouts == 0);
}
