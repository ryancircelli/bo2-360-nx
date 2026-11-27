#pragma once
// Socket helpers shared by internet play's two workers (online_link.cpp: UPnP and STUN; online_relay.cpp: the
// matchmaking relay and hole punching). Plain BSD sockets on every platform (Winsock on Windows, libnx's on the
// Switch).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::system::online::net {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

#if REX_PLATFORM_WIN32
using Fd = SOCKET;
using SockLen = int;
inline const Fd kNoSocket = INVALID_SOCKET;
inline void CloseFd(Fd fd) { closesocket(fd); }
inline int LastError() { return WSAGetLastError(); }
inline bool InProgress() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
inline short PollFd(Fd fd, short events, int ms) {
  WSAPOLLFD p{};
  p.fd = fd;
  p.events = events;
  return WSAPoll(&p, 1, std::max(ms, 0)) > 0 ? p.revents : 0;
}
inline bool SetBlocking(Fd fd, bool blocking) {
  u_long mode = blocking ? 0 : 1;
  return ioctlsocket(fd, FIONBIO, &mode) == 0;
}
#else
using Fd = int;
using SockLen = socklen_t;
inline const Fd kNoSocket = -1;
inline void CloseFd(Fd fd) { close(fd); }
inline int LastError() { return errno; }
inline bool InProgress() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
inline short PollFd(Fd fd, short events, int ms) {
  pollfd p{};
  p.fd = fd;
  p.events = events;
  return poll(&p, 1, std::max(ms, 0)) > 0 ? p.revents : 0;
}
inline bool SetBlocking(Fd fd, bool blocking) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags != -1 && fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0) return true;
  int on = blocking ? 0 : 1;
  return ioctl(fd, FIONBIO, &on) == 0;
}
#endif

class Socket {
 public:
  explicit Socket(Fd fd) : fd_(fd) {}
  ~Socket() {
    if (fd_ != kNoSocket) CloseFd(fd_);
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Fd get() const { return fd_; }
  bool valid() const { return fd_ != kNoSocket; }

 private:
  Fd fd_;
};

inline int LeftMs(Clock::time_point deadline) {
  return int(std::max<int64_t>(0, std::chrono::duration_cast<Ms>(deadline - Clock::now()).count()));
}

inline sockaddr_in Address(uint32_t ip, uint16_t port) {
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
  return to;
}

inline std::string Ip(uint32_t ip) {
  char text[16];
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
  return text;
}

// RFC 1918, shared address space (RFC 6598, carrier NAT), loopback, link-local and 0.
inline bool IsPrivate(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 20) == (172u << 4 | 1) || (ip >> 16) == (192u << 8 | 168) ||
         (ip >> 22) == (100u << 2 | 1) || (ip >> 24) == 127 || (ip >> 16) == (169u << 8 | 254) || ip == 0;
}

// An IPv4 address for a host name (or dotted address), host byte order; 0 when it cannot be had.
inline uint32_t Resolve(const std::string& host) {
  in_addr dotted{};
  if (inet_pton(AF_INET, host.c_str(), &dotted) == 1) return ntohl(dotted.s_addr);
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* found = nullptr;
  const int error = getaddrinfo(host.c_str(), nullptr, &hints, &found);
  uint32_t ip = 0;
  if (error == 0) {
    for (const addrinfo* a = found; a && !ip; a = a->ai_next) {
      if (a->ai_family == AF_INET && a->ai_addr) {
        ip = ntohl(reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
      }
    }
    freeaddrinfo(found);
  }
  if (!ip) REXSYS_INFO("[online] {} does not resolve (error {})", host, error);
  return ip;
}

}  // namespace rex::system::online::net
