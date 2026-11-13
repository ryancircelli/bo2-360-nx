// Campaign links the shared CRT without the MP HUD provenance implementation.
#define BO2_GAME_CAMPAIGN 1
#include <array>
#include <cassert>
#include <cstdio>
#include <rex/ppc/context.h>
#include "../../games/bo2/src/bo2zm_crt.cpp"
namespace bo2zm::page_writes {
std::atomic<bool> g_enabled{false};
void MarkPhysical(uint32_t, uint32_t) { assert(false); }
}
int main() {
  std::array<uint8_t, 512> memory{};
  for (unsigned i = 0; i < 124; ++i) memory[16 + i] = uint8_t(i + 1);
  PPCContext ctx{};
  ctx.r3.u64 = 256; ctx.r4.u64 = 16; ctx.r5.u64 = 124; ctx.lr = 0x8232CDB0;
  const auto original = ctx;
  rexcrt_bo2zm_memcpy(ctx, memory.data());
  assert(std::memcmp(&ctx, &original, sizeof(ctx)) == 0);
  assert(std::memcmp(memory.data() + 256, memory.data() + 16, 124) == 0);
  std::puts("PASS Campaign shared CRT links and copies without MP HUD symbols");
}
