/**
 * @file        system/patch_loader.cpp
 * @brief       Slot table and loader for hot-reloadable patch libraries
 */

#include <rex/system/patch.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <system_error>
#include <thread>

#include <rex/platform/dynlib.h>

namespace rex::patch {

SlotTable& SlotTable::Get() {
  static SlotTable* table = new SlotTable();
  return *table;
}

std::atomic<PPCFunc*>& SlotTable::Declare(const char* name, PPCFunc* original) {
  if (PatchSlot* existing = Find(name)) {
    return existing->current;
  }
  auto* slot = new PatchSlot{name, original, {original}};
  slots_.push_back(slot);
  return slot->current;
}

PatchSlot* SlotTable::Find(const std::string& name) {
  for (PatchSlot* s : slots_) {
    if (s->name == name) return s;
  }
  return nullptr;
}

void SlotTable::ResetAll() {
  for (PatchSlot* s : slots_) {
    s->current.store(s->original, std::memory_order_release);
  }
}

namespace {
struct Pending {
  std::vector<std::pair<PatchSlot*, PPCFunc*>> bind;
  uint32_t version;
};
std::atomic<Pending*> g_pending{nullptr};
std::atomic<uint32_t> g_committed{0};
}  // namespace

LoadResult PreparePatchLibrary(const std::filesystem::path& path) {
  static std::atomic<uint32_t> version{0};
  LoadResult result;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    result.error = "patch library not found: " + path.string();
    return result;
  }
  uint32_t v = ++version;
  auto tmp = std::filesystem::temp_directory_path(ec) /
             ("rex_patch_" +
              std::to_string((unsigned long long)std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count()) +
              "_" + std::to_string(v) + path.extension().string());
  std::filesystem::copy_file(path, tmp, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    result.error = "cannot copy patch library: " + ec.message();
    return result;
  }
  // Leaked on purpose (never dlclose); see header.
  auto* lib = new platform::DynamicLibrary();
  if (!lib->Load(tmp, platform::SymbolResolution::kImmediate)) {
    result.error = "dlopen failed: " + tmp.string();
    return result;
  }
  auto abi = lib->GetSymbol<PatchAbiVersionFn>(kPatchAbiSymbol);
  auto entries_fn = lib->GetSymbol<PatchEntriesFn>(kPatchEntriesSymbol);
  if (!abi || !entries_fn) {
    result.error = "not a rexglue patch library (missing exports): " + path.string();
    return result;
  }
  if (abi() != kPatchAbiVersion) {
    result.error = "patch ABI " + std::to_string(abi()) + " != host " +
                   std::to_string(kPatchAbiVersion);
    return result;
  }
  size_t count = 0;
  const PatchEntry* entries = entries_fn(&count);
  auto* pending = new Pending{{}, v};
  for (size_t i = 0; i < count; ++i) {
    PatchSlot* slot = SlotTable::Get().Find(entries[i].name);
    if (!slot) {
      result.unmatched.push_back(entries[i].name);
    } else {
      pending->bind.emplace_back(slot, entries[i].fn);
      // Optional: a library that wants to call the unpatched body defines
      // `PPCFunc* rex_patch_original_<name>` (REX_PATCH_ORIGINAL); fill it in.
      if (auto* orig = lib->GetSymbol<PPCFunc**>(
              (std::string("rex_patch_original_") + entries[i].name).c_str())) {
        *orig = slot->original;
      }
    }
  }
  result.ok = true;
  result.version = v;
  result.applied = pending->bind.size();
  delete g_pending.exchange(pending, std::memory_order_acq_rel);
  return result;
}

bool CommitPendingPatch() {
  if (!g_pending.load(std::memory_order_relaxed)) return false;
  Pending* p = g_pending.exchange(nullptr, std::memory_order_acq_rel);
  if (!p) return false;
  // Slots absent from this library go back to the original body.
  SlotTable::Get().ResetAll();
  for (auto& [slot, fn] : p->bind) slot->current.store(fn, std::memory_order_release);
  g_committed.store(p->version, std::memory_order_release);
  delete p;
  return true;
}

uint32_t CommittedPatchVersion() { return g_committed.load(std::memory_order_acquire); }

LoadResult LoadPatchLibrary(const std::filesystem::path& path) {
  LoadResult r = PreparePatchLibrary(path);
  if (r.ok) CommitPendingPatch();
  return r;
}

void StartPatchWatcher(const std::filesystem::path& path, uint32_t poll_ms) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread([path, poll_ms] {
    std::filesystem::file_time_type last{};
    std::uintmax_t last_size = 0;
    bool first = true;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
      std::error_code ec;
      auto t = std::filesystem::last_write_time(path, ec);
      // A file that does not exist yet is not a baseline: when it appears it is loaded.
      if (ec) { first = false; continue; }
      auto sz = std::filesystem::file_size(path, ec);
      if (ec) { first = false; continue; }
      if (first) {
        first = false;
        last = t;
        last_size = sz;
        continue;
      }
      if (t == last && sz == last_size) continue;
      // Let the linker finish writing: only act once the file is stable.
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
      auto t2 = std::filesystem::last_write_time(path, ec);
      auto sz2 = std::filesystem::file_size(path, ec);
      if (ec || t2 != t || sz2 != sz) continue;
      last = t;
      last_size = sz;
      LoadResult r = PreparePatchLibrary(path);
      std::fprintf(stderr, "[patch-reload] %s (version %u, %zu patched)%s%s\n",
                   r.ok ? "prepared" : "FAILED, keeping old code", r.version, r.applied,
                   r.ok ? "" : ": ", r.error.c_str());
    }
  }).detach();
}

}  // namespace rex::patch
