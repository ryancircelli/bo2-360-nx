// Bounded internal bot packs. Objects keep their embedded names and use the
// same startup/export checks as folder mods; no filename aliases or legacy fixups.
// Wire format: B2BOT001, BE u32 INI length, BE u32 file count, exact INI bytes;
// each record is BE u16 name length, BE u32 object length, name, object bytes.
// All byte lengths exclude terminators. Padding and trailing bytes are forbidden.
#pragma once
#include "bo2zm_gsc_catalog.h"
#include <array>

namespace bo2zm::gsc {
struct Bundle {
  ModOptions options;
  std::vector<Object> objects;
};

inline Bundle ReadBotBundle(const std::filesystem::path& path) {
  namespace fs = std::filesystem;
  // Check ancestors before canonicalization could hide a link. Switch device
  // paths must reach the filesystem unchanged, rather than gaining a host root.
#if defined(__SWITCH__)
  const auto file = path;
#else
  const auto file = fs::absolute(path);
#endif
  for (auto part = file; !part.empty();) {
    if (LinkedPath(part)) throw std::runtime_error("linked bot bundles are unsupported");
    const auto parent = part.parent_path();
    if (parent == part) break;
    part = parent;
  }
  if (!fs::is_regular_file(file)) throw std::runtime_error("bot bundle must be a regular file");
  constexpr size_t payload_limit = 4 * 1024 * 1024;
  const auto file_size = fs::file_size(file);
  if (file_size < 16 || file_size > payload_limit + 8192)
    throw std::runtime_error("bot bundle size is outside limits");
  size_t remaining = size_t(file_size);
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read bot bundle");
  const auto read = [&](void* destination, size_t size) {
    if (size > remaining) throw std::runtime_error("truncated bot bundle");
    if (!input.read(static_cast<char*>(destination), std::streamsize(size)))
      throw std::runtime_error("cannot read bot bundle");
    remaining -= size;
  };
  std::array<uint8_t, 16> header;
  read(header.data(), header.size());
  constexpr std::string_view magic = "B2BOT001";
  if (!std::equal(magic.begin(), magic.end(), header.begin()))
    throw std::runtime_error("invalid bot bundle magic");
  const auto metadata_size = Read32(header, 8), count = Read32(header, 12);
  if (metadata_size < 1 || metadata_size > 1024 || metadata_size > remaining || count < 1 || count > 32)
    throw std::runtime_error("invalid bot bundle metadata length or file count");
  std::string metadata(metadata_size, '\0');
  read(metadata.data(), metadata.size());
  Bundle bundle;
  bundle.options = ParseModOptions(metadata);
  if (bundle.options.mode != ScriptMode::Zombies || bundle.options.names_from_files || bundle.options.entry.empty())
    throw std::runtime_error("bot bundle requires Zombies, internal names and a default entry");
  std::unordered_set<std::string> names;
  size_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    // Record lengths precede both the name and object bytes. Subtraction keeps
    // the checks safe on 32-bit hosts even for hostile u32 lengths.
    std::array<uint8_t, 6> lengths;
    read(lengths.data(), lengths.size());
    const size_t name_size = Read16(lengths, 0), object_size = Read32(lengths, 2);
    if (name_size < 1 || name_size > 127 || name_size > remaining ||
        object_size < 64 || object_size > 1024 * 1024 || object_size > remaining - name_size ||
        object_size > payload_limit - total)
      throw std::runtime_error("invalid bot bundle record lengths or payload size");
    Object object;
    object.source.resize(name_size);
    read(object.source.data(), name_size);
    if (!ScriptName(object.source)) throw std::runtime_error("invalid bot bundle script path");
    if (!names.insert(object.source).second) throw std::runtime_error("duplicate bot bundle script path");
    object.bytes.resize(object_size);
    read(object.bytes.data(), object_size);
    std::string error;
    if (!Validate(object.bytes, false, object.info, error))
      throw std::runtime_error(object.source + ": " + error);
    if (object.source != object.info.name)
      throw std::runtime_error("bot bundle path differs from embedded internal name");
    total += object_size;
    bundle.objects.push_back(std::move(object));
  }
  if (remaining || input.peek() != std::char_traits<char>::eof() || input.bad())
    throw std::runtime_error("bot bundle has trailing bytes or a read error");
  ValidateModObjects(bundle.objects, bundle.options);
  return bundle;
}
}  // namespace bo2zm::gsc
