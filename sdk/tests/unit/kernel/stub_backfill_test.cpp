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

TEST_CASE("Ducker settings use f1 and round-trip independently", "[kernel][audio]") {
  using Fn = void (*)(PPCContext&, uint8_t*);
  const Fn setters[] = {__imp__XAudioSetDuckerLevel, __imp__XAudioSetDuckerThreshold,
    __imp__XAudioSetDuckerAttackTime, __imp__XAudioSetDuckerReleaseTime, __imp__XAudioSetDuckerHoldTime};
  const Fn getters[] = {__imp__XAudioGetDuckerLevel, __imp__XAudioGetDuckerThreshold,
    __imp__XAudioGetDuckerAttackTime, __imp__XAudioGetDuckerReleaseTime, __imp__XAudioGetDuckerHoldTime};
  PPCContext ctx{};
  for (size_t i = 0; i < 5; ++i) {
    ctx.f1.f64 = double(i + 1) / 8;
    setters[i](ctx, nullptr);
    CHECK(ctx.r3.u32 == 0);
    ctx.f1.f64 = -99;
    getters[i](ctx, nullptr);
    CHECK(ctx.f1.f64 == double(i + 1) / 8);
  }
  ctx.r3.u64 = 9;
  __imp__XAudioEnableDucker(ctx, nullptr);
  __imp__XAudioIsDuckerEnabled(ctx, nullptr);
  CHECK(ctx.r3.u32 == 1);
  ctx.r3.u64 = 0;
  __imp__XAudioEnableDucker(ctx, nullptr);
  __imp__XAudioIsDuckerEnabled(ctx, nullptr);
  CHECK(ctx.r3.u32 == 0);
}

TEST_CASE("Speaker configuration setters feed the getter", "[kernel][audio]") {
  Guest g;
  PPCContext ctx{};
  ctx.r3.u64 = 0x00010001;
  g.call(__imp__XAudioSetSpeakerConfig, ctx);
  ctx.r3.u64 = g.address;
  CHECK(g.call(__imp__XAudioGetSpeakerConfig, ctx) == 0);
  CHECK(uint32_t(*g.at<be<uint32_t>>(0)) == 0x00010001);
  ctx.r3.u64 = 0x00000004;
  g.call(__imp__XAudioOverrideSpeakerConfig, ctx);
  ctx.r3.u64 = g.address;
  g.call(__imp__XAudioGetSpeakerConfig, ctx);
  CHECK(uint32_t(*g.at<be<uint32_t>>(0)) == 4);
}

TEST_CASE("CRT count-only formatters count formatted output", "[kernel][strings]") {
  Guest g;
  std::strcpy(g.at<char>(0), "%s:%04d");
  std::strcpy(g.at<char>(64), "save");
  PPCContext ctx{};
  ctx.r3.u64 = g.address;
  ctx.r4.u64 = g.address + 64;
  ctx.r5.u64 = 7;
  CHECK(g.call(__imp___scprintf, ctx) == 9);
  auto* args = g.at<be<uint64_t>>(128);
  args[0] = g.address + 64;
  args[1] = 7;
  ctx.r3.u64 = g.address;
  ctx.r4.u64 = g.address + 128;
  CHECK(g.call(__imp___vscprintf, ctx) == 9);
  auto* wide = g.at<be<uint16_t>>(256);
  const char16_t format[] = u"%04d";
  for (size_t i = 0; i < std::size(format); ++i) wide[i] = format[i];
  ctx.r3.u64 = g.address + 256;
  ctx.r4.u64 = 7;
  CHECK(g.call(__imp___scwprintf, ctx) == 4);
}
