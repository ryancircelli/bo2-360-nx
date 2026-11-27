#include <catch2/catch_test_macros.hpp>
#include <rex/kernel/xam/nui.h>
#include <rex/ppc/context.h>
#include <rex/system/xtypes.h>
#include <algorithm>
#include <iterator>
#include <cstring>

using namespace rex;
using namespace rex::kernel::xam;

TEST_CASE("Kinect status writes exactly the 16-byte guest structure", "[kernel][nui]") {
  struct { X_NUI_DEVICE_STATUS status; uint8_t guard[8]; } buffer;
  std::memset(&buffer, 0xA5, sizeof(buffer));
  CHECK(XamNuiGetDeviceStatus_entry(ppc_ptr_t<X_NUI_DEVICE_STATUS>::from_host(&buffer.status)) == 0xC0050006);
  CHECK(buffer.status.camera_status == 0);
  CHECK(buffer.status.tilt_status == 0);
  CHECK(buffer.status.mic_status == 0);
  CHECK(buffer.status.status_bits == 0);
  for (auto byte : buffer.guard) CHECK(byte == 0xA5);
  CHECK(XamNuiGetDeviceStatus_entry(nullptr) == X_E_INVALIDARG);
}
TEST_CASE("Absent Kinect clears output records without claiming a device", "[kernel][nui]") {
  X_NUI_DEPTH_CALIBRATION calibration;
  std::memset(&calibration, 0xA5, sizeof(calibration));
  CHECK(XamNuiGetDepthCalibration_entry(ppc_ptr_t<X_NUI_DEPTH_CALIBRATION>::from_host(&calibration)) == 0xD000000F);
  CHECK(std::all_of(std::begin(calibration.data), std::end(calibration.data), [](auto b) { return b == 0; }));
  CHECK(XamNuiGetDepthCalibration_entry(nullptr) == X_E_INVALIDARG);
  X_NUI_PLAYER_ENGAGEMENT_UPDATE engagement;
  std::memset(&engagement, 0xA5, sizeof(engagement));
  XamNuiPlayerEngagementUpdate_entry(0, 0, ppc_ptr_t<X_NUI_PLAYER_ENGAGEMENT_UPDATE>::from_host(&engagement));
  CHECK(std::all_of(std::begin(engagement.data), std::end(engagement.data), [](auto b) { return b == 0; }));
  be_u64 xam = 123, xtl = 456;
  XamNuiHudGetVersions_entry(mapped_u64::from_host(&xam), mapped_u64::from_host(&xtl));
  CHECK(xam == 0);
  CHECK(xtl == 0);
  XamNuiHudGetVersions_entry(nullptr, nullptr);
  CHECK(XamNuiHudSetEngagedTrackingID_entry(0) == X_ERROR_SUCCESS);
  CHECK(XamNuiHudSetEngagedTrackingID_entry(42) == X_E_FAIL);
  CHECK(XamNuiCameraSetFlags_entry(0, 0) == X_E_DEVICE_NOT_CONNECTED);
}
TEST_CASE("Kinect identity and skeleton sentinels retain all 64 bits", "[kernel][nui]") {
  CHECK(XamNuiIdentityGetSessionId_entry() == 0xDEADF00DDEADF00Dull);
  CHECK(XamNuiSkeletonGetBestSkeletonIndex_entry(0) == 0xFFFFFFFFFFFFFFFFull);
}

namespace rex::kernel::xam {
u32 XamNuiCameraTiltSetCallback_entry(u32);
u32 XamNuiCameraTiltGetStatus_entry(mapped_void);
u32 XamNuiIdentityAbort_entry(u32);
u32 XamNuiIdentityEnrollForSignIn_entry(u32, u64, u64, u32);
u32 XamShowGoldUpgradeUI_entry(u32, u32, u32);
u32 XamShowMultiplayerUpgradeUI_entry(u32);
u32 XamShowNuiGuideUI_entry(u32, u32);
u32 XamShowNuiHardwareRequiredUI_entry(u32);
u32 XamShowMarketplaceUI_entry(u32, u32, u64, u32, u32, u32);
}
TEST_CASE("Unavailable Kinect and subscription dialogs fail deliberately", "[kernel][nui][ui]") {
  CHECK(XamNuiCameraTiltSetCallback_entry(0x82001000) == X_ERROR_SUCCESS);
  CHECK(XamNuiCameraTiltSetCallback_entry(0) == X_ERROR_SUCCESS);
  CHECK(XamNuiCameraTiltGetStatus_entry(nullptr) == X_E_FAIL);
  CHECK(XamNuiIdentityAbort_entry(0) == X_E_FAIL);
  CHECK(XamNuiIdentityEnrollForSignIn_entry(0, 0, 0, 0) == X_E_FAIL);
  CHECK(XamShowGoldUpgradeUI_entry(0, 0, 0) == X_ERROR_FUNCTION_FAILED);
  CHECK(XamShowMultiplayerUpgradeUI_entry(0) == X_ERROR_FUNCTION_FAILED);
  CHECK(XamShowNuiGuideUI_entry(0, 0) == X_ERROR_ACCESS_DENIED);
  CHECK(XamShowNuiHardwareRequiredUI_entry(1) == X_ERROR_INVALID_PARAMETER);
  // Invalid requests must fail before accessing a kernel instance.
  CHECK(XamShowMarketplaceUI_entry(4, 0, 0, 0, 0, 0) == X_ERROR_INVALID_PARAMETER);
  CHECK(XamShowMarketplaceUI_entry(0, 25, 0, 0, 0, 0) == X_ERROR_INVALID_PARAMETER);
}

extern "C" void __imp__XamNuiIdentityGetSessionId(PPCContext&, uint8_t*);
extern "C" void __imp__XamNuiSkeletonGetBestSkeletonIndex(PPCContext&, uint8_t*);
TEST_CASE("Kinect export hooks retain the full guest return register", "[kernel][nui][abi]") {
  PPCContext context{};
  __imp__XamNuiIdentityGetSessionId(context, nullptr);
  CHECK(context.r3.u64 == 0xDEADF00DDEADF00Dull);
  context.r3.u64 = 0;
  __imp__XamNuiSkeletonGetBestSkeletonIndex(context, nullptr);
  CHECK(context.r3.u64 == 0xFFFFFFFFFFFFFFFFull);
}

// Stub-family contract: deliberate returns, never a junk value (spec round 2).
extern "C" void __imp__XamNavigate(PPCContext&, uint8_t*);
extern "C" void __imp__XamShowGamerCardUI(PPCContext&, uint8_t*);
extern "C" void __imp__XamNuiGetDeviceStatus(PPCContext&, uint8_t*);
extern "C" void __imp__XamInputGetDeviceStats(PPCContext&, uint8_t*);
TEST_CASE("Stub families return deliberate codes", "[kernel][stubs]") {
  PPCContext ctx{};
  uint8_t base[16]{};
  __imp__XamNavigate(ctx, base);
  CHECK(ctx.r3.u64 == 0x65B);
  __imp__XamInputGetDeviceStats(ctx, base);
  CHECK(ctx.r3.u64 == 0x48F);
  __imp__XamShowGamerCardUI(ctx, base);
  CHECK(ctx.r3.u64 == 0x4C7);
}

extern "C" void __imp__XGetVideoFlags(PPCContext&, uint8_t*);
extern "C" void __imp__XamAvatarEnumAssets(PPCContext&, uint8_t*);
extern "C" void __imp__XuiDefault_True(PPCContext&, uint8_t*);
extern "C" void __imp__XuiHandleIsValid(PPCContext&, uint8_t*);
extern "C" void __imp__XamInputGetKeystrokeHud(PPCContext&, uint8_t*);
TEST_CASE("Video, avatar, XUI, input stub families", "[kernel][stubs]") {
  PPCContext ctx{};
  uint8_t base[16]{};
  __imp__XGetVideoFlags(ctx, base);
  CHECK(ctx.r3.u64 == 0x0F);
  __imp__XamAvatarEnumAssets(ctx, base);
  CHECK(ctx.r3.u64 == 0x80070012);
  __imp__XuiDefault_True(ctx, base);
  CHECK(ctx.r3.u64 == 1);
  __imp__XuiHandleIsValid(ctx, base);
  CHECK(ctx.r3.u64 == 0);
  __imp__XamInputGetKeystrokeHud(ctx, base);
  CHECK(ctx.r3.u64 == 0x10D2);
}
