/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/math.h>

namespace rex::graphics {

RegisterFile::RegisterFile() {
  std::memset(values, 0, sizeof(values));
}

const RegisterInfo* RegisterFile::GetRegisterInfo(uint32_t index) {
  // CommandProcessor::WriteRegister asks this for every register the guest
  // writes, thousands of times per draw. A switch with one function-local
  // static per case paid a guard check on every hit (about 5 percent of the
  // command processor thread with four-player split screen), so the table is
  // built once and looked up by index.
  static const RegisterInfo* const* const table = [] {
    static const RegisterInfo* entries[kRegisterCount] = {};
#define XE_GPU_REGISTER(index, type, name)                                  \
  {                                                                         \
    static const RegisterInfo reg_info = {RegisterInfo::Type::type, #name}; \
    if ((index) < kRegisterCount) {                                         \
      entries[(index)] = &reg_info;                                         \
    }                                                                       \
  }
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    return entries;
  }();
  return index < kRegisterCount ? table[index] : nullptr;
}

}  // namespace rex::graphics
