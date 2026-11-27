/**
 * @file        rexcodegen/xdk_signatures.h
 * @brief       Recognise XDK C library routines (memcpy, memset, ...) by their instructions, so the codegen
 *              can call the native [rexcrt] versions in every game without a per-game address table.
 *
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rex/codegen/binary_view.h>

namespace rex::codegen {

/// A routine's instruction words with addresses masked out: branch displacements, lis immediates and
/// D-form offsets from registers a lis loaded. `first` anchors the scan, `hash` (FNV-1a 64 over all
/// masked words, big-endian) confirms it.
struct XdkSignature {
  const char* key;  // [rexcrt] key: the codegen calls rexcrt_<key>
  uint32_t words;
  uint32_t first[4];
  uint64_t hash;
};

/// Builds a signature from big-endian code (used by tests and to make new table rows).
XdkSignature MakeXdkSignature(const char* key, std::span<const uint8_t> code);

/// Guest addresses in [data, data+size) (big-endian code at guest address base) matching sig.
std::vector<uint32_t> FindXdkSignature(const uint8_t* data, uint32_t size, uint32_t base,
                                       const XdkSignature& sig);

/// (key, address) for every built-in signature found exactly once in the executable sections.
std::vector<std::pair<std::string, uint32_t>> FindXdkLibraryFunctions(const BinaryView& binary);

/// (setjmp, longjmp) when the image holds the XDK's pair and calls setjmp directly: what a game config would
/// otherwise name as setjmp_address / longjmp_address.
std::optional<std::pair<uint32_t, uint32_t>> FindXdkSetjmp(const BinaryView& binary);

}  // namespace rex::codegen
