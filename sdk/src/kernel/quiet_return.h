// A stub that returns a fixed value without logging. For functions a game polls many times
// (Forza calls XamXStudioRequest 90,226 times a run) where REX_EXPORT_STUB's warning is only noise.
#pragma once
#include <rex/hook.h>

#define REX_EXPORT_QUIET_RETURN(name, retval)                                 \
  extern "C" REX_FUNC(name) {                                                 \
    (void)base;                                                               \
    ctx.r3.u64 = (retval);                                                    \
  }                                                                           \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

// Same, but leaves r3 untouched: what the old REX_EXPORT_STUB did. For functions BO2 polls where
// returning 0 changed behaviour (town4: "Downloadable content has changed" Com_Error).
#define REX_EXPORT_QUIET_PASSTHROUGH(name)                \
  extern "C" REX_FUNC(name) { (void)base; (void)ctx; }    \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);
