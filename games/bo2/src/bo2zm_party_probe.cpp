// Bounded observations of test-client admission; never changes game capacity.
#include <cstring>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(bo2zm_test_player_capacity, false, "BO2",
                    "Test helper: log test-client admission and the current server capacity")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8242F2A8);

namespace {
uint32_t ReadWord(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}
}

REX_HOOK_RAW(sub_8242F2A8) {
  __imp__sub_8242F2A8(ctx, base);
  if (!REXCVAR_GET(bo2zm_test_player_capacity)) return;
  // This creator runs on the server thread. Keep failed spawn-loop logs bounded.
  static unsigned reports = 0;
  if (reports++ >= 16) return;
  const uint32_t entity = ctx.r3.u32;
  constexpr uint32_t entities = 0x833D0640;
  constexpr uint32_t entity_size = 796;
  const bool valid = entity >= entities && entity < entities + 1024 * entity_size &&
                     (entity - entities) % entity_size == 0;
  const uint32_t dvar = ReadWord(base, 0x8387A7D4);
  const int capacity = dvar >= 0x82000000 && dvar <= 0xFFFFFFE0 ?
      int(ReadWord(base, dvar + 24)) : -1;
  REXLOG_INFO("[party probe] test-client result {:08X}, entity index {}, com_maxclients {}",
              entity, valid ? int((entity - entities) / entity_size) : -1, capacity);
}
