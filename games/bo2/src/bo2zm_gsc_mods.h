// Optional compiled GSC overrides for the Zombies server VM.
#pragma once
#include <cstddef>
#include <cstdint>
namespace bo2zm::gsc {
void Configure();
struct MenuCodeRange { uint32_t begin = 0, end = 0; };
// Current validated menu bytecode only. Stock clones and bot components are
// excluded. Published during fresh generation linking/startup; invalidated
// before begin-load and known startup rejection. No guest calls.
size_t MenuCodeRanges(MenuCodeRange* out, size_t capacity);
// True only for a current menu-only frame, with no active bot/shared module
// anywhere in the supplied synchronous call chain. PCs are never dereferenced.
bool MenuAncestry(const uint32_t* positions, size_t count);
}
