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
#include "bo2_cheat_sync_pkg/cheat_sync_logic.h"
#include "bo2_cheat_sync_pkg/cheat_sync_wire.h"
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

namespace cs = bo2cs;  // clean-room wire codec and decision logic (metal360 packages/bo2-cheat-sync, vendored)

constexpr size_t kMaxArgument = cs::kMaxArgument;
constexpr int kMaxPlayers = 4;
constexpr uint8_t kNoClient = 255;
constexpr uint16_t kGamePort = 1000;  // BO2's game socket (the server's and the client's)
constexpr uint16_t kSidePort = cs::kGuestSidePort;  // ours, moved like the game's (link::HostPort)
constexpr auto kPeerFresh = std::chrono::milliseconds(cs::kPeerLiveMs);  // a game peer heard from this recently is live
static_assert(detail::kSyncedCount == int(cs::kOptionCount), "synced options and the wire's option count differ");

// The commands a joiner sends, by number (the message's aux byte).
constexpr std::string_view kCommands[] = {
    "", "bo2zm_perk", "bo2zm_remove_perks", "bo2zm_give_weapon", "bo2zm_pap_weapon", "bo2zm_max_points",
    "bo2zm_powerup", "bo2zm_kill_zombies", "bo2zm_doors", "bo2zm_power", "bo2zm_debris", "bo2zm_goto_round",
    "bo2zm_zm_nospawn", "bo2zm_hold", "bo2zm_release"};
static_assert(std::size(kCommands) == 15, "the wire's command ids are 1..14");

bool Trace() { return REXCVAR_GET(bo2zm_cheat_sync_trace); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string Ip(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// A command argument or result text: what the menu itself sends (names, numbers, a dvar and its value).
bool PlainText(std::string_view text, size_t most) { return cs::ValidCommandText(std::string(text), most); }

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

int Send(uint32_t ip, uint16_t port, const cs::Bytes& message) {
  const int64_t socket = Socket();
  return socket < 0 ? -1 : link::SideSend(socket, ip, port, message.data(), uint32_t(message.size()));
}
int Send(uint32_t ip, uint16_t port, const std::optional<cs::Bytes>& message) {
  return message ? Send(ip, port, *message) : -1;
}

// A received datagram whose magic and version are ours: the whole datagram (each message type is checked by its own
// decoder in bo2cs), where it came from, and its type.
struct Received {
  uint8_t type = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t ip = 0;
  uint16_t port = 0;
};

bool Receive(std::array<uint8_t, cs::kReceiveBuffer>& buffer, Received& out) {
  const int64_t socket = Socket();
  if (socket < 0) return false;
  for (;;) {
    const int got = link::SideReceive(socket, buffer.data(), uint32_t(buffer.size()), &out.ip, &out.port);
    if (got < 0) return false;
    const size_t size = size_t(got);
    if (size < cs::kHeaderSize || std::memcmp(buffer.data(), "BZCS", 4) != 0) {
      if (Trace()) REXLOG_INFO("[cheat sync] ignored a {}-byte datagram from {}:{} (not ours)", size, Ip(out.ip), out.port);
      continue;
    }
    if (buffer[4] != cs::kVersion) {
      static int logged = 0;
      if (logged++ < 3) {
        REXLOG_WARN("[cheat sync] {} sends version {} messages (this build reads {}); ignored", Ip(out.ip), buffer[4],
                    cs::kVersion);
      }
      continue;
    }
    out.type = buffer[5];
    out.data = buffer.data();
    out.size = size;
    return true;
  }
}

// ---- Mod menus (bo2zm_mod_hud_wire.h), both sides; Zombies and Multiplayer ----

namespace wire = bo2zm::mod_hud_wire;
constexpr auto kMenuAskEvery = std::chrono::milliseconds(2000);
constexpr auto kMenuEvery = std::chrono::milliseconds(1000);
constexpr size_t kMaxTables = 32;

// The host: the consoles that asked for the menus (cs::MenuRequesters keeps up to eight for 6 s after their latest
// ask), and what each player's table was when last sent.
cs::MenuRequesters g_menu_peers;
struct MenuSent {
  bool used = false;
  uint16_t client = 0;
  uint32_t revision = 0;
  Clock::time_point at{};
  int empty_sends = 0;
};
std::array<MenuSent, kMaxTables> g_menu_sent;
size_t g_menu_peer_count = 0;

void MenuAsked(const Received& in) {
  if (!cs::DecodeMenuAsk(in.data, in.size)) return;  // another format: that console cannot read these tables
  const int64_t now = NowMs();
  bool known = false;
  for (const auto& peer : g_menu_peers.Active(now)) known |= peer.first == in.ip && peer.second == in.port;
  if (!known) {
    REXLOG_INFO("[cheat sync] mod menus: {}:{} fits the host's menus; sending it the players' tables", Ip(in.ip),
                in.port);
  }
  g_menu_peers.Note(in.ip, in.port, now);
}

// Each player's table to every console that asked recently: when it changes, every second while it has entries, and
// an emptied one once more a second later.
void MenuPush() {
  const auto now = Clock::now();
  const auto peers = g_menu_peers.Active(NowMs());
  if (peers.size() < g_menu_peer_count) REXLOG_INFO("[cheat sync] mod menus: a console stopped asking");
  g_menu_peer_count = peers.size();
  if (peers.empty()) return;
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
    cs::Menus menus;
    menus.revision = table.revision;
    menus.session = table.session;
    menus.epoch = table.epoch;
    menus.client = table.client;
    for (uint8_t e = 0; e < table.count; ++e) {
      cs::MenuEntry entry;
      entry.slot = table.entries[e].slot;
      std::memcpy(entry.identity, table.entries[e].id.data(), cs::kIdentitySize);
      menus.entries.push_back(entry);
    }
    const auto message = cs::EncodeMenus(menus);
    if (!message) continue;
    for (const auto& peer : peers) Send(peer.first, peer.second, message);
    sent->empty_sends = table.count ? 0 : changed ? 1 : sent->empty_sends + 1;
    sent->revision = table.revision;
    sent->at = now;
    if (Trace()) {
      REXLOG_INFO("[cheat sync] mod menus: player {} table {} ({} elements) to {} console(s)", table.client,
                  table.revision, table.count, peers.size());
    }
  }
}

void MenuHostStop() {
  g_menu_peers.Clear();
  g_menu_peer_count = 0;
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
  const int sent = Send(to, link::HostPort(kSidePort), cs::EncodeMenuAsk());
  if (Trace() || sent < 0) {
    REXLOG_INFO("[cheat sync] mod menus: asked {}{}{}", Ip(to), broadcast ? " (the LAN's broadcast)" : "",
                sent < 0 ? " -- the send failed" : "");
  }
}

void MenuReceived(const Received& in) {
  if (!REXCVAR_GET(bo2zm_mod_hud_fit)) return;
  wire::Table table;
  bool ok = false;
  if (const auto menus = cs::DecodeMenus(in.data, in.size)) {
    table.session = menus->session;
    table.epoch = menus->epoch;
    table.client = menus->client;
    table.revision = menus->revision;
    table.count = uint8_t(menus->entries.size());
    ok = true;
    for (size_t e = 0; e < menus->entries.size() && ok; ++e) {
      table.entries[e].slot = menus->entries[e].slot;
      std::memcpy(table.entries[e].id.data(), menus->entries[e].identity, cs::kIdentitySize);
      // The fitting takes only text and material elements.
      ok = cs::IdentityFittable(menus->entries[e].identity);
    }
  }
  if (!ok) {
    static int logged = 0;
    if (logged++ < 3) {
      REXLOG_WARN("[cheat sync] mod menus: ignored a table from {} ({} bytes)", Ip(in.ip), in.size);
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
  std::optional<cs::RetryTimer> retry;  // from the first send; resend at 1 s and 3 s, give up at 4 s
  Clock::time_point sent_at{};
};

struct Joiner {
  bool active = false;
  uint32_t sequence = 0;
  int count = 0;
  std::array<uint64_t, kMaxPlayers> xuids{};
  std::array<std::array<int, detail::kSyncedCount>, kMaxPlayers> values{};
  std::array<int, kMaxPlayers> clients{};  // as the host answered, -1 not yet
  bool answered = false;  // the host answered the latest message for every player, with this console's revisions
  std::optional<cs::RetryTimer> retry;  // the latest message's schedule, from its first send
  bool sent_broadcast = false;
  bool options_host_resent = false;  // the message already went again once the host's address was known
  uint32_t host_ip = 0;  // from the host's messages
  uint32_t catalog_revision = 0, world_revision = 0;  // what this console has from the host
  cs::CatalogAssembler catalog_assembler;
  uint32_t command_sequence = 0;
  std::vector<Command> commands;  // waiting for their result
};
Joiner g_join;
std::mutex g_forward_mutex;
std::vector<Command> g_forwarded;  // from Forward (any thread), sent by the next frame; under g_forward_mutex

// The peer the game socket last heard on BO2's game port (0 when none is live), for cs::DestinationInputs.
std::optional<uint32_t> GamePeer(int64_t& age_ms) {
  uint32_t ip = 0;
  int64_t age = 0;
  if (!link::LastPeer(kGamePort, &ip, &age)) return std::nullopt;
  age_ms = age;
  return ip;
}

// The host's address, 0 when not known yet.
uint32_t HostAddress() {
  if (g_join.host_ip) return g_join.host_ip;
  int64_t age = 0;
  const auto peer = GamePeer(age);
  return peer && age < kPeerFresh.count() ? *peer : 0;
}

// Where a joiner's message goes (cs::ChooseDestination): the host, or the code's address when joined online, or the
// LAN's broadcast address. 0: nowhere (no LAN). `broadcast` asks for the final fallback, which skips the host.
uint32_t Destination(bool broadcast, bool& sent_broadcast) {
  cs::DestinationInputs in;
  if (g_join.host_ip) in.learned_host = g_join.host_ip;
  int64_t age = 0;
  if (const auto peer = GamePeer(age)) {
    in.peer_ip = *peer;
    in.peer_age_ms = age;
  }
  in.online_target = rex::system::online::Target();
  uint32_t lan_ip = 0, lan_mask = 0;
  if (link::LanAddress(&lan_ip, &lan_mask) && lan_mask) {
    in.have_lan = true;
    in.lan_ip = lan_ip;
    in.lan_mask = lan_mask;
  }
  in.broadcast_fallback = broadcast;
  const auto to = cs::ChooseDestination(in);
  if (!to) {
    REXLOG_WARN("[cheat sync] no LAN address: nothing is sent");
    return 0;
  }
  const bool host_known = !broadcast && (in.learned_host || (in.peer_ip && age >= 0 && age < cs::kPeerLiveMs));
  sent_broadcast = !host_known && !in.online_target;
  return *to;
}

void SendOptions(const char* why, bool broadcast = false) {
  if (g_join.count == 0) return;
  cs::Options options;
  options.sequence = g_join.sequence;
  options.catalog_rev = g_join.catalog_revision;
  options.world_rev = g_join.world_revision;
  for (int p = 0; p < g_join.count; ++p) {
    cs::PlayerOptions player;
    player.xuid = g_join.xuids[p];
    for (int i = 0; i < detail::kSyncedCount; ++i) player.value[i] = uint8_t(g_join.values[p][i]);
    options.players.push_back(player);
  }
  const auto message = cs::EncodeOptions(options);
  if (!message) {
    REXLOG_WARN("[cheat sync] options message {} is not valid; not sent", g_join.sequence);
    return;
  }
  const uint32_t to = Destination(broadcast, g_join.sent_broadcast);
  if (!to) return;
  const uint16_t port = link::HostPort(kSidePort);
  const int sent = Send(to, port, message);
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
  g_join.clients.fill(-1);
  g_join.options_host_resent = false;
  g_join.retry.emplace(NowMs());
  SendOptions(why);
}

void SendCommand(Command& command, const char* why, bool broadcast = false) {
  cs::Command wire_command;
  wire_command.sequence = command.sequence;
  wire_command.id = command.id;
  wire_command.xuid = command.xuid;
  wire_command.pack_a_punch = command.pap;
  wire_command.argument = command.argument;
  const auto message = cs::EncodeCommand(wire_command);
  if (!message) {
    REXLOG_WARN("[cheat sync] command {} {} is not valid; not sent", kCommands[command.id], command.argument);
    return;
  }
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
  if (in.type == cs::kAnswer) {
    const auto answer = cs::DecodeAnswer(in.data, in.size);
    if (!answer) {
      REXLOG_WARN("[cheat sync] ignored a malformed answer from {}", Ip(in.ip));
      return;
    }
    if (answer->sequence != g_join.sequence) {
      if (Trace()) REXLOG_INFO("[cheat sync] answer from {} to an older message ({}); ignored", Ip(in.ip), answer->sequence);
      return;
    }
    const uint32_t catalog = answer->catalog_rev, world = answer->world_rev;
    for (const auto& entry : answer->entries) {
      for (int p = 0; p < g_join.count; ++p) {
        if (g_join.xuids[p] == entry.xuid) g_join.clients[p] = entry.client == kNoClient ? -1 : entry.client;
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
  if (in.type == cs::kResult) {
    const auto result = cs::DecodeResult(in.data, in.size);
    if (!result) {
      REXLOG_WARN("[cheat sync] ignored a malformed result from {}", Ip(in.ip));
      return;
    }
    const auto it = std::find_if(g_join.commands.begin(), g_join.commands.end(),
                                 [&](const Command& command) { return command.sequence == result->sequence; });
    if (it == g_join.commands.end()) return;  // a repeated answer
    REXLOG_INFO("[cheat sync] the host {} {} {} for player {:016X}{}{}", result->refused ? "refused" : "ran",
                kCommands[it->id], it->argument, it->xuid, result->text.empty() ? "" : ": ", result->text);
    g_join.commands.erase(it);
    return;
  }
  if (in.type == cs::kCatalog) {
    const auto part = cs::DecodeCatalogPart(in.data, in.size);
    if (!part) {
      REXLOG_WARN("[cheat sync] ignored a malformed catalog part from {}", Ip(in.ip));
      return;
    }
    if (part->revision == g_join.catalog_revision) return;  // have it
    if (const auto text = g_join.catalog_assembler.Add(*part)) {
      bo2zm::cheats::ApplyCatalog(ctx, base, *text);
      g_join.catalog_revision = part->revision;
      REXLOG_INFO("[cheat sync] the host's catalog {} received ({} bytes)", part->revision, text->size());
    }
    return;
  }
  if (in.type == cs::kWorld) {
    const auto world = cs::DecodeWorld(in.data, in.size);
    if (!world) {
      REXLOG_WARN("[cheat sync] ignored malformed world settings from {}", Ip(in.ip));
      return;
    }
    if (world->revision == g_join.world_revision) return;
    bo2zm::cheats::ApplyWorldState(world->text);
    g_join.world_revision = world->revision;
    REXLOG_INFO("[cheat sync] the host's world settings {}: {}", world->revision,
                world->text.empty() ? "the game's own" : world->text);
    return;
  }
  if (in.type == cs::kBlasts) {
    const auto blasts = cs::DecodeBlasts(in.data, in.size);
    if (!blasts) return;
    bool ours = false;
    for (int p = 0; p < g_join.count; ++p) ours |= g_join.xuids[p] == blasts->xuid;
    if (!ours) return;
    for (const auto& hit : blasts->hits) {
      float at[3] = {hit.x, hit.y, hit.z};
      bo2zm::cheats::PlayRemoteBlast(at);
    }
    if (Trace()) REXLOG_INFO("[cheat sync] {} explosive hit(s) from the host to draw", blasts->hits.size());
    return;
  }
  if (in.type == cs::kMenus) {
    MenuReceived(in);
    return;
  }
  if (Trace()) REXLOG_INFO("[cheat sync] ignored a type {} message from {}", in.type, Ip(in.ip));
}

void JoinerPoll(PPCContext& ctx, uint8_t* base) {
  std::array<uint8_t, cs::kReceiveBuffer> buffer{};
  Received in;
  while (Receive(buffer, in)) {
    if (in.type == cs::kOptions || in.type == cs::kCommand || in.type == cs::kMenuAsk) continue;  // another joiner's broadcast
    if (g_join.host_ip != in.ip) {
      REXLOG_INFO("[cheat sync] the host is {}", Ip(in.ip));
      g_join.host_ip = in.ip;
    }
    JoinerReceived(ctx, base, in);
  }
}

void Join(PPCContext& ctx, uint8_t* base) {
  static int frames = 0;
  if (++frames >= 2) {
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
    const bool again = g_join.sent_broadcast && g_join.retry && !g_join.retry->Finished() && HostAddress();
    if (again && !g_join.options_host_resent) {
      g_join.options_host_resent = true;
      SendOptions("the host is known now");  // the same message, now to the host itself
    } else if (g_join.retry) {
      switch (g_join.retry->Poll(NowMs())) {
        case cs::RetryAction::kResend: SendOptions("resend"); break;
        // The last try goes to the whole LAN, in case the address taken for the host was another player's.
        case cs::RetryAction::kResendFallback: SendOptions("resend", true); break;
        case cs::RetryAction::kGiveUp:
          REXLOG_WARN("[cheat sync] the host did not answer options message {} for every player; it is sent again "
                      "on the next change", g_join.sequence);
          break;
        case cs::RetryAction::kNone: break;
      }
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
    command.retry.emplace(NowMs());
    SendCommand(command, "picked");
    g_join.commands.push_back(std::move(command));
  }
  for (auto it = g_join.commands.begin(); it != g_join.commands.end();) {
    switch (it->retry->Poll(NowMs())) {
      case cs::RetryAction::kResend: SendCommand(*it, "resend"); break;
      case cs::RetryAction::kResendFallback: SendCommand(*it, "resend", true); break;
      case cs::RetryAction::kGiveUp:
        REXLOG_WARN("[cheat sync] the host did not answer {} {}; it was not run", kCommands[it->id], it->argument);
        it = g_join.commands.erase(it);
        continue;
      case cs::RetryAction::kNone: break;
    }
    ++it;
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
cs::OutcomeCache g_done;  // the latest 16 commands run, so a resend gets the same answer and runs nothing again
uint32_t g_world_revision = 1;
std::string g_world_text;
cs::DoublePush g_world_push;    // a changed world is pushed twice, a second apart (pushes are not answered)
uint32_t g_catalog_pushed = 0;
cs::DoublePush g_catalog_push;  // and so is a new catalog

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
  const auto parts = cs::SplitCatalog(revision, text);
  if (parts.empty()) {
    REXLOG_WARN("[cheat sync] the catalog ({} bytes) is too big to send", text.size());
    return;
  }
  for (const auto& part : parts) Send(peer.ip, peer.port, cs::EncodeCatalogPart(part));
  peer.catalog_sent = revision;
  REXLOG_INFO("[cheat sync] sent the catalog {} ({} bytes in {} part(s)) to {}", revision, text.size(), parts.size(),
              Ip(peer.ip));
}

void SendWorld(Peer& peer) {
  Send(peer.ip, peer.port, cs::EncodeWorld({g_world_revision, g_world_text}));
  peer.world_sent = g_world_revision;
  REXLOG_INFO("[cheat sync] sent the world settings {} to {}: {}", g_world_revision, Ip(peer.ip),
              g_world_text.empty() ? "the game's own" : g_world_text);
}

void HostOptions(const uint8_t* base, const Received& in) {
  const auto decoded = cs::DecodeOptions(in.data, in.size);
  if (!decoded) {
    REXLOG_WARN("[cheat sync] ignored a malformed options message from {}", Ip(in.ip));
    return;
  }
  const auto& options = decoded->options;
  std::string catalog;
  uint32_t catalog_revision = 0;
  bo2zm::cheats::CatalogText(catalog, catalog_revision);
  cs::Answer answer;
  answer.sequence = options.sequence;
  answer.catalog_rev = catalog_revision;
  answer.world_rev = g_world_revision;
  Peer* peer = PeerAt(in.ip, in.port, true);
  int answered = 0;
  for (size_t p = 0; p < options.players.size(); ++p) {
    const uint64_t xuid = options.players[p].xuid;
    if (!decoded->entry_valid[p]) {
      REXLOG_WARN("[cheat sync] ignored player {:016X} from {}: a value out of range", xuid, Ip(in.ip));
      continue;
    }
    int values[detail::kSyncedCount];
    for (int i = 0; i < detail::kSyncedCount; ++i) values[i] = options.players[p].value[i];
    if (OwnXuid(xuid)) {
      if (Trace()) REXLOG_INFO("[cheat sync] {:016X} from {} is one of this console's players; ignored", xuid, Ip(in.ip));
      continue;
    }
    int client = -1;
    bool changed = false;
    if (!bo2zm::cheats::SetRemotePlayer(base, xuid, options.sequence, values, &client, &changed)) {
      if (Trace()) REXLOG_INFO("[cheat sync] options {} for {:016X} from {} not taken", options.sequence, xuid, Ip(in.ip));
      continue;
    }
    if (changed || Trace()) {
      REXLOG_INFO("[cheat sync] received options {} from {}: player {:016X} {} -> {}", options.sequence, Ip(in.ip), xuid,
                  OptionsText(values),
                  client >= 0 ? fmt::format("applied to server client {}", client)
                              : std::string("kept until that player is among the server's"));
    }
    answer.entries.push_back({xuid, client >= 0 ? uint8_t(client) : kNoClient});
    if (answered < kMaxPlayers) peer->xuids[answered] = xuid;
    ++answered;
  }
  if (!answered) return;
  peer->count = answered;
  const int sent = Send(in.ip, in.port, cs::EncodeAnswer(answer));
  if (Trace() || sent < 0) {
    REXLOG_INFO("[cheat sync] answered options {} to {}:{} for {} player(s){}", options.sequence, Ip(in.ip), in.port,
                answered, sent < 0 ? " -- the send failed" : "");
  }
  // What the joiner does not have yet.
  if (!catalog.empty() && options.catalog_rev != catalog_revision) SendCatalog(*peer, catalog, catalog_revision);
  if (options.world_rev != g_world_revision) SendWorld(*peer);
}

void HostCommand(const uint8_t* base, const Received& in) {
  const auto decoded = cs::DecodeCommand(in.data, in.size);
  if (!decoded) {
    REXLOG_WARN("[cheat sync] ignored a malformed command from {}", Ip(in.ip));
    return;
  }
  const cs::Command& command = decoded->command;
  const uint64_t xuid = command.xuid;
  const std::string_view name = kCommands[command.id];
  bool refused = true;
  std::string why;
  if (const cs::Outcome* previous = g_done.Find(in.ip, xuid, command.sequence)) {
    refused = previous->refused;  // a resend: the same answer, nothing runs again
    why = previous->text;
  } else {
    if (!decoded->text_valid) {
      why = "the argument has unexpected characters";
    } else if (OwnXuid(xuid)) {
      why = "that is one of the host's own players";
    } else {
      refused = bo2zm::cheats::RunRemoteCommand(base, xuid, name, command.argument, command.pack_a_punch, why) != 0;
    }
    g_done.Remember({in.ip, xuid, command.sequence, refused, why});
    REXLOG_INFO("[cheat sync] received command {} from {}: player {:016X} {} {} -> {}{}{}", command.sequence,
                Ip(in.ip), xuid, name, command.argument, refused ? "refused" : "run", why.empty() ? "" : ": ", why);
  }
  PeerAt(in.ip, in.port, true);
  Send(in.ip, in.port, cs::EncodeResult({command.sequence, refused, xuid, why}));
}

void HostPush() {
  // The world settings and the catalog, to every joiner when they change (twice, a second apart: no answers come).
  const std::string world = bo2zm::cheats::WorldStateText();
  const int64_t now = NowMs();
  if (world != g_world_text) {
    g_world_text = world;
    ++g_world_revision;
    g_world_push.Changed(now);
  }
  std::string catalog;
  uint32_t catalog_revision = 0;
  const bool has_catalog = bo2zm::cheats::CatalogText(catalog, catalog_revision);
  if (has_catalog && catalog_revision != g_catalog_pushed) {
    g_catalog_pushed = catalog_revision;
    g_catalog_push.Changed(now);
  }
  const bool push_world = g_world_push.Poll(now);
  const bool push_catalog = has_catalog && g_catalog_push.Poll(now);
  for (auto& peer : g_peers) {
    if (!peer.ip) continue;
    if (push_world) SendWorld(peer);
    if (push_catalog) SendCatalog(peer, catalog, catalog_revision);
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
    cs::Blasts blasts;
    blasts.xuid = xuid;
    for (; h < count && hits[h].xuid == xuid && blasts.hits.size() < 8; ++h) {
      blasts.hits.push_back({hits[h].at[0], hits[h].at[1], hits[h].at[2]});
    }
    if (to) Send(to->ip, to->port, cs::EncodeBlasts(blasts));
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
  if (++frames < 4) return;
  frames = 0;
  if (Socket() < 0) return;
  std::array<uint8_t, cs::kReceiveBuffer> buffer{};
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
    else if (in.type == cs::kMenuAsk) MenuAsked(in);
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
    if (++frames < 4) return;
    frames = 0;
    if (Socket() < 0) return;
    std::array<uint8_t, cs::kReceiveBuffer> buffer{};
    Received in;
    if (!polling) {
      polling = true;  // whatever waited belongs to an earlier match
      while (Receive(buffer, in)) {}
      return;
    }
    while (Receive(buffer, in)) {
      if (in.type == cs::kMenuAsk) MenuAsked(in);
    }
    MenuPush();
    return;
  }
  if (polling) {
    polling = false;
    MenuHostStop();
  }
  if (!joined || !REXCVAR_GET(bo2zm_mod_hud_fit)) return;
  if (++frames >= 2) {
    frames = 0;
    std::array<uint8_t, cs::kReceiveBuffer> buffer{};
    Received in;
    while (Receive(buffer, in)) {
      if (in.type == cs::kMenus) MenuReceived(in);
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
