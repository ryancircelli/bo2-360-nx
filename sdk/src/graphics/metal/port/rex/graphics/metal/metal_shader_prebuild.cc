/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_shader_prebuild.h"

#include <dlfcn.h>

#include <cstdio>
#include <system_error>
#include <vector>

#include "fmt/format.h"
#include "rex/cvar.h"
#include "rex/filesystem.h"
#include "rex/logging.h"

#include "metal_flags.h"

namespace rex {
namespace graphics {
namespace metal {
namespace {

// One directory level per work, like the pipeline cache's own storage.
std::filesystem::path PrebuildCacheRoot(const std::filesystem::path& cache_root,
                                        const std::string& abi_tag) {
  return cache_root / "shaders" / "shareable" / "metal-prebuild" / abi_tag;
}

bool CloneOrCopy(const std::filesystem::path& from,
                 const std::filesystem::path& to_dir) {
  std::error_code ec;
  std::filesystem::create_directories(to_dir, ec);
  if (ec) {
    return false;
  }
  // APFS clonefile first (the bundle and the cache usually share the volume):
  // an app-sized folder clones in milliseconds.
  auto clonefile = reinterpret_cast<int (*)(const char*, const char*, unsigned)>(
      dlsym(RTLD_DEFAULT, "clonefile"));
  for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
    const std::filesystem::path dst = to_dir / entry.path().filename();
    std::filesystem::remove(dst, ec);
    bool done = false;
    if (clonefile) {
      done = clonefile(entry.path().c_str(), dst.c_str(), 0) == 0;
    }
    if (!done) {
      std::filesystem::copy(entry.path(), dst,
                            std::filesystem::copy_options::overwrite_existing,
                            ec);
      done = !ec;
    }
    if (!done) {
      return false;
    }
  }
  return !ec;
}

}  // namespace

void ShaderPrebuild::Initialize(MTL::Device* device,
                                const std::filesystem::path& cache_root,
                                const std::string& abi_tag) {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  device_ = device;
  if (!device_) {
    return;
  }
  const std::string& setting = REXCVAR_GET(metal_shader_prebuild);
  if (setting == "off" || setting == "0") {
    REXLOG_INFO("[shader-prebuild] off (metal_shader_prebuild={})", setting);
    return;
  }
  if (!DiscoverSourceDir(setting)) {
    return;  // nothing shipped: the runtime path, unchanged
  }
  if (!StageIntoCache(cache_root, abi_tag)) {
    return;
  }
  if (!LoadIndex()) {
    REXLOG_WARN("[shader-prebuild] {}: no usable index.tsv",
                rex::path_to_utf8(staged_dir_));
    staged_dir_.clear();
    return;
  }
  enabled_.store(true, std::memory_order_release);
  REXLOG_INFO(
      "[shader-prebuild] {} shaders, {} binary archives staged from {} "
      "(key {})",
      shaders_.size(), archives_.size(), rex::path_to_utf8(source_dir_),
      abi_tag);
}

bool ShaderPrebuild::DiscoverSourceDir(const std::string& setting) {
  if (!setting.empty()) {
    // A folder path (cvar or REX_METAL_SHADER_PREBUILD): used in place.
    source_dir_ = setting;
    staged_dir_ = source_dir_;
    return true;
  }
  const std::filesystem::path exe = rex::filesystem::GetExecutableFolder();
  for (const std::filesystem::path& candidate :
       {exe / "shader_prebuild",
        exe.parent_path() / "Resources" / "shader_prebuild",
        exe.parent_path() / "Resources" / "runtime" / "shader_prebuild"}) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate / "index.tsv", ec)) {
      source_dir_ = candidate;
      return true;
    }
  }
  return false;
}

bool ShaderPrebuild::StageIntoCache(const std::filesystem::path& cache_root,
                                    const std::string& abi_tag) {
  if (source_dir_ == staged_dir_ && !staged_dir_.empty()) {
    return true;  // cvar path: used in place, no staging
  }
  const std::filesystem::path target = PrebuildCacheRoot(cache_root, abi_tag);
  std::error_code ec;
  if (std::filesystem::is_regular_file(target / "index.tsv", ec)) {
    staged_dir_ = target;  // staged by an earlier launch of this build
    return true;
  }
  if (!CloneOrCopy(source_dir_, target)) {
    REXLOG_WARN("[shader-prebuild] could not stage {} into {}",
                rex::path_to_utf8(source_dir_), rex::path_to_utf8(target));
    return false;
  }
  staged_dir_ = target;
  return true;
}

bool ShaderPrebuild::LoadIndex() {
  std::FILE* f =
      std::fopen((staged_dir_ / "index.tsv").string().c_str(), "rb");
  if (!f) {
    return false;
  }
  char line[1024];
  bool any = false;
  while (std::fgets(line, sizeof(line), f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
      s.pop_back();
    }
    if (s.empty()) {
      continue;
    }
    const size_t t1 = s.find('\t');
    const size_t t2 = t1 == std::string::npos ? t1 : s.find('\t', t1 + 1);
    const size_t t3 = t2 == std::string::npos ? t2 : s.find('\t', t2 + 1);
    if (t2 == std::string::npos) {
      continue;
    }
    const std::string kind = s.substr(0, t1);
    if (kind == "shader") {
      if (t3 == std::string::npos) {
        continue;
      }
      ShaderEntry e;
      e.file = s.substr(t1 + 1, t2 - t1 - 1);
      e.vertex = s.substr(t2 + 1, t3 - t2 - 1) == "vs";
      e.entry = s.substr(t3 + 1);
      shaders_[e.file] = std::move(e);
    } else if (kind == "archive") {
      archives_[s.substr(t1 + 1, t2 - t1 - 1)] = s.substr(t2 + 1);
    }
    any = true;
  }
  std::fclose(f);
  return any && (!shaders_.empty() || !archives_.empty());
}

bool ShaderPrebuild::LookupLibrary(bool vertex, uint64_t ucode_hash,
                                   uint64_t modification,
                                   MTL::Library** library_out,
                                   MTL::Function** function_out,
                                   std::string* entry_out) {
  if (!enabled()) {
    return false;
  }
  const std::string key =
      fmt::format("{}_{:016X}_{:X}.metallib", vertex ? "vs" : "ps", ucode_hash,
                  modification);
  const auto it = shaders_.find(key);
  if (it == shaders_.end()) {
    return false;
  }
  const ShaderEntry& e = it->second;
  NS::String* path = NS::String::string(
      (staged_dir_ / e.file).string().c_str(), NS::UTF8StringEncoding);
  NS::Error* error = nullptr;
  MTL::Library* library = device_->newLibrary(
      NS::URL::fileURLWithPath(path), &error);
  if (!library) {
    REXLOG_WARN("[shader-prebuild] {} failed to load: {}",
                e.file, error ? error->localizedDescription()->utf8String()
                              : "unknown error");
    return false;
  }
  NS::String* entry_name = NS::String::string(e.entry.c_str(), NS::UTF8StringEncoding);
  MTL::Function* function = library->newFunction(entry_name);
  if (!function) {
    REXLOG_WARN("[shader-prebuild] {} has no entry {}", e.file, e.entry);
    library->release();
    return false;
  }
  *library_out = library;
  *function_out = function;
  *entry_out = e.entry;
  ++lookup_hits_;
  return true;
}

std::filesystem::path ShaderPrebuild::ArchiveForSet(
    const std::string& set_suffix) const {
  if (!enabled()) {
    return {};
  }
  const auto it = archives_.find(set_suffix);
  if (it == archives_.end()) {
    return {};
  }
  return staged_dir_ / it->second;
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
