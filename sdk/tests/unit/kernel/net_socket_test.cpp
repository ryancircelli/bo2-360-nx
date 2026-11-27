#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xevent.h>
#include "kernel/xam/net_socket.h"

namespace {
using namespace rex;
using namespace rex::system;
using namespace rex::kernel::xam;

template<class T> auto Map(T* value) { return ppc_ptr_t<T>::from_host(value); }

// Run kernel calls on an actual host-backed guest thread with bound kernel TLS.
template<class F> void WithKernel(F function) {
  Runtime runtime(std::filesystem::current_path());
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  std::atomic<bool> passed{false};
  auto thread = object_ref<XHostThread>(new XHostThread(runtime.kernel_state(), 128 * 1024,
      X_CREATE_SUSPENDED, [&] { passed = function(runtime); return 0; }));
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  REQUIRE(thread->Resume() == X_STATUS_SUCCESS);
  REQUIRE(thread->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS);
  REQUIRE(passed.load());
}

bool DatagramRegression(Runtime& runtime) {
  auto receiver = object_ref<XSocket>(new XSocket(runtime.kernel_state()));
  auto sender = object_ref<XSocket>(new XSocket(runtime.kernel_state()));
  if (receiver->Initialize(XSocket::AddressFamily::X_AF_INET, XSocket::Type::X_SOCK_DGRAM,
                           XSocket::Protocol::X_IPPROTO_UDP) != X_STATUS_SUCCESS ||
      sender->Initialize(XSocket::AddressFamily::X_AF_INET, XSocket::Type::X_SOCK_DGRAM,
                         XSocket::Protocol::X_IPPROTO_UDP) != X_STATUS_SUCCESS) return false;
  N_XSOCKADDR_IN bind{};
  bind.sin_family = 2;
  bind.sin_addr = 0x7F000001;
  if (receiver->Bind(&bind, sizeof(bind)) != X_STATUS_SUCCESS) return false;
  XSOCKADDR_IN destination{};
  if (receiver->GetName(&destination) != X_STATUS_SUCCESS) return false;
  const auto allocation = runtime.memory()->SystemHeapAlloc(64);
  auto data = runtime.memory()->TranslateVirtual(allocation);
  std::memcpy(data, "abcDEF", 6);
  XWSABUF sends[2]{{3, allocation}, {3, allocation + 3}};
  XWSABUF receives[2]{{2, allocation + 16}, {8, allocation + 18}};
  auto send_event = object_ref<XEvent>(new XEvent(runtime.kernel_state()));
  auto recv_event = object_ref<XEvent>(new XEvent(runtime.kernel_state()));
  send_event->Initialize(true, false);
  recv_event->Initialize(true, true);
  XWSAOVERLAPPED send_over{}, recv_over{};
  send_over.event_handle = send_event->handle();
  recv_over.event_handle = recv_event->handle();
  be_u32 sent{0}, received{0xDEADBEEF}, flags{0}, length{sizeof(XSOCKADDR_IN)};
  XSOCKADDR_IN source{};
  bool ok = true;
#if !REX_PLATFORM_WIN32
  // Post before there is data. The ninth and tenth arguments must really be the
  // overlapped pointer and callback, following the from-length eighth argument.
  ok &= NetDll_WSARecvFrom_entry(1, receiver->handle(), Map(receives), 2, Map(&received),
      Map(&flags), Map(&source), Map(&length), Map(&recv_over), nullptr) == ~0u;
  ok &= XThread::GetLastError() == 997 && received == 0xDEADBEEF;
  int64_t zero = 0;
  ok &= recv_event->Wait(0, 0, 0, &zero) == X_STATUS_TIMEOUT;
#endif
  ok &= NetDll_WSASendTo_entry(1, sender->handle(), Map(sends), 2, Map(&sent), 0,
      Map(&destination), sizeof(destination), Map(&send_over), nullptr) == 0;
  ok &= sent == 6 && send_over.internal == 0 && send_over.internal_high == 6;
  int64_t timeout = -20000000; // two seconds, bounded failure
  ok &= send_event->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS;
#if !REX_PLATFORM_WIN32
  ok &= recv_event->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS;
  ok &= NetDll_WSAGetOverlappedResult_entry(1, receiver->handle(), Map(&recv_over),
      Map(&received), 0, Map(&flags)) == 1;
#else
  ok &= NetDll_WSARecvFrom_entry(1, receiver->handle(), Map(receives), 2, Map(&received),
      Map(&flags), Map(&source), Map(&length), nullptr, nullptr) == 0;
#endif
  ok &= received == 6 && std::memcmp(data + 16, "abcDEF", 6) == 0;
  ok &= length == sizeof(XSOCKADDR_IN) && source.sin_addr == 0x7F000001;
#if !REX_PLATFORM_WIN32
  // Cancellation must publish failure and wake an event even with no traffic.
  ok &= NetDll_WSARecvFrom_entry(1, receiver->handle(), Map(receives), 2, Map(&received),
      Map(&flags), nullptr, nullptr, Map(&recv_over), nullptr) == ~0u;
  ok &= XThread::GetLastError() == 997;
  ok &= NetDll_WSACancelOverlappedIO_entry(1, receiver->handle()) == 0;
  ok &= recv_event->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS;
  ok &= NetDll_WSAGetOverlappedResult_entry(1, receiver->handle(), Map(&recv_over),
      Map(&received), 0, Map(&flags)) == 0 && XThread::GetLastError() == 995;
#endif
  // Connected sends use the same gather path; immediate receives must scatter
  // without splitting a single datagram into separate recv calls.
  N_XSOCKADDR_IN connected(&destination);
  ok &= sender->Connect(reinterpret_cast<N_XSOCKADDR*>(&connected), sizeof(connected))
        == X_STATUS_SUCCESS;
  ok &= NetDll_WSASend_entry(1, sender->handle(), Map(sends), 2, Map(&sent), 0,
                             nullptr, nullptr) == 0 && sent == 6;
  ok &= NetDll_WSARecv_entry(1, receiver->handle(), Map(receives), 2, Map(&received),
                             Map(&flags), nullptr, nullptr) == 0 && received == 6;
  ok &= std::memcmp(data + 16, "abcDEF", 6) == 0;
  // Reject an undersized source address before consuming the queued packet.
  ok &= NetDll_WSASend_entry(1, sender->handle(), Map(sends), 2, Map(&sent), 0,
                             nullptr, nullptr) == 0;
  length = 1;
  ok &= NetDll_WSARecvFrom_entry(1, receiver->handle(), Map(receives), 2, Map(&received),
      Map(&flags), Map(&source), Map(&length), nullptr, nullptr) == ~0u;
  ok &= XThread::GetLastError() == 10014;
  receives[0].len = 1;
  ok &= NetDll_WSARecvFrom_entry(1, receiver->handle(), Map(receives), 1, Map(&received),
      Map(&flags), nullptr, nullptr, nullptr, nullptr) == ~0u;
  ok &= XThread::GetLastError() == 10040 && data[16] == 'a';
#if !REX_PLATFORM_WIN32
  ok &= NetDll_WSARecvFrom_entry(1, receiver->handle(), Map(receives), 1, Map(&received),
      Map(&flags), nullptr, nullptr, Map(&recv_over), nullptr) == ~0u;
  ok &= XThread::GetLastError() == 997;
  // Native descriptor cannot be closed/reused while its receive worker is alive.
  receiver->Close();
  ok &= recv_event->Wait(0, 0, 0, &timeout) == X_STATUS_SUCCESS;
  ok &= recv_over.internal == 995;
#else
  receiver->Close();
#endif
  sender->Close();
  runtime.memory()->SystemHeapFree(allocation);
  return ok;
}
}

TEST_CASE("WSA datagrams scatter, complete events, and cancel pending receive", "[kernel][net]") {
  WithKernel(DatagramRegression);
}

TEST_CASE("getpeername validates length and distinguishes unconnected sockets", "[kernel][net]") {
  WithKernel([](rex::Runtime& runtime) {
    using namespace rex;
    using namespace rex::system;
    using namespace rex::kernel::xam;
    auto socket = object_ref<XSocket>(new XSocket(runtime.kernel_state()));
    if (socket->Initialize(XSocket::AddressFamily::X_AF_INET, XSocket::Type::X_SOCK_DGRAM,
                           XSocket::Protocol::X_IPPROTO_UDP) != X_STATUS_SUCCESS) return false;
    XSOCKADDR_IN peer{};
    be_u32 length{1};
    bool ok = NetDll_getpeername_entry(1, socket->handle(), Map(&peer), Map(&length)) == ~0u
              && XThread::GetLastError() == 10014;
    length = sizeof(peer);
    ok &= NetDll_getpeername_entry(1, socket->handle(), Map(&peer), Map(&length)) == ~0u
          && XThread::GetLastError() == 10057;
    N_XSOCKADDR_IN destination{};
    destination.sin_family = 2;
    destination.sin_addr = 0x7F000001;
    destination.sin_port = 54321;
    ok &= socket->Connect(reinterpret_cast<N_XSOCKADDR*>(&destination), sizeof(destination))
          == X_STATUS_SUCCESS;
    ok &= NetDll_getpeername_entry(1, socket->handle(), Map(&peer), Map(&length)) == 0;
    ok &= peer.sin_addr == 0x7F000001 && peer.sin_port == 54321;
    socket->Close();
    return ok;
  });
}

#include "kernel/xbdm/debug_services.h"

TEST_CASE("XBDM services copy mapped memory, validate ranges, and identify the retail console", "[kernel][xbdm]") {
  WithKernel([](rex::Runtime& runtime) {
    using namespace rex;
    using namespace rex::kernel::xbdm;
    auto allocation = DmAllocatePool_entry(256);
    if (!allocation) return false;
    auto data = runtime.memory()->TranslateVirtual(allocation);
    std::memcpy(data, "debug service", 14);
    mapped_void source(data, allocation), destination(data + 64, allocation + 64);
    be_u32 copied{999};
    bool ok = DmGetMemory_entry(source, 14, destination, Map(&copied)) == kDmSuccess;
    ok &= copied == 14 && std::memcmp(data + 64, data, 14) == 0;
    data[0] = 'D';
    ok &= DmSetMemory_entry(destination, 14, source, Map(&copied)) == kDmSuccess;
    ok &= data[64] == 'D' && copied == 14;
    // Validate overflow before touching host data; output count must be cleared.
    copied = 999;
    ok &= DmGetMemory_entry(mapped_void(data, 0xFFFFFFF0), 32, destination, Map(&copied))
          == kDmFailure && copied == 0;
    copied = 999;
    ok &= DmSetMemory_entry(destination, 1, nullptr, Map(&copied)) == X_E_INVALIDARG
          && copied == 0;
    be_u32 capacity{1}, console_type{0};
    std::memset(data + 128, 0x5A, 32);
    mapped_void name(data + 128, allocation + 128);
    ok &= DmGetXboxName_entry(name, Map(&capacity)) == kDmBufferTooSmall && data[128] == 0x5A;
    capacity = 32;
    ok &= DmGetXboxName_entry(name, Map(&capacity)) == kDmSuccess;
    ok &= std::strcmp(reinterpret_cast<char*>(data + 128), "Xbox360Name") == 0;
    ok &= DmGetConsoleType_entry(Map(&console_type)) == kDmSuccess && console_type == ~0u;
    ok &= DmGetConsoleType_entry(nullptr) == X_E_INVALIDARG;
    be_u32 ids[32]{};
    capacity = 32;
    ok &= DmGetThreadList_entry(Map(ids), Map(&capacity)) == kDmSuccess;
    const auto id = rex::system::XThread::GetCurrentThreadId();
    bool found = false;
    for (u32 i = 0; i < std::min(u32(capacity), 32u); ++i) found |= ids[i] == id;
    ok &= found;
    capacity = 0;
    ids[0] = 0xCAFE;
    ok &= DmGetThreadList_entry(Map(ids), Map(&capacity)) == kDmBufferTooSmall
          && ids[0] == 0xCAFE;
    ok &= DmResumeThread_entry(~0u) == kDmFailure;
    ok &= DmSuspendThread_entry(~0u) == kDmFailure;
    ok &= DmFreePool_entry(source) == kDmSuccess;
    return ok;
  });
}

TEST_CASE("XNet random fills and session keys vary without writing outside their buffers", "[kernel][net]") {
  using namespace rex;
  using namespace rex::kernel::xam;
  std::array<uint8_t, 66> first{}, second{};
  first.fill(0x5A);
  second.fill(0x5A);
  REQUIRE(NetDll_XNetRandom_entry(1, mapped_void(first.data() + 1, 0), 64) == 0);
  REQUIRE(NetDll_XNetRandom_entry(1, mapped_void(second.data() + 1, 0), 64) == 0);
  CHECK(first != second);
  CHECK(first.front() == 0x5A);
  CHECK(first.back() == 0x5A);
  CHECK(second.front() == 0x5A);
  CHECK(second.back() == 0x5A);
  REQUIRE(NetDll_XNetRandom_entry(1, nullptr, 0) == 0);
  REQUIRE(NetDll_XNetRandom_entry(1, nullptr, 32) == 0);
  // Partial native random words must not overwrite even a one-byte buffer.
  first.fill(0x5A);
  REQUIRE(NetDll_XNetRandom_entry(1, mapped_void(first.data() + 1, 0), 1) == 0);
  CHECK(first[0] == 0x5A);
  CHECK(first[2] == 0x5A);
  first.fill(0x5A);
  second.fill(0x5A);
  REQUIRE(NetDll_XNetCreateKey_entry(1, mapped_void(first.data() + 1, 0),
                                      mapped_void(first.data() + 17, 0)) == 0);
  REQUIRE(NetDll_XNetCreateKey_entry(1, mapped_void(second.data() + 1, 0),
                                      mapped_void(second.data() + 17, 0)) == 0);
  CHECK(std::memcmp(first.data() + 1, second.data() + 1, 8) != 0);
  CHECK(std::memcmp(first.data() + 17, second.data() + 17, 16) != 0);
  CHECK(first[0] == 0x5A);
  CHECK(first[9] == 0x5A);
  CHECK(first[16] == 0x5A);
  CHECK(first[33] == 0x5A);
  CHECK(NetDll_XNetCreateKey_entry(1, nullptr, mapped_void(first.data(), 0)) == 10022);
}
