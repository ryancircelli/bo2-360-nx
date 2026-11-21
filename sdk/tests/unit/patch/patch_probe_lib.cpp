// Patch library for the in-game live reload proof: replaces the runtime's per-swap probe.
#include <rex/ppc/context.h>
#include <rex/system/patch.h>

#ifndef PROBE_VALUE
#define PROBE_VALUE 1
#endif
static void probe_body(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = PROBE_VALUE; }
REX_PATCH(rex_diag_swap_probe, probe_body)
REX_PATCH_LIBRARY()
