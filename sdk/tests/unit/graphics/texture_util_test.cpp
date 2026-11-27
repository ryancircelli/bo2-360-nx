// Guest texture layout and fetch-constant checks for fixes ported from Xenia
// Canary (BSD-3-Clause).

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/util.h>

using namespace rex::graphics;

namespace {

xenos::xe_gpu_texture_fetch_t Fetch2D(uint32_t width, uint32_t height, uint32_t base_page,
                                      uint32_t mip_page, uint32_t mip_min, uint32_t mip_max,
                                      bool packed) {
  xenos::xe_gpu_texture_fetch_t fetch = {};
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.tiled = 1;
  fetch.pitch = (width + 31) >> 5;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.base_address = base_page;
  fetch.mip_address = mip_page;
  fetch.mip_min_level = mip_min;
  fetch.mip_max_level = mip_max;
  fetch.packed_mips = packed;
  return fetch;
}

void Pages(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t& base_page, uint32_t& mip_page,
           uint32_t& mip_min, uint32_t& mip_max) {
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
}

}  // namespace

// Canary ace153cb8: a packed level 0 puts the whole tail at base_address.
TEST_CASE("packed level 0 tail uses the base address", "[texture_util]") {
  uint32_t base_page, mip_page, mip_min, mip_max;
  Pages(Fetch2D(16, 16, 0x100, 0x200, 0, 4, true), base_page, mip_page, mip_min, mip_max);
  CHECK(base_page == 0x100);
  CHECK(mip_page == 0x100);
  CHECK(mip_min == 0);
  CHECK(mip_max == 4);
  // Not packed at level 0 (64x64 packs from level 2): separate mip page kept.
  Pages(Fetch2D(64, 64, 0x100, 0x200, 0, 6, true), base_page, mip_page, mip_min, mip_max);
  CHECK(mip_page == 0x200);
}

// Canary 4a863a0e1: a nonzero mip_min_level is sampler state and keeps a
// separate base page; only a base sharing the mip page is dropped.
TEST_CASE("mip_min_level keeps a separate base page", "[texture_util]") {
  uint32_t base_page, mip_page, mip_min, mip_max;
  Pages(Fetch2D(256, 256, 0x100, 0x200, 2, 8, false), base_page, mip_page, mip_min, mip_max);
  CHECK(base_page == 0x100);
  CHECK(mip_page == 0x200);
  CHECK(mip_min == 2);
  Pages(Fetch2D(256, 256, 0x200, 0x200, 2, 8, false), base_page, mip_page, mip_min, mip_max);
  CHECK(base_page == 0);
  CHECK(mip_min == 2);
  Pages(Fetch2D(256, 256, 0, 0x200, 0, 8, false), base_page, mip_page, mip_min, mip_max);
  CHECK(base_page == 0);
  CHECK(mip_min == 1);
}

// Canary 344fca758: a 2x1 tail is laid out along X like a square texture.
TEST_CASE("2x1 packed mips are laid out along X", "[texture_util]") {
  uint32_t x, y, z;
  REQUIRE(texture_util::GetPackedMipOffset(2, 1, 1, xenos::TextureFormat::k_8_8_8_8, 1, x, y, z));
  CHECK(x == 8);
  CHECK(y == 0);
  // A 4x1 texture is still wide, so it goes down along Y.
  REQUIRE(texture_util::GetPackedMipOffset(4, 1, 1, xenos::TextureFormat::k_8_8_8_8, 1, x, y, z));
  CHECK(x == 0);
  CHECK(y == 8);
}
