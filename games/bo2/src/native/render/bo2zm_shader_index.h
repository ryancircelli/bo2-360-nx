// The shader library's index (bo2zm_shaders.spvidx beside the pack) and the container reader it is made with.
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// With the DLC maps the pack holds about 16,000-24,000 shaders (1.1-1.7 GB, mostly SPIR-V). Reading and hashing all
// of it and parsing every container would take more than a minute of every start on a Switch, and keep every
// container in memory. The index holds, for each shader, only what identifying it needs: its container's hashes,
// position and size, its microcode's length and (masked) hash, a vertex shader's fetch instructions, and what its
// SPIR-V says (kills, the shadow minimum). It is made once by reading the whole pack (BuildShaderIndex) and kept next
// to it; a start reads only the index as long as the pack's size, shader count and body hash match.
//
// The index depends on nothing but the pack, so it can also be made offline. This file uses nothing from the
// runtime: the host packer (tools/xenosrecomp/bo2zm_pack.cpp --index) compiles it as it is and writes the same
// bytes the game would.

#pragma once

#include "bo2zm_native_shaders.h"
#include "bo2zm_shader_library.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bo2zm::native {

// Bits of a vertex fetch instruction that D3D does not touch (VertexFetchInstruction in XenosRecomp shader_code.h):
// opcode, source and destination registers, destination swizzle and predicate. The rest (fetch constant, format,
// stride, offset and numeric modes) comes from the declaration.
inline constexpr uint32_t kFetchKeeps[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};
inline constexpr uint32_t kFetchNothing[3] = {0, 0, 0};  // BO2 vertex shaders

// Reads what identifying a shader needs from its original container (see the .cpp). Returns the reason if it
// cannot be read. analyze, if given, runs on the microcode before its fetches are masked: the game passes xenia's
// constant analysis there; the index needs none of it.
using AnalyzeMicrocode = void (*)(ShaderInput& e);
const char* ReadContainer(const bo2zm::shaderlib::Shader& shader, ShaderInput& e, AnalyzeMicrocode analyze = nullptr);

// OpKill, OpTerminateInvocation and OpDemoteToHelperInvocation in the module; 2 (the conservative answer: the
// fragment stage is kept) if the SPIR-V cannot be walked.
uint32_t CountKills(const std::vector<uint32_t>& spirv);
// Whether a pixel shader's SPIR-V carries tfetch2DShadowMin (bo2zm_native_shadow_min).
bool HasMarkShadowMin(const std::vector<uint32_t>& spirv);

struct IndexEntry {
  uint64_t container_hash = 0;      // XXH3, the pack's order
  uint64_t container_hash_low = 0;  // XXH3-128 of the container: with the size, what IdentifyContainer matches
  uint64_t container_hash_high = 0;
  uint64_t container_offset = 0;
  uint32_t container_size = 0;
  uint32_t spirv_words = 0;
  uint64_t spirv_hash = 0;
  uint64_t microcode_hash = 0;  // what Identify compares: hash, or hash_no_fetch for a fetch_full vertex shader
  uint32_t microcode_words = 0;
  uint32_t fetches_start = 0;  // its fetch instructions in ShaderIndex::fetches (vertex shaders)
  uint16_t fetches_count = 0;
  uint8_t flags = 0;
  uint8_t kills = 0;
};
constexpr uint8_t kIndexVertices = 1, kIndexFetchFull = 2, kIndexShadowMin = 4, kIndexReadable = 8;

// The start's log lines, counted when the index is made.
struct IndexStats {
  uint32_t vertex = 0, pixel = 0, ignored = 0, repeated = 0, repeated_different = 0, with_kill = 0, no_kill = 0,
           with_map_shadows = 0, with_min = 0;
};

struct ShaderIndex {
  std::vector<IndexEntry> entries;  // library order
  std::vector<uint16_t> fetches;
  IndexStats stats;
};

struct IndexEvents {
  // A container the index marks unusable, and why.
  std::function<void(uint32_t number, const char* reason)> ignored;
  // After each shader: the bytes of the pack read so far, and its size.
  std::function<void(uint64_t done, uint64_t total)> progress;
};

// Reads the whole pack (library.Scan, which checks it as a load does and throws if it is damaged) and makes its
// index. pack is what library.Open said about the same file.
void BuildShaderIndex(bo2zm::shaderlib::ShaderLibrary& library, const std::filesystem::path& pack_file,
                      const bo2zm::shaderlib::PackInfo& pack, ShaderIndex& index, const IndexEvents& events = {});

// The index file's bytes. Version kIndexVersion (the .cpp).
std::vector<uint8_t> EncodeShaderIndex(const ShaderIndex& index, const bo2zm::shaderlib::PackInfo& pack);

// Reads an index file's bytes into index. Returns nullptr, or why it cannot be used for that pack (and index is
// left empty).
const char* DecodeShaderIndex(std::span<const uint8_t> bytes, const bo2zm::shaderlib::PackInfo& pack,
                              ShaderIndex& index);

// Writes the file beside its final name and renames it, so a start cut short never leaves half an index. false,
// with the reason in error, if it could not.
bool SaveShaderIndex(const std::filesystem::path& file, std::span<const uint8_t> bytes, std::string& error);

// Where a pack's index lives: the same name with .spvidx.
std::filesystem::path ShaderIndexPath(const std::filesystem::path& pack_file);

}  // namespace bo2zm::native
