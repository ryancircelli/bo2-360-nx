#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstring>
#include <queue>

#include <rex/math.h>
#include <rex/platform.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {

struct XSOCKADDR {
  rex::be<uint16_t> address_family;
  char sa_data[14];
};

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
#if REX_PLATFORM_SWITCH
    sa_len = sizeof(N_XSOCKADDR);
#endif
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, rex::countof(sa_data));
    return *this;
  }

#if REX_PLATFORM_SWITCH
  // Horizon's sockets use the BSD layout (8-bit length, then
  // 8-bit family). With a 16-bit family the kernel read family 0 and every
  // bind failed (BO2: "Failed to bind socket, error 31").
  uint8_t sa_len;
  uint8_t address_family;
#else
  uint16_t address_family;
#endif
  char sa_data[14];
};

struct XSOCKADDR_IN {
  rex::be<uint16_t> sin_family;

  // Always big-endian!
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
#if REX_PLATFORM_SWITCH
    sin_len = sizeof(N_XSOCKADDR_IN);
#endif
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

#if REX_PLATFORM_SWITCH
  // BSD layout on Horizon (see N_XSOCKADDR).
  uint8_t sin_len = sizeof(N_XSOCKADDR_IN);
  uint8_t sin_family;
#else
  uint16_t sin_family;
#endif
  rex::be<uint16_t> sin_port;
  rex::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  // The socket's local address as the guest sees it (its own bound port, not the remapped host port).
  X_STATUS GetName(XSOCKADDR_IN* out_name);
  X_STATUS GetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t* optlen);
  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(N_XSOCKADDR* name, int name_len);
  X_STATUS Bind(N_XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  object_ref<XSocket> Accept(N_XSOCKADDR* name, int* name_len);
  int Shutdown(int how);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
               uint32_t* from_len);
  int SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to, uint32_t to_len);

  struct packet {
    // These values are in network byte order.
    rex::be<uint16_t> src_port;
    rex::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  uint64_t native_handle_ = -1;
  // Switch: until when recv/recvfrom answer EWOULDBLOCK without asking (xsocket.cpp, the idle
  // window); 0 = ask.
  std::atomic<int64_t> idle_until_ns_{0};

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;

  bool broadcast_socket_ = false;
  bool nonblocking_ = false;       // FIONBIO set by the title (RecvFrom: datagrams the runtime takes)
  bool port_registered_ = false;   // in the online relay's table of the title's ports (Bind)
  bool broadcast_native_ = false;  // SO_BROADCAST set by the runtime itself (SendTo)
  uint64_t broadcast_helper_ = 0;  // a socket for broadcasts the title's own one is refused (SendTo)

  std::unique_ptr<rex::thread::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;
};

// System link (LAN play between consoles running this runtime).
namespace system_link {
// The console's LAN address and subnet mask, host byte order; false without a network.
bool LanAddress(uint32_t* ip, uint32_t* mask = nullptr);
// The XNADDR (36 bytes, as the guest stores it) of the console with this LAN address: ina = the address, abEnet made
// from it, so every peer derives the same XNADDR from the same address.
void MakeXnAddr(uint32_t ip, uint8_t* xnaddr);
// Online play: the XNADDR of this console (ip: the address it carries), tagged with its relay session and LAN address
// while the relay runs (online::relay::TagSession). The title compares XNADDRs whole, and consoles behind one public
// address are otherwise the same.
void MakeOwnXnAddr(uint32_t ip, uint8_t* xnaddr);
// The XNADDR of the console the title knows by this address: the console's own, tag included, when it is known to tag
// it; otherwise MakeXnAddr(ip).
void MakeTitleXnAddr(uint32_t ip, uint8_t* xnaddr);
// The address the title reaches the console of an XNADDR at: ina, unless the XNADDR is tagged by another console: then
// its stand-in or public address (TitleIpFor), or its LAN address when it is behind this console's own public address.
uint32_t XnAddrToTitleIp(const uint8_t* xnaddr);
// The relay's word on a console's LAN address (its endpoints' last entry); the console tags its XNADDR.
void NameConsoleLan(uint32_t ip, uint64_t console, uint32_t lan_ip);
// The address this console's own XNADDR carries: its public address while online play is on (online_link.h), its LAN
// address otherwise. false without a network.
bool TitleAddress(uint32_t* ip);
// The address is this console's own (LAN or public), or a console on its LAN (same subnet).
bool IsSelf(uint32_t ip);
bool OnLan(uint32_t ip);
// Titles bind fixed ports below 1024 (BO2: 1000 and 1001), which Horizon refuses to unprivileged processes. Such a
// port moves up by net_system_port_offset on every peer, in binds, destinations and reported sources alike. With
// REX_INSTANCE set, instance i adds i * net_instance_port_stride, so copies on one machine never collide (the System
// Link port 3074 moves the same way). See instance_ports.h.
uint16_t HostPort(uint16_t guest_port);
uint16_t HostPortFor(uint16_t guest_port, int instance);
uint16_t GuestPort(uint16_t host_port);
// Also names the local instance that sent from host_port (-1 when none).
uint16_t GuestPort(uint16_t host_port, int* instance);
// The net_system_link_trace lines (ip in host byte order, the title's port).
void TraceSend(uint32_t ip, uint16_t guest_port, int bytes, int error);
void TraceReceive(uint32_t ip, uint16_t guest_port, int bytes);
// The last other console that sent a datagram from the title's moved port guest_port (a port below 1024) to one of
// this console's title sockets: its address (host byte order) and how long ago, in milliseconds. A console that
// joined a game hears its host this way all through the match. false when none has.
bool LastPeer(uint16_t guest_port, uint32_t* ip, int64_t* age_ms);
// A small UDP socket of the runtime's own, beside the title's sockets, for the port's own messages between consoles:
// bound to port on every address, non-blocking, allowed to broadcast. -1 when it cannot be made; *bound is false when
// the socket works but the port was taken (it then sends from a port the system picks).
int64_t OpenSideSocket(uint16_t port, bool* bound);
void CloseSideSocket(int64_t socket);
// The side socket is shared: opening it again for the same port gives the same socket, and it closes when every
// opener has closed it (the cheat sync and the online relay both use it).
// ip in host byte order. The number of bytes sent, or -1. While online, a console on the internet gets it at the port
// it was last heard from (or that the relay gave for it).
int SideSend(int64_t socket, uint32_t ip, uint16_t port, const void* data, uint32_t size);
// One waiting datagram (its size, its sender in host byte order), or -1 when none is waiting. The online relay's own
// datagrams (STUN answers, punches) are taken out first.
int SideReceive(int64_t socket, void* data, uint32_t capacity, uint32_t* ip, uint16_t* port);

// For the online relay (online_relay.h).
// Sends from the socket bound to host_port: a title socket on a moved port, or the side socket. The bytes sent, or -1
// when no such socket is open.
int SendFromPort(uint16_t host_port, uint32_t ip, uint16_t port, const void* data, uint32_t size);
// Reads what waits on the side socket: the relay's datagrams are taken, the rest is kept for SideReceive.
void PumpSideSocket();
// A console on the internet that, the relay says, sends from host_port for the title's guest_port; console is its relay
// session (0 while unknown), which tells consoles behind one public address apart. Sends to it use that port until the
// console is heard from (and the port it is heard from wins).
void SeedInternetPeer(uint32_t ip, uint16_t guest_port, uint16_t host_port, uint64_t console);
// Something (a game datagram or a punch) arrived from ip:host_port on a title or side socket since
// ForgetInternetPeers: true, with the title port it was for.
bool HeardInternetPeer(uint32_t ip, uint16_t host_port, uint16_t* guest_port = nullptr);
void ForgetInternetPeers();
}  // namespace system_link

}  // namespace rex::system
