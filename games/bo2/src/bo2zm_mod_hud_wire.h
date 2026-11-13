// The mod menus of a System Link game, from its host to the consoles that joined it.
//
// A console that joined another's game runs none of the game's scripts: a mod's menus are made on the host, and the
// joined console only draws the elements its snapshots carry. It cannot tell a menu's elements from the game's own, so
// the host, which saw each element made by the mod's menu code (bo2zm_mod_hud_fit.cpp), sends it a table per player:
// which of the playerstate's HUD element slots hold a menu element, and what identifies that element there. The
// joined console fits an element only while its slot in the snapshot holds exactly that element.
//
// The table travels in the side channel's messages (bo2zm_cheat_sync.cpp: type 9, its aux byte the format version
// below, its sequence the table's revision). Body, numbers big-endian:
//   0 host session (u32)   4 host epoch (u32)   8 client number (u16)   10 entries (count from the header)
// Each entry: slot (u8: 0..30 the first group, 31..61 the second), then the element's identifying bytes (kIdBytes).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bo2zm::mod_hud_wire {

constexpr uint8_t kFormat = 1;          // the tables' format version; other versions are ignored
constexpr uint32_t kSlots = 62;         // two groups of 31 HUD elements in a playerstate
constexpr uint32_t kRecordSize = 124;   // one HUD element
constexpr uint16_t kMaxClient = 1022;
constexpr uint8_t kText = 1, kMaterial = 8;  // the element types the fitting takes (record +0x70)
// What identifies an element (the fields a client's snapshot carries as the server has them): sort, size, type,
// font, alignment and material (+0x40, +0x58, +0x70..+0x75), the label and the text (+0x56, +0x64).
constexpr uint32_t kIdFields[][2] = {{0x40, 4}, {0x58, 4}, {0x70, 6}, {0x56, 2}, {0x64, 2}};
constexpr size_t kIdBytes = 18;
constexpr size_t kEntryBytes = 1 + kIdBytes;
constexpr size_t kBodyHeader = 10;
constexpr size_t kMaxBody = kBodyHeader + kSlots * kEntryBytes;

using Id = std::array<uint8_t, kIdBytes>;
struct Entry {
  uint8_t slot = 0;
  Id id{};
  bool operator==(const Entry&) const = default;
};
struct Table {
  uint32_t session = 0, epoch = 0, revision = 0;
  uint16_t client = 0;
  uint8_t count = 0;
  std::array<Entry, kSlots> entries{};
  bool operator==(const Table& other) const {
    if (count != other.count) return false;
    for (uint8_t i = 0; i < count; ++i)
      if (!(entries[i] == other.entries[i])) return false;
    return true;  // the content; who sent it and when is compared apart
  }
};

// The identifying bytes of a 124-byte element record, in guest byte order.
inline Id IdOf(const uint8_t* record) {
  Id id{};
  size_t at = 0;
  for (const auto& field : kIdFields) {
    std::memcpy(id.data() + at, record + field[0], field[1]);
    at += field[1];
  }
  return id;
}
inline uint8_t IdType(const Id& id) { return id[8]; }  // +0x70, after sort and size
// Whether an element record is the one an entry names. A text element by its type, label and text (its other fields
// may move or animate); a material by sort, size, type, font, alignment and material. Other types never match.
inline bool Matches(const uint8_t* record, const Id& id) {
  const Id now = IdOf(record);
  const uint8_t type = IdType(id);
  if (now[8] != type) return false;
  if (type == kText) return std::memcmp(now.data() + 14, id.data() + 14, 4) == 0;
  if (type == kMaterial) return std::memcmp(now.data(), id.data(), 14) == 0;
  return false;
}

inline void Put16(uint8_t* at, uint16_t v) { at[0] = uint8_t(v >> 8); at[1] = uint8_t(v); }
inline void Put32(uint8_t* at, uint32_t v) { for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (24 - 8 * i)); }
inline uint16_t Get16(const uint8_t* at) { return uint16_t(at[0] << 8 | at[1]); }
inline uint32_t Get32(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3];
}

// The body of a table message; its size (0: the table is not valid). out holds kMaxBody bytes.
inline size_t Encode(const Table& table, uint8_t* out) {
  if (table.count > kSlots || table.client >= kMaxClient) return 0;
  Put32(out, table.session);
  Put32(out + 4, table.epoch);
  Put16(out + 8, table.client);
  uint8_t* at = out + kBodyHeader;
  for (uint8_t i = 0; i < table.count; ++i, at += kEntryBytes) {
    at[0] = table.entries[i].slot;
    std::memcpy(at + 1, table.entries[i].id.data(), kIdBytes);
  }
  return size_t(at - out);
}
// A received body, with the header's format (aux), count and sequence. false for anything malformed: another format,
// a size that does not match the count, a client or slot out of range, a slot twice, entries out of slot order, or an
// element type the fitting does not take.
inline bool Decode(uint8_t format, uint8_t count, uint32_t sequence, const uint8_t* body, size_t size, Table& out) {
  if (format != kFormat || count > kSlots || size != kBodyHeader + size_t(count) * kEntryBytes) return false;
  Table table;
  table.session = Get32(body);
  table.epoch = Get32(body + 4);
  table.client = Get16(body + 8);
  table.revision = sequence;
  table.count = count;
  if (table.client >= kMaxClient) return false;
  const uint8_t* at = body + kBodyHeader;
  int previous = -1;
  for (uint8_t i = 0; i < count; ++i, at += kEntryBytes) {
    auto& entry = table.entries[i];
    entry.slot = at[0];
    std::memcpy(entry.id.data(), at + 1, kIdBytes);
    const uint8_t type = IdType(entry.id);
    if (entry.slot >= kSlots || int(entry.slot) <= previous || (type != kText && type != kMaterial)) return false;
    previous = entry.slot;
  }
  out = table;
  return true;
}
// The entry for a slot, or nullptr.
inline const Entry* Find(const Table& table, uint32_t slot) {
  for (uint8_t i = 0; i < table.count; ++i)
    if (table.entries[i].slot == slot) return &table.entries[i];
  return nullptr;
}

}  // namespace bo2zm::mod_hud_wire
