// Structural checks for writable T6 Xbox 360 script objects. This validates
// linker metadata, not the behavior of the script's bytecode.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bo2zm::gsc {

inline uint16_t Read16(std::span<const uint8_t> b, size_t p) {
  return uint16_t((uint16_t(b[p]) << 8) | b[p + 1]);
}
inline uint32_t Read32(std::span<const uint8_t> b, size_t p) {
  return (uint32_t(b[p]) << 24) | (uint32_t(b[p + 1]) << 16) |
         (uint32_t(b[p + 2]) << 8) | b[p + 3];
}
inline void Write32(uint8_t* p, uint32_t n) {
  p[0] = uint8_t(n >> 24); p[1] = uint8_t(n >> 16);
  p[2] = uint8_t(n >> 8); p[3] = uint8_t(n);
}

inline bool ScriptPath(std::string_view name) {
  if (!(name == "maps/mp" || name.starts_with("maps/mp/") ||
        name == "common_scripts" || name.starts_with("common_scripts/")) || name.ends_with('/'))
    return false;
  size_t at = 0;
  while (at < name.size()) {
    const auto end = name.find('/', at);
    const auto part = name.substr(at, end == name.npos ? name.size() - at : end - at);
    if (part.empty() || part == "." || part == "..") return false;
    for (const auto c : part) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-')) return false;
    }
    if (end == name.npos) break;
    at = end + 1;
  }
  return true;
}

inline bool ScriptName(std::string_view name) {
  return (name.starts_with("maps/mp/") || name.starts_with("common_scripts/")) &&
         name.ends_with(".gsc") && name.size() <= 127 && ScriptPath(name);
}

struct ObjectInfo {
  std::string name;
  uint32_t code_size = 0;
  bool legacy_size = false;
  uint32_t legacy_header_strings = 0;
};

inline bool Validate(std::span<const uint8_t> b, bool allow_legacy,
                     ObjectInfo& result, std::string& error) {
  result = {};
  auto fail = [&](const char* why) { error = why; return false; };
  constexpr uint8_t magic[] = {0x80, 'G', 'S', 'C', 13, 10, 0, 6};
  if (b.size() < 64 || b.size() > 1024 * 1024) return fail("object size is outside limits");
  if (!std::equal(std::begin(magic), std::end(magic), b.begin()))
    return fail("expected T6 Xbox 360 compiled GSC");
  const size_t code = Read32(b, 0x14);
  size_t size = Read32(b, 0x2C);
  if (!size && allow_legacy) {
    const size_t exports = Read32(b, 0x1C);
    if (exports <= code) return fail("legacy code interval is invalid");
    size = exports - code;
    result.legacy_size = true;
  }
  auto range = [&](size_t p, size_t n) { return p <= b.size() && n <= b.size() - p; };
  if (code < 64 || size < 8 || !range(code, size)) return fail("invalid code interval");
  const size_t code_end = code + size;
  auto metadata = [&](size_t p, size_t n) {
    return p >= 64 && range(p, n) && (p + n <= code || p >= code_end);
  };
  using Interval = std::pair<size_t, size_t>;
  std::vector<Interval> tables, strings;
  auto table = [&](size_t p, size_t n) {
    if (!metadata(p, n)) return false;
    tables.emplace_back(p, p + n);
    return true;
  };
  auto text = [&](size_t p, std::string* out = nullptr, size_t limit = 1024,
                  bool header_literal = false) {
    // Some legacy objects use the immutable, NUL-terminated magic bytes as a
    // literal (a field name, a string or an export name): the engine reads it at
    // offset 0 like any other. Preserve those bytes only under explicit compatibility.
    if (p == 0 && header_literal) {
      strings.emplace_back(0, 7);
      ++result.legacy_header_strings;
      if (out) out->assign(reinterpret_cast<const char*>(b.data()), 6);
      return true;
    }
    if (!metadata(p, 1)) return false;
    size_t end = p;
    while (range(end, 1) && end - p <= limit && b[end]) ++end;
    if (!range(end, 1) || end - p > limit || !metadata(p, end - p + 1)) return false;
    strings.emplace_back(p, end + 1);
    if (out) out->assign(reinterpret_cast<const char*>(b.data() + p), end - p);
    return true;
  };
  if (!text(Read16(b, 0x30), &result.name) || !ScriptName(result.name))
    return fail("invalid internal script name");
  // Animation trees, external relocations and profiling records need additional
  // engine-specific validation before accepting objects that use them.
  if (b[0x3D] || Read16(b, 0x38) || Read16(b, 0x3A))
    return fail("animation, relocation or profile records are unsupported");
  size_t p = Read32(b, 0x0C), count = b[0x3C];
  if (count && !table(p, count * 4)) return fail("include table is out of bounds");
  for (size_t i = 0; i < count; ++i) {
    std::string include;
    if (!text(Read32(b, p + i * 4), &include) || include.empty() || include.size() > 127)
      return fail("invalid include name");
  }
  p = Read32(b, 0x1C); count = Read16(b, 0x34);
  if (!count || !table(p, count * 12)) return fail("invalid export table");
  for (size_t i = 0; i < count; ++i, p += 12) {
    const size_t target = Read32(b, p + 4);
    std::string name;
    if (target < code || target >= code_end || !text(Read16(b, p + 8), &name, 1024, allow_legacy) ||
        name.empty()) return fail("invalid export");
  }
  p = Read32(b, 0x18); count = Read16(b, 0x32);
  for (size_t i = 0; i < count; ++i) {
    if (!metadata(p, 4) || !text(Read16(b, p), nullptr, 16384, allow_legacy) || b[p + 3] > 1)
      return fail("invalid string fixup");
    const size_t uses = b[p + 2];
    if (!table(p, 4 + uses * 4)) return fail("string fixup list is out of bounds");
    for (size_t j = 0; j < uses; ++j) {
      const size_t target = Read32(b, p + 4 + j * 4);
      if (target < code || target > code_end - 2) return fail("string patch is outside code");
    }
    p += 4 + uses * 4;
  }
  p = Read32(b, 0x20); count = Read16(b, 0x36);
  for (size_t i = 0; i < count; ++i) {
    if (!metadata(p, 8)) return fail("invalid import");
    const auto space = Read16(b, p + 2);
    // Older compilers share the header's zero flags byte as the empty namespace.
    const bool empty_space = space == 0x3E && !b[space];
    if (!text(Read16(b, p)) || (!empty_space && !text(space))) return fail("invalid import");
    const unsigned flags = b[p + 7], kind = flags & 15;
    if (kind < 1 || kind > 5 || (flags & 0xE0)) return fail("unsupported import kind or flags");
    const size_t uses = Read16(b, p + 4);
    if (!table(p, 8 + uses * 4)) return fail("import patch list is out of bounds");
    for (size_t j = 0; j < uses; ++j) {
      const size_t target = Read32(b, p + 8 + j * 4);
      // References only patch a pointer. Calls also rewrite the opcode and
      // parameter byte, with a slightly different pointer-alignment rule.
      const auto pointer = (target + (kind == 1 ? 4 : 5)) & ~size_t(3);
      if (target < code || target >= code_end || pointer + 4 > code_end ||
          (kind != 1 && target + 2 > code_end))
        return fail("import patch is outside code");
    }
    p += 8 + uses * 4;
  }
  // Export flags are modified during linking. An overlapping table or string
  // could otherwise change metadata after its patch addresses were checked.
  std::sort(tables.begin(), tables.end());
  for (size_t i = 1; i < tables.size(); ++i)
    if (tables[i].first < tables[i - 1].second) return fail("overlapping metadata tables");
  for (const auto& s : strings) {
    const auto t = std::lower_bound(tables.begin(), tables.end(), s.first,
        [](const Interval& interval, size_t start) { return interval.second <= start; });
    if (t != tables.end() && t->first < s.second) return fail("string overlaps a metadata table");
  }
  result.code_size = uint32_t(size);
  error.clear();
  return true;
}
}  // namespace bo2zm::gsc
