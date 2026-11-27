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

#include <rex/logging.h>
#include <rex/hook.h>
#include "../quiet_return.h"
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

extern std::atomic<int> xam_dialogs_shown_;

struct X_NUI_DEVICE_STATUS {
  rex::be<uint32_t> unk0;
  rex::be<uint32_t> unk1;
  rex::be<uint32_t> unk2;
  rex::be<uint32_t> status;
  rex::be<uint32_t> unk4;
  rex::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

u32 XamNuiGetDeviceStatus_entry(ppc_ptr_t<X_NUI_DEVICE_STATUS> status_ptr) {
  status_ptr.Zero();
  status_ptr->status = 0;  // Not connected.
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
u32 XamNuiIdentityGetSessionId_entry() {
  return 0xDEADF00D;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamNuiGetDeviceStatus, rex::kernel::xam::XamNuiGetDeviceStatus_entry)
REX_EXPORT(__imp__XamShowNuiTroubleshooterUI, rex::kernel::xam::XamShowNuiTroubleshooterUI_entry)

REX_EXPORT_QUIET_RETURN(__imp__XamEnableNatalPlayback, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamEnableNuiAutomation, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamKinectGetHardwareType, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNatalDeviceAudioCalibrate, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraAdjustTilt, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraElevationAutoTilt, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraElevationGetAngle, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraElevationReverseAutoTilt, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraElevationSetAngle, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraElevationSetCallback, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraElevationStopMovement, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraGetTiltControllerType, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraRememberFloor, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraSetFlags, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraTiltGetStatus, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiCameraTiltReportStatus, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiCameraTiltSetCallback, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiEnableChatMic, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetCameraIntrinsics, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetDepthCalibration, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_STUB_RETURN(__imp__XamNuiGetDeviceSerialNumber, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetFanRate, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetLoadedDepthCalibration, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetSupportString, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetSystemGestureControl, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiGetTrueColorInfo, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudEnableInputFilter, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudGetEngagedEnrollmentIndex, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudGetEngagedTrackingID, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudGetInitializeFlags, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudGetVersions, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudInterpretFrame, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudIsEnabled, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiHudSetEngagedTrackingID, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT(__imp__XamNuiIdentityGetSessionId, rex::kernel::xam::XamNuiIdentityGetSessionId_entry)
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityAbort, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityEnrollForSignIn, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityGetColorTexture, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityGetEnrollmentInfo, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityGetQualityFlags, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityGetQualityFlagsMessage, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityIdentifyWithBiometric, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIdentityUnenroll, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIsChatMicEnabled, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiIsDeviceReady, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiNatalCameraUpdateComplete, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiNatalCameraUpdateStarting, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiPlayerEngagementUpdate, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiSetForceDeviceOff, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiSkeletonGetBestSkeletonIndex, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiSkeletonScoreUpdate, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNuiStoreDepthCalibration, 0);  // no reference; deliberate family default, behaviour unknown
