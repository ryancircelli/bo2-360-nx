/**
 * @file        system/patch.h
 * @brief       Patch-code library ABI and slot table for hot-reloadable hooks
 *
 * A game's reloadable hooks are built into a separate shared library. The game
 * executable declares a slot per patchable function (REX_PATCH_STUB); the slot
 * starts at the original recompiled body and PatchLoader repoints it at the
 * library's version. Only depends on rex/ppc/func.h so patch libraries stay
 * light to compile.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ppc/func.h>

namespace rex::patch {

inline constexpr uint32_t kPatchAbiVersion = 1;
inline constexpr const char* kPatchAbiSymbol = "rex_patch_abi_version";
inline constexpr const char* kPatchEntriesSymbol = "rex_patch_entries";

// One replaceable function: `name` is the linker symbol of the recompiled
// function it replaces (for example "sub_82001234").
struct PatchEntry {
  const char* name;
  PPCFunc* fn;
};

using PatchAbiVersionFn = uint32_t (*)();
using PatchEntriesFn = const PatchEntry* (*)(size_t* count);

//=============================================================================
// Host side: slot table
//=============================================================================

struct PatchSlot {
  std::string name;
  PPCFunc* original;
  std::atomic<PPCFunc*> current;
};

class SlotTable {
 public:
  static SlotTable& Get();
  // Registers (or returns the existing) slot, initially pointing at `original`.
  std::atomic<PPCFunc*>& Declare(const char* name, PPCFunc* original);
  // Same, for the recompiled body itself (REXGLUE_PATCHABLE_ALL): it becomes the slot's original even when a
  // REX_PATCHABLE stub declared the slot first with `__imp__X`, which in such a build jumps through the slot.
  std::atomic<PPCFunc*>& DeclareBody(const char* name, PPCFunc* body);
  PatchSlot* Find(const std::string& name);
  size_t size() const { return slots_.size(); }
  // Slots whose name contains `filter` (empty: all), at most `limit` of them.
  std::vector<PatchSlot*> List(const std::string& filter, size_t limit) const;
  // Slots currently pointing away from their original body.
  size_t PatchedCount() const;
  // Points every slot back at its original recompiled body.
  void ResetAll();

 private:
  // A build with every function patchable (REXGLUE_PATCHABLE_ALL) declares ~100k slots during static
  // initialization, so lookups go through an index instead of a scan.
  std::vector<PatchSlot*> slots_;
  std::unordered_map<std::string, PatchSlot*> index_;
};

struct LoadResult {
  bool ok = false;
  uint32_t version = 0;           // monotonically increasing per successful load
  size_t applied = 0;             // entries bound to a slot
  std::vector<std::string> unmatched;  // library entries with no slot in this build
  std::string error;              // set when ok == false (old code kept)
};

// Loads `path` (copied to a unique temp name so a rebuilt library never aliases
// a mapped one), validates the ABI and binds every entry to its slot. Fails
// closed: any error leaves all slots unchanged. Libraries are never unloaded,
// because guest threads may still return into older versions.
LoadResult LoadPatchLibrary(const std::filesystem::path& path);

// Two-phase variant for live reload. PreparePatchLibrary does the dlopen and
// slot resolution on any thread and parks the result as "pending";
// CommitPendingPatch (cheap atomic check when nothing is pending) binds the
// slots and is called at the frame boundary (graphics swap). A newer prepare
// replaces an uncommitted older one. Slots are single atomic pointers and old
// libraries stay mapped, so threads inside old code finish it safely and every
// later call takes the new code; no thread is parked. (A multi-function
// change can be seen half-applied by two threads for the span of one commit.)
LoadResult PreparePatchLibrary(const std::filesystem::path& path);
bool CommitPendingPatch();
uint32_t CommittedPatchVersion();

// Polls `path` every `poll_ms` and prepares the library when its mtime/size
// changes. Idempotent; thread runs for process lifetime.
void StartPatchWatcher(const std::filesystem::path& path, uint32_t poll_ms = 250);

}  // namespace rex::patch

//=============================================================================
// Host macros (game executable)
//=============================================================================

// Defines the callable `subroutine` as a stub that jumps through its slot.
// `original` is the symbol of the unpatched recompiled body.
#define REX_PATCH_STUB(subroutine, original)                                     \
  extern "C" void original(PPCContext& __restrict ctx, uint8_t* base);          \
  static std::atomic<PPCFunc*>& rex_patch_slot_##subroutine =                    \
      rex::patch::SlotTable::Get().Declare(#subroutine, &original);              \
  extern "C" void subroutine(PPCContext& __restrict ctx, uint8_t* base) {        \
    rex_patch_slot_##subroutine.load(std::memory_order_acquire)(ctx, base);      \
  }

// Patchable recompiled function with no codegen change: the generated code
// defines `__imp__sub_X` as the always-original body and a weak `sub_X` that
// hooks override (DEFINE_REX_FUNC), so a strong REX_PATCHABLE(sub_X) in the game
// sits in front of it for direct calls and function-table entries alike.
#define REX_PATCHABLE(subroutine) REX_PATCH_STUB(subroutine, __imp__##subroutine)

//=============================================================================
// Library macros (patch dylib)
//=============================================================================

// Body of a patch: same signature as a recompiled function. Registers itself
// for export; REX_PATCH_LIBRARY() must appear once in the library.
#define REX_PATCH(subroutine, body)                                                     \
  namespace {                                                                           \
  struct RexPatchReg_##subroutine {                                                     \
    RexPatchReg_##subroutine() {                                                        \
      rex::patch::detail::Entries().push_back({#subroutine, &body});                    \
    }                                                                                   \
  } rex_patch_reg_##subroutine;                                                         \
  }

// Library side: lets a patch body call the unpatched recompiled function.
// Declare once per patched function; call with REX_PATCH_CALL_ORIGINAL.
#define REX_PATCH_ORIGINAL(subroutine) \
  extern "C" __attribute__((visibility("default"))) PPCFunc* rex_patch_original_##subroutine = nullptr;
#define REX_PATCH_CALL_ORIGINAL(subroutine, ctx, base) rex_patch_original_##subroutine(ctx, base)

namespace rex::patch::detail {
inline std::vector<PatchEntry>& Entries() {
  static std::vector<PatchEntry> entries;
  return entries;
}
}  // namespace rex::patch::detail

#define REX_PATCH_LIBRARY()                                                     \
  extern "C" __attribute__((visibility("default"))) uint32_t rex_patch_abi_version() { \
    return rex::patch::kPatchAbiVersion;                                        \
  }                                                                             \
  extern "C" __attribute__((visibility("default")))                             \
  const rex::patch::PatchEntry* rex_patch_entries(size_t* count) {              \
    *count = rex::patch::detail::Entries().size();                              \
    return rex::patch::detail::Entries().data();                                \
  }
