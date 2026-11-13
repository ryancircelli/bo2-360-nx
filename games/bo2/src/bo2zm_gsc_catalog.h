// Read-only discovery and validation shared by script selection and VM loading.
#pragma once
#include "bo2zm_gsc_object.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bo2zm::gsc {
inline bool LinkedPath(const std::filesystem::path& path) {
  if (std::filesystem::is_symlink(path)) return true;
#if defined(_WIN32)
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return false;
#endif
}
inline std::filesystem::path LocalPath(const std::filesystem::path& path) {
#if defined(__SWITCH__)
  return path.lexically_normal();
#else
  return std::filesystem::canonical(path);
#endif
}

inline bool ModName(std::string_view name) {
  if (name.empty() || name.size() > 64 || name.find("..") != name.npos || name.ends_with('.')) return false;
  const auto alpha_num = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  if (!alpha_num(name.front()) && name.front() != '_') return false;
  return std::all_of(name.begin(), name.end(), [&](char c) {
    return alpha_num(c) || c == '-' || c == '_' || c == '.';
  });
}

struct Object {
  ObjectInfo info;
  std::vector<uint8_t> bytes;
  std::string source;
};

enum class ScriptMode { Zombies, Multiplayer, Campaign };
struct ScriptExport {
  std::string file, function;
};
struct ScriptReplacement {
  ScriptExport source, target;
};
inline ScriptReplacement ParseReplacement(std::string_view value) {
  const auto arrow = value.find("=>");
  if (arrow == value.npos || value.find("=>", arrow + 2) != value.npos)
    throw std::runtime_error("invalid replace in mod.ini; expected file.gsc::function=>file.gsc::function");
  const auto endpoint = [](std::string_view text) {
    const auto split = text.find("::");
    if (split == text.npos) throw std::runtime_error("invalid replacement export");
    ScriptExport result{std::string(text.substr(0, split)), std::string(text.substr(split + 2))};
    const auto& name = result.function;
    if (!ScriptName(result.file) || name.empty() || name.size() > 64 ||
        !((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_') ||
        !std::all_of(name.begin(), name.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        })) throw std::runtime_error("invalid replacement export");
    return result;
  };
  ScriptReplacement result{endpoint(value.substr(0, arrow)), endpoint(value.substr(arrow + 2))};
  if (result.source.file == result.target.file) throw std::runtime_error("replacement target must be a different mod script");
  return result;
}
// Call only after structural validation. A replacement must identify one public,
// zero-argument export; ambiguous overloads and nonpublic exports fail closed.
inline size_t ReplacementExport(std::span<const uint8_t> bytes, std::string_view name) {
  const auto table = Read32(bytes, 0x1C);
  size_t found = 0;
  for (size_t i = 0; i < Read16(bytes, 0x34); ++i) {
    const auto row = table + i * 12;
    const auto text = Read16(bytes, row + 8);
    if (std::string_view(reinterpret_cast<const char*>(bytes.data() + text)) != name) continue;
    if (found || bytes[row + 10] != 0 || (bytes[row + 11] & 0x10))
      throw std::runtime_error("replacement requires a unique public zero-argument export");
    found = row;
  }
  if (!found) throw std::runtime_error("replacement export not found");
  return found;
}
struct ModOptions {
  ScriptMode mode = ScriptMode::Zombies;
  bool names_from_files = false;
  std::string base;
  std::string entry;
  std::string replace;
  std::string maps;
  std::vector<std::pair<std::string, std::string>> map_entries;
};

inline std::string_view IniTrim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r");
  if (first == text.npos) return {};
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}
inline std::string IniLower(std::string_view text) {
  std::string result(text);
  for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
inline std::vector<std::string> ParseMaps(std::string_view value) {
  std::vector<std::string> result;
  for (;;) {
    const auto comma = value.find(',');
    const auto map = IniTrim(value.substr(0, comma));
    if (map.empty() || map.size() > 63 || result.size() >= 16 ||
        !std::all_of(map.begin(), map.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }) || std::find(result.begin(), result.end(), map) != result.end())
      throw std::runtime_error("invalid maps in mod.ini; expected 1-16 unique lowercase map names");
    result.emplace_back(map);
    if (comma == value.npos) return result;
    value.remove_prefix(comma + 1);
  }
}
inline bool AppliesToMap(std::string_view maps, std::string_view map) {
  if (maps.empty()) return true;
  const auto allowed = ParseMaps(maps);
  return std::find(allowed.begin(), allowed.end(), map) != allowed.end();
}
inline std::vector<std::pair<std::string, std::string>> ParseMapEntries(std::string_view value) {
  std::vector<std::pair<std::string, std::string>> entries;
  for (;;) {
    const auto comma = value.find(',');
    const auto item = IniTrim(value.substr(0, comma));
    const auto colon = item.find(':');
    if (colon == item.npos || entries.size() >= 16)
      throw std::runtime_error("invalid map_entries in mod.ini; expected map:script.gsc pairs");
    const auto map = IniTrim(item.substr(0, colon));
    const auto file = IniTrim(item.substr(colon + 1));
    ParseMaps(map);
    if (!ScriptName(file) || std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
          return entry.first == map;
        })) throw std::runtime_error("invalid map_entries in mod.ini; expected unique maps and supported scripts");
    entries.emplace_back(map, file);
    if (comma == value.npos) return entries;
    value.remove_prefix(comma + 1);
  }
}
inline std::string MapEntriesText(const std::vector<std::pair<std::string, std::string>>& entries) {
  std::string text;
  for (const auto& [map, file] : entries) {
    if (!text.empty()) text += ',';
    text += map + ':' + file;
  }
  return text;
}

// Every mod declares its mode explicitly; shared script names must never select
// a mode by accident. Campaign metadata is reserved until its VM loader exists.
inline ModOptions ParseModOptions(std::string_view text) {
  if (text.size() > 1024) throw std::runtime_error("mod.ini exceeds 1024 bytes");
  std::string record(text);
  if (record.starts_with("\xEF\xBB\xBF")) record.erase(0, 3);
  std::istringstream lines(record);
  ModOptions options;
  std::unordered_set<std::string> keys;
  bool section = false;
  std::string line;
  while (std::getline(lines, line)) {
    const auto text = IniTrim(line);
    if (text.empty() || text.front() == '#' || text.front() == ';') continue;
    if (text.front() == '[') {
      if (IniLower(text) != "[mod]") throw std::runtime_error("unknown section in mod.ini; expected [mod]");
      if (section) throw std::runtime_error("duplicate [mod] section in mod.ini");
      section = true;
      continue;
    }
    const auto equals = text.find('=');
    if (equals == text.npos) throw std::runtime_error("expected key=value in mod.ini");
    const auto key = IniLower(IniTrim(text.substr(0, equals)));
    const auto value = IniTrim(text.substr(equals + 1));
    if (key != "mode" && key != "names" && key != "base" && key != "entry" && key != "replace" && key != "maps" && key != "map_entries")
      throw std::runtime_error("unknown key in mod.ini; expected mode, names, base, entry, replace, maps or map_entries");
    if (!keys.insert(key).second) throw std::runtime_error("duplicate " + key + " in mod.ini");
    const auto lower = IniLower(value);
    if (key == "mode") {
      if (lower == "zombies") options.mode = ScriptMode::Zombies;
      else if (lower == "multiplayer") options.mode = ScriptMode::Multiplayer;
      else if (lower == "campaign") options.mode = ScriptMode::Campaign;
      else throw std::runtime_error("invalid mode in mod.ini; expected zombies, multiplayer or campaign");
    } else if (key == "names") {
      if (lower != "internal" && lower != "files")
        throw std::runtime_error("invalid names in mod.ini; expected internal or files");
      options.names_from_files = lower == "files";
    } else if (key == "map_entries") {
      options.map_entries = ParseMapEntries(value);
    } else if (key == "maps") {
      for (const auto& map : ParseMaps(value)) {
        if (!options.maps.empty()) options.maps += ',';
        options.maps += map;
      }
    } else if (key == "replace") {
      options.replace = std::string(value);
      std::replace(options.replace.begin(), options.replace.end(), '\\', '/');
      ParseReplacement(options.replace);
    } else if (key == "entry") {
      options.entry = std::string(value);
      std::replace(options.entry.begin(), options.entry.end(), '\\', '/');
      if (!ScriptName(options.entry)) throw std::runtime_error("invalid entry in mod.ini; expected a full supported .gsc path");
    } else {
      options.base = std::string(value);
      std::replace(options.base.begin(), options.base.end(), '\\', '/');
      if (options.base != "/") {
        while (options.base.ends_with('/')) options.base.pop_back();
        if (!ScriptPath(options.base))
          throw std::runtime_error("invalid base in mod.ini; expected /, maps/mp or common_scripts and optional subfolders");
      }
    }
  }
  if (!keys.contains("mode")) throw std::runtime_error("missing mode in mod.ini; declare mode=zombies or mode=multiplayer");
  if (keys.contains("base") && !options.names_from_files)
    throw std::runtime_error("base in mod.ini requires names=files");
  if (options.names_from_files && !keys.contains("base"))
    throw std::runtime_error("missing base in mod.ini; names=files requires base=/ or a target folder");
  if (!options.replace.empty() && options.entry.empty()) throw std::runtime_error("replace requires an explicit entry script");
  if (!options.map_entries.empty() && options.entry.empty()) throw std::runtime_error("map_entries requires a default entry script");
  return options;
}
inline ModOptions ReadModOptions(const std::filesystem::path& folder) {
  const auto path = folder / "mod.ini";
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) throw std::runtime_error("cannot read mod mode");
    throw std::runtime_error("missing mod.ini; declare mode=zombies or mode=multiplayer");
  }
  if (LinkedPath(path) || !std::filesystem::is_regular_file(path))
    throw std::runtime_error("mod.ini must be a regular, unlinked file");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read mod.ini");
  char buffer[1025];
  input.read(buffer, sizeof(buffer));
  if (input.bad()) throw std::runtime_error("cannot read mod.ini");
  const auto size = size_t(input.gcount());
  if (size > 1024) throw std::runtime_error("mod.ini exceeds 1024 bytes");
  return ParseModOptions(std::string_view(buffer, size));
}
inline ScriptMode ModMode(const std::filesystem::path& folder) {
  return ReadModOptions(folder).mode;
}

inline std::vector<std::string> ListMods(const std::filesystem::path& root,
                                       std::optional<ScriptMode> mode = {},
                                       std::vector<std::string>* issues = nullptr) {
  if (issues) issues->clear();
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    if (ec && ec != std::errc::no_such_file_or_directory)
      throw std::filesystem::filesystem_error("cannot scan script root", root, ec);
    ec.clear();
    if (std::filesystem::exists(root, ec)) throw std::runtime_error("script root is not a folder");
    if (ec && ec != std::errc::no_such_file_or_directory)
      throw std::filesystem::filesystem_error("cannot inspect script root", root, ec);
    return {};
  }
  std::vector<std::string> result;
  const auto base = LocalPath(root);
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(base)) {
    if (++count > 512) throw std::runtime_error("too many script-root entries");
    const auto name = entry.path().filename().string();
    try {
      if (LinkedPath(entry.path())) throw std::runtime_error("linked mod folders are unsupported");
      if (!entry.is_directory()) continue;
      if (!ModName(name))
        throw std::runtime_error("portable folder name expected: 1-64 letters, digits, underscores, dots or dashes; "
                                 "start with a letter, digit or underscore; no '..' or trailing dot");
      if (LocalPath(entry.path()) != base / name)
        throw std::runtime_error("linked mod folders are unsupported");
      const auto declared = ModMode(entry.path());
      if (declared == ScriptMode::Campaign || (mode && declared != *mode)) continue;
    } catch (const std::exception& error) {
      if (issues && issues->size() < 32) issues->push_back(name + ": " + error.what());
      continue;
    }
    result.push_back(name);
  }
  std::sort(result.begin(), result.end());
  if (result.size() > 32) result.resize(32);
  return result;
}

// Objects must already have passed Validate and any filename routing.
inline void ValidateModObjects(const std::vector<Object>& objects, const ModOptions& options) {
  std::unordered_set<std::string> names;
  for (const auto& object : objects)
    if (!names.insert(object.info.name).second) throw std::runtime_error("duplicate internal script name");
  std::vector<std::string> startup_entries;
  if (!options.entry.empty()) startup_entries.push_back(options.entry);
  for (const auto& [map, file] : options.map_entries) startup_entries.push_back(file);
  for (const auto& startup_entry : startup_entries) {
    const auto entry = std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
      return object.info.name == startup_entry;
    });
    if (entry == objects.end()) throw std::runtime_error("entry script is not present in this mod");
    bool startup = false;
    const auto table = Read32(entry->bytes, 0x1C);
    for (size_t i = 0; i < Read16(entry->bytes, 0x34); ++i) {
      const auto row = table + i * 12;
      const auto name = Read16(entry->bytes, row + 8);
      const auto* text = reinterpret_cast<const char*>(entry->bytes.data() + name);
      if (std::string_view(text) != "main" && std::string_view(text) != "init") continue;
      if (entry->bytes[row + 11] & 2)
        throw std::runtime_error("entry main or init must not also be an autoexec export");
      try { ReplacementExport(entry->bytes, text); }
      catch (const std::exception&) {
        throw std::runtime_error("entry requires unique public zero-argument main or init exports");
      }
      startup = true;
    }
    if (!startup) throw std::runtime_error("entry script requires a zero-argument main or init export");
  }
  if (!options.replace.empty()) {
    const auto replacement = ParseReplacement(options.replace);
    if (names.contains(replacement.source.file))
      throw std::runtime_error("replacement source must be a stock script outside this mod");
    const auto target = std::find_if(objects.begin(), objects.end(), [&](const auto& object) {
      return object.info.name == replacement.target.file;
    });
    if (target == objects.end()) throw std::runtime_error("replacement target script is not present in this mod");
    ReplacementExport(target->bytes, replacement.target.function);
  }
}

inline std::vector<Object> ReadMod(const std::filesystem::path& root,
                                   std::string_view selected, bool legacy,
                                   std::optional<ScriptMode> mode = {}) {
  if (!ModName(selected)) throw std::runtime_error("invalid mod folder name");
  const auto base = LocalPath(root);
  if (LinkedPath(base / selected)) throw std::runtime_error("linked mod folders are unsupported");
  const auto folder = LocalPath(base / selected);
  if (folder.parent_path() != base) throw std::runtime_error("mod folder escapes its root");
  const auto options = ReadModOptions(folder);
  if (options.mode == ScriptMode::Campaign) throw std::runtime_error("Campaign script loading is not supported yet");
  if (mode && options.mode != *mode) throw std::runtime_error("mod is for another gamemode");
  std::vector<std::filesystem::path> files;
  size_t entries = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
    if (++entries > 512) throw std::runtime_error("too many directory entries");
    if (LinkedPath(entry.path())) throw std::runtime_error("linked entries are unsupported in mod folders");
    const auto relative = LocalPath(entry.path()).lexically_relative(folder);
    if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("mod entry escapes its folder");
    if (entry.is_regular_file() && entry.path().extension() == ".gsc") files.push_back(entry.path());
  }
  if (files.empty() || files.size() > 32) throw std::runtime_error("expected 1 to 32 compiled scripts");
  std::sort(files.begin(), files.end());
  std::vector<Object> result;
  std::unordered_set<std::string> names;
  size_t total = 0;
  for (const auto& file : files) {
    const auto size = std::filesystem::file_size(file);
    if (size < 64 || size > 1024 * 1024 || size > 4 * 1024 * 1024 - total)
      throw std::runtime_error("script mod exceeds size limits");
    Object object;
    object.source = file.lexically_relative(folder).generic_string();
    object.bytes.resize(size_t(size));
    std::ifstream input(file, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(object.bytes.data()), std::streamsize(size)))
      throw std::runtime_error("cannot read script object");
    std::string error;
    if (!Validate(object.bytes, legacy, object.info, error))
      throw std::runtime_error(file.filename().string() + ": " + error);
    if (options.names_from_files) {
      const auto& relative = object.source;
      object.info.name = options.base == "/" ? relative : options.base + "/" + relative;
      if (!ScriptName(object.info.name))
        throw std::runtime_error("invalid filename-based script name; check base and folder structure");
    }
    if (!names.insert(object.info.name).second) throw std::runtime_error("duplicate internal script name");
    total += size_t(size);
    result.push_back(std::move(object));
  }
  ValidateModObjects(result, options);
  return result;
}
}  // namespace bo2zm::gsc
