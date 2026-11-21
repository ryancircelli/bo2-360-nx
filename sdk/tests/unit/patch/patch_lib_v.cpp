// Test patch library. PATCH_TEXT selects the version.
#include <rex/system/patch.h>

#ifndef PATCH_RET
#define PATCH_RET 2
#endif

struct PPCContext {
  uint64_t r3;
};

static void my_sub_1000(PPCContext& ctx, uint8_t*) { ctx.r3 = PATCH_RET; }
static void my_sub_nomatch(PPCContext& ctx, uint8_t*) { ctx.r3 = 99; }
REX_PATCH(sub_1000, my_sub_1000)
REX_PATCH(sub_nomatch, my_sub_nomatch)
REX_PATCH_LIBRARY()
