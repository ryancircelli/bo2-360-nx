// See bo2zm_online.h.
#include "bo2zm_online.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/kernel/xam/text_input.h>
#include <rex/logging.h>
#include <rex/system/join_code.h>
#include <rex/system/online_link.h>

#include "bo2zm_autoexec.h"

namespace bo2zm::online {
namespace {

namespace net = rex::system::online;
namespace code = rex::system::join_code;

// bo2zm_online_ui (menus.lua reads the same numbers).
enum MenuState : int { kNothing = 0, kPreparing = 1, kHostReady = 2, kJoinReady = 3, kFailed = 4 };

std::mutex g_mutex;
std::string g_code_error;           // a code that could not be read, until the next action; under g_mutex
std::atomic<uint64_t> g_local{1};   // changes with g_code_error

std::string Ip(uint32_t ip) {
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
  return text;
}

std::string Ports() {
  const int first = net::FirstPort();
  return std::to_string(first) + "-" + std::to_string(first + net::PortCount() - 1);
}

// Text for a game dvar in "set name \"text\"": no quotes, separators or line ends.
std::string Clean(std::string text) {
  std::replace_if(text.begin(), text.end(),
                  [](char c) { return c == '"' || c == ';' || c == '\n' || c == '\r' || c == '\\'; }, ' ');
  return text;
}

void SetCodeError(std::string text) {
  std::lock_guard lock(g_mutex);
  g_code_error = std::move(text);
  g_local.fetch_add(1, std::memory_order_relaxed);
}

// What the router did, in one line.
std::string RouterLine(const net::Report& r, bool hosting) {
  const std::string ports = "UDP " + Ports();
  if (r.port_count && r.ports_mapped == r.port_count) {
    return "The router forwards " + ports + " to this console (UPnP).";
  }
  std::string why;
  if (!r.router_found) {
    why = "No router answered with UPnP";
  } else if (r.upnp_error == 718) {
    why = "The router already forwards these ports to " + (r.conflict_ip ? Ip(r.conflict_ip) : "another device");
  } else {
    why = "The router refused to forward the ports (UPnP error " + std::to_string(r.upnp_error) + ")";
  }
  if (!hosting) return why + ": joining usually works anyway.";
  return why + ": forward " + ports + " to " + Ip(r.lan_ip) + " on the router.";
}

// The menu's checklist, one step per line, kept on screen as each step finishes.
// UPnP: whether the router took the forwarding. Some routers accept it and still drop what comes in; the relay covers
// those.
std::string UpnpStep(const net::Report& r, bool hosting) {
  std::string step;
  if (r.port_count && r.ports_mapped == r.port_count) {
    step = "UPnP: passed (the router accepted UDP " + Ports() + ")";
  } else if (!r.router_found) {
    step = "UPnP: failed (no router answered)";
  } else if (r.upnp_error == 718) {
    step = "UPnP: failed (" + (r.conflict_ip ? Ip(r.conflict_ip) : std::string("another device")) + " has the ports)";
  } else {
    step = "UPnP: failed (the router refused, error " + std::to_string(r.upnp_error) + ")";
  }
  if (!hosting && step.rfind("UPnP: failed", 0) == 0) step += ", not needed to join";
  return step;
}

// A broker's name without the usual MQTT port.
std::string RelayName(const std::string& name) {
  constexpr std::string_view kPort = ":1883";
  if (name.size() > kPort.size() && name.compare(name.size() - kPort.size(), kPort.size(), kPort) == 0) {
    return name.substr(0, name.size() - kPort.size());
  }
  return name;
}

// The relay (hole punching through public MQTT brokers, rex/system/online_relay.h): each server's state, then what
// the first working one is doing. All of them carry the same messages at once; the menu follows the first that works.
void RelaySteps(const net::Report& r, bool hosting, std::vector<std::string>& lines) {
  const auto& relay = r.relay;
  if (!relay.active) {
    lines.push_back(RouterLine(r, hosting));
    return;
  }
  int using_index = -1;
  const int count = std::min<int>(relay.brokers, int(relay.broker_list.size()));
  for (int i = 0; i < count; ++i) {
    const auto& b = relay.broker_list[size_t(i)];
    const char* state = b.state == net::relay::BrokerStatus::kWorking ? "working"
                        : b.state == net::relay::BrokerStatus::kDown  ? "down"
                                                                      : "checking...";
    lines.push_back("Relay " + std::to_string(i + 1) + " (" + RelayName(b.name) + "): " + state);
    if (using_index < 0 && b.state == net::relay::BrokerStatus::kWorking) using_index = i;
  }
  if (using_index < 0) {
    lines.push_back(count && relay.brokers_failed >= count
                        ? "No relay is working: the host's ports must be open (UPnP or a port forward)."
                        : "Waiting for a relay to answer...");
    return;
  }
  const auto& use = relay.broker_list[size_t(using_index)];
  std::string doing;
  if (hosting) {
    if (relay.peers == 0) {
      doing = use.posted ? "code posted, waiting for players" : "posting the code...";
    } else {
      doing = (relay.peers == 1 ? std::string("1 player") : std::to_string(relay.peers) + " players") + " found, " +
              (relay.peers_heard == relay.peers ? std::string("connected")
               : relay.peers_heard ? std::to_string(relay.peers_heard) + " connected"
                                   : std::string("opening the connection..."));
    }
  } else if (relay.peers == 0) {
    doing = use.posted ? "looking for the host..." : "posting this console's address...";
  } else {
    doing = relay.peers_heard ? "host found, connected" : "host found, opening the connection...";
  }
  lines.push_back("Using relay " + std::to_string(using_index + 1) + ": " + doing);
  if (relay.symmetric) lines.push_back("Warning: this network changes ports per destination; the relay may not get through.");
}

std::string Join(const std::vector<std::string>& lines) {
  std::string text;
  for (const auto& line : lines) text += (text.empty() ? "" : "|") + line;
  return text;
}

constexpr size_t kMenuSteps = 12;  // lines under the title (menus.lua shows as many)

struct Menu {
  int state = kNothing;
  std::string text;  // title|line|line..
  std::string code;
};

Menu Describe(const net::Report& r, const std::string& code_error) {
  Menu menu;
  if (!code_error.empty()) {
    menu.state = kFailed;
    menu.text = "That join code does not work|" + code_error + "|Type it exactly as the host's screen shows it.";
    return menu;
  }
  const bool hosting = r.role == net::Role::kHost;
  const std::string target_code = r.target_ip ? code::Make(r.target_ip) : std::string();
  switch (r.state) {
    case net::State::kIdle:
      menu.text = "Play over the internet|HOST ONLINE GAME opens the router's ports and gives you a join code to "
                  "share.|JOIN WITH CODE joins a friend's game with the code from their screen.";
      break;
    case net::State::kPreparing:
      menu.state = kPreparing;
      menu.text = Join({hosting ? "Preparing an online game..." : "Getting ready to join " + target_code + "...",
                        "UPnP: checking the router...", "Finding this console's internet address...",
                        "This can take up to half a minute. B cancels."});
      break;
    case net::State::kReady: {
      std::vector<std::string> lines;
      if (hosting) {
        menu.state = kHostReady;
        menu.code = code::Make(r.public_ip);
        lines.push_back("Your join code: " + menu.code);
      } else {
        menu.state = kJoinReady;
        menu.code = target_code;
        lines.push_back("Joining " + target_code);
      }
      lines.push_back(UpnpStep(r, hosting));
      RelaySteps(r, hosting, lines);
      if (r.double_nat) lines.push_back("Warning: another router or the provider's NAT sits in front of this one.");
      lines.push_back(hosting ? "Give the code to the other players. CONTINUE TO LOBBY opens your game."
                              : "FIND GAME looks for the host's game; the host must be in its lobby.");
      menu.text = Join(lines);
      break;
    }
    case net::State::kFailed:
      menu.state = kFailed;
      switch (r.failure) {
        case net::Failure::kNoNetwork:
          menu.text = "Online play is not available|This console is not connected to a network.";
          break;
        case net::Failure::kSameNetwork:
          menu.text = "That code is from your own network|Both consoles are behind the same router: play with "
                      "SYSTEM LINK instead.";
          break;
        default:
          menu.text = "Online play is not available|This console's internet address could not be found: no STUN "
                      "server answered, and the router did not give it.|Check the internet connection and try "
                      "again.";
          break;
      }
      break;
  }
  return menu;
}

}  // namespace

bool RunCommand(std::string_view name, std::string_view argument) {
  if (name != "bo2zm_online") return false;
  if (argument == "host") {
    SetCodeError({});
    net::Start(net::Role::kHost, 0);
  } else if (argument == "join") {
    const bool opened = rex::kernel::xam::ShowTextInput(
        "Join code (XXXX-XXXX)", "", 12, [](std::optional<std::string> text) {
          if (!text) {
            REXLOG_INFO("[online] join code entry cancelled");
            return;
          }
          uint32_t ip = 0;
          const code::Result result = code::Read(*text, &ip);
          if (result != code::Result::kOk) {
            REXLOG_INFO("[online] join code \"{}\" not read: {}", Clean(*text), code::Describe(result));
            SetCodeError(code::Describe(result));
            return;
          }
          REXLOG_INFO("[online] join code {} decoded: the host is {}", code::Make(ip), Ip(ip));
          SetCodeError({});
          net::Start(net::Role::kJoin, ip);
        });
    if (!opened) REXLOG_INFO("[online] a text input is already open");
  } else if (argument == "cancel" || argument == "off") {
    SetCodeError({});
    net::Stop();
  } else {
    REXLOG_WARN("[online] unknown bo2zm_online argument: {}", argument);
  }
  return true;
}

void EveryFrame(PPCContext& ctx, uint8_t* base) {
  static uint64_t shown_net = 0, shown_local = 0;
  const uint64_t net_version = net::Version();
  const uint64_t local_version = g_local.load(std::memory_order_relaxed);
  if (net_version == shown_net && local_version == shown_local) [[likely]] {
    return;
  }
  shown_net = net_version;
  shown_local = local_version;
  std::string code_error;
  {
    std::lock_guard lock(g_mutex);
    code_error = g_code_error;
  }
  const net::Report report = net::Current();
  const Menu menu = Describe(report, code_error);
  static std::string logged_code;
  if (menu.state == kHostReady && menu.code != logged_code) {
    logged_code = menu.code;
    REXLOG_INFO("[online] join code made: {} ({})", menu.code, Ip(report.public_ip));
  }
  const std::string help = "Can't connect? If UPnP and the relay both fail, the host can forward UDP ports " + Ports() +
                           " to their console.";
  // A dvar's string holds about 255 characters, less than the whole checklist: the title goes in
  // bo2zm_online_text_ui and each line in a dvar of its own (bo2zm_online_step1_ui ..), empty past the last.
  std::vector<std::string> parts;
  for (size_t at = 0; at <= menu.text.size();) {
    size_t end = menu.text.find('|', at);
    if (end == std::string::npos) end = menu.text.size();
    parts.push_back(menu.text.substr(at, end - at));
    at = end + 1;
  }
  // The steps first, so the menu never pairs a new state with old lines; one command each, as ExecuteCommands
  // takes at most 2000 bytes.
  for (size_t i = 1; i <= kMenuSteps; ++i) {
    bo2zm::autoexec::ExecuteCommands(ctx, base,
                                     "set bo2zm_online_step" + std::to_string(i) + "_ui \"" +
                                         Clean(i < parts.size() ? parts[i].substr(0, 200) : std::string()) + "\"");
  }
  bo2zm::autoexec::ExecuteCommands(ctx, base,
                                   "set bo2zm_online_ui " + std::to_string(menu.state) + "; set bo2zm_online_text_ui \"" +
                                       Clean(parts.empty() ? std::string() : parts[0]) + "\"; set bo2zm_online_code_ui \"" +
                                       Clean(menu.code) + "\"; set bo2zm_online_help_ui \"" + Clean(help) + "\"");
}

void Stop() { net::Shutdown(); }

}  // namespace bo2zm::online
