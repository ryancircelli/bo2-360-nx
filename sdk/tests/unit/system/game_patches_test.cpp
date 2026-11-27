// Game patch files (rex/system/game_patches.h): the xenia-canary .patch.toml format, read unchanged.
#include <catch2/catch_test_macros.hpp>

#include <rex/system/game_patches.h>

using namespace rex::patches;

namespace {
std::vector<uint8_t> B(std::initializer_list<int> v) {
  return {v.begin(), v.end()};
}
}  // namespace

TEST_CASE("file names follow canary's rule", "[patches]") {
  CHECK(IsPatchFileName("415608C3 - Call of Duty Black Ops II.patch.toml"));
  CHECK(IsPatchFileName("4d5309c9.patch.toml"));
  CHECK_FALSE(IsPatchFileName("415608C3 - x.toml"));
  CHECK_FALSE(IsPatchFileName("41560Z C3 - x.patch.toml"));
}

TEST_CASE("every canary value type is read as canary writes it", "[patches]") {
  auto f = ParseFile(R"(
title_name = "Test"
title_id = "415608C3"
hash = ["0x0123456789ABCDEF", "FEDCBA9876543210"]

[[patch]]
    name = "All types"
    desc = "d"
    author = "a"
    is_enabled = true
    [[patch.be8]]
        address = 0x82000000
        value = 0x1FF
    [[patch.be16]]
        address = 0x82000010
        value = 0x1234
    [[patch.be32]]
        address = 0x82000020
        value = 0x60000000
    [[patch.be64]]
        address = 0x82000030
        value = 0x0102030405060708
    [[patch.f32]]
        address = 0x82000040
        value = 1.5
    [[patch.f64]]
        address = 0x82000050
        value = 2
    [[patch.string]]
        address = 0x82000060
        value = "Hi"
    [[patch.u16string]]
        address = 0x82000070
        value = "Hi"
    [[patch.array]]
        address = 0x82000080
        value = "0xDEADbeef"

[[patch]]
    name = "Bad second write"
    [[patch.be32]]
        address = 0x82000000
        value = 1
    [[patch.be32]]
        address = 0x82000004
        value = "nope"
)");
  REQUIRE(f);
  CHECK(f->title_id == 0x415608C3);
  REQUIRE(f->hashes.size() == 2);
  CHECK(f->hashes[0] == 0x0123456789ABCDEFull);
  CHECK(f->hashes[1] == 0xFEDCBA9876543210ull);
  REQUIRE(f->patches.size() == 2);
  const auto& p = f->patches[0];
  CHECK(p.default_on);
  CHECK(p.author == "a");
  REQUIRE(p.writes.size() == 9);
  // Canary's type order: array, be16, be32, be64, be8, f32, f64, string, u16string.
  CHECK(p.writes[0].bytes == B({0xDE, 0xAD, 0xBE, 0xEF}));
  CHECK(p.writes[1].bytes == B({0x12, 0x34}));
  CHECK(p.writes[2].bytes == B({0x60, 0, 0, 0}));
  CHECK(p.writes[3].bytes == B({1, 2, 3, 4, 5, 6, 7, 8}));
  CHECK(p.writes[4].bytes == B({0xFF}));  // be8 keeps the low byte
  CHECK(p.writes[5].bytes == B({0x3F, 0xC0, 0, 0}));
  CHECK(p.writes[6].bytes == B({0x40, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(p.writes[7].bytes == B({'H', 'i'}));
  CHECK(p.writes[8].bytes == B({'H', 0, 'i', 0}));  // host order, as canary
  CHECK(p.writes[4].address == 0x82000000);
  // A malformed write stops that patch; the earlier writes stay and the file still loads.
  const auto& bad = f->patches[1];
  CHECK(bad.writes.size() == 1);
  CHECK_FALSE(bad.parse_error.empty());
  CHECK_FALSE(bad.default_on);
}

TEST_CASE("files missing required keys or broken TOML are skipped", "[patches]") {
  std::string err;
  CHECK_FALSE(ParseFile("title_name = \"x\"\ntitle_id = \"1\"\n", &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(ParseFile("title_name = [", &err));
  auto single = ParseFile("title_name = \"x\"\ntitle_id = \"1\"\nhash = \"00000000000000AB\"\n");
  REQUIRE(single);
  CHECK(single->hashes == std::vector<uint64_t>{0xAB});
  CHECK(single->patches.empty());
}

TEST_CASE("module hash is XXH3-64", "[patches]") {
  // Reference value of XXH3_64bits over an empty input.
  CHECK(HashCode(nullptr, 0) == 0x2D06800538D394C2ull);
}
