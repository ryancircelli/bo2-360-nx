/**
 * @file        rex/ppc/abi_poison.h
 * @brief       PowerPC ABI checks around guest calls, for checking builds
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * The generator emits REX_ABI_POISON_PRE and REX_ABI_POISON_POST around every
 * direct and indirect guest call when it runs with abi_poison_checks. Before
 * the call the caller snapshots the registers the PowerPC ABI preserves (r1,
 * r2, r13, r14-r31, f14-f31, v14-v31, v64-v127); after it, the snapshot is
 * compared with ctx and any difference is logged, then the volatile registers
 * the ABI leaves dead at a return (r0, r5-r12, f0, f5-f13, v0, v4-v13,
 * v32-v63) are overwritten with poison. A caller that still reads one, or a
 * callee that reads a volatile register it was not passed, changes behaviour
 * on the route corpus, which the save hash, simulation-time and image checks
 * catch. r3, r4, f1-f4 and v1-v3 may carry results and are left alone; cr2-cr4
 * are preserved too, but cr_as_local writes them back right before the call
 * line (after the snapshot), so they are not compared.
 *
 * This is the static check a generator needs before it may keep guest
 * registers in host locals synchronized by the calling convention: an ABI
 * violation would otherwise corrupt state silently.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace rex::ppc {

struct AbiPoisonSnapshot {
  uint32_t site;      // guest address of the call instruction
  uint64_t gpr[21];   // r1, r2, r13, r14..r31
  uint64_t fpr[18];   // f14..f31
  uint64_t vr_low;    // hash of v14..v31
  uint64_t vr_high;   // hash of v64..v127
};

inline std::atomic<uint64_t> g_abi_poison_calls{0};
inline std::atomic<uint64_t> g_abi_poison_mismatches{0};
inline std::atomic<uint64_t> g_abi_poison_dropped{0};

inline std::vector<AbiPoisonSnapshot>& AbiPoisonStack() {
  static thread_local std::vector<AbiPoisonSnapshot> stack;
  return stack;
}

// The general registers are not in index order in PPCContext (r3 comes
// first), so they are reached through member pointers; the floating-point and
// vector files are consecutive and walked from their first member.
inline constexpr PPCRegister(::PPCContext::* kGprMembers[32]) = {
    &::PPCContext::r0,  &::PPCContext::r1,  &::PPCContext::r2,  &::PPCContext::r3,
    &::PPCContext::r4,  &::PPCContext::r5,  &::PPCContext::r6,  &::PPCContext::r7,
    &::PPCContext::r8,  &::PPCContext::r9,  &::PPCContext::r10, &::PPCContext::r11,
    &::PPCContext::r12, &::PPCContext::r13, &::PPCContext::r14, &::PPCContext::r15,
    &::PPCContext::r16, &::PPCContext::r17, &::PPCContext::r18, &::PPCContext::r19,
    &::PPCContext::r20, &::PPCContext::r21, &::PPCContext::r22, &::PPCContext::r23,
    &::PPCContext::r24, &::PPCContext::r25, &::PPCContext::r26, &::PPCContext::r27,
    &::PPCContext::r28, &::PPCContext::r29, &::PPCContext::r30, &::PPCContext::r31,
};
static_assert(offsetof(::PPCContext, f31) - offsetof(::PPCContext, f0) == 31 * sizeof(PPCRegister),
              "floating-point registers are not consecutive");
static_assert(offsetof(::PPCContext, v127) - offsetof(::PPCContext, v0) ==
                  127 * sizeof(PPCVRegister),
              "vector registers are not consecutive");

inline uint64_t AbiPoisonHashVectors(const PPCVRegister* first, size_t count) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < count; ++i) {
    h = (h ^ first[i].u64[0]) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ first[i].u64[1]) * 0x94D049BB133111EBull;
  }
  return h;
}

inline void AbiPoisonCapture(const ::PPCContext& ctx, AbiPoisonSnapshot& snapshot) {
  const PPCRegister* f = &ctx.f0;
  const PPCVRegister* v = &ctx.v0;
  snapshot.gpr[0] = ctx.r1.u64;
  snapshot.gpr[1] = ctx.r2.u64;
  snapshot.gpr[2] = ctx.r13.u64;
  for (int i = 14; i < 32; ++i) {
    snapshot.gpr[3 + i - 14] = (ctx.*kGprMembers[i]).u64;
    snapshot.fpr[i - 14] = f[i].u64;
  }
  snapshot.vr_low = AbiPoisonHashVectors(v + 14, 18);
  snapshot.vr_high = AbiPoisonHashVectors(v + 64, 64);
}

// A poison value names the register and the call site that left it:
// 0x9R'SSSSSS, R the register number (vector numbers above 15 wrap) and
// SSSSSS the call site's word offset from the image base 0x82000000. The
// 0x90000000 range is the XEX's 4 KB view, which the title's data never
// points into, so a poison that reaches an argument register is recognizable
// even after small arithmetic.
constexpr uint32_t kAbiPoisonImageBase = 0x82000000u;
constexpr uint32_t kAbiPoisonImageWords = 0x00800000u;

inline uint32_t AbiPoisonValue(int reg, uint32_t site) {
  return 0x90000000u | (uint32_t(reg & 15) << 24) | (((site - kAbiPoisonImageBase) >> 2) & 0x00FFFFFFu);
}

inline bool AbiPoisonLooksPoisoned(uint64_t value) {
  const uint32_t low = uint32_t(value);
  return (low >> 28) == 9 && (low & 0x00FFFFFFu) < kAbiPoisonImageWords &&
         (value >> 32) == 0;
}

inline std::atomic<uint64_t> g_abi_poison_dead_reads{0};

// REX_ABI_POISON_VOLATILES=0 keeps the non-volatile and LR slot checks but
// leaves the volatile registers alone after a call: FH1 reads a volatile
// register a callee left (sub_823F3048 passes r6 from its previous call), so
// with the poison on the race crashes within seconds (NL-0.1).
inline bool AbiPoisonVolatilesEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("REX_ABI_POISON_VOLATILES");
    return !(value && value[0] == '0');
  }();
  return enabled;
}

// host_local_lr (NEAR_NATIVE_RESEARCH.md NN-1.3): a function that keeps its LR
// save slot in the `rex_saved_lr` local writes this value to the guest slot
// instead of the return address, so a reader the generator's LR-slot scan
// missed (a tail-called epilogue, a stack walk) sees a poison, and every
// reload checks that the slot still holds it (a writer the scan missed).
inline std::atomic<uint64_t> g_abi_poison_lr_slot_mismatches{0};

inline uint32_t AbiPoisonLrSlotValue(uint32_t function) {
  return AbiPoisonValue(15, function);
}

inline void AbiPoisonLrSlotCheck(uint32_t function, uint32_t site, uint32_t value) {
  if (value == AbiPoisonLrSlotValue(function)) {
    return;
  }
  const uint64_t count = ++g_abi_poison_lr_slot_mismatches;
  if (count <= 500) {
    REXCPU_ERROR("ABI_POISON lr slot: {:08X} reload at {:08X} read {:08X}, not the poison {:08X}",
                 function, site, value, AbiPoisonLrSlotValue(function));
  }
}

inline void AbiPoisonPreCall(::PPCContext& ctx, uint32_t site) {
  // An argument that still holds a poison was read from a register the ABI
  // says is dead after an earlier call: the caller of this call site relies
  // on that earlier callee preserving it.
  for (int i = 3; i <= 10; ++i) {
    const uint64_t value = (ctx.*kGprMembers[i]).u64;
    if (AbiPoisonLooksPoisoned(value)) {
      const uint64_t count = ++g_abi_poison_dead_reads;
      if (count <= 500) {
        const uint32_t low = uint32_t(value);
        REXCPU_ERROR("ABI_POISON dead read: call at {:08X} passes r{} = {:08X}, which is r{} as left by "
                     "the call at {:08X}",
                     site, i, low, (low >> 24) & 15, kAbiPoisonImageBase + ((low & 0x00FFFFFFu) << 2));
      }
    }
  }
  auto& stack = AbiPoisonStack();
  stack.emplace_back();
  AbiPoisonSnapshot& snapshot = stack.back();
  snapshot.site = site;
  AbiPoisonCapture(ctx, snapshot);
}

inline void AbiPoisonReport(uint32_t site, const char* what, int index, uint64_t before,
                            uint64_t after) {
  const uint64_t count = ++g_abi_poison_mismatches;
  if (count <= 500) {
    REXCPU_ERROR("ABI_POISON mismatch site={:08X} {}{} before={:016X} after={:016X}", site, what,
                 index, before, after);
  }
}

inline void AbiPoisonPostCall(::PPCContext& ctx, uint32_t site) {
  auto& stack = AbiPoisonStack();
  // longjmp and a guest stack switch abandon host frames without running
  // their post-call line; snapshots that cannot be this call's are dropped.
  while (!stack.empty() && stack.back().site != site) {
    stack.pop_back();
    ++g_abi_poison_dropped;
  }
  if (!stack.empty()) {
    const AbiPoisonSnapshot before = stack.back();
    stack.pop_back();
    AbiPoisonSnapshot after;
    after.site = site;
    AbiPoisonCapture(ctx, after);
    static constexpr int kGprNames[21] = {1,  2,  13, 14, 15, 16, 17, 18, 19, 20, 21,
                                          22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    for (int i = 0; i < 21; ++i) {
      if (before.gpr[i] != after.gpr[i]) {
        AbiPoisonReport(site, "r", kGprNames[i], before.gpr[i], after.gpr[i]);
      }
    }
    for (int i = 0; i < 18; ++i) {
      if (before.fpr[i] != after.fpr[i]) {
        AbiPoisonReport(site, "f", 14 + i, before.fpr[i], after.fpr[i]);
      }
    }
    if (before.vr_low != after.vr_low) {
      AbiPoisonReport(site, "v14-v31 hash", 0, before.vr_low, after.vr_low);
    }
    if (before.vr_high != after.vr_high) {
      AbiPoisonReport(site, "v64-v127 hash", 0, before.vr_high, after.vr_high);
    }
  }
  const uint64_t calls = ++g_abi_poison_calls;
  if ((calls & ((uint64_t(1) << 24) - 1)) == 0) {
    REXCPU_WARN("ABI_POISON summary calls={} mismatches={} dead_reads={} dropped={} "
                "lr_slot_mismatches={}",
                calls, g_abi_poison_mismatches.load(), g_abi_poison_dead_reads.load(),
                g_abi_poison_dropped.load(), g_abi_poison_lr_slot_mismatches.load());
  }

  // Poison the volatile registers the ABI leaves dead at a return.
  if (!AbiPoisonVolatilesEnabled()) {
    return;
  }
  auto poison_for = [site](int reg) -> uint32_t { return AbiPoisonValue(reg, site); };
  PPCRegister* f = &ctx.f0;
  PPCVRegister* v = &ctx.v0;
  ctx.r0.u64 = poison_for(0);
  for (int i = 5; i <= 12; ++i) {
    (ctx.*kGprMembers[i]).u64 = poison_for(i);
  }
  // Quiet NaNs whose payload carries the same code.
  f[0].u64 = 0x7FF8000000000000ull | poison_for(0);
  for (int i = 5; i <= 13; ++i) {
    f[i].u64 = 0x7FF8000000000000ull | poison_for(i);
  }
  auto poison_vector = [&](int i) {
    v[i].u32[0] = v[i].u32[1] = v[i].u32[2] = v[i].u32[3] = poison_for(i);
  };
  poison_vector(0);
  for (int i = 4; i <= 13; ++i) {
    poison_vector(i);
  }
  for (int i = 32; i <= 63; ++i) {
    poison_vector(i);
  }
}

}  // namespace rex::ppc
