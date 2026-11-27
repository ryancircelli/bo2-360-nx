// Authored linker metadata only; no executable or game-derived script bytes.
#include "../../games/bo2/src/bo2zm_gsc_bundle.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;
using namespace bo2zm::gsc;
using Bytes = std::vector<uint8_t>;
using Record = std::pair<std::string, Bytes>;

Bytes Fixture(const std::string& name, std::string_view function = "main") {
  Bytes bytes(512);
  constexpr uint8_t magic[] = {0x80, 'G', 'S', 'C', 13, 10, 0, 6};
  std::copy(std::begin(magic), std::end(magic), bytes.begin());
  std::copy(name.begin(), name.end(), bytes.begin() + 64);
  bytes[0x31] = 64;
  bytes[0x35] = 1;
  Write32(bytes.data() + 0x14, 256);
  Write32(bytes.data() + 0x2c, 32);
  Write32(bytes.data() + 0x1c, 288);
  Write32(bytes.data() + 292, 256);
  bytes[296] = 1; bytes[297] = 144;  // Export string at 400.
  std::copy(function.begin(), function.end(), bytes.begin() + 400);
  return bytes;
}
Bytes Pack(std::string_view metadata, const std::vector<Record>& records) {
  Bytes bytes{'B', '2', 'B', 'O', 'T', '0', '0', '1'};
  bytes.resize(16);
  Write32(bytes.data() + 8, uint32_t(metadata.size()));
  Write32(bytes.data() + 12, uint32_t(records.size()));
  bytes.insert(bytes.end(), metadata.begin(), metadata.end());
  for (const auto& [name, object] : records) {
    const auto at = bytes.size();
    bytes.resize(at + 6);
    bytes[at] = uint8_t(name.size() >> 8); bytes[at + 1] = uint8_t(name.size());
    Write32(bytes.data() + at + 2, uint32_t(object.size()));
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.insert(bytes.end(), object.begin(), object.end());
  }
  return bytes;
}
void Write(const fs::path& file, const Bytes& bytes) {
  std::ofstream output(file, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  assert(output.good());
}
template <class F> void Reject(F&& function, std::string_view reason = {}) {
  try { function(); }
  catch (const std::exception& error) {
    if (std::string_view(error.what()).find(reason) != std::string_view::npos) return;
    std::cerr << "Expected " << reason << "; got " << error.what() << '\n';
    std::abort();
  }
  std::cerr << "Expected rejection: " << reason << '\n';
  std::abort();
}
void Compare(const fs::path& pack, const fs::path& folder) {
  const auto bundle = ReadBotBundle(pack);
  const auto objects = ReadMod(folder.parent_path(), folder.filename().string(), false);
  assert(bundle.objects.size() == objects.size());
  for (const auto& object : objects) {
    const auto found = std::find_if(bundle.objects.begin(), bundle.objects.end(), [&](const auto& candidate) {
      return candidate.info.name == object.info.name;
    });
    assert(found != bundle.objects.end() && found->bytes == object.bytes && found->source == object.info.name);
    assert(found->info.code_size == object.info.code_size && !found->info.legacy_size);
  }
  const auto expected = ReadModOptions(folder);
  const auto& actual = bundle.options;
  assert(actual.mode == expected.mode && actual.names_from_files == expected.names_from_files &&
         actual.base == expected.base && actual.entry == expected.entry && actual.replace == expected.replace &&
         actual.maps == expected.maps && actual.map_entries == expected.map_entries);
}
int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--compare") {
    try { Compare(argv[2], argv[3]); }
    catch (const std::exception& error) { std::cerr << error.what(); return 1; }
    std::cout << "Bundle matches folder objects and options\n";
    return 0;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--read") {
    try { std::cout << ReadBotBundle(argv[2]).objects.size() << '\n'; return 0; }
    catch (const std::exception& error) { std::cerr << error.what(); return 1; }
  }
  const auto root = fs::temp_directory_path() / ("bo2-gsc-bundle-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(root);
  const auto file = root / "bo2zm_bots.gscpack";
  const std::string name = "maps/mp/authored.gsc", second = "common_scripts/helper.gsc";
  const std::string ini = "mode=zombies\nnames=internal\nentry=" + name + '\n';
  const auto object = Fixture(name);
  const auto valid = Pack(ini, {{name, object}});
  const auto read = [&](const Bytes& bytes) { Write(file, bytes); return ReadBotBundle(file); };
  const auto invalid = [&](const Bytes& bytes, std::string_view reason = {}) {
    Reject([&] { read(bytes); }, reason);
  };
  const auto bundle = read(valid);
  assert(bundle.objects.size() == 1 && bundle.objects[0].bytes == object &&
         bundle.objects[0].source == name && bundle.objects[0].info.name == name &&
         bundle.options.entry == name && bundle.options.mode == ScriptMode::Zombies);
  // Every cut exercises real reads, including the fixed headers and object tail.
  for (size_t size = 0; size < valid.size(); ++size)
    invalid(Bytes(valid.begin(), valid.begin() + size));
  auto bad = valid; bad[0] = 'X'; invalid(bad, "magic");
  for (const auto length : {0u, 1025u, 0xffffffffu}) {
    bad = valid; Write32(bad.data() + 8, length); invalid(bad, "metadata length");
  }
  for (const auto count : {0u, 33u, 0xffffffffu}) {
    bad = valid; Write32(bad.data() + 12, count); invalid(bad, "file count");
  }
  const auto record = 16 + ini.size();
  for (const auto length : {0u, 128u, 65535u}) {
    bad = valid; bad[record] = uint8_t(length >> 8); bad[record + 1] = uint8_t(length);
    invalid(bad, "record lengths");
  }
  for (const auto length : {0u, 63u, 1024u * 1024u + 1u, 0xffffffffu}) {
    bad = valid; Write32(bad.data() + record + 2, length); invalid(bad, "record lengths");
  }
  bad = valid; bad.push_back(0); invalid(bad, "trailing bytes");
  invalid(Bytes(4 * 1024 * 1024 + 8193), "size is outside limits");
  invalid(Pack(ini, {{name, object}, {name, object}}), "duplicate");
  invalid(Pack(ini, {{name, object}, {second, object}}), "embedded internal name");
  for (const auto path : {"a.gsc", "maps/mp/../a.gsc", "maps/mp//a.gsc", "maps/mp/A.gsc", "maps\\mp\\a.gsc",
                          "sdmc:/maps/mp/a.gsc", "maps/mp/\x80.gsc"})
    invalid(Pack(ini, {{path, object}}), "script path");
  for (const auto text : {"", "# comment", "mode=zombies\nMODE=zombies", "[other]\nmode=zombies",
                          "mode=zombies\nunknown=1", "mode=zombies\nentry=../bad.gsc",
                          "mode=zombies\nentry=maps/mp/a.gsc\nmap_entries=zm_tomb:maps/mp/b.gsc,zm_tomb:maps/mp/c.gsc"})
    invalid(Pack(text, {{name, object}}));
  invalid(Pack(std::string("mode=zombies\0", 13), {{name, object}}), "invalid mode");
  for (const auto mode : {"multiplayer", "campaign"})
    invalid(Pack(std::string("mode=") + mode + "\nentry=" + name, {{name, object}}), "requires Zombies");
  invalid(Pack(ini + "names=files\nbase=/", {{name, object}}), "duplicate names");
  invalid(Pack("mode=zombies\nnames=files\nbase=/\nentry=" + name, {{name, object}}), "internal names");
  invalid(Pack("mode=zombies", {{name, object}}), "default entry");
  invalid(Pack(ini + "entry=maps/mp/missing.gsc", {{name, object}}), "duplicate entry");
  invalid(Pack("mode=zombies\nentry=maps/mp/missing.gsc", {{name, object}}), "entry script is not present");
  invalid(Pack(ini + "map_entries=zm_tomb:" + second, {{name, object}}), "entry script is not present");
  assert(read(Pack(ini + "map_entries=zm_tomb:" + second, {{name, object}, {second, Fixture(second)}})).objects.size() == 2);
  for (const auto offset : {298, 299}) {
    bad = object; bad[offset] = offset == 298 ? 1 : 0x10;
    invalid(Pack(ini, {{name, bad}}), "unique public zero-argument");
  }
  bad = object; bad[299] = 2; invalid(Pack(ini, {{name, bad}}), "autoexec");
  invalid(Pack(ini, {{name, Fixture(name, "helper")}}), "main or init export");
  bad = object; bad[0x35] = 2; std::copy_n(object.begin() + 288, 12, bad.begin() + 300);
  invalid(Pack(ini, {{name, bad}}), "unique public zero-argument");
  const auto redirect = ini + "replace=maps/mp/stock.gsc::track=>" + second + "::track";
  assert(read(Pack(redirect, {{name, object}, {second, Fixture(second, "track")}})).objects.size() == 2);
  invalid(Pack(redirect, {{name, object}}), "replacement target script");
  invalid(Pack(redirect, {{name, object}, {second, Fixture(second)}}), "replacement export not found");
  invalid(Pack(ini + "replace=" + name + "::main=>" + second + "::main",
               {{name, object}, {second, Fixture(second)}}), "source must be a stock script");
  bad = object; Write32(bad.data() + 0x2c, 0); invalid(Pack(ini, {{name, bad}}), "code interval");
  bad = object; bad[0x3d] = 1; invalid(Pack(ini, {{name, bad}}), "unsupported");
  bad = object; Write32(bad.data() + 0x1c, 0xfffffff0); invalid(Pack(ini, {{name, bad}}), "export table");
  bad = object; Write32(bad.data() + 292, 288); invalid(Pack(ini, {{name, bad}}), "invalid export");
  // A PC object can share the magic but stores header fields little-endian.
  bad = object;
  for (const auto at : {0x14, 0x1c, 0x2c}) std::reverse(bad.begin() + at, bad.begin() + at + 4);
  for (const auto at : {0x30, 0x34}) std::reverse(bad.begin() + at, bad.begin() + at + 2);
  invalid(Pack(ini, {{name, bad}}), "code interval");
  const std::string longest = "common_scripts/" + std::string(108, 'a') + ".gsc";
  assert(read(Pack("mode=zombies\nentry=" + longest, {{longest, Fixture(longest)}})).objects[0].source.size() == 127);
  std::string padded = "\xEF\xBB\xBF" + ini + '#'; padded.resize(1024, 'x');
  assert(read(Pack(padded, {{name, object}})).options.entry == name);
  padded.push_back('x'); invalid(Pack(padded, {{name, object}}), "metadata length");
  Reject([&] { ParseModOptions(padded); }, "exceeds 1024 bytes");
  std::vector<Record> many;
  for (int i = 0; i < 32; ++i) {
    const auto path = "maps/mp/fixture" + std::to_string(i) + ".gsc";
    many.emplace_back(path, Fixture(path));
  }
  assert(read(Pack("mode=zombies\nentry=maps/mp/fixture0.gsc", many)).objects.size() == 32);
  std::vector<Record> large;
  for (int i = 0; i < 4; ++i) {
    const auto path = i ? "maps/mp/large" + std::to_string(i) + ".gsc" : name;
    auto bytes = Fixture(path); bytes.resize(1024 * 1024);
    large.emplace_back(path, std::move(bytes));
  }
  assert(read(Pack(ini, large)).objects.size() == 4);
  large.emplace_back(second, Fixture(second)); invalid(Pack(ini, large), "payload size");
  Write(file, valid);
  Reject([&] { ReadBotBundle(root); }, "regular file");
  Reject([&] { ReadBotBundle(root / "missing"); });
  std::error_code link_error;
  fs::create_symlink(file, root / "linked.gscpack", link_error);
  if (!link_error) Reject([&] { ReadBotBundle(root / "linked.gscpack"); }, "linked");
  link_error.clear();
  fs::create_directory_symlink(root, root / "linked-folder", link_error);
  if (!link_error) {
    Reject([&] { ReadBotBundle(root / "linked-folder" / file.filename()); }, "linked");
    fs::remove(root / "linked-folder");
  }
  assert(fs::equivalent(root.parent_path(), fs::temp_directory_path()) &&
         root.filename().string().starts_with("bo2-gsc-bundle-test-"));
  fs::remove_all(root);
  std::cout << "Bot bundle semantic and bounds fixtures passed\n";
}
