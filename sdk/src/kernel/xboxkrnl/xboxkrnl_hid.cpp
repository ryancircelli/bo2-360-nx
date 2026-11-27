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

u32 HidReadKeys_entry(u32 unk1, u32 unk2, u32 unk3) {
  /* TODO(gibbed):
   * Games check for the following errors:
   *   0xC000009D - translated to 0x48F  - ERROR_DEVICE_NOT_CONNECTED
   *   0x103      - translated to 0x10D2 - ERROR_EMPTY
   * Other errors appear to be ignored?
   *
   * unk1 is 0
   * unk2 is a pointer to &unk3[2], possibly a 6-byte buffer
   * unk3 is a pointer to a 20-byte buffer
   */
  return 0xC000009D;
}

u32 HidGetCapabilities_entry(u32 unk1, u32 caps_ptr) {
  // HidGetCapabilities - ordinal 0x01EA
  // Returns capabilities for HID device (keyboard/mouse)
  // Not supported in rexglue - return unsuccessful
  return X_STATUS_UNSUCCESSFUL;
}

u32 HidGetLastInputTime_entry(mapped_u32 time_ptr) {
  // HidGetLastInputTime - ordinal 0x01F1
  // Returns the last time any HID input was received
  if (time_ptr) {
    *time_ptr = 0;
  }
  return X_STATUS_SUCCESS;
}

u32 HidReadMouseChanges_entry(u32 unk1, u32 unk2) {
  // HidReadMouseChanges - ordinal 0x0273
  // Reads mouse input changes - not supported in rexglue
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HidReadKeys, rex::kernel::xboxkrnl::HidReadKeys_entry)
REX_EXPORT(__imp__HidGetCapabilities, rex::kernel::xboxkrnl::HidGetCapabilities_entry)
REX_EXPORT(__imp__HidGetLastInputTime, rex::kernel::xboxkrnl::HidGetLastInputTime_entry)
REX_EXPORT(__imp__HidReadMouseChanges, rex::kernel::xboxkrnl::HidReadMouseChanges_entry)

// XInputd stubs
REX_EXPORT_STUB_RETURN(__imp__XInputdGetCapabilities, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdReadState, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdWriteState, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdNotify, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdRawState, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdGetDeviceStats, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdResetDevice, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetRingOfLight, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetRFPowerMode, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetRadioFrequency, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdPassThroughRFCommand, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdPowerDownDevice, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdReadTextKeystroke, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdSendStayAliveRequest, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFGetDeviceInfo, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFSetEffect, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFUpdateEffect, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFEffectOperation, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFDeviceControl, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFSetDeviceGain, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFCancelIo, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdFFSetRumble, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdGetLastTextInputTime, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetTextMessengerIndicator, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetTextDeviceKeyLocks, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdGetTextDeviceKeyLocks, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdControl, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetWifiChannel, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdGetDevicePid, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdGetFailedConnectionOrBind, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetFailedConnectionOrBindCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__XInputdSetMinMaxAuthDelay, 0);

// Drv stubs
REX_EXPORT_STUB_RETURN(__imp__DrvSetSysReqCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvSetUserBindingCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvSetContentStorageCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvSetAutobind, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvGetContentStorageNotification, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvXenonButtonPressed, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvBindToUser, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvSetDeviceConfigChangeCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvDeviceConfigChange, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvSetMicArrayStartCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__DrvSetAudioLatencyCallback, 0);
