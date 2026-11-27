#include <catch2/catch_test_macros.hpp>
#include <rex/kernel/xam/nui.h>
#include <rex/system/xtypes.h>
#include <algorithm>
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
