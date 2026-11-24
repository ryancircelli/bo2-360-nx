// REXGLUE_PATCHABLE_ALL: every DEFINE_REX_FUNC body sits behind a slot, and direct __imp__ calls go through it too.
#include <cstdint>
#include <cstdio>
#include <rex/ppc/context.h>
#include <rex/system/patchable_all.h>

DEFINE_REX_FUNC(sub_1) { ctx.r3.u64 = 1; }
DEFINE_REX_FUNC(sub_2) { __imp__sub_1(ctx, base); ctx.r3.u64 += 10; }   // codegen calls __imp__ directly
static void patched_sub_1(PPCContext& c, uint8_t*) { c.r3.u64 = 100; }

static int fail(const char* what) { std::fprintf(stderr, "patchable_all_test: %s\n", what); return 1; }

int main() {
  auto& slots = rex::patch::SlotTable::Get();
  PPCContext c{};
  uint8_t* b = nullptr;
  sub_2(c, b);
  if (c.r3.u64 != 11) return fail("original body not reached");
  auto* slot = slots.Find("sub_1");
  if (!slot) return fail("sub_1 has no slot");
  slot->current.store(&patched_sub_1);
  sub_2(c, b);
  if (c.r3.u64 != 110) return fail("direct __imp__ call did not reach the patch");
  slots.ResetAll();
  sub_2(c, b);
  if (c.r3.u64 != 11) return fail("ResetAll did not restore the original");
  std::puts("patchable_all_test: ok");
  return 0;
}
