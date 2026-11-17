// SPDX-License-Identifier: BSD-3-Clause
// v0.10 rex/audio/downmix.h on v0.11: master gain is the apu.gain setting.
#pragma once
#include <string>
#include <rex/settings_registry.h>
namespace rex::audio {
inline void SetOutputGain(float gain) { (void)rex::GetSettingsRegistry().SetText("apu.gain", std::to_string(gain)); }
}  // namespace rex::audio
