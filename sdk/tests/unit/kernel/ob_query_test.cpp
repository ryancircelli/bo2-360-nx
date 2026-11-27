#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#define EXPORT(name) extern "C" void __imp__##name(PPCContext&, uint8_t*)
EXPORT(NtCreateSemaphore);
EXPORT(NtReleaseSemaphore);
EXPORT(NtQuerySemaphore);
EXPORT(NtCreateMutant);
EXPORT(NtReleaseMutant);
EXPORT(NtWaitForSingleObjectEx);
EXPORT(NtQueryMutant);
EXPORT(NtCreateTimer);
EXPORT(NtSetTimerEx);
EXPORT(NtCancelTimer);
EXPORT(NtQueryTimer);
EXPORT(NtCreateIoCompletion);
EXPORT(NtSetIoCompletion);
EXPORT(NtQueryIoCompletion);
#undef EXPORT

namespace {
using namespace rex;
using namespace rex::system;

uint8_t* g_base = nullptr;

template <typename... A>
uint32_t Call(void (*fn)(PPCContext&, uint8_t*), A... args) {
  PPCContext ctx{};
  uint64_t v[] = {uint64_t(args)...};
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (size_t i = 0; i < sizeof...(A); ++i) {
    regs[i]->u64 = v[i];
  }
  fn(ctx, g_base);
  return ctx.r3.u32;
}

uint32_t Be32(uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, g_base + addr, 4);
  return __builtin_bswap32(v);
}
int64_t Be64(uint32_t addr) {
  uint64_t v;
  std::memcpy(&v, g_base + addr, 8);
  return int64_t(__builtin_bswap64(v));
}
void PutBe64(uint32_t addr, int64_t value) {
  uint64_t v = __builtin_bswap64(uint64_t(value));
  std::memcpy(g_base + addr, &v, 8);
}
}  // namespace

TEST_CASE("NtQuery{Semaphore,Mutant,Timer,IoCompletion} report live object state", "[kernel][ob]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  g_base = runtime.virtual_membase();
  uint32_t handle = runtime.memory()->SystemHeapAlloc(0x40);
  uint32_t info = handle + 0x10;
  uint32_t due = handle + 0x30;

  auto* thread = new XHostThread(runtime.kernel_state(), 128 * 1024, 0, [&]() -> int {
    // Semaphore: count and maximum.
    CHECK(Call(__imp__NtCreateSemaphore, handle, 0, 2, 5) == X_STATUS_SUCCESS);
    uint32_t sem = Be32(handle);
    CHECK(Call(__imp__NtQuerySemaphore, sem, info) == X_STATUS_SUCCESS);
    CHECK(Be32(info) == 2);
    CHECK(Be32(info + 4) == 5);
    CHECK(Call(__imp__NtReleaseSemaphore, sem, 1, 0) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQuerySemaphore, sem, info) == X_STATUS_SUCCESS);
    CHECK(Be32(info) == 3);

    // Mutant: NT signal state (1 - depth) and ownership through a recursive acquire.
    CHECK(Call(__imp__NtCreateMutant, handle, 0, 1) == X_STATUS_SUCCESS);
    uint32_t mutant = Be32(handle);
    CHECK(Call(__imp__NtQueryMutant, mutant, info) == X_STATUS_SUCCESS);
    CHECK(int32_t(Be32(info)) == 0);
    CHECK(g_base[info + 4] == 1);
    CHECK(g_base[info + 5] == 0);
    CHECK(Call(__imp__NtWaitForSingleObjectEx, mutant, 0, 0, 0) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQueryMutant, mutant, info) == X_STATUS_SUCCESS);
    CHECK(int32_t(Be32(info)) == -1);
    CHECK(Call(__imp__NtReleaseMutant, mutant, 0) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQueryMutant, mutant, info) == X_STATUS_SUCCESS);
    CHECK(int32_t(Be32(info)) == 0);
    CHECK(g_base[info + 4] == 1);  // still owned after the inner release
    CHECK(Call(__imp__NtReleaseMutant, mutant, 0) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQueryMutant, mutant, info) == X_STATUS_SUCCESS);
    CHECK(int32_t(Be32(info)) == 1);
    CHECK(g_base[info + 4] == 0);

    // Timer: remaining time while set, signal state after expiry.
    CHECK(Call(__imp__NtCreateTimer, handle, 0, 0) == X_STATUS_SUCCESS);
    uint32_t timer = Be32(handle);
    CHECK(Call(__imp__NtQueryTimer, timer, info) == X_STATUS_SUCCESS);
    CHECK(Be64(info) == 0);
    CHECK(g_base[info + 8] == 0);
    PutBe64(due, -100000000ll);  // 10 s relative
    CHECK(Call(__imp__NtSetTimerEx, timer, due, 0, 1, 0, 0, 0, 0) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQueryTimer, timer, info) == X_STATUS_SUCCESS);
    CHECK(Be64(info) > 90000000ll);
    CHECK(Be64(info) <= 100000000ll);
    CHECK(g_base[info + 8] == 0);
    CHECK(Call(__imp__NtCancelTimer, timer, 0) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQueryTimer, timer, info) == X_STATUS_SUCCESS);
    CHECK(Be64(info) == 0);
    PutBe64(due, -10000ll);  // 1 ms
    CHECK(Call(__imp__NtSetTimerEx, timer, due, 0, 1, 0, 0, 0, 0) == X_STATUS_SUCCESS);
    bool signaled = false;
    for (int i = 0; i < 200 && !signaled; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      CHECK(Call(__imp__NtQueryTimer, timer, info) == X_STATUS_SUCCESS);
      signaled = g_base[info + 8] == 1;
    }
    CHECK(signaled);
    CHECK(Be64(info) == 0);

    // IoCompletion: queued packet count.
    CHECK(Call(__imp__NtCreateIoCompletion, handle, 0, 0, 0) == X_STATUS_SUCCESS);
    uint32_t port = Be32(handle);
    CHECK(Call(__imp__NtQueryIoCompletion, port, info) == X_STATUS_SUCCESS);
    CHECK(Be32(info) == 0);
    CHECK(Call(__imp__NtSetIoCompletion, port, 1, 2, 0, 4) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtSetIoCompletion, port, 1, 2, 0, 4) == X_STATUS_SUCCESS);
    CHECK(Call(__imp__NtQueryIoCompletion, port, info) == X_STATUS_SUCCESS);
    CHECK(Be32(info) == 2);

    // Wrong object type or bad handle.
    CHECK(Call(__imp__NtQueryMutant, sem, info) == X_STATUS_INVALID_HANDLE);
    CHECK(Call(__imp__NtQueryTimer, 0x12345678u, info) == X_STATUS_INVALID_HANDLE);
    return 0;
  });
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  uint64_t join = static_cast<uint64_t>(-100000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &join) == X_STATUS_SUCCESS);
  thread->Release();
}
