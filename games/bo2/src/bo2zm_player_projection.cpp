// Per-player FOV and jump limits. Actor ownership and effective settings come
// from the cheat core; no shared dvar or game constant is changed here.
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>

#include "bo2zm_cheats.h"
#include "bo2zm_guest.h"

REX_EXTERN(__imp__sub_821AFC90);
REX_EXTERN(__imp__sub_82687AF8);
REX_EXTERN(__imp__sub_82687B80);
REX_EXTERN(__imp__sub_82687D98);

namespace {
constexpr float kHeights[] = {39.0f, 100.0f, 250.0f, 600.0f, 1500.0f, 3500.0f};

uint32_t Word(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}
float Scalar(const uint8_t* base, uint32_t address) {
  const auto bits = Word(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
void StoreScalar(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  bits = __builtin_bswap32(bits);
  std::memcpy(base + address, &bits, sizeof(bits));
}
float SelectedHeight(const uint8_t* base, uint32_t ps) {
  const int level = bo2zm::cheats::JumpLevelForState(base, ps);
  return level > 0 && level < 6 ? kHeights[level] : 39.0f;
}

template <uint32_t Address>
uint32_t Invoke(PPCContext& call, uint8_t* base, uint32_t lr, uint32_t a, uint32_t b = 0) {
  call.r3.u64 = a;
  call.r4.u64 = b;
  call.lr = lr;
  bo2zm::guest::Call<Address>(call, base);
  return call.r3.u32;
}
float Dvar(PPCContext& call, uint8_t* base, uint32_t lr, uint32_t pointer) {
  Invoke<0x82496780>(call, base, lr, Word(base, pointer));  // call only
  return float(call.f1.f64);
}

// Preserve the guest's state-dependent FOV branches and subcalls. Only the
// ordinary scale/cap tail receives the additional local multiplier.
float LocalFov(PPCContext& call, uint8_t* base, uint32_t local, uint32_t cg,
               uint32_t viewmodel, float scale) {
  const uint32_t ps = cg + 0x480A8;
  const uint32_t weapon = Invoke<0x82252D70>(call, base, 0x821AFCE8, ps, local);  // call only
  Invoke<0x826BF970>(call, base, 0x821AFCF0, weapon);  // call only
  Invoke<0x826BF988>(call, base, 0x821AFCF8, weapon);  // call only
  float value = Dvar(call, base, 0x821AFD04, 0x82BC2774);
  if (Invoke<0x82253418>(call, base, 0x821AFD10, local))  // call only
    return Dvar(call, base, 0x821AFD24, 0x82BE5DD4);

  if ((Word(base, ps + 1156) & 4) && (weapon & 255) && !(Word(base, ps + 256) & 0x10000000)) {
    const uint32_t scratch = call.r1.u32 + 80;
    for (uint32_t i = 0; i < 12; i += 4) StoreScalar(base, scratch + i, 0.0f);
    call.r5.u64 = scratch + 4;
    call.r6.u64 = scratch + 8;
    Invoke<0x826C2F30>(call, base, 0x821AFDA4, weapon, scratch);  // call only
    const float weapon_fov = Scalar(base, scratch);
    if (Scalar(base, ps + 480) > 0.0f && weapon_fov > 0.0f) return weapon_fov;
    return Scalar(base, 0x82003608);
  }
  if (Invoke<0x821E94D0>(call, base, 0x821AFDDC, local) &&  // call only
      Word(base, cg + 0x480A0) == 1 && (Word(base, Word(base, cg + 40) + 268) & 0x4000))
    return Scalar(base, 0x820035E4);
  if (Invoke<0x821AF630>(call, base, 0x821AFE30, local)) return value;  // call only

  const auto aim_fov = [&](uint32_t lr, float initial) {
    call.r5.u64 = weapon;
    call.r7.u64 = viewmodel;
    call.f1.f64 = initial;
    Invoke<0x821AF6F8>(call, base, lr, local, ps);  // call only
    return float(call.f1.f64);
  };
  value = aim_fov(0x821AFE54, value);
  if ((Word(base, ps + 252) & 0x300) && !(Word(base, ps + 256) & 0x10000000)) {
    Invoke<0x821AFB58>(call, base, 0x821AFE84, local, ps);  // call only
    value = float(call.f1.f64);
  }
  const float cap = Dvar(call, base, 0x821AFE94, 0x82B25EC8);
  if (Invoke<0x826B4030>(call, base, 0x821AFEA0, Word(base, ps + 252))) {  // call only
    const uint16_t index_bits = uint16_t((uint16_t(base[ps + 1140]) << 8) | base[ps + 1141]);
    const auto index = int32_t(int16_t(index_bits));
    const uint32_t entity = Word(base, 0x82BBC554 + local * 4) + uint32_t(index * 884);
    if (entity && (Word(base, entity + 876) & 0x40000000)) {
      const auto definition = Invoke<0x82235FD0>(call, base, 0x821AFEE0, entity);  // call only
      const float vehicle_fov = Scalar(base, definition + 112);
      if (vehicle_fov > cap) value = vehicle_fov;
      if (Invoke<0x826B5048>(call, base, 0x821AFEF8, ps) && (weapon & 255))  // call only
        value = aim_fov(0x821AFF40, value);
    }
  }
  const float global = Dvar(call, base, 0x821AFF50, 0x82BBC568);
  const float effective_scale = float(global * scale);
  const float selected = float(effective_scale * value);
  const float selector = float(cap - selected);
  // fsel chooses the product on an unordered comparison, unlike fmin.
  return selector >= 0.0f ? cap : selected;
}
}  // namespace

REX_HOOK_RAW(sub_821AFC90) {
  const uint32_t local = ctx.r3.u32;
  const float scale = local < 4 ? bo2zm::cheats::FieldOfViewScaleForLocal(int(local)) : 0.0f;
  if (!(scale > 0.0f) || !std::isfinite(scale) || scale == 1.0f) {
    __imp__sub_821AFC90(ctx, base);
    return;
  }
  const uint32_t array = Word(base, 0x82BBAE68);
  if (!array || local >= Word(base, 0x82C70F44) || array > UINT32_MAX - 4 * 0x87700 || ctx.r1.u32 < 256) {
    __imp__sub_821AFC90(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  PPCContext call = ctx;
  call.r1.u64 = (ctx.r1.u32 - 256) & ~uint32_t(15);
  ctx.f1.f64 = LocalFov(call, base, local, array + local * 0x87700, ctx.r4.u32, scale);
}

REX_HOOK_RAW(sub_82687AF8) {
  const uint32_t ps = ctx.r3.u32, position = ctx.r4.u32, output = ctx.r5.u32;
  const float height = SelectedHeight(base, ps);
  if (height == 39.0f) { __imp__sub_82687AF8(ctx, base); return; }
  ctx.fpscr.disableFlushMode();
  const float ceiling = float(Scalar(base, ps + 196) + height);
  if (!(Scalar(base, position + 8) < ceiling)) { ctx.r3.u64 = 0; return; }
  PPCContext call = ctx;
  const float step = Dvar(call, base, 0x82687B3C, 0x845BA164);
  StoreScalar(base, output, step);
  const float z = Scalar(base, position + 8);
  const float remaining_ceiling = float(Scalar(base, ps + 196) + height);
  if (float(z + step) > remaining_ceiling) StoreScalar(base, output, float(remaining_ceiling - z));
  ctx.r3.u64 = 1;
}

REX_HOOK_RAW(sub_82687B80) {
  const uint32_t ps = ctx.r3.u32;
  const float height = SelectedHeight(base, ps);
  if (height == 39.0f) { __imp__sub_82687B80(ctx, base); return; }
  ctx.fpscr.disableFlushMode();
  const float ceiling = float(Scalar(base, ps + 196) + height);
  ctx.r3.u64 = !(Scalar(base, ps + 48) < ceiling);
}

REX_HOOK_RAW(sub_82687D98) {
  const uint32_t ps = ctx.r3.u32, previous = ctx.r4.u32;
  const float height = SelectedHeight(base, ps);
  if (height == 39.0f) { __imp__sub_82687D98(ctx, base); return; }
  ctx.fpscr.disableFlushMode();
  const float z = Scalar(base, ps + 48);
  if (!(float(z - Scalar(base, previous + 8)) > 0.0f)) return;
  const float remaining = float(float(Scalar(base, ps + 196) + height) - z);
  if (remaining < 0.1f) { StoreScalar(base, ps + 60, 0.0f); return; }
  const float twice = float(2.0f * remaining);
  const float gravity = float(int32_t(Word(base, ps + 140)));
  const float product = float(gravity * twice);
  const float cap = float(std::sqrt(double(product)));
  if (Scalar(base, ps + 60) > cap) StoreScalar(base, ps + 60, cap);
}
