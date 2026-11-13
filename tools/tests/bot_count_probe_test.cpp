// Compile the production bot dvar boundary with an authored guest-call transport.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <rex/ppc/context.h>
#include "../../games/bo2/src/bo2zm_bots.h"

// PRODUCTION_DVAR_CONSTANTS

uint32_t bot_dvar_text = 0;
unsigned allocations = 0, registrations = 0, writes = 0, reports = 0;
int requested_default = -1, registered_max = -1, effective = -1;
bool allocation_fails = false;
std::vector<std::string> queried;
std::map<std::string, std::string> engine{{"com_maxclients", "4"}, {"party_maxplayers", "1"}};
std::array<uint8_t, 65536> memory{};
namespace rex {
struct Memory {
  uint32_t SystemHeapAlloc(size_t bytes, size_t alignment) {
    assert(bytes == 128 && alignment == 16);
    ++allocations;
    return allocation_fails ? 0 : 0x2000;
  }
};
struct Runtime {
  static Runtime* instance() { static Runtime value; return &value; }
  Memory* memory() { static Memory value; return &value; }
};
}
template <typename... Args> void Log(const Args&...) { ++reports; }
#define REXLOG_INFO(...) Log(__VA_ARGS__)

namespace bo2zm::guest {
template <uint32_t address> void Call(PPCContext& ctx, uint8_t* base) {
  if constexpr (address == kRegisterInt) {
    assert(std::strcmp(reinterpret_cast<char*>(base + ctx.r3.u32), "bo2_zm_bots_count") == 0);
    assert(ctx.r5.u64 == 0 && ctx.r7.u64 == 0 && ctx.r8.u64 == ctx.r3.u64);
    requested_default = int(ctx.r4.u32);
    registered_max = int(ctx.r6.u32);
    effective = std::clamp(requested_default, 0, registered_max);
    ++registrations;
    ctx.r3.u64 = 0x3000;
  } else if constexpr (address == kSetInt) {
    assert(ctx.r3.u64 == 0x3000);
    effective = std::clamp(int(ctx.r4.u32), 0, registered_max);
    ++writes;
  } else if constexpr (address == kFindDvar) {
    const std::string name(reinterpret_cast<char*>(base + ctx.r3.u32));
    queried.push_back(name);
    if (name == "bo2_zm_bots_count") ctx.r3.u64 = 0x3000;
    else if (name == "com_maxclients") ctx.r3.u64 = 0x3100;
    else if (name == "party_maxplayers") ctx.r3.u64 = 0x3200;
    else { assert(name == "sv_maxclients"); ctx.r3.u64 = 0; }
  } else if constexpr (address == kDvarString) {
    std::string text;
    if (ctx.r3.u64 == 0x3000) text = std::to_string(effective);
    else if (ctx.r3.u64 == 0x3100) text = engine.at("com_maxclients");
    else { assert(ctx.r3.u64 == 0x3200); text = engine.at("party_maxplayers"); }
    std::strcpy(reinterpret_cast<char*>(base + 0x4000), text.c_str());
    ctx.r3.u64 = 0x4000;
  } else {
    static_assert(address == 0, "unexpected guest call");
  }
  ctx.r7.u64 = 0xabcdef; // Both production functions must restore the caller's context.
}
}

// PRODUCTION_BOT_DVAR_FUNCTIONS

void Reset() {
  allocations = registrations = writes = reports = 0;
  bot_dvar_text = 0;
  effective = registered_max = requested_default = -1;
  queried.clear();
  allocation_fails = false;
}
int main() {
  const auto original_engine = engine;
  PPCContext input{};
  input.r1.u64 = 0xf000;
  input.r3.u64 = 7;
  input.r7.u64 = 17;
  input.lr = 23;
  for (int count = 0; count <= 4; ++count) {
    Reset();
    PPCContext ctx = input;
    assert(SetBotCount(ctx, memory.data(), count));
    assert(std::memcmp(&ctx, &input, sizeof(ctx)) == 0);
    assert(allocations == 1 && registrations == 1 && writes == 1);
    assert(requested_default == count && effective == count);
    assert(registered_max == (count == 4 ? 4 : 3));
    assert(engine == original_engine);
    if (count == 4) {
      std::sort(queried.begin(), queried.end());
      assert((queried == std::vector<std::string>{"bo2_zm_bots_count", "com_maxclients",
                                                "party_maxplayers", "sv_maxclients"}));
      assert(reports == 1);
    } else assert(queried.empty() && reports == 0);
  }
  for (int invalid : {-1, 5, 8, 100}) {
    Reset();
    PPCContext ctx = input;
    assert(!SetBotCount(ctx, memory.data(), invalid));
    assert(allocations == 0 && registrations == 0 && writes == 0 && reports == 0);
    assert(std::memcmp(&ctx, &input, sizeof(ctx)) == 0 && engine == original_engine);
  }
  Reset();
  allocation_fails = true;
  PPCContext ctx = input;
  assert(!SetBotCount(ctx, memory.data(), 4));
  assert(registrations == 0 && writes == 0 && reports == 0);
  assert(std::memcmp(&ctx, &input, sizeof(ctx)) == 0);
  std::puts("Actual bot dvar boundary: test four survives, play cap stays three, engine capacity untouched");
}
