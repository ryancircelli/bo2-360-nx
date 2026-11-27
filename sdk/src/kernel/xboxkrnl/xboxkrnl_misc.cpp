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
#include "../quiet_return.h"
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void KeEnableFpuExceptions_entry(u32 enabled) {
  // TODO(benvanik): can we do anything about exceptions?
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__KeEnableFpuExceptions, rex::kernel::xboxkrnl::KeEnableFpuExceptions_entry)

REX_EXPORT_STUB_RETURN(__imp__ExSetBetaFeaturesEnabled, 0);
REX_EXPORT_STUB_RETURN(__imp__ExIsBetaFeatureEnabled, 0);
REX_EXPORT_STUB_RETURN(__imp__AniBlockOnAnimation, 0);
REX_EXPORT_STUB_RETURN(__imp__AniTerminateAnimation, 0);
REX_EXPORT_STUB_RETURN(__imp__AniSetLogo, 0);
REX_EXPORT_STUB_RETURN(__imp__AniStartBootAnimation, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxConsumerDisableEventType, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxConsumerEnableEventType, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxConsumerProcessLogs, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxConsumerRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxConsumerUnregister, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxProducerLog, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxProducerLogV, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxProducerRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxProducerUnregister, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxConsumerFlushBuffers, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxProducerLogXwpp, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxProducerLogXwppV, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxBufferRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__EtxBufferUnregister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeEnablePPUPerformanceMonitor, 0);
REX_EXPORT_STUB_RETURN(__imp__KeEnterUserMode, 0);
REX_EXPORT_STUB_RETURN(__imp__KeLeaveUserMode, 0);
REX_EXPORT_STUB_RETURN(__imp__KeCreateUserMode, 0);
REX_EXPORT_STUB_RETURN(__imp__KeDeleteUserMode, 0);
REX_EXPORT_STUB_RETURN(__imp__KeEnablePFMInterrupt, 0);
REX_EXPORT_STUB_RETURN(__imp__KeDisablePFMInterrupt, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetProfilerISR, 0);
REX_EXPORT_STUB_RETURN(__imp__KeGetVidInfo, 0);
REX_EXPORT_STUB_RETURN(__imp__KeExecuteOnProtectedStack, 0);
REX_EXPORT_STUB_RETURN(__imp__EmaExecute, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__ExRegisterThreadNotification, 0);
REX_EXPORT_STUB_RETURN(__imp__ExTerminateTitleProcess, 0);
REX_EXPORT_STUB_RETURN(__imp__ExReadModifyWriteXConfigSettingUlong, 0);
REX_EXPORT_STUB_RETURN(__imp__ExRegisterXConfigNotification, 0);
REX_EXPORT_STUB_RETURN(__imp__ExCancelAlarm, 0);
REX_EXPORT_STUB_RETURN(__imp__ExInitializeAlarm, 0);
REX_EXPORT_STUB_RETURN(__imp__ExSetAlarm, 0);
REX_EXPORT_STUB_RETURN(__imp__KeBlowFuses, 0xC0000022);
REX_EXPORT_STUB_RETURN(__imp__KeGetPMWRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeGetPRVRegister, 0x00710800);
REX_EXPORT_STUB_RETURN(__imp__KeGetSocRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetPMWRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetPowerMode, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetPRVRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetSocRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetSpecialPurposeRegister, 0);
REX_EXPORT_STUB_RETURN(__imp__KeCallAndBlockOnDpcRoutine, 0);
REX_EXPORT_STUB_RETURN(__imp__KeCallAndWaitForDpcRoutine, 0);
REX_EXPORT_STUB_RETURN(__imp__KeSetPageRelocationCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__KeRegisterSwapNotification, 0);

REX_EXPORT_STUB_RETURN(__imp__DetroitDeviceRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__IptvGetAesCtrTransform, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__IptvGetSessionKeyHash, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__IptvSetBoundaryKey, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__IptvSetSessionKey, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__IptvVerifyOmac1Signature, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__McaDeviceRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MicDeviceRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdBeginTransaction, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdCancelTransaction, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdEndTransaction, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdGetCurrentDevices, 0);
REX_EXPORT_STUB_RETURN(__imp__MtpdReadData, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdReadEvent, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdResetDevice, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdSendData, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__MtpdVerifyProximity, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__NicAttach, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__NicDetach, 0);
REX_EXPORT_STUB_RETURN(__imp__NicFlushXmitQueue, 0);
REX_EXPORT_STUB_RETURN(__imp__NicGetLinkState, 1);
REX_EXPORT_STUB_RETURN(__imp__NicGetOpt, 0);
REX_EXPORT_STUB_RETURN(__imp__NicGetStats, 0);
REX_EXPORT_STUB_RETURN(__imp__NicRegisterDevice, 0);
REX_EXPORT_STUB_RETURN(__imp__NicSetOpt, 0);
REX_EXPORT_STUB_RETURN(__imp__NicSetUnicastAddress, 0);
REX_EXPORT_STUB_RETURN(__imp__NicShutdown, 0);
REX_EXPORT_STUB_RETURN(__imp__NicUnregisterDevice, 0);
REX_EXPORT_STUB_RETURN(__imp__NicUpdateMcastMembership, 0);
REX_EXPORT_STUB_RETURN(__imp__NicXmit, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__NomnilGetExtension, 0);
REX_EXPORT_STUB_RETURN(__imp__NomnilSetLed, 0);
REX_EXPORT_STUB_RETURN(__imp__NomnilStartCloseDevice, 0);
REX_EXPORT_STUB_RETURN(__imp__NullCableRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__PsCamDeviceRequest, 0xC000000E /* STATUS_NO_SUCH_DEVICE: no camera */);
REX_EXPORT_STUB_RETURN(__imp__RmcDeviceRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__TidDeviceRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__TitleDeviceAuthRequest, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdAddDeviceComplete, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdCallAndBlockOnDpcRoutine, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdCancelAsyncTransfer, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdCancelTimer, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdEnableDisableRootHubPort, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetDeviceDescriptor, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetDeviceRootPortType, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetDeviceSpeed, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetDeviceTopology, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetEndpointDescriptor, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetNatalHardwareVersion, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetNatalHub, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetPortDeviceNode, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetRequiredDrivers, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdGetRootHubDeviceNode, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdIsDeviceAuthenticated, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdNatalHubRegisterNotificationCallback, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdOpenDefaultEndpoint, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdOpenEndpoint, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdQueueAsyncTransfer, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdQueueCloseDefaultEndpoint, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdQueueCloseEndpoint, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdQueueIsochTransfer, 0xC000009D);
REX_EXPORT_STUB_RETURN(__imp__UsbdRegisterDriverObject, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdRemoveDeviceComplete, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdResetDevice, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdResetEndpoint, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdSetTimer, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdTitleDriverResetAllUnrecognizedPorts, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdTitleDriverSetUnrecognizedPort, 0);
REX_EXPORT_STUB_RETURN(__imp__UsbdUnregisterDriverObject, 0);
REX_EXPORT_STUB_RETURN(__imp__VeSetHandlers, 0);
REX_EXPORT_STUB_RETURN(__imp__VgcHandler_SetHandlers, 0);
REX_EXPORT_STUB_RETURN(__imp__VvcHandlerCancelTransfers, 0);
REX_EXPORT_STUB_RETURN(__imp__VvcHandlerRetrieveVoiceExtension, 0);
REX_EXPORT_STUB_RETURN(__imp__WifiBeginAuthentication, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiCalculateRegulatoryDomain, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiChannelToFrequency, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiCheckCounterMeasures, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiChooseAuthenCipherSetFromBSSID, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiCompleteAuthentication, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiDeduceNetworkType, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiGetAssociationIE, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiOnMICError, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiPrepareAuthenticationContext, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiRecvEAPOLPacket, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__WifiSelectAdHocChannel, 0xC0000001);
REX_EXPORT_STUB_RETURN(__imp__XVoicedActivate, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedClose, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedGetBatteryStatus, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedGetDirectionalData, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedHeadsetPresent, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedIsActiveProcess, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedSendVPort, 0);
REX_EXPORT_STUB_RETURN(__imp__XVoicedSubmitPacket, 0);

// stub-backfill-krnl2
namespace rex::kernel::xboxkrnl {
// PVR (SPR 287) is the Xenon processor version; every other SPR reads 0.
u64 KeGetSpecialPurposeRegister_entry(u32 spr) { return spr == 287 ? 0x00710800 : 0; }
void ExFreeDebugPool_entry(u32 addr) {
  if (addr) REX_KERNEL_MEMORY()->SystemHeapFree(addr);
}
}  // namespace rex::kernel::xboxkrnl
REX_EXPORT(__imp__KeGetSpecialPurposeRegister, rex::kernel::xboxkrnl::KeGetSpecialPurposeRegister_entry)
REX_EXPORT(__imp__ExFreeDebugPool, rex::kernel::xboxkrnl::ExFreeDebugPool_entry)
