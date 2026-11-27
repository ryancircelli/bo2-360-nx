/**
 * @file        rex/kernel/xboxkrnl/guest_exception.h
 * @brief       Guest structured exception dispatch for recompiled code
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     Structure layouts, flag values and the __C_specific_handler walk
 *              follow Xenia Canary f21fc2dc4 (exception_dispatch.h/.cc,
 *              xboxkrnl_exception.cc; BSD-3-Clause, Ben Vanik / Xenia Canary).
 *              Frame discovery and resume are our own: see
 *              docs/guest-exception-dispatch.md.
 *
 * Why not a guest stack walk: recompiled functions keep r14-r31/f14-f31 in host
 * locals and elide the __savegprlr helpers, so the guest stack holds neither
 * the saved non-volatiles nor (for helper-saving functions) the return address.
 * A virtual unwind over .pdata cannot recover the caller state. Instead every
 * function with a .pdata language handler registers a HandlerFrame (codegen,
 * REX_EH_* below); dispatch walks that chain, and RtlUnwind resumes the target
 * by throwing GuestUnwind, which the target function catches and turns into a
 * goto to the continuation label. Functions without a handler cost nothing.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <rex/ppc/context.h>
#include <rex/system/xexception.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl::guest_eh {

// Exception record flags.
inline constexpr uint32_t kExceptionNoncontinuable = 0x01;
inline constexpr uint32_t kExceptionUnwinding = 0x02;
inline constexpr uint32_t kExceptionExitUnwind = 0x04;
inline constexpr uint32_t kExceptionTargetUnwind = 0x20;
inline constexpr uint32_t kExceptionUnwind = 0x66;  // unwinding|exit|target|collided

// Handler dispositions.
inline constexpr uint32_t kExceptionContinueExecution = 0;
inline constexpr uint32_t kExceptionContinueSearch = 1;

inline constexpr uint32_t kStatusNoncontinuableException = 0xC0000025;
inline constexpr uint32_t kStatusUnwind = 0xC0000027;

// .pdata flags: prolog length (8), function length in words (22), kind (2).
inline constexpr uint32_t kEntryKindHandler = 3;

struct X_RUNTIME_FUNCTION {
  be<uint32_t> begin_address;
  be<uint32_t> flags;
};
static_assert(sizeof(X_RUNTIME_FUNCTION) == 8);

struct X_DISPATCHER_CONTEXT {
  be<uint32_t> control_pc;
  be<uint32_t> function_entry;     // guest address of the X_RUNTIME_FUNCTION
  be<uint32_t> establisher_frame;  // r1 after the prolog
  be<uint32_t> context_record;
};
static_assert(sizeof(X_DISPATCHER_CONTEXT) == 0x10);

struct X_CONTEXT {
  be<uint32_t> context_flags;
  be<uint32_t> msr;
  be<uint32_t> iar;
  be<uint32_t> lr;
  be<uint64_t> ctr;
  be<uint64_t> gpr[32];
  be<uint32_t> cr;
  be<uint32_t> xer;
  be<uint64_t> fpscr;
  be<uint64_t> fpr[32];
  be<uint32_t> user_mode_control;
  be<uint32_t> fill;
  be<uint32_t> vscr[4];
  be<uint32_t> vr[128][4];
};
static_assert(sizeof(X_CONTEXT) == 0xA40);
static_assert(offsetof(X_CONTEXT, gpr) == 0x18);
static_assert(offsetof(X_CONTEXT, cr) == 0x118);
static_assert(offsetof(X_CONTEXT, fpr) == 0x128);

using X_EXCEPTION_RECORD = ::rex::system::X_EXCEPTION_RECORD;

// One live activation of a guest function that has a .pdata language handler.
// Codegen declares it at function entry; the destructor keeps the chain right
// when the host stack unwinds through the function.
struct HandlerFrame;
inline thread_local HandlerFrame* tls_handler_frames = nullptr;

struct HandlerFrame {
  HandlerFrame* prev;
  uint32_t function_entry;   // guest address of this function's .pdata entry
  uint32_t establisher = 0;  // r1 after the prolog; 0 while still in it
  uint32_t control_pc = 0;   // address of the call the function is inside
  // r14-r31 and f14-f31 as localized by codegen (null: the value lives in ctx).
  // Handlers and their funclets read the owner's non-volatiles through ctx.
  PPCRegister* gpr[18] = {};
  PPCRegister* fpr[18] = {};

  explicit HandlerFrame(uint32_t entry) : prev(tls_handler_frames), function_entry(entry) {
    tls_handler_frames = this;
  }
  ~HandlerFrame() { tls_handler_frames = prev; }
  HandlerFrame(const HandlerFrame&) = delete;
  HandlerFrame& operator=(const HandlerFrame&) = delete;
};

// Thrown by RtlUnwind once every frame above the target has run its unwind
// handler. Only the target's own catch takes it (frame identity), so it passes
// through every other recompiled and host frame.
struct GuestUnwind {
  const HandlerFrame* frame;
  uint32_t target_ip;
  uint64_t r3;
};

// What the dispatcher needs from the runtime; tests pass a synthetic one.
struct Env {
  uint8_t* membase;
  std::function<void(PPCContext&, uint32_t)> call;  // run the guest function at an address
  template <typename T>
  T* at(uint32_t address) const {
    return reinterpret_cast<T*>(membase + address);
  }
};

// Runtime switch (cvar guest_exception_dispatch, default off).
bool Enabled();
// Production environment: kernel memory + the function dispatcher.
Env& KernelEnv();

void CaptureContext(const PPCContext& ctx, X_CONTEXT* out);

// RtlRaiseException: offers the record to each registered frame's handler,
// innermost first. True when a handler returned ContinueExecution (the raise
// returns). A handler that takes the exception calls RtlUnwind, which throws
// and never comes back here. False when nobody took it.
bool Dispatch(const Env& env, PPCContext& ctx, uint32_t record_address);

// RtlUnwind: runs the unwind handlers of every frame from the innermost to the
// one whose establisher frame is target_frame, then throws GuestUnwind for it.
// Returns false when no registered frame matches, and true (after the
// handlers ran) when target_ip is RtlUnwind's own return address: the C++
// runtime unwinds that way, then jumps with ResumeContext.
bool Unwind(const Env& env, PPCContext& ctx, uint32_t target_frame, uint32_t target_ip,
            uint32_t record_address, uint64_t return_value);

// __C_specific_handler(record, establisher, context, dispatcher_context).
uint32_t CSpecificHandler(const Env& env, PPCContext& ctx, uint32_t record_address,
                          uint32_t establisher_frame, uint32_t context_address,
                          uint32_t dispatcher_context_address);

// The C++ runtime's restore-context helper (FH1 sub_830FC89C: loads every
// register from an X_CONTEXT, then bctr to its Iar): resumes the registered
// frame whose establisher is the context's r1 at its Iar, r3 from the context.
// Codegen replaces the helper with a call to this.
[[noreturn]] void ResumeContext(const Env& env, PPCContext& ctx, uint32_t context_address);

[[noreturn]] void BadResume(uint32_t function_entry, uint32_t target_ip);

}  // namespace rex::kernel::xboxkrnl::guest_eh

// ---- Codegen protocol (emitted only for functions with a .pdata handler) ----
// At entry:            REX_EH_FRAME(pdata_entry_address);
//                      __ehf.gpr[31 - 14] = &r31; ...   (each localized non-volatile)
// At the prolog end:   REX_EH_ESTABLISH();
// Before each bl:      REX_EH_CALLSITE(bl_address);
// Body:                REX_EH_BODY_BEGIN { case 0x82xxxxxx: goto loc_82xxxxxx; ... }
//                      ... the usual body ...
//                      REX_EH_BODY_END
#define REX_EH_FRAME(entry) ::rex::kernel::xboxkrnl::guest_eh::HandlerFrame __ehf(entry)
#define REX_EH_ESTABLISH() (__ehf.establisher = ctx.r1.u32)
#define REX_EH_CALLSITE(pc) (__ehf.control_pc = (pc))
#define REX_EH_BODY_BEGIN        \
  uint32_t __eh_resume = 0;      \
  for (;;) {                     \
    try {                        \
      switch (__eh_resume) {     \
        case 0:                  \
          break;                 \
        default:                 \
          ::rex::kernel::xboxkrnl::guest_eh::BadResume(__ehf.function_entry, __eh_resume);
#define REX_EH_BODY_END                                                         \
  }                                                                             \
  catch (const ::rex::kernel::xboxkrnl::guest_eh::GuestUnwind& __u) {           \
    if (__u.frame != &__ehf)                                                    \
      throw;                                                                    \
    ctx.r1.u64 = __ehf.establisher;                                             \
    ctx.r3.u64 = __u.r3;                                                        \
    __eh_resume = __u.target_ip;                                                \
  }                                                                             \
  }
