// SPDX-License-Identifier: BSD-3-Clause
// v0.10 rex/ppc/func.h: PPCFunc, function mappings and indirect-call resolution over the v0.11 tables.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <rex/ppc/recompiled_code.h>

namespace rex::runtime {
inline PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  static const std::unordered_map<uint32_t, PPCFunc*> table = [] {
    std::unordered_map<uint32_t, PPCFunc*> t;
    for (const PPCFuncMapping* m = kPPCFuncMappings; m->host; ++m) t.emplace(uint32_t(m->address), m->host);
    return t;
  }();
  auto it = table.find(guest_address);
  return it == table.end() ? nullptr : it->second;
}
}  // namespace rex::runtime
