// SPDX-License-Identifier: 0BSD
// BO2 System Link cheat-sync side channel: wire format of the nine datagram types (spec G8-cheat-sync).
// Pure encode/decode with full validation; no sockets, no game access. All integers big-endian.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace bo2cs {

constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxDatagram = 1400;
constexpr size_t kReceiveBuffer = 2048;
constexpr uint8_t kVersion = 2;
constexpr uint8_t kMenuFormat = 1;
constexpr size_t kOptionCount = 13;
constexpr uint8_t kOptionMax[kOptionCount] = {1, 2, 1, 5, 1, 1, 1, 1, 1, 1, 1, 3, 1};
constexpr size_t kMaxArgument = 64;
constexpr size_t kMaxResultText = 200;
constexpr size_t kMaxCatalogPart = 1200;
constexpr size_t kMaxCatalogParts = 16;
constexpr size_t kMaxWorldBody = 512;
constexpr size_t kMaxWorldText = 510;
constexpr size_t kMaxMenuEntries = 62;
constexpr size_t kIdentitySize = 18;

enum Type : uint8_t {
  kOptions = 1, kAnswer, kCommand, kResult, kCatalog, kWorld, kBlasts, kMenuAsk, kMenus
};

using Bytes = std::vector<uint8_t>;

struct Header {
  uint8_t type = 0, count = 0, aux = 0;
  uint32_t sequence = 0;
};

namespace detail {
inline void Put16(Bytes& b, uint16_t v) { b.push_back(v >> 8); b.push_back(v); }
inline void Put32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
inline void Put64(Bytes& b, uint64_t v) { for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
inline uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t Get64(const uint8_t* p) { return uint64_t(Get32(p)) << 32 | Get32(p + 4); }
inline Bytes Begin(uint8_t type, uint8_t count, uint8_t aux, uint32_t seq) {
  Bytes b = {'B', 'Z', 'C', 'S', kVersion, type, count, aux};
  Put32(b, seq);
  return b;
}
inline bool CommandChar(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.' || c == ' ' || c == ',' || c == '(' || c == ')' || c == '\'' || c == '-';
}
inline bool WorldChar(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.' || c == '=' || c == ';';
}
}  // namespace detail

inline bool ValidCommandText(const std::string& s, size_t max) {
  if (s.size() > max) return false;
  for (unsigned char c : s) if (!detail::CommandChar(c)) return false;
  return true;
}
inline bool ValidWorldText(const std::string& s) {
  if (s.size() > kMaxWorldText) return false;
  for (unsigned char c : s) if (!detail::WorldChar(c)) return false;
  return true;
}

// Parses magic/version/type; unknown types and wrong magic/version give nullopt. The body starts at +12.
inline std::optional<Header> ParseHeader(const uint8_t* d, size_t n) {
  if (n < kHeaderSize || std::memcmp(d, "BZCS", 4) != 0 || d[4] != kVersion || d[5] < 1 || d[5] > 9)
    return std::nullopt;
  return Header{d[5], d[6], d[7], detail::Get32(d + 8)};
}

// ---- 1 options -------------------------------------------------------------------------------
struct PlayerOptions { uint64_t xuid = 0; uint8_t value[kOptionCount] = {}; };
struct Options { uint32_t sequence = 0, catalog_rev = 0, world_rev = 0; std::vector<PlayerOptions> players; };

inline bool OptionsInRange(const uint8_t* v) {
  for (size_t i = 0; i < kOptionCount; ++i) if (v[i] > kOptionMax[i]) return false;
  return true;
}
inline std::optional<Bytes> EncodeOptions(const Options& o) {
  if (o.players.empty() || o.players.size() > 4) return std::nullopt;
  Bytes b = detail::Begin(kOptions, uint8_t(o.players.size()), kOptionCount, o.sequence);
  detail::Put32(b, o.catalog_rev);
  detail::Put32(b, o.world_rev);
  for (const auto& p : o.players) {
    if (!p.xuid || !OptionsInRange(p.value)) return std::nullopt;
    detail::Put64(b, p.xuid);
    b.insert(b.end(), p.value, p.value + kOptionCount);
  }
  return b;
}
// Receiver side keeps bad entries out of the list individually (a bad entry does not stop the others),
// but framing errors drop the whole datagram.
struct OptionsDecoded { Options options; std::vector<bool> entry_valid; };
inline std::optional<OptionsDecoded> DecodeOptions(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kOptions || h->count < 1 || h->count > 4 || h->aux != kOptionCount) return std::nullopt;
  if (n != kHeaderSize + 8 + 21u * h->count) return std::nullopt;
  OptionsDecoded r;
  r.options.sequence = h->sequence;
  r.options.catalog_rev = detail::Get32(d + 12);
  r.options.world_rev = detail::Get32(d + 16);
  for (unsigned i = 0; i < h->count; ++i) {
    const uint8_t* e = d + 20 + 21 * i;
    PlayerOptions p;
    p.xuid = detail::Get64(e);
    std::memcpy(p.value, e + 8, kOptionCount);
    r.entry_valid.push_back(p.xuid != 0 && OptionsInRange(p.value));
    r.options.players.push_back(p);
  }
  return r;
}

// ---- 2 answer --------------------------------------------------------------------------------
struct AnswerEntry { uint64_t xuid = 0; uint8_t client = 255; };
struct Answer { uint32_t sequence = 0, catalog_rev = 0, world_rev = 0; std::vector<AnswerEntry> entries; };
inline std::optional<Bytes> EncodeAnswer(const Answer& a) {
  if (a.entries.empty() || a.entries.size() > 4) return std::nullopt;
  Bytes b = detail::Begin(kAnswer, uint8_t(a.entries.size()), 0, a.sequence);
  detail::Put32(b, a.catalog_rev);
  detail::Put32(b, a.world_rev);
  for (const auto& e : a.entries) { detail::Put64(b, e.xuid); b.push_back(e.client); }
  return b;
}
inline std::optional<Answer> DecodeAnswer(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kAnswer || h->count < 1 || h->count > 4 || h->aux != 0) return std::nullopt;
  if (n != kHeaderSize + 8 + 9u * h->count) return std::nullopt;
  Answer a;
  a.sequence = h->sequence;
  a.catalog_rev = detail::Get32(d + 12);
  a.world_rev = detail::Get32(d + 16);
  for (unsigned i = 0; i < h->count; ++i) a.entries.push_back({detail::Get64(d + 20 + 9 * i), d[28 + 9 * i]});
  return a;
}

// ---- 3 command -------------------------------------------------------------------------------
struct Command { uint32_t sequence = 0; uint8_t id = 0; uint64_t xuid = 0; bool pack_a_punch = false; std::string argument; };
inline std::optional<Bytes> EncodeCommand(const Command& c) {
  if (c.id < 1 || c.id > 14 || !c.xuid || !ValidCommandText(c.argument, kMaxArgument)) return std::nullopt;
  Bytes b = detail::Begin(kCommand, 1, c.id, c.sequence);
  detail::Put64(b, c.xuid);
  b.push_back(c.pack_a_punch ? 1 : 0);
  b.push_back(uint8_t(c.argument.size()));
  b.insert(b.end(), c.argument.begin(), c.argument.end());
  return b;
}
// Framing errors drop the datagram (nullopt). A structurally decodable command with bad text is returned with
// text_valid=false so the host can refuse it.
struct CommandDecoded { Command command; bool text_valid = true; };
inline std::optional<CommandDecoded> DecodeCommand(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kCommand || h->count != 1 || h->aux < 1 || h->aux > 14) return std::nullopt;
  if (n < kHeaderSize + 10) return std::nullopt;
  const uint8_t len = d[21];
  const uint8_t flag = d[20];
  if (len > kMaxArgument || flag > 1 || n != kHeaderSize + 10u + len) return std::nullopt;
  CommandDecoded r;
  r.command = {h->sequence, h->aux, detail::Get64(d + 12), flag == 1, std::string((const char*)d + 22, len)};
  r.text_valid = r.command.xuid != 0 && ValidCommandText(r.command.argument, kMaxArgument);
  return r;
}

// ---- 4 result --------------------------------------------------------------------------------
struct Result { uint32_t sequence = 0; bool refused = false; uint64_t xuid = 0; std::string text; };
inline std::optional<Bytes> EncodeResult(Result r) {
  if (r.text.size() > kMaxResultText) r.text.resize(kMaxResultText);
  if (!ValidCommandText(r.text, kMaxResultText)) return std::nullopt;
  Bytes b = detail::Begin(kResult, 0, r.refused ? 1 : 0, r.sequence);
  detail::Put64(b, r.xuid);
  b.push_back(uint8_t(r.text.size()));
  b.insert(b.end(), r.text.begin(), r.text.end());
  return b;
}
inline std::optional<Result> DecodeResult(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kResult || h->count != 0 || h->aux > 1 || n < kHeaderSize + 9) return std::nullopt;
  const uint8_t len = d[20];
  if (len > kMaxResultText || n != kHeaderSize + 9u + len) return std::nullopt;
  Result r{h->sequence, h->aux == 1, detail::Get64(d + 12), std::string((const char*)d + 21, len)};
  if (!ValidCommandText(r.text, kMaxResultText)) return std::nullopt;
  return r;
}

// ---- 5 catalog -------------------------------------------------------------------------------
struct CatalogPart { uint32_t revision = 0; uint8_t index = 0, total = 0; std::string data; };
inline std::optional<Bytes> EncodeCatalogPart(const CatalogPart& p) {
  if (p.total < 1 || p.total > kMaxCatalogParts || p.index >= p.total || p.data.size() > kMaxCatalogPart)
    return std::nullopt;
  Bytes b = detail::Begin(kCatalog, p.index, p.total, p.revision);
  b.insert(b.end(), p.data.begin(), p.data.end());
  return b;
}
inline std::optional<CatalogPart> DecodeCatalogPart(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kCatalog || h->aux < 1 || h->aux > kMaxCatalogParts || h->count >= h->aux) return std::nullopt;
  if (n - kHeaderSize > kMaxCatalogPart) return std::nullopt;
  return CatalogPart{h->sequence, h->count, h->aux, std::string((const char*)d + kHeaderSize, n - kHeaderSize)};
}
// Splits catalog text into parts; empty or over 16 parts gives an empty list (not sent).
inline std::vector<CatalogPart> SplitCatalog(uint32_t revision, const std::string& text) {
  std::vector<CatalogPart> parts;
  if (text.empty()) return parts;
  const size_t total = (text.size() + kMaxCatalogPart - 1) / kMaxCatalogPart;
  if (total > kMaxCatalogParts) return parts;
  for (size_t i = 0; i < total; ++i)
    parts.push_back({revision, uint8_t(i), uint8_t(total), text.substr(i * kMaxCatalogPart, kMaxCatalogPart)});
  return parts;
}
// Reassembly: parts in any order, duplicates replace, a changed revision or total restarts.
class CatalogAssembler {
 public:
  // Returns the full text once every part is present.
  std::optional<std::string> Add(const CatalogPart& p) {
    if (p.revision != rev_ || p.total != parts_.size()) { rev_ = p.revision; parts_.assign(p.total, {}); have_.assign(p.total, false); }
    parts_[p.index] = p.data; have_[p.index] = true;
    for (bool h : have_) if (!h) return std::nullopt;
    std::string all;
    for (const auto& s : parts_) all += s;
    return all;
  }
 private:
  uint32_t rev_ = 0;
  std::vector<std::string> parts_;
  std::vector<bool> have_;
};

// ---- 6 world ---------------------------------------------------------------------------------
struct World { uint32_t revision = 0; std::string text; };
inline std::optional<Bytes> EncodeWorld(const World& w) {
  if (!ValidWorldText(w.text)) return std::nullopt;
  Bytes b = detail::Begin(kWorld, 0, 0, w.revision);
  detail::Put16(b, uint16_t(w.text.size()));
  b.insert(b.end(), w.text.begin(), w.text.end());
  return b;
}
inline std::optional<World> DecodeWorld(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kWorld || h->count != 0 || h->aux != 0 || n < kHeaderSize + 2) return std::nullopt;
  const size_t len = detail::Get16(d + 12);
  if (len > kMaxWorldText || n != kHeaderSize + 2 + len) return std::nullopt;
  World w{h->sequence, std::string((const char*)d + 14, len)};
  if (!ValidWorldText(w.text)) return std::nullopt;
  return w;
}

// ---- 7 blasts --------------------------------------------------------------------------------
struct Vec3 { float x, y, z; };
struct Blasts { uint64_t xuid = 0; std::vector<Vec3> hits; };
inline std::optional<Bytes> EncodeBlasts(const Blasts& bl) {
  if (!bl.xuid || bl.hits.empty() || bl.hits.size() > 8) return std::nullopt;
  Bytes b = detail::Begin(kBlasts, uint8_t(bl.hits.size()), 0, 0);
  detail::Put64(b, bl.xuid);
  for (const auto& v : bl.hits) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return std::nullopt;
    for (float f : {v.x, v.y, v.z}) { uint32_t u; std::memcpy(&u, &f, 4); detail::Put32(b, u); }
  }
  return b;
}
inline std::optional<Blasts> DecodeBlasts(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kBlasts || h->count < 1 || h->count > 8 || h->aux != 0 || h->sequence != 0) return std::nullopt;
  if (n != kHeaderSize + 8 + 12u * h->count) return std::nullopt;
  Blasts r;
  r.xuid = detail::Get64(d + 12);
  for (unsigned i = 0; i < h->count; ++i) {
    float f[3];
    for (int k = 0; k < 3; ++k) { uint32_t u = detail::Get32(d + 20 + 12 * i + 4 * k); std::memcpy(&f[k], &u, 4); }
    if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return std::nullopt;
    r.hits.push_back({f[0], f[1], f[2]});
  }
  return r;
}

// ---- 8 menu ask ------------------------------------------------------------------------------
inline Bytes EncodeMenuAsk() { return detail::Begin(kMenuAsk, 0, kMenuFormat, 0); }
inline bool DecodeMenuAsk(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  return h && h->type == kMenuAsk && h->count == 0 && h->aux == kMenuFormat && h->sequence == 0 && n == kHeaderSize;
}

// ---- 9 menus ---------------------------------------------------------------------------------
struct MenuEntry { uint8_t slot = 0; uint8_t identity[kIdentitySize] = {}; };
struct Menus { uint32_t revision = 0, session = 0, epoch = 0; uint16_t client = 0; std::vector<MenuEntry> entries; };
inline bool MenuEntriesValid(const std::vector<MenuEntry>& e) {
  if (e.size() > kMaxMenuEntries) return false;
  int prev = -1;
  for (const auto& x : e) {
    if (x.slot > 61 || int(x.slot) <= prev) return false;
    prev = x.slot;
  }
  return true;
}
inline std::optional<Bytes> EncodeMenus(const Menus& m) {
  if (m.client > 1021 || !MenuEntriesValid(m.entries)) return std::nullopt;
  Bytes b = detail::Begin(kMenus, uint8_t(m.entries.size()), kMenuFormat, m.revision);
  detail::Put32(b, m.session);
  detail::Put32(b, m.epoch);
  detail::Put16(b, m.client);
  for (const auto& e : m.entries) { b.push_back(e.slot); b.insert(b.end(), e.identity, e.identity + kIdentitySize); }
  return b;
}
inline std::optional<Menus> DecodeMenus(const uint8_t* d, size_t n) {
  auto h = ParseHeader(d, n);
  if (!h || h->type != kMenus || h->aux != kMenuFormat || h->count > kMaxMenuEntries) return std::nullopt;
  if (n != kHeaderSize + 10 + 19u * h->count) return std::nullopt;
  Menus m;
  m.revision = h->sequence;
  m.session = detail::Get32(d + 12);
  m.epoch = detail::Get32(d + 16);
  m.client = detail::Get16(d + 20);
  if (m.client > 1021) return std::nullopt;
  for (unsigned i = 0; i < h->count; ++i) {
    MenuEntry e;
    e.slot = d[22 + 19 * i];
    std::memcpy(e.identity, d + 23 + 19 * i, kIdentitySize);
    m.entries.push_back(e);
  }
  if (!MenuEntriesValid(m.entries)) return std::nullopt;
  return m;
}

// HUD element identity from the guest's 124-byte record (spec section 10.2 slices, guest byte order).
inline void MakeIdentity(const uint8_t record[124], uint8_t out[kIdentitySize]) {
  std::memcpy(out + 0, record + 0x40, 4);
  std::memcpy(out + 4, record + 0x58, 4);
  std::memcpy(out + 8, record + 0x70, 6);
  std::memcpy(out + 14, record + 0x56, 2);
  std::memcpy(out + 16, record + 0x64, 2);
}
inline bool IdentityFittable(const uint8_t id[kIdentitySize]) { return id[8] == 1 || id[8] == 8; }
inline bool IdentityMatches(const uint8_t a[kIdentitySize], const uint8_t b[kIdentitySize]) {
  if (a[8] != b[8] || !IdentityFittable(a)) return false;
  return a[8] == 1 ? std::memcmp(a + 14, b + 14, 4) == 0 : std::memcmp(a, b, 14) == 0;
}

}  // namespace bo2cs
