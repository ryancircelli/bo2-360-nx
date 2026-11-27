#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include <rex/ppc/function.h>
#include <rex/ppc/context.h>
#include "kernel/xam/net_socket.h"

using namespace rex;
using namespace rex::kernel::xam;

namespace {
template <class T> auto Map(T* v) { return ppc_ptr_t<T>::from_host(v); }
template <class T> mapped_void MapV(T* v) { return mapped_void::from_host(reinterpret_cast<void*>(v)); }

// Call a registered export by name with no arguments and return r3.
uint64_t CallStub(const char* name) {
  auto* fn = rex::ppc::FindPPCFuncByName(name);
  REQUIRE(fn != nullptr);
  PPCContext ctx{};
  ctx.r3.u64 = 0xDEADBEEF;  // junk the stub must overwrite
  fn(ctx, nullptr);
  return ctx.r3.u64;
}
}  // namespace

TEST_CASE("XNet key replace swaps only registered ids") {
  uint8_t a[8] = {1}, b[8] = {2}, k[16] = {};
  CHECK(NetDll_XNetReplaceKey_entry(1, MapV(a), MapV(b)) == 10022);
  NetDll_XNetRegisterKey_entry(1, MapV(a), MapV(k));
  CHECK(NetDll_XNetReplaceKey_entry(1, MapV(a), MapV(b)) == 0);
  CHECK(NetDll_XNetReplaceKey_entry(1, MapV(a), MapV(b)) == 10022);  // a is gone
  CHECK(NetDll_XNetUnregisterKey_entry(1, MapV(b)) == 0);
  CHECK(NetDll_XNetReplaceKey_entry(1, MapV(b), MapV(a)) == 10022);  // b was unregistered
}

TEST_CASE("XNet offline lookups fail with zeroed outputs") {
  be_u32 out{0x1234};
  CHECK(NetDll_XNetServerToInAddr_entry(1, 5, 6, Map(&out)) == 10050);
  CHECK(out == 0);
  uint8_t xnaddr[36];
  std::memset(xnaddr, 0xAA, sizeof(xnaddr));
  CHECK(NetDll_XNetInAddrToServer_entry(1, 5, MapV(xnaddr)) == 10050);
  CHECK(xnaddr[0] == 0);
  CHECK(xnaddr[35] == 0);
}

TEST_CASE("System link port defaults to 3074 and keeps a set value") {
  be_u16 port{0};
  CHECK(NetDll_XNetGetSystemLinkPort_entry(1, Map(&port)) == 0);
  CHECK(port == 0x0C02);
  NetDll_XNetSetSystemLinkPort_entry(1, 0x1234);
  CHECK(NetDll_XNetGetSystemLinkPort_entry(1, Map(&port)) == 0);
  CHECK(port == 0x1234);
}

TEST_CASE("Party / voice / xbdm / Upnp / XmlDownload stubs return deliberate values") {
  CHECK(CallStub("__imp__XamPartyCreate") == 0x80004005);
  CHECK(CallStub("__imp__XamPartySetJoinable") == 0);
  CHECK(CallStub("__imp__XamVoiceGetMicArrayStatus") == 0x8007048F);
  CHECK(CallStub("__imp__XamVoiceSubmitPacket") == 0);
  CHECK(CallStub("__imp__DmIsDebuggerPresent") == 0);
  CHECK(CallStub("__imp__DmOpenNotificationSession") == 0x80004005);
  CHECK(CallStub("__imp__NetDll_UpnpStartup") == 0);
  CHECK(CallStub("__imp__NetDll_UpnpSearchCreate") == 0xFFFFFFFF);
  CHECK(CallStub("__imp__NetDll_XmlDownloadStart") == 0x80004005);
  CHECK(CallStub("__imp__NetDll_XmlDownloadGetParseTime") == 0);
}
