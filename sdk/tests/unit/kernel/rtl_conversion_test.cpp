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
EXPORT(RtlAppendUnicodeToString);
EXPORT(RtlAppendUnicodeStringToString);
EXPORT(RtlCompareUnicodeString);
EXPORT(RtlCompareUnicodeStringN);
EXPORT(RtlCreateUnicodeString);
EXPORT(RtlUnicodeToUtf8);
EXPORT(RtlUnicodeToUtf8Size);
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


namespace {
void PutW(uint8_t* p, const char16_t* s) {  // big-endian UTF-16 with terminator
  do { p[0] = uint8_t(*s >> 8); p[1] = uint8_t(*s & 0xFF); p += 2; } while (*s++);
}
}

TEST_CASE("RTL unicode string append, compare, create and UTF-8 conversion", "[kernel][rtl]") {
  Guest g;
  auto* a = g.at<X_UNICODE_STRING>(0);
  auto* b = g.at<X_UNICODE_STRING>(8);
  PutW(g.at<uint8_t>(0x100), u"abc");
  PutW(g.at<uint8_t>(0x200), u"ABD");
  a->pointer = g.address + 0x100; a->length = 6; a->maximum_length = 8;
  b->pointer = g.address + 0x200; b->length = 6; b->maximum_length = 8;
  PPCContext ctx{};
  // compare: case-sensitive 'a'(0x61) - 'A'(0x41) > 0; case-insensitive c < D
  ctx.r3.u64 = g.address; ctx.r4.u64 = g.address + 8; ctx.r5.u64 = 0;
  CHECK(int32_t(g.call(__imp__RtlCompareUnicodeString, ctx)) > 0);
  ctx.r3.u64 = g.address; ctx.r4.u64 = g.address + 8; ctx.r5.u64 = 1;
  CHECK(int32_t(g.call(__imp__RtlCompareUnicodeString, ctx)) < 0);
  // N variant with 0xFFFF = NUL-terminated
  ctx.r3.u64 = g.address + 0x100; ctx.r4.u64 = 0xFFFF; ctx.r5.u64 = g.address + 0x200;
  ctx.r6.u64 = 2; ctx.r7.u64 = 1;
  CHECK(int32_t(g.call(__imp__RtlCompareUnicodeStringN, ctx)) > 0);  // "abc" vs "AB": length diff
  // append wide C string: fits only when length+src <= max
  auto* dst = g.at<X_UNICODE_STRING>(16);
  dst->pointer = g.address + 0x300; dst->length = 0; dst->maximum_length = 8;
  PutW(g.at<uint8_t>(0x400), u"xyz");
  ctx.r3.u64 = g.address + 16; ctx.r4.u64 = g.address + 0x400;
  CHECK(g.call(__imp__RtlAppendUnicodeToString, ctx) == X_STATUS_SUCCESS);
  CHECK(uint16_t(dst->length) == 6);
  ctx.r3.u64 = g.address + 16; ctx.r4.u64 = g.address + 0x400;
  CHECK(g.call(__imp__RtlAppendUnicodeToString, ctx) == X_STATUS_BUFFER_TOO_SMALL);
  CHECK(uint16_t(dst->length) == 6);
  // create: heap copy with terminator room
  auto* c = g.at<X_UNICODE_STRING>(24);
  ctx.r3.u64 = g.address + 24; ctx.r4.u64 = g.address + 0x400;
  CHECK(g.call(__imp__RtlCreateUnicodeString, ctx) == 1);
  CHECK(uint16_t(c->length) == 6);
  CHECK(uint16_t(c->maximum_length) == 8);
  // UTF-8: U+00E9 -> C3 A9, U+20AC -> E2 82 AC
  PutW(g.at<uint8_t>(0x500), u"é€");
  ctx.r3.u64 = g.address + 0x600; ctx.r4.u64 = 16; ctx.r5.u64 = g.address + 0x700;
  ctx.r6.u64 = g.address + 0x500; ctx.r7.u64 = 4;
  CHECK(g.call(__imp__RtlUnicodeToUtf8, ctx) == X_STATUS_SUCCESS);
  CHECK(*g.at<rex::be<uint32_t>>(0x700) == 5);
  const uint8_t exp[] = {0xC3, 0xA9, 0xE2, 0x82, 0xAC};
  CHECK(std::memcmp(g.at<uint8_t>(0x600), exp, 5) == 0);
  ctx.r3.u64 = g.address + 0x700; ctx.r4.u64 = g.address + 0x500; ctx.r5.u64 = 4;
  CHECK(g.call(__imp__RtlUnicodeToUtf8Size, ctx) == X_STATUS_SUCCESS);
  CHECK(*g.at<rex::be<uint32_t>>(0x700) == 5);
}
