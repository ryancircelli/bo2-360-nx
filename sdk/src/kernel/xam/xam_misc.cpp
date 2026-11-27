/**
 * @file        rexkernel/xam/xam_misc.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include "../quiet_return.h"

namespace rex::kernel::xam {
// No dashboard app is active: report an empty X_DASH_APP_INFO (three zero words).
void XamGetActiveDashAppInfo_entry(mapped_void dash_app) {
  if (dash_app) std::memset(dash_app.as<uint8_t*>(), 0, 12);
}

// Offline: no profile manager, so the enumerator is valid but lists no accounts.
u32 XamProfileCreateEnumerator_entry(u32 device_id, mapped_u32 handle_ptr) {
  if (!handle_ptr) return X_ERROR_INVALID_PARAMETER;
  auto e = make_object<system::XStaticUntypedEnumerator>(REX_KERNEL_STATE(), 1, 0x28);
  auto result = e->Initialize(0xFF, 0xFE, 0x23001, 0x23003, 0);
  if (XFAILED(result)) return result;
  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}
}  // namespace rex::kernel::xam
REX_EXPORT(__imp__XamGetActiveDashAppInfo, rex::kernel::xam::XamGetActiveDashAppInfo_entry)

// kinda gross but oh well
REX_EXPORT_QUIET_RETURN(__imp__CancelWaitableTimer, 0);
REX_EXPORT_QUIET_RETURN(__imp__ControlPackGetCursorPosition, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__ControlPackGetHandleManager, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__ControlPackShouldShowCursor, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__ControlPackSideNavControlGetNuiHandle, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__ControlPackSimpleCursorGetRayFromScreenPosition, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__ControlpackNuiCursorSetTrackingId, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__CreateEventA, 0);
REX_EXPORT_QUIET_RETURN(__imp__CreateMutexA, 0);
REX_EXPORT_QUIET_RETURN(__imp__CreateThread, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__CreateWaitableTimerA, 0);
REX_EXPORT_QUIET_RETURN(__imp__CurlCloseCacheEntry, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlCreateCacheEntry, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlEnumerateCacheEntry, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlOpenCacheEntry, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlOpenTitleBackingFile, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlReadCacheEntryContent, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlReadCacheEntryHeaders, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlSetCacheEntryExpireTime, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlWipeBackingFiles, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__CurlWriteCacheEntryContent, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_Clear, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_CreateIndexBuffer, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_CreatePixelShader, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_CreateTexture, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_CreateVertexBuffer, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_CreateVertexDeclaration, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_CreateVertexShader, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_DrawIndexedVertices, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_DrawIndexedVerticesUP, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_DrawVertices, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_DrawVerticesUP, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetDepthStencilSurface, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_AlphaBlendEnable, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_BlendOp, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_CullMode, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_DestBlendAlpha, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_ParameterCheck, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_SrcBlendAlpha, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_ViewportEnable, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderState_ZEnable, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetRenderTarget, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_GetViewport, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_Release, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_Resolve, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetDepthStencilSurface, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetIndices, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetPixelShader, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetPixelShaderConstantB, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetPixelShaderConstantFN, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetPixelShaderConstantF_ParameterCheck, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_AlphaBlendEnable, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_BlendOp, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_CullMode, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_DestBlendAlpha, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_ParameterCheck, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_SrcBlendAlpha, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_ViewportEnable, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderState_ZEnable, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetRenderTarget, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetSamplerState_AddressU, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetSamplerState_AddressV, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetSamplerState_MagFilter, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetSamplerState_MinFilter, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetSamplerState_ParameterCheck, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetStreamSource, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetTexture, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetVertexDeclaration, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetVertexShader, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetVertexShaderConstantFN, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetVertexShaderConstantF_ParameterCheck, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DDevice_SetViewport, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DIndexBuffer_Lock, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DIndexBuffer_Unlock, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DPixelShader_AddRef, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DPixelShader_Release, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DResource_AddRef, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DResource_Release, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DTexture_LockRect, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DTexture_UnlockRect, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DVertexBuffer_Lock, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__D3DVertexBuffer_Unlock, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DVertexDeclaration_AddRef, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DVertexDeclaration_Release, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DVertexShader_AddRef, 0);
REX_EXPORT_QUIET_RETURN(__imp__D3DVertexShader_Release, 0);
REX_EXPORT_QUIET_RETURN(__imp__DebugBreak, 0);
REX_EXPORT_QUIET_RETURN(__imp__DebugXamShowMessageBoxUI_Wait, 0);
REX_EXPORT_QUIET_RETURN(__imp__Destroy, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__DuplicateHandle, 0);
REX_EXPORT_QUIET_RETURN(__imp__EnumerateMediaObjects, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__EnumerateMediaObjects_, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__EnumerateMediaObjects_0, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__EnumerateMediaObjects_1, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__EnumerateMediaObjects_2, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__ExitThread, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__FileTimeToLocalFileTime, 0);
REX_EXPORT_QUIET_RETURN(__imp__FileTimeToSystemTime, 0);
REX_EXPORT_QUIET_RETURN(__imp__GamerCardCleanup, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__GamerCardRegisterControls, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__GamerCardStartup, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__GamerCardUnregisterControls, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__GetCodecVersion, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__GetCurrentThreadId, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__GetLastError, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__GetLocalTime, 0);
REX_EXPORT_QUIET_RETURN(__imp__GetModuleHandleA, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__GetOverlappedResult, 0);
REX_EXPORT_QUIET_RETURN(__imp__GetProcessHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__GetSystemTime, 0);
REX_EXPORT_QUIET_RETURN(__imp__GetSystemTimeAsFileTime, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__GetTickCount, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__GetTimeZoneInformation, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__InjectConnectionServerNotification, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__IsBadReadPtr, 0);
REX_EXPORT_QUIET_RETURN(__imp__IsBadWritePtr, 0);
REX_EXPORT_QUIET_RETURN(__imp__LocalAlloc, 0);
REX_EXPORT_QUIET_RETURN(__imp__LocalFree, 0);
REX_EXPORT_QUIET_RETURN(__imp__MultiByteToWideChar, 0);
REX_EXPORT_QUIET_RETURN(__imp__OnReconnect, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__OpenMutexA, 0);
REX_EXPORT_QUIET_RETURN(__imp__PIXAddCounter, 0);
REX_EXPORT_QUIET_RETURN(__imp__PIXBeginCapture, 0);
REX_EXPORT_QUIET_RETURN(__imp__PIXEndCapture, 0);
REX_EXPORT_QUIET_RETURN(__imp__PIXGetGPUSlot, 0);
REX_EXPORT_QUIET_RETURN(__imp__QueryPerformanceCounter, 0);
REX_EXPORT_QUIET_RETURN(__imp__QueryPerformanceFrequency, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__RaiseException, 0);
REX_EXPORT_QUIET_RETURN(__imp__Refresh, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__Refresh_, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__Refresh_0, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__Refresh_1, 0x80004001);
REX_EXPORT_QUIET_RETURN(__imp__ReleaseMutex, 0);
REX_EXPORT_QUIET_RETURN(__imp__ResetEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__ResumeThread, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__RtlAllocateHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlAllocateHeapSlowly, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlCompactHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlCreateHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugCompactHeap, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugEntry, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugError, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugExit, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugPrint, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugSetLevel, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDebugWalkHeap, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlDestroyHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlFindFirstFile, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlFindNextFile, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlFreeHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlFreeHeapSlowly, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlGetAttributesOnHeapAlloc, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlGetLastError, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__RtlGetModuleFileName, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlLockHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlRandom, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__RtlReAllocateHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlRemoveDirectory, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlSetAttributesOnHeapAlloc, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__RtlSetLastError, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlSetLastNTError, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__RtlSizeHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlSleep, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__RtlUniform, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlUnlockHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlValidateHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__RtlWalkHeap, 0xC00000BB);
REX_EXPORT_QUIET_RETURN(__imp__RtlZeroHeap, 0xC00000BB);
REX_EXPORT_QUIET_RETURN(__imp__SetEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__SetLastError, 0);
REX_EXPORT_QUIET_RETURN(__imp__SetUnhandledExceptionFilter, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__SetWaitableTimer, 0);
REX_EXPORT_QUIET_RETURN(__imp__Sleep, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__SleepEx, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__SystemTimeToFileTime, 0);
REX_EXPORT_QUIET_RETURN(__imp__UnhandledExceptionFilter, 0);
REX_EXPORT_QUIET_RETURN(__imp__UpnpAVServerSearchDestroy, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__UpnpAVServerSearchDoWork, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__UpnpAVServerSearchGetNextServer, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__UpnpAVServerSearchInitialize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__UpnpAVServerSearchStart, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__WaitForSingleObject, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__WaitForSingleObjectEx, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__WideCharToMultiByte, 0);
REX_EXPORT_QUIET_RETURN(__imp__XAutomationpBindController, 0);
REX_EXPORT_QUIET_RETURN(__imp__XAutomationpInputPress, 0);
REX_EXPORT_QUIET_RETURN(__imp__XAutomationpInputSetState, 0);
REX_EXPORT_QUIET_RETURN(__imp__XAutomationpInputXenonButton, 0);
REX_EXPORT_QUIET_RETURN(__imp__XAutomationpUnbindController, 0);
REX_EXPORT_QUIET_RETURN(__imp__XCustomBroadcastActionEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__XCustomBroadcastGamercardEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__XCustomGetBannerImage, 0);
REX_EXPORT_STUB_RETURN(__imp__XCustomGetCurrentGamercard, 0);  // spec: no gamercard action pending
REX_EXPORT_STUB_RETURN(__imp__XCustomGetLastActionPress, 0);  // FALSE: no press, out untouched
REX_EXPORT_STUB_RETURN(__imp__XCustomGetLastActionPressEx, 0);
REX_EXPORT_STUB_RETURN(__imp__XCustomSetAction, 0);
REX_EXPORT_STUB_RETURN(__imp__XCustomSetDynamicActions, 0);
REX_EXPORT_STUB_RETURN(__imp__XCustomUnregisterDynamicActions, 0);
REX_EXPORT_QUIET_RETURN(__imp__XDebugError, 0);
REX_EXPORT_QUIET_RETURN(__imp__XDebugWarning, 0);
REX_EXPORT_QUIET_RETURN(__imp__XDebugWarning_, 0);
REX_EXPORT_QUIET_RETURN(__imp__XFriendsCreateEnumerator, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XGGetTextureDesc, 0);
REX_EXPORT_QUIET_RETURN(__imp__XGOffsetResourceAddress, 0);
REX_EXPORT_QUIET_RETURN(__imp__XGSetTextureHeader, 0);
REX_EXPORT_QUIET_RETURN(__imp__XGSetTextureHeaderEx, 0);
REX_EXPORT_QUIET_RETURN(__imp__XGetAudioFlags, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_PASSTHROUGH(__imp__XGetDisplaySize);
REX_EXPORT_QUIET_RETURN(__imp__XGetOverlappedExtendedError, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XGetOverlappedResult, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XGetSSLCertBypassEnabled, 0);
REX_EXPORT_QUIET_RETURN(__imp__XMPRegisterCodec, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XMemAlloc, 0);
REX_EXPORT_QUIET_RETURN(__imp__XMemCpy, 0);
REX_EXPORT_QUIET_RETURN(__imp__XMemCpyStreaming, 0);
REX_EXPORT_QUIET_RETURN(__imp__XMemFree, 0);
REX_EXPORT_QUIET_RETURN(__imp__XMemSet, 0);
REX_EXPORT_QUIET_RETURN(__imp__XMemSize, 0);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonClearTicketCaches, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetDnsString, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetExtendedStatus, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetFlowToken, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetLastUPnPStatus, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetLoggedOnUsers, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XNetLogonGetMachineID, 0x80151802);  // XONLINE_E_LOGON_NOT_LOGGED_ON, out unchanged
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetNatType, 1);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetServiceInfo, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetServiceNetworkID, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetState, 0);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetTicketOpt, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetTitleID, 0)  // no reference implementation; success, no log
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetTitleVersion, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonGetUserPrivileges, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonInitOverrideInfo, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonSetConsoleCertificate, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonSetTicketOpt, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonSetTitleID, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonTaskClose, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonTaskContinue, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XNetLogonTaskStart, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XOnlinepFriendsAcceptRequest, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XOnlinepFriendsRejectRequest, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XOnlinepFriendsRemove, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XOnlinepFriendsRequest, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XRegisterThreadNotifyRoutine, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XTLGetLanguageV2, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XTLGetLocaleV1, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XTestOnlineIsConnectedToLive, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_Clear, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_Construct, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_Destruct, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_SetBool, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_SetColorFromUint, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_SetFloat, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_SetInt, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_SetString, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUIElementPropVal_SetUint, 0);
REX_EXPORT_QUIET_RETURN(__imp__XUITimeline_Run, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamAccountRecoveryGetProgress, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamAccountRecoveryGetSetTitleSyncTime, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamAccountRecoveryRecoverAllProfileData, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamAccountRecoveryRecoverTitle, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamActivateCounterMeasure, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAllocDevkitHeapAvailable, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamAllocEx, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamAllocFreeIPTVHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamAllocHeapFreeSpace, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAllocMountIPTVHeap, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamAllocSize, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamAllocUnmountIPTVHeap, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamAppAllocateInterappWorkspace, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppAllocateInterappWorkspaceEx, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppFreeInterappWorkspace, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppGetInterappWorkspace, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppGetSessionId, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppLoad, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppLoadPass2SysApps, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppReinitialize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppReportError, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppRequestLoad, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppRequestLoadEx, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppSetTestOption, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppUnloadSelf, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppUnloadStack, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAppUnrequestLoad, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamAreMixedAccountsSignedIn, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamAskUserForCSVDrainagePermission, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadBindItems, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadCacheLegacyMappingInfo, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadClearItems, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadDoFail, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetActiveItem, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetForegroundPriority, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetItems, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetLegacyId, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetLegacyMappingInfo, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetMode, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetPollingActive, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadGetUnexpectedChangeCount, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadHistoryGetItem, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadIsEnabled, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadIsItemForThisConsole, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemAdd, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamBackgroundDownloadItemGetHistoryStatus, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemGetHistoryStatusEx, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamBackgroundDownloadItemGetStatus, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemGetStatusAsync, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemGetStatusEx, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemMakeFirst, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemModify, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemRemove, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemSetHistoryStatus, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadItemToContentData, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadNetworkStorageEnable, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadNetworkStorageGetProgress, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadNetworkStorageIsEnabled, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadNetworkStorageIsRunning, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadNetworkStorageOnContentChange, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadNetworkStorageRegisterChangeCallback, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadSelectDevice, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadSetForegroundPriority, 0x80004005);
REX_EXPORT_QUIET_PASSTHROUGH(__imp__XamBackgroundDownloadSetMode)  // no reference implementation; success, no log
REX_EXPORT_QUIET_RETURN(__imp__XamBackgroundDownloadSetPollingActive, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBase64Decode, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBeginDemand, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamBuildDynamicResourceLocator, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamCacheCloseFile, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheDeleteFile, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheDeleteFileByString, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheFetchFile, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheFetchFileByString, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheIntegrityCheck, 0x490);
REX_EXPORT_STUB_RETURN(__imp__XamCacheOpenFile, 0x8007065B);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheRenameFile, 0x490);
REX_EXPORT_STUB_RETURN(__imp__XamCacheReset, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheStoreFile, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamCacheStoreFileByString, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamCertDecode, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamCertDecodeRsaPublicKey, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamCertGetSerialNumber, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamCertValidateStruct, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamClearTitle, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCodeCoverageFileClose, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCodeCoverageFileLseek, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCodeCoverageFileOpen, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCodeCoverageFileRead, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCodeCoverageFileWrite, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCompleteDemand, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamConvertEmoticons, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCopyFile, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamCustomGetCurrentActions, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceAddRef, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceDescriptorAddRef, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceDescriptorCreate, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceDescriptorRelease, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceIsBusy, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceRelease, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamD3DResourceSet, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamDataCenterGetDhcpOptionResponseHostnameAndPort, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamDataCenterLog, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDataCenterLogEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDataCenterShowForceSignInMessage, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamDbgSetBreakLevel, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDbgSetOutputLevel, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDeactivateCounterMeasure, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDebugGetBackURI, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDebugGetURISchemeCount, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDebugGetURISchemeInfo, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDecompressPNGToTexture, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamDeleteFile, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamDemand, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamDestroyGameTileCache, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamDevAuthSetFault, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamDevAuthSetFaultEx, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamDeviceRemap, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamDoesOmniNeedConfiguration, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamDownloadInSingleItemMode, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamDownloadMarketplaceStoresList, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamDownloadSingleItem, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamEnableOverdraw, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamEnableSystemAppInput, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamEtxFlushLogs, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamExecuteChallenge, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamExecutingOnBehalfOfTitle, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamFamilyAddParentalExemptionToList, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamFamilyGetMembers, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamFamilyIsParentalValidationRequired, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamFeatureEnableDisable, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFeatureEnforceImageBudget, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFeatureSetMask, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFindHUDElementByXuiId, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFindOrCreateInternalPassportAccount, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamFirstRunExperienceShouldRun, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessAddBodyProfileRecord, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessAddFitnessEvent, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessClearAll, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessClearBodyProfileRecords, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessContainsFitnessData, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessConvertByteMetToFloatMet, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessConvertFloatMetToByteMet, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessCreateFitnessEventEnumerator, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetAllBodyProfileRecords, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetAllTitleSummaries, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetCurrentBodyProfileRecord, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetOverallSummary, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetPrivacySettings, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetSuggestedPrivacySettings, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetSyncStatus, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetTitleSummaries, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessGetTitleSummary, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessInitialize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessInitializeForOneUser, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessMsgTimeToSystemTime, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessSystemTimeToMsgTime, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFitnessUnInitialize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamFlushAgraTemperatureReport, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamFreeToken, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamGetActiveCounterMeasures, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetAdaptersCollection, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetAppliesTo, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetCachedGamerTag, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetCachedGamerTagW, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetCachedTitleName, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamGetCachedTitleNameEx, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetClosedCaptioningSettings, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetClosedCaptioningState, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamGetConsoleFriendlyName, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetCurrentDemand, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetCurrentSystemOnlineManifestRevision, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetDashBackstackData, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamGetDashBackstackNodesCount, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetDashContextEx, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetDefaultImage, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetDefaultSystemImage, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetDvrStorage, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetErrorStringFromWebService, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetGameRatings, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetGamerTileKeyFromGamertag, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetHUDElementByAutoId, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetHUDElementByPath, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetHudContext, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetLastActiveUserData, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetLiveHiveValueA, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetLiveHiveValueDuringLogonAttemptA, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetLiveHiveValueDuringLogonAttemptExA, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetLiveHiveValueW, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetModernPresence, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetModernPresenceImageUrl, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetModernPresenceWorkspaceSize, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetPasscodeKeyFromVirtualKey, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetRootObj, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetSTSTokenCache, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetSecurityViolationsDetected, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetServiceEndpoint, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetSingleModernPresence, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetStagingMode, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamGetTLSConfig, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetTitleGlobalStorageValue, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamGetToken, 0x80151802);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamGetUserBalance, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetUserInMemoryWindowsLiveCreds, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetUserNeedTFA, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetUserPresetPresenceState, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetUserPresetWindowsLiveCreds, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetUserShowMessengerFriends, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetWCNConfigFile, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamGetXSTSTokenExpiryPeriodMS, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamGetXTweakManager, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamHudGetUserIndex, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamHudSetUserIndex, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamInactivityEnablePowerDown, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamInactivityGetInactivityInterruptTime, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamInactivityGetLastActivityTime, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamInactivitySetDetectionTaskPeriod, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamInactivitySetInactivityInterruptTime, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamInitializeGameTileCache, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamInstrumentationDebugConfig, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamInstrumentationGetSourceConfig, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamInstrumentationLogEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamInstrumentationLogEventEx, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIptvGetServiceName, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamIptvUninstall, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamIsAnySecurityViolationDetected, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsBackgroundSceneInTransition, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsCSVDrainageSupported, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsCounterMeasureActivated, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsCurrentTitleDatacenterClient, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsCurrentTitleDatacenterDash, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsCurrentTitleIptv, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsDataCenterMode, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsDvrRecording, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsNatalPlaybackEnabled, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsNetworkCaptivePortal, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsNuiAutomationEnabled, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsOnPrivateNetwork, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsOptionalMediaUpdateInstalled, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsScreenSaverActive, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsSecurityViolationDetected, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamIsUserUsingMetricSystem, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLaunchAvatarEditor, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLaunchURI, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLiveBaseConfigureInMemoryMockResponse, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamLoadSysApp, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderGetClearCache, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderGetCurrentTitleD3DVersion, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderGetGameInfo, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderGetLogonTitle, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderGetPriorTitleId, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderGetState, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderIsKinectUIPreferredForLogonTitle, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderIsTitleTerminatePending, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderLaunchTitleEx, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderLaunchTitleForReason, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderLaunchTitleOnDvd, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderRebootToDash, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderRebootToServerDash, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderRegisterLaunchRequestCallback, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderSetClearCache, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderSetGameInfo, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderSetSpindleSpeed, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLoaderShouldConfirmReboot, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLocateTitle, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLogLocalizationEtx, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLookupCommonStringByIndex, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcEncryptDecryptTitleMessage, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcGetInput, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcGetInputWithSeek, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardApplyTextChange, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardGetInput, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardGetRequiredBufferSize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardRegister, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardUnregister, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardUpdateText, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcKeyboardValidateTextChange, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcLogError, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcLogSessionSummary, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcSetMediaState, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcSetTitlePort, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamLrcVerifyClientId, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamMarkInternalAccountTrustedOnConsole, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamMarketplaceAcquireFreeContent, 0x80151802);
REX_EXPORT_STUB_RETURN(__imp__XamMediaVerificationClose, 0);
REX_EXPORT_STUB_RETURN(__imp__XamMediaVerificationCreate, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamMediaVerificationFailedBlocks, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamMediaVerificationInject, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamMoveFile, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStatusAddAddress, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStatusGetInformation, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageAppLauncherLaunchIfRunning, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageCheckOutTitle, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageCreateCacheOnDevice, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageDeleteCacheOnDevice, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageDeviceHasCacheFile, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetBackingDevice, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetLastSyncTime, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetNetworkDevice, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetStatus, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetTitleProperties, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetUserLastError, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetUserNetworkSyncTime, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageGetUserProperties, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageHasUserEnabledStorage, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageIsCheckingSyncStates, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageIsEnabledInLiveHive, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageIsSupportedContentType, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageIsTitleExempt, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageIsUserInSync, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageLockFile, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageOnSigninChanged, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageOnUserSyncChange, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageSetTestMock, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageSetTitleProperties, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageSetUserInSync, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageShouldHideFromTitle, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageSignInTestMock, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageUserHasPrivilege, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamNetworkStorageUserNeedsToSync, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerForceQuery, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerGetContractData, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerGetData, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerGetTimeRemaining, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerInitialize, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerIsEnabled, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerIsRunning, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerResume, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerSetData, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerShutdown, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamOfflineTimerSuspend, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerDeleteExtendedPartition, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerFindPackageContainingIndexedXEX, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerGetAuthoritativeManifestVersion, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerGetExperienceMode, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerGetFeatureRequiresUpdateStrings, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerGetFilePathW, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerGetFileSize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerHasExtendedPartition, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPackageManagerReinitialize, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPixSetDirtyCounters, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerForceNotification, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerFormatDurationString, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerGetData, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerGetNextResetDate, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerGetTimeRemaining, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerIsEnabled, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerIsRunning, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerRefreshResetDate, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerResume, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerSetData, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerSuspend, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayTimerUpdateTimeRemaining, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayToCancelGetRequest, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayToCompleteRequest, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayToGetRequest, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayToRegister, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayToReportPlaybackState, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPlayToUnregister, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPmdIdentify, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPmdReadReport, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPmdSendCommand, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamPngDecode, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamPngEncode, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamPngEncodeEx, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamPrepareGamerTiles, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileClose, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileControlPec, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileCreate, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT(__imp__XamProfileCreateEnumerator, rex::kernel::xam::XamProfileCreateEnumerator_entry)
REX_EXPORT_QUIET_RETURN(__imp__XamProfileDelete, 0x65B);
REX_EXPORT_STUB_RETURN(__imp__XamProfileEnumerate, 0x80070012);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileFindAccount, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamProfileGetCachedWindowsLiveEmail, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileGetCreationStatus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamProfileGetLastSync, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileGetLiveLegalLocale, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamProfileGetLocalTitleCount, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileIsSaveWindowsLiveCredsEnabled, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileLoadAccountInfo, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamProfileLoadWindowsLiveCredentials, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileOpen, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamProfileRecoverTitle, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileRenameAccount, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileSaveAccountInfo, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileSaveWindowsLiveCredentials, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileSetCachedWindowsLiveEmail, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamProfileSetSaveWindowsLiveCredsEnabled, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamPushBackURI, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamQueryLiveHiveA, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamQueryTimeZoneInformation, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamRamDriveCreate, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamRamDriveDestroy, 0x80004005);
REX_EXPORT_STUB_RETURN(__imp__XamReadBiometricData, 0x80070525);  // no Kinect identity
REX_EXPORT_QUIET_RETURN(__imp__XamReadGameTileImage, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReadImage, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReadString, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamReadStrings, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamReadTile, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamReadTileEx, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamReadTileToTextureEx, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamRegisterSMCNotification, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamRegisterScreenSaverCallback, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamRegisterSysApp, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamReleaseCameraPreviewStuff, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamReloadSysApp, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderClearCache, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderClearItems, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderGetItems, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderGetModifiedTime, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderGetNextItem, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderItemAdd, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamReminderItemRemove, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamRenderCameraPreviewStuff, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamReportKinectSettingsChangedEvent, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamReportTLSConnect, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamRequestToken, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamResolveURI, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamRestartTitleLoadAfterUpdate, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamResumeUpdaterUI, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSendMessageToLoadedApps, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetActiveDashAppInfo, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamSetAutomation, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetClosedCaptioningState, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetConsoleFriendlyName, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetCountry, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XamSetCurrentTitleDash, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetDashBackstackData, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetDashContextEx, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetDvrStorage, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetHudContext, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetInactivityTime, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamSetInactivityTimeFromConfig, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamSetLastActiveUserData, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamSetPowerMode, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetProfileReadTestHook, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamSetSecurityViolationDetected, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamSetStagingMode, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetTimeZoneInformation, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamSetTitleGlobalStorageValue, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetUserNeedTFA, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamSetUserPresetPresenceState, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamSetUserPresetWindowsLiveCreds, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamSetUserShowMessengerFriends, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamSetupCameraPreviewStuff, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSetupTexture, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamShouldThrottleAccountInfoCall, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamShutdown, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamSwapCancel, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSystemUpdateInstallLocal, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamSystemUpdaterLogon, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamTerminateTitleForUpdate, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamTestShowNuiTroubleshooterUI, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUIThreadDisableFontPatching, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUIThreadEnableFontPatching, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUnloadSysApp, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUnregisterSysApp, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateAllocateExtenderBuffer, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateAttachExtenderInstance, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateChainPass3FromPass2, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateEtxLogUpdateTaskFailure, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateEtxLogUpdateTaskStart, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateEtxLogUpdateTaskSuccess, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateFinish, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetBaseSystemVersion, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetChainMarker, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetCurrentSystemVersion, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetData, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetExtenderInstance, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetProgress, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateGetTitleUpdateSizeThreshold, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamUpdateStart, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamVerifyXSignerSignature, 0x65B);
REX_EXPORT_QUIET_RETURN(__imp__XamWaitForNSAL, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationCreateReport, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationCreateSampledReport, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationDestroyReport, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationGetURL, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationGetURLEx, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationSendReport, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationSetUserVar, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamWebInstrumentationSetUserVarNoEscape, 0);
REX_EXPORT_STUB_RETURN(__imp__XamWriteBiometricData, 0x80070525);
REX_EXPORT_QUIET_RETURN(__imp__XamWriteGamerTileEx, 0x490);
REX_EXPORT_QUIET_RETURN(__imp__XamWriteTile, 0x490);
REX_EXPORT_STUB_RETURN(__imp__XamXStudioRequest, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamXgiBlobTrackerSetMockInfo, 0);
REX_EXPORT_STUB_RETURN(__imp__XamXlfsInitializeUploadQueue, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamXlfsInitializeUploadQueueWithTestHooks, 0);
REX_EXPORT_STUB_RETURN(__imp__XamXlfsMountUploadQueueInstance, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamXlfsNotifyContentDeletion, 0);
REX_EXPORT_STUB_RETURN(__imp__XamXlfsUninitializeUploadQueue, 0x32);
REX_EXPORT_STUB_RETURN(__imp__XamXlfsUnmountUploadQueueInstance, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerDecrement, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerDeleteDataFile, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerGetDataFilePath, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerGetId, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerGetValue, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerGetVariableCount, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerIncrement, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerLoad, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerResetAll, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerResetValue, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerSave, 0);
REX_EXPORT_QUIET_RETURN(__imp__XamXtwManagerSetValue, 0);
REX_EXPORT_QUIET_RETURN(__imp__XampCallXAccountTransitionStoredValuePointsToCurrency, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateCheckOnlineManifestChanged, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateGetAttributes, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateGetExtendedAttributes, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateGetInstance, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateGetManifestLocalPath, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateIsRefreshTestFlagSet, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateRefreshManifest, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateRefreshManifestDuringLogon, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateRunCacheCleaner, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampDemandUpdateSetRefreshTestFlags, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampFitnessLetFireAndForgetsCatchUpInternal, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampHeapGetCount, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampHeapGetInfo, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampKioskIsSimulationMode, 0);
REX_EXPORT_QUIET_RETURN(__imp__XampKioskSetTestMock, 0);
REX_EXPORT_QUIET_RETURN(__imp__XampSetMockCallXAccountTransitionStoredValuePointsToCurrency, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XampSetOmnitureCallbackFunction, 0);
REX_EXPORT_QUIET_RETURN(__imp__XampSetSamplingRandomValue, 0);
REX_EXPORT_QUIET_RETURN(__imp__XampSystemInput, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XampWebInstrumentationSetProfileCounts, 0);
REX_EXPORT_STUB_RETURN(__imp__XampXAuthGetTitleBuffer, 0x32);
REX_EXPORT_QUIET_RETURN(__imp__XampXAuthIsLocalSocketAllowed, 0x80151802);
REX_EXPORT_STUB_RETURN(__imp__XampXAuthShutdown, 0);
REX_EXPORT_STUB_RETURN(__imp__XampXAuthStartup, 0);
REX_EXPORT_QUIET_RETURN(__imp__XapiDirectoryInformationToFindData, 0);
REX_EXPORT_QUIET_RETURN(__imp__XapiFormatObjectAttributes, 0x80004005);
REX_EXPORT_QUIET_RETURN(__imp__XapipCreateThread, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XapipGetLocale, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XdfCacheItem, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfCancelRequest, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfGetExpectedDownloadSize, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfGetItem, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfInitialize, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfLaunchNewImageFromCache, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfLoadXex, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfLoadXexFromCache, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfResumeDelayedItem, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XdfShutdown, 0x80151802);
REX_EXPORT_QUIET_RETURN(__imp__XnpGetXwppMemoryLogSnapshot, 0);
REX_EXPORT_QUIET_RETURN(__imp__XnpGetXwppRuntimeFilter, 0);
REX_EXPORT_QUIET_RETURN(__imp__XuiAlloc, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiAnimRun, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiApplyLocale, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiAttachTextureBrush, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiBroadcastMessage, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiBrushGetTexture, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiBubbleMessage, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiClassDerivesFrom, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlAttachVisual, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlGetItemAssociation, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlGetNavigation, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlGetVisual, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlIsBackButton, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlIsNavButton, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlPlayOptionalVisual, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlSetItemAssociation, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiControlWantsUnfocusedInput, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiCopyString, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiCreateObject, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiCreateSolidBrush, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiCreateTextureBrush, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDefault_EmptyString, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDefault_False, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDefault_IntegerZero, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDefault_True, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDestroyBrush, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDestroyObject, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDrawShape, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiDynamicCast, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiEditGetTextLimit, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiEditSetTextFormatInfo, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiEditSetTextLimit, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementAddChild, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementBeginRender, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementDisallowRecursiveTimelineControl, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementDiscardResources, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementEnableInput, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementEndRender, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementFindNamedFrame, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetChildById, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetDescendantById, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetFirstChild, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetFocusUser, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetFullXForm, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetId, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetLastChild, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetNext, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetOpacity, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetParent, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetPivot, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetPosition, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetPrev, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetScale, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetScreenPositionCenter, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetTimeline, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetUserData, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetUserFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementGetXForm, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementHasFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementInitFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementInitUserFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementInputEnabled, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementInsertChild, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementIsDescendant, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementLayoutTree, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementPlayNamedFrames, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementPlayTimeline, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementRenderChildren, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetBounds, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetFocus, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetHittable, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetOpacity, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetPivot, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetPosition, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetRotation, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetScale, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetUserData, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementSetUserFocus, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementTreeGetFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementTreeHasFocus, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiElementUnlink, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFigureClose, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFigureGetShape, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFigureSetFill, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFigureSetShape, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFigureSetTexture, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFillRect, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFindClass, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFree, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFreeStringTable, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFreeUnusedTextures, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiFreeVisuals, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiGetBaseClass, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiGetBaseObject, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiGetClass, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiGetObjectClass, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiGetOuter, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiHandleIsValid, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiImageElementGetImagePath, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiImageElementSetImagePath, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiInit, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiIsInstanceOf, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiKillTimer, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiListEnableItemOverride, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiListGetDefaultItemSize, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiLoadFromBinary, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiLoadStringTableFromFile, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiLoadVisualFromBinary, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiLookupStringTable, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiLookupStringTableByIndex, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiMatrixMultiply, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiNavButtonGetPressPath, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiObjectFromHandle, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiObjectGetProperty, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiObjectGetPropertyDef, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiObjectGetPropertyId, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiObjectSetProperty, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiPNGTextureLoader, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiProcessInput, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiProgressBarGetRange, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiProgressBarGetValue, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiProgressBarSetRange, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiProgressBarSetValue, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRealloc, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRegisterClass, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderBegin, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderCreateDC, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderDCDeviceChanged, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderDestroyDC, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderEnd, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderGetBackBufferSize, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderGetColorSpace, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderGetDevice, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderGetViewTransform, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderGetXuiDevice, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderInit, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderInitShared, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderPresent, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderRestoreState, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderSetViewTransform, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiRenderUninit, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceClose, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceComposeLocator, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceGetBuffer, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceGetPackageEntryCount, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceGetPackageEntryInfo, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceGetTotalSize, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceLoadAll, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceLocatorIsAbsolute, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceOpen, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceOpenAndVerifyNoLoc, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceOpenNoLoc, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceOpenPackage, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceRead, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceReleasePackage, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiResourceSeek, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneCreate, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneEnableTransitionDependency, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneInterruptTransitions, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneIsInTransition, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneNavigateBack, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneNavigateBackToFirst, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneNavigateFirst, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSceneNavigateForward, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiScenePlayBackFromTransition, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiScenePlayBackToTransition, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiScenePlayFromTransition, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiScenePlayToTransition, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSelectBrush, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSelectBrushEx, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSendMessage, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSetLocale, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSetMessageFilter, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSetTimer, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSliderGetRange, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSliderGetStep, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSliderGetValue, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSliderSetRange, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSliderSetStep, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiSliderSetValue, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTabSceneGetCount, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTabSceneGetCurrentTab, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTabSceneGoto, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTextElementGetText, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTextElementMeasureText, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTextElementSetText, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiTimersRun, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiUninit, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiUnregisterClass, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiVec2TransformCoord, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiVisualCreateInstance, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiVisualGetBasePath, 0x80004005);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__XuiVisualSetBasePath, 0);  // no reference; deliberate family default, behaviour unknown
REX_EXPORT_QUIET_RETURN(__imp__lstrcatA, 0);
REX_EXPORT_QUIET_RETURN(__imp__lstrcatW, 0);
REX_EXPORT_QUIET_RETURN(__imp__lstrcpyA, 0);
REX_EXPORT_QUIET_RETURN(__imp__lstrcpyW, 0);
REX_EXPORT_QUIET_RETURN(__imp__lstrcpynA, 0);
REX_EXPORT_QUIET_RETURN(__imp__lstrcpynW, 0);
REX_EXPORT_QUIET_RETURN(__imp__lstrlenA, 0);
REX_EXPORT_QUIET_RETURN(__imp__wsprintfA, 0);
REX_EXPORT_QUIET_RETURN(__imp__wsprintfW, 0);
