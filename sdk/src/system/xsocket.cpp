/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/online_link.h>
#include <rex/system/online_relay.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_INT32(net_system_port_offset, 30000, "Kernel",
                     "System link: a title's fixed port below 1024 is bound and addressed this much higher (the same "
                     "on every console), since the Switch refuses those ports. 0 = the title's own ports")
    .range(0, 64511);
REXCVAR_DECLARE(bool, net_online_trace);
REXCVAR_DEFINE_BOOL(net_system_link_trace, false, "Kernel",
                    "System link: log the first packets sent and received on a title's moved ports (below 1024)");

#if REX_PLATFORM_SWITCH
REXCVAR_DEFINE_INT32(switch_net_idle_poll_us, 1000, "Kernel",
                     "Switch: after recv/recvfrom on a socket finds nothing (EWOULDBLOCK), the same call on that "
                     "socket within this many microseconds answers EWOULDBLOCK again without asking the socket "
                     "service. 0 = always asks")
    .range(0, 100000);
#endif

namespace rex::system {

#if REX_PLATFORM_SWITCH
namespace {
// The guest's memory is mapped through code-memory aliases
// (guest_memory_switch.cpp), and Horizon's socket service can refuse IPC
// buffers there: a socket call failed with EFAULT on the console right before
// the game dropped to the menu with an error. Socket data goes through a host
// buffer instead: one per thread, kept between calls, so no call pays for a new
// zero-filled vector (a malloc under newlib's global heap lock).
uint8_t* HostBuffer(uint32_t len) {
  thread_local std::vector<uint8_t> buffer;
  if (buffer.size() < (len ? len : 1)) {
    buffer.resize(len ? len : 1);
  }
  return buffer.data();
}

/*
 * The idle window (switch_net_idle_poll_us). Each recv/recvfrom is an IPC to
 * the socket service. BO2's server thread polls its two sockets (VDP and UDP)
 * in its idle loop, and in a solo match nothing ever arrives: in console run
 * 015 that thread spent ~35 % of its time inside recvfrom. After a call finds
 * nothing, the next ones on the same socket within the window answer
 * EWOULDBLOCK at once, as the service would have; a packet that arrives
 * meanwhile is read at most one window later. A blocking socket never gets
 * here (its calls do not return EWOULDBLOCK), and FIONBIO clears the window.
 */
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Counters for the "[net]" line, every 10 s from the receiving calls.
std::atomic<uint64_t> g_receives{0};     // recv/recvfrom calls from the guest
std::atomic<uint64_t> g_asked{0};        // of them, sent to the socket service
std::atomic<uint64_t> g_found_none{0};   // of those, EWOULDBLOCK
std::atomic<uint64_t> g_ns_asking{0};    // time inside the service's calls
std::atomic<uint32_t> g_len_max{0};      // the largest buffer length asked for
std::atomic<int64_t> g_report_ns{0};

bool InIdleWindow(const std::atomic<int64_t>& idle_until) {
  g_receives.fetch_add(1, std::memory_order_relaxed);
  const int64_t until = idle_until.load(std::memory_order_relaxed);
  return until != 0 && NowNs() < until;
}

// After a call to the service: the counters, and the window if it found nothing.
void AfterAsking(std::atomic<int64_t>& idle_until, int ret, int error, int64_t since, uint32_t len) {
  const int64_t now = NowNs();
  uint32_t len_max = g_len_max.load(std::memory_order_relaxed);
  while (len > len_max && !g_len_max.compare_exchange_weak(len_max, len, std::memory_order_relaxed)) {
  }
  g_asked.fetch_add(1, std::memory_order_relaxed);
  g_ns_asking.fetch_add(uint64_t(now - since), std::memory_order_relaxed);
  const bool none = ret < 0 && (error == EAGAIN || error == EWOULDBLOCK);
  if (none) {
    g_found_none.fetch_add(1, std::memory_order_relaxed);
  }
  const int64_t window = int64_t(REXCVAR_GET(switch_net_idle_poll_us)) * 1000;
  idle_until.store(none && window > 0 ? now + window : 0, std::memory_order_relaxed);
  int64_t last = g_report_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
  } else if (now - last >= 10000000000ll &&
             g_report_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    const uint64_t receives = g_receives.exchange(0, std::memory_order_relaxed);
    const uint64_t asked = g_asked.exchange(0, std::memory_order_relaxed);
    const uint64_t found_none = g_found_none.exchange(0, std::memory_order_relaxed);
    const uint64_t ns = g_ns_asking.exchange(0, std::memory_order_relaxed);
    REXLOG_INFO("[net] recv/recvfrom, last {:.0f} s: {} calls, {} sent to the socket service ({} found nothing; "
                "{:.1f} ms inside it, {:.1f} us each; buffers up to {} bytes), {} answered from the idle window "
                "(switch_net_idle_poll_us {})",
                double(now - last) / 1e9, receives, asked, found_none, double(ns) / 1e6,
                asked ? double(ns) / 1e3 / double(asked) : 0.0, g_len_max.exchange(0, std::memory_order_relaxed),
                receives > asked ? receives - asked : 0, REXCVAR_GET(switch_net_idle_poll_us));
  }
}
}  // namespace
#endif

namespace system_link {

namespace {
struct LanState {
  std::mutex mutex;
  int64_t checked_ns = 0;
  bool up = false;
  uint32_t ip = 0, mask = 0;
};
LanState& Lan() {
  static LanState state;
  return state;
}

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Asks the platform; host byte order.
bool QueryLan(uint32_t& ip, uint32_t& mask) {
#if REX_PLATFORM_SWITCH
  static const bool nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  if (!nifm) return false;
  u32 address = 0, subnet = 0, gateway = 0, dns1 = 0, dns2 = 0;
  if (R_FAILED(nifmGetCurrentIpConfigInfo(&address, &subnet, &gateway, &dns1, &dns2)) || !address) return false;
  ip = ntohl(address);
  mask = ntohl(subnet);
  return true;
#else
  // The address of the interface the system would route a LAN packet through; connecting a datagram socket sends
  // nothing.
  const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle == decltype(handle)(-1)) return false;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(9);
  to.sin_addr.s_addr = htonl(0x0AFFFFFFu);  // 10.255.255.255
  sockaddr_in self{};
  socklen_t len = sizeof(self);
  const bool ok = connect(handle, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
                  getsockname(handle, reinterpret_cast<sockaddr*>(&self), &len) == 0 && self.sin_addr.s_addr;
#if REX_PLATFORM_WIN32
  closesocket(handle);
#else
  close(handle);
#endif
  if (!ok) return false;
  ip = ntohl(self.sin_addr.s_addr);
  mask = 0xFFFFFF00u;
#if REX_PLATFORM_WIN32
  // The interface's own mask, for the LAN's broadcast address.
  const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (probe != INVALID_SOCKET) {
    INTERFACE_INFO interfaces[32];
    DWORD bytes = 0;
    if (WSAIoctl(probe, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces, sizeof(interfaces), &bytes, nullptr,
                 nullptr) == 0) {
      for (DWORD i = 0; i < bytes / sizeof(INTERFACE_INFO); ++i) {
        if (ntohl(interfaces[i].iiAddress.AddressIn.sin_addr.s_addr) == ip) {
          mask = ntohl(interfaces[i].iiNetmask.AddressIn.sin_addr.s_addr);
        }
      }
    }
    closesocket(probe);
  }
#endif
  return true;
#endif
}
}  // namespace

bool LanAddress(uint32_t* ip, uint32_t* mask) {
  auto& lan = Lan();
  std::lock_guard lock(lan.mutex);
  const int64_t now = SteadyNs();
  if (!lan.checked_ns || now - lan.checked_ns > 5'000'000'000ll) {
    lan.checked_ns = now;
    uint32_t new_ip = 0, new_mask = 0;
    const bool up = QueryLan(new_ip, new_mask);
    if (up != lan.up || new_ip != lan.ip) {
      if (up) {
        REXSYS_INFO("System link: LAN address {}.{}.{}.{}", new_ip >> 24, (new_ip >> 16) & 0xFF,
                    (new_ip >> 8) & 0xFF, new_ip & 0xFF);
      } else {
        REXSYS_INFO("System link: no LAN address");
      }
    }
    lan.up = up;
    lan.ip = up ? new_ip : 0;
    lan.mask = up ? new_mask : 0;
  }
  if (ip) *ip = lan.ip;
  if (mask) *mask = lan.mask;
  return lan.up;
}

void MakeXnAddr(uint32_t ip, uint8_t* xnaddr) {
  std::memset(xnaddr, 0, 36);
  for (int i = 0; i < 4; ++i) xnaddr[i] = uint8_t(ip >> (24 - 8 * i));  // ina, network order
  // abEnet (offset 10): a locally administered address made from the IP.
  xnaddr[10] = 0x02;
  xnaddr[11] = 0x42;
  for (int i = 0; i < 4; ++i) xnaddr[12 + i] = uint8_t(ip >> (24 - 8 * i));
}

bool TitleAddress(uint32_t* ip) {
  // Local multi-copy mode gives each guest a distinct address. These guest
  // identities are translated to the host's configured 127.0.0.1 by XSocket.
  if (const char* value = std::getenv("BO2_INSTANCE")) {
    char* end = nullptr;
    const long instance = std::strtol(value, &end, 10);
    if (end && *end == '\0' && instance >= 0 && instance < 16) {
      // Avoid 127.0.0.1 because XNet treats INADDR_LOOPBACK as a request for
      // this process's own XNADDR during address conversion.
      if (ip) *ip = 0x7F000101u + uint32_t(instance);
      return true;
    }
  }
  uint32_t lan = 0;
  if (!LanAddress(&lan)) return false;
  const uint32_t public_ip = online::PublicAddress();
  if (ip) *ip = public_ip ? public_ip : lan;
  return true;
}

bool IsSelf(uint32_t ip) {
  uint32_t lan = 0;
  return ip && ((LanAddress(&lan) && ip == lan) || ip == online::PublicAddress());
}

bool OnLan(uint32_t ip) {
  uint32_t lan = 0, mask = 0;
  return LanAddress(&lan, &mask) && mask && (ip & mask) == (lan & mask);
}

uint16_t HostPort(uint16_t guest_port) {
  const int offset = REXCVAR_GET(net_system_port_offset);
  if (guest_port && guest_port < 1024 && offset) return uint16_t(guest_port + offset);
  if (guest_port == 3074) {
    if (const char* value = std::getenv("BO2_INSTANCE")) {
      char* end = nullptr;
      const long instance = std::strtol(value, &end, 10);
      if (end && *end == '\0' && instance >= 0 && instance < 16) return uint16_t(3074 + instance * 16);
    }
  }
  return guest_port;
}

namespace {
// The first packets of each kind on a moved port (net_system_link_trace).
void Trace(const char* what, uint32_t ip, uint16_t guest_port, int bytes, int error) {
  if (!REXCVAR_GET(net_system_link_trace) || !guest_port || guest_port >= 1024) return;
  static std::atomic<int> sent{0}, received{0};
  auto& count = what[0] == 's' ? sent : received;
  if (count.fetch_add(1, std::memory_order_relaxed) >= 200) return;
  REXSYS_INFO("[system link] {} {}.{}.{}.{}:{} ({} bytes{}{})", what, ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
              ip & 0xFF, guest_port, bytes, bytes < 0 ? ", errno " : "", bytes < 0 ? std::to_string(error) : "");
}
}  // namespace

void TraceSend(uint32_t ip, uint16_t guest_port, int bytes, int error) { Trace("sent to", ip, guest_port, bytes, error); }
void TraceReceive(uint32_t ip, uint16_t guest_port, int bytes) { Trace("received from", ip, guest_port, bytes, 0); }

uint16_t GuestPort(uint16_t host_port) {
  // Receive traffic from any local instance's disjoint port block, not just
  // this process's block. The instance stride leaves room for BO2's ports.
  for (int instance = 0; instance < 16; ++instance) {
    const int first = 31000 + instance * 16;  // host offset + BO2 title ports 1000..1015
    if (host_port >= first && host_port < first + 16) return uint16_t(1000 + host_port - first);
    const int system_link = 3074 + instance * 16;
    if (host_port >= system_link && host_port < system_link + 16) {
      return uint16_t(3074 + host_port - system_link);
    }
  }
  const int offset = REXCVAR_GET(net_system_port_offset);
  return offset && host_port >= offset && host_port < offset + 1024 ? uint16_t(host_port - offset) : host_port;
}

namespace {
// The last sender per moved port (LastPeer). A handful of ports at most; a new port takes the oldest entry.
struct Peer {
  std::atomic<uint32_t> port{0};
  std::atomic<uint32_t> ip{0};
  std::atomic<int64_t> when_ns{0};
};
std::array<Peer, 4> g_peers;

void NotePeer(uint32_t ip, uint16_t guest_port) {
  if (!guest_port || guest_port >= 1024 || !ip) return;
  uint32_t self = 0;
  if (LanAddress(&self) && ip == self) return;
  const int64_t now = SteadyNs();
  Peer* slot = nullptr;
  for (auto& peer : g_peers) {
    if (peer.port.load(std::memory_order_relaxed) == guest_port) slot = &peer;
  }
  if (!slot) {
    slot = &g_peers[0];
    for (auto& peer : g_peers) {
      if (peer.when_ns.load(std::memory_order_relaxed) < slot->when_ns.load(std::memory_order_relaxed)) slot = &peer;
    }
    slot->port.store(guest_port, std::memory_order_relaxed);
  }
  slot->ip.store(ip, std::memory_order_relaxed);
  slot->when_ns.store(now, std::memory_order_release);
}

#if REX_PLATFORM_WIN32
using NativeSocket = SOCKET;
#else
using NativeSocket = int;
#endif

int LastSocketError() {
#if REX_PLATFORM_WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}
}  // namespace

bool LastPeer(uint16_t guest_port, uint32_t* ip, int64_t* age_ms) {
  for (const auto& peer : g_peers) {
    if (peer.port.load(std::memory_order_relaxed) != guest_port) continue;
    const int64_t when = peer.when_ns.load(std::memory_order_acquire);
    if (!when) return false;
    if (ip) *ip = peer.ip.load(std::memory_order_relaxed);
    if (age_ms) *age_ms = (SteadyNs() - when) / 1000000;
    return true;
  }
  return false;
}

namespace {
// Online play: the consoles on the internet heard from (not this LAN), per title port. A NAT that rewrites a peer's
// source port would hand the title a port it does not know (BO2 tags packets from its ports 1000/1001 and reads any
// other port from the packet, 0x824E1CA0), so the title sees the port it talks on and replies go to the port the
// datagram really came from. The table also lets a broadcast reach them (InternetCopies).
//
// Consoles behind one NAT (two on the same phone hotspot or home network) share a public address and differ only in
// their NAT ports. The title knows a peer by its address alone, so all but the first of them get a stand-in address of
// their own (240.66.0.x, which nothing on the internet uses); sends to it go to that console's real address and ports.
// Which NAT ports belong to which console comes from the relay: each console's endpoints and punches carry its relay
// session (online_relay.h).
struct InternetPeer {
  uint32_t ip = 0;          // where it is on the internet
  uint32_t title_ip = 0;    // the address the title knows it by: ip, or a stand-in
  uint64_t console = 0;     // its relay session, 0 while unknown
  uint16_t guest_port = 0;  // the title's port, as the title sees it
  uint16_t host_port = 0;   // the port the datagram came from (or, until heard, the port the relay gave)
  int64_t when_ns = 0;
  bool heard = false;       // a datagram came from it; otherwise the relay's word only
};
std::mutex g_internet_mutex;
std::array<InternetPeer, 32> g_internet;

// The title address of each console the relay named, by public address and session (under g_internet_mutex).
struct NamedConsole {
  uint32_t ip = 0;
  uint64_t console = 0;
  uint32_t title_ip = 0;
  int64_t heard_ns = 0;  // the last datagram known to be its
  uint32_t lan_ip = 0;   // its LAN address, as it says
  bool tagged = false;   // its XNADDR carries its session (MakeOwnXnAddr): it said so through the relay or its XNADDR
};
// Consoles behind this console's own router, known from their tagged XNADDRs: the title reaches them on the LAN.
struct LanPeer {
  uint32_t lan_ip = 0;
  uint32_t ip = 0;  // the public address they share with this console
  uint64_t console = 0;
};
std::array<LanPeer, 8> g_lan_peers;
size_t g_lan_peers_next = 0;
std::array<NamedConsole, 16> g_named;
size_t g_named_next = 0;
// The relay's word on whose each public endpoint is (kept apart from what is heard, which may come from elsewhere).
struct NamedEndpoint {
  uint32_t ip = 0;
  uint16_t host_port = 0;
  uint16_t guest_port = 0;
  uint64_t console = 0;
};
std::array<NamedEndpoint, 48> g_endpoints;
size_t g_endpoints_next = 0;
uint32_t g_stand_in_next = 0;
constexpr uint32_t kStandInBase = 0xF0420000u;  // 240.66.0.0
constexpr int64_t kSharedQuietNs = 10'000'000'000ll;

std::string Dotted(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." + std::to_string((ip >> 8) & 0xFF) +
         "." + std::to_string(ip & 0xFF);
}

// The address the title knows the console with this relay session at ip by: the first one named at a public address
// keeps it, every other one there gets a stand-in. An unnamed console (session 0) is the public address.
NamedConsole& NameConsole(uint32_t ip, uint64_t console) {
  bool shared = false;
  for (auto& named : g_named) {
    if (named.ip != ip || !named.console) continue;
    if (named.console == console) return named;
    shared = true;
  }
  NamedConsole& slot = g_named[g_named_next++ % g_named.size()];
  slot = {};
  slot.ip = ip;
  slot.console = console;
  slot.title_ip = shared ? kStandInBase | (1 + g_stand_in_next++ % 250) : ip;
  if (shared) {
    REXSYS_INFO("[online] another console behind {} (a shared address): the game knows it as {}", Dotted(ip),
                Dotted(slot.title_ip));
  }
  return slot;
}

uint32_t TitleIpFor(uint32_t ip, uint64_t console) { return console ? NameConsole(ip, console).title_ip : ip; }

// The tag in an XNADDR's abOnline (offset 16, otherwise unused in System Link): "BZ", a version, the console's relay
// session and its LAN address.
constexpr int kTagAt = 16;

void WriteTag(uint8_t* xnaddr, uint64_t console, uint32_t lan_ip) {
  uint8_t* tag = xnaddr + kTagAt;
  tag[0] = 'B';
  tag[1] = 'Z';
  tag[2] = 1;
  tag[3] = 0;
  for (int i = 0; i < 8; ++i) tag[4 + i] = uint8_t(console >> (56 - 8 * i));
  for (int i = 0; i < 4; ++i) tag[12 + i] = uint8_t(lan_ip >> (24 - 8 * i));
}

bool ReadTag(const uint8_t* xnaddr, uint64_t* console, uint32_t* lan_ip) {
  const uint8_t* tag = xnaddr + kTagAt;
  if (tag[0] != 'B' || tag[1] != 'Z' || tag[2] != 1) return false;
  uint64_t session = 0;
  uint32_t lan = 0;
  for (int i = 0; i < 8; ++i) session = session << 8 | tag[4 + i];
  for (int i = 0; i < 4; ++i) lan = lan << 8 | tag[12 + i];
  if (!session) return false;
  *console = session;
  *lan_ip = lan;
  return true;
}

// The newest entry for the title's view of a console and port, or nullptr.
InternetPeer* FindTitle(uint32_t title_ip, uint16_t guest_port) {
  InternetPeer* found = nullptr;
  for (auto& peer : g_internet) {
    if (peer.when_ns && peer.title_ip == title_ip && peer.guest_port == guest_port &&
        (!found || peer.when_ns > found->when_ns)) {
      found = &peer;
    }
  }
  return found;
}

const NamedEndpoint* FindNamedEndpoint(uint32_t ip, uint16_t host_port) {
  for (const auto& named : g_endpoints) {
    if (named.console && named.ip == ip && named.host_port == host_port) return &named;
  }
  return nullptr;
}

// The newest entry for a public endpoint, or nullptr.
const InternetPeer* FindEndpoint(uint32_t ip, uint16_t host_port) {
  const InternetPeer* found = nullptr;
  for (const auto& peer : g_internet) {
    if (peer.when_ns && peer.ip == ip && peer.host_port == host_port && (!found || peer.when_ns > found->when_ns)) {
      found = &peer;
    }
  }
  return found;
}

InternetPeer* Oldest() {
  InternetPeer* slot = &g_internet[0];
  for (auto& peer : g_internet) {
    if (peer.when_ns < slot->when_ns) slot = &peer;
  }
  return slot;
}

// A datagram from ip:host_port reached a title socket bound to guest_port (both ports as the socket layer sees them);
// console is the sender's relay session when the datagram says (a punch), else 0. Gives the address and port to report
// to the title. false: the datagram comes from a port the relay named for no console, at an address where a console
// the relay named is talking right now: it may be another console behind the same address that the relay has not
// named yet, so it is dropped until it is named (its next tries get through) and the title never mixes the two up.
// A console alone behind its address is taken whatever port it comes from (a NAT may map each destination apart).
bool NoteInternetPeer(uint32_t ip, uint16_t host_port, uint16_t bound_guest_port, uint64_t punch_console,
                      uint32_t* title_ip, uint16_t* title_port) {
  uint64_t console = punch_console;
  uint16_t guest = GuestPort(host_port);
  bool rewritten = guest == host_port && bound_guest_port && bound_guest_port < 1024;
  if (rewritten) guest = bound_guest_port;
  const int64_t now = SteadyNs();
  std::lock_guard lock(g_internet_mutex);
  // The relay's endpoints say whose this endpoint is, and which title port a rewritten source port stands for.
  if (const NamedEndpoint* named = FindNamedEndpoint(ip, host_port)) {
    guest = named->guest_port;
    if (!console) console = named->console;
  } else if (const InternetPeer* known = FindEndpoint(ip, host_port)) {
    guest = known->guest_port;
  }
  rewritten = GuestPort(host_port) != guest;
  if (!console) {
    for (const auto& named : g_named) {
      if (named.console && named.ip == ip && named.heard_ns && now - named.heard_ns < kSharedQuietNs) return false;
    }
  }
  const uint32_t title = TitleIpFor(ip, console);
  for (auto& named : g_named) {
    if (console && named.console == console && named.ip == ip) named.heard_ns = now;
  }
  // A punch names its sender, and the title's datagrams to this console leave from the same port as the punch.
  if (punch_console && !FindNamedEndpoint(ip, host_port)) {
    g_endpoints[g_endpoints_next++ % g_endpoints.size()] = {ip, host_port, guest, punch_console};
  }
  InternetPeer* slot = FindTitle(title, guest);
  const bool fresh = !slot || slot->host_port != host_port || !slot->heard;
  if (!slot) slot = Oldest();
  if (fresh) {
    REXSYS_INFO("[online] heard {} from port {} on title port {}{}{}", Dotted(ip), host_port, guest,
                title != ip ? " as " + Dotted(title) : std::string(),
                rewritten ? " (a NAT rewrote the source port; replies go back to it)" : "");
  }
  slot->ip = ip;
  slot->title_ip = title;
  if (console) slot->console = console;
  slot->guest_port = guest;
  slot->host_port = host_port;
  slot->when_ns = now;
  slot->heard = true;
  if (title_ip) *title_ip = title;
  if (title_port) *title_port = guest;
  return true;
}

// Where to send for the console the title knows as title_ip, on the title's port: its public address and the port it
// was last heard from (or that the relay gave), or the moved port.
void InternetRoute(uint32_t title_ip, uint16_t guest_port, uint32_t* ip, uint16_t* port) {
  std::lock_guard lock(g_internet_mutex);
  if (const InternetPeer* peer = FindTitle(title_ip, guest_port)) {
    *ip = peer->ip;
    *port = peer->host_port;
    return;
  }
  *ip = title_ip;
  for (const auto& named : g_named) {
    if (named.console && named.title_ip == title_ip) *ip = named.ip;
  }
  *port = HostPort(guest_port);
}

uint16_t InternetHostPort(uint32_t title_ip, uint16_t guest_port) {
  uint32_t ip = 0;
  uint16_t port = 0;
  InternetRoute(title_ip, guest_port, &ip, &port);
  return port;
}

// The address the title knows the sender of a datagram from ip:host_port by (for what the side socket delivers).
uint32_t TitleSource(uint32_t ip, uint16_t host_port) {
  std::lock_guard lock(g_internet_mutex);
  if (const NamedEndpoint* named = FindNamedEndpoint(ip, host_port)) return TitleIpFor(ip, named->console);
  const InternetPeer* known = FindEndpoint(ip, host_port);
  return known ? known->title_ip : ip;
}

// A punch: the 8-byte magic, then the sender's relay session (online_relay.cpp).
uint64_t PunchSession(const uint8_t* punch) {
  uint64_t session = 0;
  for (int i = 0; i < 8; ++i) session = session << 8 | punch[8 + i];
  return session;
}

// A datagram the online relay took on a title socket or the side socket (a punch): its sender is a console on the
// internet like any other.
void NoteRelayDatagram(uint32_t ip, uint16_t host_port, uint16_t bound_guest_port, const uint8_t* punch) {
  if (online::PublicAddress() && !OnLan(ip) && !IsSelf(ip)) {
    NoteInternetPeer(ip, host_port, bound_guest_port, PunchSession(punch), nullptr, nullptr);
  }
}

// Where a broadcast to the title's guest_port also goes while online: each console on the internet heard on that
// port in the last 15 s (a host answering a search), and the host of a join code (a joiner's search). A broadcast
// never leaves the LAN, so without these copies neither side would hear the other.
std::vector<std::pair<uint32_t, uint16_t>> InternetCopies(uint16_t guest_port) {
  std::vector<std::pair<uint32_t, uint16_t>> copies;
  const uint32_t target = online::Target();
  const int64_t now = SteadyNs();
  {
    std::lock_guard lock(g_internet_mutex);
    for (const auto& peer : g_internet) {
      if (peer.guest_port == guest_port && peer.when_ns && now - peer.when_ns < 15'000'000'000ll &&
          std::none_of(copies.begin(), copies.end(),
                       [&](const auto& c) { return c.first == peer.ip && c.second == peer.host_port; })) {
        copies.push_back({peer.ip, peer.host_port});
      }
    }
  }
  if (target && std::none_of(copies.begin(), copies.end(), [&](const auto& c) { return c.first == target; })) {
    copies.push_back({target, InternetHostPort(target, guest_port)});
  }
  return copies;
}

// net_online_trace: the first internet datagrams each way, with the start of their text (BO2's connectionless
// messages, such as getinfo and infoResponse, start with four 0xFF bytes and a word).
void TraceInternet(const char* what, uint32_t ip, uint16_t port, uint16_t guest_port, const uint8_t* data, int bytes) {
  if (!REXCVAR_GET(net_online_trace) || bytes < 0) return;
  static std::atomic<int> count{0};
  if (count.fetch_add(1, std::memory_order_relaxed) >= 400) return;
  std::string text;
  for (int i = 0; i < bytes && text.size() < 24; ++i) {
    const uint8_t c = data[i];
    text += c >= 0x20 && c < 0x7F ? char(c) : '.';
  }
  REXSYS_INFO("[online] {} {}.{}.{}.{}:{} (title port {}, {} bytes) \"{}\"", what, ip >> 24, (ip >> 16) & 0xFF,
              (ip >> 8) & 0xFF, ip & 0xFF, port, guest_port, bytes, text);
}
}  // namespace

void SeedInternetPeer(uint32_t ip, uint16_t guest_port, uint16_t host_port, uint64_t console) {
  if (!ip || !host_port) return;
  const int64_t now = SteadyNs();
  std::lock_guard lock(g_internet_mutex);
  const uint32_t title = TitleIpFor(ip, console);
  if (console) {
    NamedEndpoint* named = nullptr;
    for (auto& entry : g_endpoints) {
      if (entry.ip == ip && entry.host_port == host_port) named = &entry;
    }
    if (!named) named = &g_endpoints[g_endpoints_next++ % g_endpoints.size()];
    *named = {ip, host_port, guest_port, console};
    // Datagrams heard from this endpoint before the relay said whose it is were this console's.
    for (auto& peer : g_internet) {
      if (peer.when_ns && peer.ip == ip && peer.host_port == host_port && peer.console != console) {
        peer.console = console;
        peer.title_ip = title;
        peer.guest_port = guest_port;
      }
    }
  }
  InternetPeer* slot = FindTitle(title, guest_port);
  // The port it really sends from wins over the relay's word, unless that was another console's.
  if (slot && slot->heard && (slot->host_port == host_port || slot->console == console)) return;
  if (!slot) {
    slot = Oldest();
    if (REXCVAR_GET(net_online_trace)) {
      REXSYS_INFO("[online] {} reaches title port {} at port {} (from the relay){}", Dotted(ip), guest_port, host_port,
                  title != ip ? ", known to the game as " + Dotted(title) : std::string());
    }
  }
  slot->ip = ip;
  slot->title_ip = title;
  slot->console = console;
  slot->guest_port = guest_port;
  slot->host_port = host_port;
  slot->when_ns = now;
  slot->heard = false;
}

bool HeardInternetPeer(uint32_t ip, uint16_t host_port, uint16_t* guest_port) {
  std::lock_guard lock(g_internet_mutex);
  for (const auto& peer : g_internet) {
    if (!peer.when_ns || !peer.heard || peer.ip != ip || peer.host_port != host_port) continue;
    if (guest_port) *guest_port = peer.guest_port;
    return true;
  }
  return false;
}

void ForgetInternetPeers() {
  std::lock_guard lock(g_internet_mutex);
  g_internet = {};
  g_named = {};
  g_named_next = 0;
  g_endpoints = {};
  g_endpoints_next = 0;
  g_lan_peers = {};
  g_lan_peers_next = 0;
}

void NameConsoleLan(uint32_t ip, uint64_t console, uint32_t lan_ip) {
  if (!ip || !console) return;
  std::lock_guard lock(g_internet_mutex);
  NamedConsole& named = NameConsole(ip, console);
  named.lan_ip = lan_ip;
  named.tagged = true;
}

void MakeOwnXnAddr(uint32_t ip, uint8_t* xnaddr) {
  MakeXnAddr(ip, xnaddr);
  const uint64_t console = online::relay::TagSession();
  uint32_t lan = 0;
  if (console && ip && ip == online::PublicAddress() && LanAddress(&lan)) WriteTag(xnaddr, console, lan);
}

void MakeTitleXnAddr(uint32_t ip, uint8_t* xnaddr) {
  std::lock_guard lock(g_internet_mutex);
  for (const auto& named : g_named) {
    if (named.console && named.tagged && named.title_ip == ip) {
      MakeXnAddr(named.ip, xnaddr);
      WriteTag(xnaddr, named.console, named.lan_ip);
      return;
    }
  }
  for (const auto& peer : g_lan_peers) {
    if (peer.console && peer.lan_ip == ip) {
      MakeXnAddr(peer.ip, xnaddr);
      WriteTag(xnaddr, peer.console, peer.lan_ip);
      return;
    }
  }
  MakeXnAddr(ip, xnaddr);
}

uint32_t XnAddrToTitleIp(const uint8_t* xnaddr) {
  const uint32_t ina = uint32_t(xnaddr[0]) << 24 | uint32_t(xnaddr[1]) << 16 | uint32_t(xnaddr[2]) << 8 | xnaddr[3];
  uint64_t console = 0;
  uint32_t lan = 0;
  if (!ReadTag(xnaddr, &console, &lan) || console == online::relay::TagSession()) return ina;
  const uint32_t public_ip = online::PublicAddress();
  std::lock_guard lock(g_internet_mutex);
  if (public_ip && ina == public_ip) {
    // Another console behind this one's router: the internet cannot loop back to it, the LAN reaches it.
    LanPeer* slot = nullptr;
    for (auto& peer : g_lan_peers) {
      if (peer.console == console) slot = &peer;
    }
    if (!slot) {
      slot = &g_lan_peers[g_lan_peers_next++ % g_lan_peers.size()];
      REXSYS_INFO("[online] a console behind this one's own public address: reached on the LAN at {}", Dotted(lan));
    }
    *slot = {lan, ina, console};
    return lan ? lan : kStandInBase | 0xFE;
  }
  NamedConsole& named = NameConsole(ina, console);
  named.lan_ip = lan;
  named.tagged = true;
  return named.title_ip;
}

namespace {
// The sockets on the ports internet play uses, by the port they are bound to (host byte order): the title's sockets on
// moved ports (XSocket::Bind) and the side socket. The online relay sends its STUN requests and punches from them.
struct PortSocket {
  uint16_t host_port = 0;
  uint64_t native = 0;
};
std::mutex g_port_mutex;
std::array<PortSocket, 8> g_port_sockets{};

void RegisterPortSocket(uint16_t host_port, uint64_t native) {
  std::lock_guard lock(g_port_mutex);
  PortSocket* slot = nullptr;
  for (auto& entry : g_port_sockets) {
    if (entry.host_port == host_port) slot = &entry;
  }
  for (auto& entry : g_port_sockets) {
    if (!slot && !entry.host_port) slot = &entry;
  }
  if (!slot) return;
  slot->host_port = host_port;
  slot->native = native;
}

void UnregisterPortSocket(uint64_t native) {
  std::lock_guard lock(g_port_mutex);
  for (auto& entry : g_port_sockets) {
    if (entry.host_port && entry.native == native) entry = {};
  }
}

// The side socket, shared by its openers (OpenSideSocket), with the datagrams PumpSideSocket read for SideReceive.
struct SideState {
  std::mutex mutex;
  int64_t socket = -1;
  uint16_t port = 0;
  bool bound = false;
  int openers = 0;
  struct Waiting {
    uint32_t ip = 0;
    uint16_t port = 0;
    std::vector<uint8_t> data;
  };
  std::deque<Waiting> waiting;
};
SideState& Side() {
  static SideState side;
  return side;
}

// One datagram from the side socket that is not the relay's own; -1 when none waits. Under side.mutex.
int SideRead(SideState& side, void* data, uint32_t capacity, uint32_t* ip, uint16_t* port) {
  for (int taken = 0; taken < 64; ++taken) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int got = int(recvfrom(NativeSocket(side.socket), reinterpret_cast<char*>(data), int(capacity), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len));
    if (got < 0) return -1;
    const uint32_t source = ntohl(from.sin_addr.s_addr);
    const uint16_t source_port = ntohs(from.sin_port);
    const online::Datagram kind =
        online::Classify(side.port, source, source_port, reinterpret_cast<const uint8_t*>(data), got);
    if (kind == online::Datagram::kTitle) {
      const bool internet = online::PublicAddress() && !OnLan(source) && !IsSelf(source);
      if (ip) *ip = internet ? TitleSource(source, source_port) : source;
      if (port) *port = source_port;
      return got;
    }
    if (kind == online::Datagram::kPunch) {
      NoteRelayDatagram(source, source_port, GuestPort(side.port), reinterpret_cast<const uint8_t*>(data));
    }
  }
  return -1;
}
}  // namespace

int SendFromPort(uint16_t host_port, uint32_t ip, uint16_t port, const void* data, uint32_t size) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
#if REX_PLATFORM_SWITCH
  to.sin_len = sizeof(to);
#endif
  std::lock_guard lock(g_port_mutex);  // a socket the title closes leaves the table first (XSocket::Close)
  for (const auto& entry : g_port_sockets) {
    if (entry.host_port != host_port) continue;
    return int(sendto(NativeSocket(entry.native), reinterpret_cast<const char*>(data), int(size), 0,
                      reinterpret_cast<sockaddr*>(&to), sizeof(to)));
  }
  return -1;
}

void PumpSideSocket() {
  auto& side = Side();
  std::lock_guard lock(side.mutex);
  if (side.socket < 0) return;
  std::array<uint8_t, 2048> buffer;
  for (;;) {
    SideState::Waiting item;
    const int got = SideRead(side, buffer.data(), uint32_t(buffer.size()), &item.ip, &item.port);
    if (got < 0) return;
    item.data.assign(buffer.data(), buffer.data() + got);
    if (side.waiting.size() >= 64) side.waiting.pop_front();
    side.waiting.push_back(std::move(item));
  }
}

int64_t OpenSideSocket(uint16_t port, bool* bound) {
  if (bound) *bound = false;
  auto& side = Side();
  std::lock_guard lock(side.mutex);
  if (side.socket >= 0 && side.port == port) {
    ++side.openers;
    if (bound) *bound = side.bound;
    return side.socket;
  }
  auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle == decltype(handle)(-1)) {
    REXSYS_WARN("System link: no side socket (error {})", LastSocketError());
    return -1;
  }
  const int64_t socket_handle = int64_t(handle);
  int on = 1;
  setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
  uint32_t nonblocking = 1;
  if (rex::net::socket_ioctl(socket_handle, 0x8004667Eu, reinterpret_cast<uint8_t*>(&nonblocking)) != 0) {  // FIONBIO
    REXSYS_WARN("System link: the side socket cannot be made non-blocking (error {})", LastSocketError());
    rex::net::socket_close(socket_handle);
    return -1;
  }
  sockaddr_in any{};
  any.sin_family = AF_INET;
  any.sin_port = htons(port);
  any.sin_addr.s_addr = htonl(INADDR_ANY);
#if REX_PLATFORM_SWITCH
  any.sin_len = sizeof(any);
#endif
  const bool took_port = bind(handle, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0;
  if (took_port) {
    if (bound) *bound = true;
  } else {
    REXSYS_WARN("System link: side port {} is taken (error {}); sending from another port", port,
                LastSocketError());
  }
  if (side.socket < 0) {
    side.socket = socket_handle;
    side.port = port;
    side.bound = took_port;
    side.openers = 1;
    side.waiting.clear();
    if (took_port) RegisterPortSocket(port, uint64_t(socket_handle));
  }
  return socket_handle;
}

void CloseSideSocket(int64_t socket) {
  if (socket < 0) return;
  auto& side = Side();
  std::lock_guard lock(side.mutex);
  if (socket == side.socket) {
    if (--side.openers > 0) return;
    UnregisterPortSocket(uint64_t(socket));
    side.socket = -1;
    side.waiting.clear();
  }
  rex::net::socket_close(socket);
}

int SideSend(int64_t socket, uint32_t ip, uint16_t port, const void* data, uint32_t size) {
  if (socket < 0) return -1;
  if (online::PublicAddress() && ip != 0xFFFFFFFFu && !OnLan(ip) && !IsSelf(ip)) {
    InternetRoute(ip, GuestPort(port), &ip, &port);
  }
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
#if REX_PLATFORM_SWITCH
  to.sin_len = sizeof(to);
#endif
  return int(sendto(NativeSocket(socket), reinterpret_cast<const char*>(data), int(size), 0,
                    reinterpret_cast<sockaddr*>(&to), sizeof(to)));
}

int SideReceive(int64_t socket, void* data, uint32_t capacity, uint32_t* ip, uint16_t* port) {
  if (socket < 0) return -1;
  auto& side = Side();
  {
    std::lock_guard lock(side.mutex);
    if (socket == side.socket) {
      if (side.waiting.empty()) return SideRead(side, data, capacity, ip, port);
      SideState::Waiting& item = side.waiting.front();
      const uint32_t size = std::min<uint32_t>(capacity, uint32_t(item.data.size()));
      std::memcpy(data, item.data.data(), size);
      if (ip) *ip = item.ip;
      if (port) *port = item.port;
      side.waiting.pop_front();
      return int(size);
    }
  }
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  const int got = int(recvfrom(NativeSocket(socket), reinterpret_cast<char*>(data), int(capacity), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len));
  if (got < 0) return -1;
  if (ip) *ip = ntohl(from.sin_addr.s_addr);
  if (port) *port = ntohs(from.sin_port);
  return got;
}

}  // namespace system_link

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (broadcast_helper_) {
    rex::net::socket_close(broadcast_helper_);
    broadcast_helper_ = 0;
  }
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;  // closesocket already did; the destructor must not close a number reused since
  }
  if (port_registered_) {
    system_link::UnregisterPortSocket(native_handle_);
    port_registered_ = false;
  }
  int ret = rex::net::socket_close(native_handle_);
  native_handle_ = uint64_t(-1);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (REXCVAR_GET(net_system_link_trace)) {
    uint32_t value = 0;
    if (optval_ptr && optlen >= 4) std::memcpy(&value, optval_ptr, 4);
    REXSYS_INFO("[system link] setsockopt on socket {:X} (bound port {}): level {:X} option {:X} length {} value {:08X}",
                uint64_t(native_handle_), bound_port_, level, optname, optlen, value);
  }
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int native_level = int(level);
  int native_optname = int(optname);
#if defined(__linux__)
  // Winsock's SOL_SOCKET options have the BSD numbers (the Switch's and macOS's); Linux numbers them its own way.
  if (level == 0xFFFF) {
    native_level = SOL_SOCKET;
    switch (optname) {
      case 0x0001: native_optname = SO_DEBUG; break;
      case 0x0004: native_optname = SO_REUSEADDR; break;
      case 0x0008: native_optname = SO_KEEPALIVE; break;
      case 0x0010: native_optname = SO_DONTROUTE; break;
      case 0x0020: native_optname = SO_BROADCAST; break;
      case 0x0080: native_optname = SO_LINGER; break;
      case 0x0100: native_optname = SO_OOBINLINE; break;
      case 0x1001: native_optname = SO_SNDBUF; break;
      case 0x1002: native_optname = SO_RCVBUF; break;
      default: break;
    }
  }
#endif
  int ret = setsockopt(native_handle_, native_level, native_optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
#if REX_PLATFORM_SWITCH
  idle_until_ns_.store(0, std::memory_order_relaxed);  // FIONBIO may make it blocking (the idle window)
#endif
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }
  if (cmd == 0x8004667Eu && arg_ptr) {  // FIONBIO (any byte order: zero or not)
    nonblocking_ = (arg_ptr[0] | arg_ptr[1] | arg_ptr[2] | arg_ptr[3]) != 0;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* guest_name, int name_len) {
  N_XSOCKADDR_IN moved = *guest_name;
  const uint32_t guest_ip = uint32_t(moved.sin_addr);
  const bool instance_address = guest_ip >= 0x7F000101u && guest_ip <= 0x7F000110u;
  if (std::getenv("BO2_INSTANCE") && (!moved.sin_addr || instance_address) && guest_name->sin_port &&
      (guest_name->sin_port < 1024 || guest_name->sin_port == 3074)) {
    // Guest XNADDRs use a per-copy loopback identity, but macOS only exposes
    // 127.0.0.1 on lo0 by default. Port blocks keep these host binds distinct.
    moved.sin_addr = 0x7F000001u;
  }
  moved.sin_port = system_link::HostPort(guest_name->sin_port);
  N_XSOCKADDR_IN* name = &moved;
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    // The guest only sees a generic error; log the real one.
    const uint32_t addr = name->sin_addr;
    REXSYS_WARN("XSocket::Bind failed: errno {} ({}), family {}, {}.{}.{}.{}:{}, len {}, proto {}",
                errno, std::strerror(errno), uint32_t(name->sin_family), addr >> 24,
                (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, uint16_t(name->sin_port),
                name_len, uint32_t(proto_));
#if REX_PLATFORM_SWITCH
    // Offline play never sends through these sockets (BO2's local
    // client and server talk through in-memory queues), but the game treats a
    // failed bind as fatal at startup. Retry on any address/port, and if
    // Horizon still refuses, report success so the game carries on offline.
    N_XSOCKADDR_IN any = *name;
    any.sin_addr = 0;
    any.sin_port = 0;
    if (bind(native_handle_, (sockaddr*)&any, sizeof(any)) == 0) {
      REXSYS_WARN("XSocket::Bind: bound to 0.0.0.0:any instead");
    } else {
      REXSYS_WARN("XSocket::Bind: errno {} again on 0.0.0.0:any; reporting success (offline)",
                  errno);
    }
#else
    return X_STATUS_UNSUCCESSFUL;
#endif
  }

  bound_ = true;
  bound_port_ = guest_name->sin_port;
  // A title port internet play uses (a moved one): the online relay sends its STUN requests and punches from it.
  if (type_ == X_SOCK_DGRAM && bound_port_ && (bound_port_ < 1024 || bound_port_ == 3074)) {
    system_link::RegisterPortSocket(system_link::HostPort(bound_port_), native_handle_);
    port_registered_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
#if REX_PLATFORM_SWITCH
  if (InIdleWindow(idle_until_ns_)) {
    errno = EWOULDBLOCK;
    return -1;
  }
  uint8_t* host = HostBuffer(buf_len);
  const int64_t since = NowNs();
  int ret = recv(native_handle_, reinterpret_cast<char*>(host), buf_len, flags);
  const int error = errno;
  AfterAsking(idle_until_ns_, ret, error, since, buf_len);
  if (ret > 0) {
    std::memcpy(buf, host, ret);
  }
  errno = error;
  return ret;
#else
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
#endif
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = -1;
  bool internet = false;  // from a console on the internet: the title is told title_source:title_port
  uint32_t title_source = 0;
  uint16_t title_port = 0;
  // Online play's relay sends STUN requests and punches from the title's moved ports (online_relay.h); their datagrams
  // are taken here and the title never sees them, and so is a datagram from a console behind a shared address that
  // the relay has not named yet (NoteInternetPeer). The next datagram is read in their place: on a non-blocking socket
  // that ends in "would block" as usual, a blocking one waits on as the title asked.
  for (int taken = 0;; ++taken) {
    internet = false;
    std::memset(&nfrom, 0, sizeof(nfrom));
    nfromlen = sizeof(sockaddr_in);
#if REX_PLATFORM_SWITCH
    ret = -1;
    int error = EWOULDBLOCK;
    uint8_t* host = nullptr;
    // In the idle window: what the service answers when nothing came (nfrom stays zeroed, nfromlen as set above).
    if (!InIdleWindow(idle_until_ns_)) {
      host = HostBuffer(buf_len);
      const int64_t since = NowNs();
      ret = recvfrom(native_handle_, reinterpret_cast<char*>(host), buf_len, flags, (sockaddr*)&nfrom, &nfromlen);
      error = errno;
      AfterAsking(idle_until_ns_, ret, error, since, buf_len);
    }
    const uint8_t* received = host;
#else
    ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags, (sockaddr*)&nfrom, &nfromlen);
    const uint8_t* received = buf;
#endif
    online::Datagram kind = online::Datagram::kTitle;
    if (ret > 0 && bound_port_ && bound_port_ < 1024) {
      kind = online::Classify(system_link::HostPort(bound_port_), ntohl(nfrom.sin_addr.s_addr), ntohs(nfrom.sin_port),
                              received, ret);
    }
    if (kind == online::Datagram::kTitle && ret > 0 && online::PublicAddress()) {
      const uint32_t source = ntohl(nfrom.sin_addr.s_addr);
      internet = source != INADDR_LOOPBACK && !system_link::OnLan(source) && !system_link::IsSelf(source);
      if (internet && !system_link::NoteInternetPeer(source, ntohs(nfrom.sin_port), bound_port_, 0, &title_source,
                                                     &title_port)) {
        internet = false;
        kind = online::Datagram::kStun;  // dropped, as the relay's own are
      }
    }
    if (kind == online::Datagram::kTitle) {
#if REX_PLATFORM_SWITCH
      if (ret > 0) {
        std::memcpy(buf, host, ret);
      }
      errno = error;
#endif
      break;
    }
    if (kind == online::Datagram::kPunch) {
      system_link::NoteRelayDatagram(ntohl(nfrom.sin_addr.s_addr), ntohs(nfrom.sin_port), bound_port_, received);
    }
    if (flags & 0x2) {  // MSG_PEEK left it waiting: read it for real to drop it
      char scratch[64];
      sockaddr_in ignored{};
      socklen_t ignored_len = sizeof(ignored);
      recvfrom(native_handle_, scratch, sizeof(scratch), flags & ~0x2, (sockaddr*)&ignored, &ignored_len);
    }
    if (nonblocking_ && taken >= 15) {
      // A burst of them: answer "nothing yet" now rather than keep the title's thread here.
      ret = -1;
#if REX_PLATFORM_WIN32
      WSASetLastError(WSAEWOULDBLOCK);
#else
      errno = EWOULDBLOCK;
#endif
      std::memset(&nfrom, 0, sizeof(nfrom));
      break;
    }
  }
  if (from) {
    const uint32_t source = ntohl(nfrom.sin_addr.s_addr);
    uint32_t reported = source;
    from->sin_family = nfrom.sin_family;
    from->sin_port = system_link::GuestPort(ntohs(nfrom.sin_port));
    if (ret > 0 && internet) {
      reported = title_source;
      from->sin_port = title_port;
      system_link::TraceInternet("received from", source, ntohs(nfrom.sin_port), from->sin_port, buf, ret);
    }
    if (ret > 0 && source == 0x7F000001u) {
      const uint16_t host_port = ntohs(nfrom.sin_port);
      for (int instance = 0; instance < 16; ++instance) {
        const uint16_t first = uint16_t(31000 + instance * 16);
        if (host_port >= first && host_port < first + 16) {
          reported = 0x7F000101u + uint32_t(instance);
          break;
        }
        const uint16_t system_link = uint16_t(3074 + instance * 16);
        if (host_port >= system_link && host_port < system_link + 16) {
          reported = 0x7F000101u + uint32_t(instance);
          break;
        }
      }
    }
    from->sin_addr = reported;  // BE <- BE
    if (ret > 0) {
      system_link::TraceReceive(reported, from->sin_port, ret);
      system_link::NotePeer(reported, from->sin_port);
    }
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
#if REX_PLATFORM_SWITCH
  uint8_t* host = HostBuffer(buf_len);
  std::memcpy(host, buf, buf_len);
  return send(native_handle_, reinterpret_cast<const char*>(host), buf_len, flags);
#else
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
#endif
}

namespace {
// The limited broadcast or this LAN's own broadcast address (host byte order).
bool IsBroadcast(uint32_t address) {
  uint32_t ip = 0, mask = 0;
  return address == 0xFFFFFFFFu ||
         (system_link::LanAddress(&ip, &mask) && mask && address == (ip | ~mask));
}

int LocalInstance(uint32_t address) {
  return address >= 0x7F000101u && address <= 0x7F000110u ? int(address - 0x7F000101u) : -1;
}

int ConfiguredInstance() {
  const char* value = std::getenv("BO2_INSTANCE");
  if (!value) return -1;
  char* end = nullptr;
  const long instance = std::strtol(value, &end, 10);
  return end && *end == '\0' && instance >= 0 && instance < 16 ? int(instance) : -1;
}
}  // namespace

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  sockaddr_in nto;
  std::memset(&nto, 0, sizeof(nto));
  if (to) {
    uint32_t address = to->sin_addr;
#if REX_PLATFORM_SWITCH
    // A limited broadcast (255.255.255.255) is sent to the LAN's own broadcast address, which Horizon routes.
    uint32_t lan_ip = 0, lan_mask = 0;
    if (address == 0xFFFFFFFFu && system_link::LanAddress(&lan_ip, &lan_mask) && lan_mask) {
      address = lan_ip | ~lan_mask;
    }
#endif
    // A title may broadcast through a socket it never gave SO_BROADCAST (BO2 answers a System Link search that way);
    // the Xbox let it, Windows and BSD sockets refuse the send.
    if (IsBroadcast(address) && !broadcast_native_) {
      int on = 1;
      const int set = setsockopt(native_handle_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on),
                                 sizeof(on));
      broadcast_native_ = set == 0;
      if (REXCVAR_GET(net_system_link_trace)) {
        REXSYS_INFO("[system link] SO_BROADCAST on socket {:X} for a broadcast: {}", uint64_t(native_handle_),
                    set == 0 ? "set" : "refused");
      }
    }
    nto.sin_addr.s_addr = htonl(address);
    nto.sin_family = to->sin_family;
    nto.sin_port = htons(system_link::HostPort(to->sin_port));
    if (to->sin_port && (to->sin_port < 1024 || to->sin_port == 3074)) {
      const int target_instance = LocalInstance(address);
      if (target_instance >= 0) {
        nto.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        nto.sin_port = htons(to->sin_port == 3074 ? uint16_t(3074 + target_instance * 16)
                                                  : uint16_t(30000 + target_instance * 16 + to->sin_port));
      }
    }
    // Online: a console on the internet gets the datagram at the port it was last heard from.
    if (online::PublicAddress() && !IsBroadcast(address) && LocalInstance(address) < 0 &&
        !system_link::OnLan(address) &&
        !system_link::IsSelf(address)) {
      uint32_t route_ip = address;
      uint16_t route_port = 0;
      system_link::InternetRoute(address, to->sin_port, &route_ip, &route_port);
      nto.sin_addr.s_addr = htonl(route_ip);
      nto.sin_port = htons(route_port);
    }
#if REX_PLATFORM_SWITCH
    nto.sin_len = sizeof(nto);
#endif
  }

#if REX_PLATFORM_SWITCH
  uint8_t* host = HostBuffer(buf_len);
  std::memcpy(host, buf, buf_len);
  char* data = reinterpret_cast<char*>(host);
#else
  char* data = reinterpret_cast<char*>(buf);
#endif
  const int ret = sendto(native_handle_, data, buf_len, flags, to ? (sockaddr*)&nto : nullptr, to_len);
#if REX_PLATFORM_WIN32
  const int error = ret < 0 ? WSAGetLastError() : 0;
#else
  const int error = ret < 0 ? errno : 0;
#endif
  int copied_bytes = -1;  // the last broadcast copy that went out
  if (to && online::PublicAddress()) {
    const uint32_t sent_to = ntohl(nto.sin_addr.s_addr);
    if (IsBroadcast(sent_to) && to->sin_port && to->sin_port < 1024) {
      // A broadcast stays on the LAN: online, the consoles on the internet that take part get it unicast, from this
      // same socket (so from the title's own port, which their NAT and the router's forwarding expect).
      for (const auto& [ip, port] : system_link::InternetCopies(to->sin_port)) {
        sockaddr_in copy = nto;
        copy.sin_addr.s_addr = htonl(ip);
        copy.sin_port = htons(port);
        const int copied = sendto(native_handle_, data, buf_len, flags, (sockaddr*)&copy, sizeof(copy));
        system_link::TraceInternet(copied >= 0 ? "broadcast copy sent to" : "broadcast copy FAILED to", ip, port,
                                   to->sin_port, reinterpret_cast<const uint8_t*>(data), int(buf_len));
        if (copied >= 0) copied_bytes = copied;
      }
    } else if (!IsBroadcast(sent_to) && !system_link::OnLan(sent_to) && !system_link::IsSelf(sent_to)) {
      system_link::TraceInternet(ret >= 0 ? "sent to" : "send FAILED to", sent_to, ntohs(nto.sin_port), to->sin_port,
                                 reinterpret_cast<const uint8_t*>(data), int(buf_len));
    }
  }
  // BSD and macOS do not loop a LAN broadcast back through other processes'
  // UDP sockets. Fan out the title's discovery datagram to each instance's
  // unique loopback address and port block. This stays entirely local.
  if (to && IsBroadcast(ntohl(nto.sin_addr.s_addr)) && to->sin_port &&
      (to->sin_port < 1024 || to->sin_port == 3074) &&
      std::getenv("BO2_INSTANCE")) {
    for (int instance = 0; instance < 16; ++instance) {
      if (instance == ConfiguredInstance()) continue;
      sockaddr_in copy = nto;
      copy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      copy.sin_port = htons(to->sin_port == 3074 ? uint16_t(3074 + instance * 16)
                                                 : uint16_t(30000 + instance * 16 + to->sin_port));
      const int copied = sendto(native_handle_, data, buf_len, flags, (sockaddr*)&copy, sizeof(copy));
      if (REXCVAR_GET(net_system_link_trace)) {
        REXSYS_INFO("[system link] local broadcast copy for instance {} to 127.0.0.1:{}: {}", instance,
                    ntohs(copy.sin_port), copied >= 0 ? "sent" : std::strerror(errno));
      }
      if (copied >= 0) copied_bytes = copied;
    }
  }
  if (to) system_link::TraceSend(ntohl(nto.sin_addr.s_addr), to->sin_port, ret, error);
  // A broadcast Windows refuses on the title's own socket (WSAEACCES although SO_BROADCAST is set) goes out from a
  // helper socket instead: a System Link search reply's source port does not matter to the searcher.
  if (to && ret < 0 && IsBroadcast(ntohl(nto.sin_addr.s_addr))) {
    if (!broadcast_helper_) {
      const auto helper = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      int on = 1;
      if (helper != decltype(helper)(-1) &&
          setsockopt(helper, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on)) == 0) {
        broadcast_helper_ = uint64_t(helper);
      } else if (helper != decltype(helper)(-1)) {
        rex::net::socket_close(uint64_t(helper));
      }
    }
    if (broadcast_helper_) {
#if REX_PLATFORM_SWITCH
      const int again = sendto(broadcast_helper_, reinterpret_cast<char*>(host), buf_len, flags, (sockaddr*)&nto,
                               sizeof(nto));
#else
      const int again = sendto(broadcast_helper_, reinterpret_cast<char*>(buf), buf_len, flags, (sockaddr*)&nto,
                               sizeof(nto));
#endif
      system_link::TraceSend(ntohl(nto.sin_addr.s_addr), to->sin_port, again, 0);
      if (again >= 0) return again;
    }
  }
  return ret < 0 && copied_bytes >= 0 ? copied_bytes : ret;
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
