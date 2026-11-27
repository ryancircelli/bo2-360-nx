// Per-family check of the deliberate stub return values (stub-backfill-krnl2).
#include <catch2/catch_test_macros.hpp>
#include <rex/ppc/function.h>
#include <rex/ppc/context.h>

namespace {
uint64_t Call(const char* name) {
  auto* f = rex::ppc::FindPPCFuncByName(name);
  REQUIRE(f != nullptr);
  PPCContext ctx{};
  ctx.r3.u64 = 0xDEADBEEF;
  static uint8_t mem[16];
  f(ctx, mem);
  return ctx.r3.u64;
}
}  // namespace

TEST_CASE("stub families return their deliberate value") {
  CHECK(Call("__imp__XeKeysSetKey") == 0xC0000022);      // crypto: never writes keys
  CHECK(Call("__imp__XeKeysGetStatus") == 0);            // crypto: status read
  CHECK(Call("__imp__NicXmit") == 0xC0000001);           // NIC: no link to send on
  CHECK(Call("__imp__NicGetLinkState") == 1);            // NIC: link up
  CHECK(Call("__imp__UsbdOpenEndpoint") == 0xC000009D);  // USB: no devices
  CHECK(Call("__imp__WifiBeginAuthentication") == 0xC0000001);
  CHECK(Call("__imp__HalGpioControl") == 0xC0000001);    // HAL: unsupported
  CHECK(Call("__imp__VdSetHDCPOption") == 0);            // video: accept and ignore
  CHECK(Call("__imp__XInputdReadState") == 0xC000009D);  // pad driver: absent
  CHECK(Call("__imp__KeGetPRVRegister") == 0x00710800);
}
