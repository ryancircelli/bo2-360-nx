/**
 * @file        game_manifest.cpp
 * @brief       Title manifest reader (rex/game_info.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/cvar.h>
#include <rex/game_info.h>

#include <charconv>
#include <limits>

#include <toml++/toml.hpp>

namespace rex {
namespace {

bool Fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

bool ReadString(const toml::table& table, const char* key, std::string& out, std::string* error) {
  const toml::node* node = table.get(key);
  if (!node) return true;
  if (const auto* value = node->as_string()) {
    out = value->get();
    return true;
  }
  return Fail(error, std::string("game.") + key + " must be a string");
}

}  // namespace

// A TOML scalar as cvar text.
bool ScalarText(const toml::node& node, std::string& out) {
  if (const auto* s = node.as_string()) {
    out = s->get();
  } else if (const auto* b = node.as_boolean()) {
    out = b->get() ? "true" : "false";
  } else if (const auto* i = node.as_integer()) {
    out = std::to_string(i->get());
  } else if (const auto* f = node.as_floating_point()) {
    out = std::to_string(f->get());
  } else {
    return false;
  }
  return true;
}

// Settings a renderer profile may name: the GPU backends' own.
bool IsRendererSetting(std::string_view name) {
  for (std::string_view prefix : {"metal_", "vulkan_", "d3d12_", "gpu_", "draw_resolution_scale_"}) {
    if (name.starts_with(prefix)) return true;
  }
  return name == "half_pixel_offset" || name == "anisotropic_override";
}

bool LoadGameManifest(std::string_view toml_text, GameInfo& info, std::string* error) {
  toml::table root;
  try {
    root = toml::parse(toml_text);
  } catch (const toml::parse_error& err) {
    return Fail(error, std::string(err.description()));
  }

  GameInfo next = info;
  if (const toml::table* game = root["game"].as_table()) {
    if (!ReadString(*game, "name", next.short_name, error) ||
        !ReadString(*game, "executable", next.executable, error) ||
        !ReadString(*game, "data_root", next.data_root, error) ||
        !ReadString(*game, "local_data_root", next.local_data_root, error) ||
        !ReadString(*game, "shader_pack", next.shader_pack, error) ||
        !ReadString(*game, "pipeline_list", next.pipeline_list, error)) {
      return false;
    }
    if (const toml::node* ids = game->get("title_ids")) {
      const toml::array* list = ids->as_array();
      if (!list) return Fail(error, "game.title_ids must be an array of hex strings");
      next.title_ids.clear();
      for (const toml::node& id : *list) {
        const auto* text = id.as_string();
        uint32_t value = 0;
        const std::string& s = text ? text->get() : std::string();
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
        if (!text || s.empty() || ec != std::errc() || end != s.data() + s.size()) {
          return Fail(error, "game.title_ids entries must be hex strings such as \"415608C3\"");
        }
        next.title_ids.push_back(value);
      }
    }
    if (const toml::node* port = game->get("first_link_port")) {
      const auto* value = port->as_integer();
      if (!value || value->get() < 1 || value->get() > std::numeric_limits<uint16_t>::max() - 2) {
        return Fail(error, "game.first_link_port must be a port number");
      }
      next.first_link_port = uint16_t(value->get());
    }
  }

  if (const toml::node* defaults_node = root.get("defaults")) {
    const toml::table* defaults = defaults_node->as_table();
    if (!defaults) return Fail(error, "defaults must be a table");
    next.default_cvars.clear();
    for (const auto& [key, node] : *defaults) {
      std::string value;
      if (!ScalarText(node, value)) {
        return Fail(error, "defaults." + std::string(key.str()) + " must be a string, boolean or number");
      }
      next.default_cvars.emplace_back(std::string(key.str()), std::move(value));
    }
  }

  if (const toml::node* renderer_node = root.get("renderer")) {
    const toml::table* renderer = renderer_node->as_table();
    if (!renderer) return Fail(error, "renderer must be a table");
    if (const toml::node* quirks_node = renderer->get("quirks")) {
      const toml::array* quirks = quirks_node->as_array();
      if (!quirks) return Fail(error, "renderer.quirks must be an array of names");
      next.renderer.quirks.clear();
      for (const toml::node& quirk : *quirks) {
        const auto* name = quirk.as_string();
        if (!name || name->get().empty() || name->get().find(',') != std::string::npos) {
          return Fail(error, "renderer.quirks entries must be names such as \"fable2\"");
        }
        next.renderer.quirks.push_back(name->get());
      }
    }
    if (const toml::node* settings_node = renderer->get("settings")) {
      const toml::table* settings = settings_node->as_table();
      if (!settings) return Fail(error, "renderer.settings must be a table");
      next.renderer.settings.clear();
      for (const auto& [key, node] : *settings) {
        std::string name(key.str());
        if (!IsRendererSetting(name)) {
          return Fail(error, "renderer.settings." + name + " is not a GPU backend setting");
        }
        std::string value;
        if (!ScalarText(node, value)) {
          return Fail(error, "renderer.settings." + name + " must be a string, boolean or number");
        }
        next.renderer.settings.emplace_back(std::move(name), std::move(value));
      }
    }
  }

  info = std::move(next);
  return true;
}

int ApplyGameDefaults(const GameInfo& info) {
  int applied = 0;
  for (const auto& [name, value] : info.default_cvars) {
    if (cvar::GetFlagSource(name) == cvar::Source::kDefault && cvar::SetFlagByName(name, value)) {
      ++applied;
    }
  }
  return applied;
}

bool ApplyRendererProfile(const RendererProfile& profile) {
  bool ok = true;
  if (!profile.quirks.empty()) {
    std::string quirks;
    for (const auto& quirk : profile.quirks) {
      quirks += (quirks.empty() ? "" : ",") + quirk;
    }
    ok = cvar::SetDefaultValue("metal_title_quirks", quirks) && ok;
  }
  for (const auto& [name, value] : profile.settings) {
    ok = cvar::SetDefaultValue(name, value) && ok;
  }
  return ok;
}

}  // namespace rex
