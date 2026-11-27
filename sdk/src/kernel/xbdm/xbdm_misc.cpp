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
REX_EXPORT_STUB(__imp__DmAllocatePoolWithTag);
REX_EXPORT_STUB(__imp__DmCloseCounters);
REX_EXPORT_STUB(__imp__DmCloseLoadedModules);
REX_EXPORT_STUB(__imp__DmCloseModuleSections_);
REX_EXPORT_STUB(__imp__DmCloseNotificationSession);
REX_EXPORT_STUB(__imp__DmClosePerformanceCounter);
REX_EXPORT_STUB(__imp__DmContinueThread);
REX_EXPORT(__imp__DmFreePool, rex::kernel::xbdm::DmFreePool_entry)
REX_EXPORT(__imp__DmGetMemory, rex::kernel::xbdm::DmGetMemory_entry)
REX_EXPORT_STUB(__imp__DmGetModuleLongName);
REX_EXPORT_STUB(__imp__DmGetProcAddress);
REX_EXPORT_STUB(__imp__DmGetThreadContext);
REX_EXPORT(__imp__DmGetThreadList, rex::kernel::xbdm::DmGetThreadList_entry)
REX_EXPORT_STUB(__imp__DmGetXbeInfo);
REX_EXPORT(__imp__DmGetXboxName, rex::kernel::xbdm::DmGetXboxName_entry)
REX_EXPORT_STUB(__imp__DmGo);
REX_EXPORT_STUB(__imp__DmHaltThread);
REX_EXPORT_STUB(__imp__DmIsDebuggerPresent);
REX_EXPORT_STUB(__imp__DmIsThreadStopped);
REX_EXPORT_STUB(__imp__DmLoadExtension);
REX_EXPORT_STUB(__imp__DmNotify);
REX_EXPORT_STUB(__imp__DmOpenNotificationSession);
REX_EXPORT_STUB(__imp__DmOpenPerformanceCounter);
REX_EXPORT_STUB(__imp__DmQueryPerformanceCounterHandle);
REX_EXPORT_STUB(__imp__DmReboot);
REX_EXPORT_STUB(__imp__DmRegisterCommandProcessor);
REX_EXPORT_STUB(__imp__DmRegisterNotificationProcessor);
REX_EXPORT_STUB(__imp__DmRegisterPerformanceCounter);
REX_EXPORT_STUB(__imp__DmRemoveBreakpoint);
REX_EXPORT(__imp__DmResumeThread, rex::kernel::xbdm::DmResumeThread_entry)
REX_EXPORT_STUB(__imp__DmSendNotificationString);
REX_EXPORT_STUB(__imp__DmSetBreakpoint);
REX_EXPORT_STUB(__imp__DmSetDataBreakpoint);
REX_EXPORT_STUB(__imp__DmSetInitialBreakpoint);
REX_EXPORT(__imp__DmSetMemory, rex::kernel::xbdm::DmSetMemory_entry)
REX_EXPORT_STUB(__imp__DmSetThreadContext);
REX_EXPORT_STUB(__imp__DmSetTitle);
REX_EXPORT_STUB(__imp__DmSetXboxName);
REX_EXPORT_STUB(__imp__DmStop);
REX_EXPORT_STUB(__imp__DmStopOn);
REX_EXPORT(__imp__DmSuspendThread, rex::kernel::xbdm::DmSuspendThread_entry)
REX_EXPORT_STUB(__imp__DmThreadUserData);
REX_EXPORT_STUB(__imp__DmUnloadExtension);
REX_EXPORT_STUB_RETURN(__imp__DmWalkLoadedModules, 0x82DA0104);
REX_EXPORT_STUB(__imp__DmWalkModuleSections);
REX_EXPORT_STUB(__imp__DmWalkPerformanceCounters);
REX_EXPORT_STUB(__imp__DmCloseCounters_0);
REX_EXPORT_STUB(__imp__DmIsBreakpoint);
REX_EXPORT_STUB(__imp__DmCloseCounters_1);
REX_EXPORT_STUB(__imp__DmSetUserAccess);
REX_EXPORT_STUB(__imp__DmGetUserAccess);
REX_EXPORT_STUB(__imp__DmWalkUserList);
REX_EXPORT_STUB(__imp__DmAddUser);
REX_EXPORT_STUB(__imp__DmEnableSecurity);
REX_EXPORT_STUB(__imp__DmIsSecurityEnabled);
REX_EXPORT_STUB(__imp__DmRemoveUser);
REX_EXPORT_STUB(__imp____CAP_Start_Profiling);
REX_EXPORT_STUB(__imp____CAP_End_Profiling);
REX_EXPORT_STUB(__imp____CAP_Enter_Function);
REX_EXPORT_STUB(__imp____CAP_Exit_Function);
REX_EXPORT_STUB_RETURN(__imp__DmRegisterCommandProcessorEx, 0);
REX_EXPORT_STUB(__imp__DmStartProfiling);
REX_EXPORT_STUB(__imp__DmStopProfiling);
REX_EXPORT_STUB(__imp__DmQueryMemoryStatistics);
REX_EXPORT_STUB(__imp__DmEnableStackTrace);
REX_EXPORT_STUB(__imp__DmQueryAllocationTypeName);
REX_EXPORT_STUB(__imp__DmRegisterAllocationType);
REX_EXPORT_STUB(__imp__DmInsertAllocationEntry);
REX_EXPORT_STUB(__imp__DmRemoveAllocationEntry);
REX_EXPORT_STUB(__imp__DmSetTitleEx);
REX_EXPORT_STUB(__imp__DmCaptureStackBackTrace);
REX_EXPORT_STUB(__imp__DmCrashDump);
REX_EXPORT_STUB(__imp__DmIsFastCAPEnabled);
REX_EXPORT_STUB(__imp__DmGetFileAccessCount);
REX_EXPORT_STUB(__imp__DmGetUtilityDriveInfo);
REX_EXPORT_STUB(__imp__DmGetThreadInfoEx);
REX_EXPORT_STUB(__imp__DmSetProfilingOptions);
REX_EXPORT_STUB(__imp__DmQuerySystemSettings);
REX_EXPORT_STUB(__imp__DmSaveSystemSettings);
REX_EXPORT_STUB(__imp__DmpGetPgoModuleHandleForBaseAddress);
REX_EXPORT_STUB(__imp__DmpOnPgoModuleLoad);
REX_EXPORT_STUB(__imp__DmPgoStartDataCollection);
REX_EXPORT_STUB(__imp__DmPgoStopDataCollection);
REX_EXPORT_STUB(__imp__DmPgoSaveSnapshot);
REX_EXPORT_STUB(__imp__IrtClientAbort);
REX_EXPORT_STUB(__imp__IrtPogoInit);
REX_EXPORT_STUB(__imp__IrtSetStaticInfo);
REX_EXPORT_STUB(__imp__IrtAutoSweepW);
REX_EXPORT_STUB(__imp__IrtAutoSweepA);
REX_EXPORT_STUB(__imp__DmGetDumpMode);
REX_EXPORT_STUB(__imp__DmSetDumpMode);
REX_EXPORT_STUB(__imp__DmGetDumpSettings);
REX_EXPORT_STUB(__imp__DmSetDumpSettings);
REX_EXPORT_STUB(__imp__DmGetEventDeferFlags);
REX_EXPORT_STUB(__imp__DmSetEventDeferFlags);
REX_EXPORT_STUB(__imp__DmWalkCommittedMemory);
REX_EXPORT_STUB(__imp__DmCloseCounters_2);
REX_EXPORT_STUB(__imp__DmRebootEx);
REX_EXPORT_STUB(__imp__DmMountFdfxVolume);
REX_EXPORT_STUB(__imp__DmCapGetFileHeader);
REX_EXPORT_STUB(__imp__DmCapFreeFileHeader);
REX_EXPORT_STUB(__imp__DmTraceStartRecording);
REX_EXPORT_STUB(__imp__DmTraceStartRecordingFunction);
REX_EXPORT_STUB(__imp__DmTraceSetBufferSize);
REX_EXPORT_STUB(__imp__DmTraceStopRecording);
REX_EXPORT_STUB(__imp__DmTraceSaveBuffer);
REX_EXPORT(__imp__DmGetConsoleType, rex::kernel::xbdm::DmGetConsoleType_entry)
REX_EXPORT_STUB(__imp__DmMapDevkitDrive);
REX_EXPORT_STUB(__imp__DmWalkLoadedModulesEx);
REX_EXPORT_STUB(__imp__DmGetXexHeaderField);
REX_EXPORT_STUB(__imp__DmGetMouseChanges);
REX_EXPORT_STUB(__imp__DmFindPdbSignature);
REX_EXPORT_STUB(__imp__DmGetProfilingStatus);
REX_EXPORT_STUB(__imp__DmOpticalDiscLogStart);
REX_EXPORT_STUB(__imp__DmOpticalDiscLogStop);
REX_EXPORT_STUB(__imp__DmStartSamplingProfiler);
REX_EXPORT_STUB(__imp__DmStopSamplingProfiler);
REX_EXPORT_STUB(__imp__DmGetSamplingProfilerInfo);
REX_EXPORT_STUB(__imp__DmStartFileEventCapture);
REX_EXPORT_STUB(__imp__DmStopFileEventCapture);
REX_EXPORT_STUB(__imp__DmSetFileEventMarker);
REX_EXPORT_STUB(__imp__DmMarkPseudoCreateBegin);
REX_EXPORT_STUB(__imp__DmMarkPseudoCreateEnd);
REX_EXPORT_STUB(__imp__DmMarkPseudoEventBegin);
REX_EXPORT_STUB(__imp__DmMarkPseudoEventEnd);
REX_EXPORT_STUB(__imp__DmMarkFileEventWorkerThreadBegin);
REX_EXPORT_STUB(__imp__DmMarkFileEventWorkerThreadEnd);
REX_EXPORT_STUB(__imp__DmGetSystemInfo);
REX_EXPORT_STUB(__imp__DmAbortProfiling);
REX_EXPORT_STUB(__imp__DmNetCaptureStart);
REX_EXPORT_STUB(__imp__DmNetCaptureStop);
REX_EXPORT_STUB(__imp__DmQueryTitleMemoryStatistics);
REX_EXPORT_STUB(__imp__DmAutomationGetInputProcess);
REX_EXPORT_STUB(__imp__DmAutomationBindController);
REX_EXPORT_STUB(__imp__DmAutomationUnbindController);
REX_EXPORT_STUB(__imp__DmAutomationConnectController);
REX_EXPORT_STUB(__imp__DmAutomationDisconnectController);
REX_EXPORT_STUB(__imp__DmAutomationSetGamepadState);
REX_EXPORT_STUB(__imp__DmAutomationQueueGamepadState);
REX_EXPORT_STUB(__imp__DmAutomationClearGamepadQueue);
REX_EXPORT_STUB(__imp__DmAutomationQueryGamepadQueue);
REX_EXPORT_STUB(__imp__DmAutomationGetUserDefaultProfile);
REX_EXPORT_STUB(__imp__DmAutomationSetUserDefaultProfile);
REX_EXPORT_STUB(__imp__DmTraceIsRecording);
REX_EXPORT_STUB(__imp__DmLoadDebuggerExtension);
REX_EXPORT_STUB(__imp__DmUnloadDebuggerExtension);
REX_EXPORT_STUB(__imp__DmCreateSystemThread);
REX_EXPORT_STUB(__imp__PEPLELookup);
REX_EXPORT_STUB(__imp__PEPLELookupCompact);
REX_EXPORT_STUB(__imp__PVPLELookup);
REX_EXPORT_STUB(__imp__PVPLELookupCompact);
REX_EXPORT_STUB(__imp__PVPLELookupCompactMDS);
REX_EXPORT_STUB(__imp__PVPLEFilteredLookup);
REX_EXPORT_STUB(__imp__PVPLEFilteredLookupCompact);
REX_EXPORT_STUB(__imp__PVPLEFilteredLookupCompactMDS);
REX_EXPORT_STUB(__imp__PVPLETemplatedLookup);
REX_EXPORT_STUB(__imp__PVPLETemplatedLookupCompact);
REX_EXPORT_STUB(__imp__PVPLETemplatedLookupCompactMDS);
REX_EXPORT_STUB(__imp__DmpPgoCounterOverflow);
REX_EXPORT_STUB(__imp__UpdateMDSProbeState);
REX_EXPORT_STUB(__imp__DmGetHttpRegistration);
REX_EXPORT_STUB(__imp__DmTraceSetIOThread);
REX_EXPORT_STUB(__imp__DmPMCInstallAndStart);
REX_EXPORT_STUB(__imp__DmPMCStopAndReport);
REX_EXPORT_STUB(__imp__DmPMCInstallSetup);
REX_EXPORT_STUB(__imp__DmPMCUnInstallSetup);
REX_EXPORT_STUB(__imp__DmPMCResetCounters);
REX_EXPORT_STUB(__imp__DmPMCSetTriggerProcessor);
REX_EXPORT_STUB(__imp__DmPMCStart);
REX_EXPORT_STUB(__imp__DmPMCStop);
REX_EXPORT_STUB(__imp__DmPMCGetCounter);
REX_EXPORT_STUB(__imp__DmPMCGetCounters);
REX_EXPORT_STUB(__imp__DmPMCGetCounterName);
REX_EXPORT_STUB(__imp__DmPMCDumpCounters);
REX_EXPORT_STUB(__imp__DmPMCDumpCountersVerbose);
REX_EXPORT_STUB(__imp__DmPMCGetCounterCostEstimate);
REX_EXPORT_STUB(__imp__DmPMCGetCounterSource);
REX_EXPORT_STUB(__imp__DmPMCComputeFrequency);
REX_EXPORT_STUB(__imp__DmGetAdditionalTitleMemorySetting);
REX_EXPORT_STUB(__imp__DmGetDebugMemorySize);
REX_EXPORT_STUB(__imp__DmGetConsoleDebugMemoryStatus);
REX_EXPORT_STUB(__imp__DmNetSimSetLinkStatusHidden);
REX_EXPORT_STUB(__imp__DmNetSimInsertQueue);
REX_EXPORT_STUB(__imp__DmNetSimRemoveQueue);
REX_EXPORT_STUB(__imp__DmNetSimRemoveAllQueues);
REX_EXPORT_STUB(__imp__DmNetSimModifyQueueSettings);
REX_EXPORT_STUB(__imp__DmGetConsoleFeatures);
REX_EXPORT_STUB(__imp__DmNetSimGetQueueStats);
REX_EXPORT_STUB(__imp__DmNetSimGetQueueSettings);
REX_EXPORT_STUB(__imp__DmNetSimGetNumQueues);
REX_EXPORT_STUB(__imp__DmNetSimInsertIpv4Redirect);
REX_EXPORT_STUB(__imp__DmNetSimRemoveIpv4Redirect);
REX_EXPORT_STUB(__imp__DmNetSimGetNumIpv4Redirects);
REX_EXPORT_STUB(__imp__XLFAllocate);
REX_EXPORT_STUB(__imp__XLFFree);
REX_EXPORT_STUB(__imp__XLFQueueAdd);
REX_EXPORT_STUB(__imp__XLFQueueAllocated);
REX_EXPORT_STUB(__imp__XLFQueueCreate);
REX_EXPORT_STUB(__imp__XLFQueueDestroy);
REX_EXPORT_STUB(__imp__XLFQueueGetEntryCount);
REX_EXPORT_STUB(__imp__XLFQueueIsEmpty);
REX_EXPORT_STUB(__imp__XLFQueueRemove);
REX_EXPORT_STUB(__imp__XLFQueueUnsafeDump);
REX_EXPORT_STUB(__imp__XLFStackAllocated);
REX_EXPORT_STUB(__imp__XLFStackCreate);
REX_EXPORT_STUB(__imp__XLFStackDestroy);
REX_EXPORT_STUB(__imp__XLFStackGetEntryCount);
REX_EXPORT_STUB(__imp__XLFStackIsEmpty);
REX_EXPORT_STUB(__imp__XLFStackPop);
REX_EXPORT_STUB(__imp__XLFStackPush);
REX_EXPORT_STUB(__imp__XLFStackUnsafeDump);
REX_EXPORT_STUB(__imp__XLFPriorityQueueAdd);
REX_EXPORT_STUB(__imp__XLFPriorityQueueAllocated);
REX_EXPORT_STUB(__imp__XLFPriorityQueueCreate);
REX_EXPORT_STUB(__imp__XLFPriorityQueueDestroy);
REX_EXPORT_STUB(__imp__XLFPriorityQueueGetEntryCount);
REX_EXPORT_STUB(__imp__XLFPriorityQueueIsEmpty);
REX_EXPORT_STUB(__imp__XLFPriorityQueueRemoveFirst);
REX_EXPORT_STUB(__imp__XLFPriorityQueueRemove);
REX_EXPORT_STUB(__imp__XLFPriorityQueueUnsafeDump);
REX_EXPORT_STUB(__imp__XLFHashTableAdd);
REX_EXPORT_STUB(__imp__XLFHashTableAllocated);
REX_EXPORT_STUB(__imp__XLFHashTableCreate);
REX_EXPORT_STUB(__imp__XLFHashTableDestroy);
REX_EXPORT_STUB(__imp__XLFHashTableGetEntryCount);
REX_EXPORT_STUB(__imp__XLFHashTableIsEmpty);
REX_EXPORT_STUB(__imp__XLFHashTableRemoveFirst);
REX_EXPORT_STUB(__imp__XLFHashTableRemove);
REX_EXPORT_STUB(__imp__XLFHashTableUnsafeDump);
REX_EXPORT_STUB(__imp__XLFPoolAcquireLock);
REX_EXPORT_STUB(__imp__XLFPoolAllocated);
REX_EXPORT_STUB(__imp__XLFPoolCreate);
REX_EXPORT_STUB(__imp__XLFPoolCreateLock);
REX_EXPORT_STUB(__imp__XLFPoolDestroy);
REX_EXPORT_STUB(__imp__XLFPoolDestroyLock);
REX_EXPORT_STUB(__imp__XLFPoolIncreaseEvents);
REX_EXPORT_STUB(__imp__XLFPoolIncreaseLocks);
REX_EXPORT_STUB(__imp__XLFPoolInitializeLock);
REX_EXPORT_STUB(__imp__XLFPoolNumberOfEvents);
REX_EXPORT_STUB(__imp__XLFPoolNumberOfLocks);
REX_EXPORT_STUB(__imp__XLFPoolReleaseLock);
REX_EXPORT_STUB(__imp__XLFStartLog);
REX_EXPORT_STUB(__imp__XLFStartUserLog);
REX_EXPORT_STUB(__imp__XLFInitializeLog);
REX_EXPORT_STUB(__imp__XLFInitializeUserLog);
REX_EXPORT_STUB(__imp__XLFEndLog);
REX_EXPORT_STUB(__imp__XLFLogPrint);
REX_EXPORT_STUB(__imp__XLFLogPrintV);
REX_EXPORT_STUB(__imp__XLFLogBuffer);
REX_EXPORT_STUB(__imp__XLFLogMessageStats);
REX_EXPORT_STUB(__imp__XLockFreeGetErrorHandler);
REX_EXPORT_STUB(__imp__XLockFreeSetErrorHandler);
REX_EXPORT_STUB(__imp__DmExecuteThreadRPC);
REX_EXPORT_STUB(__imp__DmGetDebuggerConnection);
