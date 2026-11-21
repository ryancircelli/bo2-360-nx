#pragma once

#include <cmath>
#include <cstddef>
#include <utility>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace rex::graphics::metal {
// One scalar per line: ps|vs HEX_HASH REGISTER COMPONENT FLOAT.
// Parse transactionally: a bad edit leaves the previous set working.
struct MetalConstantOverride {
  bool pixel;
  uint64_t hash;
  unsigned reg, component;
  float value;
};
inline bool ParseMetalConstantOverrides(
    const std::string& text, std::vector<MetalConstantOverride>& output) {
  std::vector<MetalConstantOverride> candidate;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    line = line.substr(0, line.find('#'));
    std::istringstream fields(line);
    std::string stage, hash, extra;
    MetalConstantOverride item{};
    if (!(fields >> stage)) continue;
    if (!(fields >> hash >> item.reg >> item.component >> item.value) ||
        (fields >> extra) || (stage != "ps" && stage != "vs") ||
        item.reg > 255 || item.component > 3 || !std::isfinite(item.value)) return false;
    size_t used = 0;
    try { item.hash = std::stoull(hash, &used, 16); } catch (...) { return false; }
    if (used != hash.size() || hash.front() == '-') return false;
    item.pixel = stage == "ps";
    candidate.push_back(item);
    if (candidate.size() > 4096) return false;
  }
  output = std::move(candidate);
  return true;
}
inline void ApplyMetalConstantOverrides(
    const std::vector<MetalConstantOverride>& overrides, bool pixel,
    uint64_t hash, const uint64_t* bitmap, uint8_t* data, size_t size) {
  for (const auto& item : overrides) {
    if (item.pixel != pixel || item.hash != hash ||
        !(bitmap[item.reg / 64] & (uint64_t(1) << (item.reg % 64)))) continue;
    size_t packed = 0;
    for (unsigned r = 0; r < item.reg; ++r)
      packed += (bitmap[r / 64] >> (r % 64)) & 1;
    size_t offset = (packed * 4 + item.component) * sizeof(float);
    if (offset + sizeof(float) <= size) std::memcpy(data + offset, &item.value, sizeof(float));
  }
}
}  // namespace rex::graphics::metal
