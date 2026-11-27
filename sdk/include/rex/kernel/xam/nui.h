// Kinect guest ABI. Adapted from xenia-canary 2da2519 (BSD-3-Clause).
#pragma once
#include <rex/types.h>

namespace rex::kernel::xam {
struct X_NUI_DEVICE_STATUS {
  be_u32 camera_status;
  be_u32 tilt_status;
  be_u32 mic_status;
  be_u32 status_bits;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 0x10);
struct X_NUI_DEPTH_CALIBRATION { uint8_t data[0x30]; };
struct X_NUI_PLAYER_ENGAGEMENT_UPDATE { uint8_t data[0x1C]; };

u32 XamNuiGetDeviceStatus_entry(ppc_ptr_t<X_NUI_DEVICE_STATUS>);
u64 XamNuiIdentityGetSessionId_entry();
u32 XamNuiGetDepthCalibration_entry(ppc_ptr_t<X_NUI_DEPTH_CALIBRATION>);
void XamNuiPlayerEngagementUpdate_entry(u32, u32, ppc_ptr_t<X_NUI_PLAYER_ENGAGEMENT_UPDATE>);
void XamNuiHudGetVersions_entry(mapped_u64, mapped_u64);
u32 XamNuiHudSetEngagedTrackingID_entry(u32);
u64 XamNuiSkeletonGetBestSkeletonIndex_entry(i32);
u32 XamNuiCameraSetFlags_entry(u64, u32);
}  // namespace rex::kernel::xam
