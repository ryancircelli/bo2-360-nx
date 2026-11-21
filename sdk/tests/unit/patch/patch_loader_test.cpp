// Plain-main test: slot stub, original fallback, load/reload, fail-closed, unmatched entries.
// argv[1] = patch lib v2, argv[2] = patch lib v3 (same source, different return), argv[3] = not-a-patch file
#include <cstdio>
#include <cstdlib>
#include <rex/system/patch.h>

struct PPCContext {
  uint64_t r3;
};

extern "C" void sub_1000_orig(PPCContext& ctx, uint8_t*) { ctx.r3 = 1; }
extern "C" void sub_2000_orig(PPCContext& ctx, uint8_t*) { ctx.r3 = 10; }
REX_PATCH_STUB(sub_1000, sub_1000_orig)
REX_PATCH_STUB(sub_2000, sub_2000_orig)

#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);    \
      return 1;                                           \
    }                                                     \
  } while (0)

static uint64_t call(void (*f)(PPCContext&, uint8_t*)) {
  PPCContext c{};
  f(c, nullptr);
  return c.r3;
}

int main(int argc, char** argv) {
  CHECK(argc >= 4);
  CHECK(call(sub_1000) == 1);  // original
  auto r = rex::patch::LoadPatchLibrary(argv[1]);
  CHECK(r.ok && r.applied == 1 && r.unmatched.size() == 1 && r.unmatched[0] == "sub_nomatch");
  CHECK(call(sub_1000) == 2);
  CHECK(call(sub_2000) == 10);  // not in library: original
  auto r2 = rex::patch::LoadPatchLibrary(argv[2]);
  CHECK(r2.ok && r2.version == r.version + 1);
  CHECK(call(sub_1000) == 3);  // reload swaps without restart
  auto bad = rex::patch::LoadPatchLibrary(argv[3]);
  CHECK(!bad.ok && !bad.error.empty());
  CHECK(call(sub_1000) == 3);  // fail closed: old code kept
  auto miss = rex::patch::LoadPatchLibrary("/nonexistent/lib.dylib");
  CHECK(!miss.ok);
  std::printf("patch_loader_test OK (v%u -> v%u)\n", r.version, r2.version);
  return 0;
}
