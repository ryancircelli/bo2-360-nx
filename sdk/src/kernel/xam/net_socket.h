// Socket ABI shared by the kernel exports and regression tests.
#pragma once
#include <rex/types.h>
#include <rex/system/xsocket.h>
namespace rex::kernel::xam {
struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};


u32 NetDll_XNetRandom_entry(u32, mapped_void, u32);
u32 NetDll_XNetCreateKey_entry(u32, mapped_void, mapped_void);
u32 NetDll_WSARecvFrom_entry(u32, u32, ppc_ptr_t<XWSABUF>, u32, mapped_u32, mapped_u32,
                            ppc_ptr_t<system::XSOCKADDR_IN>, mapped_u32,
                            ppc_ptr_t<XWSAOVERLAPPED>, mapped_void);
u32 NetDll_WSASendTo_entry(u32, u32, ppc_ptr_t<XWSABUF>, u32, mapped_u32, u32,
                          ppc_ptr_t<system::XSOCKADDR_IN>, u32,
                          ppc_ptr_t<XWSAOVERLAPPED>, mapped_void);
u32 NetDll_WSARecv_entry(u32, u32, ppc_ptr_t<XWSABUF>, u32, mapped_u32, mapped_u32,
                        ppc_ptr_t<XWSAOVERLAPPED>, mapped_void);
u32 NetDll_WSASend_entry(u32, u32, ppc_ptr_t<XWSABUF>, u32, mapped_u32, u32,
                        ppc_ptr_t<XWSAOVERLAPPED>, mapped_void);
u32 NetDll_getpeername_entry(u32, u32, ppc_ptr_t<system::XSOCKADDR_IN>, mapped_u32);
u32 NetDll_WSACancelOverlappedIO_entry(u32, u32);
u32 NetDll_WSAGetOverlappedResult_entry(u32, u32, ppc_ptr_t<XWSAOVERLAPPED>, mapped_u32, u32, mapped_u32);
}
