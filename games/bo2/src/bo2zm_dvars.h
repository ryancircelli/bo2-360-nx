// The game's dvars, found by name. Every dvar the game registers is recorded here by the registration hooks (in
// bo2zm_autoexec.cpp and bo2zm_preset.cpp), so the rest of the port can read and set them. A dvar's layout, as the
// game's own code uses it: flags at +0xC (0x80 means cheat-protected), type at +0x10 and the current value at +0x18
// (a byte for a bool, an int for an int or an enum, a float for a float).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rex/ppc.h>

namespace bo2zm::dvars {

enum Kind { kBool = 0, kInt = 1, kFloat = 2, kEnum = 3 };  // which register function made it

struct Dvar {
  uint32_t pointer = 0;
  int kind = kBool;
};

// From the dvar registration hooks (any thread): name and dvar are guest addresses.
void Registered(const uint8_t* base, uint32_t name, uint32_t dvar, int kind);

// The dvar with this name, once the game has registered it.
std::optional<Dvar> Find(std::string_view name);

// The current value as console text ("1", "39", "0.175").
std::string Text(const uint8_t* base, const Dvar& dvar);

// Whether the current value is value (console text; floats within a small tolerance).
bool Matches(const uint8_t* base, const Dvar& dvar, std::string_view value);

// Runs console commands with the cheat flag cleared on the given dvars (guest pointers), then puts the flag back, so
// they take whatever sv_cheats is. Main thread only (bo2zm::autoexec::ExecuteCommands).
void ExecuteIgnoringCheats(PPCContext& ctx, uint8_t* base, std::string_view commands, std::span<const uint32_t> dvars);

}  // namespace bo2zm::dvars
