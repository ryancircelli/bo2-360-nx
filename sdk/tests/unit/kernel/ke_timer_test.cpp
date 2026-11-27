#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>

#define EXPORT(name) extern "C" void __imp__##name(PPCContext&, uint8_t*)
EXPORT(KeInitializeTimerEx);
EXPORT(KeInitializeDpc);
EXPORT(KeSetTimer);
EXPORT(KeSetTimerEx);
EXPORT(KeCancelTimer);
#undef EXPORT

namespace {
using namespace rex;
using namespace rex::system;

std::atomic<int> g_dpc_calls{0};
std::atomic<uint32_t> g_dpc_args[4];
std::atomic<uint32_t> g_dpc_irql{0};

void GuestDpc(PPCContext& ctx, uint8_t* base) {
  g_dpc_args[0] = ctx.r3.u32;
  g_dpc_args[1] = ctx.r4.u32;
  g_dpc_args[2] = ctx.r5.u32;
  g_dpc_args[3] = ctx.r6.u32;
  g_dpc_irql = reinterpret_cast<X_KPCR*>(base + ctx.r13.u32)->current_irql;
  ++g_dpc_calls;
}

bool WaitForCalls(int count) {
  for (int i = 0; i < 200 && g_dpc_calls < count; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_dpc_calls >= count;
}

template <typename F> void RunOnGuestThread(KernelState* kernel, F fn) {
  auto* thread = new XHostThread(kernel, 128 * 1024, 0, [&]() -> int {
    fn();
    return 0;
  });
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  uint64_t join = static_cast<uint64_t>(-100000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &join) == X_STATUS_SUCCESS);
  thread->Release();
}
}  // namespace

TEST_CASE("KeSetTimerEx fires the KDPC at DISPATCH_LEVEL and KeCancelTimer stops it",
          "[kernel][timer]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* dispatcher = runtime.function_dispatcher();
  REQUIRE(dispatcher->InitializeFunctionTable(0x82000000, 0x1000, 0x82000000, 0x10000, true));
  uint32_t routine = dispatcher->AllocateThunk(&GuestDpc, 0);
  REQUIRE(routine != 0);

  auto* memory = runtime.memory();
  uint32_t timer = memory->SystemHeapAlloc(sizeof(X_KTIMER) + sizeof(XDPC));
  uint32_t dpc = timer + sizeof(X_KTIMER);
  uint8_t* base = runtime.virtual_membase();
  g_dpc_calls = 0;

  RunOnGuestThread(runtime.kernel_state(), [&] {
    PPCContext ctx{};
    ctx.r3.u64 = timer;
    ctx.r4.u64 = 1;  // SynchronizationTimer
    ctx.r5.u64 = 0;
    __imp__KeInitializeTimerEx(ctx, base);
    ctx.r3.u64 = dpc;
    ctx.r4.u64 = routine;
    ctx.r5.u64 = 0x1234;
    __imp__KeInitializeDpc(ctx, base);

    // One-shot 1 ms relative: first set reports "was not inserted".
    ctx.r3.u64 = timer;
    ctx.r4.u64 = static_cast<uint64_t>(-10000ll);
    ctx.r5.u64 = dpc;
    __imp__KeSetTimer(ctx, base);
    CHECK(ctx.r3.u32 == 0);
    // Catch2 REQUIRE throws, which must not escape a guest thread.
    if (!WaitForCalls(1)) {
      FAIL_CHECK("timer DPC never ran");
      return;
    }
    CHECK(g_dpc_args[0] == dpc);
    CHECK(g_dpc_args[1] == 0x1234);
    CHECK((g_dpc_args[2] | g_dpc_args[3]) != 0);  // expiry time
    CHECK(g_dpc_irql == 2);                       // IRQL_DISPATCH
    // The timer object is waitable through the same guest KTIMER.
    auto native = XObject::GetNativeObject<XTimer>(runtime.kernel_state(),
                                                   memory->TranslateVirtual(timer));
    CHECK(native);
    CHECK_FALSE((native && native->inserted()));

    // Expired timer cancels as "not inserted".
    ctx.r3.u64 = timer;
    __imp__KeCancelTimer(ctx, base);
    CHECK(ctx.r3.u32 == 0);

    // Periodic 2 ms: re-set reports inserted, fires repeatedly, cancel reports inserted.
    ctx.r3.u64 = timer;
    ctx.r4.u64 = static_cast<uint64_t>(-20000ll);
    ctx.r5.u64 = 2;
    ctx.r6.u64 = dpc;
    __imp__KeSetTimerEx(ctx, base);
    CHECK(ctx.r3.u32 == 0);
    ctx.r3.u64 = timer;
    ctx.r4.u64 = static_cast<uint64_t>(-20000ll);
    ctx.r5.u64 = 2;
    ctx.r6.u64 = dpc;
    __imp__KeSetTimerEx(ctx, base);
    CHECK(ctx.r3.u32 == 1);
    CHECK(WaitForCalls(3));
    ctx.r3.u64 = timer;
    __imp__KeCancelTimer(ctx, base);
    CHECK(ctx.r3.u32 == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int after_cancel = g_dpc_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(g_dpc_calls == after_cancel);
  });
  memory->SystemHeapFree(timer);
}
