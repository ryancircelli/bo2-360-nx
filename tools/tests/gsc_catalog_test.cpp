// Filesystem fixtures use synthetic metadata, with no game-derived bytecode.
#include "../../games/bo2/src/bo2zm_gsc_catalog.h"
#include <cassert>
#include <chrono>
#include <iostream>

namespace fs = std::filesystem;
using namespace bo2zm::gsc;
std::vector<uint8_t> Fixture(const std::string& name = "maps/mp/gametypes_zm/_clientids.gsc") {
  std::vector<uint8_t> b(192);
  const uint8_t magic[] = {0x80, 'G', 'S', 'C', 13, 10, 0, 6};
  std::copy(std::begin(magic), std::end(magic), b.begin());
  std::copy(name.begin(), name.end(), b.begin() + 64);
  b[0x31] = 64; b[0x35] = 1;
  b[100] = 'i'; b[101] = 'n'; b[102] = 'i'; b[103] = 't';
  Write32(b.data() + 0x14, 112); Write32(b.data() + 0x2C, 32);
  Write32(b.data() + 0x1C, 144); Write32(b.data() + 148, 112); b[153] = 100;
  return b;
}
void Write(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  assert(file.good());
}
template <class F> void Reject(F fn, std::string_view reason = {}) {
  bool rejected = false;
  try { fn(); } catch (const std::exception& error) {
    rejected = true;
    assert(std::string_view(error.what()).find(reason) != std::string_view::npos);
  }
  assert(rejected);
}
void Ini(const fs::path& folder, const std::string& text = "mode=zombies\n") {
  Write(folder / "mod.ini", std::vector<uint8_t>(text.begin(), text.end()));
}
void Metadata(const fs::path& folder) {
  struct Case { std::string text; ScriptMode mode; bool files; std::string base; };
  const Case valid[] = {
    {"mode=zombies", ScriptMode::Zombies, false, ""},
    {"\n # comment\n ; comment\n[mod]\n mode = ZOMBIES \n", ScriptMode::Zombies, false, ""},
    {"\xEF\xBB\xBF[MOD]\r\nNames = FILES\r\nBASE = maps\\mp\\gametypes_zm\\\r\nMODE = Multiplayer\r\n",
     ScriptMode::Multiplayer, true, "maps/mp/gametypes_zm"},
    {"base=common_scripts/sub///\nnames=files\nmode=zombies\n", ScriptMode::Zombies, true, "common_scripts/sub"},
    {"names=INTERNAL\nmode=CAMPAIGN\n\n", ScriptMode::Campaign, false, ""},
    {"mode=zombies\nnames=files\nbase=maps/mp\n", ScriptMode::Zombies, true, "maps/mp"},
    {"mode=zombies\nnames=files\nbase=common_scripts\n", ScriptMode::Zombies, true, "common_scripts"},
    {"mode=zombies\nnames=files\nbase=/\n", ScriptMode::Zombies, true, "/"},
    {"mode=zombies\n#" + std::string(1010, 'x'), ScriptMode::Zombies, false, ""},
  };
  for (const auto& test : valid) {
    Ini(folder, test.text);
    const auto options = ReadModOptions(folder);
    assert(options.mode == test.mode && options.names_from_files == test.files && options.base == test.base);
  }
  const std::pair<std::string, std::string> invalid[] = {
    {"", "missing mode"}, {"# comment\n[mod]\nnames=files", "missing mode"},
    {"mode=zombies\nnames=files", "missing base"},
    {"mode=unknown", "invalid mode"}, {"mode=zombies\nnames=header", "invalid names"},
    {"mode=zombies\nextra=true", "unknown key"}, {"mode=zombies\nextra", "expected key=value"},
    {"mode=zombies\nMODE=multiplayer", "duplicate mode"},
    {"mode=zombies\nnames=internal\nNAMES=files", "duplicate names"},
    {"mode=zombies\nnames=files\nbase=maps/mp\nBASE=common_scripts", "duplicate base"},
    {"[mod]\nmode=zombies\n[MOD]", "duplicate [mod]"},
    {"[other]\nmode=zombies", "unknown section"},
    {"mode=zombies\nbase=maps/mp", "requires names=files"},
    {"mode=zombies\nbase=maps/mp\nnames=internal", "requires names=files"},
    {"mode=zombies # comment", "invalid mode"},
    {"mode=zombies\n#" + std::string(1011, 'x'), "exceeds 1024 bytes"},
    {std::string("mode=zombies\0", 13), "invalid mode"},
  };
  for (const auto& [text, reason] : invalid) {
    Ini(folder, text);
    Reject([&] { ReadModOptions(folder); }, reason);
  }
  for (const auto base : {"", "//", "///", "/maps/mp", "C:/maps/mp", "sdmc:/maps/mp", "maps", "maps/mpx",
                          "maps/mp/../zm", "maps/mp/./zm", "maps/mp//zm", "common_scripts//sub",
                          "common_scripts/..", "common_scripts/UPPER", "maps/mp/a b", "maps/mp/a:b"}) {
    Ini(folder, std::string("mode=zombies\nnames=files\nbase=") + base);
    Reject([&] { ReadModOptions(folder); }, "invalid base");
  }
}
int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--issues") {
    std::vector<std::string> issues;
    ListMods(argv[2], ScriptMode::Zombies, &issues);
    for (const auto& issue : issues) std::cout << issue << '\n';
    return 0;
  }
  // Python can compare both parsers against the same metadata bytes.
  if (argc == 3 && std::string_view(argv[1]) == "--options") {
    try {
      const auto options = ReadModOptions(argv[2]);
      std::cout << int(options.mode) << '\n' << (options.names_from_files ? "files" : "internal")
                << '\n' << options.base << '\n' << options.entry << '\n' << options.replace << '\n'
                << options.maps << '\n' << MapEntriesText(options.map_entries) << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cout << error.what();
      return 1;
    }
  }
  const auto tmp = fs::temp_directory_path();
  const auto root = tmp / ("bo2-gsc-catalog-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(ListMods(root).empty());
  const auto bytes = Fixture();
  Write(root, bytes);
  Reject([&] { ListMods(root); });
  assert(fs::is_regular_file(root));
  fs::remove(root);
  Write(root / "Example" / "_clientids.gsc", bytes);
  assert(ListMods(root).empty());
  assert(ListMods(root, ScriptMode::Zombies).empty());
  Reject([&] { ReadMod(root, "Example", false); });
  Ini(root / "Example");
  auto mods = ListMods(root);
  assert(mods.size() == 1 && mods[0] == "Example");
  auto objects = ReadMod(root, "Example", false);
  assert(objects.size() == 1 && objects[0].bytes == bytes &&
         objects[0].info.name == "maps/mp/gametypes_zm/_clientids.gsc" && objects[0].source == "_clientids.gsc");
  Write(root / "NestedInternal" / "unrelated/sub/script.gsc", bytes);
  Ini(root / "NestedInternal");
  auto nested = ReadMod(root, "NestedInternal", false);
  assert(nested[0].bytes == bytes && nested[0].info.name == objects[0].info.name &&
         nested[0].source == "unrelated/sub/script.gsc");
  const auto common = Fixture("common_scripts/shared.gsc");
  Write(root / "CommonInternal" / "flat.gsc", common);
  Ini(root / "CommonInternal");
  const auto shared = ReadMod(root, "CommonInternal", false);
  assert(shared[0].bytes == common && shared[0].info.name == "common_scripts/shared.gsc");
  Reject([&] { ReadMod(root, "../Example", false); });
  Reject([&] { ReadMod(root, "missing", false); });
  Write(root / "Duplicate" / "a.gsc", bytes); Write(root / "Duplicate" / "b.gsc", bytes);
  Ini(root / "Duplicate");
  Reject([&] { ReadMod(root, "Duplicate", false); });
  auto legacy = bytes; Write32(legacy.data() + 0x2C, 0);
  Write(root / "Legacy" / "a.gsc", legacy);
  Ini(root / "Legacy");
  Reject([&] { ReadMod(root, "Legacy", false); });
  objects = ReadMod(root, "Legacy", true);
  assert(objects[0].info.legacy_size && objects[0].bytes == legacy);
  Write(root / "Mixed" / "a.gsc", bytes); Write(root / "Mixed" / "bad.gsc", {1, 2, 3});
  Ini(root / "Mixed");
  Reject([&] { ReadMod(root, "Mixed", false); });
  Write(root / "Example" / "notice.txt", {1, 2, 3});
  assert(ReadMod(root, "Example", false).size() == 1);
  for (int i = 0; i < 33; ++i) Write(root / "TooMany" / (std::to_string(i) + ".gsc"), bytes);
  Ini(root / "TooMany");
  Reject([&] { ReadMod(root, "TooMany", false); });
  Write(root / "Large" / "a.gsc", std::vector<uint8_t>(1024 * 1024 + 1));
  Ini(root / "Large");
  Reject([&] { ReadMod(root, "Large", false); });
  for (const auto name : {"", ".hidden", "a..b", "bad.", "../bad", "a/b", "has space"})
    assert(!ModName(name));
  assert(ModName("Gr3Zz-v4.1"));
  Write(root / "Multiplayer" / "a.gsc", bytes);
  Ini(root / "Multiplayer", "mode=multiplayer\r\n");
  assert(ModMode(root / "Example") == ScriptMode::Zombies);
  assert(ModMode(root / "Multiplayer") == ScriptMode::Multiplayer);
  auto mp = ListMods(root, ScriptMode::Multiplayer);
  assert(mp.size() == 1 && mp[0] == "Multiplayer");
  assert(ReadMod(root, "Multiplayer", false, ScriptMode::Multiplayer).size() == 1);
  Reject([&] { ReadMod(root, "Multiplayer", false, ScriptMode::Zombies); });
  Reject([&] { ReadMod(root, "Example", false, ScriptMode::Multiplayer); });
  for (const auto& text : {"", "mode=unknown\n", "mode=multiplayer\nextra"}) {
    Ini(root / "Multiplayer", text);
    Reject([&] { ModMode(root / "Multiplayer"); });
    assert(ListMods(root, ScriptMode::Multiplayer).empty());
  }
  Ini(root / "Multiplayer", std::string(65, 'x'));
  Reject([&] { ModMode(root / "Multiplayer"); });
  Ini(root / "Multiplayer", "mode=zombies\n");
  assert(ModMode(root / "Multiplayer") == ScriptMode::Zombies);
  Write(root / "Campaign" / "a.gsc", bytes);
  Ini(root / "Campaign", "mode=campaign\n");
  assert(ModMode(root / "Campaign") == ScriptMode::Campaign);
  assert(ListMods(root, ScriptMode::Campaign).empty());
  const auto supported = ListMods(root);
  assert(std::find(supported.begin(), supported.end(), "Campaign") == supported.end());
  Reject([&] { ReadMod(root, "Campaign", false); });
  Reject([&] { ReadMod(root, "Campaign", false, ScriptMode::Campaign); });
  Ini(root / "Duplicate", "mode=multiplayer\nnames=files\nbase=maps/mp\n");
  auto aliases = ReadMod(root, "Duplicate", false, ScriptMode::Multiplayer);
  assert(aliases.size() == 2 && aliases[0].info.name == "maps/mp/a.gsc" &&
         aliases[1].info.name == "maps/mp/b.gsc" && aliases[0].bytes == bytes);
  Ini(root / "Duplicate", "mode=multiplayer\r\nnames=files\r\nbase=maps/mp\r\n");
  assert(ReadModOptions(root / "Duplicate").names_from_files);
  for (const auto& text : {"mode=zombies\nnames=internal\n",
                           "mode=zombies\r\nnames=internal\r\n"}) {
    Ini(root / "Example", text);
    assert(!ReadModOptions(root / "Example").names_from_files);
    const auto internal = ReadMod(root, "Example", false, ScriptMode::Zombies);
    assert(internal[0].info.name == "maps/mp/gametypes_zm/_clientids.gsc" && internal[0].bytes == bytes);
  }
  for (const auto& text : {"mode=multiplayer\nnames=files\nextra\n",
                           "mode=multiplayer\nnames=header\n",
                           "mode=multiplayer\nnames=internal\nnames=files\n"}) {
    Ini(root / "Duplicate", text);
    Reject([&] { ReadModOptions(root / "Duplicate"); });
  }
  Write(root / "CustomBase" / "zm.gsc", bytes);
  Ini(root / "CustomBase", "base=maps\\mp\\gametypes_zm\\\nnames=files\nmode=zombies");
  const auto custom = ReadMod(root, "CustomBase", false);
  assert(custom[0].info.name == "maps/mp/gametypes_zm/zm.gsc" && custom[0].source == "zm.gsc" &&
         custom[0].bytes == bytes);
  Write(root / "FilePaths" / "maps/mp/zm/full.gsc", bytes);
  Write(root / "FilePaths" / "common_scripts/shared.gsc", bytes);
  Ini(root / "FilePaths", "mode=zombies\nnames=files\nbase=/");
  const auto paths = ReadMod(root, "FilePaths", false);
  assert(paths.size() == 2 && paths[0].info.name == "common_scripts/shared.gsc" &&
         paths[1].info.name == "maps/mp/zm/full.gsc");
  for (const auto& object : paths) assert(object.bytes == bytes);
  Write(root / "CustomBase" / "helpers/child.gsc", bytes);
  Write(root / "CustomBase" / "helpers/readme.txt", {1});
  const auto recursive = ReadMod(root, "CustomBase", false);
  assert(recursive.size() == 2 && recursive[0].info.name == "maps/mp/gametypes_zm/helpers/child.gsc" &&
         recursive[1].info.name == "maps/mp/gametypes_zm/zm.gsc");
  Write(root / "CommonFiles" / "shared.gsc", bytes);
  Ini(root / "CommonFiles", "mode=zombies\nnames=files\nbase=common_scripts");
  assert(ReadMod(root, "CommonFiles", false)[0].info.name == "common_scripts/shared.gsc");
  Write(root / "AlwaysPrefix" / "a.gsc", bytes);
  Write(root / "AlwaysPrefix" / "maps/mp/a.gsc", bytes);
  Ini(root / "AlwaysPrefix", "mode=zombies\nnames=files\nbase=maps/mp");
  const auto prefixed = ReadMod(root, "AlwaysPrefix", false);
  assert(prefixed[0].info.name == "maps/mp/a.gsc" && prefixed[1].info.name == "maps/mp/maps/mp/a.gsc");
  Ini(root / "AlwaysPrefix", "mode=zombies\nnames=files\nbase=/");
  Reject([&] { ReadMod(root, "AlwaysPrefix", false); }, "invalid filename-based script name");
  Write(root / "InvalidPath" / "Upper.gsc", bytes);
  Ini(root / "InvalidPath", "mode=zombies\nnames=files\nbase=maps/mp");
  Reject([&] { ReadMod(root, "InvalidPath", false); }, "invalid filename-based script name");
  Ini(root / "InvalidPath", "mode=zombies\nnames=internal");
  assert(ReadMod(root, "InvalidPath", false)[0].bytes == bytes);
  Write(root / "Entry" / "custom.gsc", bytes);
  Ini(root / "Entry", "mode=zombies\nentry=maps/mp/gametypes_zm/_clientids.gsc");
  assert(ReadModOptions(root / "Entry").entry == "maps/mp/gametypes_zm/_clientids.gsc");
  assert(ReadMod(root, "Entry", false).size() == 1);
  Ini(root / "Entry", "mode=zombies\nentry=maps/mp/missing.gsc");
  Reject([&] { ReadMod(root, "Entry", false); }, "entry script is not present");
  auto parameterized = bytes;
  parameterized[144 + 10] = 1;
  Write(root / "Entry" / "custom.gsc", parameterized);
  Ini(root / "Entry", "mode=zombies\nentry=maps/mp/gametypes_zm/_clientids.gsc");
  Reject([&] { ReadMod(root, "Entry", false); }, "zero-argument main or init");
  Write(root / "Entry" / "custom.gsc", bytes);
  auto private_entry = bytes;
  private_entry[144 + 11] = 0x10;
  Write(root / "Entry" / "custom.gsc", private_entry);
  Reject([&] { ReadMod(root, "Entry", false); }, "unique public zero-argument");
  auto autoexec_entry = bytes;
  autoexec_entry[144 + 11] = 2;
  Write(root / "Entry" / "custom.gsc", autoexec_entry);
  Reject([&] { ReadMod(root, "Entry", false); }, "must not also be an autoexec");
  auto duplicate_entry = bytes;
  duplicate_entry[0x35] = 2;
  std::copy_n(bytes.begin() + 144, 12, duplicate_entry.begin() + 156);
  Write(root / "Entry" / "custom.gsc", duplicate_entry);
  Reject([&] { ReadMod(root, "Entry", false); }, "unique public zero-argument");
  auto mixed_entry = duplicate_entry;
  mixed_entry[104] = 'm'; mixed_entry[105] = 'a'; mixed_entry[106] = 'i'; mixed_entry[107] = 'n';
  mixed_entry[108] = 0;
  mixed_entry[156 + 9] = 104; mixed_entry[156 + 10] = 1;
  Write(root / "Entry" / "custom.gsc", mixed_entry);
  Reject([&] { ReadMod(root, "Entry", false); }, "unique public zero-argument");
  Write(root / "Entry" / "custom.gsc", bytes);
  Ini(root / "Entry", "mode=zombies\nentry=maps/mp/gametypes_zm/_clientids.gsc\nreplace=maps/mp/stock.gsc::init=>maps/mp/gametypes_zm/_clientids.gsc::init");
  assert(ReadMod(root, "Entry", false).size() == 1);
  Ini(root / "Entry", "mode=zombies\nreplace=maps/mp/stock.gsc::init=>maps/mp/entry.gsc::init");
  Reject([&] { ReadModOptions(root / "Entry"); }, "requires an explicit entry");
  Ini(root / "Entry", "mode=zombies\nentry=maps/mp/gametypes_zm/_clientids.gsc\nreplace=maps/mp/gametypes_zm/_clientids.gsc::init=>maps/mp/other.gsc::init");
  Reject([&] { ReadMod(root, "Entry", false); }, "source must be a stock script");
  Metadata(root / "Metadata");
  Ini(root / "Metadata", "mode=zombies\nmaps= zm_tomb , zm_buried ");
  const auto scoped = ReadModOptions(root / "Metadata");
  assert(scoped.maps == "zm_tomb,zm_buried");
  assert(AppliesToMap(scoped.maps, "zm_tomb") && AppliesToMap(scoped.maps, "zm_buried"));
  assert(!AppliesToMap(scoped.maps, "zm_transit") && !AppliesToMap(scoped.maps, ""));
  assert(AppliesToMap("", "") && AppliesToMap("", "zm_transit"));
  Ini(root / "Metadata", "mode=zombies\nentry=maps/mp/base.gsc\nmap_entries= zm_tomb : maps/mp/origins.gsc ");
  assert(MapEntriesText(ReadModOptions(root / "Metadata").map_entries) == "zm_tomb:maps/mp/origins.gsc");
  for (const auto entries : {"", "zm_tomb", "zm_tomb:maps/mp/a.gsc,", "ZM_TOMB:maps/mp/a.gsc",
                             "zm_tomb:maps/mp/a.gsc,zm_tomb:maps/mp/b.gsc", "zm_tomb:../wrong.gsc"}) {
    Ini(root / "Metadata", std::string("mode=zombies\nentry=maps/mp/base.gsc\nmap_entries=") + entries);
    Reject([&] { ReadModOptions(root / "Metadata"); });
  }
  Write(root / "MapEntries" / "base.gsc", bytes);
  Write(root / "MapEntries" / "origins.gsc", Fixture("maps/mp/origins.gsc"));
  Ini(root / "MapEntries", "mode=zombies\nentry=maps/mp/gametypes_zm/_clientids.gsc\nmap_entries=zm_tomb:maps/mp/origins.gsc");
  assert(ReadMod(root, "MapEntries", false).size() == 2);
  Ini(root / "MapEntries", "mode=zombies\nentry=maps/mp/gametypes_zm/_clientids.gsc\nmap_entries=zm_tomb:maps/mp/missing.gsc");
  Reject([&] { ReadMod(root, "MapEntries", false); }, "entry script is not present");
  for (const auto maps : {"", "zm_tomb,", "zm_tomb,,zm_buried", "zm_tomb,zm_tomb",
                          "ZM_TOMB", "maps/mp/zm_tomb", "zm-tomb", "zm tomb", "../zm_tomb"}) {
    Ini(root / "Metadata", std::string("mode=zombies\nmaps=") + maps);
    Reject([&] { ReadModOptions(root / "Metadata"); }, "invalid maps");
  }
  const auto issues_root = root / "Issues";
  for (int i = 0; i < 40; ++i) fs::create_directories(issues_root / ("Invalid" + std::to_string(i)));
  Ini(issues_root / "WrongMode", "mode=multiplayer");
  Ini(issues_root / "Campaign", "mode=campaign");
  Ini(issues_root / "Valid", "mode=zombies");
  std::vector<std::string> issues = {"stale"};
  const auto listed = ListMods(issues_root, ScriptMode::Zombies, &issues);
  assert(listed == std::vector<std::string>{"Valid"} && issues.size() == 32);
  for (const auto& issue : issues) assert(issue.starts_with("Invalid") && issue.find(": missing mod.ini") != issue.npos);
  assert(ListMods(root / "MissingRoot", {}, &issues).empty() && issues.empty());
  const auto diagnostics = root / "Diagnostics";
  Ini(diagnostics / "Desktop Mod");
  Ini(diagnostics / "WrongMode", "mode=multiplayer");
  Ini(diagnostics / "Campaign", "mode=campaign");
  Write(diagnostics / "read me.txt", {});
  std::error_code link_error;
  fs::create_directory_symlink(root / "Example", diagnostics / "Linked", link_error);
  assert(ListMods(diagnostics, ScriptMode::Zombies, &issues).empty());
  assert(issues.size() == (link_error ? 1 : 2));
  assert(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
    return issue.starts_with("Desktop Mod: portable folder name expected");
  }));
  if (!link_error) assert(std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
    return issue.starts_with("Linked: linked mod folders are unsupported");
  }));
  assert(LocalPath(root / "Example/..") == fs::canonical(root));
  Reject([&] { LocalPath(root / "missing"); });
  assert(fs::equivalent(root.parent_path(), tmp) && root.filename().string().starts_with("bo2-gsc-catalog-test-"));
  fs::remove_all(root);
  std::cout << "Script catalog fixtures passed\n";
}
