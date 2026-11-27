/**
 * @file        rexcodegen/xdk_signatures.cpp
 * @brief       Built-in signatures of XDK C library routines; see xdk_signatures.h.
 *
 * @license     BSD 3-Clause License
 */
#include "xdk_signatures.h"

#include <array>

#include "codegen_logging.h"

namespace rex::codegen {

namespace {

// Rows from tools/xdk_sig.py (one 360 game image; checked to match exactly once in each of BO2, BO1,
// Hot Pursuit and Most Wanted 2012, and memmove/memset in Most Wanted 2005). All five keep r3 = dst and
// are implemented in kernel/crt/memory.cpp.
constexpr XdkSignature kSignatures[] = {
    {"memcpy", 290, {0xF861FFF8, 0x5466077E, 0x7C00222C, 0x28060000}, 0xFCA82A089414AC66ULL},
    {"memmove", 48, {0x7C032000, 0x4DC20020, 0x40E00000, 0x48000000}, 0x00261508117E2CD8ULL},
    {"memset", 40, {0x38050001, 0x7C0903A6, 0x60660000, 0x48000000}, 0xA4231FF77A4211EAULL},
    {"XMemCpy", 190, {0x7D8802A6, 0x48000001, 0x9421FF50, 0x7C7A1B78}, 0x0CC5E4EB49BB2417ULL},
    {"XMemSet", 104, {0x7D8802A6, 0x48000001, 0x7C6A1B78, 0x2B050010}, 0x70B4DD8B12A5D5F0ULL},
};

// setjmp and longjmp, in the two forms the XDK shipped: the early one (Most Wanted 2005, Gears of War) and the
// later one with the VMX registers (BO1, BO2, Forza Horizon, Fallout 3, Mass Effect 2, Hot Pursuit, Most Wanted
// 2012, Assassin's Creed). Each matches exactly once in those images, at the addresses BO1, BO2, Forza Horizon
// and Most Wanted 2005 used to name by hand.
constexpr XdkSignature kSetjmp[] = {
    {"setjmp", 179, {0x3C800000, 0x80040000, 0x2C000000, 0x7C0903A6}, 0xA5478B79EE2D52B8ULL},
    {"setjmp", 51, {0x3C800000, 0x80040000, 0x2C000000, 0x7C0903A6}, 0x54A02BA464E576F8ULL},
};
constexpr XdkSignature kLongjmp[] = {
    {"longjmp", 187, {0x7C0802A6, 0x9421FFB0, 0x90010008, 0x7C862378}, 0x7E27A9C5285F3084ULL},
    {"longjmp", 59, {0x7C0802A6, 0x9421FFB0, 0x90010008, 0x7C862378}, 0x16381A01EBFC63A4ULL},
};

uint32_t LoadBE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Masks the parts of each instruction the linker changes. Keep in step with tools/xdk_sig.py.
struct Masker {
  uint32_t lis_regs = 0;
  uint32_t operator()(uint32_t w) {
    const uint32_t op = w >> 26;
    if (op == 18)  // b/bl: displacement
      return w & 0xFC000003;
    if (op == 16)  // bc: displacement
      return w & 0xFFFF0003;
    if (op == 15) {  // lis: address high half
      lis_regs |= 1u << ((w >> 21) & 31);
      return w & 0xFFFF0000;
    }
    // addi and D-form loads/stores off a register a lis loaded: address low half
    constexpr uint64_t kDForm = (1ull << 14) | (1ull << 24) | (1ull << 32) | (1ull << 34) | (1ull << 36) |
                                (1ull << 38) | (1ull << 40) | (1ull << 42) | (1ull << 44) | (1ull << 48) |
                                (1ull << 50) | (1ull << 52) | (1ull << 54) | (1ull << 58) | (1ull << 62);
    if (((kDForm >> op) & 1) && ((lis_regs >> ((w >> 16) & 31)) & 1))
      return w & 0xFFFF0000;
    return w;
  }
};

uint64_t Fnv(uint64_t h, uint32_t w) {
  for (int s = 24; s >= 0; s -= 8)
    h = (h ^ ((w >> s) & 0xFF)) * 0x100000001B3ULL;
  return h;
}

}  // namespace

XdkSignature MakeXdkSignature(const char* key, std::span<const uint8_t> code) {
  XdkSignature sig{key, uint32_t(code.size() / 4), {}, 0xCBF29CE484222325ULL};
  Masker mask;
  for (uint32_t i = 0; i < sig.words; ++i) {
    const uint32_t m = mask(LoadBE(&code[4 * i]));
    if (i < 4)
      sig.first[i] = m;
    sig.hash = Fnv(sig.hash, m);
  }
  return sig;
}

std::vector<uint32_t> FindXdkSignature(const uint8_t* data, uint32_t size, uint32_t base,
                                       const XdkSignature& sig) {
  std::vector<uint32_t> hits;
  if (sig.words < 4 || size / 4 < sig.words)
    return hits;
  for (uint32_t i = 0; i + sig.words <= size / 4; ++i) {
    const uint8_t* p = data + 4 * i;
    Masker mask;
    bool anchored = true;
    for (int j = 0; j < 4 && anchored; ++j)
      anchored = mask(LoadBE(p + 4 * j)) == sig.first[j];
    if (!anchored)
      continue;
    if (MakeXdkSignature(sig.key, {p, 4 * size_t(sig.words)}).hash == sig.hash)
      hits.push_back(base + 4 * i);
  }
  return hits;
}

namespace {

// The routine's address when it is in the executable sections exactly once, else 0.
// shortcut: a routine linked in twice is left recompiled; give rexcrt keys a suffix if a game needs both.
uint32_t FindOnce(const BinaryView& binary, const XdkSignature& sig) {
  std::vector<uint32_t> hits;
  for (const auto& section : binary.sections()) {
    if (!section.executable || !section.data)
      continue;
    auto h = FindXdkSignature(section.data, section.size, section.baseAddress, sig);
    hits.insert(hits.end(), h.begin(), h.end());
  }
  if (hits.size() > 1)
    REXCODEGEN_WARN("[xdk] {} found {} times, left recompiled", sig.key, hits.size());
  return hits.size() == 1 ? hits[0] : 0;
}

bool HasDirectCall(const BinaryView& binary, uint32_t target) {
  for (const auto& section : binary.sections()) {
    if (!section.executable || !section.data)
      continue;
    for (uint32_t i = 0; i + 4 <= section.size; i += 4) {
      const uint32_t w = LoadBE(section.data + i);
      if ((w & 0xFC000003) == 0x48000001 &&  // bl
          section.baseAddress + i + (int32_t((w & 0x03FFFFFC) << 6) >> 6) == target)
        return true;
    }
  }
  return false;
}

}  // namespace

std::vector<std::pair<std::string, uint32_t>> FindXdkLibraryFunctions(const BinaryView& binary) {
  std::vector<std::pair<std::string, uint32_t>> found;
  for (const auto& sig : kSignatures)
    if (uint32_t addr = FindOnce(binary, sig))
      found.emplace_back(sig.key, addr);
  return found;
}

std::optional<std::pair<uint32_t, uint32_t>> FindXdkSetjmp(const BinaryView& binary) {
  uint32_t set = 0, jump = 0;
  for (const auto& sig : kSetjmp)
    if (!set)
      set = FindOnce(binary, sig);
  for (const auto& sig : kLongjmp)
    if (!jump)
      jump = FindOnce(binary, sig);
  // Both or neither: the codegen turns direct calls of the two into the host's setjmp/longjmp, and a longjmp
  // with no host setjmp behind it aborts. Assassin's Creed only calls setjmp through a pointer, so it stays
  // recompiled there.
  if (!set || !jump || !HasDirectCall(binary, set))
    return std::nullopt;
  return std::pair{set, jump};
}

}  // namespace rex::codegen
