// See rex/system/online_link.h. Plain BSD sockets on every platform (Winsock on Windows, libnx's on the Switch); no
// third-party code. Every network wait has a deadline, and every step goes to the log (the details with
// net_online_trace), so a router that never answers shows up as a line in the log, not as a menu that hangs.

#include <rex/system/online_link.h>
#include <rex/system/online_relay.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
#include <rex/platform.h>
#include <rex/system/xsocket.h>

#include "online_net.h"

REXCVAR_DEFINE_BOOL(net_online_trace, false, "Kernel",
                    "Online play: log every step of the router set-up (UPnP) and the address check (STUN) in detail, "
                    "and the internet packets the System Link layer forwards");
REXCVAR_DEFINE_BOOL(net_online_upnp, true, "Kernel",
                    "Online play: ask the router (UPnP) to forward the System Link ports to this console");
REXCVAR_DEFINE_INT32(net_online_lease_s, 3600, "Kernel",
                     "Online play: how long each router port forwarding lasts, in seconds; renewed at half that while "
                     "online. Routers that only take permanent forwardings get 0 (removed when online play ends)")
    .range(120, 86400);
REXCVAR_DEFINE_STRING(net_online_stun, "stun.l.google.com:19302,stun.cloudflare.com:3478,stun1.l.google.com:19302,"
                      "stun2.l.google.com:19302,stun3.l.google.com:19302,stun4.l.google.com:19302",
                      "Kernel",
                      "Online play: the STUN servers that tell this console its public address, tried in order. Hole "
                      "punching asks the first two at different addresses from each game port, to tell whether the NAT "
                      "maps every destination to another port");

namespace rex::system::online {
namespace {

using namespace net;

constexpr int kPorts = 3;
constexpr size_t kMaxHttp = 256 * 1024;

bool Trace() { return REXCVAR_GET(net_online_trace); }

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return out;
}

std::string_view TrimView(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    text.remove_suffix(1);
  return text;
}

// The value of an HTTP-style header in a block of lines ("Name: value"), case-insensitive; empty when absent.
std::string Header(std::string_view block, std::string_view name) {
  const std::string want = Lower(name);
  size_t at = 0;
  while (at < block.size()) {
    size_t end = block.find('\n', at);
    if (end == std::string_view::npos) end = block.size();
    const std::string_view line = block.substr(at, end - at);
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos && Lower(TrimView(line.substr(0, colon))) == want) {
      return std::string(TrimView(line.substr(colon + 1)));
    }
    at = end + 1;
  }
  return {};
}

// --- HTTP (the router's description and SOAP control) ----------------------------------------------------------------

struct Url {
  std::string host;
  uint32_t ip = 0;
  uint16_t port = 80;
  std::string path = "/";
};

bool ParseUrl(std::string_view text, Url& url) {
  text = TrimView(text);
  constexpr std::string_view kScheme = "http://";
  if (Lower(text.substr(0, kScheme.size())) != kScheme) return false;
  text.remove_prefix(kScheme.size());
  const size_t slash = text.find('/');
  const std::string_view authority = text.substr(0, slash);
  url.path = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
  const size_t colon = authority.find(':');
  url.host = std::string(authority.substr(0, colon));
  url.port = 80;
  if (colon != std::string_view::npos) {
    const int port = std::atoi(std::string(authority.substr(colon + 1)).c_str());
    if (port <= 0 || port > 65535) return false;
    url.port = uint16_t(port);
  }
  url.ip = url.host.empty() ? 0 : Resolve(url.host);
  return url.ip != 0;
}

struct HttpResult {
  int status = 0;
  std::string body;
  uint32_t local_ip = 0;  // this console's address on the connection (the LAN address the router sees)
  std::string error;
};

std::string Dechunk(std::string_view body) {
  std::string out;
  size_t at = 0;
  while (at < body.size()) {
    const size_t end = body.find("\r\n", at);
    if (end == std::string_view::npos) break;
    const unsigned long size = std::strtoul(std::string(body.substr(at, end - at)).c_str(), nullptr, 16);
    at = end + 2;
    if (size == 0 || at + size > body.size()) break;
    out.append(body.substr(at, size));
    at += size + 2;
  }
  return out;
}

HttpResult Http(const Url& url, const std::string& method, const std::string& headers, const std::string& body,
                int wait_ms) {
  HttpResult result;
  const auto deadline = Clock::now() + Ms(wait_ms);
  Socket s(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (!s.valid() || !SetBlocking(s.get(), false)) {
    result.error = "no socket (error " + std::to_string(LastError()) + ")";
    return result;
  }
  const sockaddr_in to = Address(url.ip, url.port);
  if (connect(s.get(), reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) {
    if (!InProgress()) {
      result.error = "connect refused (error " + std::to_string(LastError()) + ")";
      return result;
    }
    if (!(PollFd(s.get(), POLLOUT, std::min(LeftMs(deadline), 2000)) & POLLOUT)) {
      result.error = "no answer to the connection";
      return result;
    }
    int error = 0;
    SockLen length = sizeof(error);
    if (getsockopt(s.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error != 0) {
      result.error = "connection failed (error " + std::to_string(error) + ")";
      return result;
    }
  }
  sockaddr_in self{};
  SockLen self_length = sizeof(self);
  if (getsockname(s.get(), reinterpret_cast<sockaddr*>(&self), &self_length) == 0) {
    result.local_ip = ntohl(self.sin_addr.s_addr);
  }
  std::string request = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                        "\r\nConnection: close\r\nUser-Agent: bo2-zm-nx UPnP/1.1\r\n" + headers;
  if (method == "POST") request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n" + body;
  size_t sent = 0;
  while (sent < request.size()) {
    if (!(PollFd(s.get(), POLLOUT, LeftMs(deadline)) & POLLOUT)) {
      result.error = "the request could not be sent in time";
      return result;
    }
    const auto n = send(s.get(), request.data() + sent, int(request.size() - sent), 0);
    if (n <= 0) {
      if (n < 0 && InProgress()) continue;
      result.error = "send failed (error " + std::to_string(LastError()) + ")";
      return result;
    }
    sent += size_t(n);
  }
  std::string response;
  size_t header_end = std::string::npos;
  long content_length = -1;
  for (;;) {
    const int left = LeftMs(deadline);
    if (left <= 0) {
      if (response.empty()) result.error = "no answer in time";
      break;
    }
    const short ready = PollFd(s.get(), POLLIN, left);
    if (!(ready & (POLLIN | POLLHUP))) continue;
    char chunk[4096];
    const auto n = recv(s.get(), chunk, int(sizeof(chunk)), 0);
    if (n < 0 && InProgress()) continue;
    if (n <= 0) break;
    response.append(chunk, size_t(n));
    if (response.size() > kMaxHttp) break;
    if (header_end == std::string::npos) {
      header_end = response.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        const std::string length = Header(std::string_view(response).substr(0, header_end), "Content-Length");
        if (!length.empty()) content_length = std::atol(length.c_str());
      }
    }
    // Some routers keep the connection open in spite of "Connection: close".
    if (header_end != std::string::npos && content_length >= 0 &&
        response.size() >= header_end + 4 + size_t(content_length)) {
      break;
    }
  }
  if (header_end == std::string::npos) header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (result.error.empty()) result.error = "no HTTP answer (" + std::to_string(response.size()) + " bytes)";
    return result;
  }
  const std::string_view head = std::string_view(response).substr(0, header_end);
  const size_t space = head.find(' ');
  result.status = space == std::string_view::npos ? 0 : std::atoi(std::string(head.substr(space + 1, 3)).c_str());
  std::string_view content = std::string_view(response).substr(header_end + 4);
  if (Lower(Header(head, "Transfer-Encoding")).find("chunked") != std::string::npos) {
    result.body = Dechunk(content);
  } else {
    if (content_length >= 0 && size_t(content_length) < content.size()) content = content.substr(0, content_length);
    result.body = std::string(content);
  }
  result.error.clear();
  return result;
}

// The text inside the first <name>...</name> (any namespace prefix) in xml[from, to); empty when absent.
std::string Element(std::string_view xml, std::string_view name, size_t from = 0, size_t to = std::string::npos) {
  xml = xml.substr(0, std::min(to, xml.size()));
  size_t at = from;
  while ((at = xml.find('<', at)) != std::string_view::npos) {
    size_t tag = at + 1;
    const size_t close = xml.find('>', tag);
    if (close == std::string_view::npos) return {};
    std::string_view tag_name = xml.substr(tag, close - tag);
    const size_t colon = tag_name.find(':');
    if (colon != std::string_view::npos) tag_name.remove_prefix(colon + 1);
    if (!tag_name.empty() && tag_name.back() == '/') {
      at = close;
      continue;
    }
    const size_t space = tag_name.find_first_of(" \t\r\n");
    if (space != std::string_view::npos) tag_name = tag_name.substr(0, space);
    if (tag_name == name) {
      const size_t end = xml.find("</", close);
      if (end == std::string_view::npos) return {};
      return std::string(TrimView(xml.substr(close + 1, end - close - 1)));
    }
    at = close;
  }
  return {};
}

// --- UPnP ------------------------------------------------------------------------------------------------------------

constexpr const char* kSearchTargets[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
};

// The LOCATION of every device that answered an SSDP search.
std::vector<std::string> DiscoverRouters(const std::atomic<bool>& cancelled) {
  std::vector<std::string> locations;
  Socket s(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (!s.valid()) {
    REXSYS_WARN("[online] UPnP: no UDP socket for the router search (error {})", LastError());
    return locations;
  }
#ifdef IP_MULTICAST_TTL
  const int ttl = 2;
  setsockopt(s.get(), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
#endif
  // The multicast group first. A second round, when nobody answered, also asks the LAN's usual router address
  // directly (UPnP 1.1 devices answer a unicast search), in case multicast does not get out.
  uint32_t lan = 0, mask = 0;
  const uint32_t router_guess = system_link::LanAddress(&lan, &mask) && mask ? (lan & mask) | 1 : 0;
  const sockaddr_in group = Address(0xEFFFFFFAu, 1900);  // 239.255.255.250
  const sockaddr_in direct = Address(router_guess, 1900);
  for (int round = 0; round < 2 && locations.empty() && !cancelled; ++round) {
    for (const char* target : kSearchTargets) {
      const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n") +
                                 "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: " + target + "\r\n\r\n";
      const auto sent = sendto(s.get(), search.data(), int(search.size()), 0,
                               reinterpret_cast<const sockaddr*>(&group), sizeof(group));
      if (sent < 0) REXSYS_WARN("[online] UPnP: the router search could not be sent (error {})", LastError());
      if (round == 1 && router_guess && router_guess != lan) {
        sendto(s.get(), search.data(), int(search.size()), 0, reinterpret_cast<const sockaddr*>(&direct),
               sizeof(direct));
      }
    }
    if (Trace() || round == 1) {
      REXSYS_INFO("[online] UPnP: SSDP search sent ({} targets, round {}{})", std::size(kSearchTargets), round + 1,
                  round == 1 && router_guess ? ", also to " + Ip(router_guess) : std::string());
    }
    const auto deadline = Clock::now() + Ms(1500);
    while (!cancelled) {
      const int left = LeftMs(deadline);
      if (left <= 0 || !(PollFd(s.get(), POLLIN, left) & POLLIN)) break;
      char buffer[2048];
      sockaddr_in from{};
      SockLen from_length = sizeof(from);
      const auto got = recvfrom(s.get(), buffer, int(sizeof(buffer) - 1), 0, reinterpret_cast<sockaddr*>(&from),
                                &from_length);
      if (got <= 0) continue;
      const std::string_view reply(buffer, size_t(got));
      const std::string location = Header(reply, "LOCATION");
      if (Trace()) {
        REXSYS_INFO("[online] UPnP: SSDP reply from {}: ST {}, SERVER {}, LOCATION {}", Ip(ntohl(from.sin_addr.s_addr)),
                    Header(reply, "ST"), Header(reply, "SERVER"), location);
      }
      if (!location.empty() && std::find(locations.begin(), locations.end(), location) == locations.end()) {
        locations.push_back(location);
      }
    }
  }
  return locations;
}

struct Gateway {
  Url control;
  std::string service;  // its serviceType
  std::string location;
  uint32_t internal_ip = 0;
};

struct SoapResult {
  bool ok = false;
  int status = 0;
  uint32_t local_ip = 0;  // this console's address on the connection to the router
  int upnp_error = 0;  // <errorCode> of a SOAP fault
  std::string body;
  std::string error;  // transport
};

SoapResult Soap(const Gateway& gateway, const char* action, const std::string& arguments, int wait_ms) {
  const std::string body = std::string("<?xml version=\"1.0\"?>\r\n") +
                           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                           action + " xmlns:u=\"" + gateway.service + "\">" + arguments + "</u:" + action +
                           "></s:Body></s:Envelope>\r\n";
  const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gateway.service + "#" +
                              action + "\"\r\n";
  const HttpResult http = Http(gateway.control, "POST", headers, body, wait_ms);
  SoapResult result;
  result.status = http.status;
  result.local_ip = http.local_ip;
  result.body = http.body;
  result.error = http.error;
  result.ok = http.error.empty() && http.status == 200;
  if (!result.ok) {
    const std::string code = Element(http.body, "errorCode");
    result.upnp_error = code.empty() ? 0 : std::atoi(code.c_str());
  }
  if (Trace()) {
    REXSYS_INFO("[online] UPnP: {} -> HTTP {}{}{}{}", action, http.status,
                result.upnp_error ? ", UPnP error " + std::to_string(result.upnp_error) : std::string(),
                http.error.empty() ? "" : ", ", http.error);
  }
  return result;
}

std::string Mapping(uint16_t port) {
  return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
         "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
}

// The device descriptions' WAN connection services, best first, with their control URLs made absolute.
std::vector<Gateway> Services(const std::string& location) {
  std::vector<Gateway> found;
  Url url;
  if (!ParseUrl(location, url)) {
    REXSYS_INFO("[online] UPnP: the router's description is at a location this client cannot read: {}", location);
    return found;
  }
  const HttpResult http = Http(url, "GET", "", "", 4000);
  if (!http.error.empty() || http.status != 200) {
    REXSYS_INFO("[online] UPnP: the router's description {} could not be read: HTTP {} {}", location, http.status,
                http.error);
    return found;
  }
  if (Trace()) REXSYS_INFO("[online] UPnP: device description {} ({} bytes)", location, http.body.size());
  std::string base = Element(http.body, "URLBase");
  if (base.empty()) base = "http://" + url.host + ":" + std::to_string(url.port);
  while (!base.empty() && base.back() == '/') base.pop_back();
  const std::string_view xml = http.body;
  size_t at = 0;
  struct Ranked {
    int rank;
    Gateway gateway;
  };
  std::vector<Ranked> ranked;
  while ((at = xml.find("<service>", at)) != std::string_view::npos) {
    const size_t end = xml.find("</service>", at);
    if (end == std::string_view::npos) break;
    const std::string type = Element(xml, "serviceType", at, end);
    const std::string control = Element(xml, "controlURL", at, end);
    at = end;
    int rank = -1;
    if (type.find(":service:WANIPConnection:2") != std::string::npos) rank = 0;
    else if (type.find(":service:WANIPConnection:1") != std::string::npos) rank = 1;
    else if (type.find(":service:WANPPPConnection:1") != std::string::npos) rank = 2;
    if (rank < 0 || control.empty()) continue;
    Gateway gateway;
    gateway.service = type;
    gateway.location = location;
    const std::string absolute = Lower(control).rfind("http://", 0) == 0
                                     ? control
                                     : base + (control.front() == '/' ? "" : "/") + control;
    if (!ParseUrl(absolute, gateway.control)) continue;
    if (Trace()) REXSYS_INFO("[online] UPnP: service {} at {}", type, absolute);
    ranked.push_back({rank, std::move(gateway)});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.rank < b.rank; });
  for (auto& r : ranked) found.push_back(std::move(r.gateway));
  if (found.empty()) REXSYS_INFO("[online] UPnP: {} has no WAN connection service", location);
  return found;
}

// --- STUN ------------------------------------------------------------------------------------------------------------

struct StunAnswer {
  uint32_t ip = 0;
  uint16_t mapped_port = 0, local_port = 0;
  std::string server;
};

bool StunOnce(const std::string& host, uint16_t port, StunAnswer& answer, const std::atomic<bool>& cancelled) {
  const uint32_t server = Resolve(host);
  if (!server) return false;
  Socket s(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (!s.valid()) return false;
  sockaddr_in any = Address(0, 0);
  bind(s.get(), reinterpret_cast<const sockaddr*>(&any), sizeof(any));
  static std::mt19937 rng{std::random_device{}() ^ uint32_t(Clock::now().time_since_epoch().count())};
  uint8_t request[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
  for (int i = 8; i < 20; ++i) request[i] = uint8_t(rng());
  const sockaddr_in to = Address(server, port);
  for (int attempt = 0; attempt < 2 && !cancelled; ++attempt) {
    if (sendto(s.get(), reinterpret_cast<const char*>(request), int(sizeof(request)), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to)) < 0) {
      REXSYS_INFO("[online] STUN: the request to {} could not be sent (error {})", host, LastError());
      return false;
    }
    sockaddr_in self{};
    SockLen self_length = sizeof(self);
    if (getsockname(s.get(), reinterpret_cast<sockaddr*>(&self), &self_length) == 0) {
      answer.local_port = ntohs(self.sin_port);
    }
    const auto deadline = Clock::now() + Ms(1000);
    while (!cancelled) {
      const int left = LeftMs(deadline);
      if (left <= 0 || !(PollFd(s.get(), POLLIN, left) & POLLIN)) break;
      uint8_t reply[512];
      sockaddr_in from{};
      SockLen from_length = sizeof(from);
      const auto got = recvfrom(s.get(), reinterpret_cast<char*>(reply), int(sizeof(reply)), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_length);
      if (got < 20 || reply[0] != 0x01 || reply[1] != 0x01 || std::memcmp(reply + 4, request + 4, 16) != 0) continue;
      const size_t length = std::min<size_t>(size_t(got) - 20, size_t(reply[2]) << 8 | reply[3]);
      uint32_t mapped = 0, xor_mapped = 0;
      uint16_t mapped_port = 0, xor_port = 0;
      for (size_t at = 20; at + 4 <= 20 + length;) {
        const uint16_t type = uint16_t(reply[at] << 8 | reply[at + 1]);
        const uint16_t size = uint16_t(reply[at + 2] << 8 | reply[at + 3]);
        const uint8_t* value = reply + at + 4;
        if (at + 4 + size > 20 + length) break;
        if ((type == 0x0001 || type == 0x0020) && size >= 8 && value[1] == 0x01) {
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
        at += 4 + ((size + 3u) & ~3u);
      }
      answer.ip = xor_mapped ? xor_mapped : mapped;
      answer.mapped_port = xor_mapped ? xor_port : mapped_port;
      if (answer.ip) {
        answer.server = host + ":" + std::to_string(port);
        return true;
      }
    }
    if (Trace()) REXSYS_INFO("[online] STUN: no answer from {} ({}), attempt {}", host, Ip(server), attempt + 1);
  }
  return false;
}

bool Stun(StunAnswer& answer, const std::atomic<bool>& cancelled) {
  const std::string list = REXCVAR_GET(net_online_stun);
  size_t at = 0;
  while (at < list.size() && !cancelled) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    const std::string entry(TrimView(std::string_view(list).substr(at, end - at)));
    at = end + 1;
    if (entry.empty()) continue;
    const size_t colon = entry.rfind(':');
    const std::string host = entry.substr(0, colon);
    const int port = colon == std::string::npos ? 3478 : std::atoi(entry.c_str() + colon + 1);
    if (Trace()) REXSYS_INFO("[online] STUN: asking {}", entry);
    if (port > 0 && port < 65536 && StunOnce(host, uint16_t(port), answer, cancelled)) return true;
  }
  return false;
}

// --- The worker ------------------------------------------------------------------------------------------------------

enum class Ask { kNothing, kStart, kStop };

struct Worker {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  bool quit = false;
  bool abandon = false;  // static destruction: the thread ends without talking to the router (the leases expire)
  uint64_t job = 0, done = 0;  // posted / taken
  Ask ask = Ask::kNothing;
  Role role = Role::kNone;
  uint32_t target = 0;
  Report report;
  std::atomic<bool> cancelled{false};  // the job being run was replaced (read by the network waits)
  // The worker's own:
  bool have_gateway = false;
  Gateway gateway;
  std::vector<uint16_t> mapped;
  int lease = 0;
  Clock::time_point renew_at = Clock::time_point::max();

  ~Worker() {
    {
      std::lock_guard lock(mutex);
      quit = true;
      abandon = true;
    }
    cancelled = true;
    wake.notify_all();
    if (thread.joinable()) thread.join();
  }
};
Worker g_worker;
std::atomic<uint32_t> g_public{0};
std::atomic<uint32_t> g_target{0};
std::atomic<uint64_t> g_version{0};

void Publish(const Report& report) {
  std::lock_guard lock(g_worker.mutex);
  g_worker.report = report;
  g_worker.report.version = g_version.fetch_add(1, std::memory_order_relaxed) + 1;
}

// The forwarding request for one port. 0 done; otherwise the router's UPnP error, or -1 for no answer.
int AddMapping(uint16_t port, int lease, uint32_t* conflict) {
  const auto& gw = g_worker.gateway;
  const std::string arguments = Mapping(port) + "<NewInternalPort>" + std::to_string(port) +
                                "</NewInternalPort><NewInternalClient>" + Ip(gw.internal_ip) +
                                "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>bo2-zm-nx " +
                                std::to_string(port) + "</NewPortMappingDescription><NewLeaseDuration>" +
                                std::to_string(lease) + "</NewLeaseDuration>";
  const SoapResult result = Soap(gw, "AddPortMapping", arguments, 4000);
  if (result.ok) return 0;
  if (result.upnp_error == 718 && conflict) {
    // ConflictInMappingEntry: someone has the port. Fine when it is this console (an earlier session's forwarding).
    const SoapResult entry = Soap(gw, "GetSpecificPortMappingEntry", Mapping(port), 4000);
    const std::string client = Element(entry.body, "NewInternalClient");
    in_addr parsed{};
    *conflict = !client.empty() && inet_pton(AF_INET, client.c_str(), &parsed) == 1 ? ntohl(parsed.s_addr) : 0;
    if (entry.ok && *conflict == gw.internal_ip) return 0;
  }
  return result.upnp_error ? result.upnp_error : -1;
}

void RemoveMappings(int wait_ms) {
  if (!g_worker.have_gateway) return;
  for (const uint16_t port : g_worker.mapped) {
    const SoapResult result = Soap(g_worker.gateway, "DeletePortMapping", Mapping(port), wait_ms);
    REXSYS_INFO("[online] UPnP: forwarding of UDP {} removed: {}", port,
                result.ok ? "done" : "HTTP " + std::to_string(result.status) +
                                         (result.upnp_error ? ", error " + std::to_string(result.upnp_error) : "") +
                                         (result.error.empty() ? "" : ", " + result.error));
  }
  g_worker.mapped.clear();
  g_worker.renew_at = Clock::time_point::max();
}

// Finds the router and forwards the ports. Fills the report's UPnP fields.
void Upnp(Report& report) {
  const auto& cancelled = g_worker.cancelled;
  const uint16_t first = FirstPort();
  report.first_port = first;
  report.port_count = kPorts;
  if (!REXCVAR_GET(net_online_upnp)) {
    REXSYS_INFO("[online] UPnP: off (net_online_upnp)");
    return;
  }
  const std::vector<std::string> locations = DiscoverRouters(cancelled);
  REXSYS_INFO("[online] UPnP: {} device(s) answered the router search{}", locations.size(),
              locations.empty() ? " (is UPnP enabled on the router?)" : "");
  for (const std::string& location : locations) {
    if (cancelled) return;
    REXSYS_INFO("[online] UPnP: device {}", location);
    for (Gateway& candidate : Services(location)) {
      if (cancelled) return;
      g_worker.gateway = candidate;
      const SoapResult external = Soap(g_worker.gateway, "GetExternalIPAddress", "", 4000);
      // The LAN address the router sees this console at: the local end of the connection to it.
      g_worker.gateway.internal_ip = external.local_ip ? external.local_ip : report.lan_ip;
      const std::string text = Element(external.body, "NewExternalIPAddress");
      in_addr parsed{};
      const uint32_t external_ip =
          external.ok && inet_pton(AF_INET, text.c_str(), &parsed) == 1 ? ntohl(parsed.s_addr) : 0;
      REXSYS_INFO("[online] UPnP: control URL http://{}:{}{} ({}), external address {}, this console {}",
                  candidate.control.host, candidate.control.port, candidate.control.path, candidate.service,
                  external_ip ? Ip(external_ip) : "not given", Ip(g_worker.gateway.internal_ip));
      if (!external.ok && external.status == 0) continue;  // the service does not answer at all
      g_worker.have_gateway = true;
      report.router_found = true;
      report.router_ip = candidate.control.ip;
      report.router_external_ip = external_ip;
      int lease = REXCVAR_GET(net_online_lease_s);
      int mapped = 0;
      for (int i = 0; i < kPorts && !cancelled; ++i) {
        const uint16_t port = uint16_t(first + i);
        uint32_t conflict = 0;
        int code = AddMapping(port, lease, &conflict);
        if (code == 725 && lease != 0) {  // OnlyPermanentLeasesSupported
          REXSYS_INFO("[online] UPnP: the router takes only permanent forwardings; they are removed when online play "
                      "ends");
          lease = 0;
          code = AddMapping(port, lease, &conflict);
        }
        if (code == 0) {
          ++mapped;
          g_worker.mapped.push_back(port);
        } else {
          report.upnp_error = code;
          if (code == 718) report.conflict_ip = conflict;
        }
        REXSYS_INFO("[online] UPnP: AddPortMapping UDP {} -> {}:{} (lease {} s): {}", port,
                    Ip(g_worker.gateway.internal_ip), port, lease,
                    code == 0    ? std::string("done")
                    : code == 718 ? "error 718, the port is forwarded to " + (conflict ? Ip(conflict) : "another device")
                    : code < 0    ? std::string("no answer")
                                  : "error " + std::to_string(code));
      }
      g_worker.lease = lease;
      report.ports_mapped = mapped;
      if (mapped > 0 || external_ip) return;  // this router it is
    }
  }
}

void Prepare(uint64_t job, Role role, uint32_t target) {
  Report report;
  report.state = State::kPreparing;
  report.role = role;
  report.target_ip = target;
  const auto still = [&] {
    std::lock_guard lock(g_worker.mutex);
    return !g_worker.quit && g_worker.job == job;
  };
  uint32_t lan = 0;
  if (!system_link::LanAddress(&lan)) {
    report.state = State::kFailed;
    report.failure = Failure::kNoNetwork;
    REXSYS_WARN("[online] no network: online play needs the console on the internet");
    Publish(report);
    return;
  }
  report.lan_ip = lan;
  REXSYS_INFO("[online] preparing to {}{}; this console is {} on the LAN", role == Role::kHost ? "host" : "join",
              role == Role::kJoin ? " " + Ip(target) : std::string(), Ip(lan));
  Publish(report);

  Upnp(report);
  if (!still()) return;
  REXSYS_INFO("[online] UPnP: {} of {} ports forwarded{}", report.ports_mapped, kPorts,
              report.router_found ? "" : " (no UPnP router found)");

  StunAnswer stun;
  report.stun_ok = Stun(stun, g_worker.cancelled);
  if (!still()) return;
  if (report.stun_ok) {
    report.stun_local_port = stun.local_port;
    report.stun_mapped_port = stun.mapped_port;
    REXSYS_INFO("[online] STUN: {} says this console is {} (port {} seen as {}{})", stun.server, Ip(stun.ip),
                stun.local_port, stun.mapped_port,
                stun.local_port == stun.mapped_port ? ", the NAT keeps ports" : ", the NAT rewrites ports");
  } else {
    REXSYS_WARN("[online] STUN: no server answered ({})", REXCVAR_GET(net_online_stun));
  }

  // The address peers reach: STUN's, which is what the internet sees; the router's when STUN gave none.
  uint32_t address = report.stun_ok ? stun.ip : 0;
  if (!address && report.router_external_ip && !IsPrivate(report.router_external_ip)) {
    address = report.router_external_ip;
    REXSYS_INFO("[online] using the router's external address {}", Ip(address));
  }
  if (report.router_external_ip && (IsPrivate(report.router_external_ip) ||
                                    (report.stun_ok && report.router_external_ip != stun.ip))) {
    report.double_nat = true;
    REXSYS_WARN("[online] the router's external address {} is not the public one ({}): another router or the "
                "provider's NAT (CGNAT) sits in between, and the forwarding may not reach this console",
                Ip(report.router_external_ip), address ? Ip(address) : std::string("unknown"));
  }
  if (!address) {
    report.state = State::kFailed;
    report.failure = Failure::kNoPublicAddress;
  } else if (role == Role::kJoin && target == address) {
    report.state = State::kFailed;
    report.failure = Failure::kSameNetwork;
    report.public_ip = address;
    REXSYS_WARN("[online] the code is this network's own public address {}: use System Link on the same network",
                Ip(address));
  }
  if (report.state == State::kFailed) {
    RemoveMappings(3000);
    Publish(report);
    return;
  }
  report.public_ip = address;
  report.state = State::kReady;
  {
    std::lock_guard lock(g_worker.mutex);
    if (g_worker.quit || g_worker.job != job) return;
    g_public.store(address, std::memory_order_release);
    g_target.store(role == Role::kJoin ? target : 0, std::memory_order_release);
    // UPnP may have said yes and still not forward (some routers do), and a joiner's carrier NAT cannot be asked at
    // all: the relay and hole punching run in every case. Started under the lock, after the job check, so a Post that
    // comes now stops it again (Post stops the relay after moving the job on).
    relay::Start(role == Role::kHost, role == Role::kHost ? address : target, address);
  }
  if (!g_worker.mapped.empty() && g_worker.lease > 0) {
    g_worker.renew_at = Clock::now() + std::chrono::seconds(g_worker.lease / 2);
  }
  REXSYS_INFO("[online] ready to {}: this console's online address is {}{}", role == Role::kHost ? "host" : "join",
              Ip(address), role == Role::kJoin ? ", the host is " + Ip(target) : std::string());
  Publish(report);
}

void Renew() {
  g_worker.renew_at = Clock::time_point::max();
  if (!g_worker.have_gateway || g_worker.mapped.empty()) return;
  int renewed = 0;
  for (const uint16_t port : g_worker.mapped) {
    uint32_t conflict = 0;
    const int code = AddMapping(port, g_worker.lease, &conflict);
    renewed += code == 0;
    if (code != 0) REXSYS_WARN("[online] UPnP: renewing UDP {} failed (error {})", port, code);
  }
  REXSYS_INFO("[online] UPnP: {} of {} forwardings renewed for {} s", renewed, g_worker.mapped.size(), g_worker.lease);
  g_worker.renew_at = Clock::now() + std::chrono::seconds(std::max(60, g_worker.lease / 2));
}

void Main() {
#if REX_PLATFORM_WIN32
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  std::unique_lock lock(g_worker.mutex);
  for (;;) {
    const auto posted = [] { return g_worker.quit || g_worker.job != g_worker.done; };
    if (g_worker.renew_at == Clock::time_point::max()) {
      g_worker.wake.wait(lock, posted);
    } else {
      g_worker.wake.wait_until(lock, g_worker.renew_at, posted);
    }
    if (g_worker.quit) break;
    if (g_worker.job != g_worker.done) {
      const uint64_t job = g_worker.done = g_worker.job;
      const Ask ask = g_worker.ask;
      const Role role = g_worker.role;
      const uint32_t target = g_worker.target;
      g_worker.cancelled = false;  // under the lock: a newer post sets it again under the lock
      lock.unlock();
      RemoveMappings(3000);
      g_worker.have_gateway = false;
      if (ask == Ask::kStart) {
        Prepare(job, role, target);
      } else {
        Report idle;
        Publish(idle);
      }
      lock.lock();
    } else if (Clock::now() >= g_worker.renew_at) {
      lock.unlock();
      Renew();
      lock.lock();
    }
  }
  const bool abandon = g_worker.abandon;
  lock.unlock();
  if (!abandon) RemoveMappings(1500);
#if REX_PLATFORM_WIN32
  WSACleanup();
#endif
}

void Post(Ask ask, Role role, uint32_t target) {
  g_public.store(0, std::memory_order_release);
  g_target.store(0, std::memory_order_release);
  {
    std::lock_guard lock(g_worker.mutex);
    if (g_worker.quit) return;
    ++g_worker.job;
    g_worker.ask = ask;
    g_worker.role = role;
    g_worker.target = target;
    g_worker.report = {};
    g_worker.report.state = ask == Ask::kStart ? State::kPreparing : State::kIdle;
    g_worker.report.role = ask == Ask::kStart ? role : Role::kNone;
    g_worker.report.target_ip = target;
    g_worker.report.version = g_version.fetch_add(1, std::memory_order_relaxed) + 1;
    g_worker.cancelled = true;  // the job running now, if any, stops at its next wait
    if (!g_worker.thread.joinable()) g_worker.thread = std::thread(Main);
  }
  relay::Stop();
  g_worker.wake.notify_all();
}

}  // namespace

void Start(Role role, uint32_t target_ip) {
  REXSYS_INFO("[online] {} asked for{}", role == Role::kHost ? "hosting" : "joining",
              role == Role::kJoin ? " " + Ip(target_ip) : std::string());
  Post(Ask::kStart, role, target_ip);
}

void Stop() {
  if (!g_public.load(std::memory_order_acquire) && Current().state == State::kIdle) return;
  REXSYS_INFO("[online] online play ends");
  Post(Ask::kStop, Role::kNone, 0);
}

void Shutdown() {
  g_public.store(0, std::memory_order_release);
  g_target.store(0, std::memory_order_release);
  relay::Shutdown();
  {
    std::lock_guard lock(g_worker.mutex);
    g_worker.quit = true;
    g_worker.cancelled = true;
  }
  g_worker.wake.notify_all();
  if (g_worker.thread.joinable()) g_worker.thread.join();
}

Report Current() {
  Report report;
  {
    std::lock_guard lock(g_worker.mutex);
    report = g_worker.report;
  }
  if (report.state == State::kReady) report.relay = relay::Current();
  return report;
}

// Both counters only grow, so their sum changes whenever either does.
uint64_t Version() { return g_version.load(std::memory_order_relaxed) + relay::Version(); }
uint32_t PublicAddress() { return g_public.load(std::memory_order_acquire); }
uint32_t Target() { return g_target.load(std::memory_order_acquire); }
uint16_t FirstPort() { return system_link::HostPort(rex::Game().first_link_port); }
int PortCount() { return kPorts; }

}  // namespace rex::system::online
