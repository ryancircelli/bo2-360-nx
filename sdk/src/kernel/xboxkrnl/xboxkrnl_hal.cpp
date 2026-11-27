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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void HalReturnToFirmware_entry(u32 routine) {
  // void
  // IN FIRMWARE_REENTRY  Routine

  // Routine must be 1 'HalRebootRoutine'
  assert_true(routine == 1);

  // Leave the way XamLoaderTerminateTitle does: end the title's threads (this one included, so this
  // does not return); the app sees the main thread finish and takes its window-close hard exit.
  // exit(0) here ran static destructors under live guest, GPU and frame-dump threads, and
  // std::terminate'd on the joinable frame-dump std::thread.
  REXKRNL_ERROR("Game requested shutdown via HalReturnToFirmware");
  REX_KERNEL_STATE()->TerminateTitle();
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HalReturnToFirmware, rex::kernel::xboxkrnl::HalReturnToFirmware_entry)

REX_EXPORT_STUB_RETURN(__imp__HalGetCurrentAVPack, 0);
REX_EXPORT_STUB_RETURN(__imp__HalGpioControl, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__HalOpenCloseODDTray, 0);
REX_EXPORT_STUB_RETURN(__imp__HalReadWritePCISpace, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__HalRegisterPowerDownNotification, 0);
REX_EXPORT_STUB_RETURN(__imp__HalRegisterSMCNotification, 0);
REX_EXPORT_STUB_RETURN(__imp__HalSendSMCMessage, 0);
REX_EXPORT_STUB_RETURN(__imp__HalSetAudioEnable, 0);
REX_EXPORT_STUB_RETURN(__imp__HalIsExecutingPowerDownDpc, 0);
REX_EXPORT_STUB_RETURN(__imp__HalGetPowerUpCause, 0);
REX_EXPORT_STUB_RETURN(__imp__HalRegisterPowerDownCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__HalRegisterBackgroundModeTransitionCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__HalClampUnclampOutputDACs, 0);
REX_EXPORT_STUB_RETURN(__imp__HalPowerDownToBackgroundMode, 0);
REX_EXPORT_STUB_RETURN(__imp__HalNotifyAddRemoveBackgroundTask, 0);
REX_EXPORT_STUB_RETURN(__imp__HalCallBackgroundModeNotificationRoutines, 0);
REX_EXPORT_STUB_RETURN(__imp__HalGetMemoryInformation, 0);
REX_EXPORT_STUB_RETURN(__imp__HalNotifyBackgroundModeTransitionComplete, 0);
REX_EXPORT_STUB_RETURN(__imp__HalFinalizePowerLossRecovery, 0);
REX_EXPORT_STUB_RETURN(__imp__HalSetPowerLossRecovery, 0);
REX_EXPORT_STUB_RETURN(__imp__HalRegisterXamPowerDownCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__HalRegisterHdDvdRomNotification, 0);
REX_EXPORT_STUB_RETURN(__imp__HalGetNotedArgonErrors, 0);
REX_EXPORT_STUB_RETURN(__imp__HalReadArgonEeprom, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__HalWriteArgonEeprom, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__HalConfigureVeDevice, 0);
