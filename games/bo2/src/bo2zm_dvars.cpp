// See bo2zm_dvars.h.

#include "bo2zm_dvars.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bo2zm_autoexec.h"

namespace {

constexpr uint32_t kFlags = 0xC;
constexpr uint32_t kCurrent = 0x18;
constexpr uint32_t kCheat = 0x80;

std::mutex g_mutex;  // registration may come from more than one thread
std::unordered_map<std::string, bo2zm::dvars::Dvar> g_dvars;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + address, &v, 4);
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

}  // namespace

void bo2zm::dvars::Registered(const uint8_t* base, uint32_t name, uint32_t dvar, int kind) {
  if (!name || !dvar) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_dvars[reinterpret_cast<const char*>(base + name)] = Dvar{dvar, kind};
}

std::optional<bo2zm::dvars::Dvar> bo2zm::dvars::Find(std::string_view name) {
  std::lock_guard lock(g_mutex);
  const auto it = g_dvars.find(std::string(name));
  if (it == g_dvars.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string bo2zm::dvars::Text(const uint8_t* base, const Dvar& dvar) {
  char text[32];
  if (dvar.kind == kBool) {
    std::snprintf(text, sizeof(text), "%d", base[dvar.pointer + kCurrent] != 0);
  } else if (dvar.kind == kFloat) {
    std::snprintf(text, sizeof(text), "%g", double(LoadFloat(base, dvar.pointer + kCurrent)));
  } else {
    std::snprintf(text, sizeof(text), "%d", int32_t(Load32(base, dvar.pointer + kCurrent)));
  }
  return text;
}

bool bo2zm::dvars::Matches(const uint8_t* base, const Dvar& dvar, std::string_view value) {
  const std::string text(value);
  if (dvar.kind == kFloat) {
    const float want = std::strtof(text.c_str(), nullptr);
    const float have = LoadFloat(base, dvar.pointer + kCurrent);
    return std::fabs(have - want) <= 1e-4f * std::max(1.0f, std::fabs(want));
  }
  const long want = std::strtol(text.c_str(), nullptr, 10);
  if (dvar.kind == kBool) {
    return (base[dvar.pointer + kCurrent] != 0) == (want != 0);
  }
  return int32_t(Load32(base, dvar.pointer + kCurrent)) == want;
}

void bo2zm::dvars::ExecuteIgnoringCheats(PPCContext& ctx, uint8_t* base, std::string_view commands,
                                         std::span<const uint32_t> dvars) {
  std::vector<uint32_t> cheat;
  for (const uint32_t p : dvars) {
    const uint32_t flags = Load32(base, p + kFlags);
    if (flags & kCheat) {
      Store32(base, p + kFlags, flags & ~kCheat);
      cheat.push_back(p);
    }
  }
  bo2zm::autoexec::ExecuteCommands(ctx, base, commands);
  for (const uint32_t p : cheat) {
    Store32(base, p + kFlags, Load32(base, p + kFlags) | kCheat);
  }
}
