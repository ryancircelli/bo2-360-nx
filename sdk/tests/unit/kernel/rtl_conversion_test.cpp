#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xevent.h>
#include <rex/system/xtypes.h>

#define EXPORT(name) extern "C" void __imp__##name(PPCContext&, uint8_t*)
EXPORT(RtlAnsiStringToUnicodeString);
EXPORT(RtlMultiByteToUnicodeSize);
EXPORT(RtlUnicodeToMultiByteSize);
EXPORT(NtWriteFileGather);
EXPORT(NtCancelIoFile);
EXPORT(XAudioEnableDucker);
EXPORT(XAudioIsDuckerEnabled);
EXPORT(XAudioGetDuckerLevel);
EXPORT(XAudioSetDuckerLevel);
EXPORT(XAudioGetDuckerThreshold);
EXPORT(XAudioSetDuckerThreshold);
EXPORT(XAudioGetDuckerAttackTime);
EXPORT(XAudioSetDuckerAttackTime);
EXPORT(XAudioGetDuckerReleaseTime);
EXPORT(XAudioSetDuckerReleaseTime);
EXPORT(XAudioGetDuckerHoldTime);
EXPORT(XAudioSetDuckerHoldTime);
EXPORT(XAudioGetSpeakerConfig);
EXPORT(XAudioSetSpeakerConfig);
EXPORT(XAudioOverrideSpeakerConfig);
EXPORT(_scprintf);
EXPORT(_scwprintf);
EXPORT(_vscprintf);
#undef EXPORT

namespace {
using namespace rex;
using namespace rex::system;

struct Guest {
  Runtime runtime{std::filesystem::current_path()};
  uint32_t address = 0;
  Guest() {
    RuntimeConfig config{};
    config.tool_mode = true;
    REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
    address = runtime.memory()->SystemHeapAlloc(0x5000);
    REQUIRE(address != 0);
  }
  ~Guest() { runtime.memory()->SystemHeapFree(address); }
  template <typename T> T* at(uint32_t offset) {
    return runtime.memory()->TranslateVirtual<T*>(address + offset);
  }
  uint32_t call(void (*fn)(PPCContext&, uint8_t*), PPCContext& ctx) {
    fn(ctx, runtime.virtual_membase());
    return ctx.r3.u32;
  }
};

class MemoryFile final : public rex::filesystem::File {
 public:
  MemoryFile() : File(0, nullptr) {}
  std::vector<uint8_t> bytes;
  size_t max_write = SIZE_MAX;
  X_STATUS write_status = X_STATUS_SUCCESS;
  int writes = 0;
  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t>, size_t, size_t*) override {
    return X_STATUS_NOT_IMPLEMENTED;
  }
  X_STATUS WriteSync(std::span<const uint8_t> data, size_t offset, size_t* written) override {
    ++writes;
    *written = XFAILED(write_status) ? 0 : std::min(data.size(), max_write);
    bytes.resize(std::max(bytes.size(), offset + *written));
    std::copy_n(data.begin(), *written, bytes.begin() + offset);
    return write_status;
  }
};
}

TEST_CASE("RTL ANSI conversion uses byte lengths, preserves embedded nulls and writes BE UTF16",
          "[kernel][rtl]") {
  Guest g;
  auto* src = g.at<X_ANSI_STRING>(0);
  auto* dst = g.at<X_UNICODE_STRING>(16);
  src->pointer = g.address + 64;
  src->length = 3;
  std::memcpy(g.at<char>(64), "A\0\xE9", 3);
  dst->pointer = g.address + 128;
  dst->maximum_length = 8;
  PPCContext ctx{};
  ctx.r3.u64 = g.address + 16;
  ctx.r4.u64 = g.address;
  ctx.r5.u64 = 0;
  CHECK(g.call(__imp__RtlAnsiStringToUnicodeString, ctx) == X_STATUS_SUCCESS);
  CHECK(uint16_t(dst->length) == 6);
  const uint8_t expected[] = {0, 'A', 0, 0, 0, 0xE9, 0, 0};
  CHECK(std::memcmp(g.at<uint8_t>(128), expected, sizeof(expected)) == 0);

  // No room for a terminator: report required byte length without overwriting data.
  dst->maximum_length = 6;
  std::memset(g.at<uint8_t>(128), 0xCC, 8);
  ctx.r3.u64 = g.address + 16;
  CHECK(g.call(__imp__RtlAnsiStringToUnicodeString, ctx) == X_STATUS_BUFFER_OVERFLOW);
  CHECK(g.at<uint8_t>(128)[0] == 0xCC);
  CHECK(uint16_t(dst->length) == 6);

  ctx.r3.u64 = g.address + 16;
  ctx.r5.u64 = 1;
  REQUIRE(g.call(__imp__RtlAnsiStringToUnicodeString, ctx) == X_STATUS_SUCCESS);
  CHECK(uint16_t(dst->maximum_length) == 8);
  CHECK(std::memcmp(g.runtime.memory()->TranslateVirtual(dst->pointer), expected, 8) == 0);
  g.runtime.memory()->SystemHeapFree(dst->pointer);

  src->length = 0x8000;
  ctx.r3.u64 = g.address + 16;
  CHECK(g.call(__imp__RtlAnsiStringToUnicodeString, ctx) == X_STATUS_INVALID_PARAMETER_2);
}

TEST_CASE("RTL conversion sizes match the existing single-byte converters", "[kernel][rtl]") {
  Guest g;
  PPCContext ctx{};
  ctx.r3.u64 = g.address;
  ctx.r4.u64 = g.address + 64;
  ctx.r5.u64 = 7;
  CHECK(g.call(__imp__RtlMultiByteToUnicodeSize, ctx) == X_STATUS_SUCCESS);
  CHECK(uint32_t(*g.at<be<uint32_t>>(0)) == 14);
  ctx.r3.u64 = g.address;
  CHECK(g.call(__imp__RtlUnicodeToMultiByteSize, ctx) == X_STATUS_SUCCESS);
  CHECK(uint32_t(*g.at<be<uint32_t>>(0)) == 3);
  ctx.r3.u64 = 0;
  CHECK(g.call(__imp__RtlUnicodeToMultiByteSize, ctx) == X_STATUS_SUCCESS);
}

