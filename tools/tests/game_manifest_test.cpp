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
}  // namespace
namespace rex::cvar {
Source GetFlagSource(std::string_view name) {
  auto it = g_flags.find(std::string(name));
  return it == g_flags.end() ? Source::kDefault : it->second.second;
}
bool SetFlagByName(std::string_view name, std::string_view value) {
  auto it = g_flags.find(std::string(name));
  if (it == g_flags.end()) return false;
  it->second = {std::string(value), Source::kRuntime};
  return true;
}
}  // namespace rex::cvar

int main(int argc, char** argv) {
  assert(argc == 2);
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
  assert(bad.short_name == "bo2zm" && bad.title_ids == bo2.title_ids);

  std::puts("game manifest: BO2 values, defaults-only-when-default, partial and bad manifests passed");
  return 0;
}
