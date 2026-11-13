// Native renderer, part C5a: identify in the SPIR-V library the
// shaders the game's D3D uploads to the ring (PM4_IM_LOAD and
// IM_LOAD_IMMEDIATE).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The ring only carries the microcode. The library stores the original
// compiled container (header, constant table, definitions and microcode)
// with its SPIR-V translation made by XenosRecomp.
//
// Pixel shaders arrive as is and are identified by their microcode. Vertex
// shaders do not: D3D reorders the fetches, changes their swizzles and
// nulls the outputs the pixel shader does not read (measured), so their
// identity comes from the device objects (bo2zm_native_hooks.h) and only
// the vertex input is read from the patched microcode.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace bo2zm::shaderlib {
struct Shader;
}

namespace bo2zm::native {

// Vertex declaration element of the container (XenosRecomp shader.h).
struct VertexElement {
  uint16_t instruction = 0;  // index of the fetch instruction in the microcode
  uint8_t use = 0;           // DeclUsage: 0 position, 3 normal, 5 texcoord, 10 color...
  uint8_t index_use = 0;
};

// Sampler from the constant table: the register is the texture's fetch constant
// in pixel shaders.
struct SamplerShader {
  uint16_t reg_index = 0;
  uint16_t type = 0;  // D3DXPARAMETER_TYPE: 10-12 = 2D, 13 = 3D, 14 = cube
  // It is called SHADOWMAP_SAMPLER in the constant table: it is the shadow map sampling the library rewrites
  // (tfetch2DShadow and tfetch2DShadowMin). bo2zm_native_shadow_min.
  bool map_shadows = false;
};

struct ShaderInput {
  const bo2zm::shaderlib::Shader* shader = nullptr;  // original container and SPIR-V
  bool vertices = false;
  uint32_t number = 0;  // position in the library, for the reports
  std::vector<uint32_t> microcode;  // in host byte order, with the fetches masked
  uint64_t hash = 0;                // XXH3 of that masked microcode
  // BO2 (2008 containers) patches whole 12-byte vertex-fetch instructions, more bits than
  // kFetchKeeps leaves out, so its vertex shaders are compared with those instructions zeroed entirely, in
  // this copy.
  bool fetch_full = false;
  std::vector<uint32_t> microcode_no_fetch;
  uint64_t hash_no_fetch = 0;
  std::vector<VertexElement> elements;
  std::vector<SamplerShader> samplers;
  uint32_t outputs = 0;  // pixel shader: bits COLOR0..3 and DEPTH (PixelShaderOutputs)
  // OpKill in the SPIR-V. XenosRecomp puts one in every pixel shader for the alpha test (guarded by
  // SPEC_CONSTANT_ALPHA_TEST), so it only really discards if there is more than one.
  uint32_t kills = 0;
  bool discards = false;
  // Its SPIR-V has tfetch2DShadowMin (the BO2ZM_SHADOW_MIN_MARKER constant from shader_common.h): with
  // kSpecShadowMin it takes the minimum of the shadow map and its pair. bo2zm_native_shadow_min.
  bool shadow_min = false;
  // Bytes of its float constant buffer that the SPIR-V reads (16 per register).
  uint32_t constants_bytes = 256 * 16;
  // The float constant registers its microcode reads, as runs (first register, count), from xenia's
  // analysis (rex::graphics::Shader::AnalyzeUcode). Valid only when it addresses none of them dynamically: a draw
  // then copies only these (bo2zm_native_constants_used); the rest of the block is never read.
  std::vector<std::pair<uint16_t, uint16_t>> constants_runs;
  bool constants_runs_valid = false;
  uint32_t constants_runs_bytes = 0;
};

struct NativeShaderStats {
  uint64_t loads = 0;           // IM_LOADs received
  uint64_t different = 0;        // distinct microcodes (with their patches)
  uint64_t identified = 0;    // of the distinct ones
  uint64_t no_identify = 0;  // of the distinct ones
  uint64_t ambiguous = 0;         // distinct ones with more than one possible container
};

class NativeShaders {
 public:
  NativeShaders();
  ~NativeShaders();

  // While the index is made (the first start with a pack): the bytes of the pack read so far and its size, after
  // each shader, on the thread that called Load.
  using IndexProgress = std::function<void(uint64_t done, uint64_t total)>;

  // false, with a warning in the log, if the library is missing or damaged. Reads the pack's index
  // (bo2zm_shader_index.h), or makes it from the whole pack and saves it, calling progress as it goes.
  bool Load(const std::filesystem::path& file, const IndexProgress& progress = {});
  bool loaded() const;
  // Which pack is loaded (a hash of its size, shader count and body hash; 0 when none is), for the
  // pipeline precompile's stamp (bo2zm_native_precompile.h).
  uint64_t Identity() const;

  // Full original container (the one the D3D constructors receive).
  // nullptr if it is not in the library. Can be called from any thread
  // once the library is loaded.
  const ShaderInput* IdentifyContainer(std::span<const uint8_t> container) const;

  // The number of the entry IdentifyContainer would return, or -1, without making the entry (BO2
  // registers every shader of a zone, ~4,700 in Town, and only the ones drawn should be made: index-only
  // loading). From any thread once loaded.
  int32_t NumberOfContainer(std::span<const uint8_t> container) const;

  // The entry with that number (its position in the library), or nullptr. Like IdentifyContainer, from any
  // thread once loaded: an entry is made the first time it is asked for (read from the pack) and then never
  // changes. Pipeline prewarming (bo2zm_native_draws.cpp) uses it to recreate the pipelines of earlier launches.
  const ShaderInput* PerNumber(uint32_t number) const;

  // Microcode already in host byte order. nullptr if it is not in the library.
  // Only the ring thread uses it.
  const ShaderInput* Identify(bool vertices, std::span<const uint32_t> microcode);

  NativeShaderStats Stats() const;

 private:
  struct Data;
  void CheckIndex(const std::filesystem::path& file) const;  // bo2zm_native_shader_index_check
  std::unique_ptr<Data> data_;
};

// Short name of a DeclUsage for the reports.
const char* NameUse(uint8_t use);

// Whether a patched microcode from the ring can belong to that vertex shader: every
// declaration element has, at one of the element positions, a fetch that writes the
// same temporary register as the original. D3D reorders the fetches and changes
// formats and swizzles, but not the registers.
bool FetchCoherent(const ShaderInput& vs, std::span<const uint32_t> patched);

}  // namespace bo2zm::native
