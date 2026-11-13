// The System Link mod menu tables (games/bo2/src/bo2zm_mod_hud_wire.h): encoding, every refusal of a malformed body, and
// which element records match an entry.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "../../games/bo2/src/bo2zm_mod_hud_wire.h"

namespace wire = bo2zm::mod_hud_wire;

namespace {
std::array<uint8_t, 124> Record(uint8_t type, uint16_t body, uint16_t label) {
  std::array<uint8_t, 124> record{};
  for (size_t i = 0; i < record.size(); ++i) record[i] = uint8_t(i * 7 + 3);
  record[0x70] = type;
  wire::Put16(record.data() + 0x64, body);
  wire::Put16(record.data() + 0x56, label);
  return record;
}
wire::Table Sample() {
  wire::Table table;
  table.session = 0x12345679;
  table.epoch = 7;
  table.revision = 41;
  table.client = 3;
  const auto text = Record(wire::kText, 300, 301), material = Record(wire::kMaterial, 0, 0);
  table.entries[0] = {0, wire::IdOf(text.data())};
  table.entries[1] = {31, wire::IdOf(material.data())};
  table.entries[2] = {61, wire::IdOf(text.data())};
  table.count = 3;
  return table;
}
bool DecodeBody(const std::array<uint8_t, wire::kMaxBody>& body, size_t size, uint8_t count, wire::Table& out,
                uint8_t format = wire::kFormat) {
  return wire::Decode(format, count, 41, body.data(), size, out);
}
}  // namespace

int main() {
  // Round trip, and the layout the side channel documents.
  const auto table = Sample();
  std::array<uint8_t, wire::kMaxBody> body{};
  const size_t size = wire::Encode(table, body.data());
  assert(size == wire::kBodyHeader + 3 * wire::kEntryBytes);
  assert(wire::Get32(body.data()) == table.session && wire::Get32(body.data() + 4) == 7 && wire::Get16(body.data() + 8) == 3);
  assert(body[10] == 0 && body[10 + wire::kEntryBytes] == 31 && body[10 + 2 * wire::kEntryBytes] == 61);
  wire::Table decoded;
  assert(DecodeBody(body, size, 3, decoded));
  assert(decoded == table && decoded.session == table.session && decoded.epoch == table.epoch &&
         decoded.client == table.client && decoded.revision == 41);
  // The largest table fits the side channel's datagram (1400 bytes, 12 of them the header).
  wire::Table full;
  full.count = wire::kSlots;
  for (uint8_t i = 0; i < wire::kSlots; ++i) full.entries[i] = {i, table.entries[0].id};
  assert(wire::Encode(full, body.data()) == wire::kMaxBody && 12 + wire::kMaxBody <= 1400);
  // An empty table (a menu closed).
  wire::Table empty = table;
  empty.count = 0;
  assert(wire::Encode(empty, body.data()) == wire::kBodyHeader && DecodeBody(body, wire::kBodyHeader, 0, decoded) &&
         decoded.count == 0);

  // Refusals: another format, a size that does not match the count, a client or slot out of range, a slot twice or
  // out of order, an element type the fitting does not take, and tables Encode will not write.
  const size_t good = wire::Encode(table, body.data());
  assert(!DecodeBody(body, good, 3, decoded, wire::kFormat + 1));
  assert(!DecodeBody(body, good, 3, decoded, 0));
  assert(!DecodeBody(body, good - 1, 3, decoded) && !DecodeBody(body, good + 1, 3, decoded));
  assert(!DecodeBody(body, good, 2, decoded) && !DecodeBody(body, good, 63, decoded));
  auto broken = body;
  wire::Put16(broken.data() + 8, wire::kMaxClient); assert(!DecodeBody(broken, good, 3, decoded));
  broken = body; broken[10 + 2 * wire::kEntryBytes] = 62; assert(!DecodeBody(broken, good, 3, decoded));
  broken = body; broken[10 + wire::kEntryBytes] = 0; assert(!DecodeBody(broken, good, 3, decoded));   // slot twice
  broken = body; broken[10] = 40; assert(!DecodeBody(broken, good, 3, decoded));                     // out of order
  broken = body; broken[10 + 1 + 8] = 2; assert(!DecodeBody(broken, good, 3, decoded));              // type 2
  broken = body; broken[10 + 1 + 8] = 0; assert(!DecodeBody(broken, good, 3, decoded));              // empty record
  wire::Table bad = table;
  bad.client = wire::kMaxClient; assert(wire::Encode(bad, body.data()) == 0);
  bad = table; bad.count = wire::kSlots + 1; assert(wire::Encode(bad, body.data()) == 0);
  // A refused body leaves the output as it was.
  decoded = {};
  broken = body; wire::Encode(table, broken.data()); broken[10] = 62;
  assert(!DecodeBody(broken, good, 3, decoded) && decoded.count == 0);

  // Matching. A text element: the same type, label and text, wherever it moved or however it animates.
  auto text = Record(wire::kText, 300, 301);
  const auto text_id = wire::IdOf(text.data());
  assert(wire::Matches(text.data(), text_id));
  for (uint32_t offset : {0x00u, 0x04u, 0x0Cu, 0x40u, 0x58u, 0x71u, 0x74u}) {
    auto moved = text; moved[offset] ^= 0x40;
    assert(wire::Matches(moved.data(), text_id));
  }
  for (uint32_t offset : {0x56u, 0x57u, 0x64u, 0x65u, 0x70u}) {
    auto other = text; other[offset] ^= 0x01;
    assert(!wire::Matches(other.data(), text_id));
  }
  // A material: sort, size, type, font, alignment and material; its position and animation do not matter.
  auto material = Record(wire::kMaterial, 0, 0);
  const auto material_id = wire::IdOf(material.data());
  assert(wire::Matches(material.data(), material_id));
  for (uint32_t offset : {0x00u, 0x04u, 0x0Cu, 0x10u, 0x28u, 0x56u, 0x64u}) {
    auto moved = material; moved[offset] ^= 0x40;
    assert(wire::Matches(moved.data(), material_id));
  }
  for (uint32_t offset = 0; offset < 124; ++offset) {
    const bool identifies = (offset >= 0x40 && offset < 0x44) || (offset >= 0x58 && offset < 0x5C) ||
                            (offset >= 0x70 && offset < 0x76);
    if (!identifies) continue;
    auto other = material; other[offset] ^= 0x01;
    assert(!wire::Matches(other.data(), material_id));
  }
  // A text record never matches a material entry, nor the other way round; other types match nothing.
  assert(!wire::Matches(text.data(), material_id) && !wire::Matches(material.data(), text_id));
  auto other_type = text; other_type[0x70] = 2;
  assert(!wire::Matches(other_type.data(), wire::IdOf(other_type.data())));
  // Find by slot.
  assert(wire::Find(table, 31) == &table.entries[1] && !wire::Find(table, 30) && !wire::Find(empty, 0));
  std::puts("PASS menu tables: round trip, largest table fits a datagram, empty table, every malformed body refused, "
            "text and material matching (moved/animated still match, identity changes do not), find by slot");
}
