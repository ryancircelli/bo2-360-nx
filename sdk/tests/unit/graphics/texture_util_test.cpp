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
