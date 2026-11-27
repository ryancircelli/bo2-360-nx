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

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

u32 XUsbcamCreate_entry(u32 buffer,
                        u32 buffer_size,  // 0x4B000 640x480?
                        mapped_void unk3_ptr) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}

u32 XUsbcamGetState_entry() {
  // 0 = not connected.
  return 0;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XUsbcamCreate, rex::kernel::xboxkrnl::XUsbcamCreate_entry)
REX_EXPORT(__imp__XUsbcamGetState, rex::kernel::xboxkrnl::XUsbcamGetState_entry)

// no camera attached (ERROR_DEVICE_NOT_CONNECTED); call-site spec, untested in a game
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetCaptureMode, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamGetConfig, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamReadFrame, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSnapshot, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetView, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamGetView, 0x48F);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XUsbcamReset, 0x48F);
