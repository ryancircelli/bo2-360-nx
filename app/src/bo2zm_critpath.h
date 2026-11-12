#pragma once
#include <cstdint>
namespace bo2zm::critpath {
bool Enabled();
void Install();
void Issue(uint32_t value, uint32_t completed_guest_address);
void Test(uint32_t value, bool pending);
bool IsMain();
void BeginWait();
void EndWait();
void Present(uint64_t frame);
void Close();
}
