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

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>
#include <string>

#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include "../quiet_return.h"
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xexception.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

void DbgBreakPoint_entry() {
  rex::debug::Break();
}

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
typedef struct {
  rex::be<uint32_t> type;
  rex::be<uint32_t> name_ptr;
  rex::be<uint32_t> thread_id;
  rex::be<uint32_t> flags;
} X_THREADNAME_INFO;
static_assert_size(X_THREADNAME_INFO, 0x10);

void HandleSetThreadName(ppc_ptr_t<X_EXCEPTION_RECORD> record) {
  // SetThreadName. FFS.
  // https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx

  // TODO(benvanik): check record->number_parameters to make sure it's a
  // correct size.
  auto thread_info = reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  assert_true(thread_info->type == 0x1000);

  if (!thread_info->name_ptr) {
    REXKRNL_DEBUG("SetThreadName called with null name_ptr");
    return;
  }

  // 4D5307D6 (and its demo) has a bug where it ends up passing freed memory for
  // the name, so at the point of SetThreadName it's filled with junk.

  // TODO(gibbed): cvar for thread name encoding for conversion, some games use
  // SJIS and there's no way to automatically know this.
  auto name =
      std::string(REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(thread_info->name_ptr));
  std::replace_if(name.begin(), name.end(), [](auto c) { return c < 32 || c > 127; }, '?');

  object_ref<XThread> thread;
  if (thread_info->thread_id == -1) {
    // Current thread.
    thread = retain_object(XThread::GetCurrentThread());
  } else {
    // Lookup thread by ID.
    thread = REX_KERNEL_STATE()->GetThreadByID(thread_info->thread_id);
  }

  if (thread) {
    REXKRNL_DEBUG("SetThreadName({}, {})", thread->thread_id(), name);
    thread->set_name(name);
  }

  // TODO(benvanik): unwinding required here?
}

typedef struct {
  rex::be<int32_t> mdisp;
  rex::be<int32_t> pdisp;
  rex::be<int32_t> vdisp;
} x_PMD;

typedef struct {
  rex::be<uint32_t> properties;
  rex::be<uint32_t> type_ptr;
  x_PMD this_displacement;
  rex::be<int32_t> size_or_offset;
  rex::be<uint32_t> copy_function_ptr;
} x_s__CatchableType;

typedef struct {
  rex::be<int32_t> number_catchable_types;
  rex::be<uint32_t> catchable_type_ptrs[1];
} x_s__CatchableTypeArray;

typedef struct {
  rex::be<uint32_t> attributes;
  rex::be<uint32_t> unwind_ptr;
  rex::be<uint32_t> forward_compat_ptr;
  rex::be<uint32_t> catchable_type_array_ptr;
} x_s__ThrowInfo;

void HandleCppException(ppc_ptr_t<X_EXCEPTION_RECORD> record) {
  // C++ exception.
  // https://blogs.msdn.com/b/oldnewthing/archive/2010/07/30/10044061.aspx
  // http://www.drdobbs.com/visual-c-exception-handling-instrumentat/184416600
  // http://www.openrce.org/articles/full_view/21

  assert_true(record->number_parameters == 3);
  assert_true(record->exception_information[0] == 0x19930520);

  auto thrown_ptr = record->exception_information[1];
  auto thrown = REX_KERNEL_MEMORY()->TranslateVirtual(thrown_ptr);
  auto vftable_ptr = *reinterpret_cast<rex::be<uint32_t>*>(thrown);

  auto throw_info_ptr = record->exception_information[2];
  auto throw_info = REX_KERNEL_MEMORY()->TranslateVirtual<x_s__ThrowInfo*>(throw_info_ptr);
  auto catchable_types = REX_KERNEL_MEMORY()->TranslateVirtual<x_s__CatchableTypeArray*>(
      throw_info->catchable_type_array_ptr);

  rex::debug::Break();
}

void RtlRaiseException_entry(ppc_ptr_t<X_EXCEPTION_RECORD> record) {
  switch (record->code) {
    case 0x406D1388: {
      HandleSetThreadName(record);
      return;
    }
    case 0xE06D7363: {
      HandleCppException(record);
      return;
    }
  }

  // Not handled: log what was raised so a crash is attributable, then break.
  // Adapted from rexglue-sdk d631595 (BSD-3-Clause).
  const uint32_t count = std::min<uint32_t>(
      record->number_parameters, static_cast<uint32_t>(std::size(record->exception_information)));
  std::string parameters;
  for (uint32_t i = 0; i < count; ++i) {
    parameters += std::format("{}{:08X}", i ? " " : "",
                              static_cast<uint32_t>(record->exception_information[i]));
  }
  REXKRNL_ERROR("RtlRaiseException: code {:08X}, flags {:08X}, address {:08X}, parameters [{}]",
                static_cast<uint32_t>(record->code), static_cast<uint32_t>(record->exception_flags),
                static_cast<uint32_t>(record->exception_address), parameters);

  // TODO(benvanik): unwinding.
  // This is going to suck.
  rex::debug::Break();
}

// Raises a non-continuable exception carrying `status` (stub-noref research spec row RtlRaiseStatus).
void RtlRaiseStatus_entry(u32 status) {
  auto* memory = REX_KERNEL_MEMORY();
  uint32_t address = memory->SystemHeapAlloc(sizeof(X_EXCEPTION_RECORD));
  auto* record = memory->TranslateVirtual<X_EXCEPTION_RECORD*>(address);
  std::memset(record, 0, sizeof(*record));
  record->code = status;
  record->exception_flags = 1;  // EXCEPTION_NONCONTINUABLE
  RtlRaiseException_entry(ppc_ptr_t<X_EXCEPTION_RECORD>(record, address));
  memory->SystemHeapFree(address);
}

// Logs the failed assertion and continues (a retail kernel would break into the debugger).
void RtlAssert_entry(u32 expr, u32 file, u32 line, u32 msg) {
  auto str = [](uint32_t a) {
    return a ? std::string(REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(a)) : std::string();
  };
  REXKRNL_WARN("ASSERT: {} ({}:{}) {}", str(expr), str(file), line, str(msg));
}

void KeBugCheckEx_entry(u32 code, u32 param1, u32 param2, u32 param3, u32 param4) {
  REXKRNL_DEBUG("*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})", code, param1, param2,
                param3, param4);
  fflush(stdout);
  rex::debug::Break();
  assert_always();
}

void KeBugCheck_entry(u32 code) {
  KeBugCheckEx_entry(code, 0, 0, 0, 0);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__DbgBreakPoint, rex::kernel::xboxkrnl::DbgBreakPoint_entry)
REX_EXPORT(__imp__RtlRaiseException, rex::kernel::xboxkrnl::RtlRaiseException_entry)
REX_EXPORT(__imp__RtlRaiseStatus, rex::kernel::xboxkrnl::RtlRaiseStatus_entry)
REX_EXPORT(__imp__KeBugCheckEx, rex::kernel::xboxkrnl::KeBugCheckEx_entry)
REX_EXPORT(__imp__KeBugCheck, rex::kernel::xboxkrnl::KeBugCheck_entry)

REX_EXPORT_QUIET_PASSTHROUGH(__imp__DbgBreakPointWithStatus);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_RETURN(__imp__DbgPrompt, 0);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_PASSTHROUGH(__imp__DbgLoadImageSymbols);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_PASSTHROUGH(__imp__DbgUnLoadImageSymbols);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_PASSTHROUGH(__imp__DmPrintData);  // round-2 spec: deliberate return (behaviour unknown/not modelled)

REX_EXPORT_QUIET_RETURN(__imp__DumpGetRawDumpInfo, 0xC0000002);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_RETURN(__imp__DumpRegisterDedicatedDataBlock, 0);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_RETURN(__imp__DumpSetCollectionFacility, 0);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_RETURN(__imp__DumpUpdateDumpSettings, 0);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_RETURN(__imp__DumpWriteDump, 0xC0000002);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT_QUIET_RETURN(__imp__DumpXitThread, 0);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
REX_EXPORT(__imp__RtlAssert, rex::kernel::xboxkrnl::RtlAssert_entry)
REX_EXPORT_QUIET_PASSTHROUGH(__imp__RtlRip);  // round-2 spec: deliberate return (behaviour unknown/not modelled)
