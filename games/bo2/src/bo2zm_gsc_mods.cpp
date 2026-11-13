// Overrides ScriptParseTree lookups before the server VM links a map. Original
// bytes stay on the host; each load receives fresh writable guest objects.
#include "bo2zm_gsc_mods.h"
#include "bo2zm_gsc_catalog.h"
#include "bo2zm_gsc_bundle.h"
#include "bo2zm_gsc_plan.h"
#include "bo2zm_bots.h"
#include "bo2zm_cheats.h"
#include "bo2zm_gsc_canonical.h"
#include "bo2zm_script_manager.h"
#include "bo2zm_mode.h"
#include "bo2zm_guest.h"
#include "bo2zm_mod_hud_fit.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_822CAE50);
REX_EXTERN(__imp__sub_8252DED0);
REX_EXTERN(__imp__sub_823599E8);
REX_EXTERN(__imp__sub_8252EED8);
REX_EXTERN(__imp__sub_8252F808);

namespace bo2zm::gsc {
namespace {
struct Script {
  ObjectInfo info;
  std::vector<uint8_t> bytes;
  uint32_t descriptor = 0;
  uint32_t buffer = 0;
  bool reported = false;
  bool menu_code = false;
};
std::mutex mutex;
std::vector<Script> scripts;
uint64_t retained = 0;
uint32_t generations = 0;
bool active = false;
bool menu_ranges_active = false;  // protected by mutex
std::string mod_name;
std::optional<Component> menu_component, bot_component;
std::vector<std::string> entries;
int active_bot_count = 0;
bool menu_permitted = false;
uint32_t bot_dvar_text = 0;
bool entry_loaded = false;
bool entry_started = false;
std::optional<ScriptReplacement> replacement;
std::vector<uint8_t> source_bytes;
ObjectInfo source_info;
uint32_t source_descriptor = 0;
uint32_t source_buffer = 0;
size_t source_export = 0;
std::mutex canonical_mutex;
CanonicalNames canonical_names;
constexpr uint32_t kCanonicalCount = 0x8406F840;
constexpr uint32_t kCanonicalNames = 0x840717A8;
constexpr uint32_t kCanonicalError = 0x82068CE4;
constexpr uint32_t kComError = 0x8240C468;  // call only: type, format, optional arguments
constexpr uint64_t kRetainedLimit = 16 * 1024 * 1024;
constexpr uint32_t kLoadScript = 0x8252DA38;  // call only: server instance, extensionless script path
constexpr uint32_t kGetFunction = 0x8252FCB0;  // call only: loaded script's exported function
constexpr uint32_t kExecThread = 0x82543150;  // call only: server instance, code pointer, argument count
constexpr uint32_t kFreeThread = 0x8253D3F8;  // call only: server instance, returned thread id
constexpr uint32_t kMissingFunction = 0x829917FC;
constexpr uint32_t kFindDvar = 0x82496430;  // call only: dvar name -> dvar pointer
constexpr uint32_t kDvarString = 0x82496990;  // call only: dvar pointer -> current string
constexpr uint32_t kRegisterInt = 0x824985D0;  // call only: name, default, min, max, flags, description
constexpr uint32_t kSetInt = 0x824991B8;  // call only: dvar pointer, integer value

void InvalidateHudProvenance() {
  {
    std::lock_guard lock(mutex);
    menu_ranges_active = false;
  }
  bo2zm::mod_hud_fit::BeginEpoch();
}

std::string CurrentDvar(PPCContext& ctx, uint8_t* base, const char* name) {
  const auto saved = ctx;
  const auto stack = (ctx.r1.u32 - 512) & ~15u;
  const auto text_area = stack + 256;
  std::strcpy(reinterpret_cast<char*>(base + text_area), name);
  ctx.r1.u64 = stack;
  ctx.r3.u64 = text_area;
  bo2zm::guest::Call<kFindDvar>(ctx, base);
  const auto dvar = ctx.r3.u32;
  if (!dvar) { ctx = saved; return {}; }
  ctx.r1.u64 = stack;
  ctx.r3.u64 = dvar;
  bo2zm::guest::Call<kDvarString>(ctx, base);
  const auto pointer = ctx.r3.u32;
  ctx = saved;
  if (pointer < 0x1000 || pointer > 0xFFFFFFBFu) return {};
  const char* text = reinterpret_cast<const char*>(base + pointer);
  size_t length = 0;
  while (length < 64 && text[length]) ++length;
  return length < 64 ? std::string(text, length) : std::string();
}

// Called on the server load thread; no main-thread command runner is involved.
bool SetBotCount(PPCContext& ctx, uint8_t* base, int count) {
  if (count < 0 || count > bo2zm::bots::preferences::kSupportedMaximum) return false;
  if (!bot_dvar_text) {
    bot_dvar_text = rex::Runtime::instance()->memory()->SystemHeapAlloc(128, 16);
    if (!bot_dvar_text) return false;
    std::strcpy(reinterpret_cast<char*>(base + bot_dvar_text), "bo2_zm_bots_count");
  }
  const auto saved = ctx;
  const auto stack = (ctx.r1.u32 - 512) & ~15u;
  ctx.r1.u64 = stack;
  ctx.r3.u64 = bot_dvar_text;
  ctx.r4.u64 = count;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = bo2zm::bots::preferences::kSupportedMaximum;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = bot_dvar_text;
  bo2zm::guest::Call<kRegisterInt>(ctx, base);
  const auto dvar = ctx.r3.u32;
  ctx.r1.u64 = stack;
  ctx.r3.u64 = dvar;
  ctx.r4.u64 = count;
  bo2zm::guest::Call<kSetInt>(ctx, base);
  ctx = saved;
  return true;
}

void SelectGeneration(PPCContext& ctx, uint8_t* base, bool allow_bots = true) {
  Plan plan;
  const bo2zm::bots::Context context{CurrentDvar(ctx, base, "mapname"),
      CurrentDvar(ctx, base, "g_gametype"), CurrentDvar(ctx, base, "ui_zm_mapstartlocation")};
  menu_permitted = menu_component && AppliesToMap(menu_component->options.maps, context.map);
  if (menu_component) {
    AddComponent(plan, *menu_component, context.map);
    if (!AppliesToMap(menu_component->options.maps, context.map))
      bo2zm::scripts::SkippedMap(menu_component->options.maps);
  }
  active_bot_count = 0;
  const bool zombies_map = bo2zm::mode::IsZombies() && context.map.starts_with("zm_") &&
      context.map != "zm_frontend";
  const int requested = zombies_map ? bo2zm::bots::Requested(context, bo2zm::bots::Humans(base)) : 0;
  if (allow_bots && requested > 0 && bot_component) {
    try {
      auto combined = plan;
      AddComponent(combined, *bot_component, context.map);
      plan = std::move(combined);
      active_bot_count = requested;
      bo2zm::bots::RuntimeStatus(std::to_string(requested) + " bots enabled for this match");
      REXLOG_INFO("[bots] {} bots: map={}, gametype={}, start={}", requested,
                  context.map, context.gametype, context.start);
    } catch (const std::exception& error) {
      // The menu plan was left intact if composition failed.
      bo2zm::bots::RuntimeStatus(std::string("Bots unavailable: ") + error.what());
      REXLOG_ERROR("[bots] {}", error.what());
    }
  } else if (bo2zm::mode::IsZombies()) {
    bo2zm::bots::RuntimeStatus(requested > 0 ? "Bot files are missing or invalid" : "Bots off for this match");
  }
  std::vector<Script> pending;
  for (auto& object : plan.objects) {
    const auto contains = [&](const std::optional<Component>& component) {
      return component && std::any_of(component->objects.begin(), component->objects.end(), [&](const auto& candidate) {
        return candidate.info.name == object.info.name && candidate.bytes == object.bytes;
      });
    };
    // Shared component objects are ambiguous and remain outside menu provenance.
    const bool menu_code = menu_permitted && contains(menu_component) &&
                           !(active_bot_count > 0 && contains(bot_component));
    pending.push_back({std::move(object.info), std::move(object.bytes), 0, 0, false, menu_code});
  }
  {
    std::lock_guard lock(mutex);
    active = false;
    scripts = std::move(pending);
    entries = std::move(plan.entries);
    replacement = plan.replace.empty() ? std::optional<ScriptReplacement>{} : ParseReplacement(plan.replace);
  }
}

void PrepareReplacement(PPCContext& ctx, uint8_t* base, bool permitted) {
  active = false;
  source_descriptor = source_buffer = 0;
  source_bytes.clear();
  {
    std::lock_guard lock(canonical_mutex);
    canonical_names.Reset();
  }
  if (!permitted || !replacement) return;
  const auto saved = ctx;
  const uint32_t area = (ctx.r1.u32 - 1024) & ~15u;
  std::memcpy(base + area, replacement->source.file.c_str(), replacement->source.file.size() + 1);
  ctx.r1.u64 = area - 256;
  ctx.r3.u64 = 50;
  ctx.r4.u64 = area;
  ctx.r5.u64 = 0;
  ctx.r6.s64 = -1;
  __imp__sub_822CAE50(ctx, base);
  const auto descriptor = ctx.r3.u32;
  ctx = saved;
  try {
    if (descriptor < 0x1000 || descriptor > 0xFFFFFFF3u) throw std::runtime_error("replacement source asset is missing");
    const std::span<const uint8_t> header(base + descriptor, 12);
    const auto size = Read32(header, 4), buffer = Read32(header, 8);
    if (size < 64 || size > 1024 * 1024 || buffer < 0x1000 || uint64_t(buffer) + size > 0x100000000ull)
      throw std::runtime_error("invalid replacement source asset");
    source_bytes.assign(base + buffer, base + buffer + size);
    std::string error;
    if (!Validate(source_bytes, true, source_info, error)) throw std::runtime_error(error);
    if (source_info.name != replacement->source.file) throw std::runtime_error("replacement source asset name mismatch");
    source_export = ReplacementExport(source_bytes, replacement->source.function);
  } catch (const std::exception& error) {
    source_bytes.clear();
    bo2zm::scripts::Rejected(error.what());
    REXLOG_ERROR("[gsc] replacement refused: {}; using stock scripts", error.what());
  }
}

// Only the stock registrar and begin-load reset access the canonical-name
// pointer array. Keep its existing IDs and storage intact. Additional mod field
// names live on the host and retain IDs below the next 14-bit boundary.
bool CanonicalOverflow(PPCContext& ctx, uint8_t* base) {
  if (!active) return false;
  const auto word = [&](uint32_t at) { return Read32(std::span<const uint8_t>(base + at, 4), 0); };
  const auto text = [&](uint32_t at) {
    const char* start = reinterpret_cast<const char*>(base + at);
    size_t size = 0;
    while (size < 1024 && start[size]) ++size;
    if (size == 1024) throw std::runtime_error("canonical field name exceeds mod limit");
    return std::string(start, size);
  };
  uint32_t id = 0;
  uint32_t next = 0;
  bool added = false;
  bool failed = false;
  {
    std::string error;
    {
      std::lock_guard lock(canonical_mutex);
      try {
        const auto count = word(kCanonicalCount);
        if (count < CanonicalNames::kStockLimit) {
          // The stock registrar can longjmp on an interning error. Its guest
          // call must remain outside C++ guards, following stock VM serialization.
          return false;
        }
        id = canonical_names.Resolve(count, text(ctx.r3.u32), [&](uint32_t i) {
          return text(word(kCanonicalNames + i * 4));
        });
        next = canonical_names.Count();
        added = next != count;
        if (added) Write32(base + kCanonicalCount, next);
      } catch (const std::exception& e) { error = e.what(); }
    }
    if (!error.empty()) {
      REXLOG_ERROR("[gsc] {}", error);
      failed = true;
    }
  }
  if (failed) {
    InvalidateHudProvenance();
    ctx.r3.u64 = 0;
    ctx.r4.u64 = kCanonicalError;
    bo2zm::guest::Call<kComError>(ctx, base);
    return true;
  }
  if (added && (id == CanonicalNames::kStockLimit || (id % 256) == 0))
    REXLOG_INFO("[gsc] canonical mod field IDs: {} of {}", next, CanonicalNames::kLimit);
  ctx.r3.u64 = id;
  return true;
}

// sub_8252DED0 resets the server script table through sub_8252FF88. Old
// allocations are retained because other VM references have not been proven
// quiescent at that point. Both memory and load count have hard limits.
void NewGeneration(uint8_t* base, bool permitted) {
  std::lock_guard lock(mutex);
  active = false;
  menu_ranges_active = false;
  entry_loaded = false;
  entry_started = false;
  if (!permitted || scripts.empty()) return;
  if (replacement && source_bytes.empty()) return;
  size_t required = 0;
  for (const auto& s : scripts) required += 64 + s.info.name.size() + s.bytes.size();
  if (replacement) required += 64 + source_info.name.size() + source_bytes.size();
  // Guest heap allocations are page-rounded; account for the actual upper bound.
  const size_t allocation = (required + 0xFFF) & ~size_t(0xFFF);
  if (generations >= 64 || allocation > kRetainedLimit - retained) {
    REXLOG_ERROR("[gsc] script reload limit reached; restart the program to use this mod again");
    return;
  }
  auto* memory = rex::Runtime::instance()->memory();
  const uint32_t address = memory->SystemHeapAlloc(uint32_t(allocation), 32);
  if (!address) {
    REXLOG_ERROR("[gsc] no guest memory for script overrides; using stock scripts");
    return;
  }
  uint32_t cursor = address;
  if (replacement) {
    source_descriptor = cursor;
    const auto name = cursor + 12;
    std::memcpy(base + name, source_info.name.c_str(), source_info.name.size() + 1);
    source_buffer = (name + uint32_t(source_info.name.size()) + 1 + 31) & ~31u;
    std::memcpy(base + source_buffer, source_bytes.data(), source_bytes.size());
    base[source_buffer + source_bytes.size()] = 0;
    if (source_info.legacy_size) Write32(base + source_buffer + 0x2C, source_info.code_size);
    Write32(base + cursor, name);
    Write32(base + cursor + 4, uint32_t(source_bytes.size()));
    Write32(base + cursor + 8, source_buffer);
    cursor = (source_buffer + uint32_t(source_bytes.size()) + 1 + 3) & ~3u;
  }
  for (auto& s : scripts) {
    s.descriptor = cursor;
    const uint32_t name = cursor + 12;
    std::memcpy(base + name, s.info.name.c_str(), s.info.name.size() + 1);
    const uint32_t buffer = (name + uint32_t(s.info.name.size()) + 1 + 31) & ~31u;
    s.buffer = buffer;
    std::memcpy(base + buffer, s.bytes.data(), s.bytes.size());
    base[buffer + s.bytes.size()] = 0;
    if (s.info.legacy_size) Write32(base + buffer + 0x2C, s.info.code_size);
    Write32(base + cursor, name);
    Write32(base + cursor + 4, uint32_t(s.bytes.size()));
    Write32(base + cursor + 8, buffer);
    cursor = (buffer + uint32_t(s.bytes.size()) + 1 + 3) & ~3u;
    s.reported = false;
  }
  retained += allocation;
  if (replacement) {
    const auto target = std::find_if(scripts.begin(), scripts.end(), [&](const auto& script) {
      return script.info.name == replacement->target.file;
    });
    const auto row = ReplacementExport(target->bytes, replacement->target.function);
    const auto code = target->buffer + Read32(target->bytes, row + 4);
    // The binder and GetFunction both compute buffer + export.codeOffset.
    // The private stock clone is allocated first, so this forward offset routes
    // calls and function references into the separately linked mod module.
    Write32(base + source_buffer + source_export + 4, code - source_buffer);
    base[source_buffer + source_export + 11] |= 1;
    base[target->buffer + row + 11] |= 1;
    REXLOG_INFO("[gsc] replaced {}::{} with {}::{}", replacement->source.file,
                replacement->source.function, replacement->target.file, replacement->target.function);
  }
  ++generations;
  active = true;
  menu_ranges_active = true;
  REXLOG_INFO("[gsc] generation {}: {} scripts, {} retained guest bytes", generations, scripts.size(), retained);
}

// Run only on the server's script-loading thread. Scr_LoadScript recursively
// loads dependencies while the stock begin-load state is still open.
void LoadEntry(PPCContext& ctx, uint8_t* base) {
  if (!active || entries.empty()) return;
  entry_loaded = true;
  for (const auto& entry : entries) {
    const auto saved = ctx;
    const uint32_t area = (ctx.r1.u32 - 1024) & ~15u;
    const auto file = std::string_view(entry).substr(0, entry.size() - 4);
    std::memcpy(base + area, file.data(), file.size());
    base[area + file.size()] = 0;
    ctx.r1.u64 = area - 256;
    ctx.r3.u64 = 0;
    ctx.r4.u64 = area;
    bo2zm::guest::Call<kLoadScript>(ctx, base);
    entry_loaded = ctx.r3.u32 != 0;
    ctx = saved;
    if (entry_loaded) REXLOG_INFO("[gsc] loaded startup entry {}", entry);
    else {
      InvalidateHudProvenance();
      bo2zm::scripts::Rejected("could not load startup entry; restart before using this mod");
      REXLOG_ERROR("[gsc] could not load startup entry {}; aborting map load", entry);
      // Linked operands can already reference the private clone. Abort rather
      // than continuing with a partially linked mod. Keep canonical state until
      // the server's next coordinated reset; no host lock crosses Com_Error.
      constexpr char message[] = "Script mod startup could not load; unload the mod before retrying";
      std::memcpy(base + area, message, sizeof(message));
      ctx.r1.u64 = area - 256;
      ctx.r3.u64 = 0;
      ctx.r4.u64 = area;
      bo2zm::guest::Call<kComError>(ctx, base);
      ctx = saved;
      return;
    }
  }
}

// sub_823599E8 starts the stock server scripts and their autoexec exports.
// Start optional mod exports afterwards, once per freshly loaded generation.
void StartEntry(PPCContext& ctx, uint8_t* base) {
  if (!entry_loaded || entry_started) return;
  entry_started = true;
  for (const auto& entry : entries) {
    const auto saved = ctx;
    const uint32_t area = (ctx.r1.u32 - 1024) & ~15u;
    const auto file = std::string_view(entry).substr(0, entry.size() - 4);
    std::memcpy(base + area, file.data(), file.size());
    base[area + file.size()] = 0;
    for (const auto* function : {"main", "init"}) {
      std::strcpy(reinterpret_cast<char*>(base + area + 160), function);
      ctx.r1.u64 = area - 256;
      ctx.r3.u64 = 0;
      ctx.r4.u64 = area;
      ctx.r5.u64 = area + 160;
      ctx.r6.u64 = 0;
      ctx.r7.u64 = 1;
      bo2zm::guest::Call<kGetFunction>(ctx, base);
      const auto handle = ctx.r3.u32;
      if (!handle || handle == Read32(std::span<const uint8_t>(base + kMissingFunction, 4), 0)) continue;
      ctx.r1.u64 = area - 256;
      ctx.r3.u64 = 0;
      ctx.r4.u64 = handle;
      ctx.r5.u64 = 0;
      bo2zm::guest::Call<kExecThread>(ctx, base);
      const auto thread = ctx.r3.u32;
      ctx.r1.u64 = area - 256;
      ctx.r3.u64 = 0;
      ctx.r4.u64 = thread;
      bo2zm::guest::Call<kFreeThread>(ctx, base);
      REXLOG_INFO("[gsc] started {}::{}", entry, function);
    }
    ctx = saved;
  }
}

uint32_t Lookup(const uint8_t* base, uint32_t name) {
  std::lock_guard lock(mutex);
  if (!active || name < 0x1000 || name > 0xFFFFFF7Fu) return 0;
  const char* p = reinterpret_cast<const char*>(base + name);
  size_t n = 0;
  while (n < 128 && p[n]) ++n;
  if (n == 128) return 0;
  const std::string_view requested(p, n);
  if (replacement && requested == replacement->source.file) return source_descriptor;
  for (auto& script : scripts) {
    if (requested != script.info.name) continue;
    if (!script.reported) {
      script.reported = true;
      REXLOG_INFO("[gsc] overriding {} ({} bytes, object {:08X})", requested, script.bytes.size(), script.descriptor);
    }
    return script.descriptor;
  }
  return 0;
}
}  // namespace

size_t MenuCodeRanges(MenuCodeRange* out, size_t capacity) {
  if (!out || !capacity) return 0;
  std::lock_guard lock(mutex);
  if (!menu_ranges_active) return 0;
  size_t count = 0;
  for (const auto& script : scripts) {
    if (!script.menu_code || !script.buffer) continue;
    const uint64_t begin = uint64_t(script.buffer) + Read32(script.bytes, 0x14);
    const uint64_t end = begin + script.info.code_size;
    if (begin >= end || end > UINT32_MAX) continue;
    out[count++] = {uint32_t(begin), uint32_t(end)};
    if (count == capacity) break;
  }
  return count;
}

bool MenuAncestry(const uint32_t* positions, size_t count) {
  if (!positions || !count || count > 31) return false;
  std::lock_guard lock(mutex);
  if (!menu_ranges_active) return false;
  bool menu = false;
  for (size_t frame = 0; frame < count; ++frame) {
    for (const auto& script : scripts) {
      if (!script.buffer) continue;
      const uint64_t begin = uint64_t(script.buffer) + Read32(script.bytes, 0x14);
      const uint64_t end = begin + script.info.code_size;
      if (end > UINT32_MAX || positions[frame] < begin || positions[frame] >= end) continue;
      if (!script.menu_code) return false;
      menu = true;
    }
  }
  return menu;
}

void Configure() {
  if (bo2zm::mode::IsZombies()) {
    try {
      auto bundle = ReadBotBundle(rex::filesystem::GetExecutableFolder() / "bo2zm_bots.gscpack");
      bot_component = Component{std::move(bundle.options), std::move(bundle.objects)};
      bo2zm::bots::SetAvailability(true, "Bot files ready");
    } catch (const std::exception& error) {
      bo2zm::bots::SetAvailability(false, error.what());
      REXLOG_INFO("[bots] optional bot bundle unavailable: {}", error.what());
    }
  }
  auto selected = bo2zm::scripts::Selected();
  if (selected.empty()) return;
  try {
    const auto target = bo2zm::mode::IsZombies() ? ScriptMode::Zombies : ScriptMode::Multiplayer;
    auto objects = ReadMod(bo2zm::scripts::Root(), selected, bo2zm::scripts::LegacySizes(), target);
    const auto options = ReadModOptions(bo2zm::scripts::Root() / selected);
    Component component{options, std::move(objects)};
    if (bot_component && SameComponent(component, *bot_component) &&
        bo2zm::bots::MigrateLegacySelection(selected)) {
      bo2zm::scripts::Boot();
      REXLOG_INFO("[bots] moved standalone bot selection to the independent bot setting");
      return;
    }
    size_t total = 0;
    for (const auto& object : component.objects) {
      if (object.info.legacy_header_strings)
        REXLOG_WARN("[gsc] {} uses {} legacy header literals, preserved unchanged", object.info.name,
                    object.info.legacy_header_strings);
      total += object.bytes.size();
    }
    const auto count = component.objects.size();
    menu_component = std::move(component);
    mod_name = selected;
    bo2zm::scripts::Configured(selected);
    REXLOG_INFO("[gsc] selected mod {}: {} validated scripts ({} bytes)", selected, count, total);
  } catch (const std::exception& e) {
    bo2zm::scripts::Rejected(e.what());
    REXLOG_ERROR("[gsc] mod rejected: {}; using stock scripts", e.what());
  }
}
}  // namespace bo2zm::gsc

// TU18 DB_FindXAssetHeader: ScriptParseTree type 50, name in r4, header in r3.
REX_HOOK_RAW(sub_822CAE50) {
  if (ctx.r3.u32 == 50) {
    const auto asset = bo2zm::gsc::Lookup(base, ctx.r4.u32);
    if (asset) { ctx.r3.u64 = asset; return; }
  }
  __imp__sub_822CAE50(ctx, base);
}

REX_HOOK_RAW(sub_8252DED0) {
  const auto instance = ctx.r3.u32;
  if (instance == 0) {
    bo2zm::gsc::InvalidateHudProvenance();
    bo2zm::cheats::BeginMapLoad();
  }
  __imp__sub_8252DED0(ctx, base);
  if (instance == 0) {
    bo2zm::gsc::SelectGeneration(ctx, base);
    // Registration may raise a guest Com_Error. Composition has returned, so
    // no host-owned plan or mutex guard crosses that guest longjmp boundary.
    if (bo2zm::gsc::active_bot_count &&
        !bo2zm::gsc::SetBotCount(ctx, base, bo2zm::gsc::active_bot_count)) {
      bo2zm::gsc::SelectGeneration(ctx, base, false);
      bo2zm::bots::RuntimeStatus("Bots unavailable: no memory for bot-count setting");
    }
    bo2zm::gsc::PrepareReplacement(ctx, base, true);
    bo2zm::gsc::NewGeneration(base, true);
    if (bo2zm::gsc::active && bo2zm::gsc::menu_permitted)
      bo2zm::scripts::Configured(bo2zm::gsc::mod_name);
    bo2zm::gsc::LoadEntry(ctx, base);
  }
}

REX_HOOK_RAW(sub_823599E8) {
  __imp__sub_823599E8(ctx, base);
  bo2zm::gsc::StartEntry(ctx, base);
  bo2zm::cheats::EndMapLoad();
  bo2zm::mod_hud_fit::EndMapLoad();
}

// The stock binder returns zero without naming an unresolved export in its
// final Com_Error. Keep the import identity in the host log for mod diagnosis.
REX_HOOK_RAW(sub_8252EED8) {
  const auto buffer = ctx.r4.u32, record = ctx.r5.u32;
  __imp__sub_8252EED8(ctx, base);
  if (ctx.r3.u32 || !bo2zm::gsc::active) return;
  const auto text = [&](uint32_t at) {
    const auto* start = reinterpret_cast<const char*>(base + at);
    size_t length = 0;
    while (length < 127 && start[length]) ++length;
    return std::string(start, length);
  };
  const auto half = [&](uint32_t at) {
    return bo2zm::gsc::Read16(std::span<const uint8_t>(base + at, 2), 0);
  };
  REXLOG_ERROR("[gsc] unresolved import in {}: {}::{} ({} arguments, flags {:02X})",
               text(buffer + half(buffer + 0x30)), text(buffer + half(record + 2)),
               text(buffer + half(record)), base[record + 6], base[record + 7]);
}

REX_HOOK_RAW(sub_8252F808) {
  if (!bo2zm::gsc::CanonicalOverflow(ctx, base)) __imp__sub_8252F808(ctx, base);
}
