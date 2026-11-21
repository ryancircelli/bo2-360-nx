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

LoadResult LoadPatchLibrary(const std::filesystem::path& path) {
  static std::atomic<uint32_t> version{0};
  LoadResult result;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    result.error = "patch library not found: " + path.string();
    return result;
  }
  uint32_t v = version.load() + 1;
  auto tmp = std::filesystem::temp_directory_path(ec) /
             ("rex_patch_" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(v) +
              path.extension().string());
  std::filesystem::copy_file(path, tmp, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    result.error = "cannot copy patch library: " + ec.message();
    return result;
  }
  // Leaked on purpose (never dlclose); see header.
  auto* lib = new platform::DynamicLibrary();
  if (!lib->Load(tmp, platform::SymbolResolution::kImmediate)) {
    delete lib;  // closes a failed (null) handle only
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
  // Resolve everything first so a bad library changes nothing.
  std::vector<std::pair<PatchSlot*, PPCFunc*>> bind;
  for (size_t i = 0; i < count; ++i) {
    PatchSlot* slot = SlotTable::Get().Find(entries[i].name);
    if (!slot) {
      result.unmatched.push_back(entries[i].name);
    } else {
      bind.emplace_back(slot, entries[i].fn);
    }
  }
  // Slots absent from this library go back to the original body.
  SlotTable::Get().ResetAll();
  for (auto& [slot, fn] : bind) {
    slot->current.store(fn, std::memory_order_release);
  }
  version.store(v);
  result.ok = true;
  result.version = v;
  result.applied = bind.size();
  return result;
}

}  // namespace rex::patch
