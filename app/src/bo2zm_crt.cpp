// BO2's memory copy and fill routines, as native code ([rexcrt] in app/overrides.toml).
//
// The codegen drops the recompiled bodies of the functions listed under [rexcrt] and calls rexcrt_<key> instead,
// from every direct call and from the dispatch table (so function pointers land here too). The addresses, and the
// evidence that each one is a plain memcpy/memmove/memset, are in app/overrides.toml:
//   bo2zm_memcpy   0x82916330  CRT memcpy (forward)            bo2zm_memmove 0x82916270  CRT memmove
//   bo2zm_memset   0x82916AE0  CRT memset                      bo2zm_xmemcpy 0x827D33F0  VMX memcpy (dcbzl)
//   bo2zm_xmemset  0x827D2EC0  VMX memset (dcbzl)              bo2zm_bigcopy 0x827D3290, bo2zm_bigcopy2 0x827D3340
//
// Why not the SDK's own rexcrt_memcpy/memset (kernel/crt/memory.cpp): those go through HostToGuestFunction, which
// sets r3 = host pointer - base. On PC, guest addresses from 0xE0000000 up sit 0x1000 further in host memory
// (PhysicalHostOffset), so a copy into that physical view would return dst + 0x1000, and MSVC callers may use
// memcpy's return value. Here r3 is left as it came in, which is dst: what every one of these routines returns.
// No other register is written: the 360 routines only clobber volatile registers, which no caller may rely on.
//
// All copies use memmove semantics. That is a superset of each 360 routine wherever its result was defined: the
// CRT memcpy only copies forwards (memmove's dst < src case), and the VMX copies clear destination lines with dcbzl
// before reading the source, so they never supported overlap at all.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/system/write_trace.h>

#include <fmt/format.h>

#include "bo2zm_page_writes.h"
#if !defined(BO2_GAME_CAMPAIGN)
#include "bo2zm_mod_hud_fit.h"
#endif

namespace {

inline uint8_t* Host(uint8_t* base, uint32_t guest) {
  return rex::memory::GuestPtr<uint8_t*>(base, guest);
}

// r3 = dst, r4 = src, r5 = byte count; returns dst in r3 (untouched).
// Copies and fills into physical memory are stamped for the renderer's vertex reuse (bo2zm_page_writes.h): the
// asset loader and the UP draws write vertex data this way.
inline void Move(PPCContext& ctx, uint8_t* base, const char* tag) {
  const uint32_t n = ctx.r5.u32;
  if (n != 0) [[likely]] {
#if !defined(BO2_GAME_CAMPAIGN)
    if ((n == 124 && bo2zm::mod_hud_fit::IsCopySite(uint32_t(ctx.lr))) ||
        (n == 10760 && (bo2zm::mod_hud_fit::IsUnknownPsCopySite(uint32_t(ctx.lr)) ||
                        bo2zm::mod_hud_fit::IsBulkPsCopySite(uint32_t(ctx.lr))))) [[unlikely]]
      bo2zm::mod_hud_fit::BeforeCopy(ctx, base);
#endif
    std::memmove(Host(base, ctx.r3.u32), Host(base, ctx.r4.u32), n);
#if !defined(BO2_GAME_CAMPAIGN)
    if (n == 124 && bo2zm::mod_hud_fit::IsCopySite(uint32_t(ctx.lr))) [[unlikely]]
      bo2zm::mod_hud_fit::AfterCopy(ctx, base);
#endif
    bo2zm::page_writes::MarkGuest(ctx.r3.u32, n);
    if (rex::write_trace::WantedGuest(ctx.r3.u32, n)) [[unlikely]] {
      rex::write_trace::NotePhysicalSlow(tag, rex::write_trace::ToPhysical(ctx.r3.u32), n, uint32_t(ctx.lr),
                                         fmt::format("dst {:08X} src {:08X} (src phys {:08X})", ctx.r3.u32,
                                                     ctx.r4.u32, rex::write_trace::ToPhysical(ctx.r4.u32)));
    }
  }
}

// r3 = dst, r4 = value (its low byte), r5 = byte count; returns dst in r3 (untouched).
inline void Fill(PPCContext& ctx, uint8_t* base, const char* tag) {
  const uint32_t n = ctx.r5.u32;
  if (n != 0) [[likely]] {
    std::memset(Host(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), n);
    bo2zm::page_writes::MarkGuest(ctx.r3.u32, n);
    if (rex::write_trace::WantedGuest(ctx.r3.u32, n)) [[unlikely]] {
      rex::write_trace::NotePhysicalSlow(tag, rex::write_trace::ToPhysical(ctx.r3.u32), n, uint32_t(ctx.lr),
                                         fmt::format("dst {:08X} value {:02X}", ctx.r3.u32, ctx.r4.u32 & 0xFF));
    }
  }
}

}  // namespace

extern "C" REX_FUNC(rexcrt_bo2zm_memcpy) { Move(ctx, base, "crt-memcpy"); }
extern "C" REX_FUNC(rexcrt_bo2zm_memmove) { Move(ctx, base, "crt-memmove"); }
extern "C" REX_FUNC(rexcrt_bo2zm_xmemcpy) { Move(ctx, base, "crt-xmemcpy"); }
extern "C" REX_FUNC(rexcrt_bo2zm_bigcopy) { Move(ctx, base, "crt-bigcopy"); }
extern "C" REX_FUNC(rexcrt_bo2zm_bigcopy2) { Move(ctx, base, "crt-bigcopy2"); }
extern "C" REX_FUNC(rexcrt_bo2zm_memset) { Fill(ctx, base, "crt-memset"); }
extern "C" REX_FUNC(rexcrt_bo2zm_xmemset) { Fill(ctx, base, "crt-xmemset"); }
