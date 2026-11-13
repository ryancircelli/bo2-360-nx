// External dependencies for the production validation functions extracted by the Python runner.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <rex/ppc/context.h>

namespace fault {
bool armed = false, persistent = false;
size_t attempt = 0, fail_on = 0, live = 0;
std::array<void*, 8> pointers{};
void Arm(size_t failure, bool keep_failing = false) {
  assert(live == 0);
  attempt = 0;
  fail_on = failure;
  persistent = keep_failing;
  armed = true;
}
void Disarm() { assert(live == 0); armed = false; }
}

void* operator new(size_t bytes) {
  if (fault::armed) {
    ++fault::attempt;
    if (fault::fail_on && (fault::attempt == fault::fail_on ||
                          (fault::persistent && fault::attempt > fault::fail_on))) throw std::bad_alloc();
  }
  void* p = std::malloc(bytes ? bytes : 1);
  if (!p) throw std::bad_alloc();
  if (fault::armed) {
    bool tracked = false;
    for (void*& slot : fault::pointers) {
      if (!slot) { slot = p; ++fault::live; tracked = true; break; }
    }
    assert(tracked);
  }
  return p;
}
void operator delete(void* p) noexcept {
  for (void*& slot : fault::pointers) {
    if (p && slot == p) { slot = nullptr; --fault::live; break; }
  }
  std::free(p);
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* p) noexcept { ::operator delete(p); }

bool bo2zm_native_audio = true, bo2zm_native_skinning = true, bo2zm_native_lzx = true;
bool bo2zm_native_audio_check_only = false;
#define REXCVAR_GET(name) (name)
#define REXCVAR_ALIAS(alias, name) static_assert(true)
unsigned warnings = 0, summaries = 0;
bool log_oom = false;
template <typename... Args> void Warn(const Args&...) {
  ++warnings;
  if (log_oom) {
    assert(fault::live == 0); // Recovery releases snapshots before it tries to log.
    fault::fail_on = fault::attempt + 1;
    std::vector<uint8_t> log_buffer(8); // The diagnostic itself also fails a real allocation.
  }
}
template <typename... Args> void Info(const Args&...) {}
#define REXLOG_WARN(...) Warn(__VA_ARGS__)
#define REXLOG_INFO(...) Info(__VA_ARGS__)
uint64_t NowNs() { static uint64_t ticks = 0; return ++ticks; }
void Summarize() { ++summaries; }
uint8_t* Host(uint8_t* base, uint32_t guest) { return base + guest; }

// PRODUCTION_VALIDATION_CODE

enum class Throw { kNone, kBadAlloc, kOther };
struct GameFailure {};
unsigned originals = 0, natives = 0, external_effects = 0;
Throw original_throw = Throw::kNone, native_throw = Throw::kNone;
bool mismatch_memory = false, mismatch_context = false;
PPCContext original_result{}, expected_input{};
std::array<uint8_t, 64> memory{}, original_memory{}, expected_input_memory{};

void Apply(PPCContext& ctx, uint8_t* base) {
  ++base[0];
  base[7] += 3;
  ctx.r3.u64 += 5;
  ctx.r7.u64 ^= 0x1234;
  ctx.f1.f64 += 0.5;
  ctx.lr += 4;
  ctx.fpscr.csr ^= uint32_t(PPCFPSCRRegister::FlushMask);
  ctx.fpscr.setcsr(ctx.fpscr.csr);
}
void ThrowIf(Throw kind) {
  if (kind == Throw::kBadAlloc) throw std::bad_alloc();
  if (kind == Throw::kOther) throw GameFailure();
}
void Original(PPCContext& ctx, uint8_t* base) {
  ++originals;
  ++external_effects; // This external side effect is not undone by snapshot restoration.
  assert(std::memcmp(&ctx, &expected_input, sizeof(ctx)) == 0);
  assert(std::memcmp(base, expected_input_memory.data(), expected_input_memory.size()) == 0);
  assert(((ctx.fpscr.csr ^ PPCFPSCRRegister::Platform::getcsr()) & PPCFPSCRRegister::GuestMask) == 0);
  Apply(ctx, base);
  std::memcpy(&original_result, &ctx, sizeof(ctx));
  std::memcpy(original_memory.data(), base, original_memory.size());
  ThrowIf(original_throw);
}
void Native(PPCContext& ctx, uint8_t* base) {
  ++natives;
  Apply(ctx, base);
  if (mismatch_memory || native_throw != Throw::kNone) base[0] = 99;
  if (mismatch_context || native_throw != Throw::kNone) ctx.r7.u64 = 999;
  ThrowIf(native_throw);
}
Regions Outputs(const PPCContext&, uint8_t*) {
  Regions rg;
  rg.Append(0, 16);
  return rg;
}
PPCContext Input() {
  fault::Disarm();
  originals = natives = external_effects = warnings = summaries = 0;
  original_throw = native_throw = Throw::kNone;
  log_oom = mismatch_memory = mismatch_context = false;
  t_checking = 0;
  memory.fill(0);
  memory[0] = 10;
  memory[7] = 17;
  PPCContext ctx{};
  ctx.r3.u64 = 17;
  ctx.r7.u64 = 23;
  ctx.f1.f64 = 1.25;
  ctx.lr = 0x12345678;
  ctx.fpscr.csr = ctx.fpscr.getcsr();
  std::memcpy(&expected_input, &ctx, sizeof(ctx));
  expected_input_memory = memory;
  return ctx;
}
void AssertOriginal(const PPCContext& ctx, bool check_host_fp = true) {
  assert(originals == 1 && external_effects == 1);
  assert(std::memcmp(&ctx, &original_result, sizeof(ctx)) == 0);
  assert(memory == original_memory);
  // Host exception-status flags can change during exception handling; guest rounding
  // and flush mode must still agree with the original's saved FPSCR.
  if (check_host_fp) {
    if (((ctx.fpscr.csr ^ PPCFPSCRRegister::Platform::getcsr()) & PPCFPSCRRegister::GuestMask) != 0) {
      std::fprintf(stderr, "FP saved=%x host=%x originals=%u natives=%u warnings=%u fail=%zu phasecounter=%zu\n",
                   ctx.fpscr.csr, PPCFPSCRRegister::Platform::getcsr(), originals, natives, warnings,
                   fault::fail_on, fault::attempt);
    }
    assert(((ctx.fpscr.csr ^ PPCFPSCRRegister::Platform::getcsr()) & PPCFPSCRRegister::GuestMask) == 0);
  }
}

int main() {
  // Both initial validation and periodic validation: fail each actual vector allocation,
  // with both transient/persistent allocation failure and a throwing OOM logger.
  for (int phase : {0, 1}) for (size_t failure : {size_t(1), size_t(2)}) {
    for (bool persistent : {false, true}) for (bool logger_fails : {false, true}) {
      PPCContext ctx = Input();
      Replacement affected{"affected"}, sibling{"sibling"};
      affected.phase.store(phase);
      log_oom = logger_fails;
      fault::Arm(failure, persistent);
      Call(affected, ctx, memory.data(), Original, Native, Outputs, true);
      AssertOriginal(ctx);
      assert(fault::attempt == failure + size_t(logger_fails) && fault::live == 0);
      assert(natives == 0 && warnings == 1 && summaries == 0 && t_checking == 0);
      assert(affected.phase.load() == 2 && affected.checked.load() == 0 && affected.timed.load() == 0);
      assert(sibling.phase.load() == 0);
      fault::Disarm();

      // Subsequent calls stay on the original even if allocations/logging would still fail.
      ctx = Input();
      log_oom = true;
      fault::Arm(1, true);
      Call(affected, ctx, memory.data(), Original, Native, Outputs, true);
      AssertOriginal(ctx);
      assert(fault::attempt == 0 && natives == 0 && warnings == 0);
      fault::Disarm();

      // A different replacement still validates normally and restores the original's outputs.
      ctx = Input();
      fault::Arm(0);
      Call(sibling, ctx, memory.data(), Original, Native, Outputs, true);
      AssertOriginal(ctx);
      assert(natives == 1 && sibling.checked.load() == 1 && sibling.phase.load() == 0);
      assert(fault::attempt == 2 && fault::live == 0);
      fault::Disarm();
    }
  }

  // Ordinary comparison, promotion, direct native calls and mismatch fallback still work.
  for (bool whole : {false, true}) {
    PPCContext ctx = Input();
    Replacement normal{"normal"};
    normal.for_apply = 1;
    Call(normal, ctx, memory.data(), Original, Native, Outputs, whole);
    AssertOriginal(ctx);
    assert(natives == 1 && normal.checked.load() == 1 && normal.phase.load() == 1);
    ctx = Input();
    Call(normal, ctx, memory.data(), Original, Native, Outputs, whole);
    assert(originals == 0 && natives == 1 && normal.checked.load() == 1);
  }
  for (bool context_only : {false, true}) {
    PPCContext ctx = Input();
    Replacement different{"different"};
    mismatch_memory = !context_only;
    mismatch_context = context_only;
    Call(different, ctx, memory.data(), Original, Native, Outputs, true);
    AssertOriginal(ctx);
    assert(natives == 1 && different.phase.load() == 2 && different.checked.load() == 0);
  }

  // Exceptions from the original/native function are outside the allocation catches.
  // Do not re-run the original or restore guest state after either function throws.
  for (bool in_native : {false, true}) for (Throw kind : {Throw::kBadAlloc, Throw::kOther}) {
    PPCContext ctx = Input();
    Replacement throwing{"throwing"};
    if (in_native) native_throw = kind; else original_throw = kind;
    bool caught = false;
    try { Call(throwing, ctx, memory.data(), Original, Native, Outputs, true); }
    catch (const std::bad_alloc&) { assert(kind == Throw::kBadAlloc); caught = true; }
    catch (const GameFailure&) { assert(kind == Throw::kOther); caught = true; }
    assert(caught && originals == 1 && external_effects == 1);
    assert(natives == unsigned(in_native) && throwing.phase.load() == 0 && warnings == 0);
    if (in_native) assert(memory[0] == 99 && ctx.r7.u64 == 999);
    else AssertOriginal(ctx, false);
    // The production exception path is otherwise unchanged; it is fatal to the runtime.
    t_checking = 0;
  }
  std::puts("Actual audio validation wrapper: snapshot OOM, single original call, outputs, logging, isolation and exceptions pass");
}
