// Diagnostic: log what the party member counter sees (bo2zm_debug_party).
//
// sub_822875A8(party) counts "real" party members; the lobby compares it to
// the party_minplayers dvar and shows "Cannot begin the game. Minimum 1
// players." when it is lower. From the disassembly:
//   - returns 0 when the word at party+0x95A0 is 0
//   - else, over 18 slots of 0x148 bytes, counts slots whose state byte at
//     party+0x208+i*0x148 is >= 3 and whose word at party+0x324+i*0x148 has
//     bit 0x2 clear.
// The hook logs those fields whenever they change, then runs the original.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

REXCVAR_DEFINE_BOOL(bo2zm_debug_party, false, "BO2",
                    "Log the party member counter's inputs (sub_822875A8) when they change");

namespace {

#if !defined(__SWITCH__)
REXCVAR_DEFINE_BOOL(bo2zm_test_exit_on_error, false, "BO2",
                    "PC test helper: flush the log and exit at the first Com_Error instead of restarting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

REX_EXTERN(__imp__sub_822875A8);
REX_HOOK_RAW(sub_822875A8) {
  const uint32_t party = ctx.r3.u32;
  __imp__sub_822875A8(ctx, base);
  if (!REXCVAR_GET(bo2zm_debug_party)) {
    return;
  }
  const uint32_t count = ctx.r3.u32;
  const uint8_t* p = base + party;
  std::string slots;
  for (int i = 0; i < 18; i++) {
    const uint8_t state = p[0x208 + i * 0x148];
    const uint32_t flags = LoadBE32(p + 0x324 + i * 0x148);
    if (state || flags) {
      slots += fmt::format(" [{}: state {} flags {:08X}]", i, state, flags);
    }
  }
  const std::string line = fmt::format("party {:08X} active {:08X} count {}{}", party,
                                       LoadBE32(p + 0x95A0), count, slots);
  static std::mutex mutex;
  static std::string last;
  std::lock_guard lock(mutex);
  if (line != last) {
    last = line;
    REXLOG_INFO("[party] {} (lr {:08X})", line, uint32_t(ctx.lr));
  }
}

// Com_Error(type, fmt, ...) = sub_8240C468 (spills r5-r10 for the varargs; when
// raised inside a task callback the game replaces the message with "Com_Error
// called within a task success/failure callback"). Always logged: the original
// message is what tells us what failed.
namespace {

// Escaped text at a guest address, or "" if it isn't a printable string.
std::string GuestText(uint8_t* base, uint32_t value, bool escape_bytes = false) {
  if (value < 0x10000 || value >= 0xFFFF0000) {
    return {};
  }
  const char* s = reinterpret_cast<const char*>(base + value);
  std::string text;
  size_t n = 0;
  for (; n < 4096 && s[n]; n++) {
    const char c = s[n];
    if (c == '\n') {
      text += "\\n";
    } else if (c == '\t') {
      text += "\\t";
    } else if (c == '\r') {
      text += "\\r";
    } else if (c >= 0x20 && c < 0x7F) {
      text += c;
    } else if (escape_bytes) {
      text += fmt::format("\\x{:02X}", uint8_t(c));
    } else {
      return {};  // not a text string
    }
  }
  return n == 4096 ? text + "..." : text;
}

// Image range only: other addresses may be unmapped.
std::string GuestStringOrHex(uint8_t* base, uint32_t value) {
  if (value >= 0x82000000 && value < 0x84CE0000) {
    const std::string text = GuestText(base, value);
    if (text.size() >= 2) {
      return fmt::format("\"{}\"", text);
    }
  }
  return fmt::format("{:#x}", value);
}

// The conversions of a printf format, in order ('s', 'd', ...).
std::string Conversions(const std::string& format) {
  std::string out;
  for (size_t i = 0; i < format.size(); i++) {
    if (format[i] != '%') {
      continue;
    }
    i++;
    while (i < format.size() && std::strchr("-+ #0123456789.*hlLqjzt", format[i])) {
      i++;
    }
    if (i < format.size() && format[i] != '%') {
      out += format[i];
    }
  }
  return out;
}

// The varargs in r5..r8, formatted by the conversions of the format: a %s
// argument is read as text whatever its address (stack strings included),
// since the game's own vsnprintf reads it right after.
std::string ComErrorArgs(uint8_t* base, uint32_t format_address, const uint32_t (&args)[4]) {
  const std::string format = GuestText(base, format_address);
  const std::string conv = Conversions(format);
  std::string out;
  for (size_t i = 0; i < 4; i++) {
    if (!out.empty()) {
      out += ' ';
    }
    const bool is_string = i < conv.size() && conv[i] == 's';
    const std::string text = is_string ? GuestText(base, args[i], true) : std::string();
    out += !text.empty() ? fmt::format("\"{}\"", text) : GuestStringOrHex(base, args[i]);
  }
  return out;
}

}  // namespace

REX_EXTERN(__imp__sub_8240C468);
REX_HOOK_RAW(sub_8240C468) {
  const uint32_t args[4] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  REXLOG_ERROR("[Com_Error] type {} fmt {} args {} (lr {:08X})", ctx.r3.u32,
               GuestStringOrHex(base, ctx.r4.u32), ComErrorArgs(base, ctx.r4.u32, args),
               uint32_t(ctx.lr));
  rex::FlushLogging();
#if !defined(__SWITCH__)
  if (REXCVAR_GET(bo2zm_test_exit_on_error)) std::_Exit(EXIT_FAILURE);
#endif
  __imp__sub_8240C468(ctx, base);
}
