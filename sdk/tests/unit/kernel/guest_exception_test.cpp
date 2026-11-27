// Guest exception dispatch over a synthetic module: guest functions are host
// functions written the way codegen emits a function with a .pdata handler
// (REX_EH_* protocol), .pdata entries / scope tables live in a fake guest
// memory, and a map stands in for the function table.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <map>
#include <vector>

#include <rex/kernel/xboxkrnl/guest_exception.h>

using namespace rex::kernel::xboxkrnl::guest_eh;

namespace {

constexpr uint32_t kPdataA = 0x1000, kPdataC = 0x1008, kPdataE = 0x1010;
constexpr uint32_t kBeginA = 0x2010, kBeginC = 0x3010, kBeginE = 0x2810;
constexpr uint32_t kCSpecificThunk = 0x4000, kCxxHandler = 0x4100, kCrtHandler = 0x4200;
constexpr uint32_t kScopesA = 0x5000, kScopesC = 0x5100;
constexpr uint32_t kFilter = 0x6000, kFinally = 0x6100, kCatchFunclet = 0x6200;
constexpr uint32_t kRecord = 0x7000, kStackTop = 0xF000;
constexpr uint32_t kExceptA = 0x2030, kContinueE = 0x2834;
constexpr uint32_t kCode = 0xC0000005, kCxxCode = 0xE06D7363;

std::vector<uint8_t> mem;
std::map<uint32_t, PPCFunc*> functions;
Env env;

struct Observed {
  int32_t filter_result = 1;
  int filter_calls = 0, finally_calls = 0, finally_abnormal = 0;
  uint64_t filter_r31 = 0, funclet_r31 = 0;
  int unhandled = 0, continued = 0, normal_return = 0;
  int cxx_unwind_calls = 0, cxx_target_unwinds = 0;
  uint32_t resumed_r3 = 0, resumed_r1 = 0, establisher = 0;
  uint64_t resumed_r31 = 0;
  bool resumed = false;
} obs;

void put32(uint32_t a, uint32_t v) { *env.at<rex::be<uint32_t>>(a) = v; }

void pdata(uint32_t entry, uint32_t begin, uint32_t handler, uint32_t data) {
  put32(entry, begin);
  put32(entry + 4, (3u << 30) | (0x40u << 8) | 2);  // kind 3 (handler), 64 words, prolog 2
  put32(begin - 8, handler);
  put32(begin - 4, data);
}

void scope(uint32_t table, uint32_t b, uint32_t e, uint32_t handler, uint32_t jump) {
  put32(table, 1);
  put32(table + 4, b);
  put32(table + 8, e);
  put32(table + 12, handler);
  put32(table + 16, jump);
}

// D: raises (RtlRaiseException as the kernel export does it).
void fnD(PPCContext& ctx, uint8_t*) {
  if (Dispatch(env, ctx, kRecord))
    ++obs.continued;
  else
    ++obs.unhandled;
}

// C: an unwind-only function: a __finally around its call to D.
void fnC(PPCContext& ctx, uint8_t* base) {
  REX_EH_FRAME(kPdataC);
  ctx.r1.u32 -= 0x60;
  REX_EH_ESTABLISH();
  REX_EH_CALLSITE(kBeginC + 0x14);
  fnD(ctx, base);
  ctx.r1.u32 += 0x60;
}

// B: no handler at all, so no frame.
void fnB(PPCContext& ctx, uint8_t* base) {
  ctx.r1.u32 -= 0x40;
  fnC(ctx, base);
  ctx.r1.u32 += 0x40;
}

// A: __try { B(); } __except (filter) { ... } with r31 localized.
void fnA(PPCContext& ctx, uint8_t* base) {
  PPCRegister r31{};
  REX_EH_FRAME(kPdataA);
  __ehf.gpr[31 - 14] = &r31;
  ctx.r1.u32 -= 0x80;
  REX_EH_ESTABLISH();
  obs.establisher = ctx.r1.u32;
  r31.u64 = 0x1234;
  REX_EH_BODY_BEGIN
  case kExceptA:
    goto loc_except;
  }
  REX_EH_CALLSITE(kBeginA + 0x14);
  fnB(ctx, base);
  ++obs.normal_return;
  ctx.r1.u32 += 0x80;
  return;
loc_except:
  obs.resumed = true;
  obs.resumed_r3 = ctx.r3.u32;
  obs.resumed_r1 = ctx.r1.u32;
  obs.resumed_r31 = r31.u64;
  ctx.r1.u32 += 0x80;
  return;
  REX_EH_BODY_END
}

// E: try { B(); } catch (...) { funclet } with a C++-style language handler.
void fnE(PPCContext& ctx, uint8_t* base) {
  PPCRegister r31{};
  REX_EH_FRAME(kPdataE);
  __ehf.gpr[31 - 14] = &r31;
  ctx.r1.u32 -= 0x90;
  REX_EH_ESTABLISH();
  obs.establisher = ctx.r1.u32;
  r31.u64 = 0x5678;
  REX_EH_BODY_BEGIN
  case kContinueE:
    goto loc_continue;
  }
  REX_EH_CALLSITE(kBeginE + 0x10);
  fnB(ctx, base);
  ++obs.normal_return;
  ctx.r1.u32 += 0x90;
  return;
loc_continue:
  obs.resumed = true;
  obs.resumed_r3 = ctx.r3.u32;
  obs.resumed_r1 = ctx.r1.u32;
  obs.resumed_r31 = r31.u64;
  ctx.r1.u32 += 0x90;
  return;
  REX_EH_BODY_END
}

// The title's import thunk for __C_specific_handler.
void cSpecificThunk(PPCContext& ctx, uint8_t*) {
  ctx.r3.u64 = CSpecificHandler(env, ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

void filter(PPCContext& ctx, uint8_t*) {
  ++obs.filter_calls;
  const uint32_t record = *env.at<rex::be<uint32_t>>(ctx.r3.u32);  // EXCEPTION_POINTERS
  CHECK(*env.at<rex::be<uint32_t>>(record) == kCode);
  obs.filter_r31 = ctx.r31.u64;  // the owner's localized r31, handed over via ctx
  ctx.r3.s64 = obs.filter_result;
}

void finallyBlock(PPCContext& ctx, uint8_t*) {
  ++obs.finally_calls;
  obs.finally_abnormal += ctx.r3.u32 == 1;
}

void catchFunclet(PPCContext& ctx, uint8_t*) {
  obs.funclet_r31 = ctx.r31.u64;
  ctx.r3.u64 = kContinueE;
}

// A minimal __CxxFrameHandler: catches everything, as the CRT's does for catch(...).
void cxxHandler(PPCContext& ctx, uint8_t*) {
  const uint32_t record = ctx.r3.u32, establisher = ctx.r4.u32;
  auto* rec = env.at<X_EXCEPTION_RECORD>(record);
  if (rec->exception_flags & kExceptionUnwind) {
    ++obs.cxx_unwind_calls;
    obs.cxx_target_unwinds += (rec->exception_flags & kExceptionTargetUnwind) != 0;
    ctx.r3.u64 = kExceptionContinueSearch;
    return;
  }
  functions[kCatchFunclet](ctx, env.membase);
  const uint32_t continuation = ctx.r3.u32;
  Unwind(env, ctx, establisher, continuation, record, 0);
  FAIL("Unwind returned");
}

// The FH1 C++ runtime's way: unwind back to itself, run the funclet, then
// jump to the continuation through a restored context.
constexpr uint32_t kCrtReturn = 0x830FC9E4, kResumeContext = 0x8000;
void crtHandler(PPCContext& ctx, uint8_t*) {
  const uint32_t record = ctx.r3.u32, establisher = ctx.r4.u32;
  auto* rec = env.at<X_EXCEPTION_RECORD>(record);
  if (rec->exception_flags & kExceptionUnwind) {
    ++obs.cxx_unwind_calls;
    obs.cxx_target_unwinds += (rec->exception_flags & kExceptionTargetUnwind) != 0;
    ctx.r3.u64 = kExceptionContinueSearch;
    return;
  }
  ctx.lr = kCrtReturn;
  CHECK(Unwind(env, ctx, establisher, kCrtReturn, record, 0));
  functions[kCatchFunclet](ctx, env.membase);
  auto* context = env.at<X_CONTEXT>(kResumeContext);
  context->gpr[1] = establisher;
  context->gpr[3] = 7;
  context->iar = ctx.r3.u32;
  ResumeContext(env, ctx, kResumeContext);
}

void setup(uint32_t code) {
  mem.assign(0x10000, 0);
  env.membase = mem.data();
  env.call = [](PPCContext& ctx, uint32_t address) { functions.at(address)(ctx, env.membase); };
  functions = {{kCSpecificThunk, cSpecificThunk}, {kFilter, filter},
               {kFinally, finallyBlock},          {kCatchFunclet, catchFunclet},
               {kCxxHandler, cxxHandler},         {kCrtHandler, crtHandler}};
  pdata(kPdataA, kBeginA, kCSpecificThunk, kScopesA);
  pdata(kPdataC, kBeginC, kCSpecificThunk, kScopesC);
  pdata(kPdataE, kBeginE, kCxxHandler, 0x5200);
  scope(kScopesA, kBeginA + 0x10, kBeginA + 0x18, kFilter, kExceptA);
  scope(kScopesC, kBeginC + 0x10, kBeginC + 0x18, kFinally, 0);
  put32(kRecord, code);
  put32(kRecord + 4, 0);
  obs = {};
}

}  // namespace

TEST_CASE("SEH __except catches a raise three frames down", "[guest_eh]") {
  setup(kCode);
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  fnA(ctx, env.membase);
  CHECK(obs.resumed);
  CHECK(obs.normal_return == 0);
  CHECK(obs.filter_calls == 1);
  CHECK(obs.filter_r31 == 0x1234);
  CHECK(obs.finally_calls == 1);  // C's __finally ran during the unwind
  CHECK(obs.finally_abnormal == 1);
  CHECK(obs.resumed_r3 == kCode);
  CHECK(obs.resumed_r1 == obs.establisher);
  CHECK(obs.resumed_r31 == 0x1234);
  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(tls_handler_frames == nullptr);
}

TEST_CASE("filter declining leaves the raise unhandled", "[guest_eh]") {
  setup(kCode);
  obs.filter_result = 0;
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  fnA(ctx, env.membase);
  CHECK(obs.unhandled == 1);
  CHECK(obs.finally_calls == 0);
  CHECK(obs.normal_return == 1);
  CHECK(!obs.resumed);
  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(tls_handler_frames == nullptr);
}

TEST_CASE("filter returning -1 continues after the raise", "[guest_eh]") {
  setup(kCode);
  obs.filter_result = -1;
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  fnA(ctx, env.membase);
  CHECK(obs.continued == 1);
  CHECK(obs.normal_return == 1);
  CHECK(tls_handler_frames == nullptr);
}

TEST_CASE("C++-style handler: catch funclet, unwind, resume at the continuation", "[guest_eh]") {
  setup(kCxxCode);
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  fnE(ctx, env.membase);
  CHECK(obs.resumed);
  CHECK(obs.funclet_r31 == 0x5678);
  CHECK(obs.finally_calls == 1);
  CHECK(obs.cxx_unwind_calls == 1);  // only the target itself
  CHECK(obs.cxx_target_unwinds == 1);
  CHECK(obs.resumed_r3 == 0);
  CHECK(obs.resumed_r1 == obs.establisher);
  CHECK(obs.resumed_r31 == 0x5678);
  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(tls_handler_frames == nullptr);
}

TEST_CASE("no registered frames: raise unhandled, RtlUnwind returns", "[guest_eh]") {
  setup(kCode);
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  fnB(ctx, env.membase);  // C registers, but its __finally scope has no filter
  CHECK(obs.unhandled == 1);
  CHECK(!Unwind(env, ctx, 0x1234, 0, 0, 0));
  CHECK(tls_handler_frames == nullptr);
}

TEST_CASE("C++ runtime style: unwind to self, funclet, resume through a context", "[guest_eh]") {
  setup(kCxxCode);
  pdata(kPdataE, kBeginE, kCrtHandler, 0x5200);
  PPCContext ctx{};
  ctx.r1.u64 = kStackTop;
  fnE(ctx, env.membase);
  CHECK(obs.resumed);
  CHECK(obs.funclet_r31 == 0x5678);
  CHECK(obs.finally_calls == 1);
  CHECK(obs.cxx_target_unwinds == 1);
  CHECK(obs.resumed_r3 == 7);
  CHECK(obs.resumed_r1 == obs.establisher);
  CHECK(obs.resumed_r31 == 0x5678);
  CHECK(ctx.r1.u32 == kStackTop);
  CHECK(tls_handler_frames == nullptr);
}
