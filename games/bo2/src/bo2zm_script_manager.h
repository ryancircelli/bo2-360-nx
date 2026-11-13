// Script selection is persistent; VM changes take effect through a clean restart.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <rex/ppc.h>

namespace bo2zm::scripts {
void Boot();
std::filesystem::path Root();
std::string Selected();
bool LegacySizes();
bool SupportsCurrentMode();
void Configured(std::string_view mod);
void Rejected(std::string_view reason);
void SkippedMap(std::string_view maps);
void Publish(PPCContext& ctx, uint8_t* base);
void EveryFrame(PPCContext& ctx, uint8_t* base);
bool Command(std::string_view name, std::string_view argument);
}  // namespace bo2zm::scripts
