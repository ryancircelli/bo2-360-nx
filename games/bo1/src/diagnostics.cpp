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

// Concurrency detector for the script tokenizer (diagnostic only): do two guest threads run it at once?
#include <atomic>
#include <thread>
#include <functional>
static std::atomic<int> g_tok_depth{0};
static std::atomic<uint64_t> g_tok_owner{0};
static std::atomic<int> g_tok_logs{0};
static uint64_t TokTid() { return std::hash<std::thread::id>()(std::this_thread::get_id()); }
#define TOK_HOOK(ADDR)                                                                         \
  REX_EXTERN(__imp__sub_##ADDR);                                                               \
  REX_HOOK_RAW(sub_##ADDR) {                                                                   \
    uint64_t me = TokTid();                                                                    \
    { static std::atomic<uint64_t> seen[64]; int c = 0; uint32_t cpu = *(base + ctx.r13.u32 + 268); uint64_t key = (me << 8) | cpu | 0x100000000000000ull; for (auto& x : seen) { uint64_t v = x.load(); if (v == key) { c = 1; break; } if (v == 0 && x.compare_exchange_strong(v, key)) { REXLOG_ERROR("BO1 tokenizer-user {} host_tid={:x} cpu={} r13={:08X}", #ADDR, me, cpu, ctx.r13.u32); c = 1; break; } } (void)c; } \
    int d = g_tok_depth.fetch_add(1);                                                          \
    uint64_t prev = g_tok_owner.exchange(me);                                                  \
    if (d > 0 && prev != me && g_tok_logs.fetch_add(1) < 50)                                   \
      REXLOG_ERROR("BO1 tokenizer {} CONCURRENT depth={} me={:x} prev={:x} lr={:08X}", #ADDR,  \
                   d, me, prev, static_cast<uint32_t>(ctx.lr));                                \
    __imp__sub_##ADDR(ctx, base);                                                              \
    g_tok_depth.fetch_sub(1);                                                                  \
  }
TOK_HOOK(8239EAA8)
TOK_HOOK(8239EB20)
