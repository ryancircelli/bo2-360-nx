// The SPIR-V shader library (see bo2zm_shader_library.h): reading and checking pack files.
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.

#include "bo2zm_shader_library.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads
 * through 64- and 32-bit pointers without may_alias, and GCC may hoist that read above the write of the
 * data being hashed (strict aliasing). That made the texture key read keys[4] before writing it, and
 * the same texture was created several times (the disassembly of a built ELF shows it). Same
 * fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bo2zm::shaderlib {
namespace {
constexpr std::array<uint8_t, 8> kSignature{'B','Z','M','S','P','V',0,0};
// The disc and update zones hold 4,404 shaders (~227 MB of unoptimized SPIR-V); Zombies and Multiplayer with all
// their DLC, 32,842 (about 3.4 GB). The SPIR-V stays in the file until a shader is used, so the pack may be large;
// what each shader keeps in memory is its container. The file stays under 4 GB, the largest a FAT32 SD card holds.
constexpr size_t kMaxFile = size_t(4) * 1024 * 1024 * 1024 - 1;
constexpr size_t kMaxShaders = 65536;
constexpr size_t kMaxOriginal = 64 * 1024;
constexpr size_t kMaxSpirv = 4 * 1024 * 1024;

void Require(bool condition, const char* error) {
  if (!condition) throw std::runtime_error(error);
}

uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}

struct RingReader {
  std::span<const uint8_t> data;
  size_t position = 0;
  std::span<const uint8_t> Take(size_t n) {
    Require(n <= data.size() - position, "Truncated shader pack");
    auto r = data.subspan(position, n);
    position += n;
    return r;
  }
  uint32_t U32() {
    auto p = Take(4);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }
  uint64_t U64() {
    const uint64_t low = U32();
    return low | uint64_t(U32()) << 32;
  }
};

void U32(std::vector<uint8_t>& d, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (i * 8)));
}
void U64(std::vector<uint8_t>& d, uint64_t v) {
  U32(d, uint32_t(v)); U32(d, uint32_t(v >> 32));
}

void Validate(Shader& s) {
  Require(s.original.size() >= 24 && s.original.size() <= kMaxOriginal,
         "Container length out of bounds");
  const uint32_t signature = BE(s.original.data());
  // BO2 containers carry the 2008 signature (0x102A1100); the older 2005 one is accepted too.
  Require((signature & ~1u) == 0x102A0E00 || (signature & ~1u) == 0x102A1100, "Unknown container signature");
  const uint32_t virtual_size = BE(s.original.data() + 4);
  const uint32_t physical = BE(s.original.data() + 8);
  // A 2008 (BO2) physical part also holds literal constants before the microcode, so it is
  // only word-aligned; the 2005 one is whole 12-byte instructions.
  const bool format_2008 = (signature & ~1u) == 0x102A1100;
  Require(virtual_size >= 24 && physical && !(physical % (format_2008 ? 4 : 12)) &&
             uint64_t(virtual_size) + physical == s.original.size(),
         "Inconsistent container lengths");
  s.vertices = (signature & 1) != 0;
  s.hash = XXH3_64bits(s.original.data(), s.original.size());
  Require(s.spirv.size() >= 5 && s.spirv.size() <= kMaxSpirv / 4,
         "SPIR-V length out of bounds");
  Require(s.spirv[0] == 0x07230203 && s.spirv[4] == 0,
         "Bad SPIR-V header");
  // This checks the wrapper; it does not replace spirv-val in the packager.
  bool input = false;
  for (size_t i = 5; i < s.spirv.size();) {
    const uint32_t words = s.spirv[i] >> 16, op = s.spirv[i] & 65535;
    Require(words && words <= s.spirv.size() - i, "Truncated SPIR-V instruction");
    if (op == 15) {  // OpEntryPoint: execution model, id, name and interface.
      Require(!input && words >= 5 && s.spirv[i+1] == (s.vertices ? 0u : 4u) &&
                 s.spirv[i+3] == 0x6e69616d && s.spirv[i+4] == 0,
             "The SPIR-V stage or main entry point does not match the container");
      input = true;
    }
    i += words;
  }
  Require(input, "SPIR-V without a main entry point");
}

bool Smaller(const Shader& a, const Shader& b) {
  if (a.hash != b.hash) return a.hash < b.hash;
  return a.original < b.original;
}
}  // namespace

void ShaderLibrary::Load(std::span<const uint8_t> file) {
  Require(file.size() >= 24 && file.size() <= kMaxFile,
         "Shader pack size out of bounds");
  RingReader l{file};
  auto signature = l.Take(8);
  Require(std::equal(signature.begin(), signature.end(), kSignature.begin()), "Unknown pack signature");
  Require(l.U32() == 1, "Unsupported pack version");
  const uint32_t amount = l.U32();
  Require(amount && amount <= kMaxShaders, "Shader count out of bounds");
  const uint64_t hash = l.U64();
  Require(XXH3_64bits(file.data() + 24, file.size() - 24) == hash,
         "Tampered shader pack");
  std::vector<Shader> new_ones;
  new_ones.reserve(amount);
  for (uint32_t i = 0; i < amount; ++i) {
    const uint32_t original = l.U32(), words = l.U32();
    const uint64_t expected = l.U64();
    Require(original >= 24 && original <= kMaxOriginal &&
               words >= 5 && words <= kMaxSpirv / 4, "Entry too large");
    Shader s;
    auto data = l.Take(original);
    s.original.assign(data.begin(), data.end());
    // Reading the whole range first avoids allocations with a truncated file.
    RingReader code{l.Take(size_t(words) * 4)};
    s.spirv.reserve(words);
    for (uint32_t j = 0; j < words; ++j) s.spirv.push_back(code.U32());
    Validate(s);
    Require(s.hash == expected, "Bad container hash");
    if (!new_ones.empty()) Require(Smaller(new_ones.back(), s), "Repeated or unsorted entries");
    new_ones.push_back(std::move(s));
  }
  Require(l.position == file.size(), "Trailing data in the shader pack");
  shaders_ = std::move(new_ones);
}

namespace {
uint32_t LE32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Opens a pack file and checks its 24-byte header (signature, version, count); the stream is left after
// it. The body hash is only read here: Read checks it against the whole body.
PackInfo OpenPack(std::ifstream& f, const std::filesystem::path& file) {
  f.open(file, std::ios::binary | std::ios::ate);
  Require(bool(f), "Could not open the shader pack");
  const auto n = f.tellg();
  Require(n >= 24 && n <= std::streamoff(kMaxFile), "Pack size out of bounds");
  f.seekg(0);
  uint8_t header[24];
  Require(bool(f.read(reinterpret_cast<char*>(header), sizeof(header))), "Truncated shader pack");
  Require(std::equal(header, header + 8, kSignature.begin()), "Unknown pack signature");
  Require(LE32(header + 8) == 1, "Unsupported pack version");
  PackInfo info;
  info.size = uint64_t(n);
  info.amount = LE32(header + 12);
  Require(info.amount && info.amount <= kMaxShaders, "Shader count out of bounds");
  info.body_hash = LE32(header + 16) | uint64_t(LE32(header + 20)) << 32;
  return info;
}
}  // namespace

void ShaderLibrary::Load(const std::filesystem::path& file, const Inspect& inspect) {
  Read(file, inspect, true);
}

void ShaderLibrary::Scan(const std::filesystem::path& file, const Inspect& scan) {
  Read(file, scan, false);
}

PackInfo ShaderLibrary::Open(const std::filesystem::path& file) {
  std::ifstream f;
  const PackInfo info = OpenPack(f, file);
  shaders_.clear();
  UseFile(file);
  return info;
}

void ShaderLibrary::UseFile(const std::filesystem::path& file) {
  std::lock_guard<std::mutex> lock(spirv_mutex_);
  spirv_file_.close();
  file_ = file;
}

bool ShaderLibrary::ReadAt(uint64_t offset, void* to, size_t bytes) const {
  std::lock_guard<std::mutex> lock(spirv_mutex_);
  std::ifstream& f = spirv_file_;
  if (!f.is_open()) {
    f.open(file_, std::ios::binary);
  }
  f.clear();
  return f.is_open() && f.seekg(std::streamoff(offset)) &&
         f.read(reinterpret_cast<char*>(to), std::streamsize(bytes));
}

std::vector<uint8_t> ShaderLibrary::Container(uint64_t offset, uint32_t size, uint64_t hash) const {
  std::vector<uint8_t> bytes(size);
  if (size < 24 || size > kMaxOriginal || !ReadAt(offset, bytes.data(), size) ||
      XXH3_64bits(bytes.data(), size) != hash) {
    bytes.clear();
  }
  return bytes;
}

// Read entry by entry with the same checks as Load(span) (the hash with XXH3's streaming state), instead
// of the whole file at once (285 MB more of heap while loading). Each shader's SPIR-V is dropped as soon as it is
// checked and inspected, and its position is kept for Spirv. The pack is little-endian, as the Switch and the PC are.
// keep = false (Scan) keeps no shader at all: whoever scans keeps what it needs.
void ShaderLibrary::Read(const std::filesystem::path& file, const Inspect& inspect, bool keep) {
  static_assert(std::endian::native == std::endian::little, "the pack's words are read as they are");
  std::ifstream f;
  const PackInfo info = OpenPack(f, file);
  const auto read = [&](void* to, size_t bytes) {
    Require(bool(f.read(reinterpret_cast<char*>(to), std::streamsize(bytes))), "Truncated shader pack");
  };
  const auto le32 = LE32;
  const uint32_t amount = info.amount;
  const uint64_t hash = info.body_hash;
  const uint64_t n = info.size;
  constexpr size_t kHeader = 24;
  std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> state(XXH3_createState(), &XXH3_freeState);
  Require(state != nullptr, "Could not hash the shader pack");
  XXH3_64bits_reset(state.get());
  uint64_t position = kHeader;
  std::vector<Shader> new_ones;
  new_ones.reserve(keep ? amount : 0);
  uint64_t previous_hash = 0;
  std::vector<uint8_t> previous_original;  // without keep, the last container, for the order check
  // Every shader's SPIR-V goes through this one buffer. A vector per shader, freed after it, leaves ~60 KB holes
  // between the small blocks that stay (the containers): with the 16,149-shader pack that fragments ~1 GB of heap,
  // and the match's texture uploads later fail with std::bad_alloc on a Switch Lite.
  std::vector<uint32_t> scratch;
  for (uint32_t i = 0; i < amount; ++i) {
    uint8_t entry[16];
    read(entry, sizeof(entry));
    XXH3_64bits_update(state.get(), entry, sizeof(entry));
    const uint32_t original = le32(entry), words = le32(entry + 4);
    const uint64_t expected = le32(entry + 8) | uint64_t(le32(entry + 12)) << 32;
    Require(original >= 24 && original <= kMaxOriginal &&
               words >= 5 && words <= kMaxSpirv / 4, "Entry too large");
    Shader s;
    s.original.resize(original);
    read(s.original.data(), original);
    XXH3_64bits_update(state.get(), s.original.data(), original);
    scratch.resize(words);
    read(scratch.data(), size_t(words) * 4);
    XXH3_64bits_update(state.get(), scratch.data(), size_t(words) * 4);
    s.spirv.swap(scratch);  // the buffer, for the checks and inspect
    s.container_offset = position + sizeof(entry);
    s.spirv_offset = s.container_offset + original;
    s.spirv_words = words;
    position = s.spirv_offset + uint64_t(words) * 4;
    Validate(s);
    Require(s.hash == expected, "Bad container hash");
    if (i > 0) {
      const std::vector<uint8_t>& last = keep ? new_ones.back().original : previous_original;
      Require(previous_hash < s.hash || (previous_hash == s.hash && last < s.original),
              "Repeated or unsorted entries");
    }
    s.spirv_hash = XXH3_64bits(s.spirv.data(), s.spirv.size() * sizeof(uint32_t));
    if (inspect) {
      inspect(i, s);
    }
    s.spirv.swap(scratch);  // the buffer back; the shader keeps none (Spirv reads it from the file)
    previous_hash = s.hash;
    if (keep) {
      new_ones.push_back(std::move(s));
    } else {
      previous_original.swap(s.original);
    }
  }
  Require(position == n, "Trailing data in the shader pack");
  Require(XXH3_64bits_digest(state.get()) == hash, "Tampered shader pack");
  UseFile(file);
  shaders_ = std::move(new_ones);
  for (Shader& s : shaders_) {
    s.library = this;
  }
}

uint64_t ShaderLibrary::SpirvWords() const {
  uint64_t words = 0;
  for (const Shader& s : shaders_) {
    if (s.spirv_offset) {
      words += s.spirv_words;
    }
  }
  return words;
}

std::vector<uint32_t> Spirv(const Shader& s) {
  if (!s.library || !s.spirv_offset || !s.spirv.empty()) {
    return s.spirv;
  }
  std::vector<uint32_t> words(s.spirv_words);
  // The pack's hash is not checked over the whole file at start (bo2zm_native_shaders.cpp reads its own
  // index of it instead), so what is read is checked here, shader by shader.
  if (!s.library->ReadAt(s.spirv_offset, words.data(), words.size() * 4) ||
      (s.spirv_hash && XXH3_64bits(words.data(), words.size() * 4) != s.spirv_hash)) {
    words.clear();
  }
  return words;
}

const Shader* ShaderLibrary::Find(std::span<const uint8_t> original) const {
  if (original.size() < 24 || original.size() > kMaxOriginal) return nullptr;
  const uint64_t hash = XXH3_64bits(original.data(), original.size());
  auto it = std::lower_bound(shaders_.begin(), shaders_.end(), hash,
      [](const Shader& s, uint64_t h) { return s.hash < h; });
  // A hash collision can never pick another shader: the bytes are compared.
  for (; it != shaders_.end() && it->hash == hash; ++it)
    if (it->original.size() == original.size() &&
        std::equal(original.begin(), original.end(), it->original.begin())) return &*it;
  return nullptr;
}

std::vector<uint8_t> PackShaders(std::vector<Shader> shaders) {
  Require(!shaders.empty() && shaders.size() <= kMaxShaders, "Shader count out of bounds");
  for (auto& s : shaders) Validate(s);
  std::sort(shaders.begin(), shaders.end(), Smaller);
  std::vector<uint8_t> body;
  uint32_t amount = 0;
  const Shader* earlier = nullptr;
  for (const auto& s : shaders) {
    if (earlier && earlier->original == s.original) {
      Require(earlier->spirv == s.spirv, "A container has two different translations");
      continue;
    }
    const size_t n = 16 + s.original.size() + s.spirv.size() * 4;
    Require(n <= kMaxFile - 24 - body.size(), "Pack too large");
    U32(body, uint32_t(s.original.size())); U32(body, uint32_t(s.spirv.size()));
    U64(body, s.hash);
    body.insert(body.end(), s.original.begin(), s.original.end());
    for (uint32_t word : s.spirv) U32(body, word);
    ++amount;
    earlier = &s;
  }
  std::vector<uint8_t> file(kSignature.begin(), kSignature.end());
  U32(file, 1); U32(file, amount);
  U64(file, XXH3_64bits(body.data(), body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}
}  // namespace bo2zm::shaderlib
