// Put the primary local user first when starting a Local match. Use the game's
// complete slot swap before connection initialization assigns player state.
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#include "bo2zm_guest.h"

REX_EXTERN(__imp__sub_82404358);

namespace {
constexpr uint32_t kManager = 0x837F6A28;
constexpr uint32_t kPrimary = 0x82976AC8;
constexpr uint32_t kModes = 0x837F6A80;
constexpr uint32_t kSwap = 0x82404188;  // call only

uint32_t ReadWord(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

bool IsConnectionSetup(const PPCContext& ctx, const uint8_t* base, uint32_t caller) {
  if (caller == 0x8227746C) return true;
  if (caller != 0x82271620) return false;
  // This wrapper's 96-byte frame saves its caller's LR at +88. Limit the
  // extension to map setup, rather than every caller of the general slot sort.
  if (ctx.r1.u32 > UINT32_MAX - 92) return false;
  const uint32_t outer = ReadWord(base, ctx.r1.u32 + 88);
  return outer == 0x824332F8 || outer == 0x82298D68;
}
}  // namespace

REX_HOOK_RAW(sub_82404358) {
  const uint32_t caller = uint32_t(ctx.lr);
  const bool setup = IsConnectionSetup(ctx, base, caller);
  __imp__sub_82404358(ctx, base);
  if (!setup || !(ReadWord(base, kModes) & (1u << 2))) return;

  const uint32_t primary = ReadWord(base, kPrimary);
  if (primary == 0 || primary >= 4) return;
  uint32_t count = 0, controllers = 0;
  bool inactive = false;
  for (uint32_t local = 0; local < 4; ++local) {
    const uint32_t record = kManager + local * 20;
    if (ReadWord(base, record + 4) != local) return;
    if (!(ReadWord(base, record) & 1)) {
      inactive = true;
      continue;
    }
    const uint32_t controller = ReadWord(base, record + 8);
    if (inactive || controller >= 4 || (controllers & (1u << controller))) return;
    controllers |= 1u << controller;
    ++count;
  }
  if (primary >= count) return;
  const uint32_t controller = ReadWord(base, kManager + primary * 20 + 8);

  const PPCContext saved = ctx;
  for (uint32_t local = primary; local > 0; --local) {
    ctx = saved;
    ctx.r3.u64 = local;
    ctx.r4.u64 = local - 1;
    bo2zm::guest::Call<kSwap>(ctx, base);
  }
  ctx = saved;
  REXLOG_INFO("[local order] primary controller {} assigned local client 0 before connection setup", controller);
}
