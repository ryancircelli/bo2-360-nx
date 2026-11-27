#include <catch2/catch_test_macros.hpp>
#include <rex/types.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xam {
u32 XamAvatarGetAssetsResultSize_entry(u32, mapped_u32, mapped_u32);
u32 XamAvatarGetInstrumentation_entry(u64, mapped_u32);
u32 XamInputNonControllerGetRawEx_entry(u32, mapped_u32, mapped_u32, mapped_u16);
u32 XamInputNonControllerSetRawEx_entry(u32, mapped_u32, u32);
u32 XamInputNonControllerGetRaw_entry(mapped_u16, mapped_u32, mapped_u32);
u32 XamInputNonControllerSetRaw_entry(u32, mapped_u32);
}
using namespace rex;
using namespace rex::kernel::xam;

TEST_CASE("Disabled avatar subsystem reports empty resource sizes", "[kernel][avatar]") {
  be_u32 result_size = 0xAAAAAAAA, gpu_size = 0xBBBBBBBB;
  auto result = mapped_u32::from_host(&result_size), gpu = mapped_u32::from_host(&gpu_size);
  CHECK(XamAvatarGetAssetsResultSize_entry(~u32{0}, result, gpu) == X_ERROR_SUCCESS);
  CHECK(result_size == 0);
  CHECK(gpu_size == 0);
  result_size = 123;
  CHECK(XamAvatarGetAssetsResultSize_entry(0, result, nullptr) == X_E_INVALIDARG);
  CHECK(result_size == 123);
  CHECK(XamAvatarGetAssetsResultSize_entry(0, nullptr, gpu) == X_E_INVALIDARG);
  CHECK(XamAvatarGetInstrumentation_entry(1, result) == 1);
  CHECK(result_size == 0);
  CHECK(XamAvatarGetInstrumentation_entry(1, nullptr) == 1);
}
TEST_CASE("Raw portal I/O validates requests and leaves failed outputs alone", "[kernel][input]") {
  be_u32 buffer = 0xAABBCCDD, length = 32;
  be_u16 state = 0xCAFE;
  auto data = mapped_u32::from_host(&buffer), count = mapped_u32::from_host(&length);
  auto status = mapped_u16::from_host(&state);
  for (u32 device : {5u, 6u}) {
    CHECK(XamInputNonControllerGetRawEx_entry(device, data, count, status) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(XamInputNonControllerSetRawEx_entry(device, data, 32) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
  CHECK(XamInputNonControllerGetRaw_entry(status, count, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(XamInputNonControllerSetRaw_entry(32, data) == X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(buffer == 0xAABBCCDD);
  CHECK(length == 32);
  CHECK(state == 0xCAFE);
  CHECK(XamInputNonControllerGetRawEx_entry(4, data, count, status) == X_ERROR_INVALID_PARAMETER);
  CHECK(XamInputNonControllerGetRawEx_entry(5, nullptr, count, status) == X_ERROR_INVALID_PARAMETER);
  CHECK(XamInputNonControllerGetRawEx_entry(5, data, nullptr, status) == X_ERROR_INVALID_PARAMETER);
  CHECK(XamInputNonControllerGetRawEx_entry(5, data, count, nullptr) == X_ERROR_INVALID_PARAMETER);
  for (u32 size : {0u, 33u}) {
    length = size;
    CHECK(XamInputNonControllerGetRawEx_entry(5, data, count, status) == X_ERROR_INVALID_PARAMETER);
    CHECK(XamInputNonControllerSetRawEx_entry(5, data, size) == X_ERROR_INVALID_PARAMETER);
  }
  CHECK(XamInputNonControllerSetRawEx_entry(4, data, 1) == X_ERROR_INVALID_PARAMETER);
  CHECK(XamInputNonControllerSetRawEx_entry(5, nullptr, 1) == X_ERROR_INVALID_PARAMETER);
}
