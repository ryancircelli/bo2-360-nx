// System Link cheat sync: every player's cheats in a System Link Zombies match, host and joiners alike.
//
// Who is who. A console hosts when its own server runs (bo2zm::cheats::LocalServer); it has joined someone else's
// match when the game's client is in a match (cl_ingame) and it does not host (bo2zm::cheats::JoinedRemoteGame). Only
// those two talk, on a UDP port of the runtime's own next to the game's: BO2's ports 1000 and 1001 move up by
// net_system_port_offset (30000 by default) and this one is the game's 1002 moved the same way, 31002 by default.
//
// What a joiner's cheats need. Its own console draws and predicts its player, so the client's side of the cheats runs
// there (bo2zm_cheats.cpp's predicted actors: the fly modes, jumps, feather fall and the view kick in its prediction,
// the field of view, the third person camera and crosshair). The server's side runs on the host:
//   - Player options (detail::kSyncedOptions) go to the host, which applies them to that player's server entity
//     (bo2zm::cheats::SetRemotePlayer). Sent once when the match starts, again whenever one changes, and resent after
//     1 s and 3 s until the host has answered for every player with the catalog and world revisions this console has.
//   - Commands (perks, guns, points, power-ups, and the match-wide ones: rounds, doors, power, debris, stop spawns,
//     kill all, and the world settings game speed, zombies freeze, gravity and unlimited sprint) go to the host when
//     the player picks them, and the host runs them for that player (bo2zm::cheats::RunRemoteCommand) and answers with
//     the result. Resent after 1 s and 3 s until answered; the host runs each command once.
//   - The host sends its menu catalog (the map's guns, perks and power-ups) and its world settings to each joiner when
//     the joiner does not have the latest (its options say which it has) and whenever they change; the joiner shows the
//     catalog in its menus and holds the world settings too, since its prediction reads gravity and sprint itself.
//   - The host sends a joiner's explosive bullet hits back to it, so its console draws the explosions.
// Nothing is sent otherwise. A joiner sends to the address of the host's last message, or else the console its game
// socket last heard on BO2's game port (system_link::LastPeer; a joiner's server traffic comes from the host); until
// it knows one, and for a last resend in case the address taken was another player's, to the LAN's broadcast address
// (online, joined by a code: to the code's host instead).
// The host listens while its server runs and another console is talking to its game port. Player XUIDs are unique per
// console (UserProfile, console_id.txt); the host ignores this console's own.
//
// Messages, all numbers big-endian. The header (12 bytes):
//   0 magic "BZCS"   4 version (2)   5 type   6 count   7 aux   8 sequence (u32)
// 1 options  (joiner -> host): count players, aux options per player, sequence the joiner's message number (newer is
//            larger, wrapping). Body: catalog revision (u32), world revision (u32) the joiner has, then per player its
//            XUID (u64) and one byte per synced option.
// 2 answer   (host -> joiner): count players, sequence the options message's. Body: the host's catalog and world
//            revisions, then per player its XUID and server client (255: none yet).
// 3 command  (joiner -> host): aux the command (kCommands), sequence the joiner's command number. Body: XUID, flags
//            (1: Pack-a-Punch the gun), argument length (u8) and the argument.
// 4 result   (host -> joiner): aux 0 done, 1 refused; sequence the command's. Body: XUID, text length (u8), text.
// 5 catalog  (host -> joiner): count the part (0..), aux the parts, sequence the catalog revision. Body: the part.
// 6 world    (host -> joiner): sequence the world revision. Body: text length (u16), text.
// 7 blasts   (host -> joiner): count hits (1..8). Body: XUID, then each hit's x, y, z (floats).
// 8 menus?   (joiner -> host): aux the menu table format (bo2zm_mod_hud_wire.h), no body. "This console fits the host's
//            mod menus": sent every 2 s while in a joined match, Zombies and Multiplayer alike.
// 9 menus    (host -> joiner): count entries, aux the format, sequence the table's revision; the body is one player's
//            menu table (bo2zm_mod_hud_wire.h). Sent to every console that asked in the last 6 s, when a table changes
//            and every second while it holds entries.
// Consoles of an older build ignore types 8 and 9; a table of another format is ignored.
// The receiver checks the size, magic, version, type, counts, characters and every value's range before using any of
// it, and the host checks each command again the way the menu does.

#include "bo2zm_cheat_sync.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/online_link.h>
#include <rex/system/xsocket.h>

#include "bo2zm_cheats.h"
#include "bo2zm_mod_hud_fit.h"
#include "bo2zm_mod_hud_wire.h"
#include "bo2zm_mode.h"
#include "bo2zm_preset.h"

REXCVAR_DEFINE_BOOL(bo2zm_cheat_sync, true, "BO2",
                    "System Link: send this console's player cheats and cheat commands to the host of a match it "
                    "joined, and run those of players who joined this console's match");
REXCVAR_DECLARE(bool, bo2zm_mod_hud_fit);
REXCVAR_DEFINE_BOOL(bo2zm_cheat_sync_trace, false, "BO2",
                    "System Link: log every cheat sync message sent and received (the rare events are always logged)");

namespace {

namespace link = rex::system::system_link;
namespace detail = bo2zm::cheats::detail;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x425A4353;  // "BZCS"
constexpr uint8_t kVersion = 2;
enum : uint8_t {
  kOptions = 1, kAnswer = 2, kCommand = 3, kResult = 4, kCatalog = 5, kWorld = 6, kBlasts = 7, kMenuAsk = 8, kMenus = 9
};
constexpr size_t kHeader = 12;
constexpr size_t kRevisions = 8;
constexpr int kMaxPlayers = 4;
constexpr size_t kOptionsEntry = 8 + detail::kSyncedCount;
constexpr size_t kAnswerEntry = 9;
constexpr size_t kMaxDatagram = 1400;
constexpr size_t kCatalogPart = 1200;
constexpr int kMaxCatalogParts = 16;
constexpr int kMaxBlasts = 8;
constexpr size_t kMaxArgument = 64;
constexpr uint8_t kNoClient = 255;
constexpr uint16_t kGamePort = 1000;  // BO2's game socket (the server's and the client's)
constexpr uint16_t kSidePort = 1002;  // ours, moved like the game's (link::HostPort)
constexpr auto kPeerFresh = std::chrono::milliseconds(5000);  // a game peer heard from this recently is live
constexpr std::chrono::milliseconds kResends[] = {std::chrono::milliseconds(1000), std::chrono::milliseconds(3000)};
constexpr int kLast = int(std::size(kResends));
constexpr int kHostPollEvery = 4;  // frames between the host's looks at its socket
constexpr int kJoinPollEvery = 2;  // and the joiner's (the host's explosions should show without delay)

// The commands a joiner sends, by number (the message's aux byte).
constexpr std::string_view kCommands[] = {
    "", "bo2zm_perk", "bo2zm_remove_perks", "bo2zm_give_weapon", "bo2zm_pap_weapon", "bo2zm_max_points",
    "bo2zm_powerup", "bo2zm_kill_zombies", "bo2zm_doors", "bo2zm_power", "bo2zm_debris", "bo2zm_goto_round",
    "bo2zm_zm_nospawn", "bo2zm_hold", "bo2zm_release"};

bool Trace() { return REXCVAR_GET(bo2zm_cheat_sync_trace); }

std::string Ip(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

void Put16(uint8_t* at, uint16_t v) {
  at[0] = uint8_t(v >> 8);
  at[1] = uint8_t(v);
}
void Put32(uint8_t* at, uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (24 - 8 * i));
}
void Put64(uint8_t* at, uint64_t v) {
  for (int i = 0; i < 8; ++i) at[i] = uint8_t(v >> (56 - 8 * i));
}
uint16_t Get16(const uint8_t* at) { return uint16_t(at[0] << 8 | at[1]); }
uint32_t Get32(const uint8_t* at) {
  return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3];
}
uint64_t Get64(const uint8_t* at) { return uint64_t(Get32(at)) << 32 | Get32(at + 4); }

// A message being built.
struct Message {
  std::array<uint8_t, kMaxDatagram> bytes{};
  size_t size = kHeader;
  Message(uint8_t type, uint8_t count, uint8_t aux, uint32_t sequence) {
    Put32(bytes.data(), kMagic);
    bytes[4] = kVersion;
    bytes[5] = type;
    bytes[6] = count;
    bytes[7] = aux;
    Put32(bytes.data() + 8, sequence);
  }
  bool Room(size_t more) const { return size + more <= bytes.size(); }
  void U8(uint8_t v) { bytes[size++] = v; }
  void U16(uint16_t v) { Put16(bytes.data() + size, v); size += 2; }
  void U32(uint32_t v) { Put32(bytes.data() + size, v); size += 4; }
  void U64(uint64_t v) { Put64(bytes.data() + size, v); size += 8; }
  void Text(std::string_view text) { std::memcpy(bytes.data() + size, text.data(), text.size()); size += text.size(); }
};

// A command argument or result text: what the menu itself sends (names, numbers, a dvar and its value).
bool PlainText(std::string_view text, size_t most) {
  return text.size() <= most &&
         text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_. ,()'-") ==
             std::string_view::npos;
}

// The XUID the game plays this controller's profile with (UserProfile::xuid), 0 without one.
uint64_t ControllerXuid(int controller) {
  auto* kernel_state = REX_KERNEL_STATE();
  auto* profile = kernel_state && controller >= 0 && controller < detail::kLocalPlayers
                      ? kernel_state->user_profile(uint32_t(controller))
                      : nullptr;
  return profile ? profile->xuid() : 0;
}

bool OwnXuid(uint64_t xuid) {
  for (int controller = 0; controller < detail::kLocalPlayers; ++controller) {
    if (ControllerXuid(controller) == xuid) return true;
  }
  return false;
}

std::string OptionsText(const int* values) {
  std::string text;
  for (int i = 0; i < detail::kSyncedCount; ++i) {
    const auto option = detail::kSyncedOptions[i];
    if (values[i] == 0) continue;
    if (!text.empty()) text += ' ';
    text += fmt::format("{}={}", detail::OptionName(option), values[i]);
  }
  return text.empty() ? "all off" : text;
}

// The side socket, opened the first time it is needed and kept.
int64_t Socket() {
  static int64_t socket = -1;
  static bool tried = false;
  if (!tried) {
    tried = true;
    bool bound = false;
    const uint16_t port = link::HostPort(kSidePort);
    socket = link::OpenSideSocket(port, &bound);
    REXLOG_INFO("[cheat sync] side socket on port {}: {}", port,
                socket < 0 ? "not available" : bound ? "open" : "open, sending only (the port is taken)");
  }
  return socket;
}

int Send(uint32_t ip, uint16_t port, const Message& message) {
  const int64_t socket = Socket();
  return socket < 0 ? -1 : link::SideSend(socket, ip, port, message.bytes.data(), uint32_t(message.size));
}

// A received datagram whose header is ours: its type, count, aux and sequence, the body after the header.
struct Received {
  uint8_t type = 0, count = 0, aux = 0;
  uint32_t sequence = 0;
  const uint8_t* body = nullptr;
  size_t body_size = 0;
  uint32_t ip = 0;
  uint16_t port = 0;
};

bool Receive(std::array<uint8_t, 2048>& buffer, Received& out) {
  const int64_t socket = Socket();
  if (socket < 0) return false;
  for (;;) {
    const int got = link::SideReceive(socket, buffer.data(), uint32_t(buffer.size()), &out.ip, &out.port);
    if (got < 0) return false;
    const size_t size = size_t(got);
    if (size < kHeader || Get32(buffer.data()) != kMagic) {
      if (Trace()) REXLOG_INFO("[cheat sync] ignored a {}-byte datagram from {}:{} (not ours)", size, Ip(out.ip), out.port);
      continue;
    }
    if (buffer[4] != kVersion) {
      static int logged = 0;
      if (logged++ < 3) {
        REXLOG_WARN("[cheat sync] {} sends version {} messages (this build reads {}); ignored", Ip(out.ip), buffer[4],
                    kVersion);
      }
      continue;
    }
    out.type = buffer[5];
    out.count = buffer[6];
    out.aux = buffer[7];
    out.sequence = Get32(buffer.data() + 8);
    out.body = buffer.data() + kHeader;
    out.body_size = size - kHeader;
    return true;
  }
}

// ---- Mod menus (bo2zm_mod_hud_wire.h), both sides; Zombies and Multiplayer ----

namespace wire = bo2zm::mod_hud_wire;
constexpr auto kMenuAskEvery = std::chrono::milliseconds(2000);
constexpr auto kMenuPeerFresh = std::chrono::milliseconds(6000);
constexpr auto kMenuEvery = std::chrono::milliseconds(1000);
constexpr size_t kMaxTables = 32;

// The host: the consoles that asked for the menus, and what each player's table was when last sent.
struct MenuPeer {
  uint32_t ip = 0;
  uint16_t port = 0;
  Clock::time_point heard{};
};
std::array<MenuPeer, 8> g_menu_peers;
struct MenuSent {
  bool used = false;
  uint16_t client = 0;
  uint32_t revision = 0;
  Clock::time_point at{};
  int empty_sends = 0;
};
std::array<MenuSent, kMaxTables> g_menu_sent;

void MenuAsked(const Received& in) {
  if (in.aux != wire::kFormat || in.body_size != 0) return;  // another format: that console cannot read these tables
  const auto now = Clock::now();
  MenuPeer* slot = nullptr;
  for (auto& peer : g_menu_peers) if (peer.ip == in.ip && peer.port == in.port) slot = &peer;
  if (!slot) {
    slot = &g_menu_peers[0];
    for (auto& peer : g_menu_peers) if (!peer.ip || peer.heard < slot->heard) slot = &peer;
    if (slot->ip) REXLOG_WARN("[cheat sync] mod menus: too many consoles ask; {} is dropped", Ip(slot->ip));
    REXLOG_INFO("[cheat sync] mod menus: {}:{} fits the host's menus; sending it the players' tables", Ip(in.ip),
                in.port);
    *slot = {in.ip, in.port, now};
  }
  slot->heard = now;
}

// Each player's table to every console that asked recently: when it changes, every second while it has entries, and
// an emptied one once more a second later.
void MenuPush() {
  const auto now = Clock::now();
  int peers = 0;
  for (auto& peer : g_menu_peers) {
    if (peer.ip && now - peer.heard > kMenuPeerFresh) {
      REXLOG_INFO("[cheat sync] mod menus: {} stopped asking", Ip(peer.ip));
      peer = {};
    }
    peers += peer.ip != 0;
  }
  if (!peers) return;
  static std::array<wire::Table, kMaxTables> tables;  // 38 KB, off the stack; only the main frame comes here
  const size_t count = bo2zm::mod_hud_fit::HostTables(tables.data(), tables.size());
  for (size_t i = 0; i < count; ++i) {
    const auto& table = tables[i];
    MenuSent* sent = nullptr;
    for (auto& entry : g_menu_sent) if (entry.used && entry.client == table.client) sent = &entry;
    if (!sent) {
      for (auto& entry : g_menu_sent) if (!entry.used) { sent = &entry; break; }
      if (!sent) continue;
      *sent = {true, table.client, table.revision - 1, {}, 0};
    }
    const bool changed = sent->revision != table.revision;
    if (!changed && (now - sent->at < kMenuEvery || (!table.count && sent->empty_sends >= 2))) continue;
    Message message(kMenus, table.count, wire::kFormat, table.revision);
    if (!message.Room(wire::kBodyHeader + size_t(table.count) * wire::kEntryBytes)) continue;
    const size_t size = wire::Encode(table, message.bytes.data() + message.size);
    if (!size) continue;
    message.size += size;
    for (const auto& peer : g_menu_peers)
      if (peer.ip) Send(peer.ip, peer.port, message);
    sent->empty_sends = table.count ? 0 : changed ? 1 : sent->empty_sends + 1;
    sent->revision = table.revision;
    sent->at = now;
    if (Trace()) {
      REXLOG_INFO("[cheat sync] mod menus: player {} table {} ({} elements) to {} console(s)", table.client,
                  table.revision, table.count, peers);
    }
  }
}

void MenuHostStop() {
  g_menu_peers = {};
  g_menu_sent = {};
}

// The console that joined: where its tables come from, and when it last asked.
uint32_t g_menu_host = 0;
Clock::time_point g_menu_asked{};
bool g_menu_joined = false;

uint32_t Destination(bool broadcast, bool& sent_broadcast);

void MenuAsk() {
  if (!REXCVAR_GET(bo2zm_mod_hud_fit)) return;
  g_menu_joined = true;
  const auto now = Clock::now();
  if (g_menu_asked != Clock::time_point{} && now - g_menu_asked < kMenuAskEvery) return;
  g_menu_asked = now;
  bool broadcast = false;
  const uint32_t to = g_menu_host ? g_menu_host : Destination(false, broadcast);
  if (!to) return;
  Message message(kMenuAsk, 0, wire::kFormat, 0);
  const int sent = Send(to, link::HostPort(kSidePort), message);
  if (Trace() || sent < 0) {
    REXLOG_INFO("[cheat sync] mod menus: asked {}{}{}", Ip(to), broadcast ? " (the LAN's broadcast)" : "",
                sent < 0 ? " -- the send failed" : "");
  }
}

void MenuReceived(const Received& in) {
  if (!REXCVAR_GET(bo2zm_mod_hud_fit)) return;
  wire::Table table;
  if (!wire::Decode(in.aux, in.count, in.sequence, in.body, in.body_size, table)) {
    static int logged = 0;
    if (logged++ < 3) {
      REXLOG_WARN("[cheat sync] mod menus: ignored a table from {} (format {}, {} entries, {} bytes)", Ip(in.ip),
                  in.aux, in.count, in.body_size);
    }
    return;
  }
  if (g_menu_host != in.ip) {
    REXLOG_INFO("[cheat sync] mod menus: tables from {}", Ip(in.ip));
    g_menu_host = in.ip;
  }
  bo2zm::mod_hud_fit::StoreJoinedTable(table);
}

void MenuLeave() {
  if (!g_menu_joined) return;
  g_menu_joined = false;
  g_menu_host = 0;
  g_menu_asked = {};
  bo2zm::mod_hud_fit::ResetJoinedTables();
}

// ---- The joiner ----

struct Command {
  uint32_t sequence = 0;
  uint64_t xuid = 0;
  uint8_t id = 0;
  bool pap = false;
  std::string argument;
  Clock::time_point first_at{}, sent_at{};
  int resends = 0;
};

struct Joiner {
  bool active = false;
  uint32_t sequence = 0;
  int count = 0;
  std::array<uint64_t, kMaxPlayers> xuids{};
  std::array<std::array<int, detail::kSyncedCount>, kMaxPlayers> values{};
  std::array<int, kMaxPlayers> clients{};  // as the host answered, -1 not yet
  bool answered = false;  // the host answered the latest message for every player, with this console's revisions
  Clock::time_point first_at{};  // the latest message's first send
  Clock::time_point sent_at{};   // its last send
  int resends = 0;
  bool sent_broadcast = false;
  uint32_t host_ip = 0;  // from the host's messages
  uint32_t catalog_revision = 0, world_revision = 0;  // what this console has from the host
  uint32_t catalog_building = 0;                      // the revision whose parts are arriving
  std::vector<std::string> catalog_parts;
  std::vector<bool> catalog_have;
  uint32_t command_sequence = 0;
  std::vector<Command> commands;  // waiting for their result
};
Joiner g_join;
std::mutex g_forward_mutex;
std::vector<Command> g_forwarded;  // from Forward (any thread), sent by the next frame; under g_forward_mutex

// The host's address, 0 when not known yet.
uint32_t HostAddress() {
  if (g_join.host_ip) return g_join.host_ip;
  uint32_t ip = 0;
  int64_t age = 0;
  return link::LastPeer(kGamePort, &ip, &age) && age < kPeerFresh.count() ? ip : 0;
}

// Where a joiner's message goes: the host, or the LAN's broadcast address. 0: nowhere (no LAN). Joined by a code
// over the internet, a broadcast would never reach the host: the code's address stands in for it.
uint32_t Destination(bool broadcast, bool& sent_broadcast) {
  uint32_t to = broadcast ? 0 : HostAddress();
  if (!to) to = rex::system::online::Target();
  sent_broadcast = to == 0;
  if (!to) {
    uint32_t ip = 0, mask = 0;
    if (!link::LanAddress(&ip, &mask) || !mask) {
      REXLOG_WARN("[cheat sync] no LAN address: nothing is sent");
      return 0;
    }
    to = ip | ~mask;
  }
  return to;
}

void SendOptions(const char* why, bool broadcast = false) {
  if (g_join.count == 0) return;
  Message message(kOptions, uint8_t(g_join.count), uint8_t(detail::kSyncedCount), g_join.sequence);
  message.U32(g_join.catalog_revision);
  message.U32(g_join.world_revision);
  for (int p = 0; p < g_join.count; ++p) {
    message.U64(g_join.xuids[p]);
    for (int i = 0; i < detail::kSyncedCount; ++i) message.U8(uint8_t(g_join.values[p][i]));
  }
  const uint32_t to = Destination(broadcast, g_join.sent_broadcast);
  if (!to) return;
  const uint16_t port = link::HostPort(kSidePort);
  const int sent = Send(to, port, message);
  g_join.sent_at = Clock::now();
  for (int p = 0; p < g_join.count; ++p) {
    REXLOG_INFO("[cheat sync] sent options ({}, message {}) to {}:{}{}: player {:016X} {}{}", why, g_join.sequence,
                Ip(to), port, g_join.sent_broadcast ? " (the LAN's broadcast)" : "", g_join.xuids[p],
                OptionsText(g_join.values[p].data()), sent < 0 ? " -- the send failed" : "");
  }
}

// A new options message with the current values: at the match start, on every change, and when the host has newer
// catalog or world settings than this console got.
void SendNew(const char* why) {
  ++g_join.sequence;
  g_join.answered = false;
  g_join.resends = 0;
  g_join.clients.fill(-1);
  g_join.first_at = Clock::now();
  SendOptions(why);
}

void SendCommand(Command& command, const char* why, bool broadcast = false) {
  Message message(kCommand, 1, command.id, command.sequence);
  message.U64(command.xuid);
  message.U8(command.pap ? 1 : 0);
  message.U8(uint8_t(command.argument.size()));
  message.Text(command.argument);
  bool sent_broadcast = false;
  const uint32_t to = Destination(broadcast, sent_broadcast);
  if (!to) return;
  const uint16_t port = link::HostPort(kSidePort);
  const int sent = Send(to, port, message);
  command.sent_at = Clock::now();
  REXLOG_INFO("[cheat sync] sent command {} ({}, number {}) to {}:{}{}: player {:016X} {} {}{}", command.id, why,
              command.sequence, Ip(to), port, sent_broadcast ? " (the LAN's broadcast)" : "", command.xuid,
              kCommands[command.id], command.argument, sent < 0 ? " -- the send failed" : "");
}

void JoinerReceived(PPCContext& ctx, uint8_t* base, const Received& in) {
  if (in.type == kAnswer) {
    const int count = in.count;
    if (count < 1 || count > kMaxPlayers || in.body_size != kRevisions + count * kAnswerEntry) {
      REXLOG_WARN("[cheat sync] ignored a malformed answer from {}", Ip(in.ip));
      return;
    }
    if (in.sequence != g_join.sequence) {
      if (Trace()) REXLOG_INFO("[cheat sync] answer from {} to an older message ({}); ignored", Ip(in.ip), in.sequence);
      return;
    }
    const uint32_t catalog = Get32(in.body), world = Get32(in.body + 4);
    for (int a = 0; a < count; ++a) {
      const uint8_t* entry = in.body + kRevisions + a * kAnswerEntry;
      const uint64_t xuid = Get64(entry);
      for (int p = 0; p < g_join.count; ++p) {
        if (g_join.xuids[p] == xuid) g_join.clients[p] = entry[8] == kNoClient ? -1 : entry[8];
      }
    }
    bool all = true;
    for (int p = 0; p < g_join.count; ++p) all &= g_join.clients[p] >= 0;
    const bool current = catalog == g_join.catalog_revision && world == g_join.world_revision;
    if (all && !g_join.answered) {
      for (int p = 0; p < g_join.count; ++p) {
        REXLOG_INFO("[cheat sync] the host applies options message {} to player {:016X} (its server client {})",
                    g_join.sequence, g_join.xuids[p], g_join.clients[p]);
      }
    }
    if (Trace() && (!all || !current)) {
      REXLOG_INFO("[cheat sync] the host at {}: {} catalog {} (here {}), world {} (here {})", Ip(in.ip),
                  all ? "has every player," : "does not have every player yet,", catalog, g_join.catalog_revision,
                  world, g_join.world_revision);
    }
    g_join.answered = all && current;
    return;
  }
  if (in.type == kResult) {
    if (in.body_size < 9 || in.body_size != 9 + size_t(in.body[8]) || in.aux > 1) {
      REXLOG_WARN("[cheat sync] ignored a malformed result from {}", Ip(in.ip));
      return;
    }
    const std::string_view text(reinterpret_cast<const char*>(in.body + 9), in.body[8]);
    const auto it = std::find_if(g_join.commands.begin(), g_join.commands.end(),
                                 [&](const Command& command) { return command.sequence == in.sequence; });
    if (it == g_join.commands.end()) return;  // a repeated answer
    if (!PlainText(text, 255)) return;
    REXLOG_INFO("[cheat sync] the host {} {} {} for player {:016X}{}{}", in.aux == 0 ? "ran" : "refused",
                kCommands[it->id], it->argument, it->xuid, text.empty() ? "" : ": ", text);
    g_join.commands.erase(it);
    return;
  }
  if (in.type == kCatalog) {
    const int part = in.count, parts = in.aux;
    if (parts < 1 || parts > kMaxCatalogParts || part >= parts || in.body_size > kCatalogPart) {
      REXLOG_WARN("[cheat sync] ignored a malformed catalog part from {}", Ip(in.ip));
      return;
    }
    if (in.sequence == g_join.catalog_revision) return;  // have it
    if (in.sequence != g_join.catalog_building || int(g_join.catalog_parts.size()) != parts) {
      g_join.catalog_building = in.sequence;
      g_join.catalog_parts.assign(parts, std::string());
      g_join.catalog_have.assign(parts, false);
    }
    g_join.catalog_parts[part].assign(reinterpret_cast<const char*>(in.body), in.body_size);
    g_join.catalog_have[part] = true;
    if (std::all_of(g_join.catalog_have.begin(), g_join.catalog_have.end(), [](bool have) { return have; })) {
      std::string text;
      for (const auto& piece : g_join.catalog_parts) text += piece;
      bo2zm::cheats::ApplyCatalog(ctx, base, text);
      g_join.catalog_revision = in.sequence;
      g_join.catalog_parts.clear();
      g_join.catalog_have.clear();
      REXLOG_INFO("[cheat sync] the host's catalog {} received ({} bytes)", in.sequence, text.size());
    }
    return;
  }
  if (in.type == kWorld) {
    if (in.body_size < 2 || in.body_size != 2 + size_t(Get16(in.body)) || in.body_size > 512) {
      REXLOG_WARN("[cheat sync] ignored malformed world settings from {}", Ip(in.ip));
      return;
    }
    if (in.sequence == g_join.world_revision) return;
    const std::string_view text(reinterpret_cast<const char*>(in.body + 2), Get16(in.body));
    if (text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.=;") !=
        std::string_view::npos) {
      REXLOG_WARN("[cheat sync] the host's world settings have unexpected characters; ignored");
      return;
    }
    bo2zm::cheats::ApplyWorldState(text);
    g_join.world_revision = in.sequence;
    REXLOG_INFO("[cheat sync] the host's world settings {}: {}", in.sequence, text.empty() ? "the game's own" : text);
    return;
  }
  if (in.type == kBlasts) {
    const int count = in.count;
    if (count < 1 || count > kMaxBlasts || in.body_size != 8 + size_t(count) * 12) return;
    const uint64_t xuid = Get64(in.body);
    bool ours = false;
    for (int p = 0; p < g_join.count; ++p) ours |= g_join.xuids[p] == xuid;
    if (!ours) return;
    for (int h = 0; h < count; ++h) {
      float at[3];
      for (int i = 0; i < 3; ++i) {
        const uint32_t bits = Get32(in.body + 8 + h * 12 + i * 4);
        std::memcpy(&at[i], &bits, 4);
      }
      if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) return;
      bo2zm::cheats::PlayRemoteBlast(at);
    }
    if (Trace()) REXLOG_INFO("[cheat sync] {} explosive hit(s) from the host to draw", count);
    return;
  }
  if (in.type == kMenus) {
    MenuReceived(in);
    return;
  }
  if (Trace()) REXLOG_INFO("[cheat sync] ignored a type {} message from {}", in.type, Ip(in.ip));
}

void JoinerPoll(PPCContext& ctx, uint8_t* base) {
  std::array<uint8_t, 2048> buffer{};
  Received in;
  while (Receive(buffer, in)) {
    if (in.type == kOptions || in.type == kCommand || in.type == kMenuAsk) continue;  // another joiner's broadcast
    if (g_join.host_ip != in.ip) {
      REXLOG_INFO("[cheat sync] the host is {}", Ip(in.ip));
      g_join.host_ip = in.ip;
    }
    JoinerReceived(ctx, base, in);
  }
}

void Join(PPCContext& ctx, uint8_t* base) {
  static int frames = 0;
  if (++frames >= kJoinPollEvery) {
    frames = 0;
    JoinerPoll(ctx, base);
  }
  MenuAsk();
  bo2zm::cheats::SyncedPlayer players[kMaxPlayers];
  const int count = bo2zm::cheats::LocalSyncedPlayers(players, kMaxPlayers);
  Joiner now = g_join;
  now.count = 0;
  for (int p = 0; p < count; ++p) {
    const uint64_t xuid = ControllerXuid(players[p].controller);
    if (!xuid) continue;
    now.xuids[now.count] = xuid;
    for (int i = 0; i < detail::kSyncedCount; ++i) now.values[now.count][i] = players[p].values[i];
    ++now.count;
  }
  bool same = now.count == g_join.count;
  for (int p = 0; same && p < now.count; ++p) {
    same = now.xuids[p] == g_join.xuids[p] && now.values[p] == g_join.values[p];
  }
  if (!g_join.active) {
    // The sequences start from the clock, so a restarted game's messages are newer than its earlier ones.
    const uint32_t start = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now().time_since_epoch()).count());
    g_join = {};
    g_join.active = true;
    g_join.count = now.count;
    g_join.xuids = now.xuids;
    g_join.values = now.values;
    g_join.sequence = start;
    g_join.command_sequence = start;
    REXLOG_INFO("[cheat sync] in a match this console joined: sending its {} player(s)' cheats to the host",
                g_join.count);
    SendNew("match start");
  } else if (!same) {
    g_join.count = now.count;
    g_join.xuids = now.xuids;
    g_join.values = now.values;
    SendNew("options changed");
  } else if (!g_join.answered) {
    const auto now_time = Clock::now();
    if (g_join.resends < kLast) {
      if (g_join.sent_broadcast && g_join.resends == 0 && HostAddress()) {
        SendOptions("the host is known now");  // the same message, now to the host itself
      } else if (now_time - g_join.first_at >= kResends[g_join.resends]) {
        ++g_join.resends;
        // The last try goes to the whole LAN, in case the address taken for the host was another player's.
        SendOptions("resend", g_join.resends == kLast);
      }
    } else if (g_join.resends == kLast && now_time - g_join.sent_at >= std::chrono::seconds(1)) {
      ++g_join.resends;
      REXLOG_WARN("[cheat sync] the host did not answer options message {} for every player; it is sent again on the "
                  "next change", g_join.sequence);
    }
  }

  // Commands: the new ones, then the resends.
  std::vector<Command> forwarded;
  {
    std::lock_guard lock(g_forward_mutex);
    forwarded.swap(g_forwarded);
  }
  for (auto& command : forwarded) {
    if (g_join.commands.size() >= 8) {
      REXLOG_WARN("[cheat sync] too many commands waiting for the host; {} dropped", kCommands[command.id]);
      continue;
    }
    command.sequence = ++g_join.command_sequence;
    command.first_at = Clock::now();
    SendCommand(command, "picked");
    g_join.commands.push_back(std::move(command));
  }
  const auto now_time = Clock::now();
  for (auto it = g_join.commands.begin(); it != g_join.commands.end();) {
    if (it->resends < kLast) {
      if (now_time - it->first_at >= kResends[it->resends]) {
        ++it->resends;
        SendCommand(*it, "resend", it->resends == kLast);
      }
      ++it;
    } else if (now_time - it->sent_at >= std::chrono::seconds(1)) {
      REXLOG_WARN("[cheat sync] the host did not answer {} {}; it was not run", kCommands[it->id], it->argument);
      it = g_join.commands.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- The host ----

struct Peer {
  uint32_t ip = 0;
  uint16_t port = 0;
  std::array<uint64_t, kMaxPlayers> xuids{};
  int count = 0;
  uint32_t catalog_sent = 0, world_sent = 0;  // the revisions last sent to it
};
std::array<Peer, 8> g_peers;
struct Done {
  uint32_t ip = 0;
  uint64_t xuid = 0;
  uint32_t sequence = 0;
  uint8_t status = 0;
  std::string why;
};
std::array<Done, 16> g_done;  // the latest commands run, so a resend gets the same answer and runs nothing again
size_t g_done_next = 0;
uint32_t g_world_revision = 1;
std::string g_world_text;
Clock::time_point g_world_again{};  // a second push of a change, a second later (pushes are not answered)
uint32_t g_catalog_pushed = 0;
Clock::time_point g_catalog_again{};  // the second push of a new catalog

Peer* PeerAt(uint32_t ip, uint16_t port, bool add) {
  for (auto& peer : g_peers) {
    if (peer.ip == ip && peer.port == port) return &peer;
  }
  if (!add) return nullptr;
  for (auto& peer : g_peers) {
    if (!peer.ip) {
      peer = {};
      peer.ip = ip;
      peer.port = port;
      return &peer;
    }
  }
  g_peers[0] = {};
  g_peers[0].ip = ip;
  g_peers[0].port = port;
  return &g_peers[0];
}

void SendCatalog(Peer& peer, const std::string& text, uint32_t revision) {
  const int parts = int((text.size() + kCatalogPart - 1) / kCatalogPart);
  if (parts < 1 || parts > kMaxCatalogParts) {
    REXLOG_WARN("[cheat sync] the catalog ({} bytes) is too big to send", text.size());
    return;
  }
  for (int part = 0; part < parts; ++part) {
    Message message(kCatalog, uint8_t(part), uint8_t(parts), revision);
    message.Text(std::string_view(text).substr(part * kCatalogPart, kCatalogPart));
    Send(peer.ip, peer.port, message);
  }
  peer.catalog_sent = revision;
  REXLOG_INFO("[cheat sync] sent the catalog {} ({} bytes in {} part(s)) to {}", revision, text.size(), parts,
              Ip(peer.ip));
}

void SendWorld(Peer& peer) {
  Message message(kWorld, 0, 0, g_world_revision);
  message.U16(uint16_t(g_world_text.size()));
  message.Text(g_world_text);
  Send(peer.ip, peer.port, message);
  peer.world_sent = g_world_revision;
  REXLOG_INFO("[cheat sync] sent the world settings {} to {}: {}", g_world_revision, Ip(peer.ip),
              g_world_text.empty() ? "the game's own" : g_world_text);
}

void HostOptions(const uint8_t* base, const Received& in) {
  const int count = in.count;
  if (count < 1 || count > kMaxPlayers || in.aux != detail::kSyncedCount ||
      in.body_size != kRevisions + count * kOptionsEntry) {
    REXLOG_WARN("[cheat sync] ignored a malformed options message from {} ({} players, {} options)", Ip(in.ip), count,
                in.aux);
    return;
  }
  Message answer(kAnswer, 0, 0, in.sequence);
  std::string catalog;
  uint32_t catalog_revision = 0;
  bo2zm::cheats::CatalogText(catalog, catalog_revision);
  answer.U32(catalog_revision);
  answer.U32(g_world_revision);
  Peer* peer = PeerAt(in.ip, in.port, true);
  int answered = 0;
  for (int p = 0; p < count; ++p) {
    const uint8_t* entry = in.body + kRevisions + p * kOptionsEntry;
    const uint64_t xuid = Get64(entry);
    int values[detail::kSyncedCount];
    bool valid = xuid != 0;
    for (int i = 0; i < detail::kSyncedCount; ++i) {
      values[i] = entry[8 + i];
      valid &= values[i] <= detail::OptionMaximum(detail::kSyncedOptions[i]);
    }
    if (!valid) {
      REXLOG_WARN("[cheat sync] ignored player {:016X} from {}: a value out of range", xuid, Ip(in.ip));
      continue;
    }
    if (OwnXuid(xuid)) {
      if (Trace()) REXLOG_INFO("[cheat sync] {:016X} from {} is one of this console's players; ignored", xuid, Ip(in.ip));
      continue;
    }
    int client = -1;
    bool changed = false;
    if (!bo2zm::cheats::SetRemotePlayer(base, xuid, in.sequence, values, &client, &changed)) {
      if (Trace()) REXLOG_INFO("[cheat sync] options {} for {:016X} from {} not taken", in.sequence, xuid, Ip(in.ip));
      continue;
    }
    if (changed || Trace()) {
      REXLOG_INFO("[cheat sync] received options {} from {}: player {:016X} {} -> {}", in.sequence, Ip(in.ip), xuid,
                  OptionsText(values),
                  client >= 0 ? fmt::format("applied to server client {}", client)
                              : std::string("kept until that player is among the server's"));
    }
    answer.U64(xuid);
    answer.U8(client >= 0 ? uint8_t(client) : kNoClient);
    if (answered < kMaxPlayers) peer->xuids[answered] = xuid;
    ++answered;
  }
  if (!answered) return;
  peer->count = answered;
  answer.bytes[6] = uint8_t(answered);
  const int sent = Send(in.ip, in.port, answer);
  if (Trace() || sent < 0) {
    REXLOG_INFO("[cheat sync] answered options {} to {}:{} for {} player(s){}", in.sequence, Ip(in.ip), in.port,
                answered, sent < 0 ? " -- the send failed" : "");
  }
  // What the joiner does not have yet.
  if (!catalog.empty() && Get32(in.body) != catalog_revision) SendCatalog(*peer, catalog, catalog_revision);
  if (Get32(in.body + 4) != g_world_revision) SendWorld(*peer);
}

void HostCommand(const uint8_t* base, const Received& in) {
  if (in.count != 1 || in.aux < 1 || in.aux >= std::size(kCommands) || in.body_size < 10 ||
      in.body_size != 10 + size_t(in.body[9]) || in.body[9] > kMaxArgument || in.body[8] > 1) {
    REXLOG_WARN("[cheat sync] ignored a malformed command from {}", Ip(in.ip));
    return;
  }
  const uint64_t xuid = Get64(in.body);
  const bool pap = in.body[8] == 1;
  const std::string_view argument(reinterpret_cast<const char*>(in.body + 10), in.body[9]);
  const std::string_view name = kCommands[in.aux];
  uint8_t status = 1;
  std::string why;
  const auto previous = std::find_if(g_done.begin(), g_done.end(), [&](const Done& done) {
    return done.ip == in.ip && done.xuid == xuid && done.sequence == in.sequence;
  });
  if (previous != g_done.end()) {
    status = previous->status;  // a resend: the same answer, nothing runs again
    why = previous->why;
  } else {
    if (!PlainText(argument, kMaxArgument)) {
      why = "the argument has unexpected characters";
    } else if (OwnXuid(xuid)) {
      why = "that is one of the host's own players";
    } else {
      status = uint8_t(bo2zm::cheats::RunRemoteCommand(base, xuid, name, argument, pap, why) == 0 ? 0 : 1);
    }
    g_done[g_done_next++ % g_done.size()] = {in.ip, xuid, in.sequence, status, why};
    REXLOG_INFO("[cheat sync] received command {} from {}: player {:016X} {} {} -> {}{}{}", in.sequence, Ip(in.ip),
                xuid, name, argument, status == 0 ? "run" : "refused", why.empty() ? "" : ": ", why);
  }
  PeerAt(in.ip, in.port, true);
  Message result(kResult, 0, status, in.sequence);
  result.U64(xuid);
  const std::string text = why.substr(0, 200);
  result.U8(uint8_t(text.size()));
  result.Text(text);
  Send(in.ip, in.port, result);
}

void HostPush() {
  // The world settings and the catalog, to every joiner when they change (twice, a second apart: no answers come).
  const std::string world = bo2zm::cheats::WorldStateText();
  const auto now = Clock::now();
  bool push_world = false;
  if (world != g_world_text) {
    g_world_text = world;
    ++g_world_revision;
    push_world = true;
    g_world_again = now + std::chrono::seconds(1);
  } else if (g_world_again != Clock::time_point{} && now >= g_world_again) {
    g_world_again = {};
    for (auto& peer : g_peers) peer.world_sent = 0;
    push_world = true;
  }
  std::string catalog;
  uint32_t catalog_revision = 0;
  const bool has_catalog = bo2zm::cheats::CatalogText(catalog, catalog_revision);
  bool push_catalog = false;
  if (has_catalog && catalog_revision != g_catalog_pushed) {
    g_catalog_pushed = catalog_revision;
    push_catalog = true;
    g_catalog_again = now + std::chrono::seconds(1);
  } else if (has_catalog && g_catalog_again != Clock::time_point{} && now >= g_catalog_again) {
    g_catalog_again = {};
    for (auto& peer : g_peers) peer.catalog_sent = 0;
    push_catalog = true;
  }
  for (auto& peer : g_peers) {
    if (!peer.ip) continue;
    if (push_world && peer.world_sent != g_world_revision) SendWorld(peer);
    if (push_catalog && peer.catalog_sent != catalog_revision) SendCatalog(peer, catalog, catalog_revision);
  }

  // Remote players' explosive bullet hits, to the consoles they play on.
  bo2zm::cheats::RemoteBlastHit hits[16];
  const int count = bo2zm::cheats::TakeRemoteBlasts(hits, 16);
  for (int h = 0; h < count;) {
    const uint64_t xuid = hits[h].xuid;
    Peer* to = nullptr;
    for (auto& peer : g_peers) {
      for (int p = 0; peer.ip && p < peer.count; ++p) {
        if (peer.xuids[p] == xuid) to = &peer;
      }
    }
    int n = 0;
    Message message(kBlasts, 0, 0, 0);
    message.U64(xuid);
    for (; h < count && hits[h].xuid == xuid && n < kMaxBlasts; ++h, ++n) {
      for (const float v : hits[h].at) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        message.U32(bits);
      }
    }
    message.bytes[6] = uint8_t(n);
    if (to) Send(to->ip, to->port, message);
  }
}

void Host(const uint8_t* base) {
  static int frames = 0;
  static bool polling = false;
  uint32_t peer = 0;
  int64_t age = 0;
  const bool talking = link::LastPeer(kGamePort, &peer, &age) && age < kPeerFresh.count();
  if (!talking) {
    if (polling) {
      polling = false;
      g_peers = {};
      MenuHostStop();
    }
    return;
  }
  if (++frames < kHostPollEvery) return;
  frames = 0;
  if (Socket() < 0) return;
  std::array<uint8_t, 2048> buffer{};
  Received in;
  if (!polling) {
    // Whatever waited while this console was not listening belongs to an earlier match: dropped.
    polling = true;
    int dropped = 0;
    while (Receive(buffer, in)) ++dropped;
    REXLOG_INFO("[cheat sync] hosting a match with another console ({}): listening for its players' cheats{}", Ip(peer),
                dropped ? fmt::format(" ({} old message(s) dropped)", dropped) : std::string());
    return;
  }
  while (Receive(buffer, in)) {
    if (in.type == kOptions) HostOptions(base, in);
    else if (in.type == kCommand) HostCommand(base, in);
    else if (in.type == kMenuAsk) MenuAsked(in);
    // Answers, results and pushes are a host's: another match's.
  }
  HostPush();
  MenuPush();
}

// Multiplayer, or Zombies with the cheat sync off: only the mod menus' tables. The host is the console whose server
// runs while another console talks to its game port; a console in a match without its own server joined it.
void MenusOnly(const uint8_t* base) {
  static int frames = 0;
  static bool polling = false;
  const bool hosting = bo2zm::cheats::LocalServer(base);
  const bool joined = !hosting && bo2zm::preset::ClientInGame();
  if (!joined) MenuLeave();
  if (hosting) {
    uint32_t peer = 0;
    int64_t age = 0;
    if (!(link::LastPeer(kGamePort, &peer, &age) && age < kPeerFresh.count())) {
      if (polling) {
        polling = false;
        MenuHostStop();
      }
      return;
    }
    if (++frames < kHostPollEvery) return;
    frames = 0;
    if (Socket() < 0) return;
    std::array<uint8_t, 2048> buffer{};
    Received in;
    if (!polling) {
      polling = true;  // whatever waited belongs to an earlier match
      while (Receive(buffer, in)) {}
      return;
    }
    while (Receive(buffer, in)) {
      if (in.type == kMenuAsk) MenuAsked(in);
    }
    MenuPush();
    return;
  }
  if (polling) {
    polling = false;
    MenuHostStop();
  }
  if (!joined || !REXCVAR_GET(bo2zm_mod_hud_fit)) return;
  if (++frames >= kJoinPollEvery) {
    frames = 0;
    std::array<uint8_t, 2048> buffer{};
    Received in;
    while (Receive(buffer, in)) {
      if (in.type == kMenus) MenuReceived(in);
    }
  }
  MenuAsk();
}

}  // namespace

bool bo2zm::cheat_sync::Forward(int controller, std::string_view name, std::string_view argument, bool pack_a_punch) {
  const auto it = std::find(std::begin(kCommands) + 1, std::end(kCommands), name);
  if (it == std::end(kCommands) || !PlainText(argument, kMaxArgument)) {
    REXLOG_WARN("[cheat sync] {} {}: not a command the host takes", name, argument);
    return false;
  }
  const uint64_t xuid = ControllerXuid(controller);
  if (!xuid) return false;
  Command command;
  command.xuid = xuid;
  command.id = uint8_t(it - std::begin(kCommands));
  command.pap = pack_a_punch;
  command.argument = std::string(argument);
  std::lock_guard lock(g_forward_mutex);
  if (g_forwarded.size() >= 8) return false;
  g_forwarded.push_back(std::move(command));
  return true;
}

void bo2zm::cheat_sync::TraceFrame(bool map_loading, bool local_actor) {
  if (!Trace()) return;
  static Clock::time_point next{};
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(10);
  uint32_t peer = 0;
  int64_t age = -1;
  const bool heard = link::LastPeer(kGamePort, &peer, &age);
  REXLOG_INFO("[cheat sync] frame: map loading {}, a local player on this server {}, the client in a match {}, "
              "game port last heard {} ({} ms ago)", map_loading, local_actor, bo2zm::preset::ClientInGame(),
              heard ? Ip(peer) : std::string("never"), age);
}

void bo2zm::cheat_sync::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!base) return;
  if (!REXCVAR_GET(bo2zm_cheat_sync) || !bo2zm::mode::IsZombies()) {
    MenusOnly(base);
    return;
  }
  static int state = 0;  // 0 neither, 1 hosting, 2 joined
  const bool hosting = bo2zm::cheats::LocalServer(base);
  const bool joined = !hosting && bo2zm::cheats::JoinedRemoteGame();
  const int now = hosting ? 1 : joined ? 2 : 0;
  if (now != state) {
    state = now;
    if (Trace() || now == 2) {
      REXLOG_INFO("[cheat sync] this console {}", now == 1 ? "runs the server" : now == 2 ? "joined another's match"
                                                                                  : "is in no match");
    }
  }
  if (hosting) Host(base);
  if (joined) {
    Join(ctx, base);
  } else {
    MenuLeave();
  }
  if (!joined && g_join.active) {
    REXLOG_INFO("[cheat sync] out of the joined match: the host's world settings go back to the game's");
    bo2zm::cheats::ApplyWorldState({});
    g_join = {};
    std::lock_guard lock(g_forward_mutex);
    g_forwarded.clear();
  }
}
