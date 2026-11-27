/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/nui.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

extern std::atomic<int> xam_dialogs_shown_;

u32 XamNuiGetDeviceStatus_entry(ppc_ptr_t<X_NUI_DEVICE_STATUS> status_ptr) {
  if (!status_ptr) return X_E_INVALIDARG;
  status_ptr.Zero();
  status_ptr->status_bits = 0;  // Not connected.
  return 0xC0050006;       // No Kinect (Xenia's value); r3 used to keep the struct pointer.
}

u32 XamShowNuiTroubleshooterUI_entry(u32 unk1, u32 unk2, u32 unk3) {
  // unk1 is 0xFF - possibly user index?
  // unk2, unk3 appear to always be zero.

  if (REXCVAR_GET(headless)) {
    return 0;
  }
  // TODO(tomc): Implement imgui stuff
  // const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  // ui::Window* display_window = emulator->display_window();
  // ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  // if (display_window && imgui_drawer) {
  //  rex::thread::Fence fence;
  //  if (display_window->app_context().CallInUIThreadSynchronous([&]() {
  //        rex::ui::ImGuiDialog::ShowMessageBox(
  //            imgui_drawer, "NUI Troubleshooter",
  //            "The game has indicated there is a problem with NUI (Kinect).")
  //            ->Then(&fence);
  //      })) {
  //    ++xam_dialogs_shown_;
  //    fence.Wait();
  //    --xam_dialogs_shown_;
  //  }
  //}

  return 0;
}

// Identifies the Kinect identity session; any non-zero id works with no sensor attached.
u64 XamNuiIdentityGetSessionId_entry() {
  return 0xDEADF00DDEADF00Dull;
}

// No Kinect controller is initialized. These paths follow xenia-canary 2da2519
// xam_nui.cc and the zero-initialized NUIController state (BSD-3-Clause).
u32 XamNuiIsDeviceReady_entry() { return 0; }
u32 XamNuiIsChatMicEnabled_entry() { return 0; }
u32 XamNuiHudIsEnabled_entry() { return 0; }
u32 XamNuiHudGetInitializeFlags_entry() { return 0; }
u32 XamNuiCameraGetTiltControllerType_entry() { return 0; }
u64 XamNuiHudGetEngagedTrackingID_entry() { return 0; }
u64 XamNuiSkeletonGetBestSkeletonIndex_entry(i32) { return ~u64{0}; }
u32 XamNuiHudSetEngagedTrackingID_entry(u32 id) {
  return id ? X_E_FAIL : X_ERROR_SUCCESS;
}
u32 XamNuiCameraSetFlags_entry(u64, u32) { return X_E_DEVICE_NOT_CONNECTED; }
void XamNuiHudGetVersions_entry(mapped_u64 xam_version, mapped_u64 xtl_version) {
  if (xam_version) *xam_version = 0;
  if (xtl_version) *xtl_version = 0;
}
u32 XamNuiGetDepthCalibration_entry(ppc_ptr_t<X_NUI_DEPTH_CALIBRATION> data) {
  if (!data) return X_E_INVALIDARG;
  data.Zero();
  return 0xD000000F;  // STATUS_NO_SUCH_FILE | FACILITY_NT_BIT, as in Canary.
}
void XamNuiPlayerEngagementUpdate_entry(u32, u32,
    ppc_ptr_t<X_NUI_PLAYER_ENGAGEMENT_UPDATE> data) {
  if (data) data.Zero();
}

// Canary's tilt callback is retained, but no sensor exists to dispatch it.
static std::atomic<u32> tilt_callback{0};
u32 XamNuiCameraTiltSetCallback_entry(u32 callback) {
  tilt_callback.store(callback, std::memory_order_relaxed);
  return X_ERROR_SUCCESS;
}
u32 XamNuiCameraTiltGetStatus_entry(mapped_void) {
  // Canary returns E_FAIL for both invalid records and an unavailable sensor.
  return X_E_FAIL;
}
u32 XamNuiIdentityAbort_entry(u32) { return X_E_FAIL; }
u32 XamNuiIdentityEnrollForSignIn_entry(u32, u64, u64, u32) { return X_E_FAIL; }
u32 XamIsNuiAutomationEnabled_entry(u32, u32) { return X_ERROR_SUCCESS; }
u32 XamIsNatalPlaybackEnabled_entry(u32, u32) { return X_ERROR_SUCCESS; }

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamNuiGetDeviceStatus, rex::kernel::xam::XamNuiGetDeviceStatus_entry)
REX_EXPORT(__imp__XamShowNuiTroubleshooterUI, rex::kernel::xam::XamShowNuiTroubleshooterUI_entry)

REX_EXPORT_STUB(__imp__XamEnableNatalPlayback);
REX_EXPORT_STUB(__imp__XamEnableNuiAutomation);
REX_EXPORT_STUB(__imp__XamKinectGetHardwareType);
REX_EXPORT_STUB(__imp__XamNatalDeviceAudioCalibrate);
REX_EXPORT_STUB(__imp__XamNuiCameraAdjustTilt);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationAutoTilt);
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraElevationGetAngle, 0x80004005);  // X_E_FAIL: no Kinect (callers test blt)
REX_EXPORT_STUB(__imp__XamNuiCameraElevationReverseAutoTilt);
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraElevationSetAngle, 0x80004005);  // X_E_FAIL: no Kinect (callers test blt)
REX_EXPORT_STUB(__imp__XamNuiCameraElevationSetCallback);
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraElevationStopMovement, 0x80004005);  // X_E_FAIL: no Kinect (callers test blt)
REX_EXPORT(__imp__XamNuiCameraGetTiltControllerType, rex::kernel::xam::XamNuiCameraGetTiltControllerType_entry)
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraRememberFloor, 0x80004005);  // X_E_FAIL: no Kinect (callers test blt)
REX_EXPORT(__imp__XamNuiCameraSetFlags, rex::kernel::xam::XamNuiCameraSetFlags_entry)
REX_EXPORT(__imp__XamNuiCameraTiltGetStatus, rex::kernel::xam::XamNuiCameraTiltGetStatus_entry)
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraTiltReportStatus, 0x80004005);  // X_E_FAIL: no Kinect
REX_EXPORT(__imp__XamNuiCameraTiltSetCallback, rex::kernel::xam::XamNuiCameraTiltSetCallback_entry)
REX_EXPORT_STUB(__imp__XamNuiEnableChatMic);
REX_EXPORT_STUB(__imp__XamNuiGetCameraIntrinsics);
REX_EXPORT(__imp__XamNuiGetDepthCalibration, rex::kernel::xam::XamNuiGetDepthCalibration_entry)
REX_EXPORT_STUB_RETURN(__imp__XamNuiGetDeviceSerialNumber, 0x80004005);  // X_E_FAIL: no Kinect (callers test blt)
REX_EXPORT_STUB(__imp__XamNuiGetFanRate);
REX_EXPORT_STUB(__imp__XamNuiGetLoadedDepthCalibration);
REX_EXPORT_STUB(__imp__XamNuiGetSupportString);
REX_EXPORT_STUB(__imp__XamNuiGetSystemGestureControl);
REX_EXPORT_STUB(__imp__XamNuiGetTrueColorInfo);
REX_EXPORT_STUB(__imp__XamNuiHudEnableInputFilter);
REX_EXPORT_STUB(__imp__XamNuiHudGetEngagedEnrollmentIndex);
REX_EXPORT(__imp__XamNuiHudGetEngagedTrackingID, rex::kernel::xam::XamNuiHudGetEngagedTrackingID_entry)
REX_EXPORT(__imp__XamNuiHudGetInitializeFlags, rex::kernel::xam::XamNuiHudGetInitializeFlags_entry)
REX_EXPORT(__imp__XamNuiHudGetVersions, rex::kernel::xam::XamNuiHudGetVersions_entry)
REX_EXPORT_STUB(__imp__XamNuiHudInterpretFrame);
REX_EXPORT(__imp__XamNuiHudIsEnabled, rex::kernel::xam::XamNuiHudIsEnabled_entry)
REX_EXPORT(__imp__XamNuiHudSetEngagedTrackingID, rex::kernel::xam::XamNuiHudSetEngagedTrackingID_entry)
REX_EXPORT(__imp__XamNuiIdentityGetSessionId, rex::kernel::xam::XamNuiIdentityGetSessionId_entry)
REX_EXPORT(__imp__XamNuiIdentityAbort, rex::kernel::xam::XamNuiIdentityAbort_entry)
REX_EXPORT(__imp__XamNuiIdentityEnrollForSignIn, rex::kernel::xam::XamNuiIdentityEnrollForSignIn_entry)
REX_EXPORT_STUB(__imp__XamNuiIdentityGetColorTexture);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetEnrollmentInfo);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetQualityFlags);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetQualityFlagsMessage);
REX_EXPORT_STUB(__imp__XamNuiIdentityIdentifyWithBiometric);
REX_EXPORT_STUB(__imp__XamNuiIdentityUnenroll);
REX_EXPORT(__imp__XamNuiIsChatMicEnabled, rex::kernel::xam::XamNuiIsChatMicEnabled_entry)
REX_EXPORT(__imp__XamNuiIsDeviceReady, rex::kernel::xam::XamNuiIsDeviceReady_entry)
REX_EXPORT_STUB(__imp__XamNuiNatalCameraUpdateComplete);
REX_EXPORT_STUB(__imp__XamNuiNatalCameraUpdateStarting);
REX_EXPORT(__imp__XamNuiPlayerEngagementUpdate, rex::kernel::xam::XamNuiPlayerEngagementUpdate_entry)
REX_EXPORT_STUB(__imp__XamNuiSetForceDeviceOff);
REX_EXPORT(__imp__XamNuiSkeletonGetBestSkeletonIndex, rex::kernel::xam::XamNuiSkeletonGetBestSkeletonIndex_entry)
REX_EXPORT_STUB(__imp__XamNuiSkeletonScoreUpdate);
REX_EXPORT_STUB(__imp__XamNuiStoreDepthCalibration);

REX_EXPORT(__imp__XamIsNuiAutomationEnabled, rex::kernel::xam::XamIsNuiAutomationEnabled_entry)

REX_EXPORT(__imp__XamIsNatalPlaybackEnabled, rex::kernel::xam::XamIsNatalPlaybackEnabled_entry)
