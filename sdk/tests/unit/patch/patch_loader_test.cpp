// Plain-main test: slot stub, original fallback, load/reload, fail-closed, unmatched entries.
// argv[1] = patch lib v2, argv[2] = patch lib v3 (same source, different return), argv[3] = not-a-patch file
#include <chrono>
#include <cstdio>
#include <thread>
#include <filesystem>
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
  // Two-phase: prepare does not change behaviour until the frame-boundary commit.
  auto pr = rex::patch::PreparePatchLibrary(argv[1]);
  CHECK(pr.ok);
  CHECK(call(sub_1000) == 3);
  CHECK(rex::patch::CommitPendingPatch());
  CHECK(call(sub_1000) == 2);
  CHECK(!rex::patch::CommitPendingPatch());  // nothing pending
  // Watcher: replace a watched file; new code appears after the next commit, no restart.
  std::filesystem::path watched = std::filesystem::temp_directory_path() / "rex_patch_watched.dylib";
  std::filesystem::copy_file(argv[1], watched, std::filesystem::copy_options::overwrite_existing);
  rex::patch::StartPatchWatcher(watched, 50);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::filesystem::copy_file(argv[2], watched, std::filesystem::copy_options::overwrite_existing);
  bool committed = false;
  for (int i = 0; i < 40 && !committed; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    committed = rex::patch::CommitPendingPatch();
  }
  CHECK(committed);
  CHECK(call(sub_1000) == 3);
  std::printf("patch_loader_test OK (v%u -> v%u)\n", r.version, r2.version);
  return 0;
}
