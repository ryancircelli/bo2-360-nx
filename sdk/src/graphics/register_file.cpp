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

  // Several context registers power up with non-zero values on real Xenos
  // hardware, so a title that reads one of them before writing it would
  // otherwise observe 0. These are the hardware reset defaults, corroborated by
  // the AMD R6xx/R7xx 3D register reference and the Mesa r600g driver, which
  // programs the same registers to the same values at context init.
  values[XE_GPU_REG_VGT_MAX_VTX_INDX] = 0x0000FFFF;
  values[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] = 0x0000FFFF;
  values[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 0x20002000;  // 8192 x 8192
  values[XE_GPU_REG_RB_STENCILREFMASK_BF] = 0x00FFFF00;
  values[XE_GPU_REG_PA_SU_POINT_SIZE] = 0x00080008;
  values[XE_GPU_REG_PA_SU_POINT_MINMAX] = 0x04000010;
  values[XE_GPU_REG_PA_SU_LINE_CNTL] = 0x00000008;
  values[XE_GPU_REG_PA_SC_LINE_CNTL] = 0x00000400;
  values[XE_GPU_REG_VGT_HOS_REUSE_DEPTH] = 0x0000000E;
  values[XE_GPU_REG_VGT_VERTEX_REUSE_BLOCK_CNTL] = 0x0000000E;
  values[XE_GPU_REG_VGT_OUT_DEALLOC_CNTL] = 0x00000010;
  // Guard-band clip/discard adjust (2.0 clip, 1.0 discard); hardwired on
  // hardware.
  values[XE_GPU_REG_PA_CL_GB_VERT_CLIP_ADJ] = 0x40000000;  // 2.0f
  values[XE_GPU_REG_PA_CL_GB_VERT_DISC_ADJ] = 0x3F800000;  // 1.0f
  values[XE_GPU_REG_PA_CL_GB_HORZ_CLIP_ADJ] = 0x40000000;  // 2.0f
  values[XE_GPU_REG_PA_CL_GB_HORZ_DISC_ADJ] = 0x3F800000;  // 1.0f
  values[XE_GPU_REG_PA_SC_AA_MASK] = 0x0000FFFF;
  // Tessellation levels also reset to 1.0f, but the backends apply the
  // effective factor as (register + 1.0f), so they stay 0 here.
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
