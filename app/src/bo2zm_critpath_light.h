#pragma once
#include <cstdint>
// Read directly by the generated callback-return probes; initialized before guest execution.
extern "C" bool bo2zm_critpath_light;
extern "C" void Bo2LightJobComplete(const uint8_t* base, uint32_t record);
namespace bo2zm::critpath::light {
void Install();
void Queued(const uint8_t* base, uint32_t record);
void Begin(const uint8_t* base, uint32_t group);
void End(const uint8_t* base);
void Present(uint64_t frame);
void Close();
}
