// Guest texture layout and fetch-constant checks for fixes ported from Xenia
// Canary (BSD-3-Clause).

#include <algorithm>
#include <cstdint>

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

// Canary bc6b7a020: non-base 2D array levels are aligned to four slices, and
// volume mips use their own (halved) depth.
TEST_CASE("array and volume mip layout", "[texture_util]") {
  constexpr auto k8888 = xenos::TextureFormat::k_8_8_8_8;
  // 64x64x3 array: level 1 is 32x32 = 4 KB per slice, 4 slices before level 2.
  texture_util::TextureGuestLayout array = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 2, 64, 64, 3, true, k8888, false, true, 2);
  CHECK(array.mips[1].array_slice_stride_bytes == 4096);
  CHECK(array.mip_offsets_bytes[2] == 4096 * 4);
  // A single-slice 2D texture is not padded.
  texture_util::TextureGuestLayout single = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 2, 64, 64, 1, true, k8888, false, true, 2);
  CHECK(single.mip_offsets_bytes[2] == 4096);
  // 64x64x16 volume: level 1 is 32x32x8.
  texture_util::TextureGuestLayout volume = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k3D, 2, 64, 64, 16, true, k8888, false, true, 2);
  CHECK(volume.mips[1].array_slice_stride_bytes == 4096 * 8);
  CHECK(volume.mip_offsets_bytes[2] == 4096 * 8);
  // Packed 1x1 volume mips: Z placement counts from the mip, not the tail.
  uint32_t x, y, z;
  REQUIRE(texture_util::GetPackedMipOffset(128, 128, 256, k8888, 8, x, y, z));
  CHECK(z == 4);
}

// Canary 81e3deaee: a packed base level uses power-of-two height and depth for
// its slice strides.
TEST_CASE("packed base slice strides use rounded dimensions", "[texture_util]") {
  constexpr auto k8888 = xenos::TextureFormat::k_8_8_8_8;
  // 16x65: 128 rows (not 96) of 128 bytes.
  texture_util::TextureGuestLayout tall = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 1, 16, 65, 1, true, k8888, true, true, 0);
  REQUIRE(tall.packed_level == 0);
  CHECK(tall.base.array_slice_stride_bytes == 128 * 128);
  // 16x16x9 volume: 16 slices (not 12) of 4 KB.
  texture_util::TextureGuestLayout volume = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k3D, 1, 16, 16, 9, true, k8888, true, true, 0);
  CHECK(volume.base.array_slice_stride_bytes == 4096 * 16);
  // Unpacked base keeps the exact height.
  texture_util::TextureGuestLayout unpacked = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 1, 32, 65, 1, true, k8888, false, true, 0);
  CHECK(unpacked.base.array_slice_stride_bytes == 128 * 96);
}

// Canary 19da17403: linear mip rows align to max(256 / block size, 32) blocks,
// so 96bpp rows are 32 blocks (384 bytes), not rounded up to 512 bytes.
TEST_CASE("linear mip row pitch", "[texture_util]") {
  texture_util::TextureGuestLayout rgb32f = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 2, 64, 64, 1, false,
      xenos::TextureFormat::k_32_32_32_FLOAT, false, true, 1);
  CHECK(rgb32f.mips[1].row_pitch_bytes == 32 * 12);
  texture_util::TextureGuestLayout rgba8 = texture_util::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, 2, 64, 64, 1, false, xenos::TextureFormat::k_8_8_8_8,
      false, true, 1);
  CHECK(rgba8.mips[1].row_pitch_bytes == 256);
}

namespace {
constexpr uint32_t kPitch3D = 64, kHeight3D = 64;
int64_t Tiled3D(uint32_t x, uint32_t y, uint32_t z, uint32_t bpb_log2) {
  return texture_util::GetTiledOffset3D(int32_t(x), int32_t(y), int32_t(z), kPitch3D, kHeight3D,
                                        bpb_log2);
}
}  // namespace

// Canary c9c8e483b: the lowest address in a 3D tile of an odd Z/4 group is at
// Y=8, and the Z alignment mask was missing its -1.
TEST_CASE("tiled 3D lower bound is the tile minimum", "[texture_util]") {
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    for (uint32_t front : {0u, 2u, 4u, 5u}) {
      for (uint32_t top : {0u, 40u}) {
        for (uint32_t left : {0u, 33u}) {
          uint32_t x0 = left & ~31u, y0 = top & ~31u, z0 = front & ~3u;
          int64_t tile_min = INT64_MAX;
          for (uint32_t z = z0; z < z0 + 4; ++z) {
            for (uint32_t y = y0; y < y0 + 32; ++y) {
              for (uint32_t x = x0; x < x0 + 32; ++x) {
                tile_min = std::min(tile_min, Tiled3D(x, y, z, bpb_log2));
              }
            }
          }
          INFO("bpb_log2=" << bpb_log2 << " left=" << left << " top=" << top
                           << " front=" << front);
          CHECK(int64_t(texture_util::GetTiledAddressLowerBound3D(
                    left, top, front, kPitch3D, kHeight3D, bpb_log2)) == tile_min);
        }
      }
    }
  }
}

// Canary c332733af: the 3D upper bound is the exact end of the last block, not
// a closed-form estimate that pulled in an extra page.
TEST_CASE("tiled 3D upper bound is exact", "[texture_util]") {
  for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
    for (uint32_t back : {1u, 4u, 6u}) {
      for (uint32_t bottom : {8u, 40u, 64u}) {
        for (uint32_t right : {16u, 40u, 64u, 72u}) {
          int64_t max_end = 0;
          for (uint32_t z = 0; z < back; ++z) {
            for (uint32_t y = 0; y < bottom; ++y) {
              for (uint32_t x = 0; x < right; ++x) {
                max_end = std::max(max_end, Tiled3D(x, y, z, bpb_log2) + (int64_t(1) << bpb_log2));
              }
            }
          }
          INFO("bpb_log2=" << bpb_log2 << " right=" << right << " bottom=" << bottom
                           << " back=" << back);
          CHECK(int64_t(texture_util::GetTiledAddressUpperBound3D(right, bottom, back, kPitch3D,
                                                                  kHeight3D, bpb_log2)) == max_end);
        }
      }
    }
  }
}
