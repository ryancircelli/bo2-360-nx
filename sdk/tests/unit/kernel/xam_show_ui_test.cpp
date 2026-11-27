// No-UI XamShow*UI answers: each family fails a bad request before touching the kernel.
#include <catch2/catch_test_macros.hpp>
#include <rex/ppc/context.h>
#include <rex/system/xtypes.h>

using namespace rex;

namespace rex::kernel::xam {
u32 XamShowOfflinePanelUI_entry(u32);
u32 XamShowPartyUI_entry(u32, u32);
u32 XamShowCommunitySessionsUI_entry(u32, u32);
u32 XamShowCustomMessageComposeUI_entry(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32,
                                        u32, u32, u32, u32);
u32 XamShowCustomPlayerListUI_entry(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
}  // namespace rex::kernel::xam
using namespace rex::kernel::xam;

TEST_CASE("Guide panels refuse a user slot that cannot exist", "[kernel][ui]") {
  CHECK(XamShowOfflinePanelUI_entry(4) == X_ERROR_NO_SUCH_USER);
  CHECK(XamShowPartyUI_entry(4, 0) == X_ERROR_NO_SUCH_USER);
  CHECK(XamShowCommunitySessionsUI_entry(4, 0) == X_ERROR_NO_SUCH_USER);
  // Refused synchronously even with an overlapped: nothing is queued for completion.
  CHECK(XamShowCustomMessageComposeUI_entry(4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1000) ==
        X_ERROR_NO_SUCH_USER);
  CHECK(XamShowCustomPlayerListUI_entry(4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1000) ==
        X_ERROR_NO_SUCH_USER);
}

// Guest calls through the export hooks, as Forza Horizon makes them.
extern "C" void __imp__XamShowNuiMessageBoxUI(PPCContext&, uint8_t*);
extern "C" void __imp__XamShowNuiDeviceSelectorUI(PPCContext&, uint8_t*);
extern "C" void __imp__XamShowNuiSigninUI(PPCContext&, uint8_t*);
TEST_CASE("Kinect dialogs are refused with no Kinect HUD", "[kernel][ui][nui]") {
  PPCContext ctx{};
  uint8_t base[16]{};
  __imp__XamShowNuiMessageBoxUI(ctx, base);
  CHECK(ctx.r3.u64 == X_ERROR_ACCESS_DENIED);
  __imp__XamShowNuiDeviceSelectorUI(ctx, base);
  CHECK(ctx.r3.u64 == X_ERROR_ACCESS_DENIED);
  __imp__XamShowNuiSigninUI(ctx, base);
  CHECK(ctx.r3.u64 == X_ERROR_ACCESS_DENIED);
}
