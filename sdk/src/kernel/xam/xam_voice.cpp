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

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include "../quiet_return.h"
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {

u32 XamVoiceIsActiveProcess_entry() {
  // Returning 0 here will short-circuit a bunch of voice stuff.
  return 0;
}

u32 XamVoiceCreate_entry(u32 unk1,  // 0
                         u32 unk2,  // 0xF
                         mapped_u32 out_voice_ptr) {
  // Null out the ptr.
  out_voice_ptr.Zero();
  return X_ERROR_ACCESS_DENIED;
}

u32 XamVoiceClose_entry(mapped_void voice_ptr) {
  return 0;
}

u32 XamVoiceHeadsetPresent_entry(mapped_void voice_ptr) {
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamVoiceIsActiveProcess, rex::kernel::xam::XamVoiceIsActiveProcess_entry)
REX_EXPORT(__imp__XamVoiceCreate, rex::kernel::xam::XamVoiceCreate_entry)
REX_EXPORT(__imp__XamVoiceClose, rex::kernel::xam::XamVoiceClose_entry)
REX_EXPORT(__imp__XamVoiceHeadsetPresent, rex::kernel::xam::XamVoiceHeadsetPresent_entry)

REX_EXPORT_STUB_RETURN(__imp__XamMuteSound, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceDisableMicArray, 0x8007048f);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetBatteryStatus, 0x8007048f);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetDirectionalData, 0x8007048f);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetMicArrayAudio, 0x8007048f);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetMicArrayAudioEx, 0x8007048F);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetMicArrayFilenameDesc, 0x8007048f);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetMicArrayStatus, 0x8007048F);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceGetMicArrayUnderrunStatus, 0x8007048F);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceMuteMicArray, 0x8007048f);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceRecordUserPrivileges, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceSetAudioCaptureRoutine, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamVoiceSetMicArrayBeamAngle, 0x8007048f);
REX_EXPORT_QUIET_PASSTHROUGH(__imp__XamVoiceSetMicArrayIdleUsers)  // no reference implementation; success, no log
REX_EXPORT_STUB_RETURN(__imp__XamVoiceSubmitPacket, 0);
