// Own executable: an unhandled raise ends in rex::debug::Break(), whose SIGTRAP handler swallows only
// the first trap in a process.
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

extern "C" void __imp__RtlRaiseStatus(PPCContext&, uint8_t*);

TEST_CASE("RtlRaiseStatus raises through RtlRaiseException and returns to the caller", "[kernel][rtl]") {
  rex::Runtime runtime{std::filesystem::current_path()};
  rex::RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  PPCContext ctx{};
  ctx.r3.u64 = 0xC0000017;  // STATUS_NO_MEMORY: not one RtlRaiseException handles itself
  __imp__RtlRaiseStatus(ctx, runtime.virtual_membase());
  // Reaching here means the record was built and the unhandled-code path logged and trapped once.
  SUCCEED();
}
