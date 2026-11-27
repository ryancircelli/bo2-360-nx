#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <vector>
#include <utility>

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

using rex::X_STATUS;
using rex::X_RESULT;

namespace {
struct Fixture {
  rex::Runtime runtime{std::filesystem::current_path()};
  Fixture() {
    rex::RuntimeConfig config{};
    config.tool_mode = true;
    REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  }
  rex::system::KernelState* kernel() { return runtime.kernel_state(); }
  template <typename T> rex::system::object_ref<T> New() {
    return rex::system::object_ref<T>(new T(kernel()));
  }
};
rex::X_STATUS Poll(rex::system::XObject& object) {
  uint64_t timeout = 0;
  return object.Wait(0, 0, 0, &timeout);
}
}  // namespace

TEST_CASE("Semaphores consume counts and reject overflow without changing count", "[runtime][semaphore]") {
  Fixture f;
  auto sem = f.New<rex::system::XSemaphore>();
  REQUIRE(sem->Initialize(1, 2));
  CHECK(sem->maximum_count() == 2);
  CHECK(Poll(*sem) == X_STATUS_SUCCESS);
  CHECK(Poll(*sem) == X_STATUS_TIMEOUT);
  int32_t previous = -1;
  REQUIRE(sem->ReleaseSemaphore(2, &previous));
  CHECK(previous == 0);
  CHECK_FALSE(sem->ReleaseSemaphore(1, nullptr));
  CHECK(Poll(*sem) == X_STATUS_SUCCESS);
  CHECK(Poll(*sem) == X_STATUS_SUCCESS);
  CHECK(Poll(*sem) == X_STATUS_TIMEOUT);
}

// Mutant/timer ownership needs a guest-visible current thread.
template <typename F> void RunOnHost(Fixture& f, F fn) {
  auto* thread = new rex::system::XHostThread(f.kernel(), 128 * 1024, 0, [fn]() -> uint32_t {
    fn();
    return 0;
  });
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  uint64_t join = static_cast<uint64_t>(-100000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &join) == X_STATUS_SUCCESS);
  thread->Release();
}

TEST_CASE("Mutants are recursive and require matching releases", "[runtime][mutant]") {
  Fixture f;
  RunOnHost(f, [&] {
  auto mutant = f.New<rex::system::XMutant>();
  mutant->Initialize(false);
  CHECK(Poll(*mutant) == X_STATUS_SUCCESS);
  CHECK(Poll(*mutant) == X_STATUS_SUCCESS);
  CHECK(mutant->ReleaseMutant(0, false, false) == X_STATUS_SUCCESS);
  CHECK(mutant->ReleaseMutant(0, false, false) == X_STATUS_SUCCESS);
  CHECK(mutant->ReleaseMutant(0, false, false) == X_STATUS_MUTANT_NOT_OWNED);
  });
}

TEST_CASE("Wait-any returns the signaled index and failed wait-all preserves signals", "[runtime][wait]") {
  Fixture f;
  auto first = f.New<rex::system::XEvent>();
  auto second = f.New<rex::system::XEvent>();
  first->Initialize(false, false);
  second->Initialize(false, true);
  rex::system::XObject* objects[] = {first.get(), second.get()};
  uint64_t zero = 0;
  CHECK(rex::system::XObject::WaitMultiple(2, objects, 0, 0, 0, 0, &zero) == X_STATUS_TIMEOUT);
  CHECK(rex::system::XObject::WaitMultiple(2, objects, 1, 0, 0, 0, &zero) == X_STATUS_SUCCESS + 1);
  CHECK(Poll(*second) == X_STATUS_TIMEOUT);
  first->Set(0, false);
  second->Set(0, false);
  CHECK(rex::system::XObject::WaitMultiple(2, objects, 0, 0, 0, 0, &zero) == X_STATUS_SUCCESS);
  CHECK(Poll(*first) == X_STATUS_TIMEOUT);
  CHECK(Poll(*second) == X_STATUS_TIMEOUT);
}

TEST_CASE("Relative timers fire with their reset semantics and can be cancelled", "[runtime][timer]") {
  Fixture f;
  // SetTimer needs a guest-visible current thread, so run the body on a host thread.
  auto* thread = new rex::system::XHostThread(f.kernel(), 128 * 1024, 0, [&]() -> uint32_t {
  for (uint32_t type : {0u, 1u}) {
    auto timer = f.New<rex::system::XTimer>();
    timer->Initialize(type);
    CHECK(Poll(*timer) == X_STATUS_TIMEOUT);
    CHECK(timer->SetTimer(-10000, 0, 0, 0, false) == X_STATUS_SUCCESS);
    // Bounded wait allows scheduling delays without testing exact wall time.
    uint64_t timeout = static_cast<uint64_t>(-10000000ll);
    CHECK(timer->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS);
    CHECK(Poll(*timer) == (type == 0 ? X_STATUS_SUCCESS : X_STATUS_TIMEOUT));
    CHECK(timer->SetTimer(-100000000, 0, 0, 0, false) == X_STATUS_SUCCESS);
    CHECK(timer->Cancel() == X_STATUS_SUCCESS);
    CHECK(Poll(*timer) == X_STATUS_TIMEOUT);
    CHECK(timer->SetTimer(-10000, 0, 0, 0, true) == X_STATUS_TIMER_RESUME_IGNORED);
  }
    return 0;
  });
  CHECK(thread->Create() == X_STATUS_SUCCESS);
  uint64_t join = static_cast<uint64_t>(-100000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &join) == X_STATUS_SUCCESS);
  thread->Release();
}

TEST_CASE("Duplicate handles retain the object after the original handle closes", "[runtime][handles]") {
  Fixture f;
  auto event = f.New<rex::system::XEvent>();
  event->Initialize(false, true);
  auto table = f.kernel()->object_table();
  auto original = event->handle();
  rex::X_HANDLE duplicate = 0;
  REQUIRE(table->DuplicateHandle(original, &duplicate) == X_STATUS_SUCCESS);
  CHECK(duplicate != original);
  CHECK(table->LookupObject<rex::system::XEvent>(original | 3).get() == event.get());
  CHECK_FALSE(table->LookupObject<rex::system::XSemaphore>(duplicate));
  REQUIRE(table->ReleaseHandle(original) == X_STATUS_SUCCESS);
  CHECK_FALSE(table->LookupObject<rex::system::XEvent>(original));
  auto retained = table->LookupObject<rex::system::XEvent>(duplicate);
  REQUIRE(retained);
  CHECK(Poll(*retained) == X_STATUS_SUCCESS);
  REQUIRE(table->ReleaseHandle(duplicate) == X_STATUS_SUCCESS);
  CHECK_FALSE(table->LookupObject<rex::system::XEvent>(duplicate));
  CHECK(table->DuplicateHandle(original, nullptr) == X_STATUS_INVALID_HANDLE);
  CHECK_FALSE(table->LookupObject<rex::system::XEvent>(0));
  CHECK_FALSE(table->LookupObject<rex::system::XEvent>(0xFFFFFFFF));
}

TEST_CASE("Notification masks versions FIFO and selective dequeue preserve wait state", "[runtime][notify]") {
  Fixture f;
  auto listener = f.New<rex::system::XNotifyListener>();
  // Use category 1 to avoid category 0's synthetic startup notifications.
  listener->Initialize(2, 1);
  constexpr uint32_t first = (1u << 25) | 7;
  constexpr uint32_t second = (1u << 25) | (1u << 16) | 8;
  listener->EnqueueNotification(7, 99);  // wrong category
  listener->EnqueueNotification((1u << 25) | (2u << 16) | 9, 99);  // too new
  CHECK(Poll(*listener) == X_STATUS_TIMEOUT);
  listener->EnqueueNotification(first, 10);
  listener->EnqueueNotification(second, 20);
  listener->EnqueueNotification(first, 30);
  CHECK(Poll(*listener) == X_STATUS_SUCCESS);
  uint32_t id = 0, data = 0;
  CHECK_FALSE(listener->DequeueNotification(123, &data));
  REQUIRE(listener->DequeueNotification(second, &data));
  CHECK(data == 20);
  CHECK(Poll(*listener) == X_STATUS_SUCCESS);
  REQUIRE(listener->DequeueNotification(&id, &data));
  CHECK(id == first);
  CHECK(data == 10);
  REQUIRE(listener->DequeueNotification(&id, &data));
  CHECK(data == 30);
  CHECK(Poll(*listener) == X_STATUS_TIMEOUT);
  CHECK_FALSE(listener->DequeueNotification(&id, &data));
  f.kernel()->UnregisterNotifyListener(listener.get());
}

TEST_CASE("Static enumerators deliver bounded batches and stop at exhaustion", "[runtime][enumerator]") {
  Fixture f;
  auto enumerator = rex::system::object_ref<rex::system::XStaticUntypedEnumerator>(
      new rex::system::XStaticUntypedEnumerator(f.kernel(), 2, sizeof(uint32_t)));
  REQUIRE(enumerator->Initialize(0, 1, 2, 3, 0) == X_STATUS_SUCCESS);
  for (uint32_t item : {11u, 22u, 33u}) {
    std::memcpy(enumerator->AppendItem(), &item, sizeof(item));
  }
  CHECK(enumerator->item_count() == 3);
  std::array<uint32_t, 3> output{0, 0, 0xDEADBEEF};
  uint32_t written = 0;
  REQUIRE(enumerator->WriteItems(0, reinterpret_cast<uint8_t*>(output.data()), &written) == X_ERROR_SUCCESS);
  CHECK(written == 2);
  CHECK(output[0] == 11);
  CHECK(output[1] == 22);
  CHECK(output[2] == 0xDEADBEEF);
  REQUIRE(enumerator->WriteItems(0, reinterpret_cast<uint8_t*>(output.data()), &written) == X_ERROR_SUCCESS);
  CHECK(written == 1);
  CHECK(output[0] == 33);
  CHECK(enumerator->WriteItems(0, reinterpret_cast<uint8_t*>(output.data()), &written) == X_ERROR_NO_MORE_FILES);
}

TEST_CASE("Guest heaps align reserve commit protect and release allocations", "[runtime][memory]") {
  Fixture f;
  auto memory = f.runtime.memory();
  auto heap = memory->LookupHeapByType(false, 0x1000);
  REQUIRE(heap);
  const auto free_pages = heap->unreserved_page_count();
  uint32_t address = 0;
  REQUIRE(heap->Alloc(0x2000, 0x10000, rex::memory::kMemoryAllocationReserve,
                      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address));
  CHECK(address % 0x10000 == 0);
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(address, &info));
  CHECK(info.state == rex::memory::kMemoryAllocationReserve);
  CHECK(info.allocation_size == 0x2000);
  CHECK_FALSE(heap->AllocFixed(address, 0x2000, 0x1000, rex::memory::kMemoryAllocationReserve,
                               rex::memory::kMemoryProtectRead));
  REQUIRE(heap->AllocFixed(address, 0x2000, 0x1000, rex::memory::kMemoryAllocationCommit,
                           rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  auto bytes = memory->TranslateVirtual<uint8_t*>(address);
  bytes[0] = 0x42;
  bytes[0x1FFF] = 0x24;
  CHECK(bytes[0] == 0x42);
  CHECK(bytes[0x1FFF] == 0x24);
  REQUIRE(heap->Protect(address, 0x2000, rex::memory::kMemoryProtectRead));
  uint32_t protect = 0;
  REQUIRE(heap->QueryProtect(address, &protect));
  CHECK(protect == rex::memory::kMemoryProtectRead);
  REQUIRE(heap->Decommit(address, 0x2000));
  REQUIRE(heap->Release(address));
  CHECK(heap->unreserved_page_count() == free_pages);
}

TEST_CASE("Alertable waits return USER_APC when an APC is queued; non-alertable waits time out", "[runtime][apc]") {
  Fixture f;
  RunOnHost(f, [&] {
    auto* self = rex::system::XThread::GetCurrentThread();
    auto event = f.New<rex::system::XEvent>();
    event->Initialize(true, false);
    uint64_t timeout = static_cast<uint64_t>(-10 * 10000ll);  // 10 ms

    // Non-alertable: a queued APC must not interrupt the wait.
    self->EnqueueApc(0, 0, 0, 0);
    CHECK(event->Wait(3, 1, 0, &timeout) == X_STATUS_TIMEOUT);

    // Alertable: the pending APC wakes the wait immediately.
    CHECK(event->Wait(3, 1, 1, &timeout) == X_STATUS_USER_APC);

    // DeliverAPCs drains the user queue; with nothing pending the wait times out again.
    self->DeliverAPCs();
    CHECK(event->Wait(3, 1, 1, &timeout) == X_STATUS_TIMEOUT);
  });
}

TEST_CASE("Alertable delay is interrupted by an APC", "[runtime][apc]") {
  Fixture f;
  RunOnHost(f, [&] {
    auto* self = rex::system::XThread::GetCurrentThread();
    uint64_t interval = static_cast<uint64_t>(-10 * 10000ll);
    self->EnqueueApc(0, 0, 0, 0);
    CHECK(self->Delay(1, 1, interval) == X_STATUS_USER_APC);
    self->DeliverAPCs();
    CHECK(self->Delay(1, 1, interval) == X_STATUS_SUCCESS);
  });
}
