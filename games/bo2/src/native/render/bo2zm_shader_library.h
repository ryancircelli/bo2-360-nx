// The SPIR-V shader library: the game's shaders translated by XenosRecomp, each kept with its original
// container, in one versioned pack file that is read one shader at a time (ShaderLibrary).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace bo2zm::shaderlib {

// The original container also identifies constants and interface. The physical
// code alone is not enough. It is looked up before the driver creates its object.
class ShaderLibrary;

struct Shader {
  std::vector<uint8_t> original;
  // Read it through Spirv(). Loaded from a pack file it is never kept: Spirv() reads it from the file.
  std::vector<uint32_t> spirv;
  // XXH3 of the SPIR-V, taken while the pack loads; Spirv() checks what it reads against it.
  uint64_t spirv_hash = 0;
  uint64_t hash = 0;
  bool vertices = false;
  uint64_t container_offset = 0;  // where the container is in the pack file (Scan)
  uint64_t spirv_offset = 0;  // where the SPIR-V is in the pack file; 0 = only in memory
  uint32_t spirv_words = 0;
  const ShaderLibrary* library = nullptr;
};

// The shader's SPIR-V, read from its pack file when it was released (any thread), and not kept: the
// driver copies it into the shader module made from it (vk_common_CreateShaderModule), which is what stays. Empty if
// the file could not be read or what it read does not match spirv_hash.
std::vector<uint32_t> Spirv(const Shader& shader);

// What a pack's header says (ShaderLibrary::Open).
struct PackInfo {
  uint64_t size = 0;       // bytes in the file
  uint32_t amount = 0;     // shaders
  uint64_t body_hash = 0;  // XXH3 of everything after the header, as the header gives it
};

class ShaderLibrary {
 public:
  // Transactional load: an invalid file does not replace the library.
  // Pointers returned by Find last until the next successful load.
  void Load(std::span<const uint8_t> file);
  // A pack file is read one shader at a time. inspect (if given) sees each shader, its position in the
  // library and its SPIR-V; then the SPIR-V is dropped (Spirv reads it back when needed), so loading never holds more
  // than one shader's. With the DLC maps the pack is 1.1 GB of SPIR-V (16,149 shaders): holding all of it until the
  // end of the load runs a Switch Lite out of memory.
  using Inspect = std::function<void(uint32_t index, const Shader& shader)>;
  void Load(const std::filesystem::path& file, const Inspect& inspect = {});
  // Reads every shader of a pack file with the same checks as Load(path), and keeps none of them: scan
  // sees each one (its container, SPIR-V and their places in the file). Afterwards Container and Spirv read from
  // that file. For an index of the pack kept elsewhere (bo2zm_native_shaders.cpp).
  void Scan(const std::filesystem::path& file, const Inspect& scan);
  // Only the header of a pack file, checked as Load(path) checks it; afterwards Container and Spirv
  // read from that file. The shaders themselves are not read: for an index made earlier by Scan.
  PackInfo Open(const std::filesystem::path& file);
  // One container from the file Scan or Open used (offset and size as Scan gave them), any thread.
  // Empty if it could not be read or does not match hash (its XXH3, as in the pack).
  std::vector<uint8_t> Container(uint64_t offset, uint32_t size, uint64_t hash) const;
  // The SPIR-V words of every shader loaded from a file (all of it stays in the file).
  uint64_t SpirvWords() const;
  const Shader* Find(std::span<const uint8_t> original) const;
  const std::vector<Shader>& shaders() const { return shaders_; }

 private:
  friend std::vector<uint32_t> Spirv(const Shader& shader);
  void Read(const std::filesystem::path& file, const Inspect& inspect, bool keep);
  void UseFile(const std::filesystem::path& file);
  // Reads bytes at offset from file_ into to, under spirv_mutex_; false if it could not.
  bool ReadAt(uint64_t offset, void* to, size_t bytes) const;
  std::vector<Shader> shaders_;
  std::filesystem::path file_;  // the pack Load(path), Scan or Open read
  mutable std::mutex spirv_mutex_;
  mutable std::ifstream spirv_file_;  // under spirv_mutex_, kept open for Spirv and Container
};

// Versioned local format, little-endian, with integrity checks.
// It contains private data derived from the game; it is not embedded in the executable.
std::vector<uint8_t> PackShaders(std::vector<Shader> shaders);

}  // namespace bo2zm::shaderlib
