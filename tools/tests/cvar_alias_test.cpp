// Old cvar names (REXCVAR_ALIAS) against sdk/src/core/cvar.cpp: config files, command line, environment,
// lookups by name, change callbacks, SaveConfig and registration order.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/platform/env.h>

namespace rex::platform::env {
std::optional<std::string> get(std::string_view name) {
  const char* value = std::getenv(std::string(name).c_str());
  return value ? std::optional<std::string>(value) : std::nullopt;
}
bool set(std::string_view name, std::string_view value) {
  return setenv(std::string(name).c_str(), std::string(value).c_str(), 1) == 0;
}
bool unset(std::string_view name) {
  return unsetenv(std::string(name).c_str()) == 0;
}
}  // namespace rex::platform::env

// The alias registers before its cvar here; the other order is covered below.
REXCVAR_ALIAS(old_limit, fps_limit);
REXCVAR_DEFINE_INT32(fps_limit, 60, "Test", "frame cap");
REXCVAR_DEFINE_BOOL(postfx, true, "Test", "post effects");
REXCVAR_ALIAS(old_postfx, postfx);
REXCVAR_DEFINE_STRING(label, "a", "Test", "label");
REXCVAR_ALIAS(old_label, label);
REXCVAR_DEFINE_INT32(plain, 1, "Test", "no alias");

namespace cv = rex::cvar;

namespace {
std::filesystem::path Write(const char* name, const char* text) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path) << text;
  return path;
}

void Reset() {
  cv::testing::ResetAllForTesting();
  for (const char* name : {"REX_OLD_LIMIT", "REX_FPS_LIMIT", "REX_OLD_LABEL", "REX_LABEL"}) {
    unsetenv(name);
  }
}

void Init(std::initializer_list<const char*> args) {
  std::vector<char*> argv{const_cast<char*>("test")};
  for (const char* arg : args) argv.push_back(const_cast<char*>(arg));
  cv::Init(int(argv.size()), argv.data());
}
}  // namespace

int main() {
  // Lookups by the old name reach the same storage.
  assert(cv::CanonicalName("old_limit") == "fps_limit");
  assert(cv::CanonicalName("plain") == "plain");
  assert(cv::SetFlagByName("old_limit", "30"));
  assert(REXCVAR_GET(fps_limit) == 30);
  assert(cv::GetFlagByName("old_limit") == "30");
  assert(cv::GetFlagInfo("old_limit") != nullptr && cv::GetFlagInfo("old_limit")->name == "fps_limit");
  assert(cv::GetFlagSource("old_limit") == cv::Source::kRuntime);
  assert(cv::Query<int32_t>("old_limit") == 30);
  cv::ResetToDefault("old_limit");
  assert(REXCVAR_GET(fps_limit) == 60);
  for (const auto& name : cv::ListFlags()) assert(name.rfind("old_", 0) != 0);

  // Callbacks registered under either name fire for writes through either name.
  int calls = 0;
  cv::RegisterChangeCallback("old_label", [&](std::string_view, std::string_view) { ++calls; });
  cv::RegisterChangeCallback("label", [&](std::string_view, std::string_view) { ++calls; });
  assert(cv::SetFlagByName("label", "b") && calls == 2);
  assert(cv::SetFlagByName("old_label", "c") && calls == 4);
  cv::UnregisterChangeCallbacks("old_label");
  assert(cv::SetFlagByName("label", "d") && calls == 4);

  // Config file: an old-name file still applies; when both are present the current name wins in either order.
  Reset();
  cv::LoadConfig(Write("cvar_alias_a.toml", "old_limit = 30\nold_postfx = false\n"));
  assert(REXCVAR_GET(fps_limit) == 30 && !REXCVAR_GET(postfx));
  Reset();
  cv::LoadConfig(Write("cvar_alias_b.toml", "fps_limit = 45\nold_limit = 30\n"));
  assert(REXCVAR_GET(fps_limit) == 45);
  Reset();
  cv::LoadConfig(Write("cvar_alias_c.toml", "old_label = \"x\"\nlabel = \"y\"\n"));
  assert(REXCVAR_GET(label) == "y");

  // SaveConfig writes the current name only.
  {
    auto path = std::filesystem::temp_directory_path() / "cvar_alias_saved.toml";
    cv::SaveConfig(path);
    std::stringstream text;
    text << std::ifstream(path).rdbuf();
    assert(text.str().find("label = \"y\"") != std::string::npos);
    assert(text.str().find("old_") == std::string::npos);
  }

  // Command line: --old=, --old, --no-old.
  Reset();
  Init({"--old_limit=20", "--old_postfx=false", "--old_label", "z"});
  assert(REXCVAR_GET(fps_limit) == 20 && !REXCVAR_GET(postfx) && REXCVAR_GET(label) == "z");
  assert(cv::GetFlagSource("fps_limit") == cv::Source::kCommandLine);
  Reset();
  Init({"--no-old_postfx"});
  assert(!REXCVAR_GET(postfx));
  Reset();
  Init({"--old_postfx"});
  assert(REXCVAR_GET(postfx));

  // Environment: REX_<OLD> applies; REX_<NEW> beats it; the command line beats both.
  Reset();
  setenv("REX_OLD_LIMIT", "25", 1);
  Init({});
  cv::ApplyEnvironment();
  assert(REXCVAR_GET(fps_limit) == 25);
  Reset();
  setenv("REX_OLD_LIMIT", "25", 1);
  setenv("REX_FPS_LIMIT", "35", 1);
  Init({});
  cv::ApplyEnvironment();
  assert(REXCVAR_GET(fps_limit) == 35);
  Reset();
  setenv("REX_OLD_LIMIT", "25", 1);
  Init({"--fps_limit=15"});
  cv::ApplyEnvironment();
  assert(REXCVAR_GET(fps_limit) == 15);

  // A cvar registered after Init (runtime module) picks up values given under its old name.
  Reset();
  Init({"--older_late=7"});
  cv::LoadConfig(Write("cvar_alias_d.toml", "older_late = 3\n"));
  {
    cv::AliasRegistrar alias("older_late", "late");
    int32_t storage = 0;
    {
      cv::FlagRegistrar late({"late", cv::FlagType::Int32, "Test", "late",
                              [&](std::string_view v) { storage = std::atoi(std::string(v).c_str()); return true; },
                              [&] { return std::to_string(storage); }, [](std::string_view) {},
                              cv::Lifecycle::kHotReload, {}, "0", false});
      assert(storage == 7);
      assert(cv::GetFlagByName("older_late") == "7");
    }
    // Unregistering the cvar drops its alias from the index too.
    assert(cv::GetFlagInfo("older_late") == nullptr && cv::GetFlagInfo("late") == nullptr);
  }
  assert(cv::CanonicalName("older_late") == "older_late");

  // An alias may not shadow a cvar or another alias.
  cv::RegisterAlias("plain", "fps_limit");
  assert(cv::GetFlagInfo("plain")->name == "plain");
  cv::RegisterAlias("old_limit", "plain");
  assert(cv::CanonicalName("old_limit") == "fps_limit");

  // A title's own default (SetDefaultValue, the manifest's [renderer] settings).
  Reset();
  Init({});
  assert(cv::SetDefaultValue("old_limit", "30"));  // through the old name
  assert(REXCVAR_GET(fps_limit) == 30 && cv::GetFlagSource("fps_limit") == cv::Source::kDefault);
  assert(!cv::HasNonDefaultValue("fps_limit") && cv::GetFlagInfo("fps_limit")->default_value == "30");
  assert(!cv::SetDefaultValue("fps_limit", "fast") && REXCVAR_GET(fps_limit) == 30);
  cv::LoadConfig(Write("cvar_alias_e.toml", "label = \"b\"\n"));
  assert(cv::SetDefaultValue("label", "c"));  // a config value still wins
  assert(REXCVAR_GET(label) == "b");
  cv::ResetToDefault("label");
  assert(REXCVAR_GET(label) == "c");
  // A cvar registered later (a GPU plugin's) takes it, and a config value still wins over it.
  cv::LoadConfig(Write("cvar_alias_f.toml", "plugin_b = 9\n"));
  assert(cv::SetDefaultValue("plugin_a", "5") && cv::SetDefaultValue("plugin_b", "5"));
  {
    int32_t a = 0, b = 0;
    auto entry = [](const char* name, int32_t& v) {
      return cv::FlagEntry{name, cv::FlagType::Int32, "Test", name,
                           [&v](std::string_view s) { v = std::atoi(std::string(s).c_str()); return true; },
                           [&v] { return std::to_string(v); }, [](std::string_view) {},
                           cv::Lifecycle::kHotReload, {}, "0", false};
    };
    cv::FlagRegistrar flag_a(entry("plugin_a", a)), flag_b(entry("plugin_b", b));
    assert(a == 5 && cv::GetFlagSource("plugin_a") == cv::Source::kDefault);
    assert(b == 9 && cv::GetFlagInfo("plugin_b")->default_value == "5");
  }

  std::puts("cvar aliases: lookups, callbacks, config, SaveConfig, command line, environment, late registration, "
            "title defaults passed");
  return 0;
}
