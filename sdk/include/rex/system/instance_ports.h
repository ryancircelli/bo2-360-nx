// Same-machine multi-instance port mapping (REX_INSTANCE 0..15), for any title.
// A title's fixed ports (below 1024, and the System Link port 3074) are moved to host ports. Instance i gets its own
// block `stride` ports wide, so copies on one machine never collide and a receiver can tell which instance sent a
// datagram from the source port alone. Pure functions: the callers pass the cvars in, which keeps this unit-testable.
#pragma once

#include <cstdint>

namespace rex::system::instance_ports {

inline constexpr uint16_t kSystemLinkPort = 3074;  // Xbox 360 System Link (XNet), not a title's own choice
inline constexpr int kMaxInstances = 16;

// Does this guest port move at all (a title's fixed low port, or the System Link port)?
inline bool Moved(uint16_t guest) { return guest && (guest < 1024 || guest == kSystemLinkPort); }

// Host port for a guest port. instance < 0 = no instance (plain offset). Unmoved ports and overflow past 65535 stay as is.
inline uint16_t Host(uint16_t guest, int instance, int offset, int stride) {
  const int block = instance < 0 ? 0 : instance * stride;
  int host = guest;
  if (guest && guest < 1024) {
    host = guest + (offset || instance >= 0 ? offset + block : 0);
  } else if (guest == kSystemLinkPort && instance >= 0) {
    host = guest + block;
  }
  return host > 0 && host <= 0xFFFF ? uint16_t(host) : guest;
}

// Inverse. With an instance configured every instance's block is searched (datagrams from the other local copies) and
// *instance names the sender; otherwise only the plain offset applies and *instance is -1.
inline uint16_t Guest(uint16_t host, bool instanced, int offset, int stride, int* instance = nullptr) {
  if (instance) *instance = -1;
  for (int i = 0; instanced && i < kMaxInstances; ++i) {
    const int first = offset + i * stride;
    if (host >= first && host < first + 1024 && host - first != 0) {
      if (instance) *instance = i;
      return uint16_t(host - first);
    }
    if (host == kSystemLinkPort + i * stride) {
      if (instance) *instance = i;
      return kSystemLinkPort;
    }
  }
  return offset && host >= offset && host < offset + 1024 ? uint16_t(host - offset) : host;
}

}  // namespace rex::system::instance_ports
