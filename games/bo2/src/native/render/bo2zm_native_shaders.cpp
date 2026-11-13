// Native renderer, part C5a (see bo2zm_native_shaders.h).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Containers are read, and the library index is made, written and read, in bo2zm_shader_index.cpp.

#include "bo2zm_native_shaders.h"
#include "../bo2zm_native_system.h"

#include "bo2zm_shader_index.h"
#include "bo2zm_shader_library.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <exception>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read keys[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#if defined(__SWITCH__)
#include <rex/graphics/pipeline/shader/shader.h>  // AnalyzeConstants
#include <rex/string/buffer.h>
#endif

REXCVAR_DEFINE_BOOL(bo2zm_native_shader_index_check, false, "BO2",
                    "Test: at start the whole shader pack is also read as before its index (bo2zm_shaders.spvidx) "
                    "and every shader made from the index is compared with it (log: \"index check\"). Slow and "
                    "uses much memory: for the PC")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

// The float constants a shader's microcode reads, from xenia's analysis of it. Without this, a draw has to
// copy the whole block up to the shader's highest register, or all 256 registers whenever its constant table has an
// entry of more than one register; on the console that made the VS copies ~2 KB each on 59 % of the draws, 0.72 us
// each into the uncached upload buffer (~3.4 GB/s). Without dynamic addressing the SPIR-V reads exactly the registers
// the instructions name, so only those need copying. On the PC the analysis lives in the GPU plugin, a DLL the program
// does not link, so there every draw still copies the whole block.
void AnalyzeConstants(bo2zm::native::ShaderInput& e) {
  e.constants_runs.clear();
  e.constants_runs_valid = false;
  e.constants_runs_bytes = 0;
  if (e.microcode.empty()) {
    return;
  }
#if defined(__SWITCH__)
  try {
    rex::graphics::Shader analysis(
        e.vertices ? rex::graphics::xenos::ShaderType::kVertex : rex::graphics::xenos::ShaderType::kPixel, 0,
        e.microcode.data(), e.microcode.size(), std::endian::native);
    rex::string::StringBuffer disassembly;
    analysis.AnalyzeUcode(disassembly);
    const auto& map = analysis.constant_register_map();
    if (map.float_dynamic_addressing) {
      return;
    }
    for (uint32_t reg = 0; reg < 256;) {
      if (!((map.float_bitmap[reg / 64] >> (reg % 64)) & 1)) {
        ++reg;
        continue;
      }
      const uint32_t first = reg;
      while (reg < 256 && ((map.float_bitmap[reg / 64] >> (reg % 64)) & 1)) {
        ++reg;
      }
      e.constants_runs.emplace_back(uint16_t(first), uint16_t(reg - first));
      e.constants_runs_bytes += (reg - first) * 16;
    }
    // Many short runs cost more in calls than they save: copy the whole block instead.
    e.constants_runs_valid = e.constants_runs.size() <= 24;
  } catch (const std::exception&) {
    e.constants_runs.clear();
  }
#endif
}

}  // namespace

namespace bo2zm::native {
namespace {

constexpr uint32_t kMaxWarnings = 48;

constexpr const char* kUses[] = {"position", "weight",   "indices",    "normal",
                                 "point_size", "texcoord", "tangent",    "binormal",
                                 "tessfactor","position_t", "color",    "fog",
                                 "depth", "sample", "usage14",     "usage15"};

// The game's container reader: the index's (bo2zm_shader_index.cpp) with xenia's constant analysis.
const char* Read(const bo2zm::shaderlib::Shader& shader, ShaderInput& e) {
  return ReadContainer(shader, e, AnalyzeConstants);
}

struct RawKey {
  uint64_t hash;
  uint32_t words;
  bool vertices;
  bool operator==(const RawKey&) const = default;
};

struct RawKeyHash {
  size_t operator()(const RawKey& c) const {
    return size_t(c.hash ^ (uint64_t(c.words) << 1) ^ uint64_t(c.vertices));
  }
};

}  // namespace

struct NativeShaders::Data {
  bo2zm::shaderlib::ShaderLibrary library;
  std::vector<IndexEntry> index;  // library order
  uint64_t identity = 0;  // the pack's size, count and body hash (Identity)
  std::vector<uint16_t> fetches;
  IndexStats index_stats;
  // (vertices, words) -> candidate entries.
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidates;
  // Shaders made on first use (Make), per number; never freed while loaded, so any thread may keep the pointers.
  std::unique_ptr<std::atomic<const ShaderInput*>[]> made;
  std::mutex making;
  std::deque<bo2zm::shaderlib::Shader> made_shaders;  // under making
  std::deque<ShaderInput> made_inputs;                // under making
  std::vector<uint8_t> unmakeable;                    // under making: Make failed for that number
  uint32_t make_warnings = 0;                         // under making
  std::unordered_map<RawKey, const ShaderInput*, RawKeyHash> cache;
  std::vector<uint32_t> temporal;
  NativeShaderStats stats;
  uint32_t warnings = 0;
  bool loaded = false;

  // The shader with that number, made the first time (any thread): parsed from given (its container, as the game
  // created it: IdentifyContainer has matched it to the index) or from its container in the pack. nullptr if it is
  // not usable.
  const ShaderInput* Make(uint32_t number, std::span<const uint8_t> given = {});
  bool ReadIndex(const std::filesystem::path& file, const bo2zm::shaderlib::PackInfo& pack);
  // Reads the whole pack, makes its index (bo2zm_shader_index.h) and saves it as file.
  void MakeIndex(const std::filesystem::path& pack_file, const bo2zm::shaderlib::PackInfo& pack,
                 const std::filesystem::path& file, const IndexProgress& progress);
  void Take(ShaderIndex&& from);
};

NativeShaders::NativeShaders() : data_(std::make_unique<Data>()) {}
NativeShaders::~NativeShaders() = default;

bool NativeShaders::loaded() const {
  return data_->loaded;
}

uint64_t NativeShaders::Identity() const {
  return data_->loaded ? data_->identity : 0;
}

bool FetchCoherent(const ShaderInput& vs, std::span<const uint32_t> patched) {
  if (!vs.vertices || patched.size() != vs.microcode.size()) {
    return false;
  }
  for (const VertexElement& element : vs.elements) {
    const uint32_t reg_index = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
    bool found = false;
    for (const VertexElement& other : vs.elements) {
      const uint32_t d0 = patched[size_t(other.instruction) * 3];
      if (((d0 >> 12) & 0x3F) == reg_index && (d0 & 0x1F) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

const ShaderInput* NativeShaders::Data::Make(uint32_t number, std::span<const uint8_t> given) {
  if (number >= index.size()) {
    return nullptr;
  }
  if (const ShaderInput* e = made[number].load(std::memory_order_acquire)) {
    return e;
  }
  const IndexEntry& x = index[number];
  if (!(x.flags & kIndexReadable)) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(making);
  if (const ShaderInput* e = made[number].load(std::memory_order_relaxed)) {
    return e;
  }
  if (unmakeable[number]) {
    return nullptr;
  }
  bo2zm::shaderlib::Shader s;
  if (given.empty()) {
    s.original = library.Container(x.container_offset, x.container_size, x.container_hash);
  } else {
    s.original.assign(given.begin(), given.end());
  }
  ShaderInput e;
  const char* reason = s.original.empty() ? "its container could not be read from the pack" : nullptr;
  if (!reason) {
    s.hash = x.container_hash;
    s.vertices = (x.flags & kIndexVertices) != 0;
    s.container_offset = x.container_offset;
    s.spirv_offset = x.container_offset + x.container_size;
    s.spirv_words = x.spirv_words;
    s.spirv_hash = x.spirv_hash;
    s.library = &library;
    made_shaders.push_back(std::move(s));
    e.shader = &made_shaders.back();
    e.number = number;
    reason = Read(made_shaders.back(), e);
    if (!reason && (e.microcode.size() != x.microcode_words ||
                    (e.fetch_full ? e.hash_no_fetch : e.hash) != x.microcode_hash)) {
      reason = "its microcode does not match the index";
    }
  }
  if (reason) {
    unmakeable[number] = 1;
    if (make_warnings++ < 16) {
      REXLOG_WARN("[native] C5a: library shader n{} not usable: {}", number, reason);
    }
    return nullptr;
  }
  if (!e.vertices) {
    e.kills = x.kills;
    e.discards = e.kills != 1;  // 1 = only the alpha test one
    e.shadow_min = (x.flags & kIndexShadowMin) != 0;
  }
  made_inputs.push_back(std::move(e));
  made[number].store(&made_inputs.back(), std::memory_order_release);
  return &made_inputs.back();
}

bool NativeShaders::Data::ReadIndex(const std::filesystem::path& file, const bo2zm::shaderlib::PackInfo& pack) {
  std::ifstream f(file, std::ios::binary | std::ios::ate);
  if (!f) {
    REXLOG_INFO("[native] C5a: no library index yet ({}): making it", file.string());
    return false;
  }
  const auto size = f.tellg();
  std::vector<uint8_t> bytes(size > 0 ? size_t(size) : 0);
  f.seekg(0);
  ShaderIndex read;
  const char* stale = !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))
                          ? "too short"
                          : DecodeShaderIndex(bytes, pack, read);
  if (stale) {
    REXLOG_INFO("[native] C5a: library index {} ({}): making it again", file.string(), stale);
    return false;
  }
  Take(std::move(read));
  return true;
}

void NativeShaders::Data::MakeIndex(const std::filesystem::path& pack_file, const bo2zm::shaderlib::PackInfo& pack,
                                    const std::filesystem::path& file, const IndexProgress& progress) {
  ShaderIndex made;
  IndexEvents events;
  events.ignored = [](uint32_t number, const char* reason) {
    REXLOG_WARN("[native] C5a: library container {} ignored: {}", number, reason);
  };
  events.progress = progress;
  BuildShaderIndex(library, pack_file, pack, made, events);
  const std::vector<uint8_t> bytes = EncodeShaderIndex(made, pack);
  Take(std::move(made));
  std::string error;
  if (!SaveShaderIndex(file, bytes, error)) {
    REXLOG_WARN("[native] C5a: could not save the library index ({}): the next start makes it again", error);
  }
}

void NativeShaders::Data::Take(ShaderIndex&& from) {
  index = std::move(from.entries);
  fetches = std::move(from.fetches);
  index_stats = from.stats;
}

// bo2zm_native_shader_index_check: the whole pack read in full, without the index (every container kept and parsed), and
// each shader compared with the one made from the index (Make), field by field; IdentifyContainer must find each
// container's own entry. Every shader gets made: a test on the PC.
void NativeShaders::CheckIndex(const std::filesystem::path& file) const {
  Data& d = *data_;
  const auto start = std::chrono::steady_clock::now();
  bo2zm::shaderlib::ShaderLibrary full;
  std::vector<std::pair<uint32_t, bool>> scans;
  try {
    full.Load(file, [&](uint32_t number, const bo2zm::shaderlib::Shader& s) {
      scans.resize(std::max<size_t>(scans.size(), size_t(number) + 1));
      scans[number] = {CountKills(s.spirv), HasMarkShadowMin(s.spirv)};
    });
  } catch (const std::exception& e) {
    REXLOG_WARN("[native] C5a: index check: the pack could not be read: {}", e.what());
    return;
  }
  uint32_t checked = 0, different = 0;
  const auto& shaders = full.shaders();
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    ShaderInput a;
    const char* reason = Read(shaders[i], a);
    const ShaderInput* b = PerNumber(i);
    const char* what = nullptr;
    if (reason || !b) {
      what = (reason != nullptr) == (b == nullptr) ? nullptr : "usable in only one of them";
    } else if (a.vertices != b->vertices || a.microcode != b->microcode || a.hash != b->hash ||
               a.fetch_full != b->fetch_full || a.microcode_no_fetch != b->microcode_no_fetch ||
               a.hash_no_fetch != b->hash_no_fetch || a.outputs != b->outputs ||
               a.constants_bytes != b->constants_bytes || a.constants_runs != b->constants_runs ||
               a.constants_runs_valid != b->constants_runs_valid || b->shader->original != shaders[i].original ||
               bo2zm::shaderlib::Spirv(*b->shader) != bo2zm::shaderlib::Spirv(shaders[i])) {
      what = "a field differs";
    } else if (a.elements.size() != b->elements.size() || a.samplers.size() != b->samplers.size()) {
      what = "elements or samplers differ";
    } else if (!a.vertices && (b->kills != scans[i].first || b->shadow_min != scans[i].second ||
                               b->discards != (scans[i].first != 1))) {
      what = "kills or the shadow minimum differ";
    } else if (IdentifyContainer(shaders[i].original) != b) {
      what = "IdentifyContainer finds another entry";
    }
    for (size_t k = 0; !what && b && k < a.elements.size(); ++k) {
      const VertexElement &x = a.elements[k], &y = b->elements[k];
      if (x.instruction != y.instruction || x.use != y.use || x.index_use != y.index_use) {
        what = "an element differs";
      }
    }
    for (size_t k = 0; !what && b && k < a.samplers.size(); ++k) {
      const SamplerShader &x = a.samplers[k], &y = b->samplers[k];
      if (x.reg_index != y.reg_index || x.type != y.type || x.map_shadows != y.map_shadows) {
        what = "a sampler differs";
      }
    }
    ++checked;
    if (what && ++different <= 12) {
      REXLOG_WARN("[native] C5a: index check: shader n{}: {}", i, what);
    }
  }
  REXLOG_INFO("[native] C5a: index check: {} of {} shaders compared with the whole pack's, {} different ({:.1f} s)",
              checked, d.index.size(), different,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

bool NativeShaders::Load(const std::filesystem::path& file, const IndexProgress& progress) {
  Data& d = *data_;
  const auto start = std::chrono::steady_clock::now();
  const std::filesystem::path index_file = ShaderIndexPath(file);
  bool made_now = false;
  try {
    const bo2zm::shaderlib::PackInfo pack = d.library.Open(file);
    const uint64_t identity[3] = {pack.size, pack.amount, pack.body_hash};
    d.identity = XXH3_64bits(identity, sizeof(identity));
    if (!d.ReadIndex(index_file, pack)) {
      d.MakeIndex(file, pack, index_file, progress);
      made_now = true;
    }
  } catch (const std::exception& e) {
    REXLOG_WARN("[native] C5a: shader library not available ({}): {}", file.string(), e.what());
    d.index.clear();
    return false;
  }
  const size_t count = d.index.size();
  d.made = std::make_unique<std::atomic<const ShaderInput*>[]>(count);
  for (size_t i = 0; i < count; ++i) {
    d.made[i].store(nullptr, std::memory_order_relaxed);
  }
  d.unmakeable.assign(count, 0);
  d.candidates.clear();
  uint64_t spirv_words = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const IndexEntry& x = d.index[i];
    spirv_words += x.spirv_words;
    if (x.flags & kIndexReadable) {
      d.candidates[{(x.flags & kIndexVertices) != 0, x.microcode_words}].push_back(i);
    }
  }
  d.cache.clear();
  const IndexStats& s = d.index_stats;
  d.loaded = s.vertex + s.pixel > 0;
  if (d.loaded && REXCVAR_GET(bo2zm_native_shader_index_check)) {
    CheckIndex(file);
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("[native] C5a: library index {} {} in {:.0f} ms ({} shaders; each one is read from the pack the first "
              "time it is used)",
              index_file.string(), made_now ? "made from the whole pack and saved" : "read", ms, count);
  REXLOG_INFO("[native] C5a: library with {} shaders ({} vertex, {} pixel); {} groups with the same microcode, {} of "
              "them with different SPIR-V; {:.1f} MB of SPIR-V left in the pack (read when used)",
              s.vertex + s.pixel, s.vertex, s.pixel, s.repeated, s.repeated_different,
              double(spirv_words * 4) / 1048576.0);
  REXLOG_INFO("[native] C5a: pixel shaders that can discard pixels {} of {} (the rest only carry the alpha test's kill: "
              "with no color to write, their stage can be removed)",
              s.with_kill, s.with_kill + s.no_kill);
  REXLOG_INFO("[native] C5a: shadow by minimum: {} pixel shaders sample the shadow map (SHADOWMAP_SAMPLER) "
              "and {} carry tfetch2DShadowMin{}",
              s.with_map_shadows, s.with_min,
              s.with_min && s.with_min == s.with_map_shadows
                  ? " (library with the minimum)"
                  : " (without the minimum: the shadow map is still copied as always)");
  return d.loaded;
}

const ShaderInput* NativeShaders::Identify(bool vertices,
                                                 std::span<const uint32_t> microcode) {
  Data& d = *data_;
  ++d.stats.loads;
  const RawKey key{XXH3_64bits(microcode.data(), microcode.size_bytes()),
                         uint32_t(microcode.size()), vertices};
  if (auto it = d.cache.find(key); it != d.cache.end()) {
    return it->second;
  }
  ++d.stats.different;

  const ShaderInput* chosen = nullptr;
  uint32_t matches = 0;
  if (auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      c != d.candidates.end()) {
    for (uint32_t number : c->second) {
      // The index's hash first; a candidate is made (read from the pack) only when it matches.
      const IndexEntry& x = d.index[number];
      uint64_t hash = key.hash;
      if (x.fetches_count) {
        d.temporal.assign(microcode.begin(), microcode.end());
        // BO2 vertex shaders drop their whole fetch instructions (fetch_full).
        const uint32_t* mask = (x.flags & kIndexFetchFull) ? kFetchNothing : kFetchKeeps;
        for (uint32_t k = 0; k < x.fetches_count; ++k) {
          const size_t instruction = d.fetches[size_t(x.fetches_start) + k];
          for (size_t j = 0; j < 3; ++j) {
            d.temporal[instruction * 3 + j] &= mask[j];
          }
        }
        hash = XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t));
      }
      if (hash != x.microcode_hash) {
        continue;
      }
      if (!x.fetches_count) {
        d.temporal.assign(microcode.begin(), microcode.end());
      }
      const ShaderInput* e = d.Make(number);
      if (!e || d.temporal != (e->fetch_full ? e->microcode_no_fetch : e->microcode)) {
        continue;
      }
      if (!chosen) {
        chosen = e;
      }
      ++matches;
    }
  }

  // Vertex shaders arrive patched: a mismatch is normal.
  // Vertex shader misses are logged with bo2zm_native_log_vs_misses, to see what BO2's D3D patches; otherwise
  // they stay quiet.
  const bool warn = d.warnings < kMaxWarnings && (!vertices || bo2zm::native::LogVertexShaderMisses());
  if (chosen) {
    ++d.stats.identified;
    if (matches > 1) {
      ++d.stats.ambiguous;
    }
    if (warn && vertices) {
      // What D3D has patched in each fetch: it becomes the vertex input.
      ++d.warnings;
      std::string detail;
      for (const VertexElement& element : chosen->elements) {
        const size_t p = size_t(element.instruction) * 3;
        const uint32_t d0 = microcode[p], d1 = microcode[p + 1], d2 = microcode[p + 2];
        const uint32_t fetch = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
        const int32_t offset = int32_t(d2 << 1) >> 9;
        detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}{}", kUses[element.use & 0xF],
                               element.index_use, fetch, (d1 >> 16) & 0x3F, d2 & 0xFF, offset,
                               ((d1 >> 30) & 0x1) ? "/mini" : "");
      }
      REXLOG_INFO("[native] C5a: VS n{} ({} words, {} matches):{}", chosen->number,
                  microcode.size(), matches, detail);
    }
  } else {
    ++d.stats.no_identify;
    if (warn) {
      ++d.warnings;
      const auto c = d.candidates.find({vertices, uint32_t(microcode.size())});
      REXLOG_WARN("[native] C5a: unidentified {} shader: {} words, hash {:016X} ({} containers of that type and "
                  "length)",
                  vertices ? "vertex" : "pixel", microcode.size(), key.hash,
                  c != d.candidates.end() ? c->second.size() : 0);
      // Diagnostics: which words change against the containers of the same
      // length (incoming / container original, without the mask).
      for (size_t k = 0; c != d.candidates.end() && k < c->second.size() && k < 2; ++k) {
        const ShaderInput* made = d.Make(c->second[k]);
        if (!made) {
          continue;
        }
        const ShaderInput& e = *made;
        const auto& o = e.shader->original;
        const size_t virtual_size = size_t(o[4]) << 24 | size_t(o[5]) << 16 | size_t(o[6]) << 8 | o[7];
        std::string mismatches;
        uint32_t how_many = 0;
        for (size_t i = 0; i < microcode.size(); ++i) {
          uint32_t incoming = microcode[i];
          for (const VertexElement& element : e.elements) {
            const size_t p = size_t(element.instruction) * 3;
            if (i >= p && i < p + 3) {
              incoming &= kFetchKeeps[i - p];
            }
          }
          if (incoming == e.microcode[i]) {
            continue;
          }
          if (++how_many <= 24) {
            const size_t b = virtual_size + i * 4;
            const uint32_t original = uint32_t(o[b]) << 24 | uint32_t(o[b + 1]) << 16 |
                                      uint32_t(o[b + 2]) << 8 | o[b + 3];
            mismatches += fmt::format(" {}:{:08X}/{:08X}", i, microcode[i], original);
          }
        }
        std::string fetch;
        for (const VertexElement& element : e.elements) {
          fetch += fmt::format(" {}", element.instruction);
        }
        REXLOG_WARN("[native] C5a:   versus n{}: {} different words; fetch in the instructions{}:{}",
                    e.number, how_many, fetch, mismatches);
      }
    }
  }
  d.cache.emplace(key, chosen);
  return chosen;
}

const ShaderInput* NativeShaders::IdentifyContainer(
    std::span<const uint8_t> container) const {
  Data& d = *data_;
  if (!d.loaded || container.size() < 24 || container.size() > 65536) {
    return nullptr;
  }
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
  auto it = std::lower_bound(d.index.begin(), d.index.end(), hash,
                             [](const IndexEntry& x, uint64_t h) { return x.container_hash < h; });
  std::optional<XXH128_hash_t> wide;
  for (; it != d.index.end() && it->container_hash == hash; ++it) {
    if (it->container_size != container.size()) {
      continue;
    }
    const uint32_t number = uint32_t(it - d.index.begin());
    // A hash collision never picks another shader: a made one's container is compared byte by byte; for one not
    // made yet, the 128-bit hash of the container (with the 64-bit one and the size) decides, and it is made from
    // these very bytes.
    if (const ShaderInput* e = d.made[number].load(std::memory_order_acquire)) {
      if (std::equal(container.begin(), container.end(), e->shader->original.begin(), e->shader->original.end())) {
        return e;
      }
      continue;
    }
    if (!wide) {
      wide = XXH3_128bits(container.data(), container.size());
    }
    if (wide->low64 == it->container_hash_low && wide->high64 == it->container_hash_high) {
      return d.Make(number, container);
    }
  }
  return nullptr;
}

int32_t NativeShaders::NumberOfContainer(std::span<const uint8_t> container) const {
  Data& d = *data_;
  if (!d.loaded || container.size() < 24 || container.size() > 65536) {
    return -1;
  }
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
  auto it = std::lower_bound(d.index.begin(), d.index.end(), hash,
                             [](const IndexEntry& x, uint64_t h) { return x.container_hash < h; });
  std::optional<XXH128_hash_t> wide;
  for (; it != d.index.end() && it->container_hash == hash; ++it) {
    if (it->container_size != container.size()) {
      continue;
    }
    const uint32_t number = uint32_t(it - d.index.begin());
    // The same tests as IdentifyContainer, so a number from here and an entry from there always agree.
    if (const ShaderInput* e = d.made[number].load(std::memory_order_acquire)) {
      if (std::equal(container.begin(), container.end(), e->shader->original.begin(), e->shader->original.end())) {
        return int32_t(number);
      }
      continue;
    }
    if (!wide) {
      wide = XXH3_128bits(container.data(), container.size());
    }
    if (wide->low64 == it->container_hash_low && wide->high64 == it->container_hash_high) {
      return int32_t(number);
    }
  }
  return -1;
}

// Made the first time it is asked for (Data::Make).
const ShaderInput* NativeShaders::PerNumber(uint32_t number) const {
  Data& d = *data_;
  return d.loaded ? d.Make(number) : nullptr;
}

const char* NameUse(uint8_t use) {
  return kUses[use & 0xF];
}

NativeShaderStats NativeShaders::Stats() const {
  return data_->stats;
}

}  // namespace bo2zm::native
