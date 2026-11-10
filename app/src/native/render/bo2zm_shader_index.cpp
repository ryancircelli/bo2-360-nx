// The shader library's index and the container reader (see bo2zm_shader_index.h).
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Containers are big-endian; the index file is little-endian.

#include "bo2zm_shader_index.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <system_error>
#include <tuple>
#include <utility>

// See bo2zm_native_shaders.cpp: xxHash must read with memcpy, so it is included here with its implementation first.
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bo2zm::native {
namespace {

struct Container {
  const std::vector<uint8_t>& o;

  bool Has(size_t position, size_t bytes) const {
    return position <= o.size() && bytes <= o.size() - position;
  }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// bo2zm_native_shadow_min. The zero-terminated string starting at `position` is `label`.
bool NameIs(const Container& c, size_t position, const char* label) {
  const size_t n = std::strlen(label);
  return c.Has(position, n + 1) && std::memcmp(c.o.data() + position, label, n) == 0 && c.o[position + n] == 0;
}

// bo2zm_native_shadow_min. The library has tfetch2DShadowMin if the pixel shader's SPIR-V contains the
// BO2ZM_SHADOW_MIN_MARKER constant from shader_common.h (a 32-bit OpConstant): nothing else uses it.
constexpr uint32_t kMarkShadowMin = 0x5E3B1A84u;

// The index file, little-endian: an 80-byte header (signature, version, shaders, the pack's size and header hash,
// fetch instructions, the nine IndexStats counts, XXH3 of the rest), then 72 bytes per shader, then the fetch
// instructions (16 bits each). kIndexVersion changes whenever ReadContainer or what the index keeps changes: an
// index of another version is made again.
constexpr std::array<uint8_t, 8> kIndexSignature{'B', 'Z', 'M', 'S', 'I', 'D', 'X', 0};
constexpr uint32_t kIndexVersion = 2;  // 2: elements of undeclared vertex fetches (SynthesizeElements)
constexpr size_t kIndexHeader = 80;
constexpr size_t kIndexEntry = 72;

void Put32(std::vector<uint8_t>& d, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (i * 8)));
}
void Put64(std::vector<uint8_t>& d, uint64_t v) {
  Put32(d, uint32_t(v));
  Put32(d, uint32_t(v >> 32));
}
uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Get64(const uint8_t* p) {
  return Get32(p) | uint64_t(Get32(p + 4)) << 32;
}

uint32_t* StatsField(IndexStats& s, size_t i) {
  uint32_t* const fields[] = {&s.vertex,  &s.pixel,   &s.ignored,          &s.repeated, &s.repeated_different,
                              &s.with_kill, &s.no_kill, &s.with_map_shadows, &s.with_min};
  return fields[i];
}
constexpr size_t kStatsFields = 9;

// Uses given, in this order, to vertex fetches that have no element in the container: the XDK's own vertex shaders
// (ShadeVertexPassThru, ShadeVertexPassThruPosOnly) declare none. Keep in step with UNDECLARED_FETCH_USES in
// tools/xenosrecomp/translator/shader_recompiler.cpp, which names the shader inputs the same way.
constexpr std::array<std::pair<uint8_t, uint8_t>, 6> kUndeclaredFetchUses{
    {{0, 0}, {3, 0}, {6, 0}, {7, 0}, {10, 0}, {10, 1}}};  // position, normal, tangent, binormal, color 0 and 1

// Adds an element for every vertex fetch without one, in the order the translator meets them: control flow order,
// each EXEC's instructions in turn. Returns false if a fetch is left without a use.
bool SynthesizeElements(ShaderInput& e) {
  const std::vector<uint32_t>& m = e.microcode;
  const size_t slots = m.size() / 3;
  size_t end_cf = slots;  // the control flow ends where the first EXEC's instructions start
  for (size_t s = 0; s < end_cf && s < slots; ++s) {
    const uint32_t w0 = m[s * 3], w1 = m[s * 3 + 1], w2 = m[s * 3 + 2];
    const uint64_t pair[2] = {uint64_t(w0) | uint64_t(w1 & 0xFFFF) << 32,
                              uint64_t((w1 >> 16) | (w2 << 16)) | uint64_t(w2 >> 16) << 32};
    for (const uint64_t cf : pair) {
      const uint32_t opcode = uint32_t(cf >> 44) & 0xF;
      const bool exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
      if (!exec) continue;
      const uint32_t address = uint32_t(cf) & 0xFFF;
      const uint32_t count = uint32_t(cf >> 12) & 0x7;
      uint32_t sequence = uint32_t(cf >> 16) & 0xFFF;
      end_cf = std::min<size_t>(end_cf, address);
      for (uint32_t i = 0; i < count; ++i, sequence >>= 2) {
        const size_t at = size_t(address) + i;
        if (!(sequence & 0x1) || at >= slots || (m[at * 3] & 0x1F) != 0) continue;  // not a vertex fetch
        if (std::any_of(e.elements.begin(), e.elements.end(),
                        [&](const VertexElement& x) { return x.instruction == at; })) {
          continue;
        }
        bool given = false;
        for (const auto& [use, index] : kUndeclaredFetchUses) {
          if (std::any_of(e.elements.begin(), e.elements.end(),
                          [&](const VertexElement& x) { return x.use == use && x.index_use == index; })) {
            continue;
          }
          VertexElement element;
          element.instruction = uint16_t(at);
          element.use = use;
          element.index_use = index;
          e.elements.push_back(element);
          given = true;
          break;
        }
        if (!given) return false;
      }
    }
  }
  return true;
}

}  // namespace

// Reads what is needed from the container. BO2 uses the 2008 layout (XenosRecomp shader.h ShaderContainer): CTAB at
// +0x10, shader header at +0x18, and the microcode is shader->size bytes at shader->physicalOffset inside the
// physical part (BO2 keeps literal constants before it: physicalOffset 64-128 in most containers). Its vertex element
// list starts at header + 0x24 (VertexShader::vertexElementsAndInterpolators). The older 2005 layout is read as well:
// 24-byte header with signature, virtual part, physical part, definitions (+12), CTAB (+16) and shader header (+20),
// where the microcode is the whole physical part. Reference: docs/shaders.md.
const char* ReadContainer(const bo2zm::shaderlib::Shader& shader, ShaderInput& e, AnalyzeMicrocode analyze) {
  const Container c{shader.original};
  if (!c.Has(0, 24)) return "container too short";
  const uint32_t virtual_size = c.U32(4);
  const uint32_t physical = c.U32(8);
  const bool format_2008 = (c.U32(0) & 0xFFFFFF00u) == 0x102A1100u;
  if (format_2008 && !c.Has(0, 36)) return "container too short";
  const uint32_t table = c.U32(format_2008 ? 0x10 : 16);
  const uint32_t header = c.U32(format_2008 ? 0x18 : 20);
  if (!physical || (physical % 4) || !c.Has(virtual_size, physical)) {
    return "microcode outside the container";
  }
  if (header >= virtual_size || size_t(header) + (shader.vertices ? 40 : 32) > virtual_size) {
    return "shader header outside the virtual part";
  }
  uint32_t start_micro = 0;
  uint32_t bytes_micro = physical;
  if (format_2008) {
    start_micro = c.U32(header);
    bytes_micro = c.U32(header + 4);
    if (!bytes_micro || (bytes_micro % 4) || (start_micro % 4) || uint64_t(start_micro) + bytes_micro > physical) {
      return "microcode outside the physical part";
    }
  }

  e.vertices = shader.vertices;
  e.microcode.resize(bytes_micro / 4);
  for (size_t i = 0; i < e.microcode.size(); ++i) {
    e.microcode[i] = c.U32(size_t(virtual_size) + start_micro + i * 4);
  }
  if (analyze) {
    analyze(e);  // before the fetches are masked
  }

  if (e.vertices) {
    // List at +0x28 (2005) or +0x24 (2008), after skipping the words at +0x18; +0x1C = element count.
    const uint32_t previous = c.U32(header + 24);
    const uint32_t amount = c.U32(header + 28);
    const size_t start = size_t(header) + (format_2008 ? 36 : 40) + size_t(previous) * 4;
    if (amount > 64 || previous > 1024 || start + size_t(amount) * 4 > virtual_size) {
      return "vertex elements outside the virtual part";
    }
    for (uint32_t i = 0; i < amount; ++i) {
      const uint32_t value = c.U32(start + size_t(i) * 4);
      VertexElement element;
      element.instruction = uint16_t(value & 0xFFF);
      element.use = uint8_t((value >> 12) & 0xF);
      element.index_use = uint8_t((value >> 16) & 0xF);
      if ((size_t(element.instruction) + 1) * 3 > e.microcode.size()) {
        return "vertex element points outside the microcode";
      }
      e.elements.push_back(element);
    }
    if (!SynthesizeElements(e)) {
      return "vertex fetch without an element, and no input left for it";
    }
    for (const VertexElement& element : e.elements) {
      for (size_t j = 0; j < 3; ++j) {
        e.microcode[size_t(element.instruction) * 3 + j] &= kFetchKeeps[j];
      }
    }
  } else {
    if (!c.Has(header + 24, 8)) return "short pixel shader header";
    e.outputs = c.U32(header + 28);
  }

  // Constant table: only the samplers (register and type).
  if (!table || !c.Has(table + 4, 28)) return "no constant table";
  const size_t base = size_t(table) + 4;
  const uint32_t constants = c.U32(base + 12);
  const uint32_t info = c.U32(base + 16);
  if (constants > 1024 || !c.Has(base + info, size_t(constants) * 20)) {
    return "constant table outside the container";
  }
  // Float registers the SPIR-V reads: the highest in the table, or up to the end of the
  // buffer if there is an array with relative indexing (XenosRecomp shader_recompiler.cpp:
  // 1172-1183: tailCount = 256 in VS and 224 in PS).
  uint32_t registers_float = 0;
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) == 2) {  // RegisterSet::Float4
      const uint32_t index = c.U16(p + 6);
      const uint32_t how_many = c.U16(p + 8);
      registers_float = std::max(registers_float, how_many > 1 ? (e.vertices ? 256u : 224u)
                                                              : index + 1);
    }
  }
  e.constants_bytes = std::min<uint32_t>(std::max<uint32_t>(registers_float, 1), 256) * 16;
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    SamplerShader sampler;
    sampler.reg_index = c.U16(p + 6);
    const uint32_t type = c.U32(p + 12);
    sampler.type = c.Has(base + type, 4) ? c.U16(base + type + 2) : 0;
    // bo2zm_native_shadow_min. The name is at that distance from the start of the table, like the type
    // (XenosRecomp shader_recompiler.cpp: constantTableData + constantInfo->name).
    sampler.map_shadows = NameIs(c, base + size_t(c.U32(p)), "SHADOWMAP_SAMPLER");
    e.samplers.push_back(sampler);
  }

  e.hash = XXH3_64bits(e.microcode.data(), e.microcode.size() * sizeof(uint32_t));
  if (format_2008 && e.vertices) {  // see ShaderInput::fetch_full
    e.fetch_full = true;
    e.microcode_no_fetch = e.microcode;
    for (const VertexElement& element : e.elements) {
      for (size_t j = 0; j < 3; ++j) {
        e.microcode_no_fetch[size_t(element.instruction) * 3 + j] = 0;
      }
    }
    e.hash_no_fetch =
        XXH3_64bits(e.microcode_no_fetch.data(), e.microcode_no_fetch.size() * sizeof(uint32_t));
  }
  return nullptr;
}

bool HasMarkShadowMin(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return false;
  }
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    if (words == 0 || i + words > spirv.size()) {
      return false;
    }
    if ((spirv[i] & 0xFFFF) == 43 && words == 4 && spirv[i + 3] == kMarkShadowMin) {
      return true;
    }
    i += words;
  }
  return false;
}

uint32_t CountKills(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return 2;
  }
  uint32_t kills = 0;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return 2;
    }
    if (code == 252 || code == 4416 || code == 5380) {
      ++kills;
    }
    i += words;
  }
  return kills;
}

void BuildShaderIndex(bo2zm::shaderlib::ShaderLibrary& library, const std::filesystem::path& pack_file,
                      const bo2zm::shaderlib::PackInfo& pack, ShaderIndex& index, const IndexEvents& events) {
  index = {};
  std::vector<IndexEntry>& entries = index.entries;
  std::vector<uint16_t>& fetches = index.fetches;
  IndexStats& stats = index.stats;
  // Shaders with the same microcode (the log line): how many, the first one's SPIR-V, and whether another differs.
  struct Group {
    uint32_t members = 0;
    uint64_t first_spirv = 0;
    bool different = false;
  };
  std::map<std::tuple<bool, uint32_t, uint64_t>, Group> groups;
  library.Scan(pack_file, [&](uint32_t number, const bo2zm::shaderlib::Shader& s) {
    IndexEntry x;
    x.container_hash = s.hash;
    const XXH128_hash_t wide = XXH3_128bits(s.original.data(), s.original.size());
    x.container_hash_low = wide.low64;
    x.container_hash_high = wide.high64;
    x.container_offset = s.container_offset;
    x.container_size = uint32_t(s.original.size());
    x.spirv_words = s.spirv_words;
    x.spirv_hash = s.spirv_hash;
    x.flags = s.vertices ? kIndexVertices : 0;
    if (events.progress) {
      events.progress(s.spirv_offset + uint64_t(s.spirv_words) * 4, pack.size);
    }
    ShaderInput e;
    if (const char* reason = ReadContainer(s, e)) {
      if (events.ignored) {
        events.ignored(number, reason);
      }
      ++stats.ignored;
      entries.push_back(x);
      return;
    }
    x.flags |= kIndexReadable | (e.fetch_full ? kIndexFetchFull : 0);
    x.microcode_words = uint32_t(e.microcode.size());
    x.microcode_hash = e.fetch_full ? e.hash_no_fetch : e.hash;
    if (e.vertices) {
      ++stats.vertex;
      x.fetches_start = uint32_t(fetches.size());
      x.fetches_count = uint16_t(e.elements.size());
      for (const VertexElement& element : e.elements) {
        fetches.push_back(element.instruction);
      }
    } else {
      ++stats.pixel;
      x.kills = uint8_t(std::min<uint32_t>(CountKills(s.spirv), 255));
      (x.kills != 1 ? stats.with_kill : stats.no_kill) += 1;
      const bool shadow_min = HasMarkShadowMin(s.spirv);
      x.flags |= shadow_min ? kIndexShadowMin : 0;
      for (const SamplerShader& sampler : e.samplers) {
        if (sampler.map_shadows) {
          ++stats.with_map_shadows;
          stats.with_min += shadow_min ? 1 : 0;
          break;
        }
      }
    }
    Group& group = groups[{e.vertices, x.microcode_words, e.hash}];
    if (group.members++ == 0) {
      group.first_spirv = s.spirv_hash;
    } else {
      group.different |= group.first_spirv != s.spirv_hash;
    }
    entries.push_back(x);
  });
  for (const auto& [key, group] : groups) {
    if (group.members >= 2) {
      ++stats.repeated;
      stats.repeated_different += group.different ? 1 : 0;
    }
  }
}

std::vector<uint8_t> EncodeShaderIndex(const ShaderIndex& index, const bo2zm::shaderlib::PackInfo& pack) {
  std::vector<uint8_t> body;
  body.reserve(index.entries.size() * kIndexEntry + index.fetches.size() * 2);
  for (const IndexEntry& x : index.entries) {
    Put64(body, x.container_hash);
    Put64(body, x.container_hash_low);
    Put64(body, x.container_hash_high);
    Put64(body, x.container_offset);
    Put32(body, x.container_size);
    Put32(body, x.spirv_words);
    Put64(body, x.spirv_hash);
    Put64(body, x.microcode_hash);
    Put32(body, x.microcode_words);
    Put32(body, x.fetches_start);
    Put32(body, x.fetches_count);
    body.push_back(x.flags);
    body.push_back(x.kills);
    body.push_back(0);
    body.push_back(0);
  }
  for (uint16_t fetch : index.fetches) {
    body.push_back(uint8_t(fetch));
    body.push_back(uint8_t(fetch >> 8));
  }
  std::vector<uint8_t> out(kIndexSignature.begin(), kIndexSignature.end());
  Put32(out, kIndexVersion);
  Put32(out, uint32_t(index.entries.size()));
  Put64(out, pack.size);
  Put64(out, pack.body_hash);
  Put32(out, uint32_t(index.fetches.size()));
  IndexStats stats = index.stats;
  for (size_t i = 0; i < kStatsFields; ++i) {
    Put32(out, *StatsField(stats, i));
  }
  Put64(out, XXH3_64bits(body.data(), body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

const char* DecodeShaderIndex(std::span<const uint8_t> bytes, const bo2zm::shaderlib::PackInfo& pack,
                              ShaderIndex& index) {
  index = {};
  if (bytes.size() < kIndexHeader) {
    return "too short";
  }
  if (!std::equal(kIndexSignature.begin(), kIndexSignature.end(), bytes.begin()) ||
      Get32(&bytes[8]) != kIndexVersion) {
    return "another version";
  }
  if (Get32(&bytes[12]) != pack.amount || Get64(&bytes[16]) != pack.size || Get64(&bytes[24]) != pack.body_hash) {
    return "made for another pack";
  }
  const uint32_t fetch_count = Get32(&bytes[32]);
  if (bytes.size() != kIndexHeader + size_t(pack.amount) * kIndexEntry + size_t(fetch_count) * 2) {
    return "wrong length";
  }
  if (XXH3_64bits(bytes.data() + kIndexHeader, bytes.size() - kIndexHeader) != Get64(&bytes[72])) {
    return "damaged";
  }
  for (size_t i = 0; i < kStatsFields; ++i) {
    *StatsField(index.stats, i) = Get32(&bytes[36 + i * 4]);
  }
  index.entries.resize(pack.amount);
  const uint8_t* p = bytes.data() + kIndexHeader;
  for (IndexEntry& x : index.entries) {
    x.container_hash = Get64(p);
    x.container_hash_low = Get64(p + 8);
    x.container_hash_high = Get64(p + 16);
    x.container_offset = Get64(p + 24);
    x.container_size = Get32(p + 32);
    x.spirv_words = Get32(p + 36);
    x.spirv_hash = Get64(p + 40);
    x.microcode_hash = Get64(p + 48);
    x.microcode_words = Get32(p + 56);
    x.fetches_start = Get32(p + 60);
    x.fetches_count = uint16_t(Get32(p + 64));
    x.flags = p[68];
    x.kills = p[69];
    if (uint64_t(x.fetches_start) + x.fetches_count > fetch_count ||
        x.container_offset + x.container_size + uint64_t(x.spirv_words) * 4 > pack.size) {
      index = {};
      return "an entry out of bounds";
    }
    p += kIndexEntry;
  }
  index.fetches.resize(fetch_count);
  for (uint16_t& fetch : index.fetches) {
    fetch = uint16_t(p[0] | p[1] << 8);
    p += 2;
  }
  return nullptr;
}

bool SaveShaderIndex(const std::filesystem::path& file, std::span<const uint8_t> bytes, std::string& error) {
  std::filesystem::path partial = file;
  partial += ".partial";
  {
    std::ofstream f(partial, std::ios::binary | std::ios::trunc);
    if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())) || !f.flush()) {
      error = "could not write " + partial.string();
      return false;
    }
  }
  std::error_code code;
  std::filesystem::remove(file, code);
  std::filesystem::rename(partial, file, code);
  if (code) {
    error = "could not rename " + partial.string() + " to " + file.string() + ": " + code.message();
    return false;
  }
  return true;
}

std::filesystem::path ShaderIndexPath(const std::filesystem::path& pack_file) {
  std::filesystem::path index_file = pack_file;
  index_file.replace_extension(".spvidx");
  return index_file;
}

}  // namespace bo2zm::native
