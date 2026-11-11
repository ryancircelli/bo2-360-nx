/**
 * Narrow accessor that lets the trace capture / replay code reach the
 * CommandProcessor internals it needs (register file, gamma ramp, memory)
 * without widening the CommandProcessor public interface.
 */

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

namespace rex::graphics::trace {

class TraceAccess {
 public:
  static RegisterFile& registers(CommandProcessor& cp) { return *cp.register_file_; }
  static memory::Memory* memory(CommandProcessor& cp) { return cp.memory_; }
  static uint32_t title_id(CommandProcessor& cp) {
    // KernelState::title_id() needs an executable module, which the replay
    // tool (no game loaded) does not have.
    if (!cp.kernel_state_ || !cp.kernel_state_->GetExecutableModule()) {
      return 0;
    }
    return cp.kernel_state_->title_id();
  }

  static void WriteRegister(CommandProcessor& cp, uint32_t index, uint32_t value) {
    cp.WriteRegister(index, value);
  }

  // 256 DC_LUT_30_COLOR followed by 3 * 128 DC_LUT_PWL_DATA (Xenia layout).
  static constexpr size_t kGammaRampBytes =
      sizeof(uint32_t) * 256 + sizeof(uint32_t) * 3 * 128;

  static void ReadGammaRamp(CommandProcessor& cp, uint8_t* out, uint8_t& rw_component) {
    std::memcpy(out, cp.gamma_ramp_256_entry_table_, sizeof(cp.gamma_ramp_256_entry_table_));
    std::memcpy(out + sizeof(cp.gamma_ramp_256_entry_table_), cp.gamma_ramp_pwl_rgb_,
                sizeof(cp.gamma_ramp_pwl_rgb_));
    rw_component = uint8_t(cp.gamma_ramp_rw_component_);
  }

  static void WriteGammaRamp(CommandProcessor& cp, const uint8_t* in, uint8_t rw_component) {
    std::memcpy(cp.gamma_ramp_256_entry_table_, in, sizeof(cp.gamma_ramp_256_entry_table_));
    std::memcpy(cp.gamma_ramp_pwl_rgb_, in + sizeof(cp.gamma_ramp_256_entry_table_),
                sizeof(cp.gamma_ramp_pwl_rgb_));
    cp.gamma_ramp_rw_component_ = rw_component;
  }
};

static_assert(TraceAccess::kGammaRampBytes == sizeof(uint32_t) * (256 + 3 * 128));

}  // namespace rex::graphics::trace
