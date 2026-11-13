// Standalone linker-metadata checks; fixtures contain no game bytecode.
#include "../../games/bo2/src/bo2zm_gsc_object.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace bo2zm::gsc;
std::vector<uint8_t> Fixture(const std::string& name = "maps/mp/gametypes_zm/_clientids.gsc") {
  std::vector<uint8_t> b(192);
  const uint8_t magic[] = {0x80, 'G', 'S', 'C', 13, 10, 0, 6};
  std::copy(std::begin(magic), std::end(magic), b.begin());
  std::copy(name.begin(), name.end(), b.begin() + 64);
  b[0x31] = 64;
  b[0x35] = 1;
  b[100] = 'i'; b[101] = 'n'; b[102] = 'i'; b[103] = 't';
  Write32(b.data() + 0x14, 112); Write32(b.data() + 0x2C, 32);
  Write32(b.data() + 0x1C, 144);
  Write32(b.data() + 148, 112); b[153] = 100;
  return b;
}
int main(int argc, char** argv) {
  ObjectInfo info; std::string error;
  auto valid = Fixture();
  assert(Validate(valid, false, info, error));
  for (const auto name : {"maps/mp/a.gsc", "maps/mp/gametypes_zm/sub/a.gsc", "common_scripts/a.gsc",
                          "common_scripts/sub/shared.gsc"}) assert(ScriptName(name));
  for (const auto name : {"maps/mp", "common_scripts", "maps/mp.gsc", "common_scripts.gsc",
                          "maps/zm/a.gsc", "maps/mpx/a.gsc", "common_scripts_extra/a.gsc",
                          "/maps/mp/a.gsc", "C:/maps/mp/a.gsc", "maps\\mp\\a.gsc", "maps/mp/../a.gsc",
                          "maps/mp/./a.gsc", "common_scripts//a.gsc", "maps/mp/a.gsc/",
                          "common_scripts/A.gsc", "maps/mp/a b.gsc", "maps/mp/a.GSC"}) assert(!ScriptName(name));
  const std::string longest = "common_scripts/" + std::string(108, 'a') + ".gsc";
  assert(longest.size() == 127 && ScriptName(longest) && !ScriptName("common_scripts/a" + longest.substr(15)));
  auto common = Fixture("common_scripts/shared.gsc");
  assert(Validate(common, false, info, error) && info.name == "common_scripts/shared.gsc");
  for (size_t size = 0; size < 64; ++size)
    assert(!Validate(std::span(valid).first(size), false, info, error));
  auto b = valid; b[7] = 5; assert(!Validate(b, false, info, error));
  b = valid; Write32(b.data() + 0x14, 0xFFFFFFF0); assert(!Validate(b, false, info, error));
  b = valid; Write32(b.data() + 148, 144); assert(!Validate(b, false, info, error));
  b = valid; b[64] = '/'; assert(!Validate(b, false, info, error));
  b = valid; b[0x3D] = 1; assert(!Validate(b, false, info, error));
  b = valid; Write32(b.data() + 0x2C, 0);
  assert(!Validate(b, false, info, error));
  assert(Validate(b, true, info, error) && info.legacy_size && info.code_size == 32);
  b = valid; b[0x33] = 1; Write32(b.data() + 0x18, 160);
  b[161] = 100; b[162] = 1; Write32(b.data() + 164, 143);
  assert(!Validate(b, false, info, error));
  Write32(b.data() + 164, 140); assert(Validate(b, false, info, error));
  b = valid; b[0x37] = 1; Write32(b.data() + 0x20, 160);
  b[161] = 100; b[163] = 62; b[165] = 1; b[167] = 2; Write32(b.data() + 168, 140);
  assert(!Validate(b, false, info, error));
  Write32(b.data() + 168, 112); assert(Validate(b, false, info, error));
  b[167] = 0; assert(!Validate(b, false, info, error));
  b[167] = 6; assert(!Validate(b, false, info, error));
  b[167] = 0x82; assert(!Validate(b, false, info, error));
  b[167] = 1; Write32(b.data() + 168, 139);
  assert(Validate(b, false, info, error));
  b[167] = 2; assert(!Validate(b, false, info, error));
  b[167] = 2; Write32(b.data() + 168, 112);
  b[163] = 0; assert(!Validate(b, false, info, error));
  b = valid; b[0x33] = 1; Write32(b.data() + 0x18, 160);
  b[161] = 100; b[163] = 2; assert(!Validate(b, false, info, error));
  b = valid; b[0x3C] = 1; Write32(b.data() + 0x0C, 144); Write32(b.data() + 144, 100);
  assert(!Validate(b, false, info, error));
  b = valid; b[144] = 'x'; b[145] = 0; b[153] = 144;
  assert(!Validate(b, false, info, error));
  b = valid; b[0x33] = 1; Write32(b.data() + 0x18, 160); b[163] = 1;
  assert(!Validate(b, false, info, error));
  assert(Validate(b, true, info, error) && info.legacy_header_strings == 1);
  b[163] = 0; assert(Validate(b, true, info, error) && info.legacy_header_strings == 1); b[163] = 1;
  b[161] = 1; assert(!Validate(b, true, info, error));
  b = valid; b.resize(20000); b[0x33] = 1; Write32(b.data() + 0x18, 160);
  b[161] = 192; std::fill(b.begin() + 192, b.begin() + 1492, 'x');
  assert(Validate(b, false, info, error));
  std::fill(b.begin() + 192, b.begin() + 192 + 16385, 'x');
  assert(!Validate(b, false, info, error));
  std::cout << "GSC object structural checks passed\n";
  for (int i = 1; i < argc; ++i) {
    std::ifstream file(argv[i], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (!Validate(bytes, true, info, error)) {
      std::cerr << argv[i] << ": " << error << '\n'; return 1;
    }
    std::cout << info.name << ": " << bytes.size() << " bytes, code=" << info.code_size
              << ", legacy=" << info.legacy_size << '\n';
  }
}
