// XBDM service ABI used by the exports and kernel regression tests.
#pragma once
#include <rex/types.h>
namespace rex::kernel::xbdm {
constexpr u32 kDmSuccess = 0x02DA0000;
constexpr u32 kDmFailure = 0x82DA0000;
constexpr u32 kDmBufferTooSmall = 0x82DA0105;
u32 DmAllocatePool_entry(u32);
u32 DmFreePool_entry(mapped_void);
u32 DmGetXboxName_entry(mapped_void, mapped_u32);
u32 DmGetConsoleType_entry(mapped_u32);
u32 DmGetMemory_entry(mapped_void, u32, mapped_void, mapped_u32);
u32 DmSetMemory_entry(mapped_void, u32, mapped_void, mapped_u32);
u32 DmGetThreadList_entry(mapped_u32, mapped_u32);
u32 DmResumeThread_entry(u32);
u32 DmSuspendThread_entry(u32);
}
