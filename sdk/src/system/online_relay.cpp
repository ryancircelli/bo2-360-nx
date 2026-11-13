// See rex/system/online_relay.h (the flow) and rex/system/relay_wire.h (the formats). One thread of its own, woken
// every 50 ms while something is under way, every 250 ms in between, and asleep without a session; every socket it owns is non-blocking, and every wait has a
// deadline, so a broker that never answers shows up as a log line, not as a stall.

#include <rex/system/online_relay.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/game_info.h>
#include <rex/logging.h>
#include <rex/system/join_code.h>
#include <rex/system/relay_wire.h>
#include <rex/system/xsocket.h>

#include "online_net.h"

REXCVAR_DEFINE_STRING(net_online_brokers,
                      "broker.hivemq.com:1883,broker.emqx.io:1883,test.mosquitto.org:1883,broker.mqtt.cool:1883",
                      "Kernel",
                      "Online play: the public MQTT brokers (host:port, comma-separated) through which the consoles of a "
                      "join code find each other for UDP hole punching. All that answer are used at once; empty turns "
                      "the relay off");
REXCVAR_DEFINE_BOOL(net_online_punch, true, "Kernel",
                    "Online play: swap UDP endpoints through the relay (net_online_brokers) and open both NATs with "
                    "hole punching, for routers that do not forward the ports");
REXCVAR_DECLARE(bool, net_online_trace);
REXCVAR_DECLARE(std::string, net_online_stun);

namespace rex::system::online {
namespace {

using namespace net;
namespace wire = relay_wire;
namespace mqtt = relay_wire::mqtt;

constexpr int kPorts = 3;
// The title's link ports from rex::Game().first_link_port: VDP, UDP, then the side channel (ours).
// BO2: 1000, 1001, 1002.
uint16_t GuestPort(int i) { return uint16_t(rex::Game().first_link_port + i); }
constexpr char kPunchMagic[8] = {'B', 'O', '2', 'Z', 'M', 'P', 'C', 'H'};
constexpr int kPunchSize = 18;  // magic, session (8), the title port it is for (2)
constexpr uint16_t kKeepAliveS = 30;
constexpr auto kTick = Ms(50);
constexpr auto kIdleTick = Ms(250);
constexpr auto kPunchEvery = Ms(250);
constexpr auto kPunchFor = std::chrono::seconds(10);
constexpr auto kJoinPostEvery = std::chrono::seconds(2);
constexpr auto kJoinPostFor = std::chrono::minutes(3);
constexpr auto kStunRefresh = std::chrono::seconds(30);

bool Trace() { return REXCVAR_GET(net_online_trace); }

std::mt19937_64& Rng() {
  static std::mt19937_64 rng{(uint64_t(std::random_device{}()) << 32) ^ uint64_t(Clock::now().time_since_epoch().count())};
  return rng;
}

// ---- STUN on the game's own ports --------------------------------------------------------------------------------
// The requests go out from the relay's thread; the answers come back on the game's sockets (or the side socket) and
// reach Classify on whatever thread reads them.

struct StunTx {
  std::array<uint8_t, 12> id{};
  int port = 0;    // index into the guest ports (GuestPort)
  int server = 0;  // 0 or 1
  Clock::time_point sent{};
};

struct PortMap {
  std::array<uint32_t, 2> ip{};    // per STUN server
  std::array<uint16_t, 2> port{};
};

std::mutex g_stun_mutex;
std::vector<StunTx> g_tx;                 // under g_stun_mutex
std::array<PortMap, kPorts> g_maps{};     // under g_stun_mutex
std::atomic<int> g_tx_count{0};

// The mapped address of a Binding success response; false when it has none.
bool StunMapped(const uint8_t* reply, int size, uint32_t& ip, uint16_t& port) {
  const size_t length = std::min<size_t>(size_t(size) - 20, size_t(reply[2]) << 8 | reply[3]);
  uint32_t mapped = 0, xor_mapped = 0;
  uint16_t mapped_port = 0, xor_port = 0;
  for (size_t at = 20; at + 4 <= 20 + length;) {
    const uint16_t type = uint16_t(reply[at] << 8 | reply[at + 1]);
    const uint16_t attr = uint16_t(reply[at + 2] << 8 | reply[at + 3]);
    const uint8_t* value = reply + at + 4;
    if (at + 4 + attr > 20 + length) break;
    if ((type == 0x0001 || type == 0x0020) && attr >= 8 && value[1] == 0x01) {
      const uint16_t p = uint16_t(value[2] << 8 | value[3]);
      const uint32_t a = uint32_t(value[4]) << 24 | uint32_t(value[5]) << 16 | uint32_t(value[6]) << 8 | value[7];
      if (type == 0x0020) {
        xor_port = p ^ 0x2112;
        xor_mapped = a ^ 0x2112A442u;
      } else {
        mapped_port = p;
        mapped = a;
      }
    }
    at += 4 + ((attr + 3u) & ~3u);
  }
  ip = xor_mapped ? xor_mapped : mapped;
  port = xor_mapped ? xor_port : mapped_port;
  return ip != 0;
}

}  // namespace

Datagram Classify(uint16_t local_port, uint32_t ip, uint16_t port, const uint8_t* data, int size) {
  if (size >= kPunchSize && std::memcmp(data, kPunchMagic, sizeof(kPunchMagic)) == 0) return Datagram::kPunch;
  // A STUN response: success (0x0101) or error (0x0111), with the RFC 5389 magic cookie. No game datagram starts this
  // way by chance often enough to matter (48 fixed bits), and late answers are taken too.
  if (size < 20 || data[0] != 0x01 || (data[1] != 0x01 && data[1] != 0x11) || data[4] != 0x21 || data[5] != 0x12 ||
      data[6] != 0xA4 || data[7] != 0x42) {
    return Datagram::kTitle;
  }
  if (g_tx_count.load(std::memory_order_relaxed) == 0) return Datagram::kStun;
  uint32_t mapped_ip = 0;
  uint16_t mapped_port = 0;
  const bool mapped = data[1] == 0x01 && StunMapped(data, size, mapped_ip, mapped_port);
  std::lock_guard lock(g_stun_mutex);
  for (auto it = g_tx.begin(); it != g_tx.end(); ++it) {
    if (std::memcmp(it->id.data(), data + 8, 12) != 0) continue;
    if (mapped) {
      auto& map = g_maps[size_t(it->port)];
      map.ip[size_t(it->server)] = mapped_ip;
      map.port[size_t(it->server)] = mapped_port;
    }
    if (Trace()) {
      REXSYS_INFO("[online] relay: STUN answer on port {} from {}:{}: {}", local_port, Ip(ip), port,
                  mapped ? Ip(mapped_ip) + ":" + std::to_string(mapped_port) : std::string("an error"));
    }
    g_tx.erase(it);
    g_tx_count.store(int(g_tx.size()), std::memory_order_relaxed);
    break;
  }
  return Datagram::kStun;
}

namespace relay {
namespace {

// ---- Brokers ------------------------------------------------------------------------------------------------------

struct Broker {
  enum class State { kDown, kConnecting, kHandshake, kUp };
  std::string name;  // host:port, as configured
  std::string host;
  uint16_t port = 1883;
  uint32_t ip = 0;
  Fd fd = kNoSocket;
  State state = State::kDown;
  std::vector<uint8_t> in, out;
  Clock::time_point retry_at{}, deadline{}, last_rx{}, last_tx{};
  int failures = 0;
  bool failed_last = false;
  bool ever_up = false;
  uint64_t published = 0;  // host: the endpoint signature this broker has
  bool posted = false;     // this console's endpoints went out on it (for the menu)
  int found = 0;           // consoles first found through it (for the menu)
  uint16_t next_id = 1;
};

struct Peer {
  uint64_t session = 0;  // 0: the host as the code gives it, before its message arrives
  uint32_t ip = 0;
  int count = 0;
  std::array<wire::Endpoint, wire::kMaxEndpoints> endpoints{};
  Clock::time_point punch_until{}, next_punch{};
  int punches = 0;
  bool heard = false;
};

struct Session {
  bool active = false;
  bool host = false;
  uint32_t code_ip = 0, public_ip = 0;
  std::string code;  // plain form
  std::string host_topic, join_topic, client_id;
  uint64_t session = 0;
  Clock::time_point started{}, next_stun{}, stun_ready_at{}, next_join_post{};
  int stun_round = 0;
  bool endpoints_ready = false;
  uint64_t signature = 0;  // of the endpoints posted
  bool join_done = false;
  bool symmetric_logged = false;
  std::array<uint32_t, 2> stun_ip{};
  std::array<uint16_t, 2> stun_port{};
  int stun_servers = 0;
  int64_t side = -1;  // the side socket, held open while the session runs
  std::vector<Peer> peers;
  std::vector<Broker> brokers;
};

Session g_session;  // the relay thread's only (defined before g_control, so it outlives the thread at exit)

struct Control {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  bool quit = false;
  bool abandon = false;  // static destruction: end without talking to the brokers
  uint64_t job = 0;
  bool want = false, host = false;
  uint32_t code_ip = 0, public_ip = 0;
  Status status;
  ~Control() {
    {
      std::lock_guard lock(mutex);
      quit = true;
      abandon = true;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
  }
};
Control g_control;
std::atomic<uint64_t> g_version{0};
// This console's session once a broker has taken it (TagSession); 0 otherwise.
std::atomic<uint64_t> g_tag_session{0};

std::vector<Broker> ConfiguredBrokers() {
  std::vector<Broker> brokers;
  const std::string list = REXCVAR_GET(net_online_brokers);
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    std::string entry = list.substr(at, end - at);
    at = end + 1;
    entry.erase(0, entry.find_first_not_of(" \t"));
    entry.erase(entry.find_last_not_of(" \t") + 1);
    if (entry.empty()) continue;
    Broker b;
    b.name = entry;
    const size_t colon = entry.rfind(':');
    b.host = entry.substr(0, colon);
    if (colon != std::string::npos) {
      const int port = std::atoi(entry.c_str() + colon + 1);
      if (port <= 0 || port > 65535) continue;
      b.port = uint16_t(port);
    }
    if (!b.host.empty() && brokers.size() < 8) brokers.push_back(std::move(b));
  }
  return brokers;
}

void Drop(Broker& b, const std::string& why, bool failure) {
  if (b.fd != kNoSocket) CloseFd(b.fd);
  b.fd = kNoSocket;
  const bool was_up = b.state == Broker::State::kUp;
  b.state = Broker::State::kDown;
  b.in.clear();
  b.out.clear();
  b.published = 0;
  b.posted = false;
  if (failure) {
    b.failed_last = true;
    ++b.failures;
    const int wait_s = std::min(60, 2 << std::min(b.failures, 5));
    b.retry_at = Clock::now() + std::chrono::seconds(wait_s);
    if (b.failures >= 3) b.ip = 0;  // resolve again next time
    if (was_up || b.failures == 1 || Trace()) {
      REXSYS_INFO("[online] relay: broker {} {}: {}; trying again in {} s", b.name, was_up ? "lost" : "not reached",
                  why, wait_s);
    }
  }
}

void Queue(Broker& b, const std::vector<uint8_t>& packet) {
  b.out.insert(b.out.end(), packet.begin(), packet.end());
  b.last_tx = Clock::now();
}

// Sends what it can of the queue without waiting.
bool Flush(Broker& b) {
  while (!b.out.empty()) {
    const auto n = send(b.fd, reinterpret_cast<const char*>(b.out.data()), int(b.out.size()), 0);
    if (n < 0) {
      if (InProgress()) return true;
      Drop(b, "send failed (error " + std::to_string(LastError()) + ")", true);
      return false;
    }
    if (n == 0) return true;
    b.out.erase(b.out.begin(), b.out.begin() + n);
  }
  return true;
}

void Connect(Broker& b) {
  if (!b.ip) b.ip = Resolve(b.host);  // blocking, on this thread only
  if (!b.ip) {
    Drop(b, "the name does not resolve", true);
    return;
  }
  b.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (b.fd == kNoSocket || !SetBlocking(b.fd, false)) {
    Drop(b, "no socket (error " + std::to_string(LastError()) + ")", true);
    return;
  }
  const sockaddr_in to = Address(b.ip, b.port);
  if (connect(b.fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0 && !InProgress()) {
    Drop(b, "connect refused (error " + std::to_string(LastError()) + ")", true);
    return;
  }
  b.state = Broker::State::kConnecting;
  b.deadline = Clock::now() + std::chrono::seconds(6);
  if (Trace()) REXSYS_INFO("[online] relay: connecting to broker {} ({})", b.name, Ip(b.ip));
}

// ---- The session --------------------------------------------------------------------------------------------------

uint64_t Signature(const wire::Message& m) {
  uint64_t h = m.public_ip;
  for (int i = 0; i < m.count; ++i) {
    const auto& e = m.endpoints[size_t(i)];
    h = wire::Mix(h ^ (uint64_t(e.local_port) << 48 | uint64_t(e.ip) << 16 | e.port) ^ (e.from_stun ? 1ull << 63 : 0));
  }
  return h;
}

// This console's endpoints as known now: STUN's where it answered, the public address and the same port otherwise.
// A last entry with local port 0 carries its LAN address (consoles behind the same router reach each other there,
// and the XNADDR tag carries it too: system_link::MakeOwnXnAddr). Consoles that predate it skip an entry without a
// port.
wire::Message OwnMessage() {
  wire::Message m;
  m.role = g_session.host ? wire::kHost : wire::kJoiner;
  m.session = g_session.session;
  m.public_ip = g_session.public_ip;
  m.count = kPorts;
  std::lock_guard lock(g_stun_mutex);
  for (int i = 0; i < kPorts; ++i) {
    auto& e = m.endpoints[size_t(i)];
    e.local_port = system_link::HostPort(GuestPort(i));
    const PortMap& map = g_maps[size_t(i)];
    const int s = map.ip[0] ? 0 : (map.ip[1] ? 1 : -1);
    if (s >= 0) {
      e.ip = map.ip[size_t(s)];
      e.port = map.port[size_t(s)];
      e.from_stun = true;
    } else {
      e.ip = g_session.public_ip;
      e.port = e.local_port;
    }
  }
  uint32_t lan = 0;
  if (system_link::LanAddress(&lan) && lan && m.count < wire::kMaxEndpoints) {
    auto& e = m.endpoints[size_t(m.count++)];
    e.local_port = 0;
    e.ip = lan;
    e.port = 0;
  }
  return m;
}

std::string EndpointsText(const wire::Message& m) {
  std::string text;
  for (int i = 0; i < m.count; ++i) {
    const auto& e = m.endpoints[size_t(i)];
    if (!text.empty()) text += ", ";
    if (!e.local_port) {
      text += "LAN " + Ip(e.ip);
      continue;
    }
    text += std::to_string(e.local_port) + " -> " + Ip(e.ip) + ":" + std::to_string(e.port) + (e.from_stun ? "" : "?");
  }
  return text;
}

void PublishOwn(Broker& b, const wire::Message& m, bool retain) {
  const auto payload = wire::Encode(m, g_session.code, Rng()());
  Queue(b, mqtt::Publish(g_session.host ? g_session.host_topic : g_session.join_topic, payload.data(), payload.size(),
                         retain));
  b.posted = true;
  if (Trace()) {
    REXSYS_INFO("[online] relay: posted this console's endpoints on {} ({})", b.name, EndpointsText(m));
  }
}

void StartPunching(Peer& p) {
  const auto now = Clock::now();
  if (p.punch_until < now) p.next_punch = now;
  p.punch_until = now + kPunchFor;
}

// The peer's endpoint for one of this console's ports (by index), or nullptr.
const wire::Endpoint* PeerEndpoint(const Peer& p, int index) {
  for (int i = 0; i < p.count; ++i) {
    if (system_link::GuestPort(p.endpoints[size_t(i)].local_port) == GuestPort(index)) return &p.endpoints[size_t(i)];
  }
  return nullptr;
}

void Seed(const Peer& p) {
  for (int i = 0; i < p.count; ++i) {
    const auto& e = p.endpoints[size_t(i)];
    if (!e.local_port) {
      if (p.session && p.ip) system_link::NameConsoleLan(p.ip, p.session, e.ip);
    } else if (e.ip && e.port) {
      system_link::SeedInternetPeer(e.ip, system_link::GuestPort(e.local_port), e.port, p.session);
    }
  }
}

wire::Message PeerMessage(const Peer& p) {
  wire::Message m;
  m.public_ip = p.ip;
  m.count = p.count;
  m.endpoints = p.endpoints;
  return m;
}

void Found(const wire::Message& m, const std::string& broker) {
  if (m.session == g_session.session) return;  // this console's own
  if (g_session.host != (m.role == wire::kJoiner)) return;
  Peer* peer = nullptr;
  if (!g_session.host && !g_session.peers.empty()) {
    peer = &g_session.peers.front();  // a joiner has one peer, the host (first the guess from the code)
  } else {
    for (auto& p : g_session.peers) {
      if (p.session == m.session) peer = &p;
    }
  }
  if (!peer) {
    if (g_session.peers.size() >= 8) return;
    g_session.peers.push_back({});
    peer = &g_session.peers.back();
  }
  const bool changed = peer->session != m.session || Signature(PeerMessage(*peer)) != Signature(m);
  peer->session = m.session;
  peer->ip = m.public_ip;
  peer->count = m.count;
  peer->endpoints = m.endpoints;
  if (changed) {
    for (auto& b : g_session.brokers) {
      if (b.name == broker) ++b.found;
    }
    REXSYS_INFO("[online] relay: {} {} found through broker {}: {}", g_session.host ? "player" : "host", Ip(m.public_ip),
                broker, EndpointsText(m));
  } else if (Trace()) {
    REXSYS_INFO("[online] relay: {} {} posted again on {}", g_session.host ? "player" : "host", Ip(m.public_ip), broker);
  }
  Seed(*peer);
  StartPunching(*peer);
}

void OnPublish(const mqtt::Parsed& packet, const Broker& b) {
  std::string_view topic;
  const uint8_t* payload = nullptr;
  size_t size = 0;
  if (!mqtt::ReadPublish(packet, topic, payload, size)) return;
  const std::string_view want = g_session.host ? g_session.join_topic : g_session.host_topic;
  if (topic != want) return;
  if (size == 0) {
    if (Trace()) REXSYS_INFO("[online] relay: the host's post on {} is cleared", b.name);
    return;
  }
  wire::Message m;
  if (!wire::Decode(payload, size, g_session.code, m)) {
    if (Trace()) REXSYS_INFO("[online] relay: an unreadable {}-byte post on {} ignored", size, b.name);
    return;
  }
  Found(m, b.name);
}

void Receive(Broker& b) {
  for (;;) {
    uint8_t chunk[2048];
    const auto n = recv(b.fd, reinterpret_cast<char*>(chunk), int(sizeof(chunk)), 0);
    if (n < 0) {
      if (InProgress()) break;
      Drop(b, "receive failed (error " + std::to_string(LastError()) + ")", true);
      return;
    }
    if (n == 0) {
      Drop(b, "the broker closed the connection", true);
      return;
    }
    b.in.insert(b.in.end(), chunk, chunk + n);
    b.last_rx = Clock::now();
    if (b.in.size() > 256 * 1024) {
      Drop(b, "too much data", true);
      return;
    }
  }
  size_t at = 0;
  while (at < b.in.size()) {
    mqtt::Parsed packet;
    const long used = mqtt::Parse(b.in.data() + at, b.in.size() - at, packet);
    if (used < 0) {
      Drop(b, "a malformed packet", true);
      return;
    }
    if (used == 0) break;
    at += size_t(used);
    if (packet.type == mqtt::kConnack) {
      if (b.state != Broker::State::kHandshake) continue;
      const int code = packet.body_size >= 2 ? packet.body[1] : -1;
      if (code != 0) {
        Drop(b, "the broker refused the connection (code " + std::to_string(code) + ")", true);
        return;
      }
      b.state = Broker::State::kUp;
      b.failures = 0;
      b.failed_last = false;
      REXSYS_INFO("[online] relay: broker {} ({}) connected{}", b.name, Ip(b.ip), b.ever_up ? " again" : "");
      b.ever_up = true;
      g_tag_session.store(g_session.session, std::memory_order_relaxed);
      Queue(b, mqtt::Subscribe(b.next_id++, g_session.host ? g_session.join_topic : g_session.host_topic));
    } else if (packet.type == mqtt::kPublish) {
      OnPublish(packet, b);
    }
    // SUBACK and PINGRESP need nothing beyond last_rx.
  }
  b.in.erase(b.in.begin(), b.in.begin() + long(at));
}

void TickBroker(Broker& b, const wire::Message& own, uint64_t signature) {
  const auto now = Clock::now();
  switch (b.state) {
    case Broker::State::kDown:
      if (now >= b.retry_at) Connect(b);
      return;
    case Broker::State::kConnecting: {
      const short ready = PollFd(b.fd, POLLOUT, 0);
      if (!(ready & (POLLOUT | POLLERR | POLLHUP))) {
        if (now >= b.deadline) Drop(b, "no answer to the connection", true);
        return;
      }
      int error = 0;
      SockLen length = sizeof(error);
      if (getsockopt(b.fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error != 0) {
        Drop(b, "connection failed (error " + std::to_string(error) + ")", true);
        return;
      }
      b.state = Broker::State::kHandshake;
      b.deadline = now + std::chrono::seconds(6);
      b.last_rx = now;
      Queue(b, mqtt::Connect(g_session.client_id, kKeepAliveS,
                             g_session.host ? std::string_view(g_session.host_topic) : std::string_view()));
      break;
    }
    case Broker::State::kHandshake:
    case Broker::State::kUp:
      break;
  }
  if (!Flush(b)) return;
  Receive(b);
  if (b.state == Broker::State::kHandshake && now >= b.deadline) {
    Drop(b, "no answer to MQTT CONNECT", true);
    return;
  }
  if (b.state != Broker::State::kUp) return;
  if (g_session.host && g_session.endpoints_ready && b.published != signature) {
    PublishOwn(b, own, true);
    b.published = signature;
  }
  if (now - b.last_rx > std::chrono::seconds(kKeepAliveS + 15)) {
    Drop(b, "silent for " + std::to_string(kKeepAliveS + 15) + " s", true);
    return;
  }
  if (now - b.last_tx >= std::chrono::seconds(kKeepAliveS * 2 / 3)) Queue(b, mqtt::PingReq());
  Flush(b);
}

// ---- STUN rounds ----------------------------------------------------------------------------------------------------

void ChooseStunServers() {
  g_session.stun_servers = 0;
  const std::string list = REXCVAR_GET(net_online_stun);
  size_t at = 0;
  while (at < list.size() && g_session.stun_servers < 2) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    std::string entry = list.substr(at, end - at);
    at = end + 1;
    entry.erase(0, entry.find_first_not_of(" \t"));
    entry.erase(entry.find_last_not_of(" \t") + 1);
    if (entry.empty()) continue;
    const size_t colon = entry.rfind(':');
    const int port = colon == std::string::npos ? 3478 : std::atoi(entry.c_str() + colon + 1);
    const uint32_t ip = port > 0 && port < 65536 ? Resolve(entry.substr(0, colon)) : 0;
    if (!ip || (g_session.stun_servers == 1 && ip == g_session.stun_ip[0])) continue;
    g_session.stun_ip[size_t(g_session.stun_servers)] = ip;
    g_session.stun_port[size_t(g_session.stun_servers)] = uint16_t(port);
    ++g_session.stun_servers;
  }
  REXSYS_INFO("[online] relay: {} STUN server(s) for the game's ports{}", g_session.stun_servers,
              g_session.stun_servers ? " (" + Ip(g_session.stun_ip[0]) + ")" : std::string());
}

int KnownPorts() {
  std::lock_guard lock(g_stun_mutex);
  int known = 0;
  for (const auto& map : g_maps) known += map.ip[0] || map.ip[1];
  return known;
}

bool Symmetric() {
  std::lock_guard lock(g_stun_mutex);
  for (const auto& map : g_maps) {
    if (map.ip[0] && map.ip[1] && (map.ip[0] != map.ip[1] || map.port[0] != map.port[1])) return true;
  }
  return false;
}

void StunRound() {
  const auto now = Clock::now();
  int sent = 0;
  std::string missing;
  {
    std::lock_guard lock(g_stun_mutex);
    // Forget requests nobody answered.
    g_tx.erase(std::remove_if(g_tx.begin(), g_tx.end(), [&](const StunTx& t) { return now - t.sent > Ms(10000); }),
               g_tx.end());
  }
  for (int i = 0; i < kPorts; ++i) {
    for (int s = 0; s < g_session.stun_servers; ++s) {
      StunTx tx;
      tx.port = i;
      tx.server = s;
      tx.sent = now;
      uint8_t request[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
      for (int b = 0; b < 12; ++b) request[8 + b] = tx.id[size_t(b)] = uint8_t(Rng()());
      {
        std::lock_guard lock(g_stun_mutex);
        g_tx.push_back(tx);
        g_tx_count.store(int(g_tx.size()), std::memory_order_relaxed);
      }
      const int n = system_link::SendFromPort(system_link::HostPort(GuestPort(i)), g_session.stun_ip[size_t(s)],
                                              g_session.stun_port[size_t(s)], request, sizeof(request));
      if (n < 0) {
        std::lock_guard lock(g_stun_mutex);
        g_tx.pop_back();
        g_tx_count.store(int(g_tx.size()), std::memory_order_relaxed);
        if (s == 0) missing += (missing.empty() ? "" : ", ") + std::to_string(system_link::HostPort(GuestPort(i)));
        break;
      }
      ++sent;
    }
  }
  ++g_session.stun_round;
  if (Trace() || (g_session.stun_round == 1 && !missing.empty())) {
    REXSYS_INFO("[online] relay: STUN round {}: {} request(s) from the game's ports{}", g_session.stun_round, sent,
                missing.empty() ? std::string() : "; no socket on port " + missing + " yet");
  }
}

void TickStun() {
  if (!g_session.stun_servers) {
    g_session.endpoints_ready = true;
    return;
  }
  const auto now = Clock::now();
  // The side socket is read by the cheat sync only during a match: its STUN answers are fetched here meanwhile.
  bool side_waiting = false;
  {
    std::lock_guard lock(g_stun_mutex);
    for (const auto& t : g_tx) side_waiting |= t.port == kPorts - 1;
  }
  if (side_waiting) system_link::PumpSideSocket();
  if (now >= g_session.next_stun) {
    StunRound();
    // Quick retries while ports are missing, then a refresh now and then (it also keeps the NAT's mappings alive).
    const bool all = KnownPorts() == kPorts;
    g_session.next_stun = now + (all || g_session.stun_round >= 4 ? std::chrono::duration_cast<Ms>(kStunRefresh) : Ms(1000));
  }
  if (!g_session.endpoints_ready && (KnownPorts() == kPorts || now >= g_session.stun_ready_at)) {
    g_session.endpoints_ready = true;
    const wire::Message own = OwnMessage();
    REXSYS_INFO("[online] relay: this console's endpoints: {} ({} of {} seen by STUN)", EndpointsText(own),
                KnownPorts(), kPorts);
  }
  if (!g_session.symmetric_logged && Symmetric()) {
    g_session.symmetric_logged = true;
    REXSYS_WARN("[online] relay: two STUN servers see different public ports for the same port: this network's NAT "
                "maps each destination separately (symmetric), and hole punching will likely fail; forwarding the "
                "ports on the router, or the other player hosting, may work instead");
  }
}

// ---- Punching -----------------------------------------------------------------------------------------------------

void TickPeers() {
  const auto now = Clock::now();
  for (auto& p : g_session.peers) {
    if (!p.heard && p.ip) {
      uint16_t guest = 0, from = 0;
      for (int i = 0; i < p.count && !p.heard; ++i) {
        from = p.endpoints[size_t(i)].port;
        p.heard = system_link::HeardInternetPeer(p.endpoints[size_t(i)].ip, from, &guest);
      }
      if (p.heard) {
        REXSYS_INFO("[online] relay: first datagram from {} ({}:{}, title port {}) after {} punch(es)",
                    g_session.host ? "the player" : "the host", Ip(p.ip), from, guest, p.punches);
      }
    }
    if (now >= p.punch_until || now < p.next_punch) continue;
    p.next_punch = now + kPunchEvery;
    Seed(p);  // keeps the published endpoints fresh for the socket layer while punching
    std::string sent;
    for (int i = 0; i < kPorts; ++i) {
      const wire::Endpoint* e = PeerEndpoint(p, i);
      if (!e || !e->ip || !e->port) continue;
      uint8_t punch[kPunchSize];
      std::memcpy(punch, kPunchMagic, sizeof(kPunchMagic));
      for (int b = 0; b < 8; ++b) punch[8 + b] = uint8_t(g_session.session >> (56 - 8 * b));
      punch[16] = uint8_t(GuestPort(i) >> 8);
      punch[17] = uint8_t(GuestPort(i));
      const uint16_t from = system_link::HostPort(GuestPort(i));
      const int n = system_link::SendFromPort(from, e->ip, e->port, punch, sizeof(punch));
      if (Trace() && p.punches == 0) {
        sent += (sent.empty() ? "" : ", ") + std::to_string(from) + " -> " + Ip(e->ip) + ":" + std::to_string(e->port) +
                (n < 0 ? " (failed)" : "");
      }
    }
    if (Trace() && p.punches == 0) REXSYS_INFO("[online] relay: punching {}", sent);
    ++p.punches;
  }
}

// ---- Session start and end --------------------------------------------------------------------------------------

void Begin(bool host, uint32_t code_ip, uint32_t public_ip) {
  g_tag_session.store(0, std::memory_order_relaxed);
  g_session = {};
  g_session.active = true;
  g_session.host = host;
  g_session.code_ip = code_ip;
  g_session.public_ip = public_ip;
  g_session.code = join_code::Plain(code_ip);
  g_session.host_topic = wire::HostTopic(g_session.code);
  g_session.join_topic = wire::JoinTopic(g_session.code);
  g_session.session = Rng()() | 1;
  g_session.client_id = "bzn" + wire::Hex64(Rng()());
  g_session.started = Clock::now();
  g_session.stun_ready_at = g_session.started + std::chrono::seconds(3);
  g_session.next_stun = g_session.started;
  g_session.next_join_post = g_session.started;
  {
    std::lock_guard lock(g_stun_mutex);
    g_tx.clear();
    g_maps = {};
    g_tx_count.store(0, std::memory_order_relaxed);
  }
  system_link::ForgetInternetPeers();
  bool bound = false;
  g_session.side = system_link::OpenSideSocket(system_link::HostPort(GuestPort(kPorts - 1)), &bound);
  g_session.brokers = ConfiguredBrokers();
  REXSYS_INFO("[online] relay: {} {} through {} broker(s) on topic {}", host ? "hosting" : "joining",
              join_code::Make(code_ip), g_session.brokers.size(), wire::Topic(g_session.code));
  if (!host) {
    // Until the host's post arrives: its public address and the same ports, so the knocks start at once.
    Peer guess;
    guess.ip = code_ip;
    guess.count = kPorts;
    for (int i = 0; i < kPorts; ++i) {
      auto& e = guess.endpoints[size_t(i)];
      e.local_port = system_link::HostPort(GuestPort(i));
      e.ip = code_ip;
      e.port = e.local_port;
    }
    StartPunching(guess);
    g_session.peers.push_back(guess);
  }
  ChooseStunServers();
}

void End(bool talk) {
  if (!g_session.active) return;
  for (auto& b : g_session.brokers) {
    if (b.state == Broker::State::kUp && talk) {
      if (g_session.host) Queue(b, mqtt::Publish(g_session.host_topic, nullptr, 0, true));  // clears the retained post
      Queue(b, mqtt::Disconnect());
      Flush(b);
    }
    if (b.fd != kNoSocket) CloseFd(b.fd);
    b.fd = kNoSocket;
  }
  if (g_session.side >= 0) system_link::CloseSideSocket(g_session.side);
  REXSYS_INFO("[online] relay: session ended{}", g_session.host && talk ? " (the host's post is cleared)" : "");
  {
    std::lock_guard lock(g_stun_mutex);
    g_tx.clear();
    g_tx_count.store(0, std::memory_order_relaxed);
  }
  g_tag_session.store(0, std::memory_order_relaxed);
  g_session = {};
}

Status Measure() {
  Status s;
  s.active = g_session.active;
  if (!s.active) return s;
  s.brokers = int(g_session.brokers.size());
  for (size_t i = 0; i < g_session.brokers.size(); ++i) {
    const auto& b = g_session.brokers[i];
    s.brokers_up += b.state == Broker::State::kUp;
    s.brokers_failed += b.state != Broker::State::kUp && b.failed_last;  // until it comes up again
    if (i >= s.broker_list.size()) continue;
    auto& entry = s.broker_list[i];
    entry.name = b.name;
    entry.state = b.state == Broker::State::kUp ? BrokerStatus::kWorking
                  : b.failed_last               ? BrokerStatus::kDown
                                                : BrokerStatus::kChecking;
    entry.posted = b.posted && b.state == Broker::State::kUp;
    entry.found = b.found;
  }
  s.ports = KnownPorts();
  s.port_count = kPorts;
  s.symmetric = Symmetric();
  for (const auto& p : g_session.peers) {
    if (!p.session) continue;
    ++s.peers;
    s.peers_heard += p.heard;
  }
  return s;
}

bool Same(const Status& a, const Status& b) {
  return a.active == b.active && a.broker_list == b.broker_list && a.brokers == b.brokers && a.brokers_up == b.brokers_up &&
         a.brokers_failed == b.brokers_failed && a.ports == b.ports && a.port_count == b.port_count &&
         a.symmetric == b.symmetric && a.peers == b.peers && a.peers_heard == b.peers_heard;
}

void Report() {
  const Status now = Measure();
  std::lock_guard lock(g_control.mutex);
  if (Same(now, g_control.status)) return;
  g_control.status = now;
  g_version.fetch_add(1, std::memory_order_relaxed);
}

// One round of everything; the time until the next. Quick while something is under way (STUN, a connection, posting,
// punching), slow otherwise: a host keeps its relay running through a whole match, and on the Switch every look at a
// broker socket is a call to the socket service.
Ms Tick() {
  TickStun();
  const wire::Message own = OwnMessage();
  const uint64_t signature = Signature(own);
  if (g_session.endpoints_ready && g_session.signature != signature) {
    if (g_session.signature && Trace()) REXSYS_INFO("[online] relay: endpoints changed: {}", EndpointsText(own));
    g_session.signature = signature;
  }
  for (auto& b : g_session.brokers) TickBroker(b, own, signature);
  const auto now = Clock::now();
  if (!g_session.host && !g_session.join_done) {
    bool heard = false;
    for (const auto& p : g_session.peers) heard |= p.heard;
    if (heard || now - g_session.started > kJoinPostFor) {
      g_session.join_done = true;
      REXSYS_INFO("[online] relay: {}; this console stops posting", heard ? "the host answers directly"
                                                                          : "no answer from the host in 3 minutes");
    } else if (g_session.endpoints_ready && now >= g_session.next_join_post) {
      g_session.next_join_post = now + kJoinPostEvery;
      for (auto& b : g_session.brokers) {
        if (b.state != Broker::State::kUp) continue;
        PublishOwn(b, own, false);
        Flush(b);
      }
    }
  }
  TickPeers();
  Report();
  bool busy = !g_session.endpoints_ready || (!g_session.host && !g_session.join_done);
  for (const auto& b : g_session.brokers) {
    busy |= b.state == Broker::State::kConnecting || b.state == Broker::State::kHandshake || !b.out.empty();
  }
  for (const auto& p : g_session.peers) busy |= now < p.punch_until;
  {
    std::lock_guard lock(g_stun_mutex);
    busy |= !g_tx.empty() && now - g_tx.back().sent < Ms(2000);
  }
  return busy ? kTick : kIdleTick;
}

void Main() {
#if REX_PLATFORM_WIN32
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  uint64_t done = 0;
  std::unique_lock lock(g_control.mutex);
  for (;;) {
    if (g_control.quit) break;
    if (g_control.job != done) {
      done = g_control.job;
      const bool want = g_control.want, host = g_control.host;
      const uint32_t code_ip = g_control.code_ip, public_ip = g_control.public_ip;
      lock.unlock();
      End(true);
      if (want) Begin(host, code_ip, public_ip);
      Report();
      lock.lock();
      continue;
    }
    if (!g_session.active) {
      g_control.wake.wait(lock, [&] { return g_control.quit || g_control.job != done; });
      continue;
    }
    lock.unlock();
    const Ms wait = Tick();
    lock.lock();
    g_control.wake.wait_for(lock, wait, [&] { return g_control.quit || g_control.job != done; });
  }
  const bool abandon = g_control.abandon;
  lock.unlock();
  if (abandon) {
    // Static destruction: other modules may be gone already. The sockets close; the brokers notice by themselves.
    for (auto& b : g_session.brokers) {
      if (b.fd != kNoSocket) CloseFd(b.fd);
    }
  } else {
    End(true);
  }
#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}

void Post(bool want, bool host, uint32_t code_ip, uint32_t public_ip) {
  {
    std::lock_guard lock(g_control.mutex);
    if (g_control.quit) return;
    if (!want && !g_control.want && !g_control.thread.joinable()) return;
    ++g_control.job;
    g_control.want = want;
    g_control.host = host;
    g_control.code_ip = code_ip;
    g_control.public_ip = public_ip;
    if (!g_control.thread.joinable()) g_control.thread = std::thread(Main);
  }
  g_control.wake.notify_all();
}

}  // namespace

void Start(bool host, uint32_t code_ip, uint32_t public_ip) {
  if (!REXCVAR_GET(net_online_punch) || REXCVAR_GET(net_online_brokers).empty()) {
    REXSYS_INFO("[online] relay: off (net_online_punch, net_online_brokers)");
    return;
  }
  Post(true, host, code_ip, public_ip);
}

void Stop() { Post(false, false, 0, 0); }

void Shutdown() {
  {
    std::lock_guard lock(g_control.mutex);
    g_control.quit = true;
  }
  g_control.wake.notify_all();
  if (g_control.thread.joinable()) g_control.thread.join();
}

Status Current() {
  std::lock_guard lock(g_control.mutex);
  return g_control.status;
}

uint64_t Version() { return g_version.load(std::memory_order_relaxed); }

uint64_t TagSession() { return g_tag_session.load(std::memory_order_relaxed); }

}  // namespace relay
}  // namespace rex::system::online
