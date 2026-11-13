// Compose optional script components before exposing any objects to the VM.
#pragma once
#include "bo2zm_gsc_catalog.h"

namespace bo2zm::gsc {
struct Component {
  ModOptions options;
  std::vector<Object> objects;
};
struct Plan {
  std::vector<Object> objects;
  std::vector<std::string> entries;
  std::string replace;
};
inline std::string MapEntry(const ModOptions& options, std::string_view map) {
  for (const auto& [name, file] : options.map_entries) if (name == map) return file;
  return options.entry;
}
inline bool SameComponent(const Component& a, const Component& b) {
  if (a.options.mode != b.options.mode || a.options.entry != b.options.entry ||
      a.options.replace != b.options.replace || a.options.maps != b.options.maps ||
      a.options.map_entries != b.options.map_entries || a.objects.size() != b.objects.size()) return false;
  for (const auto& object : a.objects) {
    const auto found = std::find_if(b.objects.begin(), b.objects.end(), [&](const auto& other) {
      return object.info.name == other.info.name && object.bytes == other.bytes;
    });
    if (found == b.objects.end()) return false;
  }
  return true;
}
// Transactional: a refused component leaves the already selected mod intact.
inline void AddComponent(Plan& plan, const Component& component, std::string_view map) {
  if (!AppliesToMap(component.options.maps, map)) return;
  auto next = plan;
  if (!component.options.replace.empty()) {
    if (!next.replace.empty() && next.replace != component.options.replace)
      throw std::runtime_error("mod and bots request different stock-script redirects");
    next.replace = component.options.replace;
  }
  for (const auto& object : component.objects) {
    const auto found = std::find_if(next.objects.begin(), next.objects.end(), [&](const auto& other) {
      return object.info.name == other.info.name;
    });
    if (found != next.objects.end()) {
      if (found->bytes != object.bytes) throw std::runtime_error("script conflict: " + object.info.name);
    } else next.objects.push_back(object);
  }
  if (!next.replace.empty()) {
    const auto redirect = ParseReplacement(next.replace);
    if (std::any_of(next.objects.begin(), next.objects.end(), [&](const auto& object) {
          return object.info.name == redirect.source.file;
        })) throw std::runtime_error("mod overrides the stock script required by bots: " + redirect.source.file);
  }
  size_t size = 0;
  for (const auto& object : next.objects) size += object.bytes.size();
  if (next.objects.size() > 32 || size > 4 * 1024 * 1024)
    throw std::runtime_error("combined mod and bots exceed the script budget");
  const auto entry = MapEntry(component.options, map);
  if (!entry.empty() && std::find(next.entries.begin(), next.entries.end(), entry) == next.entries.end())
    next.entries.push_back(entry);
  plan = std::move(next);
}
}  // namespace bo2zm::gsc
