#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

extern "C" void __imp__KeIpiGenericCall(PPCContext&, uint8_t*);
extern "C" void __imp__XexPcToFileHeader(PPCContext&, uint8_t*);

namespace {
using namespace rex;
using namespace rex::system;

uint32_t g_seen_context = 0;

void GuestIpiRoutine(PPCContext& ctx, uint8_t*) {
  g_seen_context = ctx.r3.u32;
  ctx.r31.u64 = 0xBAD;  // a callee-clobbered register must not leak back to the caller
  ctx.r3.u64 = ctx.r3.u32 * 2 + 1;
}
}  // namespace

TEST_CASE("KeIpiGenericCall runs the routine once on the calling thread", "[kernel][ipi]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* dispatcher = runtime.function_dispatcher();
  REQUIRE(dispatcher->InitializeFunctionTable(0x82000000, 0x1000, 0x82000000, 0x10000, true));
  uint32_t routine = dispatcher->AllocateThunk(&GuestIpiRoutine, 0);
  REQUIRE(routine != 0);
  uint8_t* base = runtime.virtual_membase();

  auto* thread = new XHostThread(runtime.kernel_state(), 128 * 1024, 0, [&]() -> int {
    auto* guest = XThread::GetCurrentThread()->thread_state()->context();
    guest->r31.u64 = 0x1234;
    PPCContext ctx{};
    ctx.r3.u64 = routine;
    ctx.r4.u64 = 20;
    __imp__KeIpiGenericCall(ctx, base);
    CHECK(g_seen_context == 20);
    CHECK(ctx.r3.u32 == 41);
    CHECK(guest->r31.u64 == 0x1234);
    return 0;
  });
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  uint64_t join = static_cast<uint64_t>(-100000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &join) == X_STATUS_SUCCESS);
  thread->Release();
}

TEST_CASE("XexPcToFileHeader returns 0 when no title image contains the pc", "[kernel][xex]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  uint8_t* base = runtime.virtual_membase();
  uint32_t entry = runtime.memory()->SystemHeapAlloc(4);
  *reinterpret_cast<uint32_t*>(base + entry) = 0xAAAAAAAA;
  PPCContext ctx{};
  ctx.r3.u64 = 0x82001000;
  ctx.r4.u64 = entry;
  __imp__XexPcToFileHeader(ctx, base);
  CHECK(ctx.r3.u32 == 0);
  CHECK(*reinterpret_cast<uint32_t*>(base + entry) == 0xAAAAAAAA);
}
