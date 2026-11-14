// Title manifest reader (sdk/src/core/game_manifest.cpp) against games/bo2/game.toml and bad input.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/game_info.h>

// The two cvar calls ApplyGameDefaults makes, over a small table.
namespace {
std::map<std::string, std::pair<std::string, rex::cvar::Source>> g_flags = {
    {"offline_user_count", {"1", rex::cvar::Source::kDefault}},
    {"fullscreen", {"false", rex::cvar::Source::kConfig}},
};
std::map<std::string, std::string> g_defaults;
}  // namespace
namespace rex::cvar {
Source GetFlagSource(std::string_view name) {
  auto it = g_flags.find(std::string(name));
  return it == g_flags.end() ? Source::kDefault : it->second.second;
}
bool SetDefaultValue(std::string_view name, std::string_view value) {
  if (value == "bad") return false;
  g_defaults[std::string(name)] = std::string(value);
  return true;
}
bool SetFlagByName(std::string_view name, std::string_view value) {
  auto it = g_flags.find(std::string(name));
  if (it == g_flags.end()) return false;
  it->second = {std::string(value), Source::kRuntime};
  return true;
}
}  // namespace rex::cvar

int main(int argc, char** argv) {
  assert(argc == 3 || argc == 6);
  std::ifstream file(argv[1]);
  std::stringstream text;
  text << file.rdbuf();

  rex::GameInfo bo2;
  std::string error;
  if (!rex::LoadGameManifest(text.str(), bo2, &error)) {
    std::fprintf(stderr, "games/bo2/game.toml: %s\n", error.c_str());
    return 1;
  }
  // The values BO2 hard-coded before the manifest.
  assert(bo2.short_name == "bo2zm");
  assert(bo2.title_ids.size() == 1 && bo2.title_ids[0] == 0x415608C3u);
  assert(bo2.executable == "game:\\default_mp.xex");
  assert(bo2.data_root == "game");
  assert(bo2.shader_pack == "bo2zm_shaders.spvpack");
  assert(bo2.pipeline_list == "bo2zm_pso.psolist");
  assert(bo2.first_link_port == 1000);
  assert(bo2.default_cvars.size() == 1 && bo2.default_cvars[0].first == "offline_user_count" &&
         bo2.default_cvars[0].second == "4");

  // BO2's renderer profile names the Metal settings it ships with, each at the SDK's own
  // default (sdk/src/graphics/metal/metal_flags.cpp), so applying it changes nothing.
  assert(bo2.renderer.quirks.empty() && bo2.renderer.settings.size() == 6);
  {
    std::ifstream flags_file(argv[2]);
    std::stringstream flags;
    flags << flags_file.rdbuf();
    for (const auto& [name, value] : bo2.renderer.settings) {
      const std::string needle = "(" + name + ", " + value + ",";
      if (flags.str().find(needle) == std::string::npos) {
        std::fprintf(stderr, "renderer.settings.%s = %s is not the SDK default\n", name.c_str(), value.c_str());
        return 1;
      }
    }
  }
  assert(rex::ApplyRendererProfile(bo2.renderer));
  assert(g_defaults.size() == 6 && g_defaults["metal_present_drawables"] == "2" &&
         g_defaults.count("metal_title_quirks") == 0);
  rex::GameInfo fable;
  assert(rex::LoadGameManifest("[renderer]\nquirks = [\"fable2\", \"other\"]\n[renderer.settings]\nmetal_retina = false\n",
                               fable, &error));
  assert(rex::ApplyRendererProfile(fable.renderer));
  assert(g_defaults["metal_title_quirks"] == "fable2,other" && g_defaults["metal_retina"] == "false");
  fable.renderer.settings.emplace_back("metal_retina", "bad");
  assert(!rex::ApplyRendererProfile(fable.renderer));

  // Defaults apply only to settings still at their compiled-in default.
  bo2.default_cvars.emplace_back("fullscreen", "true");
  assert(rex::ApplyGameDefaults(bo2) == 1);
  assert(g_flags["offline_user_count"].first == "4");
  assert(g_flags["fullscreen"].first == "false");

  // A manifest leaving keys out keeps the current values.
  rex::GameInfo partial;
  assert(rex::LoadGameManifest("[game]\nname = \"nfsmw\"\n", partial, &error));
  assert(partial.short_name == "nfsmw" && partial.first_link_port == 1000 && partial.data_root == "game");

  // Bad input fails and leaves the info alone.
  rex::GameInfo bad = bo2;
  assert(!rex::LoadGameManifest("[game]\ntitle_ids = [\"zz\"]\n", bad, &error) && !error.empty());
  assert(!rex::LoadGameManifest("[game]\nname = 3\n", bad, &error));
  assert(!rex::LoadGameManifest("[game]\nfirst_link_port = 70000\n", bad, &error));
  assert(!rex::LoadGameManifest("[game\n", bad, &error));
  assert(!rex::LoadGameManifest("[renderer.settings]\noffline_user_count = 2\n", bad, &error));
  assert(!rex::LoadGameManifest("[renderer]\nquirks = [\"a,b\"]\n", bad, &error));
  assert(!rex::LoadGameManifest("[renderer]\nquirks = \"fable2\"\n", bad, &error));
  assert(bad.short_name == "bo2zm" && bad.title_ids == bo2.title_ids);

  // BO2 has no development disc fallback; the other packs name theirs.
  assert(bo2.local_data_root.empty());
  assert(rex::LoadGameManifest("[game]\nlocal_data_root = \"~/Games/Xbox360/x/disc\"\n", partial, &error) &&
         partial.local_data_root == "~/Games/Xbox360/x/disc");

  // The other title packs (games/nfsmw, fh1, bo1): default.xex, one title id, Metal by default.
  const std::pair<const char*, uint32_t> packs[] = {{"nfsmw", 0x454107D9u}, {"fh1", 0x4D5309C9u}, {"bo1", 0x41560855u}};
  for (int i = 3; i < argc; ++i) {
    std::ifstream pack_file(argv[i]);
    std::stringstream pack_text;
    pack_text << pack_file.rdbuf();
    rex::GameInfo pack;
    if (!rex::LoadGameManifest(pack_text.str(), pack, &error)) {
      std::fprintf(stderr, "%s: %s\n", argv[i], error.c_str());
      return 1;
    }
    const auto& [name, title] = packs[i - 3];
    assert(pack.short_name == name && pack.title_ids.size() == 1 && pack.title_ids[0] == title);
    assert(pack.executable == "game:\\default.xex");
    assert(pack.local_data_root == std::string("~/Games/Xbox360/") + name + "/disc");
    assert(pack.default_cvars.size() == 1 && pack.default_cvars[0].first == "gpu_plugin" &&
           pack.default_cvars[0].second == "metal");
  }

  std::puts("game manifest: BO2 values, renderer profile, defaults-only-when-default, partial and bad manifests, other title packs passed");
  return 0;
}
