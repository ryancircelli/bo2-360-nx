// Shipped shader prebuild (tools/mac/shader_prebuild.py): build-time-compiled
// shader state that cuts the first-launch seed.
//
// The app bundle carries a "shader_prebuild" folder (built at app build time,
// beside the shader seed):
//   index.tsv     one line per artifact: "shader <file> <vs|ps> <entry>" for a
//                 translated shader library (the FABLE2_NS_DUMP_DIR harvest,
//                 translated Metal that CompileNativeXlate would otherwise
//                 produce at runtime), "archive <set-suffix> <file>" for a
//                 metal-tt universal binary archive of the seed's pipelines.
//   *.metallib    the translated shader libraries, keyed
//                 <vs|ps>_<ucode hash>_<modification>.metallib.
//   *.gpubin      metal-tt binary archives (GPU binaries for every targeted
//                 Apple GPU), one per pipeline archive set.
//
// First launch stages the folder into the shader cache, keyed by the shader
// storage ABI tag, so it survives app updates and a build that changes the
// translator or pipeline formats leaves it unused (a stale prebuild can never
// be picked up):
//   <cache_root>/shaders/shareable/metal-prebuild/<abi tag>/
// Pipeline creation then finds the GPU binaries in the staged archives (a
// lookup, not a compile), and native translations install from the staged
// libraries instead of translating + compiling MSL. Everything missing falls
// back to the ordinary runtime path; nothing here is needed for correctness.
//
// The metal_shader_prebuild cvar (REX_METAL_SHADER_PREBUILD) selects the
// folder: empty = shader_prebuild beside the executable / in the app bundle's
// Resources, 'off'/'0' = disabled, else a folder path used in place (for
// development and harvesting).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace rex {
namespace graphics {
namespace metal {

class ShaderPrebuild {
 public:
  // Discovers the shipped folder, stages it under the cache root keyed by the
  // ABI tag, and reads the index. Safe to call repeatedly (later calls are
  // no-ops). Without a device, or when nothing shipped, enabled() is false.
  void Initialize(MTL::Device* device, const std::filesystem::path& cache_root,
                  const std::string& abi_tag);

  bool enabled() const { return enabled_.load(std::memory_order_acquire); }
  const std::filesystem::path& staged_dir() const { return staged_dir_; }

  // CompileNativeXlate hook: on a store hit, loads the staged library file and
  // returns its entry point, skipping the translate + MSL compile. Only the
  // plain variant (no baked constants / point / rectangle / tessellation
  // variants) is prebuilt. Returns false when there is no hit or the library
  // fails to load (the caller falls back to the normal path).
  bool LookupLibrary(bool vertex, uint64_t ucode_hash, uint64_t modification,
                     MTL::Library** library_out, MTL::Function** function_out,
                     std::string* entry_out);

  // Number of translations served from the store (for the seed report).
  uint64_t lookup_hits() const { return lookup_hits_; }

  // Path of the staged metal-tt binary archive for a pipeline archive set
  // suffix (path_suffixes in the pipeline cache: "rtv-bindless",
  // "rtv-bindless.ns1", ...), or empty when the prebuild ships none for it.
  std::filesystem::path ArchiveForSet(const std::string& set_suffix) const;

 private:
  bool DiscoverSourceDir(const std::string& setting);
  bool StageIntoCache(const std::filesystem::path& cache_root,
                      const std::string& abi_tag);
  bool LoadIndex();

  MTL::Device* device_ = nullptr;
  bool initialized_ = false;  // Initialize ran (succeed or not); CP thread only
  std::atomic<bool> enabled_{false};  // set last, after the maps below
  std::filesystem::path source_dir_;  // the shipped folder (bundle)
  std::filesystem::path staged_dir_;  // source_dir_, or the staged copy
  struct ShaderEntry {
    bool vertex = false;
    std::string file;
    std::string entry;
  };
  std::unordered_map<std::string, ShaderEntry> shaders_;  // key -> entry
  std::unordered_map<std::string, std::string> archives_; // set suffix -> file
  uint64_t lookup_hits_ = 0;
};

}  // namespace metal
}  // namespace graphics
}  // namespace rex
