/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee; 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xbdm/private.h>
#include <rex/logging.h>
#include <rex/hook.h>

#include "debug_services.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
#include <rex/memory/utils.h>
#include <rex/system/xthread.h>

namespace rex::kernel::xbdm {
using namespace rex::system;

u32 DmAllocatePool_entry(u32 bytes) {
  return REX_KERNEL_MEMORY()->SystemHeapAlloc(bytes);
}

u32 DmFreePool_entry(mapped_void allocation) {
  if (!allocation) return X_E_INVALIDARG;
  REX_KERNEL_MEMORY()->SystemHeapFree(allocation.guest_address());
  return kDmSuccess;
}

u32 DmGetXboxName_entry(mapped_void name, mapped_u32 capacity) {
  if (!name || !capacity) return X_E_INVALIDARG;
  // Same fallback console name as the credited Xenia debug monitor.
  constexpr char console_name[] = "Xbox360Name";
  if (*capacity < sizeof(console_name)) return kDmBufferTooSmall;
  std::memcpy(name.host_address(), console_name, sizeof(console_name));
  return kDmSuccess;
}

u32 DmGetConsoleType_entry(mapped_u32 type) {
  if (!type) return X_E_INVALIDARG;
  *type = ~0u;  // Retail; this runtime does not expose devkit hardware.
  return kDmSuccess;
}

namespace {
bool Accessible(mapped_void pointer, u32 bytes, bool writable) {
  const u32 address = pointer.guest_address();
  if (!pointer || !address || !bytes || bytes - 1 > UINT32_MAX - address) return false;
  auto heap = REX_KERNEL_MEMORY()->LookupHeap(address);
  if (!heap) return false;
  // QueryRangeAccess takes an inclusive last address.
  const auto access = heap->QueryRangeAccess(address, address + bytes - 1);
  return writable ? access == memory::PageAccess::kReadWrite
                  : access != memory::PageAccess::kNoAccess;
}

u32 CopyDebugMemory(mapped_void source, u32 bytes, mapped_void destination, mapped_u32 copied) {
  if (copied) *copied = 0;
  if (!source || !destination || !bytes) return X_E_INVALIDARG;
  // Both sides must belong to mapped guest memory. Validate before touching it,
  // including address overflow and pages straddling the end of a heap.
  if (!Accessible(source, bytes, false) || !Accessible(destination, bytes, true))
    return kDmFailure;
  std::memmove(destination.host_address(), source.host_address(), bytes);
  if (copied) *copied = bytes;
  return kDmSuccess;
}
}

u32 DmGetMemory_entry(mapped_void source, u32 bytes, mapped_void destination, mapped_u32 copied) {
  return CopyDebugMemory(source, bytes, destination, copied);
}

u32 DmSetMemory_entry(mapped_void destination, u32 bytes, mapped_void source, mapped_u32 copied) {
  return CopyDebugMemory(source, bytes, destination, copied);
}

u32 DmGetThreadList_entry(mapped_u32 output, mapped_u32 capacity) {
  if (!output || !capacity) return X_E_INVALIDARG;
  // Snapshot pinned objects, then filter through the active thread registry.
  // Object tables can contain duplicate handles and retained exited threads.
  std::vector<u32> ids;
  for (const auto& thread : REX_KERNEL_OBJECTS()->GetObjectsByType<XThread>()) {
    const auto id = thread->thread_id();
    if (REX_KERNEL_STATE()->GetThreadByID(id)) ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  const u32 available = *capacity;
  if (ids.size() > available) return kDmBufferTooSmall;
  // Only write the entries actually returned; callers own any remaining space.
  for (size_t i = 0; i < ids.size(); ++i) output[i] = ids[i];
  *capacity = u32(ids.size());
  return kDmSuccess;
}

u32 DmResumeThread_entry(u32 id) {
  auto thread = REX_KERNEL_STATE()->GetThreadByID(id);
  if (!thread || XFAILED(thread->Resume())) return kDmFailure;
  return kDmSuccess;
}

u32 DmSuspendThread_entry(u32 id) {
  auto thread = REX_KERNEL_STATE()->GetThreadByID(id);
  if (!thread || XFAILED(thread->Suspend())) return kDmFailure;
  return kDmSuccess;
}
}

REX_EXPORT(__imp__DmAllocatePool, rex::kernel::xbdm::DmAllocatePool_entry)
REX_EXPORT_STUB_RETURN(__imp__DmAllocatePoolWithTag, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmCloseCounters, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCloseLoadedModules, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmCloseModuleSections_, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCloseNotificationSession, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmClosePerformanceCounter, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmContinueThread, 0x0);
REX_EXPORT(__imp__DmFreePool, rex::kernel::xbdm::DmFreePool_entry)
REX_EXPORT(__imp__DmGetMemory, rex::kernel::xbdm::DmGetMemory_entry)
REX_EXPORT_STUB_RETURN(__imp__DmGetModuleLongName, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmGetProcAddress, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmGetThreadContext, 0x80004005);
REX_EXPORT(__imp__DmGetThreadList, rex::kernel::xbdm::DmGetThreadList_entry)
REX_EXPORT_STUB_RETURN(__imp__DmGetXbeInfo, 0x80004005);
REX_EXPORT(__imp__DmGetXboxName, rex::kernel::xbdm::DmGetXboxName_entry)
REX_EXPORT_STUB_RETURN(__imp__DmGo, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmHaltThread, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmIsDebuggerPresent, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmIsThreadStopped, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmLoadExtension, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmNotify, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmOpenNotificationSession, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmOpenPerformanceCounter, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmQueryPerformanceCounterHandle, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmReboot, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterCommandProcessor, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterNotificationProcessor, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterPerformanceCounter, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRemoveBreakpoint, 0x0);
REX_EXPORT(__imp__DmResumeThread, rex::kernel::xbdm::DmResumeThread_entry)
REX_EXPORT_STUB_RETURN(__imp__DmSendNotificationString, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetBreakpoint, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetDataBreakpoint, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetInitialBreakpoint, 0x0);
REX_EXPORT(__imp__DmSetMemory, rex::kernel::xbdm::DmSetMemory_entry)
REX_EXPORT_STUB_RETURN(__imp__DmSetThreadContext, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetTitle, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetXboxName, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStop, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStopOn, 0x0);
REX_EXPORT(__imp__DmSuspendThread, rex::kernel::xbdm::DmSuspendThread_entry)
REX_EXPORT_STUB_RETURN(__imp__DmThreadUserData, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmUnloadExtension, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmWalkLoadedModules, 0x82DA0104);
REX_EXPORT_STUB_RETURN(__imp__DmWalkModuleSections, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmWalkPerformanceCounters, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCloseCounters_0, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmIsBreakpoint, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCloseCounters_1, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetUserAccess, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetUserAccess, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmWalkUserList, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAddUser, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmEnableSecurity, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmIsSecurityEnabled, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRemoveUser, 0x0);
REX_EXPORT_STUB_RETURN(__imp____CAP_Start_Profiling, 0);
REX_EXPORT_STUB_RETURN(__imp____CAP_End_Profiling, 0);
REX_EXPORT_STUB_RETURN(__imp____CAP_Enter_Function, 0);
REX_EXPORT_STUB_RETURN(__imp____CAP_Exit_Function, 0);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterCommandProcessorEx, 0);
REX_EXPORT_STUB_RETURN(__imp__DmStartProfiling, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStopProfiling, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmQueryMemoryStatistics, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmEnableStackTrace, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmQueryAllocationTypeName, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterAllocationType, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmInsertAllocationEntry, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRemoveAllocationEntry, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetTitleEx, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCaptureStackBackTrace, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCrashDump, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmIsFastCAPEnabled, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetFileAccessCount, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetUtilityDriveInfo, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetThreadInfoEx, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetProfilingOptions, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmQuerySystemSettings, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSaveSystemSettings, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmpGetPgoModuleHandleForBaseAddress, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmpOnPgoModuleLoad, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmPgoStartDataCollection, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPgoStopDataCollection, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPgoSaveSnapshot, 0x0);
REX_EXPORT_STUB_RETURN(__imp__IrtClientAbort, 0);
REX_EXPORT_STUB_RETURN(__imp__IrtPogoInit, 0);
REX_EXPORT_STUB_RETURN(__imp__IrtSetStaticInfo, 0);
REX_EXPORT_STUB_RETURN(__imp__IrtAutoSweepW, 0);
REX_EXPORT_STUB_RETURN(__imp__IrtAutoSweepA, 0);
REX_EXPORT_STUB_RETURN(__imp__DmGetDumpMode, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetDumpMode, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetDumpSettings, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetDumpSettings, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetEventDeferFlags, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetEventDeferFlags, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmWalkCommittedMemory, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCloseCounters_2, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmRebootEx, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmMountFdfxVolume, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCapGetFileHeader, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCapFreeFileHeader, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceStartRecording, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceStartRecordingFunction, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceSetBufferSize, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceStopRecording, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceSaveBuffer, 0x0);
REX_EXPORT(__imp__DmGetConsoleType, rex::kernel::xbdm::DmGetConsoleType_entry)
REX_EXPORT_STUB_RETURN(__imp__DmMapDevkitDrive, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmWalkLoadedModulesEx, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmGetXexHeaderField, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetMouseChanges, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmFindPdbSignature, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetProfilingStatus, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmOpticalDiscLogStart, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmOpticalDiscLogStop, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStartSamplingProfiler, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStopSamplingProfiler, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetSamplingProfilerInfo, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStartFileEventCapture, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmStopFileEventCapture, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmSetFileEventMarker, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmMarkPseudoCreateBegin, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmMarkPseudoCreateEnd, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmMarkPseudoEventBegin, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmMarkPseudoEventEnd, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmMarkFileEventWorkerThreadBegin, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmMarkFileEventWorkerThreadEnd, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetSystemInfo, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAbortProfiling, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetCaptureStart, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetCaptureStop, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmQueryTitleMemoryStatistics, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationGetInputProcess, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationBindController, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationUnbindController, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationConnectController, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationDisconnectController, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationSetGamepadState, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationQueueGamepadState, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationClearGamepadQueue, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationQueryGamepadQueue, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationGetUserDefaultProfile, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmAutomationSetUserDefaultProfile, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceIsRecording, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmLoadDebuggerExtension, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__DmUnloadDebuggerExtension, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmCreateSystemThread, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__PEPLELookup, 0);
REX_EXPORT_STUB_RETURN(__imp__PEPLELookupCompact, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLELookup, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLELookupCompact, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLELookupCompactMDS, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLEFilteredLookup, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLEFilteredLookupCompact, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLEFilteredLookupCompactMDS, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLETemplatedLookup, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLETemplatedLookupCompact, 0);
REX_EXPORT_STUB_RETURN(__imp__PVPLETemplatedLookupCompactMDS, 0);
REX_EXPORT_STUB_RETURN(__imp__DmpPgoCounterOverflow, 0x0);
REX_EXPORT_STUB_RETURN(__imp__UpdateMDSProbeState, 0);
REX_EXPORT_STUB_RETURN(__imp__DmGetHttpRegistration, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmTraceSetIOThread, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCInstallAndStart, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCStopAndReport, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCInstallSetup, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCUnInstallSetup, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCResetCounters, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCSetTriggerProcessor, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCStart, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCStop, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCGetCounter, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCGetCounters, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCGetCounterName, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCDumpCounters, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCDumpCountersVerbose, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCGetCounterCostEstimate, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCGetCounterSource, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmPMCComputeFrequency, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetAdditionalTitleMemorySetting, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetDebugMemorySize, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetConsoleDebugMemoryStatus, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimSetLinkStatusHidden, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimInsertQueue, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimRemoveQueue, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimRemoveAllQueues, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimModifyQueueSettings, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetConsoleFeatures, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimGetQueueStats, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimGetQueueSettings, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimGetNumQueues, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimInsertIpv4Redirect, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimRemoveIpv4Redirect, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmNetSimGetNumIpv4Redirects, 0x0);
REX_EXPORT_STUB_RETURN(__imp__XLFAllocate, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFFree, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueAdd, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueAllocated, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueCreate, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueGetEntryCount, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueIsEmpty, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueRemove, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFQueueUnsafeDump, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackAllocated, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackCreate, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackGetEntryCount, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackIsEmpty, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackPop, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackPush, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStackUnsafeDump, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueAdd, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueAllocated, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueCreate, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueGetEntryCount, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueIsEmpty, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueRemoveFirst, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueRemove, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPriorityQueueUnsafeDump, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableAdd, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableAllocated, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableCreate, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableGetEntryCount, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableIsEmpty, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableRemoveFirst, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableRemove, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFHashTableUnsafeDump, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolAcquireLock, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolAllocated, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolCreate, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolCreateLock, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolDestroy, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolDestroyLock, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolIncreaseEvents, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolIncreaseLocks, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolInitializeLock, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolNumberOfEvents, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolNumberOfLocks, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFPoolReleaseLock, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStartLog, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFStartUserLog, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFInitializeLog, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFInitializeUserLog, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFEndLog, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFLogPrint, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFLogPrintV, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFLogBuffer, 0);
REX_EXPORT_STUB_RETURN(__imp__XLFLogMessageStats, 0);
REX_EXPORT_STUB_RETURN(__imp__XLockFreeGetErrorHandler, 0);
REX_EXPORT_STUB_RETURN(__imp__XLockFreeSetErrorHandler, 0);
REX_EXPORT_STUB_RETURN(__imp__DmExecuteThreadRPC, 0x0);
REX_EXPORT_STUB_RETURN(__imp__DmGetDebuggerConnection, 0x0);
