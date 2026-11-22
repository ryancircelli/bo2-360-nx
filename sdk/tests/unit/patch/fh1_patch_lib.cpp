// Patch library for FH1 sub_82416380 (D3D draw function): traces calls and forwards to the original.
// Build with -DPATCH_TAG=1 / 2 to produce distinguishable versions.
#include <atomic>
#include <cstdio>
#include <rex/ppc/context.h>
#include <rex/system/patch.h>

#ifndef PATCH_TAG
#define PATCH_TAG 1
#endif
REX_PATCH_ORIGINAL(sub_82416380)
static std::atomic<uint64_t> g_calls{0};
static void draw_patch(PPCContext& ctx, uint8_t* base) {
  uint64_t n = ++g_calls;
  uint32_t r3_in = ctx.r3.u32;
  REX_PATCH_CALL_ORIGINAL(sub_82416380, ctx, base);
  if (n == 1 || n % 20000 == 0) {
    fprintf(stderr, "[patch-fh1] v%d sub_82416380 call #%llu r3_in=0x%08X r3_out=0x%08X (original ran)\n",
            PATCH_TAG, (unsigned long long)n, r3_in, ctx.r3.u32);
    fflush(stderr);
  }
}
REX_PATCH(sub_82416380, draw_patch)
REX_PATCH_LIBRARY()
