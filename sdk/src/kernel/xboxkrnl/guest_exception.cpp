/**
 * @file        kernel/xboxkrnl/guest_exception.cpp
 * @brief       Guest structured exception dispatch (see guest_exception.h)
 *
 * @license     BSD 3-Clause License
 *
 * @remarks     CSpecificHandler follows Xenia Canary f21fc2dc4
 *              __C_specific_handler_entry (BSD-3-Clause); the rest is ours.
 */

#include <rex/kernel/xboxkrnl/guest_exception.h>

#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/abi_poison.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

REXCVAR_DEFINE_BOOL(guest_exception_dispatch, false, "Kernel",
                    "Dispatch guest RtlRaiseException/RtlUnwind to the title's .pdata handlers "
                    "(needs code generated with exception frames; off = log and break as before)");

namespace rex::kernel::xboxkrnl::guest_eh {

namespace {

// Guest-visible structures handed to a handler, carved below the caller's r1.
struct Scratch {
  X_CONTEXT context;
  X_DISPATCHER_CONTEXT dispatcher_context;
  be<uint32_t> exception_pointers[2];
  X_EXCEPTION_RECORD unwind_record;
};
constexpr uint32_t kStackGap = 0x100;  // room the guest callee may write above its own frame

uint32_t CarveScratch(const PPCContext& ctx) {
  return (ctx.r1.u32 - kStackGap - static_cast<uint32_t>(sizeof(Scratch))) & ~0xFu;
}

uint32_t HandlerOf(const Env& env, uint32_t function_entry) {
  auto* entry = env.at<X_RUNTIME_FUNCTION>(function_entry);
  if ((entry->flags >> 30) != kEntryKindHandler)
    return 0;
  return *env.at<be<uint32_t>>(entry->begin_address - 8);
}

// Runs a guest routine with the owner's non-volatiles in ctx and the stack
// below the scratch area. Returns r3. If the routine unwinds (throws), ctx is
// left for the target's catch, which resets r1 and r3.
uint32_t CallFor(const Env& env, PPCContext& ctx, const HandlerFrame* owner, uint32_t scratch,
                 uint32_t function, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                 uint32_t r12 = 0) {
  const PPCContext saved = ctx;
  if (owner) {
    for (int i = 0; i < 18; ++i) {
      if (owner->gpr[i])
        ctx.*rex::ppc::kGprMembers[14 + i] = *owner->gpr[i];
      if (owner->fpr[i])
        (&ctx.f14)[i] = *owner->fpr[i];
    }
  }
  ctx.r1.u64 = scratch - kStackGap;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.r12.u64 = r12;
  env.call(ctx, function);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(guest_exception_dispatch);
}

Env& KernelEnv() {
  static Env env{REX_KERNEL_MEMORY()->virtual_membase(), [](PPCContext& ctx, uint32_t address) {
                   auto* fn = REX_KERNEL_STATE()->function_dispatcher()->GetFunction(address);
                   if (!fn) {
                     REXKRNL_ERROR("guest exception: no function at {:08X}", address);
                     ctx.r3.u64 = kExceptionContinueSearch;
                     return;
                   }
                   fn(ctx, REX_KERNEL_MEMORY()->virtual_membase());
                 }};
  return env;
}

void CaptureContext(const PPCContext& in, X_CONTEXT* out) {
  std::memset(out, 0, sizeof(X_CONTEXT));
  out->context_flags = 0x7;  // control | floating point | integer (no vector)
  out->msr = in.msr;
  out->iar = static_cast<uint32_t>(in.lr);
  out->lr = static_cast<uint32_t>(in.lr);
  out->ctr = in.ctr.u64;
  for (int i = 0; i < 32; ++i) {
    out->gpr[i] = (in.*rex::ppc::kGprMembers[i]).u64;  // not in index order
    out->fpr[i] = (&in.f0)[i].u64;
  }
  uint32_t cr = 0;
  const PPCCRRegister* fields[8] = {&in.cr0, &in.cr1, &in.cr2, &in.cr3,
                                    &in.cr4, &in.cr5, &in.cr6, &in.cr7};
  for (int i = 0; i < 8; ++i)
    cr |= fields[i]->raw() << (28 - 4 * i);
  out->cr = cr;
  out->xer = (uint32_t(in.xer.so) << 31) | (uint32_t(in.xer.ov) << 30) | (uint32_t(in.xer.ca) << 29);
}

bool Dispatch(const Env& env, PPCContext& ctx, uint32_t record_address) {
  auto* record = env.at<X_EXCEPTION_RECORD>(record_address);
  const uint32_t scratch = CarveScratch(ctx);
  auto* s = env.at<Scratch>(scratch);
  const uint32_t context_address = scratch + offsetof(Scratch, context);
  const uint32_t dc_address = scratch + offsetof(Scratch, dispatcher_context);
  CaptureContext(ctx, &s->context);
  record->exception_flags = record->exception_flags & ~kExceptionUnwind;

  for (const HandlerFrame* f = tls_handler_frames; f; f = f->prev) {
    const uint32_t handler = f->establisher ? HandlerOf(env, f->function_entry) : 0;
    if (!handler)
      continue;
    s->dispatcher_context.control_pc = f->control_pc;
    s->dispatcher_context.function_entry = f->function_entry;
    s->dispatcher_context.establisher_frame = f->establisher;
    s->dispatcher_context.context_record = context_address;
    const uint32_t disposition = CallFor(env, ctx, f, scratch, handler, record_address,
                                         f->establisher, context_address, dc_address);
    if (disposition == kExceptionContinueExecution) {
      if (record->exception_flags & kExceptionNoncontinuable) {
        REXKRNL_ERROR("guest exception {:08X}: handler {:08X} continued a noncontinuable raise",
                      static_cast<uint32_t>(record->code), handler);
        return false;
      }
      return true;
    }
    if (disposition != kExceptionContinueSearch)
      REXKRNL_WARN("guest exception {:08X}: handler {:08X} returned disposition {}",
                   static_cast<uint32_t>(record->code), handler, disposition);
  }
  return false;
}

bool Unwind(const Env& env, PPCContext& ctx, uint32_t target_frame, uint32_t target_ip,
            uint32_t record_address, uint64_t return_value) {
  const HandlerFrame* target = nullptr;
  for (const HandlerFrame* f = tls_handler_frames; f && !target; f = f->prev) {
    if (f->establisher && f->establisher == target_frame)
      target = f;
  }
  if (!target) {
    // Exit unwinds (target 0) and frames from code generated without
    // exception frames end here; the caller carries on as RtlUnwind's return.
    REXKRNL_ERROR("RtlUnwind: no registered frame {:08X} (target ip {:08X})", target_frame,
                  target_ip);
    return false;
  }

  const uint32_t scratch = CarveScratch(ctx);
  auto* s = env.at<Scratch>(scratch);
  if (!record_address) {
    record_address = scratch + offsetof(Scratch, unwind_record);
    std::memset(&s->unwind_record, 0, sizeof(s->unwind_record));
    s->unwind_record.code = kStatusUnwind;
    s->unwind_record.exception_address = static_cast<uint32_t>(ctx.lr);
  }
  auto* record = env.at<X_EXCEPTION_RECORD>(record_address);
  const uint32_t context_address = scratch + offsetof(Scratch, context);
  const uint32_t dc_address = scratch + offsetof(Scratch, dispatcher_context);
  CaptureContext(ctx, &s->context);
  s->context.iar = target_ip;

  for (const HandlerFrame* f = tls_handler_frames;; f = f->prev) {
    const uint32_t handler = f->establisher ? HandlerOf(env, f->function_entry) : 0;
    if (handler) {
      record->exception_flags = (record->exception_flags & ~kExceptionUnwind) |
                                kExceptionUnwinding |
                                (f == target ? kExceptionTargetUnwind : 0);
      s->dispatcher_context.control_pc = f->control_pc;
      s->dispatcher_context.function_entry = f->function_entry;
      s->dispatcher_context.establisher_frame = f->establisher;
      s->dispatcher_context.context_record = context_address;
      CallFor(env, ctx, f, scratch, handler, record_address, f->establisher, context_address,
              dc_address);
    }
    if (f == target)
      break;
  }
  // The C++ runtime's "unwind and come back here" form: target_ip is RtlUnwind's own
  // return address. It then restores a context itself (ResumeContext).
  if (target_ip == static_cast<uint32_t>(ctx.lr))
    return true;
  throw GuestUnwind{target, target_ip, return_value};
}

void ResumeContext(const Env& env, PPCContext& ctx, uint32_t context_address) {
  const auto* context = env.at<X_CONTEXT>(context_address);
  const auto r1 = static_cast<uint32_t>(context->gpr[1]);
  for (const HandlerFrame* f = tls_handler_frames; f; f = f->prev) {
    if (f->establisher && f->establisher == r1)
      throw GuestUnwind{f, context->iar, context->gpr[3]};
  }
  REXKRNL_ERROR("guest exception: no registered frame {:08X} to resume at {:08X} (lr {:08X})", r1,
                static_cast<uint32_t>(context->iar), static_cast<uint32_t>(ctx.lr));
  std::abort();
}

uint32_t CSpecificHandler(const Env& env, PPCContext& ctx, uint32_t record_address,
                          uint32_t establisher_frame, uint32_t context_address,
                          uint32_t dispatcher_context_address) {
  struct X_SCOPE_RECORD {
    be<uint32_t> begin_address;
    be<uint32_t> end_address;
    be<uint32_t> handler_address;
    be<uint32_t> jump_target;
  };
  auto* record = env.at<X_EXCEPTION_RECORD>(record_address);
  auto* context = env.at<X_CONTEXT>(context_address);
  auto* dc = env.at<X_DISPATCHER_CONTEXT>(dispatcher_context_address);
  const uint32_t control_pc = dc->control_pc;
  const uint32_t begin = env.at<X_RUNTIME_FUNCTION>(dc->function_entry)->begin_address;
  const uint32_t scope_table = *env.at<be<uint32_t>>(begin - 4);
  const uint32_t scope_count = *env.at<be<uint32_t>>(scope_table);
  auto* scopes = env.at<X_SCOPE_RECORD>(scope_table + 4);

  const uint32_t scratch = CarveScratch(ctx);
  auto* s = env.at<Scratch>(scratch);
  // The owner's non-volatiles are already in ctx (CallFor loaded them).

  if (!(record->exception_flags & kExceptionUnwind)) {
    // Dispatching: run the filters of the __except scopes around the call.
    const uint32_t pointers = scratch + offsetof(Scratch, exception_pointers);
    s->exception_pointers[0] = record_address;
    s->exception_pointers[1] = context_address;
    for (uint32_t i = 0; i < scope_count; ++i) {
      const X_SCOPE_RECORD& scope = scopes[i];
      if (control_pc < scope.begin_address || control_pc >= scope.end_address ||
          !scope.jump_target)
        continue;
      const int32_t value = static_cast<int32_t>(CallFor(env, ctx, nullptr, scratch,
                                                         scope.handler_address, pointers,
                                                         scope.handler_address,
                                                         establisher_frame, 0, establisher_frame));
      if (value < 0)
        return kExceptionContinueExecution;
      if (value > 0) {
        // Continue at the __except block with the code in r3 (throws on success).
        Unwind(env, ctx, establisher_frame, scope.jump_target, record_address, record->code);
      }
    }
  } else {
    // Unwinding: run the __finally blocks of the scopes being left.
    const uint32_t target_pc = context->iar;
    for (uint32_t i = 0; i < scope_count; ++i) {
      const X_SCOPE_RECORD& scope = scopes[i];
      if (control_pc < scope.begin_address || control_pc >= scope.end_address)
        continue;
      if (target_pc >= scope.begin_address && target_pc <= scope.end_address)
        break;
      if (scope.jump_target) {
        if (target_pc == scope.jump_target)
          break;
        continue;
      }
      // Past the scope, so a colliding unwind doesn't rerun it.
      dc->control_pc = scope.end_address + 4;
      CallFor(env, ctx, nullptr, scratch, scope.handler_address, 1 /* abnormal */,
              scope.handler_address, establisher_frame, 0, establisher_frame);
    }
  }
  return kExceptionContinueSearch;
}

void BadResume(uint32_t function_entry, uint32_t target_ip) {
  REXKRNL_ERROR("guest exception: resume {:08X} is not a continuation label of the function at "
                ".pdata entry {:08X} (regenerate with exception frames)",
                target_ip, function_entry);
  std::abort();
}

}  // namespace rex::kernel::xboxkrnl::guest_eh
