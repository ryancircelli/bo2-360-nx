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
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {

u32 XamPartyGetUserList_entry(u32 player_count, mapped_u32 party_list) {
  // Some titles read the count even when no online party exists.
  if (party_list) *party_list = 0;
  // 5345085D wants specifically this code to skip loading party data.
  // This code is not documented in NT_STATUS code list
  return 0x807D0003;
}

u32 XamPartySendGameInvites_entry(u32 r3, u32 r4, u32 r5) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamPartySetCustomData_entry(u32 r3, u32 r4, u32 r5) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamPartyGetBandwidth_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamPartyGetUserList, rex::kernel::xam::XamPartyGetUserList_entry)
REX_EXPORT(__imp__XamPartySendGameInvites, rex::kernel::xam::XamPartySendGameInvites_entry)
REX_EXPORT(__imp__XamPartySetCustomData, rex::kernel::xam::XamPartySetCustomData_entry)
REX_EXPORT(__imp__XamPartyGetBandwidth, rex::kernel::xam::XamPartyGetBandwidth_entry)

REX_EXPORT_STUB_RETURN(__imp__XamPartyAddLocalUsers, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyAutomationInprocCall, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartyCreate, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetInfo, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetInfoEx, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetNetworkCounters, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartyGetRoutingTable, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartyIsCoordinator, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartyJoin, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyJoinEx, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyKickUser, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyLeave, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyOverrideNatType, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartyRemoveLocalUsers, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartySendInvite, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartySendInviteDeprecated, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetConnectivityGraph, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetJoinable, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetTestDelay, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XamPartySetTestFlags, 0x0);

namespace rex::kernel::xam {
u32 XamPartyGetAccessLevel_entry(mapped_u32 out) {
  if (out) *out = 0;
  return 0;
}
u32 XamPartyGetFormation_entry(mapped_u32 out) {
  if (out) *out = 0;
  return 0;
}
u32 XamPartyGetJoinable_entry(mapped_u32 out) {
  if (out) *out = 0;
  return 0;
}
u32 XamPartyGetState_entry(mapped_u32 out) {
  if (out) *out = 0;
  return 0;
}
u32 XamPartyGetUserListInternal_entry(u32 c, mapped_u32 l) { return XamPartyGetUserList_entry(c, l); }
}  // namespace rex::kernel::xam
REX_EXPORT(__imp__XamPartyGetAccessLevel, rex::kernel::xam::XamPartyGetAccessLevel_entry)
REX_EXPORT(__imp__XamPartyGetFormation, rex::kernel::xam::XamPartyGetFormation_entry)
REX_EXPORT(__imp__XamPartyGetJoinable, rex::kernel::xam::XamPartyGetJoinable_entry)
REX_EXPORT(__imp__XamPartyGetState, rex::kernel::xam::XamPartyGetState_entry)
REX_EXPORT(__imp__XamPartyGetUserListInternal, rex::kernel::xam::XamPartyGetUserListInternal_entry)
