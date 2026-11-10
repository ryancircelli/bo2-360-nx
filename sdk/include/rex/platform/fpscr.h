/**
 * @file        platform/fpscr.h
 * @brief       Platform-specific FPSCR constants and intrinsics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>

#include <rex/types.h>

#include <simde/x86/sse.h>

// SSE3 constants are missing from simde
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

namespace rex::platform {

// simde does not handle denormal flags, so we need to implement per-arch.
#if defined(__x86_64__) || defined(_M_X64)

struct FPSCRPlatform {
  static constexpr size_t RoundShift = 13;
  static constexpr size_t RoundMaskVal = SIMDE_MM_ROUND_MASK;
  static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  // Exception mask bits (1 = exception masked/disabled)
  static constexpr u32 ExceptionMask = (1 << 7) |   // IM - Invalid operation
                                       (1 << 8) |   // DM - Denormal operand
                                       (1 << 9) |   // ZM - Zero divide
                                       (1 << 10) |  // OM - Overflow
                                       (1 << 11) |  // UM - Underflow
                                       (1 << 12);   // PM - Precision (Inexact)
  static constexpr size_t GuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                           SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};
  // Indexed by the host RC field (MXCSR bits 13-14): Nearest, Down, Up, Zero. Values are
  // the PowerPC FPSCR[RN] encodings (0 Nearest, 1 Zero, 2 +Inf, 3 -Inf).
  static constexpr size_t HostToGuest[] = {0, 3, 2, 1};

  static inline u32 getcsr() noexcept { return simde_mm_getcsr(); }

  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr |= ExceptionMask;  // Set mask bits to disable exceptions
  }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

struct FPSCRPlatform {
  // RMode
  static constexpr size_t RoundShift = 22;
  static constexpr size_t RoundMaskVal = 3 << RoundShift;
  // FZ and FZ16
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
  // Indexed by the PowerPC FPSCR[RN] value (0 Nearest, 1 Zero, 2 +Infinity, 3 -Infinity);
  // FPCR RMode is 0 RN (nearest), 1 RP (+Infinity), 2 RM (-Infinity), 3 RZ (zero).
  static constexpr size_t GuestToHost[] = {0 << RoundShift, 3 << RoundShift, 1 << RoundShift,
                                           2 << RoundShift};
  // Indexed by the host RMode field: Nearest, +Infinity, -Infinity, Zero. The ARM order
  // differs from the x86 MXCSR order (Nearest, Down, Up, Zero), so this table is per arch.
  static constexpr size_t HostToGuest[] = {0, 2, 3, 1};
  // Exception enable bits (0 = exception disabled, ARM defaults to 0)
  static constexpr u32 ExceptionMask = (1 << 8) |   // IOE - Invalid Operation
                                       (1 << 9) |   // DZE - Division by Zero
                                       (1 << 10) |  // OFE - Overflow
                                       (1 << 11) |  // UFE - Underflow
                                       (1 << 12) |  // IXE - Inexact
                                       (1 << 15);   // IDE - Input Denormal

  static inline u32 getcsr() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }

  static inline void setcsr(u32 csr) noexcept {
    __asm__ __volatile__("msr fpcr, %0" : : "r"(static_cast<u64>(csr)));
  }

  static inline void InitHostExceptions(u32& csr) noexcept {
    csr &= ~ExceptionMask;  // Clear enable bits to disable exceptions
  }
};

#else
#error "Missing implementation for FPSCR."
#endif

// Build-time check: every PowerPC rounding mode (FPSCR[RN] 0..3) survives guest -> host ->
// guest, so mffs reads back what mtfsf wrote.
constexpr bool FPSCRRoundTablesInvert() {
  for (size_t mode = 0; mode < 4; ++mode) {
    size_t host = FPSCRPlatform::GuestToHost[mode];
    if ((host & ~FPSCRPlatform::RoundMaskVal) != 0) return false;
    if (FPSCRPlatform::HostToGuest[host >> FPSCRPlatform::RoundShift] != mode) return false;
  }
  return true;
}
static_assert(FPSCRRoundTablesInvert(), "FPSCR GuestToHost/HostToGuest must invert each other");

}  // namespace rex::platform
