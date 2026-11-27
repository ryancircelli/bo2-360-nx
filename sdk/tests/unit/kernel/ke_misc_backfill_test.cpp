#include <cstring>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/xio.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

extern "C" void __imp__KeIpiGenericCall(PPCContext&, uint8_t*);
extern "C" void __imp__XexPcToFileHeader(PPCContext&, uint8_t*);
extern "C" void __imp__NtCreateSymbolicLinkObject(PPCContext&, uint8_t*);
extern "C" void __imp__NtQueryDirectoryObject(PPCContext&, uint8_t*);
extern "C" void __imp__NtClose(PPCContext&, uint8_t*);

namespace {
using namespace rex;
using namespace rex::system;

uint32_t g_seen_context = 0;

void GuestIpiRoutine(PPCContext& ctx, uint8_t*) {
  g_seen_context = ctx.r3.u32;
  ctx.r31.u64 = 0xBAD;  // a callee-clobbered register must not leak back to the caller
  ctx.r3.u64 = ctx.r3.u32 * 2 + 1;
}
}  // namespace

TEST_CASE("KeIpiGenericCall runs the routine once on the calling thread", "[kernel][ipi]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* dispatcher = runtime.function_dispatcher();
  REQUIRE(dispatcher->InitializeFunctionTable(0x82000000, 0x1000, 0x82000000, 0x10000, true));
  uint32_t routine = dispatcher->AllocateThunk(&GuestIpiRoutine, 0);
  REQUIRE(routine != 0);
  uint8_t* base = runtime.virtual_membase();

  auto* thread = new XHostThread(runtime.kernel_state(), 128 * 1024, 0, [&]() -> int {
    auto* guest = XThread::GetCurrentThread()->thread_state()->context();
    guest->r31.u64 = 0x1234;
    PPCContext ctx{};
    ctx.r3.u64 = routine;
    ctx.r4.u64 = 20;
    __imp__KeIpiGenericCall(ctx, base);
    CHECK(g_seen_context == 20);
    CHECK(ctx.r3.u32 == 41);
    CHECK(guest->r31.u64 == 0x1234);
    return 0;
  });
  REQUIRE(thread->Create() == X_STATUS_SUCCESS);
  uint64_t join = static_cast<uint64_t>(-100000000ll);
  REQUIRE(thread->Wait(0, 0, 0, &join) == X_STATUS_SUCCESS);
  thread->Release();
}

TEST_CASE("XexPcToFileHeader returns 0 when no title image contains the pc", "[kernel][xex]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  uint8_t* base = runtime.virtual_membase();
  uint32_t entry = runtime.memory()->SystemHeapAlloc(4);
  *reinterpret_cast<uint32_t*>(base + entry) = 0xAAAAAAAA;
  PPCContext ctx{};
  ctx.r3.u64 = 0x82001000;
  ctx.r4.u64 = entry;
  __imp__XexPcToFileHeader(ctx, base);
  CHECK(ctx.r3.u32 == 0);
  CHECK(*reinterpret_cast<uint32_t*>(base + entry) == 0xAAAAAAAA);
}

TEST_CASE("NtCreateSymbolicLinkObject registers the link and returns a closable handle",
          "[kernel][ob]") {
  Runtime runtime{std::filesystem::current_path()};
  RuntimeConfig config{};
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  auto* memory = runtime.memory();
  uint8_t* base = runtime.virtual_membase();
  uint32_t block = memory->SystemHeapAlloc(0x100);
  auto put_string = [&](uint32_t string_at, uint32_t chars_at, const char* text) {
    std::memcpy(base + chars_at, text, std::strlen(text) + 1);
    auto* s = memory->TranslateVirtual<X_ANSI_STRING*>(string_at);
    s->length = uint16_t(std::strlen(text));
    s->maximum_length = uint16_t(std::strlen(text) + 1);
    s->pointer = chars_at;
  };
  put_string(block + 0x10, block + 0x20, "\\??\\Q:");
  put_string(block + 0x18, block + 0x40, "\\Device\\Harddisk0\\Partition1");
  auto* attrs = memory->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(block);
  attrs->root_directory = 0;
  attrs->name_ptr = block + 0x10;
  attrs->attributes = 64;
  uint32_t handle_out = block + 0x80;

  PPCContext ctx{};
  ctx.r3.u64 = handle_out;
  ctx.r4.u64 = block;
  ctx.r5.u64 = block + 0x18;
  __imp__NtCreateSymbolicLinkObject(ctx, base);
  CHECK(ctx.r3.u32 == X_STATUS_SUCCESS);
  std::string target;
  CHECK(runtime.file_system()->FindSymbolicLink("Q:", target));
  CHECK(target == "\\Device\\Harddisk0\\Partition1");

  uint32_t handle = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + handle_out));
  CHECK(handle != 0);
  ctx = {};
  ctx.r3.u64 = handle;
  __imp__NtClose(ctx, base);
  CHECK(ctx.r3.u32 == X_STATUS_SUCCESS);

  // No directory objects exist, so enumeration ends at once with an empty result.
  uint32_t ret_len = block + 0x84;
  *reinterpret_cast<uint32_t*>(base + ret_len) = 0xFFFFFFFF;
  ctx = {};
  ctx.r3.u64 = 0x1234;
  ctx.r4.u64 = block + 0xA0;
  ctx.r5.u64 = 0x40;
  ctx.r6.u64 = 1;
  ctx.r7.u64 = 1;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = ret_len;
  __imp__NtQueryDirectoryObject(ctx, base);
  CHECK(ctx.r3.u32 == X_STATUS_NO_MORE_ENTRIES);
  CHECK(*reinterpret_cast<uint32_t*>(base + ret_len) == 0);
}
