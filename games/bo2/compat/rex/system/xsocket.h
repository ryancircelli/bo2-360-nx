// SPDX-License-Identifier: BSD-3-Clause
// v0.10 rex/system/xsocket.h subset on v0.11: System Link peer lookup (no System Link layer yet: no peer).
#pragma once
#include <cstdint>
namespace rex::system::system_link {
template <class P, class A>
inline bool LastPeer(uint16_t, P*, A*) { return false; }
}  // namespace rex::system::system_link
