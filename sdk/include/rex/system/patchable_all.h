/**
 * @file        system/patchable_all.h
 * @brief       Test builds: every recompiled function gets a hot-swap slot
 *
 * Put ahead of the generated PCH by rexglue_patchable_all() (CMake option REXGLUE_PATCHABLE_ALL, default OFF). It
 * defines DEFINE_REX_FUNC before the generated header does, so each function's entry points (`sub_X`, and
 * `__imp__sub_X`, which codegen calls directly) tail-jump through a slot (rex/system/patch.h) to the body. A patch library can then replace any
 * function, not only the ones a game lists with REX_PATCHABLE. Game hooks (strong `sub_X` definitions) still win,
 * exactly as before, and their calls to `__imp__sub_X` reach a patch too. Cost: one load and an indirect tail jump per call, and one slot per function registered
 * at start-up, so it is for test builds only; shipped builds leave the option off and compile unchanged.
 */

#pragma once

#if defined(__APPLE__) && defined(__clang__)
#include <rex/system/patch.h>

// Every entry point goes through the slot: `__imp__X` (what codegen calls directly, and what hooks call as "the
// original") is the slot jump, `X` stays the weak stub hooks override, and the recompiled body moves to a
// file-local `rex_body_X` that the slot points at until a patch replaces it.
#define DEFINE_REX_FUNC(name)                                                                         \
  extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*);                                   \
  static void rex_body_##name(PPCContext& __restrict, uint8_t*);                                     \
  static std::atomic<PPCFunc*>& rex_patch_slot_##name =                                              \
      rex::patch::SlotTable::Get().DeclareBody(#name, &rex_body_##name);                             \
  __attribute__((noinline)) extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) { \
    [[clang::musttail]] return rex_patch_slot_##name.load(std::memory_order_acquire)(ctx, base);      \
  }                                                                                                   \
  __attribute__((weak, noinline)) extern "C" void name(PPCContext& __restrict ctx, uint8_t* base) {   \
    [[clang::musttail]] return __imp__##name(ctx, base);                                             \
  }                                                                                                   \
  static void rex_body_##name([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#else
#error "REXGLUE_PATCHABLE_ALL is implemented for clang on macOS only"
#endif
