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

TEST_CASE("Gather write combines 32-bit segments into one write and one completion", "[kernel][io]") {
  Guest g;
  auto* backing = new MemoryFile();
  auto file = object_ref<XFile>(new XFile(g.runtime.kernel_state(), backing, true));
  auto port = object_ref<XIOCompletion>(new XIOCompletion(g.runtime.kernel_state()));
  file->RegisterIOCompletionPort(17, port);
  auto event = object_ref<XEvent>(new XEvent(g.runtime.kernel_state()));
  event->Initialize(true, false);
  auto* segments = g.at<be<uint32_t>>(0);
  segments[0] = g.address + 0x1000;
  segments[1] = g.address + 0x3000; // deliberately noncontiguous pages
  std::memset(g.at<uint8_t>(0x1000), 0x12, 4096);
  std::memset(g.at<uint8_t>(0x3000), 0x34, 4096);
  *g.at<be<uint64_t>>(64) = 32;
  PPCContext ctx{};
  ctx.r3.u64 = file->handle();
  ctx.r4.u64 = event->handle();
  ctx.r6.u64 = 99;
  ctx.r7.u64 = g.address + 80;
  ctx.r8.u64 = g.address;
  ctx.r9.u64 = 8192;
  ctx.r10.u64 = g.address + 64;
  CHECK(g.call(__imp__NtWriteFileGather, ctx) == X_STATUS_SUCCESS);
  auto* iosb = g.at<X_IO_STATUS_BLOCK>(80);
  CHECK(uint32_t(iosb->status) == X_STATUS_SUCCESS);
  CHECK(uint32_t(iosb->information) == 8192);
  REQUIRE(backing->bytes.size() == 8224);
  CHECK(backing->bytes[32] == 0x12);
  CHECK(backing->bytes[4127] == 0x12);
  CHECK(backing->bytes[4128] == 0x34);
  CHECK(backing->bytes.back() == 0x34);
  CHECK(backing->writes == 1);
  uint64_t timeout = 0;
  CHECK(event->Wait(0, 0, false, &timeout) == X_STATUS_SUCCESS);
  XIOCompletion::IONotification packet{};
  REQUIRE(port->WaitForNotification(0, &packet));
  CHECK(packet.num_bytes == 8192);
  CHECK(packet.key_context == 17);
  CHECK(packet.apc_context == 99);
  CHECK_FALSE(port->WaitForNotification(0, &packet));

  // Cancel has no work to do in the forced synchronous I/O model.
  ctx.r3.u64 = file->handle();
  ctx.r4.u64 = g.address + 80;
  CHECK(g.call(__imp__NtCancelIoFile, ctx) == X_STATUS_SUCCESS);
  CHECK(uint32_t(iosb->information) == 0);
  ctx.r3.u64 = 0;
  CHECK(g.call(__imp__NtCancelIoFile, ctx) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("Gather write handles zero length, short writes and async status", "[kernel][io]") {
  Guest g;
  auto* backing = new MemoryFile();
  auto file = object_ref<XFile>(new XFile(g.runtime.kernel_state(), backing, false));
  *g.at<be<uint32_t>>(0) = g.address + 0x1000;
  PPCContext ctx{};
  ctx.r3.u64 = file->handle();
  ctx.r7.u64 = g.address + 80;
  ctx.r8.u64 = g.address;
  ctx.r9.u64 = 4096;
  backing->max_write = 1024;
  CHECK(g.call(__imp__NtWriteFileGather, ctx) == X_STATUS_PENDING);
  auto* iosb = g.at<X_IO_STATUS_BLOCK>(80);
  CHECK(uint32_t(iosb->information) == 1024);
  CHECK(uint32_t(iosb->status) == X_STATUS_SUCCESS);
  ctx.r3.u64 = file->handle();
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 0;
  CHECK(g.call(__imp__NtWriteFileGather, ctx) == X_STATUS_PENDING);
  CHECK(uint32_t(iosb->information) == 0);
  ctx.r3.u64 = file->handle();
  ctx.r9.u64 = 4096;
  CHECK(g.call(__imp__NtWriteFileGather, ctx) == X_STATUS_INVALID_PARAMETER);
  ctx.r3.u64 = 0;
  CHECK(g.call(__imp__NtWriteFileGather, ctx) == X_STATUS_INVALID_HANDLE);
}

