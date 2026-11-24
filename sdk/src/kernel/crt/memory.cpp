/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/hook.h>
#include <rex/system/xmemory.h>

namespace rex::kernel::crt {

static void* native_memchr(const void* ptr, int val, uint32_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------
static void* native_XMemSet128(void* dst, int val, uint32_t n) {
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, uint32_t n) {
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, uint32_t dstsz, const void* src, uint32_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, uint32_t dstsz, const void* src, uint32_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memmove(dst, src, count);
  return 0;
}

// memcpy/memmove/memset and XMemCpy/XMemSet: r3 = dst, r4 = src or fill byte, r5 = count. They return dst,
// which is r3 already, so r3 is left alone: a REX_HOOK return would be host pointer - base, 0x1000 too high
// for destinations from 0xE0000000 where PhysicalHostOffset applies. Copies use memmove semantics, a superset
// of the 360 routines (the CRT memcpy copies forwards only; XMemCpy clears destination lines with dcbzl
// before reading, so it never allowed overlap).
inline void Move(PPCContext& ctx, uint8_t* base) {
  if (const uint32_t n = ctx.r5.u32)
    std::memmove(rex::memory::GuestPtr<uint8_t*>(base, ctx.r3.u32),
                 rex::memory::GuestPtr<uint8_t*>(base, ctx.r4.u32), n);
}
inline void Fill(PPCContext& ctx, uint8_t* base) {
  if (const uint32_t n = ctx.r5.u32)
    std::memset(rex::memory::GuestPtr<uint8_t*>(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), n);
}

}  // namespace rex::kernel::crt

extern "C" REX_FUNC(rexcrt_memcpy) { rex::kernel::crt::Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_memmove) { rex::kernel::crt::Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_XMemCpy) { rex::kernel::crt::Move(ctx, base); }
extern "C" REX_FUNC(rexcrt_memset) { rex::kernel::crt::Fill(ctx, base); }
extern "C" REX_FUNC(rexcrt_XMemSet) { rex::kernel::crt::Fill(ctx, base); }

REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
REX_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
REX_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
REX_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
