// Optional client script HUD fitting. Server sources and the elements stay
// unchanged: a mod element is drawn with its view's screen placement replaced.
#pragma once
#include <cstdint>
#include <cstddef>
#include <rex/ppc.h>

#include "bo2zm_mod_hud_wire.h"

namespace bo2zm::mod_hud_fit {
void BeginEpoch();
// Normal server script startup completed. View observations must be freshly
// published after this boundary before a replicated copy can be fitted.
void EndMapLoad();
constexpr bool IsCopySite(uint32_t lr) {
  return lr == 0x8232CDB0 || lr == 0x8232CE8C ||
         lr == 0x8232CF68 || lr == 0x8232D044;
}
// This full playerstate copy tail-calls the shared CRT without changing LR.
constexpr bool IsUnknownPsCopySite(uint32_t lr) { return lr == 0x82439010; }
constexpr bool IsBulkPsCopySite(uint32_t lr) { return lr == 0x8230369C; }
// Diagnostic metadata publication precedes the destination becoming visible.
// Never changes either copy buffer or the guest register context.
void BeforeCopy(const PPCContext& ctx, uint8_t* base);
// Called after the existing CRT copy. Must not modify registers or invoke guest
// code; unrelated copies leave through the inexpensive caller guard.
void AfterCopy(const PPCContext& ctx, uint8_t* base);

// System Link (bo2zm_mod_hud_wire.h). The host: its players' current menu tables, at most capacity of them; none when
// this console hosts no System Link game or no mod made a menu. Any thread.
size_t HostTables(mod_hud_wire::Table* out, size_t capacity);
// A console that joined another's game: a table from the host (checked by mod_hud_wire::Decode), kept until a newer
// one for the same player, another map or host, ResetJoinedTables, or a few seconds without news replace it.
void StoreJoinedTable(const mod_hud_wire::Table& table);
// Out of the joined game: every table goes.
void ResetJoinedTables();
}
