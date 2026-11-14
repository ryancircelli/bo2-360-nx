// Runs the port's Lua (games/bo2/src/ui/menus.lua) inside the game's menu system.
//
// BO2 builds its menus in Lua (HavokScript) loaded from its fastfiles, and the executable carries the Lua compiler
// (the base library's loadstring). So instead of changing game files, the port keeps its menu changes as Lua source,
// compiled into the program as text, and runs it in the menu system's Lua state with the game's own functions:
//   - the menu Lua state is the pointer at 0x845CD1F8 (the game's "load and run a Lua file" function passes it, e.g.
//     for ui_mp/t6/zombie/ffotdzm.lua);
//   - sub_82708D68(L, compiler settings = *(L + 8) + 728, text, length, chunk name) compiles a buffer, as loadstring
//     does, and returns 0 on success;
//   - sub_826F2720(L, arguments, results, error handler) is lua_pcall, as the base library's pcall uses it;
//   - sub_824A0BA8 / sub_824A0BC0 (38) enter and leave the lock the game holds around the same work;
//   - the global state's word at +284 is the security mode: 2 ("secure") refuses source text ("Attempt to load
//     source code into a secure state."), so it is set to 0 only while our chunk compiles, then put back.
// Stack values are 8 bytes: the value, then the type in the low 4 bits of the second word (1 boolean, 4 string);
// lua_State + 36 is the top of the stack.
//
// The Lua returns true once it has installed itself. Until then (the menu system not ready yet) it is tried again,
// at most every 30 frames. A new Lua state (the menus restarted) gets it again, and so does the same state when its
// globals were rebuilt: once a second a tiny probe chunk checks that LUI.bo2zm (our state table) is still there, and
// has the menu Lua wrap again any menu function the game loaded anew.
//
// Once a frame is too late for a level's loading screen: loading a level starts the menus in a new Lua state
// (sub_82719880 makes it with sub_82713790) and opens the loading screen in that same frame. sub_82719880 loads the
// menu modules with sub_8270BDB0 (r3 = the name) before it opens any menu: "T6.CoDRoot" and "T6.main" for the menus,
// "T6.HUD" for a level, then in both the fix-of-the-day module ("T6.Zombie.ffotdzm" in Zombies, else "T6.ffotdmp";
// the campaign program loads "T6.CoDRoot", "T6.HUD" and "T6.ffotdsp" for its menus and its levels alike).
// The menu Lua is installed (or wraps again) right after that last one. Not earlier: run right after "T6.CoDRoot",
// before the engine's functions were there, it failed and the game crashed.

#include "bo2zm_ui.h"

#include <cstring>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "bo2zm_game.h"
#include "bo2zm_ui_lua.h"  // generated from games/bo2/src/ui/menus.lua (games/bo2/CMakeLists.txt)

REXCVAR_DEFINE_BOOL(bo2zm_ui_changes, true, "BO2",
                    "Applies mode-specific home menus, Extras and graphics settings. Online and System Link "
                    "entries are marked as work in progress.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_824A0BA8);
REX_EXTERN(__imp__sub_824A0BC0);
REX_EXTERN(__imp__sub_82708D68);
REX_EXTERN(__imp__sub_826F2720);

namespace {

constexpr uint32_t kMenuLuaState = BO2_GAME_ADDR(0x845CD1F8, 0x83EF2F40);
constexpr uint32_t kGameLock = 38;
constexpr uint32_t kStateGlobal = 8;
constexpr uint32_t kGlobalCompilerSettings = 728;
constexpr uint32_t kGlobalSecurityMode = 284;
constexpr uint32_t kStateTop = 36;
constexpr uint32_t kStateStackBottom = 48;
constexpr uint32_t kTypeBoolean = 1;
constexpr uint32_t kTypeString = 4;
constexpr const char kChunkName[] = "=bo2zm menus";
// The probe also lets the menu Lua wrap again what the game replaced since (it reloads its menu files for a match).
constexpr const char kProbeLua[] =
    "local LUI = rawget(_G, \"LUI\") local s = LUI and rawget(LUI, \"bo2zm\") "
    "if not s then return false end if s.install then s.install() end return true";
constexpr const char kProbeName[] = "=bo2zm probe";
constexpr uint32_t kProbeEveryFrames = 60;

uint32_t g_installed_state = 0;  // the Lua state the menu changes are installed in
uint32_t g_wait_frames = 0;
uint32_t g_attempts = 0;
uint32_t g_text = 0;  // guest copies of the Lua text and the chunk name
uint32_t g_name = 0;
uint32_t g_probe_text = 0;  // and of the probe's
uint32_t g_probe_name = 0;
uint32_t g_probe_frames = 0;
uint32_t g_stack_relocations = 0;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

// The text of a Lua string value, for the log: the printable run inside the string object.
std::string_view StringOf(const uint8_t* base, uint32_t object) {
  if (object < 0x1000) {
    return {};
  }
  const char* p = reinterpret_cast<const char*>(base + object);
  for (uint32_t start = 0; start < 32; ++start) {
    uint32_t n = 0;
    while (n < 300 && p[start + n] >= 0x20 && p[start + n] < 0x7F) {
      ++n;
    }
    if (n >= 6) {
      return {p + start, n};
    }
  }
  return {};
}

bool CopyToGuest(uint8_t* base) {
  if (g_text) {
    return true;
  }
  auto* memory = rex::Runtime::instance()->memory();
  const size_t length = std::strlen(bo2zm::ui::kMenusLua);
  g_text = memory->SystemHeapAlloc(uint32_t(length + 1));
  g_name = memory->SystemHeapAlloc(sizeof(kChunkName));
  g_probe_text = memory->SystemHeapAlloc(sizeof(kProbeLua));
  g_probe_name = memory->SystemHeapAlloc(sizeof(kProbeName));
  if (!g_text || !g_name || !g_probe_text || !g_probe_name) {
    REXLOG_ERROR("[ui] no guest memory for the menu Lua ({} bytes)", length);
    g_text = 0;
    return false;
  }
  std::memcpy(base + g_text, bo2zm::ui::kMenusLua, length + 1);
  std::memcpy(base + g_name, kChunkName, sizeof(kChunkName));
  std::memcpy(base + g_probe_text, kProbeLua, sizeof(kProbeLua));
  std::memcpy(base + g_probe_name, kProbeName, sizeof(kProbeName));
  return true;
}

// Compiles and runs a chunk (guest text of length bytes, guest chunk name) in state L. Returns whether it returned
// true: for the menu Lua, that it is installed; for the probe, that it still is.
bool RunLua(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t text, uint32_t length, uint32_t name) {
  const auto saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 512) & ~uint64_t(0xF);
  ctx.r3.u64 = kGameLock;
  __imp__sub_824A0BA8(ctx, base);

  const uint32_t top_before = Load32(base, L + kStateTop);
  const uint32_t bottom_before = Load32(base, L + kStateStackBottom);
  const uint32_t top_offset = top_before - bottom_before;
  const uint32_t global = Load32(base, L + kStateGlobal);
  const uint32_t security = Load32(base, global + kGlobalSecurityMode);
  Store32(base, global + kGlobalSecurityMode, 0);
  ctx.r3.u64 = L;
  ctx.r4.u64 = global + kGlobalCompilerSettings;
  ctx.r5.u64 = text;
  ctx.r6.u64 = length;
  ctx.r7.u64 = name;
  __imp__sub_82708D68(ctx, base);
  Store32(base, global + kGlobalSecurityMode, security);
  int status = int32_t(ctx.r3.u32);
  const char* stage = "compile";
  if (status == 0) {
    stage = "run";
    ctx.r3.u64 = L;
    ctx.r4.u64 = 0;  // arguments
    ctx.r5.u64 = 1;  // results
    ctx.r6.u64 = 0;  // no error handler
    __imp__sub_826F2720(ctx, base);
    status = int32_t(ctx.r3.u32);
  }
  bool installed = false;
  // Compilation or execution can grow the VM stack and free its old allocation.
  // Keep its saved depth, then restore within the current allocation.
  const uint32_t bottom_after = Load32(base, L + kStateStackBottom);
  const uint32_t restore_top = bottom_after + top_offset;
  if (bottom_after != bottom_before) {
    ++g_stack_relocations;
    REXLOG_INFO("[ui] menu Lua stack relocated {:08X} -> {:08X} (count {}, {} saved values)",
                bottom_before, bottom_after, g_stack_relocations, top_offset / 8);
  }
  const uint32_t top = Load32(base, L + kStateTop);
  if (top > restore_top) {
    const uint32_t value = Load32(base, top - 8);
    const uint32_t type = Load32(base, top - 4) & 0xF;
    if (status == 0) {
      installed = type == kTypeBoolean && value != 0;
    } else if (g_attempts <= 3) {
      const std::string_view error = type == kTypeString ? StringOf(base, value) : std::string_view();
      REXLOG_ERROR("[ui] menu Lua failed to {} (status {}): {}", stage, status, error);
    }
    Store32(base, L + kStateTop, restore_top);  // drop the result or the error
  }

  ctx.r3.u64 = kGameLock;
  __imp__sub_824A0BC0(ctx, base);
  ctx = saved;
  return installed;
}

// Installs the menu Lua in state L, or (installed there already) has it wrap again what the game loaded since.
// Returns false when it could not install yet (the menu system not ready, or no guest memory).
bool InstallIn(PPCContext& ctx, uint8_t* base, uint32_t L) {
  if (L == g_installed_state) {
    if (RunLua(ctx, base, L, g_probe_text, uint32_t(sizeof(kProbeLua) - 1), g_probe_name)) {
      return true;
    }
    REXLOG_INFO("[ui] the menu system was rebuilt in Lua state {:08X}: installing the menu changes again", L);
    g_installed_state = 0;
  }
  if (!CopyToGuest(base)) {
    return false;
  }
  ++g_attempts;
  if (!RunLua(ctx, base, L, g_text, uint32_t(std::strlen(bo2zm::ui::kMenusLua)), g_name)) {
    return false;
  }
  g_installed_state = L;
  g_attempts = 0;
  REXLOG_INFO("[ui] menu changes installed in the menu Lua state {:08X} (games/bo2/src/ui/menus.lua)", L);
  return true;
}

}  // namespace

// sub_8270BDB0: the menu system loads one of its Lua modules (see the top of this file).
REX_EXTERN(__imp__sub_8270BDB0);
REX_HOOK_RAW(sub_8270BDB0) {
  const uint32_t name = ctx.r3.u32;
  __imp__sub_8270BDB0(ctx, base);
  if (!REXCVAR_GET(bo2zm_ui_changes) || name == 0) {
    return;
  }
  const std::string_view module(reinterpret_cast<const char*>(base + name));
  if (module != "T6.Zombie.ffotdzm" && module != "T6.ffotdmp" && module != "T6.ffotdsp") {
    return;
  }
  if (const uint32_t L = Load32(base, kMenuLuaState); L != 0) {
    const uint64_t result = ctx.r3.u64;
    InstallIn(ctx, base, L);
    ctx.r3.u64 = result;
  }
}

// The menu system's handler for a Lua error nothing caught (sub_8270BBD0, the Lua state in r4, the message on top of
// its stack): it prints the message to the game's console, which this log does not get, and ends with a Com_Error
// "UI Error <djb2 hash of the message, mod 100000>" that resets the menus. The message is logged here first.
REX_EXTERN(__imp__sub_8270BBD0);
REX_HOOK_RAW(sub_8270BBD0) {
  const uint32_t L = ctx.r4.u32;
  const uint32_t top = L ? Load32(base, L + kStateTop) : 0;
  if (top >= 8 && (Load32(base, top - 4) & 0xF) == kTypeString) {
    REXLOG_ERROR("[ui] Lua error the menus did not catch: {}", StringOf(base, Load32(base, top - 8)));
  } else {
    REXLOG_ERROR("[ui] Lua error the menus did not catch (no message on the stack)");
  }
  __imp__sub_8270BBD0(ctx, base);
}

void bo2zm::ui::EveryFrame(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(bo2zm_ui_changes)) [[unlikely]] {
    return;
  }
  const uint32_t L = Load32(base, kMenuLuaState);
  if (L == 0) [[unlikely]] {
    return;
  }
  if (L == g_installed_state) [[likely]] {
    if (++g_probe_frames < kProbeEveryFrames) [[likely]] {
      return;
    }
    g_probe_frames = 0;
  } else if (g_wait_frames > 0) {
    --g_wait_frames;
    return;
  }
  if (!InstallIn(ctx, base, L)) {
    g_wait_frames = g_text ? 30 : 0xFFFFFFFF;
  }
}
