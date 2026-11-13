// Optional fitting of replicated client HUDs with synchronous menu provenance: a mod's elements in a split-screen
// view are drawn as the full screen would draw them, shrunk into the view. Server originals and the game's own HUDs
// keep their existing geometry.
#include "bo2zm_mod_hud_fit.h"
#include "bo2zm_gsc_mods.h"
#include "bo2zm_mod_hud_wire.h"

#include <array>
#include <atomic>
#include <bitset>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(bo2zm_test_mod_hud_trace, false, "BO2",
                    "Test helper: observe script HUD creation and replication provenance without changing HUDs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_mod_hud_fit, true, "BO2",
                    "Fit supported local script HUDs to split-screen views (on a console that joined a System Link "
                    "game: the elements the host names as a mod menu's)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_split_even_margins, true, "BO2",
                    "Split screen: every view keeps the same margins on all four sides, in proportion to its size, so "
                    "each player's HUD sits in the same place in their view. false = the game's own layout, with "
                    "margins only on the screen's outer edges (for televisions that crop the picture)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(bo2zm_split_margin_percent, 3.0, "BO2",
                      "Split screen with even margins: each view's margin on every side, in percent of the view's "
                      "width and height (the full screen keeps the game's own 7.5)")
    .range(0.0, 15.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_mod_local_host, true, "BO2",
                    "In a Local game, every split-screen player on this console counts as the host for the loaded "
                    "mod's own scripts (isHost), so each one gets the host's menu; in a System Link game this "
                    "console hosts, every player does (bots included). The game's own scripts are unchanged")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8232A280);
REX_EXTERN(__imp__sub_8237C968);
REX_EXTERN(__imp__sub_8232A420);
REX_EXTERN(__imp__sub_8223BD90);
REX_EXTERN(__imp__sub_821DC058);

namespace bo2zm::mod_hud_fit {
namespace {
constexpr uint32_t kPool = 0x833A5FC0, kStride = 136, kCount = 1116;
constexpr uint32_t kVm = 0x840DF138, kFrames = kVm + 32;
constexpr uint32_t kStack = 0x840DF3D8, kMaxStack = 0x840E33D0;
constexpr unsigned kCreationReports = 32, kCopyReports = 32;
struct Creation {
  uint64_t epoch = 0, serial = 0;
  uint32_t owner = 1023, caller = 0;
  bool menu = false;
};
struct PsIdentity {
  uint32_t root = 0, stride = 0, ps = 0, owner = 0;
  bool operator==(const PsIdentity&) const = default;
};
struct CompletedCopy {
  Creation before{};
  uint32_t source = 0, caller = 0;
  bool pending = false, complete = false, menu = false;
  std::array<uint8_t, 124> bytes{};
};
struct PsEvidence {
  uint64_t epoch = 0;
  PsIdentity identity{};
  std::array<CompletedCopy, 62> slots{};
};
// Four local players; a missing/full bank is unknown, never permissive.
std::array<PsEvidence, 4> completed_copies{};
struct VmObservation {
  uint32_t depth = 0, frame = 0, position = 0, local_id = 0;
  bool readable = false;
  bool menu = false;
  std::array<uint32_t, 31> positions{};
};
std::mutex mutex;
std::array<Creation, kCount> creations{};
uint64_t epoch = 1, serial = 0;
unsigned creation_reports = 0, copy_reports = 0, menu_creation_reports = 0, menu_copy_reports = 0;
unsigned fitted_copy_reports = 0;
struct View {
  int local = -1;
  uint32_t controller = 0, number = 0, client = 0, cg = 0;
  int x = 0, y = 0, width = 0, height = 0;
  bool operator==(const View&) const = default;
};
struct Layout {
  uint32_t allocated = 0, width = 0, height = 0;
  std::array<View, 4> actors{};
  bool operator==(const Layout&) const = default;
};
Layout layout{};
std::array<View, 4> views{};
uint64_t layout_revision = 0;
bool map_ready = false;
// Numeric string IDs are shared across owners and may be recycled. Negative
// evidence is deliberately never reset, including at map or HUD-pool resets.
std::bitset<512> stock_ids;
bool id_observation_failed = false;
std::array<std::bitset<512>, 1023> menu_body_ids{};
std::array<std::bitset<512>, 1023> menu_label_ids{}, qualified_body_ids{};
std::bitset<1023> owner_blocked, owner_ready;
std::array<PsIdentity, 1023> ready_identity{};
std::array<std::array<uint32_t, 512>, 4> draw_signatures{};
std::array<uint64_t, 4> local_session_ids{};
unsigned positive_id_reports = 0, veto_id_reports = 0, archive_id_reports = 0;
unsigned menu_ps_reports = 0;
unsigned allowed_draw_reports = 0, denied_draw_reports = 0;
unsigned first_unknown_reports = 0;
std::array<unsigned, 4> client_fit_reports{};
unsigned owner_block_reports = 0;

uint32_t Word(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}
int Index(uint32_t hud) {
  if (hud < kPool || hud - kPool >= kStride * kCount || (hud - kPool) % kStride) return -1;
  return int((hud - kPool) / kStride);
}
bool Readable(uint32_t address) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  auto* heap = runtime->memory()->LookupHeap(address);
  uint32_t protection = 0;
  return heap && heap->QueryProtect(address, &protection) && (protection & rex::memory::kMemoryProtectRead);
}
bool GuestSpan(uint32_t address, uint32_t bytes, bool writable = false) {
  if (address < 0x40000000 || !bytes || uint64_t(address) + bytes > 0x100000000ull) return false;
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  const uint32_t required = rex::memory::kMemoryProtectRead |
                            (writable ? rex::memory::kMemoryProtectWrite : 0);
  for (uint64_t at = address; at < uint64_t(address) + bytes;) {
    auto* heap = runtime->memory()->LookupHeap(uint32_t(at));
    uint32_t protection = 0;
    if (!heap || !heap->QueryProtect(uint32_t(at), &protection) || (protection & required) != required) return false;
    at = (at & ~uint64_t(4095)) + 4096;
  }
  return true;
}

// Engine view/setup and server-copy threads only, under the host sidecar lock.
// No renderer callback reads guest memory or pairs compacted overlay indices.
// server = false: the client's side only (a console that joined another's game has no server records); the views'
// client field is then 0.
bool ReadLayout(const uint8_t* base, Layout& out, bool server = true) {
  out = {};
  if (!GuestSpan(0x82C70F44, 4) || !GuestSpan(0x82BBAE68, 4) || !GuestSpan(0x82C153E8, 8)) return false;
  out.allocated = Word(base, 0x82C70F44);
  out.width = Word(base, 0x82C153E8); out.height = Word(base, 0x82C153EC);
  const uint32_t array = Word(base, 0x82BBAE68);
  if (!out.allocated || out.allocated > 4 || !out.width || out.width > 16384 || !out.height ||
      out.height > 16384 || array < 0x40000000 || uint64_t(array) + out.allocated * 0x87700ull > 0x100000000ull) return false;
  for (uint32_t local = 0; local < 4; ++local) {
    const uint32_t manager = 0x837F6A28 + local * 20;
    if (!GuestSpan(manager, 12)) return false;
    if (!(Word(base, manager) & 1)) continue;
    const uint32_t controller = Word(base, manager + 8);
    if (local >= out.allocated || Word(base, manager + 4) != local || controller >= 4 ||
        !GuestSpan(0x83E402C0 + controller * 172, 1) || !GuestSpan(0x82C6FDC8 + local * 1112, 4) ||
        !base[0x83E402C0 + controller * 172] || !(Word(base, 0x82C6FDC8 + local * 1112) & 1)) return false;
    const uint32_t cg = array + local * 0x87700;
    if (!GuestSpan(cg, 4) || !GuestSpan(cg + 0x4D880, 16)) return false;
    const uint32_t number = Word(base, cg);
    if (number >= 1022) return false;
    uint32_t client = 0;
    if (server) {
      const uint32_t entity = 0x833D0640 + number * 796;
      if (!GuestSpan(entity, 344)) return false;
      client = Word(base, entity + 340);
      if (!base[entity + 251] || client < 0x40000000 || uint64_t(client) + 22520 > 0x100000000ull ||
          !GuestSpan(client + 416, 4)) return false;
      // A player who died watches another: their playerstate then carries the followed player's number.
      const uint32_t shown = Word(base, client + 416);
      if (shown != number) {
        const uint32_t followed = 0x833D0640 + shown * 796;
        if (shown >= 1022 || !GuestSpan(followed, 344) || !base[followed + 251]) return false;
        const uint32_t followed_client = Word(base, followed + 340);
        if (followed_client < 0x40000000 || !GuestSpan(followed_client + 416, 4) ||
            Word(base, followed_client + 416) != shown) return false;
      }
    }
    View view{int(local), controller, number, client, cg,
              int32_t(Word(base, cg + 0x4D880)), int32_t(Word(base, cg + 0x4D884)),
              int32_t(Word(base, cg + 0x4D888)), int32_t(Word(base, cg + 0x4D88C))};
    if (view.x < 0 || view.y < 0 || view.width <= 0 || view.height <= 0 ||
        uint32_t(view.width) > out.width || uint32_t(view.height) > out.height ||
        uint32_t(view.x) > out.width - uint32_t(view.width) ||
        uint32_t(view.y) > out.height - uint32_t(view.height)) return false;
    for (const auto& other : out.actors)
      if (other.local >= 0 && (other.controller == controller || other.number == number ||
                               (server && other.client == client))) return false;
    out.actors[local] = view;
  }
  return Word(base, 0x82C70F44) == out.allocated && Word(base, 0x82BBAE68) == array;
}
void ObserveView(uint8_t* base, uint32_t local, uint64_t observed_epoch) {
  std::lock_guard lock(mutex);
  if (!map_ready || epoch != observed_epoch || local >= 4) return;
  Layout current;
  if (!ReadLayout(base, current)) {
    views.fill({}); layout = {}; ++layout_revision; return;
  }
  if (current != layout) {
    layout = current; views.fill({}); ++layout_revision;
  }
  views[local] = current.actors[local];
}

float Scalar(const uint8_t* data, uint32_t offset) {
  const auto bits = Word(data, offset);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}
void PutScalar(uint8_t* data, uint32_t offset, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(data + offset, &bits, 4);
}
uint16_t TextId(const uint8_t* data, uint32_t offset) {
  return (uint16_t(data[offset]) << 8) | data[offset + 1];
}
void BlockOwner(uint32_t owner, const char* reason) {
  if (owner >= owner_blocked.size() || owner_blocked.test(owner)) return;
  owner_blocked.set(owner);
  if (REXCVAR_GET(bo2zm_test_mod_hud_trace) && owner_block_reports < 16) {
    ++owner_block_reports;
    REXLOG_INFO("[mod-hud-fit] owner={} disabled until map reset: {}", owner, reason);
  }
}
// Caller holds mutex. No guest calls and no allocation: these observations
// precede the corresponding live/archive destination write in the CRT.
void RecordVeto(const uint8_t* payload, const char* origin, uint32_t source) {
  for (uint32_t offset : {0x64u, 0x56u}) {
    const uint16_t id = TextId(payload, offset);
    if (!id) continue;
    if (id >= stock_ids.size()) {
      if (REXCVAR_GET(bo2zm_test_mod_hud_trace) && !id_observation_failed)
        REXLOG_WARN("[mod-hud-id] invalid origin={} source={:08X} field={:02X} id={}; ID admission disabled", origin, source, offset, id);
      id_observation_failed = true;
      continue;
    }
    for (uint32_t owner = 0; owner < menu_body_ids.size(); ++owner)
      if (menu_body_ids[owner].test(id) || menu_label_ids[owner].test(id)) BlockOwner(owner, "known text ID veto");
    if (stock_ids.test(id)) continue;
    stock_ids.set(id);
    if (REXCVAR_GET(bo2zm_test_mod_hud_trace) && veto_id_reports++ < 64)
      REXLOG_INFO("[mod-hud-id] veto origin={} source={:08X} field={:02X} id={}", origin, source, offset, id);
  }
}
struct LocalScopeValues {
  uint32_t game = 0, session = 0, dvar = 0, dvar_type = 0, connection = 0;
  int running = -1, connection_type = -1, demo = -1;
};
bool LocalListenScope(const uint8_t* base, uint32_t local, uint64_t& identity, LocalScopeValues* values = nullptr) {
  identity = 0;
  LocalScopeValues raw;
  if (values) *values = raw;
  for (uint32_t at : {0x837F6A80u, 0x837F8CB4u, 0x837F8DC0u, 0x82C70F50u, 0x82C70F44u, 0x84456C84u})
    if (!GuestSpan(at, 4)) return false;
  const uint32_t allocated = Word(base, 0x82C70F44), dvar = Word(base, 0x837F8DC0);
  raw.game = Word(base, 0x837F6A80); raw.session = Word(base, 0x837F8CB4);
  raw.demo = int32_t(Word(base, 0x84456C84)); raw.dvar = dvar;
  if (GuestSpan(dvar, 0x19)) { raw.dvar_type = Word(base, dvar + 0x10); raw.running = base[dvar + 0x18]; }
  const uint32_t array = Word(base, 0x82C70F50);
  const uint64_t connection = uint64_t(array) + uint64_t(local) * 0x358A8;
  if (allocated && allocated <= 4 && local < allocated && array >= 0x40000000 &&
      connection + 0x2436C <= 0x100000000ull && GuestSpan(uint32_t(connection) + 0x24368, 4)) {
    raw.connection = uint32_t(connection); raw.connection_type = int32_t(Word(base, raw.connection + 0x24368));
  }
  if (values) *values = raw;
  // A game whose server runs on this console for its local players: a Local game (the Local game mode, bit 2 of the
  // game mode word, in an offline session, bit 0 of the session mode word) or a System Link game this console hosts
  // (a System Link session, bit 1, that is not online, bit 2; its lobby sets the private match game mode instead of
  // the Local one). A console that joined another's game runs no server and its connection is not the loopback one.
  const bool local_game = (raw.game & 4) && (raw.session & 1);
  const bool system_link = (raw.session & 6) == 2;
  if (!(local_game || system_link) || raw.demo != 0 ||
      raw.dvar_type != 1 || raw.running <= 0 || raw.connection_type != 2) return false;
  identity = (connection << 32) | dvar;
  return true;
}
// This console hosts a System Link game (a System Link session that is not online, its server running, no demo):
// the game other consoles join, whose players' menus are sent to them.
bool HostScope(const uint8_t* base) {
  for (uint32_t at : {0x837F8CB4u, 0x837F8DC0u, 0x84456C84u})
    if (!GuestSpan(at, 4)) return false;
  const uint32_t dvar = Word(base, 0x837F8DC0);
  return (Word(base, 0x837F8CB4) & 6) == 2 && Word(base, 0x84456C84) == 0 && GuestSpan(dvar, 0x19) &&
         Word(base, dvar + 0x10) == 1 && base[dvar + 0x18] > 0;
}

// The host's side: each player's menu elements as its latest snapshot carries them, for the consoles that joined.
// Written by the snapshot playerstate copy, read by the side channel's frame (HostTables); under mutex.
struct Published {
  uint32_t owner = UINT32_MAX;
  mod_hud_wire::Table table{};
};
std::array<Published, 32> published{};
uint32_t published_revision = 0;
const uint32_t host_session = [] {
  const auto ticks = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  return uint32_t(ticks ^ (ticks >> 32)) | 1;
}();
void Publish(uint32_t owner, const mod_hud_wire::Table& table) {
  Published* slot = nullptr;
  for (auto& entry : published) if (entry.owner == owner) slot = &entry;
  if (!slot) {
    if (!table.count) return;  // nothing to say about a player that never had a menu
    for (auto& entry : published) if (entry.owner == UINT32_MAX) { slot = &entry; break; }
    if (!slot) return;  // more players with menus than the table holds: the rest stay unfitted on joined consoles
    slot->owner = owner;
    slot->table = {};
  } else if (slot->table == table) {
    return;
  }
  const uint32_t revision = ++published_revision;
  slot->table = table;
  slot->table.session = host_session;
  slot->table.epoch = uint32_t(epoch);
  slot->table.client = uint16_t(owner);
  slot->table.revision = revision;
}

// The joined console's side: the host's tables, by the client number a snapshot playerstate carries. Written by the
// side channel's frame (StoreJoinedTable), read by the HUD draw; under joined_mutex. joined_count is 0 whenever no
// table is held, so the draw of a console that joined nobody's game, or a host without mod menus, takes no lock.
struct Joined {
  mod_hud_wire::Table table{};
  std::chrono::steady_clock::time_point received{};
  bool used = false;
};
std::mutex joined_mutex;
std::array<Joined, 32> joined{};
uint32_t joined_session = 0, joined_epoch = 0;
std::atomic<int> joined_count{0};
constexpr auto kJoinedFresh = std::chrono::milliseconds(3000);  // the host sends every table at least every second
unsigned joined_fit_reports = 0;
bool RegisteredPs(const uint8_t* base, uint32_t ps, PsIdentity& out, bool require_client = true) {
  if (!GuestSpan(0x83B44E24, 8)) return false;
  const uint32_t root = Word(base, 0x83B44E24), stride = Word(base, 0x83B44E28);
  if (root < 0x40000000 || stride != 0x57F8 || ps < root || (ps - root) % stride) return false;
  const uint32_t owner = (ps - root) / stride;
  if (owner >= 1022 || !GuestSpan(ps, 10760)) return false;
  const uint32_t entity = 0x833D0640 + owner * 796;
  if (!GuestSpan(entity, 344) || !base[entity + 251] || Word(base, entity + 340) != ps ||
      (require_client && Word(base, ps + 416) != owner) || Word(base, 0x83B44E24) != root || Word(base, 0x83B44E28) != stride) return false;
  out = {root, stride, ps, owner};
  return true;
}
// A dead player's playerstate while they watch another: a registered player's slot whose client number is the
// followed player's (registered) one.
bool SpectatorCopy(const uint8_t* base, uint32_t ps, PsIdentity* followed_out = nullptr) {
  PsIdentity slot, followed;
  if (!RegisteredPs(base, ps, slot, false) || !GuestSpan(ps + 416, 4)) return false;
  const uint32_t number = Word(base, ps + 416);
  if (number == slot.owner || number >= 1022) return false;
  const uint32_t followed_ps = slot.root + number * slot.stride;
  if (!RegisteredPs(base, followed_ps, followed)) return false;
  if (followed_out) *followed_out = followed;
  return true;
}
bool RegisteredSlot(const uint8_t* base, uint32_t destination, PsIdentity& out, size_t& slot) {
  if (!GuestSpan(0x83B44E24, 8)) return false;
  const uint32_t root = Word(base, 0x83B44E24), stride = Word(base, 0x83B44E28);
  if (root < 0x40000000 || stride != 0x57F8 || destination < root) return false;
  const uint32_t relative = destination - root, offset = relative % stride;
  if (!RegisteredPs(base, destination - offset, out)) return false;
  for (size_t group = 0; group < 2; ++group) {
    const uint32_t start = group ? 6912 : 3068;
    if (offset >= start && offset - start < 31 * 124 && (offset - start) % 124 == 0) {
      slot = group * 31 + (offset - start) / 124;
      return true;
    }
  }
  return false;
}
PsEvidence* EvidenceFor(const PsIdentity& identity, bool create) {
  for (auto& bank : completed_copies)
    if (bank.epoch == epoch && bank.identity == identity) return &bank;
  if (create) for (auto& bank : completed_copies) {
    if (bank.epoch == epoch && bank.identity.root == identity.root && bank.identity.stride == identity.stride) continue;
    bank = {}; bank.epoch = epoch; bank.identity = identity;
    return &bank;
  }
  return nullptr;
}
bool LocalPsScope(const uint8_t* base, const PsIdentity& identity) {
  Layout actors;
  if (!ReadLayout(base, actors)) return false;
  for (const auto& actor : actors.actors) if (actor.local >= 0 && actor.client == identity.ps && actor.number == identity.owner) {
    uint64_t session = 0;
    return LocalListenScope(base, uint32_t(actor.local), session);
  }
  return false;
}
bool OwnerCanFit(const uint8_t* base, uint32_t owner, uint32_t ps) {
  if (owner >= owner_ready.size() || !owner_ready.test(owner) || owner_blocked.test(owner) ||
      id_observation_failed || ready_identity[owner].ps != ps || !GuestSpan(0x83B44E24, 8) ||
      Word(base, 0x83B44E24) != ready_identity[owner].root || Word(base, 0x83B44E28) != ready_identity[owner].stride) return false;
  Layout current;
  if (!ReadLayout(base, current)) return false;
  for (const auto& actor : current.actors) if (actor.local >= 0 && actor.number == owner && actor.client == ps) {
    uint64_t session = 0;
    return LocalListenScope(base, uint32_t(actor.local), session);
  }
  return false;
}
void BeginLiveEvidence(const PPCContext& ctx, uint8_t* base, const Creation& creation) {
  PsIdentity identity; size_t slot = 0;
  if (!RegisteredSlot(base, ctx.r3.u32, identity, slot)) return;
  if (auto* bank = EvidenceFor(identity, true)) {
    auto& copy = bank->slots[slot];
    copy = {};  // no old completed occupant survives a new write in flight
    copy.before = creation; copy.source = ctx.r4.u32; copy.caller = uint32_t(ctx.lr); copy.pending = true;
  }
}
void CompleteLiveEvidence(const PPCContext& ctx, uint8_t* base, const Creation& creation) {
  PsIdentity identity; size_t slot = 0;
  if (!RegisteredSlot(base, ctx.r3.u32, identity, slot)) return;
  auto* bank = EvidenceFor(identity, false);
  if (!bank) return;
  auto& copy = bank->slots[slot];
  if (!copy.pending || copy.source != ctx.r4.u32 || copy.caller != uint32_t(ctx.lr)) return;
  copy.pending = false; copy.complete = true;
  copy.menu = copy.before.epoch == epoch && copy.before.serial && copy.before.menu &&
      copy.before.serial == creation.serial && creation.epoch == epoch && creation.menu &&
      creation.owner == copy.before.owner && creation.owner == identity.owner &&
      Word(base, ctx.r4.u32 + 124) == identity.owner && Word(base, ctx.r4.u32 + 128) == 0;
  std::memcpy(copy.bytes.data(), base + ctx.r3.u32, copy.bytes.size());
}
// Caller holds mutex. Ineligibility is not an identity transition: startup
// views/connections may be cold while completed server copies are already valid.
void ObserveSessionIdentity(uint32_t local, bool eligible, uint64_t session) {
  if (!eligible) return;
  if (local_session_ids[local] && local_session_ids[local] != session) {
    menu_body_ids.fill({}); menu_label_ids.fill({}); qualified_body_ids.fill({});
    owner_ready.reset(); completed_copies.fill({});
  }
  local_session_ids[local] = session;
}
void ObserveClientDraw(const PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bo2zm_test_mod_hud_trace) || uint32_t(ctx.lr) != 0x821DC5AC || ctx.r3.u32 >= 4) return;
  const uint32_t local = ctx.r3.u32, hud = ctx.r4.u32;
  std::lock_guard lock(mutex);
  uint64_t session = 0;
  LocalScopeValues scope;
  const bool local_scope = LocalListenScope(base, local, session, &scope);
  ObserveSessionIdentity(local, local_scope, session);
  Layout current;
  if (!map_ready || !ReadLayout(base, current) || current != layout) return;
  const auto& view = current.actors[local];
  if (view.local < 0 || views[local] != view || !GuestSpan(view.cg + 40, 4)) return;
  const uint32_t snapshot = Word(base, view.cg + 40);
  if (!GuestSpan(snapshot, 16 + 10760)) return;
  const uint32_t ps = snapshot + 16;
  if (Word(base, ps + 416) != view.number) return;
  bool member = false;
  for (uint32_t group : {3068u, 6912u}) {
    for (uint32_t slot = 0; slot < 31; ++slot) {
      const uint32_t at = ps + group + slot * 124;
      if (base[at + 0x70] == 0) break;
      if (at == hud) member = true;
    }
  }
  if (!member || base[hud + 0x70] != 1 || base[hud + 0x79] != 255) return;
  const uint16_t body = TextId(base + hud, 0x64), label = TextId(base + hud, 0x56);
  const bool legal = body > 0 && body < 512 && label < 512;
  const bool positive = legal && menu_body_ids[view.number].test(body);
  const bool body_veto = legal && stock_ids.test(body);
  const bool label_veto = legal && label && stock_ids.test(label);
  unsigned active = 0;
  for (const auto& actor : current.actors) if (actor.local >= 0) ++active;
  const float factor = std::min(float(view.width) / current.width, float(view.height) / current.height);
  const bool admitted = legal && positive && !body_veto && !label_veto && !id_observation_failed &&
                        local_scope && !owner_blocked.test(view.number) && active > 1 && factor > 0 && factor < 1;
  auto& reports = admitted ? allowed_draw_reports : denied_draw_reports;
  const auto report_id = legal ? body : 0;
  const uint32_t signature = 1 + ((label < 512 ? label : 0) << 7) +
      unsigned(legal) + 2 * unsigned(positive) + 4 * unsigned(body_veto) +
      8 * unsigned(label_veto) + 16 * unsigned(id_observation_failed) + 32 * unsigned(admitted) + 64 * unsigned(local_scope);
  auto& previous = draw_signatures[local][report_id];
  if (previous == signature || reports >= 64) return;
  previous = signature; ++reports;
  REXLOG_INFO("[mod-hud-id] draw epoch={} local={} owner={} snapshot={:08X} hud={:08X} body={} label={} font={} from_font={} x={} y={} menu_body={} body_veto={} label_veto={} observation_failed={} game_mask={:08X} session_mask={:08X} sv_dvar={:08X} sv_type={} sv_current={} connection={:08X} connection_type={} demo={} local_scope={} would_admit={} client_fitting=false producer_coverage_proven=false factor={} changed=false",
              epoch, local, view.number, snapshot, hud, body, label,
              Scalar(base + hud, 0xC), Scalar(base + hud, 0x10), Scalar(base + hud, 0), Scalar(base + hud, 4),
              positive, body_veto, label_veto,
              id_observation_failed, scope.game, scope.session, scope.dvar, scope.dvar_type, scope.running,
              scope.connection, scope.connection_type, scope.demo, local_scope, admitted, factor);
}
// A mod HUD element of a split-screen player is drawn as the full screen would draw it, shrunk into that player's
// view: during its stock draw, the view's screen placement (0x822A8570, the record the draw positions every element
// with) is replaced by one made from the full-screen record. Every anchor (top, middle, bottom, the safe-area ones)
// and every scale then keeps its full-screen relation to the others, so an element group laid out against different
// anchors stays together; the element itself is not changed.
struct ClientPlacement {
  uint32_t record = 0;  // guest address of the record to draw with
  uint32_t sp = 0;      // the draw's stack pointer: lookups deeper than this belong to the draw
  float width = 0, height = 0;  // the view, in the record's pixels
};
constexpr uint32_t kViewPlacements = 0x82CBC168, kFullPlacement = 0x82CBC360, kPlacementSize = 124;
uint32_t synthetic_placements = 0;  // four records in guest memory, one per local player
// Whether the snapshot record is still the server's copy of a menu-created material: a draw-side copy can differ
// in its animation times, so the comparison covers what identifies the element (sort, size, type, font, alignment
// and material).
bool SameMaterial(const uint8_t* copy, const uint8_t* record) {
  return std::memcmp(copy + 0x40, record + 0x40, 4) == 0 && std::memcmp(copy + 0x58, record + 0x58, 4) == 0 &&
         std::memcmp(copy + 0x70, record + 0x70, 6) == 0;
}
// The full-screen placement mapped into a view: pixel positions scaled by the view's factor about its own origin
// (the scaled image centred in the view), pixel scales multiplied by it, the pixel-to-unit scales divided by it, and
// the fields in HUD units (+0x18..+0x34, the HUD scale at +0x78) kept.
bool BuildPlacement(uint8_t* base, const View& view, const Layout& current, uint32_t record) {
  // The records come from the system heap, below the range GuestSpan accepts: the heap's own protection says
  // whether they are there.
  if (!GuestSpan(kFullPlacement, kPlacementSize) || !Readable(record) || !Readable(record + kPlacementSize - 1))
    return false;
  const uint8_t* full = base + kFullPlacement;
  if (Scalar(full, 0x40) != float(current.width) || Scalar(full, 0x44) != float(current.height)) {
    static bool reported = false;
    if (REXCVAR_GET(bo2zm_test_mod_hud_trace) && !reported) {
      reported = true;
      REXLOG_WARN("[mod-hud-fit] the full-screen placement is {}x{}, not the image's {}x{}: no fitting",
                  Scalar(full, 0x40), Scalar(full, 0x44), current.width, current.height);
    }
    return false;
  }
  const float factor = std::min(float(view.width) / current.width, float(view.height) / current.height);
  if (!(factor > 0.0f && factor <= 1.0f)) return false;
  const float ox = (view.width - factor * current.width) * 0.5f, oy = (view.height - factor * current.height) * 0.5f;
  uint8_t* out = base + record;
  std::memcpy(out, full, kPlacementSize);
  for (uint32_t offset : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x40u, 0x44u, 0x70u, 0x74u})
    PutScalar(out, offset, Scalar(full, offset) * factor);
  for (uint32_t offset : {0x10u, 0x14u}) PutScalar(out, offset, Scalar(full, offset) / factor);
  for (uint32_t offset = 0x38; offset <= 0x6C; offset += 4) {
    if (offset == 0x40 || offset == 0x44) continue;
    PutScalar(out, offset, Scalar(full, offset) * factor + ((offset & 4) ? oy : ox));
  }
  return true;
}
// Caller holds mutex. The fitted placement for an element of this view's that qualified: only in split screen (more
// than one view), the view's own record built from the full-screen one.
bool FitInto(const PPCContext& ctx, uint8_t* base, const View& view, const Layout& current, uint32_t local,
             ClientPlacement& out) {
  unsigned active = 0;
  for (const auto& actor : current.actors) if (actor.local >= 0) ++active;
  if (active < 2) return false;
  if (!synthetic_placements) {
    auto* memory = rex::Runtime::instance()->memory();
    synthetic_placements = memory ? memory->SystemHeapAlloc(kPlacementSize * 4) : 0;
    if (!synthetic_placements) return false;
  }
  out.record = synthetic_placements + local * kPlacementSize;
  out.sp = ctx.r1.u32;
  out.width = float(view.width); out.height = float(view.height);
  return BuildPlacement(base, view, current, out.record);
}
// The element's slot (0..61) in the snapshot playerstate's two groups, counting only their active prefixes as the
// snapshot encoder does; -1 when it is not one of them.
int SnapshotSlot(const uint8_t* base, uint32_t ps, uint32_t hud) {
  for (uint32_t group = 0; group < 2; ++group) for (uint32_t slot = 0; slot < 31; ++slot) {
    const auto at = ps + (group ? 6912u : 3068u) + slot * 124;
    if (!base[at + 0x70]) break;
    if (at == hud) return int(group * 31 + slot);
  }
  return -1;
}
bool PreparePlacement(const PPCContext& ctx, uint8_t* base, ClientPlacement& out) {
  if (!REXCVAR_GET(bo2zm_mod_hud_fit) || uint32_t(ctx.lr) != 0x821DC5AC || ctx.r3.u32 >= 4) return false;
  std::lock_guard lock(mutex);
  const uint32_t local = ctx.r3.u32, hud = ctx.r4.u32;
  uint64_t session = 0;
  const bool local_scope = LocalListenScope(base, local, session);
  ObserveSessionIdentity(local, local_scope, session);
  if (!local_scope) return false;
  Layout current;
  if (!map_ready || !ReadLayout(base, current) || current != layout) return false;
  const auto& view = current.actors[local];
  if (view.local < 0 || views[local] != view || !GuestSpan(view.cg + 40, 4)) return false;
  const uint32_t snapshot = Word(base, view.cg + 40);
  if (!GuestSpan(snapshot, 16 + 10760)) return false;
  const uint32_t ps = snapshot + 16;
  // The elements' owner: the view's player, or the player they watch after dying.
  uint32_t owner = view.number;
  PsIdentity identity;
  if (Word(base, ps + 416) == view.number) {
    if (!OwnerCanFit(base, owner, view.client) || !RegisteredPs(base, view.client, identity) ||
        identity != ready_identity[owner]) return false;
  } else {
    PsIdentity followed;
    if (!SpectatorCopy(base, view.client, &followed) || followed.owner != Word(base, ps + 416)) return false;
    owner = followed.owner;
    if (!OwnerCanFit(base, owner, followed.ps) || followed != ready_identity[owner]) return false;
    identity = followed;
  }
  const int member = SnapshotSlot(base, ps, hud);
  if (member < 0 || base[hud + 0x79] != 255) return false;
  if (base[hud + 0x70] == 1) {
    const auto body = TextId(base + hud, 0x64), label = TextId(base + hud, 0x56);
    if (!body || body >= 512 || label >= 512 || !qualified_body_ids[owner].test(body)) return false;
    if (stock_ids.test(body) || (label && stock_ids.test(label))) {
      BlockOwner(owner, "client text ID veto"); return false;
    }
  } else if (base[hud + 0x70] == 8) {
    // A material has no text to match: it qualifies when this snapshot slot still holds the completed copy of a
    // menu-created element that the server made for this owner.
    const auto* bank = EvidenceFor(identity, false);
    if (!bank) return false;
    const auto& copy = bank->slots[size_t(member)];
    if (!copy.complete || copy.pending || !copy.menu || !SameMaterial(copy.bytes.data(), base + hud)) return false;
  } else {
    return false;
  }
  if (!FitInto(ctx, base, view, current, local, out)) return false;
  if (REXCVAR_GET(bo2zm_test_mod_hud_trace) && client_fit_reports[local] < 8) {
    ++client_fit_reports[local];
    REXLOG_INFO("[mod-hud-fit] client epoch={} local={} owner={} hud={:08X} type={} body={} label={} placement={:08X}",
                epoch, local, owner, hud, base[hud + 0x70], TextId(base + hud, 0x64), TextId(base + hud, 0x56),
                out.record);
  }
  return true;
}
// A console that joined another's System Link game: an element its snapshot carries is fitted when the host's latest
// table (fresh, of the host's current session and map) for the playerstate's client says its slot holds exactly that
// menu element. That client is the view's player, or the player they watch after dying. Nothing here reads the
// server's records, which such a console does not have.
bool PrepareJoinedPlacement(const PPCContext& ctx, uint8_t* base, ClientPlacement& out) {
  if (!joined_count.load(std::memory_order_acquire) || !REXCVAR_GET(bo2zm_mod_hud_fit) ||
      uint32_t(ctx.lr) != 0x821DC5AC || ctx.r3.u32 >= 4) return false;
  const uint32_t local = ctx.r3.u32, hud = ctx.r4.u32;
  // A console whose own server runs decides from its own evidence (PreparePlacement), never from a table.
  if (!GuestSpan(0x837F8DC0, 4)) return false;
  const uint32_t server_dvar = Word(base, 0x837F8DC0);
  if (!GuestSpan(server_dvar, 0x19) || Word(base, server_dvar + 0x10) != 1 || base[server_dvar + 0x18]) return false;
  std::lock_guard lock(mutex);
  Layout current;
  if (!ReadLayout(base, current, false)) return false;
  const auto& view = current.actors[local];
  if (view.local < 0 || !GuestSpan(view.cg + 40, 4)) return false;
  const uint32_t snapshot = Word(base, view.cg + 40);
  if (!GuestSpan(snapshot, 16 + 10760)) return false;
  const uint32_t ps = snapshot + 16, client = Word(base, ps + 416);
  const int slot = SnapshotSlot(base, ps, hud);
  if (client >= mod_hud_wire::kMaxClient || slot < 0 || base[hud + 0x79] != 255) return false;
  {
    std::lock_guard tables(joined_mutex);
    const auto now = std::chrono::steady_clock::now();
    const Joined* held = nullptr;
    for (const auto& entry : joined)
      if (entry.used && entry.table.client == client) held = &entry;
    if (!held || now - held->received > kJoinedFresh) return false;
    const auto* named = mod_hud_wire::Find(held->table, uint32_t(slot));
    if (!named || !mod_hud_wire::Matches(base + hud, named->id)) return false;
  }
  if (!FitInto(ctx, base, view, current, local, out)) return false;
  if (REXCVAR_GET(bo2zm_test_mod_hud_trace) && joined_fit_reports < 16) {
    ++joined_fit_reports;
    REXLOG_INFO("[mod-hud-fit] joined local={} client={} slot={} hud={:08X} type={} body={} label={} placement={:08X}",
                local, client, slot, hud, base[hud + 0x70], TextId(base + hud, 0x64), TextId(base + hud, 0x56),
                out.record);
  }
  return true;
}
// The placement the current draw uses instead of its view's (0: none). Set only around a stock HUD draw on this
// thread; a lookup counts as the draw's only from deeper in the guest stack, and the next draw resets it, so a
// guest error unwinding out of the draw leaves nothing behind.
thread_local ClientPlacement active_placement;
// Split screen with even margins: the game gives each view margins only on the screen's outer edges, so every
// player's HUD sits somewhere else in their view. The placement lookups hand out a copy of the view's record with
// the same margin on all four sides, bo2zm_split_margin_percent of the view's size (bo2zm_split_even_margins). The
// game's own records are not changed; a copy is rebuilt when its record changes.
uint32_t even_placements = 0;          // four records in guest memory
std::array<std::array<uint8_t, kPlacementSize>, 4> even_sources{};
std::array<bool, 4> even_built{};
std::mutex even_mutex;
uint32_t EvenPlacement(uint8_t* base, uint32_t record) {
  if (!REXCVAR_GET(bo2zm_split_even_margins) || record < kViewPlacements || (record - kViewPlacements) % kPlacementSize ||
      (record - kViewPlacements) / kPlacementSize >= 4 || !GuestSpan(kFullPlacement, kPlacementSize))
    return record;
  const uint32_t index = (record - kViewPlacements) / kPlacementSize;
  std::lock_guard lock(even_mutex);
  if (!even_placements) {
    auto* memory = rex::Runtime::instance()->memory();
    even_placements = memory ? memory->SystemHeapAlloc(kPlacementSize * 4) : 0;
    if (!even_placements) return record;
  }
  const uint32_t copy = even_placements + index * kPlacementSize;
  const uint8_t* view = base + record;
  if (even_built[index] && std::memcmp(even_sources[index].data(), view, kPlacementSize) == 0) return copy;
  const uint8_t* full = base + kFullPlacement;
  const float width = Scalar(view, 0x40), height = Scalar(view, 0x44), sx = Scalar(view, 0x00), sy = Scalar(view, 0x04);
  const float full_width = Scalar(full, 0x40), full_height = Scalar(full, 0x44);
  if (!(width > 0 && height > 0 && sx > 0 && sy > 0 && full_width > 0 && full_height > 0)) return record;
  // Only a view smaller than the screen is a split-screen view; the menus build the HUD before its first frame.
  if (width >= full_width && height >= full_height) return record;
  const float share = float(REXCVAR_GET(bo2zm_split_margin_percent) / 100.0);
  const float mx = share * width, my = share * height;
  const float x0 = Scalar(view, 0x38), y0 = Scalar(view, 0x3C);
  uint8_t* out = base + copy;
  std::memcpy(out, view, kPlacementSize);
  for (uint32_t offset : {0x50u, 0x60u}) { PutScalar(out, offset, x0 + mx); PutScalar(out, offset + 4, y0 + my); }
  for (uint32_t offset : {0x58u, 0x68u}) {
    PutScalar(out, offset, x0 + width - mx); PutScalar(out, offset + 4, y0 + height - my);
  }
  // The same bounds in HUD units (the view's own scale).
  for (uint32_t offset : {0x18u, 0x28u}) { PutScalar(out, offset, mx / sx); PutScalar(out, offset + 4, my / sy); }
  for (uint32_t offset : {0x20u, 0x30u}) {
    PutScalar(out, offset, (width - mx) / sx); PutScalar(out, offset + 4, (height - my) / sy);
  }
  std::memcpy(even_sources[index].data(), view, kPlacementSize);
  even_built[index] = true;
  return copy;
}

// The shared HUD scale (0x82CBC45C), which the text and size calculations multiply in and which split screen raises
// so that each view's HUD stays as big as the full screen's. A fitted draw runs at the full screen's 1.0; the value
// it replaced is put back after the draw, or at the next draw if a guest error unwound out of it.
constexpr uint32_t kHudScale = 0x82CBC45C;
thread_local bool hud_scale_saved = false;
thread_local uint32_t saved_hud_scale = 0;
void RestoreHudScale(uint8_t* base) {
  if (!hud_scale_saved) return;
  hud_scale_saved = false;
  const uint32_t bits = __builtin_bswap32(saved_hud_scale);
  std::memcpy(base + kHudScale, &bits, 4);
}
void UseFullScreenHudScale(uint8_t* base) {
  saved_hud_scale = Word(base, kHudScale);
  hud_scale_saved = true;
  const float one = 1.0f;
  uint32_t bits;
  std::memcpy(&bits, &one, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(base + kHudScale, &bits, 4);
}
bool StackPointer(uint32_t pointer) {
  return pointer >= kStack && pointer <= kMaxStack && (pointer - kStack) % 8 == 0;
}
// The script call chain of the builtin being dispatched now (any builtin or method).
VmObservation ReadScriptStack(const uint8_t* base) {
  VmObservation result{Word(base, kVm + 8), Word(base, kVm + 12)};
  const uint32_t top = Word(base, kVm + 16);
  if (!result.depth || result.depth > 31 || result.frame != kFrames + 20 * result.depth ||
      Word(base, kVm + 4) != kMaxStack || !StackPointer(top)) return result;
  // The current continuation is published by the interpreter immediately
  // before builtin dispatch. Slot zero is a bridge, not a live caller.
  for (uint32_t slot = result.depth; slot; --slot) {
    const auto frame = kFrames + 20 * slot;
    const uint32_t pc = Word(base, frame);
    if (!pc || !Readable(pc)) return result;
    result.positions[result.depth - slot] = pc;
  }
  if (Word(base, kVm + 8) != result.depth || Word(base, kVm + 12) != result.frame ||
      Word(base, kVm + 16) != top) return result;
  result.position = result.positions[0];
  result.local_id = Word(base, result.frame + 8);
  result.readable = true;
  result.menu = gsc::MenuAncestry(result.positions.data(), result.depth);
  return result;
}
VmObservation ObserveVm(const uint8_t* base, uint32_t caller) {
  if (caller != 0x8232C400) return {};  // newClientHudElem's allocator call only
  return ReadScriptStack(base);
}
void Clear(uint32_t hud) {
  const int index = Index(hud);
  if (index < 0) return;
  std::lock_guard lock(mutex);
  creations[size_t(index)] = {};
}
void Created(uint32_t hud, uint32_t owner, uint32_t caller,
             uint64_t creation_epoch, VmObservation vm) {
  const int index = Index(hud);
  if (index < 0) return;
  std::array<gsc::MenuCodeRange, 32> ranges{};
  const bool trace = REXCVAR_GET(bo2zm_test_mod_hud_trace);
  const size_t count = trace ? gsc::MenuCodeRanges(ranges.data(), ranges.size()) : 0;
  Creation creation;
  bool report = false;
  {
    std::lock_guard lock(mutex);
    if (creation_epoch != epoch) return;
    creations[size_t(index)] = {};
    creation = {epoch, ++serial, owner, caller, vm.readable && vm.menu && owner < 1023};
    creations[size_t(index)] = creation;
    auto& reports = creation.menu ? menu_creation_reports : creation_reports;
    report = trace && reports < kCreationReports;
    if (report) ++reports;
  }
  if (!report) return;
  REXLOG_INFO("[mod-hud] create epoch={} serial={} slot={} owner={} caller={:08X} vm_depth={} vm_frame={:08X} readable={} position={:08X} local_id={} menu_ranges={} synchronous_menu={}",
              creation.epoch, creation.serial, index, owner, caller, vm.depth, vm.frame, vm.readable,
              vm.position, vm.local_id, count, creation.menu);
  for (uint32_t i = 0; vm.readable && i < vm.depth; ++i)
    REXLOG_INFO("[mod-hud] frame slot={} position={:08X}", vm.depth - i, vm.positions[i]);
  for (size_t i = 0; i < count; ++i)
    REXLOG_INFO("[mod-hud] menu code {:08X}..{:08X}", ranges[i].begin, ranges[i].end);
}
}  // namespace

void BeginEpoch() {
  std::lock_guard lock(mutex);
  ++epoch;
  creations.fill({});
  map_ready = false;
  layout = {}; views.fill({}); ++layout_revision;
  creation_reports = copy_reports = menu_creation_reports = menu_copy_reports = 0;
  fitted_copy_reports = 0;
  menu_body_ids.fill({}); draw_signatures.fill({});
  local_session_ids.fill(0);
  completed_copies.fill({});
  menu_label_ids.fill({}); qualified_body_ids.fill({}); owner_ready.reset(); owner_blocked.reset(); ready_identity.fill({});
  client_fit_reports.fill(0); owner_block_reports = 0;
  published.fill({});
  first_unknown_reports = 0;
  positive_id_reports = archive_id_reports = menu_ps_reports = allowed_draw_reports = denied_draw_reports = 0;
}

void BeforeCopy(const PPCContext& ctx, uint8_t* base) {
  const bool trace = REXCVAR_GET(bo2zm_test_mod_hud_trace);
  if (!trace && !REXCVAR_GET(bo2zm_mod_hud_fit)) return;
  if (ctx.r5.u32 == 10760 && IsBulkPsCopySite(uint32_t(ctx.lr))) {
    std::lock_guard lock(mutex);
    PsIdentity identity;
    if (!RegisteredPs(base, ctx.r3.u32, identity, false)) return;
    const auto owner = identity.owner;
    // A player who died watches another: the game copies the followed player's playerstate (its client number is
    // the followed player's) over theirs every frame. Their evidence goes; the mod makes their menu again when they
    // come back.
    const bool spectating = GuestSpan(ctx.r4.u32 + 416, 4) && Word(base, ctx.r4.u32 + 416) != owner;
    if (!spectating && (owner_ready.test(owner) || menu_body_ids[owner].any() || menu_label_ids[owner].any()))
      BlockOwner(owner, "bulk playerstate overwrite");
    for (auto& bank : completed_copies) if (bank.epoch == epoch && bank.identity.ps == identity.ps) bank = {};
    menu_body_ids[owner].reset(); menu_label_ids[owner].reset(); qualified_body_ids[owner].reset();
    Publish(owner, {});  // the consoles that joined stop fitting this player's former elements too
    owner_ready.reset(owner); ready_identity[owner] = {};
    return;
  }
  const bool unknown_ps = ctx.r5.u32 == 10760 && IsUnknownPsCopySite(uint32_t(ctx.lr));
  if (!unknown_ps && (ctx.r5.u32 != 124 || !IsCopySite(uint32_t(ctx.lr)))) return;
  const uint32_t source = ctx.r4.u32;
  std::lock_guard lock(mutex);
  if (!GuestSpan(source, unknown_ps ? 10760 : 136)) {
    if (trace && !id_observation_failed) REXLOG_WARN("[mod-hud-id] unreadable source={:08X}; ID admission disabled", source);
    id_observation_failed = true;
    return;
  }
  if (unknown_ps) {
    bool menu_history = false;
    PsIdentity identity;
    const bool registered = RegisteredPs(base, source, identity);
    if (!registered && SpectatorCopy(base, source)) return;
    const bool local_scope = registered && LocalPsScope(base, identity);
    // Classify the completed producer independently of client draw readiness.
    // Fresh Local scope remains mandatory for qualification and actual fitting.
    const auto* bank = registered && !id_observation_failed ? EvidenceFor(identity, false) : nullptr;
    // Hosting a System Link game: this copy builds the snapshot the player's console receives, so the menu elements
    // seen here, slot by slot, are the ones that console may fit.
    const bool host_scope = registered && HostScope(base);
    mod_hud_wire::Table table;
    unsigned active = 0, known_menu = 0, unknown = 0;
    for (size_t group = 0; group < 2; ++group) for (size_t slot = 0; slot < 31; ++slot) {
      const uint32_t record = source + (group ? 6912 : 3068) + uint32_t(slot) * 124;
      if (!base[record + 0x70]) break;  // exact encoder active-prefix rule
      ++active;
      const auto body = TextId(base + record, 0x64);
      if (body && body < 512) for (const auto& actor : layout.actors)
        if (actor.local >= 0 && menu_body_ids[actor.number].test(body)) menu_history = true;
      const auto* copy = bank ? &bank->slots[group * 31 + slot] : nullptr;
      const bool equal = copy && copy->complete && !copy->pending &&
          std::memcmp(copy->bytes.data(), base + record, copy->bytes.size()) == 0;
      if (equal && copy->menu) {
        ++known_menu;
        const auto label = TextId(base + record, 0x56);
        if (local_scope && base[record + 0x70] == 1 && body && body < 512 && label < 512) {
          if (stock_ids.test(body) || (label && stock_ids.test(label))) BlockOwner(identity.owner, "source text ID veto");
          else {
            qualified_body_ids[identity.owner].set(body);
            owner_ready.set(identity.owner); ready_identity[identity.owner] = identity;
          }
        }
        const uint8_t type = base[record + 0x70];
        if (host_scope && ((type == mod_hud_wire::kText && body && body < 512 && label < 512 && !stock_ids.test(body) &&
                            !(label && stock_ids.test(label))) || type == mod_hud_wire::kMaterial))
          table.entries[table.count++] = {uint8_t(group * 31 + slot), mod_hud_wire::IdOf(base + record)};
      } else {
        const auto label = TextId(base + record, 0x56);
        const bool new_id = (body && body < 512 && !stock_ids.test(body)) ||
                            (label && label < 512 && !stock_ids.test(label));
        if (trace && new_id && first_unknown_reports < 16) {
          ++first_unknown_reports;
          REXLOG_INFO("[mod-hud-id] first_unknown epoch={} ps={:08X} owner={} group={} slot={} body={} label={} registered={} local_scope={} bank={} complete={} pending={} menu={} equal={} ref_body={} observation_failed={}",
                      epoch, source, registered ? int(identity.owner) : -1, group, slot, body, label,
                      registered, local_scope, bank != nullptr, copy && copy->complete, copy && copy->pending,
                      copy && copy->menu, equal, copy && copy->complete ? TextId(copy->bytes.data(), 0x64) : 0, id_observation_failed);
        }
        if (copy && copy->complete && copy->menu && copy->bytes[0x70] == 1 && TextId(copy->bytes.data(), 0x64))
          BlockOwner(identity.owner, "ambiguous completed text record");
        ++unknown; RecordVeto(base + record, "unknown-ps", record);
      }
    }
    if (host_scope) {
      if (owner_blocked.test(identity.owner) || id_observation_failed) table.count = 0;
      Publish(identity.owner, table);
    }
    auto& reports = menu_history ? menu_ps_reports : archive_id_reports;
    if (trace && reports < 8) {
      ++reports;
      Layout actors;
      const bool actors_valid = ReadLayout(base, actors);
      int source_local = -1, source_owner = -1, destination_local = -1, destination_owner = -1;
      if (actors_valid) for (const auto& actor : actors.actors) {
        if (actor.local < 0) continue;
        if (actor.client == source) { source_local = actor.local; source_owner = int(actor.number); }
        if (actor.client == ctx.r3.u32) { destination_local = actor.local; destination_owner = int(actor.number); }
      }
      REXLOG_INFO("[mod-hud-id] PS_SCAN source={:08X} destination={:08X} bytes={} registered={} registered_base={:08X} registered_stride={} registered_owner={} local_scope={} active={} exact_menu={} veto_records={} actors_valid={} source_local={} source_owner={} destination_local={} destination_owner={} menu_id_history={} stock_ids={} client_fitting=false producer_coverage_proven=false changed=false",
                  source, ctx.r3.u32, ctx.r5.u32, registered, identity.root, identity.stride, registered ? int(identity.owner) : -1,
                  local_scope, active, known_menu, unknown, actors_valid, source_local, source_owner,
                  destination_local, destination_owner, menu_history, stock_ids.count());
    }
    return;
  }
  const int index = Index(source);
  const auto creation = index >= 0 ? creations[size_t(index)] : Creation{};
  const uint32_t owner = Word(base, source + 124);
  const bool menu = creation.epoch == epoch && creation.serial && creation.menu &&
                    owner < 1023 && creation.owner == owner && Word(base, source + 128) == 0;
  auto evidence_creation = creation;
  evidence_creation.menu = menu;
  BeginLiveEvidence(ctx, base, evidence_creation);
  if (!menu) { RecordVeto(base + source, "live-unknown", source); return; }
  const uint16_t body = TextId(base + source, 0x64), label = TextId(base + source, 0x56);
  if (body >= 512 || label >= 512) {
    if (trace && !id_observation_failed)
      REXLOG_WARN("[mod-hud-id] invalid menu source={:08X} body={} label={}; ID admission disabled", source, body, label);
    id_observation_failed = true;
    return;
  }
  if (base[source + 0x70] != 1 || !body) return;
  if (label) menu_label_ids[owner].set(label);
  if (stock_ids.test(body) || (label && stock_ids.test(label))) BlockOwner(owner, "new menu uses vetoed text ID");
  if (menu_body_ids[owner].test(body)) return;
  menu_body_ids[owner].set(body);
  if (trace && positive_id_reports++ < 64)
    REXLOG_INFO("[mod-hud-id] menu epoch={} owner={} body={} label={} stock_veto={}",
                epoch, owner, body, label, stock_ids.test(body) || (label && stock_ids.test(label)));
}

size_t HostTables(mod_hud_wire::Table* out, size_t capacity) {
  std::lock_guard lock(mutex);
  size_t count = 0;
  for (const auto& entry : published)
    if (entry.owner != UINT32_MAX && count < capacity) out[count++] = entry.table;
  return count;
}

void StoreJoinedTable(const mod_hud_wire::Table& table) {
  std::lock_guard lock(joined_mutex);
  const auto clear = [] { for (auto& entry : joined) entry = {}; };
  if (table.session != joined_session) {
    clear(); joined_session = table.session; joined_epoch = table.epoch;  // another host, or the host restarted
  } else if (table.epoch != joined_epoch) {
    if (int32_t(table.epoch - joined_epoch) < 0) return;  // from a map the host has left
    clear(); joined_epoch = table.epoch;
  }
  Joined* slot = nullptr;
  for (auto& entry : joined) if (entry.used && entry.table.client == table.client) slot = &entry;
  if (slot && int32_t(table.revision - slot->table.revision) < 0) return;  // overtaken by a newer one
  if (!slot) {
    if (!table.count) return;
    for (auto& entry : joined) if (!entry.used) { slot = &entry; break; }
    if (!slot) return;
  }
  slot->table = table;
  slot->received = std::chrono::steady_clock::now();
  slot->used = true;
  int held = 0;
  for (const auto& entry : joined) held += entry.used && entry.table.count;
  joined_count.store(held, std::memory_order_release);
}

void ResetJoinedTables() {
  std::lock_guard lock(joined_mutex);
  for (auto& entry : joined) entry = {};
  joined_session = joined_epoch = 0;
  joined_count.store(0, std::memory_order_release);
}

void EndMapLoad() {
  std::lock_guard lock(mutex);
  layout = {}; views.fill({}); ++layout_revision;
  map_ready = true;
}

void AfterCopy(const PPCContext& ctx, uint8_t* base) {
  const bool trace = REXCVAR_GET(bo2zm_test_mod_hud_trace), fit = REXCVAR_GET(bo2zm_mod_hud_fit);
  if ((!trace && !fit) || ctx.r5.u32 != 124 || !IsCopySite(uint32_t(ctx.lr))) return;
  const int index = Index(ctx.r4.u32);
  Creation creation;
  const bool fitted = false;  // materials are fitted where they are drawn (PreparePlacement)
  const int target_local = -1;
  const float factor = 1.0f;
  uint64_t revision = 0;
  {
    std::lock_guard lock(mutex);
    creation = index >= 0 ? creations[size_t(index)] : Creation{};
    const bool tagged = creation.epoch == epoch && creation.serial;
    CompleteLiveEvidence(ctx, base, creation);
    if (!trace || !tagged) return;
    revision = layout_revision;
    // Cold/startup copies must not consume the first successful-fit evidence.
    auto& reports = fitted ? fitted_copy_reports : creation.menu ? menu_copy_reports : copy_reports;
    if (reports >= kCopyReports) return;
    ++reports;
  }
  REXLOG_INFO("[mod-hud] copy epoch={} serial={} slot={} source={:08X} destination={:08X} lr={:08X} owner={} type={} synchronous_menu={} unchanged={} fitted={} local={} factor={} view_revision={}",
              creation.epoch, creation.serial, index, ctx.r4.u32, ctx.r3.u32, uint32_t(ctx.lr),
              Word(base, ctx.r4.u32 + 124), base[ctx.r4.u32 + 112], creation.menu,
              !fitted, fitted, target_local, factor, revision);
}
// isHost (the script method, sub_8231DD80) for the loaded mod's scripts: true for any local split-screen player of a
// Local game, and for every player of a System Link game this console hosts (the mod runs only on the host, so a
// joining console's players get its menu this way; bots too, as nothing here tells them apart). r3 holds the entity
// reference left-justified (entity number in bits 48-63, class in 32-47). The game's own scripts, and online play,
// keep the stock answer.
bool LocalHostCall(const uint8_t* base, uint64_t entref) {
  if (!REXCVAR_GET(bo2zm_mod_local_host)) return false;
  const uint32_t number = uint32_t(entref >> 48) & 0xFFFF, kind = uint32_t(entref >> 32) & 0xFFFF;
  if (kind != 0 || number >= 1022) return false;
  if (!ReadScriptStack(base).menu) return false;
  // System Link (bit 1 of the session mode word at 0x837F8CB4, bit 2 online), hosted here: the player's entity has
  // its client on this console's server.
  if (GuestSpan(0x837F8CB4, 4) && (Word(base, 0x837F8CB4) & 6) == 2) {
    const uint32_t entity = 0x833D0640 + number * 796;
    return GuestSpan(entity, 344) && base[entity + 251] && Word(base, entity + 340) >= 0x40000000;
  }
  if (!GuestSpan(0x82BBAE68, 4) || !GuestSpan(0x82C70F44, 4)) return false;
  const uint32_t array = Word(base, 0x82BBAE68), allocated = Word(base, 0x82C70F44);
  if (array < 0x40000000 || !allocated || allocated > 4) return false;
  for (uint32_t local = 0; local < allocated; ++local) {
    const uint32_t manager = 0x837F6A28 + local * 20;
    if (!GuestSpan(manager, 12) || !(Word(base, manager) & 1) || Word(base, manager + 4) != local) continue;
    const uint32_t cg = array + local * 0x87700;
    if (!GuestSpan(cg, 4) || Word(base, cg) != number) continue;
    uint64_t session = 0;
    return LocalListenScope(base, local, session);
  }
  return false;
}

// Trace: the screen placement records (0x82CBC168, 124 bytes each; 0x822A8570 maps a local player to one through
// 0x82403B90) once per layout with more than one view.
void TracePlacements(const uint8_t* base) {
  if (!REXCVAR_GET(bo2zm_test_mod_hud_trace)) return;
  static uint64_t traced_revision = 0;
  std::lock_guard lock(mutex);
  unsigned active = 0;
  for (const auto& actor : layout.actors) if (actor.local >= 0) ++active;
  if (active < 2 || traced_revision == layout_revision || !GuestSpan(0x82CBC168, 124 * 5)) return;
  traced_revision = layout_revision;
  REXLOG_INFO("[mod-hud-fit] HUD scale {} (0x82CBC45C), horizontal factor {} (0x82CBC358)",
              Scalar(base, 0x82CBC45C), Scalar(base, 0x82CBC358));
  for (const auto& actor : layout.actors)
    if (actor.local >= 0)
      REXLOG_INFO("[mod-hud-fit] view local={} owner={} rect={},{} {}x{} image={}x{}", actor.local, actor.number,
                  actor.x, actor.y, actor.width, actor.height, layout.width, layout.height);
  for (uint32_t record = 0; record < 5; ++record) {
    std::string text;
    for (uint32_t field = 0; field < 31; ++field)
      text += fmt::format(" {:02X}={:g}", field * 4, Scalar(base + 0x82CBC168 + record * 124, field * 4));
    REXLOG_INFO("[mod-hud-fit] placement {}:{}", record, text);
  }
}
}  // namespace bo2zm::mod_hud_fit

REX_HOOK_RAW(sub_8232A280) {
  if (!REXCVAR_GET(bo2zm_test_mod_hud_trace) && !REXCVAR_GET(bo2zm_mod_hud_fit)) {
    __imp__sub_8232A280(ctx, base); return;
  }
  const uint32_t caller = uint32_t(ctx.lr), owner = ctx.r3.u32;
  const auto vm = bo2zm::mod_hud_fit::ObserveVm(base, caller);
  uint64_t epoch;
  {
    std::lock_guard lock(bo2zm::mod_hud_fit::mutex);
    epoch = bo2zm::mod_hud_fit::epoch;
  }
  __imp__sub_8232A280(ctx, base);
  bo2zm::mod_hud_fit::Created(ctx.r3.u32, owner, caller, epoch, vm);
}

REX_HOOK_RAW(sub_8237C968) {
  bo2zm::mod_hud_fit::Clear(ctx.r3.u32);
  __imp__sub_8237C968(ctx, base);
}

REX_HOOK_RAW(sub_8232A420) {
  bo2zm::mod_hud_fit::BeginEpoch();
  __imp__sub_8232A420(ctx, base);
}

REX_HOOK_RAW(sub_8223BD90) {
  if (!REXCVAR_GET(bo2zm_mod_hud_fit) && !REXCVAR_GET(bo2zm_test_mod_hud_trace)) {
    __imp__sub_8223BD90(ctx, base); return;
  }
  const uint32_t local = ctx.r3.u32;
  uint64_t observed_epoch;
  {
    std::lock_guard lock(bo2zm::mod_hud_fit::mutex);
    observed_epoch = bo2zm::mod_hud_fit::epoch;
  }
  __imp__sub_8223BD90(ctx, base);
  bo2zm::mod_hud_fit::ObserveView(base, local, observed_epoch);
  bo2zm::mod_hud_fit::TracePlacements(base);
}

REX_EXTERN(__imp__sub_8231DD80);
REX_EXTERN(__imp__sub_8253D758);  // call only: Scr_AddInt
REX_HOOK_RAW(sub_8231DD80) {
  if (!bo2zm::mod_hud_fit::LocalHostCall(base, ctx.r3.u64)) {
    __imp__sub_8231DD80(ctx, base); return;
  }
  // The stock method's own last step: push the answer for the script (r3 = the server VM, r4 = the value).
  PPCContext call = ctx;
  call.r3.u64 = 0; call.r4.u64 = 1;
  __imp__sub_8253D758(call, base);
}

REX_HOOK_RAW(sub_821DC058) {
  bo2zm::mod_hud_fit::ObserveClientDraw(ctx, base);
  auto& active = bo2zm::mod_hud_fit::active_placement;
  active = {};
  bo2zm::mod_hud_fit::RestoreHudScale(base);
  bo2zm::mod_hud_fit::ClientPlacement placement;
  if (!bo2zm::mod_hud_fit::PreparePlacement(ctx, base, placement) &&
      !bo2zm::mod_hud_fit::PrepareJoinedPlacement(ctx, base, placement)) {
    __imp__sub_821DC058(ctx, base); return;
  }
  active = placement;
  bo2zm::mod_hud_fit::UseFullScreenHudScale(base);
  __imp__sub_821DC058(ctx, base);
  bo2zm::mod_hud_fit::RestoreHudScale(base);
  active = {};
}

// The screen placement for a local player (0x822A8570, through the manager's mapping) and by index (0x822A85A0):
// during a fitted draw, the record built for that player's view.
// A material's quad (0x82262A20: x, y, width, height and its texture coordinates s0, t0, s1, t1 in f1-f8, in the
// placement's view pixels). The full screen, shrunk into the view, may be narrower or shorter than the view (two
// players' views are wider): a quad reaching the full screen's edge, as a bar laid across the screen does, is
// carried on to the view's edge on that side, and one wholly off the full screen is not drawn. The full screen also
// cuts what reaches past its edges and a view does not, so a fitted element is then cut to its view, with the
// texture coordinates cut in proportion.
REX_EXTERN(__imp__sub_82262A20);
REX_HOOK_RAW(sub_82262A20) {
  const auto& active = bo2zm::mod_hud_fit::active_placement;
  if (!active.record || ctx.r1.u32 >= active.sp) { __imp__sub_82262A20(ctx, base); return; }
  const uint8_t* place = base + active.record;
  // The shrunk full screen inside the view, and the view.
  const double screen_left = bo2zm::mod_hud_fit::Scalar(place, 0x38);
  const double screen_top = bo2zm::mod_hud_fit::Scalar(place, 0x3C);
  const double screen_right = screen_left + bo2zm::mod_hud_fit::Scalar(place, 0x40);
  const double screen_bottom = screen_top + bo2zm::mod_hud_fit::Scalar(place, 0x44);
  const double left = 0, top = 0, right = active.width, bottom = active.height;
  // One axis: a quad at or past the shrunk screen's edge reaches the view's edge (its texture stretched with it).
  const auto bleed = [](double& p, double& n, double lo, double hi, double screen_lo, double screen_hi) {
    if (!(n > 0.0)) return;
    double p0 = p, p1 = p + n;
    if (p0 <= screen_lo + 0.5 && screen_lo > lo) p0 = std::min(p0, lo);
    if (p1 >= screen_hi - 0.5 && screen_hi < hi) p1 = std::max(p1, hi);
    p = p0; n = p1 - p0;
  };
  // One axis: position p, size n, texture coordinates a..b; false when nothing is left inside lo..hi.
  const auto clip = [](double& p, double& n, double& a, double& b, double lo, double hi) {
    if (!(n > 0.0)) return true;  // degenerate or mirrored quads are drawn as they are
    const double p0 = std::max(p, lo), p1 = std::min(p + n, hi);
    if (p1 <= p0) return false;
    const double a0 = a + (b - a) * (p0 - p) / n, b0 = a + (b - a) * (p1 - p) / n;
    p = p0; n = p1 - p0; a = a0; b = b0;
    return true;
  };
  double x = ctx.f1.f64, y = ctx.f2.f64, w = ctx.f3.f64, h = ctx.f4.f64;
  double s0 = ctx.f5.f64, t0 = ctx.f6.f64, s1 = ctx.f7.f64, t1 = ctx.f8.f64;
  // Off the full screen altogether: not shown there, so not here either.
  const auto off_screen = [](double p, double n, double screen_lo, double screen_hi) {
    return n > 0.0 && (p + n <= screen_lo || p >= screen_hi);
  };
  if (off_screen(x, w, screen_left, screen_right) || off_screen(y, h, screen_top, screen_bottom)) return;
  bleed(x, w, left, right, screen_left, screen_right);
  bleed(y, h, top, bottom, screen_top, screen_bottom);
  if (!clip(x, w, s0, s1, left, right) || !clip(y, h, t0, t1, top, bottom)) return;
  PPCContext call = ctx;
  call.f1.f64 = double(float(x)); call.f2.f64 = double(float(y));
  call.f3.f64 = double(float(w)); call.f4.f64 = double(float(h));
  call.f5.f64 = double(float(s0)); call.f6.f64 = double(float(t0));
  call.f7.f64 = double(float(s1)); call.f8.f64 = double(float(t1));
  __imp__sub_82262A20(call, base);
  ctx.r1 = call.r1;
}

// Engine.GetUserSafeAreaForController (0x8271F3F0): the menus' safe area for a player, which bounds their HUD. r3 is
// the Lua state; it returns how many values it pushed: left, top, right and bottom in the view's menu units, centred
// on the view (a view spans -640..640 by -360..360). In split screen the game keeps the margin only on the screen's
// outer edges (the top-left view gets -448, -252, 640, 360). With bo2zm_split_even_margins every side gets
// bo2zm_split_margin_percent of the view, so every player's HUD sits in the same place; a centred (full-screen) area
// is left as it is.
REX_EXTERN(__imp__sub_8271F3F0);
REX_HOOK_RAW(sub_8271F3F0) {
  const uint32_t L = ctx.r3.u32;
  __imp__sub_8271F3F0(ctx, base);
  if (!REXCVAR_GET(bo2zm_split_even_margins) || ctx.r3.u32 != 4 || !bo2zm::mod_hud_fit::Readable(L + 36)) return;
  const uint32_t top = bo2zm::mod_hud_fit::Word(base, L + 36);
  constexpr uint32_t kTypeNumber = 3;
  std::array<uint32_t, 4> slots{};
  std::array<float, 4> values{};
  for (uint32_t i = 0; i < 4; ++i) {
    slots[i] = top - 8 * (4 - i);
    if (!bo2zm::mod_hud_fit::Readable(slots[i]) || !bo2zm::mod_hud_fit::Readable(slots[i] + 7) ||
        (bo2zm::mod_hud_fit::Word(base, slots[i] + 4) & 0xF) != kTypeNumber) return;
    values[i] = bo2zm::mod_hud_fit::Scalar(base + slots[i], 0);
  }
  // One axis: low..high inside -half..half, with the same margin on both sides.
  const float share = float(REXCVAR_GET(bo2zm_split_margin_percent) / 100.0);
  // A split view is off-centre on at least one axis (a full-width view of two only vertically).
  if (std::fabs(values[0] + values[2]) < 0.5f && std::fabs(values[1] + values[3]) < 0.5f) return;
  const auto even = [share](float& low, float& high) {
    const float half = std::max(std::fabs(low), std::fabs(high));
    if (!(half > 0.0f) || !(high > low)) return;
    const float margin = 2.0f * half * share;
    low = -half + margin; high = half - margin;
  };
  even(values[0], values[2]);
  even(values[1], values[3]);
  for (uint32_t i = 0; i < 4; ++i) bo2zm::mod_hud_fit::PutScalar(base + slots[i], 0, values[i]);
}

REX_EXTERN(__imp__sub_822A8570);
REX_EXTERN(__imp__sub_822A85A0);
REX_HOOK_RAW(sub_822A8570) {
  const auto& active = bo2zm::mod_hud_fit::active_placement;
  if (active.record && ctx.r1.u32 < active.sp) { ctx.r3.u64 = active.record; return; }
  __imp__sub_822A8570(ctx, base);
  ctx.r3.u64 = bo2zm::mod_hud_fit::EvenPlacement(base, ctx.r3.u32);
}
REX_HOOK_RAW(sub_822A85A0) {
  const auto& active = bo2zm::mod_hud_fit::active_placement;
  if (active.record && ctx.r1.u32 < active.sp) { ctx.r3.u64 = active.record; return; }
  __imp__sub_822A85A0(ctx, base);
  ctx.r3.u64 = bo2zm::mod_hud_fit::EvenPlacement(base, ctx.r3.u32);
}
