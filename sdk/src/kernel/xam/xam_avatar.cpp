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

u32 XamAvatarInitialize_entry(u32 unk1,                  // 1, 4, etc
                              u32 unk2,                  // 0 or 1
                              u32 processor_number,      // for thread creation?
                              mapped_u32 function_ptrs,  // 20b, 5 pointers
                              mapped_void unk5,          // ptr in data segment
                              u32 unk6                   // flags - 0x00300000, 0x30, etc
) {
  // Negative to fail. Game should immediately call XamAvatarShutdown.
  return ~0u;
}

void XamAvatarShutdown_entry() {
  // No-op.
}

// xenia-canary 2da2519 xam_avatar.cc (BSD-3-Clause). No avatar
// resources exist while initialization is disabled, so both required sizes are zero.
u32 XamAvatarGetAssetsResultSize_entry(u32, mapped_u32 result_size, mapped_u32 gpu_size) {
  if (!result_size || !gpu_size) return X_E_INVALIDARG;
  *result_size = 0;
  *gpu_size = 0;
  return X_ERROR_SUCCESS;
}
u32 XamAvatarGetInstrumentation_entry(u64, mapped_u32 output) {
  if (output) *output = 0;
  return 1;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamAvatarInitialize, rex::kernel::xam::XamAvatarInitialize_entry)
REX_EXPORT(__imp__XamAvatarShutdown, rex::kernel::xam::XamAvatarShutdown_entry)

REX_EXPORT_STUB_RETURN(__imp__XamAvatarBeginEnumAssets, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarEndEnumAssets, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarEnumAssets, 0x80070012);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGenerateMipMaps, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetAssetBinary, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetAssetIcon, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetAssets, 0x80004005);
REX_EXPORT(__imp__XamAvatarGetAssetsResultSize, rex::kernel::xam::XamAvatarGetAssetsResultSize_entry)
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetInstalledAssetPackageDescription, 0x80004005);
REX_EXPORT(__imp__XamAvatarGetInstrumentation, rex::kernel::xam::XamAvatarGetInstrumentation_entry)
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetManifestLocalUser, 0x65B);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetManifestsByXuid, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetMetadataRandom, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetMetadataSignedOutProfile, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarGetMetadataSignedOutProfileCount, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarLoadAnimation, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarManifestGetBodyType, 1);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarReinstallAwardedAsset, 0);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarSetCustomAsset, 0);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarSetManifest, 0x65B);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarSetMocks, 0);
REX_EXPORT_STUB_RETURN(__imp__XamAvatarWearNow, 0x80004005);
