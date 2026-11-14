#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_82332180);
REX_HOOK_RAW(sub_82332180) {
  std::string format;
  if (ctx.r4.u32) {
    const char* text = reinterpret_cast<const char*>(base + ctx.r4.u32);
    format.assign(text, strnlen(text, 2048));
  }
  REXLOG_ERROR(
      "BO1 Com_Error code={} lr={:08X} format=\"{}\" args={:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
      ctx.r3.u32, static_cast<uint32_t>(ctx.lr), format, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
      ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
  __imp__sub_82332180(ctx, base);
}

REX_EXTERN(__imp__sub_825FB7A0);
REX_HOOK_RAW(sub_825FB7A0) {
  REXLOG_ERROR("BO1 longjmp lr={:08X} buf={:08X} val={:08X} r1={:08X}", static_cast<uint32_t>(ctx.lr),
               ctx.r3.u32, ctx.r4.u32, ctx.r1.u32);
  __imp__sub_825FB7A0(ctx, base);
}

// Who asks the loader to leave the title? Callers of the XamLoaderLaunchTitle wrapper sub_824D0218. Log-only.
static void LogLeaveTitle(const char* who, PPCContext& ctx, uint8_t* base) {
  std::string stack_code;
  for (uint32_t off = 0; off < 1024; off += 4) {
    uint32_t v;
    std::memcpy(&v, base + ctx.r1.u32 + off, 4);
    v = __builtin_bswap32(v);
    if (v >= 0x82000000u && v < 0x82800000u) stack_code += fmt::format(" +{:X}:{:08X}", off, v);
  }
  REXLOG_ERROR("BO1 leave-title {} lr={:08X} r3={:08X} r4={:08X} r1={:08X} stack-code:{}", who,
               static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r1.u32, stack_code);
}
REX_EXTERN(__imp__sub_823339F8);
REX_HOOK_RAW(sub_823339F8) { LogLeaveTitle("sub_823339F8", ctx, base); __imp__sub_823339F8(ctx, base); }
REX_EXTERN(__imp__sub_823C2EB8);
REX_HOOK_RAW(sub_823C2EB8) { LogLeaveTitle("sub_823C2EB8", ctx, base); __imp__sub_823C2EB8(ctx, base); }
REX_EXTERN(__imp__sub_824D0D50);
REX_HOOK_RAW(sub_824D0D50) { LogLeaveTitle("sub_824D0D50", ctx, base); __imp__sub_824D0D50(ctx, base); }
